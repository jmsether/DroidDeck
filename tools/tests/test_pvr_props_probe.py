import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_vk_external_image_sync import vulkan_include_args  # noqa: E402  the NDK header lookup

TESTS = Path(__file__).resolve().parent
PROBE = TESTS.parent / "pixel10" / "pvr_props_probe.c"
STRICT = ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-UNDEBUG"]


def facts(output):
    """Probe output as {key: [values]}: one 'scope.key=value' fact per line, keys may repeat."""
    result = {}
    for line in output.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            result.setdefault(key, []).append(value)
    return result


class PvrPropsProbeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler:
            raise AssertionError("A C compiler is required for the PowerVR property probe tests")
        cls.temp = tempfile.TemporaryDirectory(prefix="droiddeck-pvr-probe-")
        tmp = Path(cls.temp.name)
        cls.probe, cls.fake, cls.helpers = tmp / "pvr_props_probe", tmp / "fake_hal.so", tmp / "helpers"
        include = vulkan_include_args()
        for command in (
            [compiler, *STRICT, *include, str(PROBE), "-ldl", "-o", str(cls.probe)],
            [compiler, *STRICT, *include, "-shared", "-fPIC", str(TESTS / "fake_powervr_hal.c"), "-o", str(cls.fake)],
            # the helper test includes the probe whole, so the probe's own main-only functions are unused there
            [compiler, *STRICT, "-Wno-unused-function", *include, str(TESTS / "pvr_props_probe_test.c"), "-ldl",
             "-o", str(cls.helpers)],
        ):
            built = subprocess.run(command, capture_output=True, text=True)
            if built.returncode:
                raise AssertionError(" ".join(command) + "\n" + built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_probe(self, hal=None, **env):
        ran = subprocess.run([str(self.probe), str(hal or self.fake)], capture_output=True, text=True, timeout=120,
                             env={**os.environ, **env})
        return ran.returncode, facts(ran.stdout)

    def test_helpers(self):
        ran = subprocess.run([str(self.helpers)], capture_output=True, text=True, timeout=30)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_hal_identity_and_driver(self):
        _, f = self.run_probe()
        self.assertEqual(f["section.hal"], ["exit=0"])
        self.assertEqual(f["hal.load"], ["plain"])
        self.assertEqual(f["hal.module.id"], ["vulkan"])
        self.assertIn("HWMT ok", f["hal.module.tag"][0])
        self.assertIn("HWDT ok", f["hal.device.tag"][0])
        self.assertEqual(f["hal.device.vendorID"], ["0x1010"])
        self.assertEqual(f["hal.device.deviceID"], ["0x71061212"])
        self.assertEqual(f["hal.device.apiVersion"], ["1.4.317"])
        self.assertEqual(f["hal.device.driverVersion"], ["6908880 (0x696bd0) vk_decoded=1.662.3024"])
        self.assertEqual(f["hal.driver.name"], ["Imagination Technologies"])
        self.assertEqual(f["hal.driver.info"], ["25.3@6908880"])
        self.assertEqual(f["hal.driver.release"], ["25.3 build=6908880 build_matches_driverVersion=1"])
        self.assertIn("VK_EXT_physical_device_drm", f["hal.ext"])
        self.assertIn("VK_EXT_physical_device_drm=1 spec=1", f["hal.has"])
        self.assertIn("VK_EXT_mutable_descriptor_type=0 spec=0", f["hal.has"])

    def test_hal_device_version_is_reported_against_both_encodings(self):
        for version, upstream, patched in (("0x00000001", 0, 1), ("0x00010000", 1, 1), ("0x00020000", 0, 0)):
            with self.subTest(version=version):
                _, f = self.run_probe(FAKE_HAL_VERSION=version)
                self.assertEqual(
                    f["hal.device.version"],
                    [f"{int(version, 16):#010x} accepted_by_upstream_hybris={upstream} accepted_by_powervr_patch={patched}"])

    def test_drm_properties_state(self):
        for mode, state in (("untouched", "untouched"), ("zeroed", "zeroed"), ("filled", "filled")):
            with self.subTest(mode=mode):
                _, f = self.run_probe(FAKE_HAL_DRM=mode)
                self.assertEqual(f["hal.drm.state"], [state])
        _, f = self.run_probe(FAKE_HAL_DRM="filled")
        self.assertIn("render=226:128", f["hal.drm.fields"][0])
        _, f = self.run_probe(FAKE_HAL_DRM="untouched")
        self.assertIn("hasRender=0xcdcdcdcd", f["hal.drm.fields"][0])

    def test_bc_summary_counts_only_d3d_formats(self):
        _, f = self.run_probe()
        self.assertEqual(f["hal.features"], ["textureCompressionBC=0 textureCompressionETC2=0 textureCompressionASTC_LDR=0"])
        self.assertEqual(f["hal.bc.d3d_native_complete"], ["10/14"])
        self.assertEqual(f["hal.bc.d3d_sampled_but_incomplete"], [""])
        self.assertEqual(f["hal.bc.d3d_not_sampleable"],
                         ["BC6H_UFLOAT_BLOCK,BC6H_SFLOAT_BLOCK,BC7_UNORM_BLOCK,BC7_SRGB_BLOCK"])
        self.assertEqual(len(f["hal.fmt"]), 20)

    def test_queue_families_are_listed_for_the_bc_policy(self):
        _, f = self.run_probe()
        self.assertEqual(f["hal.queue.count"], ["1"])
        self.assertEqual(f["hal.queue.family"], ["0 flags=0x7 count=2 granularity=1x1x1"])

    def test_native_buffer_gralloc_usage_width(self):
        _, f = self.run_probe()
        self.assertEqual(f["hal.native_buffer.spec_version"], ["11 (icd needs >= 8)"])
        self.assertEqual(f["hal.native_buffer.gralloc_fns"], ["v1=0 v2=1 v3=0 v4=0 (icd.c calls v2)"])
        usage = [v for k, vs in f.items() if k.startswith("hal.native_buffer.usage.") for v in vs]
        self.assertEqual(len(usage), 6)
        self.assertTrue(all("exceeds_32_bits=0" in v for v in usage))
        _, f = self.run_probe(FAKE_HAL_WIDE_USAGE="1")
        usage = [v for k, vs in f.items() if k.startswith("hal.native_buffer.usage.") for v in vs]
        self.assertTrue(all("exceeds_32_bits=1" in v for v in usage), usage)

    def test_missing_hal_is_a_fact_and_the_other_section_still_runs(self):
        rc, f = self.run_probe(hal="/nonexistent/vulkan.powervr.so")
        self.assertNotEqual(rc, 0)
        self.assertEqual(f["hal.load"], ["failed"])
        self.assertEqual(f["section.hal"], ["exit=2"])
        self.assertIn("section.loader", f)  # ran (and, on a CI box without a loader, reported why it could not)

    def test_a_crashing_hal_costs_its_section_only(self):
        rc, f = self.run_probe(FAKE_HAL_CRASH="1")
        self.assertNotEqual(rc, 0)
        self.assertEqual(f["section.hal"], ["killed signal=11"])
        self.assertEqual(f["hal.device.name"], ["PowerVR D-Series DXT-48-1536 MC1"])  # printed before the crash
        self.assertIn("section.loader", f)

    def test_driver_text_is_sanitized(self):
        # Control bytes and newlines in a driver string must not forge extra fact lines.
        _, f = self.run_probe(FAKE_HAL_DRIVER_INFO="25.3@1\nforged.key=1")
        self.assertNotIn("forged.key", f)
        self.assertEqual(f["hal.driver.info"], ["25.3@1?forged.key=1"])


if __name__ == "__main__":
    unittest.main()
