#!/usr/bin/env bash
# CI-only full libhybris build, using the upstream pinned cross-toolchain recipe.
# Usage: build_runtime.sh OUT WSI_DIR
#   OUT      receives runtime.tzst and version.txt
#   WSI_DIR  the output of tools/vulkan-wsi/build.sh (the X11 surface/swapchain layer). The runtime
#            ships it byte-identical beside the compat layer, so games can present on gamescope's
#            Xwayland; SHA256SUMS there is verified before it is packed.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
usage="usage: build_runtime.sh <output directory> <X11 WSI layer directory (tools/vulkan-wsi/build.sh output)>"
out=${1:?$usage}
wsi=${2:?$usage}
wsi=$(cd "$wsi" && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)
. "$root/tools/hybris/source.env"
work=$(mktemp -d "${TMPDIR:-/tmp}/droiddeck-hybris.XXXXXX")
trap 'rm -rf "$work"' EXIT
"$root/tools/hybris/prepare_source.sh" libhybris "$work/libhybris"
"$root/tools/hybris/prepare_source.sh" rootfs "$work/rootfs"
export ARLINUX_WSI_PROTOCOL_DIR="$work/rootfs/graphics-protocols"
export CONTAINER_ENGINE=podman
"$work/libhybris/tools/build-aarch64.sh" --out "$work/build"

mkdir -p "$work/package/lib" "$work/package/licenses"
cp -a "$work/build/install/usr/lib/hybris/." "$work/package/lib/"
find "$work/package/lib" -name '*.la' -delete
rm -rf "$work/package/lib/include" "$work/package/lib/pkgconfig" "$work/package/lib/bin"
# Resolve the wrapper's private dependencies without putting its EGL/Vulkan frontends
# on the session's LD_LIBRARY_PATH. The Linux runtime keeps owning its Mesa loader.
while IFS= read -r -d '' library; do
  patchelf --set-rpath '$ORIGIN:$ORIGIN/../..' "$library"
done < <(find "$work/package/lib" -type f -name '*.so*' -print0)
# The X11 WSI layer stays byte-identical to its build (no patchelf). Its manifest's library_path is
# relative, so manifest and library sit side by side in lib/; the runtime's SHA256SUMS covers both.
(cd "$wsi" && sha256sum -c SHA256SUMS)
install -m 0755 "$wsi/libVkLayer_window_system_integration.so" "$work/package/lib/"
install -m 0644 "$wsi/VkLayer_window_system_integration.json" "$work/package/lib/"
for library in libhybris-vulkan-icd.so.0 libhybris-common.so.1 libVkLayer_hybris_compat.so libhybris/linker/q.so libVkLayer_window_system_integration.so; do
  test -s "$work/package/lib/$library"
done
cp "$work/libhybris"/LICENSE.* "$work/package/licenses/"
cp "$work/build/manifest.json" "$work/package/build-manifest.json"
printf '%s\n' "$HYBRIS_REVISION" > "$work/package/source-commit"
# Ship the exact patched source and protocol license with the runtime.
git -C "$work/libhybris" diff > "$work/package/licenses/powervr.patch"
git -C "$work/libhybris" archive HEAD | zstd -T0 -q -o "$work/package/licenses/libhybris-source.tar.zst"
cp "$ARLINUX_WSI_PROTOCOL_DIR/wayland-android.xml" "$work/package/licenses/"
# ...and the same for the X11 WSI layer: its license, downstream patch, pinned source and commit.
install -m 0644 "$wsi/LICENSE" "$work/package/licenses/vulkan-wsi-layer.LICENSE"
install -m 0644 "$wsi/downstream.patch" "$work/package/licenses/vulkan-wsi-layer.patch"
install -m 0644 "$wsi/source.tar.zst" "$work/package/licenses/vulkan-wsi-layer-source.tar.zst"
install -m 0644 "$wsi/source-commit" "$work/package/licenses/vulkan-wsi-layer.source-commit"
(cd "$work/package" && find lib -type f -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
tar --zstd -cf "$out/runtime.tzst" -C "$work/package" .
sha256sum "$out/runtime.tzst" | cut -d' ' -f1 > "$out/version.txt"
printf 'Built libhybris %s for Android HAL access from AArch64 Linux\n' "$HYBRIS_REVISION"
