#!/usr/bin/env bash
# The one way DroidDeck obtains its pinned libhybris inputs (tools/hybris/source.env):
#   prepare_source.sh libhybris DEST   taowen/libhybris at the pin, patched as the runtime ships it
#   prepare_source.sh rootfs DEST      taowen/arlinux-rootfs at the pin (graphics protocols)
#   prepare_source.sh headers DEST     KhronosGroup/Vulkan-Headers at the pin (host tests)
# DEST must not exist. Patch order: powervr-hal-version.patch, then patches/*.patch by name.
# Used by build_runtime.sh (CI) and tools/tests/test_hybris_compat.py, so both see the same tree.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/hybris/source.env"
fetch() {
  git init -q "$3"
  git -C "$3" fetch -q --depth=1 "$1" "$2"
  git -C "$3" checkout -q --detach FETCH_HEAD
  test "$(git -C "$3" rev-parse HEAD)" = "$2"
}
what=${1:?libhybris, rootfs or headers}
dest=${2:?destination required}
test ! -e "$dest" || { echo "prepare_source.sh: $dest already exists" >&2; exit 2; }
case "$what" in
  libhybris)
    fetch https://github.com/taowen/libhybris.git "$HYBRIS_REVISION" "$dest"
    for patch in "$root/tools/hybris/powervr-hal-version.patch" "$root"/tools/hybris/patches/*.patch; do
      git -C "$dest" -c core.autocrlf=false apply --check "$patch"
      git -C "$dest" -c core.autocrlf=false apply "$patch"
    done
    # Files a patch creates are untracked; mark them so `git diff` (the shipped provenance patch)
    # includes them.
    git -C "$dest" add -N . ;;
  rootfs) fetch https://github.com/taowen/arlinux-rootfs.git "$PROTOCOL_REVISION" "$dest" ;;
  headers) fetch https://github.com/KhronosGroup/Vulkan-Headers.git "$VULKAN_HEADERS_REVISION" "$dest" ;;
  *) echo "prepare_source.sh: unknown input $what" >&2; exit 2 ;;
esac
