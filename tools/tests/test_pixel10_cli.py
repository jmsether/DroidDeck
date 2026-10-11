"""tools/pixel10/pixel10.py against a fake adb: what it asks the device and what it writes down."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("pixel10", TOOLS / "pixel10" / "pixel10.py")
pixel10 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pixel10)

LD_CONFIG = b"dir.system = /system/bin/\n[system]\nnamespace.default.isolated = false\n[vendor]\nnamespace.default.isolated = true\n"


def done(stdout=b"", returncode=0, stderr=b""):
    return subprocess.CompletedProcess([], returncode, stdout, stderr)


def tar_of(files):
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w") as archive:
        for name, data in files.items():
            member = tarfile.TarInfo(name)
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
    return raw.getvalue()


class FakeAdb(pixel10.Adb):
    """Answers the first (needle, result) whose needle is in the command line; records every call."""

    def __init__(self, answers=()):
        super().__init__("adb", "fake-1")
        self.answers, self.calls = list(answers), []

    def run(self, *args, timeout=60):
        self.calls.append(args)
        line = " ".join(args)
        for needle, result in self.answers:
            if needle in line:
                return result
        return done()


class Pixel10Test(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.out = Path(self.temp.name) / "out"

    def quiet(self, call, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = call(*args)
        return code, out.getvalue(), err.getvalue()

    def test_facts_are_read_only_and_identify_nothing(self):
        commands = " ".join(command for _, command in pixel10.FACTS + pixel10.RAW)
        self.assertNotRegex(commands, r"serial|imei|meid|android_id|mac_addr|wifi|bluetooth|\bput\b|\bset\b|\brm\b|reboot")

    def test_facts_report_and_raw_files(self):
        adb = FakeAdb([("cat /linkerconfig/ld.config.txt", done(LD_CONFIG)), ("getprop", done(b"ro.board.platform=laguna\n")),
                       ("cmd gpu vkjson", done(b"{}"))])
        code, out, _ = self.quiet(pixel10.facts, adb, self.out)
        self.assertEqual(code, 0)
        report = (self.out / "facts.txt").read_text()
        self.assertIn("### build properties", report)
        self.assertIn("ro.board.platform=laguna", report)
        self.assertEqual((self.out / "vkjson.json").read_bytes(), b"{}")
        self.assertEqual((self.out / "ld.config.system.txt").read_text(), "[system]\nnamespace.default.isolated = false\n")
        self.assertEqual(len(adb.calls), len(pixel10.FACTS) + len(pixel10.RAW))

    def test_section_of_a_missing_header_is_empty(self):
        self.assertEqual(pixel10.section("[a]\nx\n", "system"), "")
        self.assertEqual(pixel10.section("", "system"), "")

    def test_probe_pushes_runs_and_keeps_the_output(self):
        binary = Path(self.temp.name) / "pvr_props_probe"
        binary.write_bytes(b"\x7fELF")
        adb = FakeAdb([("./pvr_props_probe", done(b"section.hal=exit=0\nhal.drm.state=untouched\nhal.fmt=BC7\n"))])
        code, out, _ = self.quiet(pixel10.probe, adb, binary, "/vendor/lib64/hw/vulkan.powervr.so", self.out)
        self.assertEqual(code, 0)
        self.assertIn("hal.drm.state=untouched", out)
        self.assertNotIn("hal.fmt", out)  # the summary is a few lines; the file has everything
        self.assertIn("hal.fmt=BC7", (self.out / "props.txt").read_text())
        self.assertEqual([c[0] for c in adb.calls], ["shell", "push", "shell", "shell"])
        self.assertEqual(adb.calls[1][2], "/data/local/tmp/pixel10/pvr_props_probe")
        self.assertEqual(adb.calls[3][1], "cd /data/local/tmp/pixel10 && ./pvr_props_probe /vendor/lib64/hw/vulkan.powervr.so")

    def test_probe_without_a_binary_is_a_usage_error(self):
        code, _, err = self.quiet(pixel10.probe, FakeAdb(), Path(self.temp.name) / "missing", None, self.out)
        self.assertEqual(code, 2)
        self.assertIn("no such probe binary", err)

    def test_logs_from_a_debuggable_build_take_the_newest_session(self):
        packed = tar_of({"2026-10-10-02-steam/session.log": b"hello"})
        adb = FakeAdb([("ls -1 files/logs", done(b"2026-10-10-01-steam\n2026-10-10-02-steam\ntools\nstores\n")),
                       ("exec-out", done(packed))])
        code, out, _ = self.quiet(pixel10.logs, adb, self.out)
        self.assertEqual(code, 0)
        self.assertEqual((self.out / "2026-10-10-02-steam" / "session.log").read_bytes(), b"hello")
        self.assertIn("-C files/logs 2026-10-10-02-steam", [c for c in adb.calls if c[0] == "exec-out"][0][1])

    def test_logs_refuse_a_tar_that_escapes_the_output_folder(self):
        evil = tar_of({"../evil.txt": b"x"})
        adb = FakeAdb([("ls -1 files/logs", done(b"2026-10-10-01-steam\n")), ("exec-out", done(evil))])
        with self.assertRaisesRegex(SystemExit, "outside the output folder"):
            self.quiet(pixel10.logs, adb, self.out)
        self.assertFalse((self.out.parent / "evil.txt").exists())

    def test_logs_from_a_release_build_use_the_shared_zip(self):
        adb = FakeAdb([("run-as", done(b"run-as: package not debuggable", 1)),
                       ("ls -t /sdcard/Download", done(b"notes.txt\nDroidDeck-2026-10-10-02-steam.zip\nDroidDeck-old.zip\n"))])
        code, _, _ = self.quiet(pixel10.logs, adb, self.out)
        self.assertEqual(code, 0)
        pull = [c for c in adb.calls if c[0] == "pull"][0]
        self.assertEqual(pull[1], "/sdcard/Download/DroidDeck-2026-10-10-02-steam.zip")

    def test_logs_without_any_route_say_how_to_make_one(self):
        adb = FakeAdb([("run-as", done(b"not debuggable", 1))])
        code, _, err = self.quiet(pixel10.logs, adb, self.out)
        self.assertEqual(code, 4)
        self.assertIn("Share logs", err)

    def test_install_never_uninstalls_a_differently_signed_app(self):
        apk = Path(self.temp.name) / "a.apk"
        apk.write_bytes(b"PK")
        adb = FakeAdb([("install", done(b"Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: signatures do not match]", 1))])
        code, _, err = self.quiet(pixel10.install, adb, apk)
        self.assertEqual(code, 4)
        self.assertIn("adb uninstall com.droiddeck.launcher", err)
        self.assertEqual([c[0] for c in adb.calls], ["install"])
        self.assertEqual(adb.calls[0][:3], ("install", "-r", "--no-incremental"))

    def test_install_reports_success(self):
        apk = Path(self.temp.name) / "a.apk"
        apk.write_bytes(b"PK")
        self.assertEqual(self.quiet(pixel10.install, FakeAdb([("install", done(b"Performing Streamed Install\nSuccess\n"))]), apk)[0], 0)

    def test_adb_is_found_in_the_sdk_platform_tools(self):
        tools = Path(self.temp.name) / "sdk" / "platform-tools"
        tools.mkdir(parents=True)
        adb = tools / ("adb.exe" if os.name == "nt" else "adb")
        adb.write_bytes(b"")
        adb.chmod(0o755)
        env = {k: v for k, v in os.environ.items() if k not in ("ADB", "ANDROID_HOME", "ANDROID_SDK_ROOT")}
        with mock.patch.dict(os.environ, {**env, "PATH": "", "ANDROID_SDK_ROOT": str(tools.parent)}, clear=True):
            self.assertEqual(Path(pixel10.adb_path(None)), adb)

    def test_logs_take_the_newest_zip_even_when_its_name_has_spaces(self):
        adb = FakeAdb([("run-as", done(b"not debuggable", 1)),
                       ("ls -t /sdcard/Download", done(b"DroidDeck-2026-10-10-02-steam (1).zip" + bytes([10]) + b"DroidDeck-2026-10-10-02-steam.zip" + bytes([10])))])
        self.assertEqual(self.quiet(pixel10.logs, adb, self.out)[0], 0)
        self.assertEqual([c for c in adb.calls if c[0] == "pull"][0][1], "/sdcard/Download/DroidDeck-2026-10-10-02-steam (1).zip")

    def test_adb_path_order(self):
        with mock.patch.dict(os.environ, {"ADB": "from-env"}), mock.patch("shutil.which", return_value="from-path"):
            self.assertEqual(pixel10.adb_path("from-arg"), "from-arg")
            self.assertEqual(pixel10.adb_path(None), "from-env")
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch("shutil.which", return_value="from-path"):
            self.assertEqual(pixel10.adb_path(None), "from-path")
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch("shutil.which", return_value=None):
            with self.assertRaises(SystemExit):
                pixel10.adb_path(None)


if __name__ == "__main__":
    unittest.main()
