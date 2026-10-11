# PowerVR through the existing ARLinux libhybris wrapper

An offscreen Vulkan draw passed on Pixel 11 Pro XL / Tensor G6 / PowerVR
C-Series CXTP-48-1536 MC1 on 2026-10-07, without AVF. This is a tested reuse
candidate. The experimental integration selects it automatically for PowerVR,
keeps its libraries separate from the rootfs, and presents Android buffers through
the existing DroidDeck compositor. Steam/window validation is still in progress.

## Inputs

- [ARLinux's upstream reuse report](https://github.com/Droid-Deck/DroidDeck/issues/165#issuecomment-6014766410).
- [libhybris source](https://github.com/taowen/libhybris/tree/bc8cb3586b79d208fd21b925fa0209b3e432e008),
  commit `bc8cb3586b79d208fd21b925fa0209b3e432e008` on `arlinux`.
- [Arch runtime release v0.1.3](https://github.com/taowen/arlinux-arch/releases/tag/v0.1.3),
  `arch.zip` SHA-256
  `a2dfcc565a2bbdfdf0743e279c9fd85bb40aee44ca39a8d461c3c72f93135f37`.
  Its generic GPU overlay supplied the Android ABI bridge. Only the five-file
  Vulkan ICD was rebuilt from the source revision above, with Wayland disabled.
- The standalone [triangle probe](render-probe.c), originally built by CI at
  DroidDeck commit `10ab01bc89e2668c132ed35cce1b1274cd931910`. Its runtime library search
  path was adjusted to select the isolated Linux loader and hybris libraries.

## Build the standalone probe

From the repository root:

```sh
python3 tools/hybris/build_render_probe.py "$TMPDIR/droiddeck-hybris-render"
```

The builder uses the installed Termux glibc toolchain, or an AArch64 Linux cross
compiler on Linux. It compiles only the small test executable and two shaders.
Run from an AArch64 glibc environment configured for the isolated runtime. Set
`HYBRIS_ICD` to the absolute path of that runtime's libhybris ICD manifest; the
required expansion below prevents an unset value from selecting a default ICD.
Both loader variable names select the same manifest, including older loaders.

```sh
VK_DRIVER_FILES="${HYBRIS_ICD:?Set HYBRIS_ICD to the isolated ICD manifest}" \
VK_ICD_FILENAMES="${HYBRIS_ICD:?Set HYBRIS_ICD to the isolated ICD manifest}" \
  "$TMPDIR/droiddeck-hybris-render/render-probe" 0x1010 \
  "$TMPDIR/droiddeck-hybris-render/triangle.ppm"
```

On Termux, place the glibc launcher (for example, `aether-run`) before the
executable path. The builder neither installs the probe on `PATH` nor installs
the isolated runtime.

This directory has no dependency on the AVF kernel, staging tools or workflow.
Those experiments remain on the `wallentx/powervr-avf` branch.

## Compatibility finding

The PowerVR HAL opens successfully and supplies the expected device tag and
all three required Vulkan entry points. It reports device version `0x00000001`,
the `HARDWARE_DEVICE_API_VERSION(0, 1)` encoding. Upstream hybris accepts only
`HWVULKAN_DEVICE_API_VERSION_0_1`, encoded as `0x00010000`.

[powervr-hal-version.patch](powervr-hal-version.patch) accepts both encodings
while retaining the other device validation. Android's own
[Vulkan loader](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/android17-release/vulkan/libvulkan/driver.cpp)
does not enforce that exact device-version comparison.

Two otherwise identical offscreen ICD builds produced:

| Check | Unchanged upstream | With version patch |
| --- | --- | --- |
| HAL metadata accepted | No | Yes |
| Vulkan instance and PowerVR device | No | Yes |
| Graphics pipeline and draw | Not reached | Passed |
| Fence and image readback | Not reached | Passed |
| Pixels | No image | 1,152 red; 2,944 blue; zero others |

The image was also counted independently of the C probe. The test ran through
Termux's command service and Aether's glibc launcher, UID 10445, SELinux domain
`runas_app`. It does not establish behavior under DroidDeck's own app identity.
The compatibility layer was disabled: BC emulation and games remain untested.

## Experimental presentation integration

ARLinux's existing `android_wlegl` protocol exchanges Android native handles
and server-allocated buffers. DroidDeck already handles Android Hardware
Buffers through its `banner_ahb_v1` protocol. The new `android_wlegl` adapter
imports native handles using Android's buffer API, then imports those buffers
into the system Vulkan device. It does not infer a DMA-BUF layout from vendor
metadata. The original Adreno path remains separate.

The producer waits for GPU completion before committing; DroidDeck acquires
and returns buffer ownership and waits for its own render fence before releasing
the Wayland buffer. The compositor's existing surface and input lifecycle is reused.
End-to-end window and Steam testing remains required before calling this tested
support. No AVF transport is used by this candidate.

`build_runtime.sh` builds the pinned upstream wrapper and our HAL-version patch
on CI. The fetch-and-patch step lives in `tools/hybris/prepare_source.sh`, shared with the
host tests in `tools/tests/test_hybris_compat.py`. The APK carries an isolated runtime
archive, licenses, source and patch.
`check_runtime.py` verifies the archive, AArch64 libraries and provenance before
upload. Explicit imported driver choices are preserved; Auto selects this wrapper
only on PowerVR devices.

System PowerVR sessions also enable `BL_POWERVR_DRM_HANDLES=1` in the session
preload. The Android `pvr` render node rejects PRIME-to-GEM conversion even for
DMA-BUFs that its Vulkan driver exports and imports successfully. The adapter
tries the real operation first, then provides an owned descriptor-backed handle
only for that node's `EINVAL` result. Real kernel handles continue to libdrm.
An owned render descriptor and open-file-description checks prevent a reused
descriptor number from inheriting an old handle. Other drivers and manually
selected ICDs do not enable this path.

`python3 -m unittest tools.tests.test_drm_handles` checks scoping, descriptor
reuse, fork behavior, handle cleanup and native-handle passthrough. Native device
probes also verified DMA-BUF identity and Vulkan memory import/image binding;
these checks do not establish correct pixel rendering or end-to-end Steam support.

`BL_STEAM_GL_PRESENT=1` selects OpenGL/DRI3 presentation for the system-PowerVR
Steam client. The session disables Gamescope's implicit WSI layer and Mesa's
Kopper path, and selects ANGLE's OpenGL backend. Zink still renders through the
system Vulkan driver; this avoids creating Vulkan window surfaces against an inner
server that provides neither `android_wlegl` nor `TAWC-DRI`. Other driver selections
retain the existing Steam presentation settings. Device rendering remains unverified.

## Local evidence

The test checkout keeps the release archive, source, build commands, patch,
HAL metadata, before/after logs, image, result JSON and rerun instructions under
the ignored `.local/benchmarks/powervr/hybris/` directory. No device libraries,
normal Arch kernel, or DroidDeck runtime were replaced.

## Experimental BC decoding for Direct3D

[BC-PROFILE.md](BC-PROFILE.md) describes the opt-in profile, patched DXVK,
GPU regression evidence and remaining app and precision gates.
