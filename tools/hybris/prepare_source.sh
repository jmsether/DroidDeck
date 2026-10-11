#!/usr/bin/env bash
# The one way DroidDeck obtains its pinned libhybris inputs (tools/hybris/source.env):
#   prepare_source.sh libhybris DEST   taowen/libhybris at the pin, patched as the runtime ships it
#   prepare_source.sh rootfs DEST      taowen/arlinux-rootfs at the pin (graphics protocols)
#   prepare_source.sh headers DEST     KhronosGroup/Vulkan-Headers at the pin (host tests)
# DEST must not exist. Patch order: powervr-hal-version.patch, then patches/*.patch by name.
# Used by build_runtime.sh (CI) and tools/tests/test_hybris_compat.py, so both see the same tree.
set -euo pipefail
export GIT_TERMINAL_PROMPT=0  # a pinned repo that goes private must fail, not prompt
root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/hybris/source.env"
fetch() {
  git init -q "$3"
  # Transient network failures: up to 3 bounded attempts (5 s, then 10 s apart), then fail.
  for attempt in 1 2 3; do
    timeout 300 git -C "$3" fetch -q --depth=1 "$1" "$2" && break
    echo "prepare_source.sh: fetch of $1 failed (attempt $attempt of 3)" >&2
    [ "$attempt" -lt 3 ] || return 1
    sleep $((attempt * 5))
  done
  git -C "$3" checkout -q --detach FETCH_HEAD
  test "$(git -C "$3" rev-parse HEAD)" = "$2"
}
what=${1:?libhybris, rootfs or headers}
dest=${2:?destination required}
test ! -e "$dest" || { echo "prepare_source.sh: $dest already exists" >&2; exit 2; }
# Past the guard this script created $dest, so a failure never leaves a half-prepared tree behind.
trap 'status=$?; [ "$status" -eq 0 ] || rm -rf "$dest"' EXIT
case "$what" in
  libhybris)
    fetch https://github.com/taowen/libhybris.git "$HYBRIS_REVISION" "$dest"
    for patch in "$root/tools/hybris/powervr-hal-version.patch" "$root"/tools/hybris/patches/*.patch; do
      git -C "$dest" -c core.autocrlf=false apply --check "$patch"
      git -C "$dest" -c core.autocrlf=false apply "$patch"
    done
    # Files a patch creates are untracked; mark them so `git diff` (the shipped provenance patch)
    # includes them. -f because libhybris's .gitignore (config.*, compile, missing, ...) would
    # otherwise silently drop patch-created files from that diff; a fresh checkout has no ignored
    # files, so -f only picks up patch-created ones.
    git -C "$dest" add -N -f . ;;
  rootfs) fetch https://github.com/taowen/arlinux-rootfs.git "$PROTOCOL_REVISION" "$dest" ;;
  headers) fetch https://github.com/KhronosGroup/Vulkan-Headers.git "$VULKAN_HEADERS_REVISION" "$dest" ;;
  *) echo "prepare_source.sh: unknown input $what" >&2; exit 2 ;;
esac
