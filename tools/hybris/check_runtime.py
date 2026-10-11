"""Check the isolated driver payload before publishing an APK."""
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import subprocess
import tarfile
from zipfile import ZipFile


REQUIRED = (
    "lib/libhybris-vulkan-icd.so.0", "lib/libhybris-common.so.1",
    "lib/libhybris/linker/q.so", "lib/libVkLayer_hybris_compat.so",
    "lib/libVkLayer_window_system_integration.so",
)
# Vulkan layer manifests the runtime must carry and the layer name each declares. A game's
# VK_INSTANCE_LAYERS names these; the loader skips a name it cannot find without a word, so a layer
# missing here would be a black screen in every game. library_path must be directory-qualified
# ("./libX.so") and resolve inside the archive.
LAYERS = {
    "lib/VkLayer_hybris_compat.json": "VK_LAYER_HYBRIS_compat",
    "lib/VkLayer_window_system_integration.json": "VK_LAYER_window_system_integration",
}
PROVENANCE = (
    "licenses/libhybris-source.tar.zst", "licenses/powervr.patch", "source-commit",
    "licenses/vulkan-wsi-layer.LICENSE", "licenses/vulkan-wsi-layer.patch",
    "licenses/vulkan-wsi-layer-source.tar.zst", "licenses/vulkan-wsi-layer.source-commit",
)


def check(archive, version):
    if hashlib.sha256(archive).hexdigest() != version.strip():
        raise ValueError("system Vulkan archive checksum mismatch")
    unpacked = subprocess.run(["zstd", "-dc"], input=archive, capture_output=True, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(unpacked)) as package:
        members = {}
        for entry in package:
            path = PurePosixPath(entry.name)
            if path.is_absolute() or ".." in path.parts:
                raise ValueError("unsafe runtime archive path")
            name = str(path)
            if name in members:
                raise ValueError("duplicate runtime archive path")
            members[name] = entry

        def read(name, seen=()):
            if name in seen or len(seen) > 8:
                raise ValueError("runtime symlink cycle")
            entry = members.get(name)
            if entry is None:
                raise ValueError("missing runtime file: " + name)
            if entry.issym():
                link = PurePosixPath(entry.linkname)
                if link.is_absolute() or ".." in link.parts:
                    raise ValueError("unsafe runtime symlink")
                return read(str(PurePosixPath(name).parent / link), (*seen, name))
            if not entry.isfile():
                raise ValueError("runtime entry is not a file: " + name)
            return package.extractfile(entry).read()

        for name in REQUIRED:
            data = read(name)
            if data[:6] != b"\x7fELF\x02\x01" or data[18:20] != b"\xb7\x00":
                raise ValueError("runtime library is not ELF64 AArch64: " + name)
        for name, layer_name in LAYERS.items():
            manifest = json.loads(read(name))
            layer = manifest.get("layer") if isinstance(manifest, dict) else None
            layer = layer if isinstance(layer, dict) else {}
            if layer.get("name") != layer_name:
                raise ValueError("runtime layer manifest has the wrong layer name: %s (found %r)"
                                 % (name, layer.get("name")))
            # The loader resolves a path with a directory against the manifest but dlopens a bare name
            # through the library search path, which does not include lib/.
            library = layer.get("library_path")
            if not isinstance(library, str) or "/" not in library:
                raise ValueError("runtime layer manifest has no directory-qualified library_path: " + name)
            read(str(PurePosixPath(name).parent / library))
        for line in read("SHA256SUMS").decode("ascii").splitlines():
            digest, name = line.split("  ", 1)
            if not name.startswith("lib/") or hashlib.sha256(read(name)).hexdigest() != digest:
                raise ValueError("runtime library checksum mismatch: " + name)
        for name in PROVENANCE:
            if not read(name):
                raise ValueError("missing runtime provenance: " + name)


def check_graphics_profile(manifest, wrapper):
    profile = json.loads(manifest)
    if profile.get("format") != 1 or profile.get("id") != "powervr-bc-dxvk-v1":
        raise ValueError("unsupported graphics profile manifest")
    expected = profile.get("wrapper", {})
    if (expected.get("path") != "run-proton.py" or expected.get("size") != len(wrapper)
            or expected.get("sha256") != hashlib.sha256(wrapper).hexdigest()):
        raise ValueError("graphics profile wrapper checksum mismatch")
    compile(wrapper, "run-proton.py", "exec")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    args = parser.parse_args()
    with ZipFile(args.apk) as package:
        check(package.read("assets/hybris/runtime.tzst"),
              package.read("assets/hybris/version.txt").decode("ascii"))
        check_graphics_profile(package.read("assets/graphics-profile/manifest.json"),
                               package.read("assets/graphics-profile/run-proton.py"))
    print("APK system Vulkan payload, source provenance and graphics profile wrapper verified")


if __name__ == "__main__":
    main()
