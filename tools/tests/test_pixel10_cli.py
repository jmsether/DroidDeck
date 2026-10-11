"""tools/pixel10/pixel10.py against a fake adb: what it asks the device and what it writes down."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import re
import shlex
import struct
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


def elf(machine=183):
    """The first 64 bytes of an ELF64 little-endian file for `machine` (183 = AArch64, 62 = x86-64)."""
    header = bytearray(64)
    header[:6] = b"\x7fELF\x02\x01"
    header[18:20] = struct.pack("<H", machine)
    return bytes(header)


def simple_commands(line):
    """Every simple command of a device shell line as its words: split on ; && | and read inside $( )."""
    lexer = shlex.shlex(line, posix=True, punctuation_chars=True)
    lexer.whitespace_split = True
    commands, words = [], []
    for token in lexer:
        if set(token) <= set("();&|<>"):
            commands, words = commands + ([words] if words else []), []
            continue
        words.append(token)
        for inner in re.findall(r"\$\(([^()]*)\)", token):
            commands += simple_commands(inner)
    return commands + ([words] if words else [])


# The leading words of every command FACTS and RAW may run: reads only.
READ_ONLY = {
    ("for",), ("done",), ("echo",), ("getprop",), ("uname",), ("cat",), ("getconf",), ("grep",), ("zcat",), ("ls",),
    ("head",), ("stat",), ("getenforce",), ("id",), ("df",), ("pm", "list"), ("settings", "get"), ("device_config", "get"),
    ("dumpsys", "package", pixel10.PACKAGE), ("dumpsys", "gpu", "--global"), ("cmd", "gpu", "vkjson"),
}


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


class Sandbox(unittest.TestCase):
    """A scratch folder and a way to call a function with its output captured."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.out = Path(self.temp.name) / "out"

    def quiet(self, call, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = call(*args)
        return code, out.getvalue(), err.getvalue()

    def file(self, name, data):
        path = Path(self.temp.name) / name
        path.write_bytes(data)
        return path


class AdbTest(unittest.TestCase):
    def test_argv_is_the_serial_then_the_command(self):
        with mock.patch.object(pixel10.subprocess, "run", return_value=done(b"ok")) as run:
            result = pixel10.Adb("adb", "S1").run("install", "-r", "--no-incremental", "a.apk", timeout=5)
        run.assert_called_once_with(["adb", "-s", "S1", "install", "-r", "--no-incremental", "a.apk"],
                                    capture_output=True, timeout=5)
        self.assertEqual(result.stdout, b"ok")

    def test_a_hung_command_is_exit_124_with_its_partial_output(self):
        for partial, expected in ((b"half a line", b"half a line"), (None, b"")):
            with self.subTest(partial=partial):
                hung = subprocess.TimeoutExpired(["adb"], 5, output=partial)
                with mock.patch.object(pixel10.subprocess, "run", side_effect=hung):
                    result = pixel10.Adb("adb", "S1").shell("sleep 99", timeout=5)
                self.assertEqual((result.returncode, result.stdout, result.stderr), (124, expected, b"timeout"))

    def test_an_adb_that_cannot_run_is_an_adb_error(self):
        with mock.patch.object(pixel10.subprocess, "run", side_effect=FileNotFoundError("adb")):
            with self.assertRaises(pixel10.AdbError):
                pixel10.Adb("adb", "S1").shell("true")


class Pixel10Test(Sandbox):
    def test_facts_commands_are_reads_only(self):
        for name, command in pixel10.FACTS + pixel10.RAW:
            with self.subTest(command=name):
                for words in simple_commands(command):
                    words = words[1:] if words[0] == "do" else words
                    self.assertTrue(any(tuple(words[:len(verb)]) == verb for verb in READ_ONLY), words[:3])
                for forbidden in ("--clear", "setprop", "settings put", "pm clear", "pm uninstall", "reboot", ">"):
                    self.assertNotIn(forbidden, command)
                self.assertNotRegex(command, r"(^|[\s;|&])rm\s")

    def test_facts_identify_nothing(self):
        commands = " ".join(command for _, command in pixel10.FACTS + pixel10.RAW)
        self.assertNotRegex(commands, r"serial|imei|meid|android_id|mac_addr|wifi|bluetooth")

    def test_gpu_stats_are_global_only(self):
        # Per-app GpuStats records carry package names.
        self.assertIn(("dumpsys-gpu.txt", "dumpsys gpu --global"), pixel10.RAW)
        for _, command in pixel10.FACTS + pixel10.RAW:
            self.assertNotRegex(command, r"dumpsys gpu(?! --global)")

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

    def test_facts_where_every_command_failed_are_still_written_but_not_a_success(self):
        adb = FakeAdb([("", done(b"", 1, b"error: device offline"))])
        code, _, err = self.quiet(pixel10.facts, adb, self.out)
        self.assertEqual(code, 3)
        self.assertIn("pixel10:", err)
        report = (self.out / "facts.txt").read_text()
        for heading, _ in pixel10.FACTS:
            self.assertIn(f"### {heading}", report)
        self.assertEqual(len(adb.calls), len(pixel10.FACTS) + len(pixel10.RAW))

    def test_facts_with_one_failing_command_are_a_success(self):
        code, _, _ = self.quiet(pixel10.facts, FakeAdb([("zcat", done(b"", 1))]), self.out)
        self.assertEqual(code, 0)
        self.assertIn("[exit 1]", (self.out / "facts.txt").read_text())

    def test_a_command_that_timed_out_keeps_its_partial_output_and_the_run_going(self):
        adb = FakeAdb([("cmd gpu vkjson", done(b'{"partial"', 124, b"timeout"))])
        code, _, _ = self.quiet(pixel10.facts, adb, self.out)
        self.assertEqual(code, 0)
        self.assertEqual((self.out / "vkjson.json").read_bytes(), b'{"partial"')
        self.assertIn("exit 124", (self.out / "facts.txt").read_text())
        self.assertEqual(len(adb.calls), len(pixel10.FACTS) + len(pixel10.RAW))

    def test_section_of_a_missing_header_is_empty(self):
        self.assertEqual(pixel10.section("[a]\nx\n", "system"), "")
        self.assertEqual(pixel10.section("", "system"), "")

    def test_probe_pushes_runs_and_keeps_the_output(self):
        binary = self.file("pvr_props_probe", elf())
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

    def test_probe_refuses_a_file_that_is_not_an_aarch64_executable(self):
        for header in (elf(62), b"MZ" + bytes(62), b"", b"\x7fELF\x02\x01"):  # x86-64, PE, empty, truncated
            with self.subTest(header=header[:8]):
                adb = FakeAdb()
                code, _, err = self.quiet(pixel10.probe, adb, self.file("pvr_props_probe", header), None, self.out)
                self.assertEqual(code, 2)
                self.assertIn("AArch64", err)
                self.assertEqual(adb.calls, [])  # nothing was pushed

    def test_probe_that_could_not_run_is_not_a_success(self):
        for code in (124, 126, 127, 139):  # timeout, not executable, not found, SIGSEGV
            with self.subTest(adb_exit=code):
                adb = FakeAdb([("./pvr_props_probe", done(b"section.loader=exit=0\n", code, b"sh: ./pvr_props_probe: failed"))])
                result, _, err = self.quiet(pixel10.probe, adb, self.file("pvr_props_probe", elf()), None, self.out)
                self.assertEqual(result, 3)
                self.assertIn("pixel10:", err)

    def test_probe_without_any_section_line_is_not_a_success(self):
        adb = FakeAdb([("./pvr_props_probe", done(b"error: closed\n", 0))])
        code, _, err = self.quiet(pixel10.probe, adb, self.file("pvr_props_probe", elf()), None, self.out)
        self.assertEqual(code, 3)
        self.assertEqual((self.out / "props.txt").read_bytes(), b"error: closed\n")  # kept for the user
        self.assertIn("pixel10:", err)

    def test_probe_whose_section_died_still_reports_what_it_found(self):
        adb = FakeAdb([("./pvr_props_probe", done(b"section.loader=exit=0\nsection.hal=killed signal=11\n", 1))])
        code, out, _ = self.quiet(pixel10.probe, adb, self.file("pvr_props_probe", elf()), None, self.out)
        self.assertEqual(code, 0)  # the facts are the result; what they say never changes the status
        self.assertIn("section.hal=killed signal=11", out)

    def test_probe_push_failure_is_exit_3(self):
        adb = FakeAdb([("push", done(b"", 1, b"adb: error: failed to copy"))])
        code, _, err = self.quiet(pixel10.probe, adb, self.file("pvr_props_probe", elf()), None, self.out)
        self.assertEqual(code, 3)
        self.assertIn("push failed", err)
        self.assertEqual([c[0] for c in adb.calls], ["shell", "push"])

    def test_probe_echoes_the_lines_the_bring_up_gates_read(self):
        gates = ["hal.drm.state=untouched", "hal.drm.fields=hasPrimary=0xcdcdcdcd hasRender=0xcdcdcdcd",
                 "hal.native_buffer.spec_version=11 (icd needs >= 8)", "hal.native_buffer.device=vkCreateDevice=0 family=0",
                 "hal.native_buffer.gralloc_fns=v1=0 v2=1 v3=0 v4=0 (icd.c calls v2)",
                 "hal.native_buffer.usage.RGBA8.use0x10=v2 result=0 exceeds_32_bits=0",
                 "hal.queue.count=1", "hal.queue.family=0 flags=0x7 count=2 granularity=1x1x1",
                 "hal.device.version=0x00000001 accepted_by_upstream_hybris=0 accepted_by_powervr_patch=1",
                 "hal.device.apiVersion=1.4.317", "hal.driver.info=25.3@6908880",
                 "hal.driver.release=25.3 build=6908880 build_matches_driverVersion=1"]
        noise = ["probe.version=1", "hal.ext=VK_KHR_driver_properties", "hal.fmt=BC7_UNORM_BLOCK optimal=0x0", "loader.device.name=x"]
        stdout = "\n".join(["section.hal=exit=0", *noise[:2], *gates, *noise[2:]]).encode() + b"\n"
        adb = FakeAdb([("./pvr_props_probe", done(stdout))])
        code, out, _ = self.quiet(pixel10.probe, adb, self.file("pvr_props_probe", elf()), None, self.out)
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines()[:-1], ["section.hal=exit=0", *gates])

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

    def test_logs_that_got_error_text_instead_of_a_tar_are_exit_3(self):
        adb = FakeAdb([("ls -1 files/logs", done(b"2026-10-10-01-steam\n")),
                       ("exec-out", done(b"run-as: couldn't stat files/logs/2026-10-10-01-steam\n"))])
        code, _, err = self.quiet(pixel10.logs, adb, self.out)
        self.assertEqual(code, 3)
        self.assertIn("pixel10:", err)

    def test_logs_that_cannot_be_written_are_exit_3(self):
        adb = FakeAdb([("ls -1 files/logs", done(b"2026-10-10-01-steam\n")),
                       ("exec-out", done(tar_of({"2026-10-10-01-steam/session.log": b"x"})))])
        with mock.patch.object(tarfile.TarFile, "extractall", side_effect=OSError("No space left on device")):
            code, _, err = self.quiet(pixel10.logs, adb, self.out)
        self.assertEqual(code, 3)
        self.assertIn("No space left", err)

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
        self.assertEqual(adb.calls, [("install", "-r", "--no-incremental", str(apk))])

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
            with self.assertRaisesRegex(pixel10.AdbError, "adb not found"):
                pixel10.adb_path(None)


class MainTest(Sandbox):
    """main(): argument checks, exit codes, and how the pieces are wired. No adb, no device."""

    def setUp(self):
        super().setUp()
        self.fake = FakeAdb()
        self.adb_path = self.patch("adb_path", return_value="adb")
        self.resolve = self.patch("resolve_serial", return_value="fake-1")
        self.make_adb = self.patch("Adb", return_value=self.fake)

    def patch(self, name, **kwargs):
        patcher = mock.patch.object(pixel10, name, **kwargs)
        self.addCleanup(patcher.stop)
        return patcher.start()

    def main(self, *argv):
        return self.quiet(pixel10.main, list(argv))

    def no_device_was_touched(self):
        self.adb_path.assert_not_called()
        self.resolve.assert_not_called()
        self.assertEqual(self.fake.calls, [])

    def test_bad_arguments_are_exit_2(self):
        for argv in ((), ("explode",), ("probe",), ("install",), ("facts", "--serial", "S1")):  # --serial goes before the subcommand
            with self.subTest(argv=argv), self.assertRaises(SystemExit) as raised:
                self.main(*argv)
            self.assertEqual(raised.exception.code, 2)
        self.no_device_was_touched()

    def test_a_missing_local_file_is_exit_2_before_any_device_is_touched(self):
        for argv, message in ((("probe", str(self.out / "missing")), "no such probe binary"),
                              (("install", str(self.out / "missing.apk")), "no such APK"),
                              (("probe", str(self.file("x86", elf(62)))), "AArch64")):
            with self.subTest(argv=argv):
                code, _, err = self.main(*argv)
                self.assertEqual(code, 2)
                self.assertIn(message, err)
        self.no_device_was_touched()

    def test_adb_not_found_is_exit_3(self):
        self.adb_path.side_effect = pixel10.AdbError("adb not found; set ADB")
        code, _, err = self.main("facts", "--out", str(self.out))
        self.assertEqual(code, 3)
        self.assertIn("adb not found", err)
        self.resolve.assert_not_called()

    def test_no_authorized_device_is_exit_3(self):
        self.resolve.side_effect = pixel10.DeviceResolutionError("No authorized Android device found")
        code, _, err = self.main("facts", "--out", str(self.out))
        self.assertEqual(code, 3)
        self.assertIn("No authorized Android device found", err)
        self.assertEqual(self.fake.calls, [])

    def test_adb_failing_in_the_middle_of_a_command_is_exit_3(self):
        self.fake.run = mock.Mock(side_effect=pixel10.AdbError("adb failed: [WinError 2]"))
        code, _, err = self.main("facts", "--out", str(self.out))
        self.assertEqual(code, 3)
        self.assertIn("adb failed", err)

    def test_adb_and_serial_options_reach_the_resolver_and_install_gets_its_full_argv(self):
        apk = self.file("a.apk", b"PK")
        self.fake.answers = [("install", done(b"Performing Streamed Install\nSuccess\n"))]
        code, _, _ = self.main("--adb", "X", "--serial", "S1", "install", str(apk))
        self.assertEqual(code, 0)
        self.adb_path.assert_called_once_with("X")
        self.resolve.assert_called_once_with("adb", "S1", log=True)
        self.make_adb.assert_called_once_with("adb", "fake-1")
        self.assertEqual(self.fake.calls, [("install", "-r", "--no-incremental", str(apk))])

    def test_every_command_reaches_its_function_and_returns_its_exit_code(self):
        self.fake.answers = [("", done(b"", 1, b"error: device offline"))]
        self.assertEqual(self.main("facts", "--out", str(self.out / "facts"))[0], 3)
        self.assertEqual(self.main("probe", str(self.file("p", elf())), "--hal", "/x/y.so", "--out", str(self.out / "probe"))[0], 3)
        self.assertEqual(self.main("install", str(self.file("a.apk", b"PK")))[0], 3)
        self.assertEqual(self.main("logs", "--out", str(self.out / "logs"))[0], 4)  # run-as fails, no zip in Download: needs the user
        self.assertEqual(self.fake.calls[-1], ("shell", "ls -t /sdcard/Download"))


if __name__ == "__main__":
    unittest.main()
