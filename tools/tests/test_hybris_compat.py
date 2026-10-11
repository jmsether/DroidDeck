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
        work = Path(cls.tmp.name)
        tree = os.environ.get("HYBRIS_PATCHED_DIR")
        if not tree:
            tree = work / "libhybris"
            subprocess.run(["bash", str(PREPARE), "libhybris", str(tree)], check=True)
        include = os.environ.get("VULKAN_INCLUDE")
        if not include:
            subprocess.run(["bash", str(PREPARE), "headers", str(work / "vulkan-headers")], check=True)
            include = work / "vulkan-headers/include"
        if header_version(include) < MIN_HEADER:
            raise AssertionError(f"Vulkan headers >= 1.4.{MIN_HEADER} needed at {include}")
        cls.vulkan = Path(tree) / "hybris/vulkan"
        cls.include = Path(include)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def build(self, name, sources):
        program = Path(self.tmp.name) / name
        command = [self.compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-UNDEBUG", "-pthread",
                   "-I", str(self.include), "-I", str(self.vulkan), *map(str, sources), "-o", str(program)]
        built = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        return program

    def run_program(self, program, *args, **env):
        environment = {k: v for k, v in os.environ.items()
                       if not k.startswith("HYBRIS_") and k != "EXPECT_DISABLED"}
        environment.update(env)
        # cwd is the scratch directory: the DRM harness writes its redirected stderr (and any assert
        # text) to drm_test_stderr.txt there, never into the checkout.
        ran = subprocess.run([str(program), *args], capture_output=True, text=True, timeout=60, env=environment,
                             cwd=self.tmp.name)
        log = Path(self.tmp.name) / "drm_test_stderr.txt"
        detail = log.read_text() if program.name == "drm-properties" and log.is_file() else ""
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr + detail)
        return ran

    def test_patched_tree_compiles_the_bc_policy(self):
        # Characterization: today's patched bc_policy.c builds warning-free against the pinned headers.
        source = Path(self.tmp.name) / "empty_main.c"
        source.write_text("int main(void) { return 0; }\n")
        objects = [self.vulkan / "compat/bc_policy.c", self.vulkan / "compat/bc_image.c"]
        command = [self.compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread", "-fsyntax-only",
                   "-I", str(self.include), "-I", str(self.vulkan), *map(str, objects)]
        checked = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)


if __name__ == "__main__":
    unittest.main()
