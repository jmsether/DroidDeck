import hashlib
import importlib.util
import io
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import unittest

spec = importlib.util.spec_from_file_location(
    "hybris_runtime", Path(__file__).resolve().parents[1] / "hybris/check_runtime.py")
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)


class HybrisRuntimeTest(unittest.TestCase):
    def test_graphics_profile_manifest_matches_the_shipped_wrapper(self):
        profile = Path(__file__).resolve().parents[1] / "hybris"
        runtime.check_graphics_profile((profile / "graphics-profile.json").read_bytes(),
                                       (profile / "run-proton.py").read_bytes())

    def test_changed_graphics_wrapper_requires_a_new_manifest(self):
        profile = Path(__file__).resolve().parents[1] / "hybris"
        with self.assertRaisesRegex(ValueError, "graphics profile wrapper checksum mismatch"):
            runtime.check_graphics_profile((profile / "graphics-profile.json").read_bytes(),
                                           (profile / "run-proton.py").read_bytes() + b"\n# changed\n")

    def fixture(self, missing=None, machine=b"\xb7\x00", stale=False, unsafe=False, layers=None):
        """An archive as build_runtime.sh packs it; `layers` overrides fields of a layer manifest."""
        elf = bytearray(64)
        elf[:6] = b"\x7fELF\x02\x01"
        elf[18:20] = machine
        files = {name: bytes(elf) for name in runtime.REQUIRED if name != missing}
        for name, layer_name in runtime.LAYERS.items():
            layer = {"name": layer_name, "library_path": "./lib%s.so" % PurePosixPath(name).stem,
                     **(layers or {}).get(name, {})}
            if name != missing:
                files[name] = json.dumps({"file_format_version": "1.2.0", "layer": layer}).encode()
        files["SHA256SUMS"] = "".join(
            hashlib.sha256(data).hexdigest() + "  " + name + "\n"
            for name, data in files.items()).encode()
        files.update({name: b"provenance" for name in runtime.PROVENANCE if name != missing})
        if stale:
            files[runtime.REQUIRED[0]] += b"changed after hashing"
        raw = io.BytesIO()
        with tarfile.open(fileobj=raw, mode="w") as archive:
            for name, data in files.items():
                member = tarfile.TarInfo(name)
                member.size = len(data)
                if unsafe and name == runtime.REQUIRED[0]:
                    member.type = tarfile.SYMTYPE
                    member.linkname = "../../outside"
                    member.size = 0
                    archive.addfile(member)
                else:
                    archive.addfile(member, io.BytesIO(data))
        packed = subprocess.run(["zstd", "-cq"], input=raw.getvalue(),
                                capture_output=True, check=True).stdout
        return packed, hashlib.sha256(packed).hexdigest()

    def test_complete_aarch64_payload(self):
        runtime.check(*self.fixture())

    def test_missing_wrapper_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing runtime file"):
            runtime.check(*self.fixture(missing=runtime.REQUIRED[0]))

    def test_missing_x11_wsi_layer_is_rejected(self):
        for name in ("lib/libVkLayer_window_system_integration.so", "lib/VkLayer_window_system_integration.json"):
            with self.subTest(name), self.assertRaisesRegex(ValueError, "missing runtime file: " + name):
                runtime.check(*self.fixture(missing=name))

    def test_layer_manifest_must_name_its_layer(self):
        manifest = "lib/VkLayer_window_system_integration.json"
        with self.assertRaisesRegex(ValueError, "wrong layer name: " + manifest + r" \(found 'VK_LAYER_other'\)"):
            runtime.check(*self.fixture(layers={manifest: {"name": "VK_LAYER_other"}}))

    def test_layer_manifest_library_must_be_in_the_archive(self):
        # A bare name passes the archive lookup (it resolves beside the manifest) but the loader would
        # dlopen it through the library search path, where lib/ is deliberately not.
        manifest = "lib/VkLayer_window_system_integration.json"
        for library, error in (("./libMissing.so", "missing runtime file: lib/libMissing.so"),
                               ("/usr/lib/libVkLayer_window_system_integration.so", "missing runtime file: /usr/lib/"),
                               (None, "directory-qualified library_path"),
                               ("libVkLayer_window_system_integration.so", "directory-qualified library_path")):
            with self.subTest(library), self.assertRaisesRegex(ValueError, error):
                runtime.check(*self.fixture(layers={manifest: {"library_path": library}}))

    def test_wsi_layer_provenance_is_required(self):
        self.assertLessEqual({"licenses/vulkan-wsi-layer.LICENSE", "licenses/vulkan-wsi-layer.patch",
                              "licenses/vulkan-wsi-layer-source.tar.zst",
                              "licenses/vulkan-wsi-layer.source-commit"}, set(runtime.PROVENANCE))

    def test_missing_provenance_is_rejected(self):
        for name in runtime.PROVENANCE:
            with self.subTest(name), self.assertRaisesRegex(ValueError, "missing runtime file: " + name):
                runtime.check(*self.fixture(missing=name))

    def test_game_defaults_name_only_layers_the_runtime_ships(self):
        # The loader skips an unknown name in VK_INSTANCE_LAYERS without a word: every game would
        # lose its surface (or the compat layer) and nothing would say why.
        source = (Path(__file__).resolve().parents[2]
                  / "app/src/main/java/com/droiddeck/launcher/core/GameEnvironment.kt").read_text(encoding="utf-8")
        powervr = re.search(r"val POWERVR\b.*?linkedMapOf\((.*?)\n    \)", source, re.S)
        layers = re.search(r'"VK_INSTANCE_LAYERS"\s+to\s+"([^"]+)"', powervr.group(1) if powervr else "")
        self.assertIsNotNone(layers, "GameEnvironment.POWERVR sets no VK_INSTANCE_LAYERS")
        self.assertLessEqual(set(layers.group(1).split(":")), set(runtime.LAYERS.values()))
        self.assertIn("VK_LAYER_window_system_integration", layers.group(1).split(":"))

    def test_host_architecture_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "not ELF64 AArch64"):
            runtime.check(*self.fixture(machine=b"\x3e\x00"))

    def test_modified_library_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "library checksum mismatch"):
            runtime.check(*self.fixture(stale=True))

    def test_archive_checksum_is_required(self):
        packed, _ = self.fixture()
        with self.assertRaisesRegex(ValueError, "archive checksum mismatch"):
            runtime.check(packed, "0" * 64)

    def test_symlink_cannot_escape_payload(self):
        with self.assertRaisesRegex(ValueError, "unsafe runtime symlink"):
            runtime.check(*self.fixture(unsafe=True))
