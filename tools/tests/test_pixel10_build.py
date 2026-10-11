import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "build_props_probe", Path(__file__).resolve().parents[1] / "pixel10/build_props_probe.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class BuildPropsProbeTest(unittest.TestCase):
    def fake_ndk(self, root, tag="linux-x86_64", suffix=""):
        clang = Path(root) / "toolchains/llvm/prebuilt" / tag / "bin" / ("clang" + suffix)
        clang.parent.mkdir(parents=True)
        clang.write_bytes(b"")
        return clang

    def test_ndk_home_is_found(self):
        with tempfile.TemporaryDirectory() as tmp:
            clang = self.fake_ndk(tmp)
            self.assertEqual(build.find_clang({"ANDROID_NDK_HOME": tmp}, "Linux"), clang)

    def test_sdk_layout_uses_the_version_the_app_pins(self):
        with tempfile.TemporaryDirectory() as tmp:
            clang = self.fake_ndk(Path(tmp) / "ndk" / build.NDK_VERSION)
            self.assertEqual(build.find_clang({"ANDROID_HOME": tmp}, "Linux"), clang)
            with self.assertRaises(SystemExit):  # another NDK version in the same SDK is not accepted
                build.find_clang({"ANDROID_SDK_ROOT": str(Path(tmp) / "elsewhere")}, "Linux")

    def test_windows_host_runs_clang_exe(self):
        with tempfile.TemporaryDirectory() as tmp:
            clang = self.fake_ndk(tmp, "windows-x86_64", ".exe")
            self.assertEqual(build.find_clang({"ANDROID_NDK_ROOT": tmp}, "Windows"), clang)

    def test_missing_ndk_is_a_clear_exit(self):
        with self.assertRaisesRegex(SystemExit, "NDK 27.3.13750724 not found"):
            build.find_clang({}, "Linux")

    def test_command_targets_the_device(self):
        command = build.compile_command(Path("clang"), Path("p.c"), Path("out"))
        self.assertIn("--target=aarch64-linux-android29", command)
        self.assertEqual(command[-3:], ["-ldl", "-o", "out"])
        self.assertNotIn("-Werror", command)

    def test_only_aarch64_elf_is_accepted(self):
        elf = bytearray(64)
        elf[:6] = b"\x7fELF\x02\x01"
        elf[18:20] = b"\xb7\x00"
        self.assertTrue(build.is_aarch64_elf(bytes(elf)))
        elf[18:20] = b"\x3e\x00"
        self.assertFalse(build.is_aarch64_elf(bytes(elf)))
        self.assertFalse(build.is_aarch64_elf(b"MZ" + bytes(62)))
        self.assertFalse(build.is_aarch64_elf(b"\x7fELF\x02\x01"))  # too short to hold e_machine
        self.assertFalse(build.is_aarch64_elf(b""))


if __name__ == "__main__":
    unittest.main()
