#!/usr/bin/env python3
"""Pixel 10 bring-up over ADB: device facts (phase 0), the property probe (phase 1), session logs (phase 2).

    python tools/pixel10/pixel10.py [--adb ADB] [--serial SERIAL] facts   [--out DIR]
    python tools/pixel10/pixel10.py [--adb ADB] [--serial SERIAL] probe   PVR_PROPS_PROBE [--hal PATH] [--out DIR]
    python tools/pixel10/pixel10.py [--adb ADB] [--serial SERIAL] logs    [--out DIR]
    python tools/pixel10/pixel10.py [--adb ADB] [--serial SERIAL] install APK

--adb and --serial go before the subcommand. adb is found through --adb, ADB, PATH or
ANDROID_HOME/ANDROID_SDK_ROOT; the device through --serial, ADB_SERIAL / ANDROID_SERIAL or the only
authorized one (tools/adb_device.py). `facts` only reads. `probe` pushes one file to
/data/local/tmp/pixel10. `install` replaces the app in place and never uninstalls anything.
Exit codes: 0 done, 2 bad arguments, 3 adb or device error, 4 needs the user.
"""
from __future__ import annotations

import argparse
import datetime
import io
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE.parent)]  # this folder (build_props_probe) and tools/ (adb_device)
from adb_device import DeviceResolutionError, resolve_serial  # noqa: E402
from build_props_probe import is_aarch64_elf  # noqa: E402

PACKAGE = "com.droiddeck.launcher"
REMOTE = "/data/local/tmp/pixel10"
SESSION_FOLDER = re.compile(r"^\d{4}-\d{2}-\d{2}-\d{2,}-.+$")
SHARE_ZIP = re.compile(r"^DroidDeck-.+\.zip$")
TIMED_OUT = 124  # the exit status timeout(1) uses; what a command that outlived its time limit reports
# Probe lines echoed to the console: the ones the bring-up gates read. props.txt has everything.
SUMMARY = re.compile(
    r"section\.|hal\.(driver\.|drm\.(state|fields)|device\.(version|apiVersion)|bc\.|queue\.(count|family)"
    r"|native_buffer\.(spec_version|device|gralloc_fns|usage\.))")

# Identity of the build under test: no serial number, account or network name.
PROPS = (
    "ro.build.fingerprint ro.build.id ro.build.display.id ro.build.type ro.build.tags ro.build.version.release "
    "ro.build.version.sdk ro.build.version.security_patch ro.build.version.incremental ro.product.manufacturer "
    "ro.product.model ro.product.name ro.product.device ro.product.board ro.board.platform ro.hardware "
    "ro.hardware.vulkan ro.hardware.egl ro.soc.manufacturer ro.soc.model ro.opengles.version "
    "ro.vendor.build.fingerprint ro.boot.verifiedbootstate ro.boot.flash.locked ro.product.cpu.abilist "
    "persist.sys.fflag.override.settings_enable_monitor_phantom_procs"
)

# (heading, command run by the device's shell). Every command is read-only and may fail on its own.
FACTS = [
    ("build properties", f'for p in {PROPS}; do echo "$p=$(getprop $p)"; done'),
    ("kernel", "uname -a; cat /proc/version"),
    ("page size", "getconf PAGE_SIZE; grep -m1 KernelPageSize /proc/self/smaps"),
    ("kernel config", "zcat /proc/config.gz | grep -E '^CONFIG_(ARM64_(4K|16K|64K)_PAGES|ARM64_VA_BITS(_[0-9]+)?"
                      "|ARM64_PA_BITS(_[0-9]+)?|DRM_POWERVR|DRM_IMAGINATION)='"),
    ("cpu", "grep -m1 Features /proc/cpuinfo; grep -c ^processor /proc/cpuinfo"),
    ("vulkan hal", "ls -lZ /vendor/lib64/hw/vulkan.powervr.so; ls -l /vendor/lib64/hw | head -40"),
    ("drm nodes", "ls -lZ /dev/dri/; stat -c '%n %F %t:%T hex' /dev/dri/*; ls -l /sys/class/drm/; "
                  "cat /sys/class/drm/renderD128/device/uevent"),
    ("pvr sync and modules", "ls -lZ /dev/pvr_sync; grep -i -E 'pvr|powervr' /proc/modules"),
    ("vulkan and gles features", "pm list features | grep -i -E 'vulkan|opengles'"),
    ("selinux and identity", "getenforce; id; cat /proc/self/attr/current"),
    ("linker namespace directories", "grep -n '^dir\\.' /linkerconfig/ld.config.txt"),
    ("phantom process limit", "settings get global settings_enable_monitor_phantom_procs; "
                              "device_config get activity_manager max_phantom_processes"),
    ("storage", "df -h /data /sdcard"),
    ("droiddeck packages", "pm list packages | grep -i droiddeck"),
    ("droiddeck package", f"dumpsys package {PACKAGE} | grep -E 'versionName|versionCode|targetSdk|flags=|pkgFlags|"
                          "firstInstallTime|lastUpdateTime|dataDir|primaryCpuAbi'"),
]
# Large outputs kept as files of their own.
RAW = [
    ("ld.config.txt", "cat /linkerconfig/ld.config.txt"),
    ("cpuinfo.txt", "cat /proc/cpuinfo"),
    ("vkjson.json", "cmd gpu vkjson"),
    ("dumpsys-gpu.txt", "dumpsys gpu --global"),  # --global: the per-app records name the installed packages
]


class AdbError(Exception):
    """adb itself could not be run (not found, not executable). Exit code 3."""


class Adb:
    """The one door to adb: every call is argv, bytes in and out."""

    def __init__(self, exe: str, serial: str) -> None:
        self.exe, self.serial = exe, serial

    def run(self, *args: str, timeout: float = 60) -> subprocess.CompletedProcess:
        argv = [self.exe, "-s", self.serial, *args]
        try:
            return subprocess.run(argv, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired as hung:  # one hung command costs itself only; what it printed is kept
            return subprocess.CompletedProcess(argv, TIMED_OUT, hung.stdout or b"", b"timeout")
        except OSError as error:
            raise AdbError(f"adb failed: {error}") from error

    def shell(self, command: str, timeout: float = 60) -> subprocess.CompletedProcess:
        return self.run("shell", command, timeout=timeout)


def text(result: subprocess.CompletedProcess) -> str:
    return (result.stdout + result.stderr).decode("utf-8", "replace").replace("\r\n", "\n")


def section(config: str, name: str) -> str:
    """One [name] section of a linkerconfig file: the header line up to the next header."""
    out, inside = [], False
    for line in config.splitlines():
        if line.startswith("["):
            inside = line.strip() == f"[{name}]"
        if inside:
            out.append(line)
    return "\n".join(out) + ("\n" if out else "")


def facts(adb: Adb, out: Path) -> int:
    """Every command runs and is written down, failed or not. A run where none of them worked is exit 3."""
    out.mkdir(parents=True, exist_ok=True)
    report, codes = [], []
    for heading, command in FACTS:
        result = adb.shell(command)
        codes.append(result.returncode)
        report.append(f"### {heading}\n$ {command}\n{text(result)}[exit {result.returncode}]\n")
    for name, command in RAW:
        result = adb.shell(command, timeout=120)
        codes.append(result.returncode)
        (out / name).write_bytes(result.stdout)
        report.append(f"### {name}\n$ {command}\n[{len(result.stdout)} bytes saved, exit {result.returncode}]\n")
    config = (out / "ld.config.txt").read_text("utf-8", "replace")
    (out / "ld.config.system.txt").write_text(section(config, "system"), "utf-8")
    (out / "facts.txt").write_text("\n".join(report), "utf-8")
    print(f"facts written to {out / 'facts.txt'}")
    if all(codes):
        print("pixel10: every command failed; is the phone connected, unlocked and authorized?", file=sys.stderr)
        return 3
    return 0


def refuse(problem: str | None) -> bool:
    """Print a problem with a local file argument; True when there is one (exit 2)."""
    if problem:
        print(f"pixel10: {problem}", file=sys.stderr)
    return problem is not None


def probe_problem(binary: Path) -> str | None:
    if not binary.is_file():
        return f"no such probe binary: {binary}"
    try:
        with binary.open("rb") as file:
            header = file.read(64)
    except OSError as error:
        return f"cannot read the probe binary {binary}: {error}"
    if not is_aarch64_elf(header):
        return f"{binary} is not an ELF64 AArch64 executable; build it with tools/pixel10/build_props_probe.py"
    return None


def apk_problem(apk: Path) -> str | None:
    return None if apk.is_file() else f"no such APK: {apk}"


# Command -> check of its file argument, run before any device is touched.
FILE_CHECKS = {"probe": probe_problem, "install": apk_problem}


def probe(adb: Adb, binary: Path, hal: str | None, out: Path) -> int:
    """Exit 3 when the probe could not run (adb exit >= 124: timeout, not executable, killed) or printed no
    section line; a section that died is itself a fact in props.txt, not a failure of this command."""
    if refuse(probe_problem(binary)):
        return 2
    out.mkdir(parents=True, exist_ok=True)
    adb.shell(f"mkdir -p {REMOTE}")
    pushed = adb.run("push", str(binary), f"{REMOTE}/pvr_props_probe", timeout=120)
    if pushed.returncode:
        print("pixel10: adb push failed:\n" + text(pushed), file=sys.stderr)
        return 3
    adb.shell(f"chmod 755 {REMOTE}/pvr_props_probe")
    ran = adb.shell(f"cd {REMOTE} && ./pvr_props_probe {shlex.quote(hal) if hal else ''}", timeout=240)
    (out / "props.txt").write_bytes(ran.stdout)
    (out / "props.stderr.txt").write_bytes(ran.stderr)
    for line in text(ran).splitlines():
        if SUMMARY.match(line):
            print(line)
    print(f"probe output written to {out / 'props.txt'} (adb exit {ran.returncode})")
    ran_sections = any(line.startswith("section.") for line in ran.stdout.decode("utf-8", "replace").splitlines())
    if ran.returncode >= TIMED_OUT or not ran_sections:
        print(f"pixel10: the probe produced no results (adb exit {ran.returncode}); see {out / 'props.stderr.txt'}",
              file=sys.stderr)
        return 3
    return 0


def latest_session(names: list[str]) -> str | None:
    folders = sorted(n for n in names if SESSION_FOLDER.match(n))
    return folders[-1] if folders else None


def extract(data: bytes, out: Path) -> None:
    """Unpack a tar from the device; a member that would land outside `out` stops the whole unpack."""
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        for member in archive.getmembers():
            if member.name.startswith(("/", "\\")) or ".." in Path(member.name).parts:
                raise SystemExit(f"pixel10: refusing tar member outside the output folder: {member.name}")
        if hasattr(tarfile, "data_filter"):
            archive.extractall(out, filter="data")
        else:
            archive.extractall(out)


def logs(adb: Adb, out: Path) -> int:
    """Newest session folder. Pulled with run-as when the installed app is debuggable; otherwise the
    Share logs zip, if the user saved it to Download; otherwise the steps that make one."""
    out.mkdir(parents=True, exist_ok=True)
    if adb.shell(f"run-as {PACKAGE} true").returncode == 0:
        listing = text(adb.shell(f"run-as {PACKAGE} ls -1 files/logs")).split()
        name = latest_session(listing)
        if name:
            packed = adb.run("exec-out", f"run-as {PACKAGE} tar -cf - -C files/logs {shlex.quote(name)}", timeout=300)
            if packed.returncode == 0 and packed.stdout:
                try:
                    extract(packed.stdout, out)
                except (tarfile.TarError, OSError) as error:  # exec-out can hand back error text instead of a tar
                    print(f"pixel10: the session folder {name} could not be unpacked: {error}", file=sys.stderr)
                    return 3
                print(f"session logs {name} written to {out / name}")
                return 0
        print("pixel10: run-as works but no session folder could be read", file=sys.stderr)
        return 3
    names = text(adb.shell("ls -t /sdcard/Download")).splitlines()
    zips = [n for n in names if SHARE_ZIP.match(n)]
    if zips:
        pulled = adb.run("pull", f"/sdcard/Download/{zips[0]}", str(out / zips[0]), timeout=300)
        print(text(pulled).strip())
        return 0 if pulled.returncode == 0 else 3
    print("pixel10: this build is not debuggable, so adb cannot read the app's private logs.\n"
          "  1. In the app, end the session and tap Share logs (or Setup > Latest logs > Share logs).\n"
          "  2. Send the zip to the PC (Quick Share, Drive, e-mail) or save it to Download, then run this again.",
          file=sys.stderr)
    return 4


def install(adb: Adb, apk: Path) -> int:
    if refuse(apk_problem(apk)):
        return 2
    result = adb.run("install", "-r", "--no-incremental", str(apk), timeout=600)
    output = text(result)
    print(output.strip())
    if "INSTALL_FAILED_UPDATE_INCOMPATIBLE" in output or "signatures do not match" in output:
        print(f"pixel10: another build of {PACKAGE} with a different signature is installed. Uninstalling it deletes\n"
              f"  its runtime and data, so this tool does not: run `adb uninstall {PACKAGE}` yourself, then install again.",
              file=sys.stderr)
        return 4
    return 0 if result.returncode == 0 and "Success" in output else 3


def adb_path(configured: str | None) -> str:
    found = configured or os.environ.get("ADB") or shutil.which("adb")
    for variable in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        sdk = os.environ.get(variable)
        if not found and sdk:
            found = shutil.which("adb", path=str(Path(sdk) / "platform-tools"))
    if not found:
        raise AdbError("adb not found; set ADB to adb.exe or put platform-tools on PATH")
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--adb")
    parser.add_argument("--serial")
    commands = parser.add_subparsers(dest="command", required=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    for name in ("facts", "probe", "logs"):
        commands.add_parser(name).add_argument("--out", type=Path, default=Path("pixel10-runs") / stamp / name)
    commands.choices["probe"].add_argument("file", type=Path, metavar="PVR_PROPS_PROBE")
    commands.choices["probe"].add_argument("--hal")
    commands.add_parser("install").add_argument("file", type=Path, metavar="APK")
    args = parser.parse_args(argv)
    if args.command in FILE_CHECKS and refuse(FILE_CHECKS[args.command](args.file)):
        return 2  # before any device is looked for
    try:
        exe = adb_path(args.adb)
        adb = Adb(exe, resolve_serial(exe, args.serial, log=True))
        if args.command == "facts":
            return facts(adb, args.out)
        if args.command == "probe":
            return probe(adb, args.file, args.hal, args.out)
        if args.command == "logs":
            return logs(adb, args.out)
        return install(adb, args.file)
    except (AdbError, DeviceResolutionError) as error:
        print(f"pixel10: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
