#!/usr/bin/env python3
"""Build the bionic PowerVR property probe with the Android NDK; does not build an APK.

The NDK is found through ANDROID_NDK_HOME, ANDROID_NDK_ROOT, ANDROID_HOME/ndk/<version> or
ANDROID_SDK_ROOT/ndk/<version> (the version app/build.gradle pins), then Android Studio's default
SDK folder on Windows. The probe has no link-time Vulkan dependency; it dlopens libvulkan.so.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import struct
import subprocess

NDK_VERSION = "27.3.13750724"  # ndkVersion in app/build.gradle
API = 29
HOSTS = {"Windows": ("windows-x86_64", ".exe"), "Linux": ("linux-x86_64", ""), "Darwin": ("darwin-x86_64", "")}


def ndk_roots(environ):
    for name in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"):
        if environ.get(name):
            yield Path(environ[name])
    for name in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        if environ.get(name):
            yield Path(environ[name]) / "ndk" / NDK_VERSION
    if environ.get("LOCALAPPDATA"):
        yield Path(environ["LOCALAPPDATA"]) / "Android" / "Sdk" / "ndk" / NDK_VERSION


def find_clang(environ, system):
    tag, suffix = HOSTS[system]
    for root in ndk_roots(environ):
        clang = root / "toolchains" / "llvm" / "prebuilt" / tag / "bin" / ("clang" + suffix)
        if clang.is_file():
            return clang
    raise SystemExit("Android NDK %s not found; set ANDROID_NDK_HOME to its folder" % NDK_VERSION)


def compile_command(clang, source, output):
    # No -Werror: a warning from a newer NDK header must not cost a device iteration.
    return [str(clang), "--target=aarch64-linux-android%d" % API, "-O2", "-std=c11", "-Wall", "-Wextra",
            str(source), "-ldl", "-o", str(output)]


def is_aarch64_elf(header):
    return len(header) >= 20 and header[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", header, 18)[0] == 183


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source = Path(__file__).resolve().parent / "pvr_props_probe.c"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    command = compile_command(find_clang(os.environ, platform.system()), source, output / "pvr_props_probe")
    subprocess.run(command, check=True)
    if not is_aarch64_elf((output / "pvr_props_probe").read_bytes()[:64]):
        raise SystemExit("Compiler did not produce an ELF64 AArch64 executable")
    (output / "build-command.json").write_text(json.dumps(command, indent=2) + "\n")
    print("Built", output / "pvr_props_probe")


if __name__ == "__main__":
    main()
