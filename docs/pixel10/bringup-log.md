# Pixel 10 Pro bring-up log

Newest entry first. Every entry: date, build (commit + APK name), device build fingerprint, driver
string, what ran, result, and where the raw output lives (pixel10-runs/... on the dev PC).
Facts here are measured on the device; anything inferred says so.

---

## 2026-10-11 - Baseline session: Steam's first window kills the session

Build: `DroidDeck-0.3.2-aea026c.apk` (before this port's changes). Device as in Phase 0.
Raw output: `pixel10-runs/logs-baseline-03/unpacked/2026-10-11-03-steam/`,
`pixel10-runs/logcat-droiddeck-baseline.txt`.

What the user saw: Steam downloads its update, then the app returns to the launcher. No crash dialog.

Chain in `session.log`, all at 00:51:04:
- `zink: failed drmPrimeFDToHandle Operation not supported`, repeated.
- gamescope `wlr_linux_dmabuf_v1.c:219 Failed to import DMA-BUF FD for plane 0: Operation not
  supported`.
- Xwayland `error 7: importing the supplied dmabufs failed`; gamescope restarts Xwayland; the
  X clients lose `:0` and the session ends.

Measured on the device with a one-off bionic probe, shell uid (not the app sandbox):
- PRIME FD-to-handle on `renderD128` (`pvr`) with a `/dev/dma_heap/system` buffer: `EINVAL`. This is
  the result `tools/linuxfs/preload/drm.c` emulates, so the emulation path was taken.
- `fcntl(F_DUPFD_QUERY)`: `EINVAL` for every pair (needs Linux 6.10; this kernel is 6.6).
- `kcmp(KCMP_FILE)`: answers from the shell (0 for a dup, 1 for a second open). It is not in AOSP's
  app seccomp allowlist, so inside the app it fails. Not measured in the app directly; the shim's
  `EOPNOTSUPP` shows it returned an error there rather than killing the process.
- DRM fdinfo for this node shows no `drm-client-id`, so it cannot tell two opens apart either.

Cause: with neither identity check available, the shim refused every PowerVR import with
`EOPNOTSUPP` (its fail-safe against a reused descriptor number). Fix on `pixel10`: when the kernel
cannot answer, PowerVR handles match by descriptor number as KGSL's always have, and each process
that hits this prints `droiddeck-drm: this kernel cannot compare open files ...` once. Kernels that
can answer keep the open-file-description checks.

---

## 2026-10-11 - Phase 0 and 1: device facts and property probe

Device: Pixel 10 Pro (`blazer`), `google/blazer/blazer:17/CP3A.260905.009/16091614:user/release-keys`,
Android 17 (SDK 37), security patch 2026-09-05, Tensor G5 (`laguna`), locked bootloader, verified
boot green. No DroidDeck installed. Tool: `tools/pixel10/pixel10.py` at the commits of Task 6.
Raw output: `pixel10-runs/20261011T063608Z/facts/`, `pixel10-runs/20261011T063647Z/probe/props.txt`.

Phase 0 (facts, read-only):
- Vulkan HAL `/vendor/lib64/hw/vulkan.powervr.so` present (3,167,784 bytes,
  `same_process_hal_file`), so `SystemVulkanDriver.isPowerVr()` is true. `ro.hardware.vulkan=powervr`.
- `/dev/dri/renderD128` is char 226:128, `crw-rw-rw-`, `gpu_device`, sysfs `DRIVER=pvrsrvkm`
  (`/sys/class/drm/renderD128 -> .../34f00000.gpu0/drm/renderD128`). `/dev/dri` also lists
  `card0` (display), `card1` (g2d) and `card2` (226:2, the GPU's own primary node).
- Kernel `6.6.127-android15-8-...-4k`; `PAGE_SIZE` 4096; `CONFIG_ARM64_VA_BITS=39` (the 39-bit
  address-space limit of upstream #415 applies).
- CPU: 8 cores; features include `atomics lrcpc ilrcpc uscat flagm2 afp sve2 i8mm bf16`.
- System Vulkan (vkjson): PowerVR D-Series DXT-48-1536 MC1, vendor 0x1010, device 0x71061212,
  API 1.4.317, driverInfo `25.3@6908880` (conformance 1.4.0.5); 161 device extensions including
  `VK_EXT_physical_device_drm`, `VK_KHR_push_descriptor`, `VK_KHR_maintenance5`, `_6`;
  `textureCompressionBC` false; one queue family, flags 0x1f, 2 queues, granularity 1x1x1.
- `settings_enable_monitor_phantom_procs` and `max_phantom_processes` read `null` (defaults).

Phase 1 (property probe, bionic, shell uid, HAL dlopened directly):
- `hal.device.version=0x00000001`: rejected by upstream hybris, accepted by
  `tools/hybris/powervr-hal-version.patch`. Gate passes.
- `hal.drm.state=filled` (`hasPrimary=0 hasRender=1 render=226:128`): the 25.3 HAL fills
  `VkPhysicalDeviceDrmPropertiesEXT` itself, contrary to the research prediction (which came from
  a third-party ICD). Task 8's fill is therefore a no-op on this device (`outcome=driver-answered`
  expected); it stays as protection for drivers that do not answer.
- BC: `d3d_native_complete=10/14` (BC1 RGBA through BC5 with all five required features),
  `d3d_sampled_but_incomplete` empty, not sampleable: BC6H UF/SF, BC7 UNORM/SRGB. Matches the `d3d`
  policy's expected emulate mask 0xF000.
- `VK_ANDROID_native_buffer` spec 11 (ICD needs >= 8); gralloc functions v1..v4 exported (ICD calls
  v2); every RGBA8/BGRA8 swapchain usage query fits in 32 bits. Gate passes.
- Queue family 0: flags 0x1f, granularity 1x1x1: the BC policy's queue refusal cannot trigger.
- The probe's Vulkan device create succeeded (`vkCreateDevice=0`); no hang, no reboot.
