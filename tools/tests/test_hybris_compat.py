"""Host checks of DroidDeck's libhybris patches (tools/hybris/patches) against the pinned source.

The tree comes from tools/hybris/prepare_source.sh, the script the CI runtime build uses, so the
code tested here is the code shipped. HYBRIS_PATCHED_DIR may name an already prepared tree and
VULKAN_INCLUDE a Vulkan-Headers include directory (>= 1.4.303); otherwise both are fetched at their
pins. Each policy scenario runs in its own process: the compat layer reads its environment once.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TESTS = Path(__file__).resolve().parent
PREPARE = ROOT / "tools/hybris/prepare_source.sh"
MIN_HEADER = 303


def header_version(include):
    header = Path(include) / "vulkan/vulkan_core.h"
    if not header.is_file():
        return 0
    found = re.search(r"#define VK_HEADER_VERSION (\d+)", header.read_text(errors="replace"))
    return int(found.group(1)) if found else 0


class HybrisCompatTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which(os.environ.get("CC", "cc"))
        if not cls.compiler:
            raise AssertionError("A C compiler is required for the libhybris compat tests")
        cls.tmp = tempfile.TemporaryDirectory(prefix="droiddeck-hybris-")
        cls.addClassCleanup(cls.tmp.cleanup)
        work = Path(cls.tmp.name)
        include = os.environ.get("VULKAN_INCLUDE")
        if not include:
            subprocess.run(["bash", str(PREPARE), "headers", str(work / "vulkan-headers")], check=True,
                           timeout=1000)
            include = work / "vulkan-headers/include"
        if header_version(include) < MIN_HEADER:
            raise AssertionError(f"Vulkan headers >= 1.4.{MIN_HEADER} needed at {include}")
        tree = os.environ.get("HYBRIS_PATCHED_DIR")
        if not tree:
            tree = work / "libhybris"
            subprocess.run(["bash", str(PREPARE), "libhybris", str(tree)], check=True, timeout=1000)
        cls.vulkan = Path(tree) / "hybris/vulkan"
        cls.include = Path(include)

    def build(self, name, sources):
        program = Path(self.tmp.name) / name
        command = [self.compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-UNDEBUG", "-pthread",
                   "-I", str(self.include), "-I", str(self.vulkan), *map(str, sources), "-o", str(program)]
        built = subprocess.run(command, capture_output=True, text=True, errors="replace")
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        return program

    def run_program(self, program, *args, **env):
        environment = {k: v for k, v in os.environ.items()
                       if not k.startswith("HYBRIS_") and k != "EXPECT_DISABLED"}
        environment.update(env)
        # cwd is the scratch directory: the DRM harness writes its redirected stderr (and any assert
        # text) to drm_test_stderr.txt there, never into the checkout.
        ran = subprocess.run([str(program), *args], capture_output=True, text=True, errors="replace", timeout=60,
                             env=environment, cwd=self.tmp.name)
        log = Path(self.tmp.name) / "drm_test_stderr.txt"
        detail = log.read_text(errors="replace") if program.name.startswith("drm-") and log.is_file() else ""
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr + detail)
        return ran

    def test_patched_tree_compiles_the_bc_policy(self):
        # Characterization: today's patched bc_policy.c builds warning-free against the pinned headers.
        sentinel = "candidate->common.version != HARDWARE_DEVICE_API_VERSION(0, 1)"
        self.assertTrue(sentinel in (self.vulkan / "icd/icd.c").read_text(errors="replace"),
                        "tree is not patched with powervr-hal-version.patch")
        objects = [self.vulkan / "compat/bc_policy.c", self.vulkan / "compat/bc_image.c",
                   *sorted(self.vulkan.glob("compat/bc_rules.c"))]
        command = [self.compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread", "-fsyntax-only",
                   "-I", str(self.include), "-I", str(self.vulkan), *map(str, objects)]
        checked = subprocess.run(command, capture_output=True, text=True, errors="replace")
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_drm_properties_fill(self):
        program = self.build("drm-properties", [TESTS / "hybris_drm_properties_test.c",
                                                self.vulkan / "compat/drm_properties.c"])
        self.run_program(program)
        self.run_program(program, HYBRIS_VULKAN_COMPAT_DRM_PROPERTIES="0", EXPECT_DISABLED="1")

    def test_layer_properties_handler_compiles(self):
        # The properties2 handler is in the layer, which the DRM harness does not build.
        command = [self.compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread", "-fsyntax-only",
                   "-I", str(self.include), "-I", str(self.vulkan), str(self.vulkan / "layer/physical.c")]
        checked = subprocess.run(command, capture_output=True, text=True, errors="replace")
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_unregistered_device_gets_no_drm_fill(self):
        program = self.build("drm-layer", [TESTS / "hybris_drm_layer_test.c", self.vulkan / "layer/physical.c",
                                           self.vulkan / "compat/drm_properties.c"])
        self.run_program(program)

    def test_bc_rules(self):
        program = self.build("bc-rules", [TESTS / "hybris_bc_rules_check.c", self.vulkan / "compat/bc_rules.c"])
        self.assertIn("failures=0", self.run_program(program).stdout)

    def test_bc_policy_against_a_vulkan_1_4_driver(self):
        program = self.build("bc-policy", [TESTS / "hybris_bc_policy_fake.c",
                                           *(self.vulkan / f"compat/{name}.c" for name in ("bc_policy", "bc_rules", "bc_image"))])
        for policy in ("unset", "missing", "force", "dxvk", "d3d", "0"):
            with self.subTest(policy=policy):
                env = {} if policy == "unset" else {"HYBRIS_BC_TEXTURES": policy}
                self.assertIn("failures=0", self.run_program(program, policy, **env).stdout)


if __name__ == "__main__":
    unittest.main()
