#!/usr/bin/env python3
"""Pixel 10 bring-up over ADB: device facts (phase 0), the property probe (phase 1), session logs (phase 2).

    python tools/pixel10/pixel10.py facts   [--out DIR]
    python tools/pixel10/pixel10.py probe   PVR_PROPS_PROBE [--hal PATH] [--out DIR]
    python tools/pixel10/pixel10.py logs    [--out DIR]
    python tools/pixel10/pixel10.py install APK

adb is found through --adb, ADB, PATH or ANDROID_HOME/ANDROID_SDK_ROOT; the device through --serial,
ADB_SERIAL / ANDROID_SERIAL or the only authorized one (tools/adb_device.py). `facts` only reads.
`probe` pushes one file to /data/local/tmp/pixel10. `install` replaces the app in place and never
uninstalls anything. Exit codes: 0 done, 2 bad arguments, 3 adb or device error, 4 needs the user.
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

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from adb_device import DeviceResolutionError, resolve_serial  # noqa: E402

PACKAGE = "com.droiddeck.launcher"
REMOTE = "/data/local/tmp/pixel10"
SESSION_FOLDER = re.compile(r"^\d{4}-\d{2}-\d{2}-\d{2,}-.+$")
SHARE_ZIP = re.compile(r"^DroidDeck-.+\.zip$")

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
    ("dumpsys-gpu.txt", "dumpsys gpu"),
]


class Adb:
    """The one door to adb: every call is argv, bytes in and out."""

    def __init__(self, exe: str, serial: str) -> None:
        self.exe, self.serial = exe, serial

    def run(self, *args: str, timeout: float = 60) -> subprocess.CompletedProcess:
        try:
            return subprocess.run([self.exe, "-s", self.serial, *args], capture_output=True, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise SystemExit(f"pixel10: adb failed: {error}") from error

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
    out.mkdir(parents=True, exist_ok=True)
    report = []
    for heading, command in FACTS:
        result = adb.shell(command)
        report.append(f"### {heading}\n$ {command}\n{text(result)}[exit {result.returncode}]\n")
    for name, command in RAW:
        result = adb.shell(command, timeout=120)
        (out / name).write_bytes(result.stdout)
        report.append(f"### {name}\n$ {command}\n[{len(result.stdout)} bytes saved, exit {result.returncode}]\n")
    config = (out / "ld.config.txt").read_text("utf-8", "replace")
    (out / "ld.config.system.txt").write_text(section(config, "system"), "utf-8")
    (out / "facts.txt").write_text("\n".join(report), "utf-8")
    print(f"facts written to {out / 'facts.txt'}")
    return 0


def probe(adb: Adb, binary: Path, hal: str | None, out: Path) -> int:
    if not binary.is_file():
        print(f"pixel10: no such probe binary: {binary}", file=sys.stderr)
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
        if re.match(r"(section\.|hal\.(driver\.|drm\.state|device\.version|bc\.|native_buffer\.(spec|gralloc)))", line):
            print(line)
    print(f"probe output written to {out / 'props.txt'} (adb exit {ran.returncode})")
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
                extract(packed.stdout, out)
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
    if not apk.is_file():
        print(f"pixel10: no such APK: {apk}", file=sys.stderr)
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
        raise SystemExit("pixel10: adb not found; set ADB to adb.exe or put platform-tools on PATH")
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--adb")
    parser.add_argument("--serial")
    commands = parser.add_subparsers(dest="command", required=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    for name in ("facts", "probe", "logs"):
        commands.add_parser(name).add_argument("--out", type=Path, default=Path("pixel10-runs") / stamp / name)
    commands.choices["probe"].add_argument("binary", type=Path)
    commands.choices["probe"].add_argument("--hal")
    commands.add_parser("install").add_argument("apk", type=Path)
    args = parser.parse_args(argv)
    exe = adb_path(args.adb)
    try:
        adb = Adb(exe, resolve_serial(exe, args.serial, log=True))
    except DeviceResolutionError as error:
        print(f"pixel10: {error}", file=sys.stderr)
        return 3
    if args.command == "facts":
        return facts(adb, args.out)
    if args.command == "probe":
        return probe(adb, args.binary, args.hal, args.out)
    if args.command == "logs":
        return logs(adb, args.out)
    return install(adb, args.apk)


if __name__ == "__main__":
    raise SystemExit(main())
