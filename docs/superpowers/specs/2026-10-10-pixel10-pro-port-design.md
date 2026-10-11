# DroidDeck on the Pixel 10 Pro (Tensor G5 / PowerVR) — design

Status: approved 2026-10-10. Branch: `pixel10` (personal fork; not intended for upstream).

## 1. Goal and scope

Run DroidDeck on the Google Pixel 10 Pro (`blazer`) on **stable Android 17 firmware**
(PowerVR driver 25.3@6908880, Vulkan 1.4.317): Steam Big Picture, Windows DX9/10/11
games through DXVK, and DX12 games through VKD3D-Proton — with as many games as
possible working **without per-game settings**.

The other Pixel 10 models (10, 10 Pro XL, 10 Pro Fold) share the Tensor G5 SoC and
PowerVR D-Series DXT-48-1536 GPU and should benefit, but only the Pixel 10 Pro is a
test target.

Non-goals: upstream submission, Adreno validation, AVF/Venus transports, Mesa's
open PowerVR driver (it does not support D-Series and Pixel uses the proprietary
`pvrsrvkm` kernel driver), unrelated features carried by the base branch.

## 2. Starting point (research summary, 2026-10-10)

Upstream DroidDeck is Adreno-only at three layers: the guest rootfs ships only Turnip
on KGSL; the in-app compositor's Auto driver falls through to `turnip-sdk36` on
Android 16+ non-Adreno devices; the UI only knows Adreno families. No working Pixel 10
port exists. Prior art:

| Effort | Transport | Furthest result | Device |
|---|---|---|---|
| `wallentx/dev` (= tag `v0.3.2-progress.20261010.4`, `abe58618`) | libhybris (taowen/libhybris `bc8cb358`) + `android_wlegl` compositor import + gamescope p12 patches 0120–0125 + manual DXVK BC profile | Steam UI; Dark Souls Remastered ~16 FPS | Pixel 11 Pro XL (Tensor G6, driver 26.1) |
| `gogetwoo/pixel-venus`, `Masuary/pixel-powervr` | Venus over vtest | Steam sign-in, frozen/flickering frames; vtest has no dma_buf import | Pixel 10 Pro XL |
| `Fizzolas/DroidDeckTensor` | own libhybris build | Steam client segfaults in the bionic linker; commits proprietary vendor blobs (never copy) | Pixel 10 Pro XL |

Pixel 10 GPU facts that shape the design (gpuinfo.org reports 49760, 50840, 51436, 51806):

- `textureCompressionBC = false` on every driver; BC1–BC5 are individually sampleable,
  BC6H/BC7 are absent. Stock DXVK drops the adapter without the feature.
- The libhybris compat layer's default BC policy is disabled for `apiVersion >= 1.4`,
  so it does nothing on Pixel 10 today.
- Missing: `VK_EXT_mutable_descriptor_type`, `VK_EXT_memory_budget`,
  `VK_EXT_external_memory_host`, `VK_EXT_map_memory_placed`.
- Predicted first M1 failure: the 25.3 HAL advertises `VK_EXT_physical_device_drm` but
  leaves `VkPhysicalDeviceDrmPropertiesEXT` unfilled, and gamescope 3.16.29 requires
  `hasRender`.
- Open 25.3 driver bugs: `CmdBeginRenderPass2` / load-op of stored attachments
  (flutter#194034), `CmdClearAttachments` (Eden#561), `CreatePipelineCache` (godot#115171).
  DXVK uses dynamic rendering and never creates pipeline caches; VKD3D-Proton does.
- Kernel 6.6 with 4 KiB pages and a 39-bit VA (same limit as upstream #415).

## 3. Decisions

| # | Decision | Reason |
|---|---|---|
| D1 | Base on `wallentx/dev`, merge upstream `main` into it | Only stack with real games on PowerVR; personal fork tolerates its unrelated changes |
| D2 | Build with GitHub Actions on the user's fork | wallentx's CI already builds the hybris runtime, gamescope p12 and the APK green; local toolchain absent, E: nearly full |
| D3 | Driver gaps are fixed once, in the libhybris compat layer | It is the single door between the guest and the vendor driver; fixes there reach gamescope, DXVK, VKD3D and everything else |
| D4 | BC handled in the layer; stock Proton DXVK/VKD3D unmodified | Removes the patched-DXVK, per-game wrapper and companion-ZIP workflow |
| D5 | Game support ladder: global PowerVR default → driver-version-gated workaround → per-game override last | User goal: maximum games with no per-game setup |
| D6 | Commits keep the Claude co-author trailer | Personal fork; upstream's AI-trailer CI check does not apply |

## 4. Repository and build

- Local clone `E:\projects\PixelDroidDeck`; remotes `origin` (upstream) and `wallentx`
  are read-only sources, merged periodically; the user's GitHub fork is the push target
  (created and pushed only with the user's confirmation).
- `pixel10` starts at `abe58618`. First change: merge `origin/main`; resolve
  `docs/progress-log.md`; then the full CI build and JVM test suite must pass before any
  Pixel work, because both sides edited `OnScreenControls.kt` and `SessionActivity.kt`.
- Component releases (gamescope, hybris runtime inputs) publish to the fork's own
  releases through the existing `BUILD_ASSETS_REPOSITORY` mechanism; every pin keeps its
  SHA-256.
- APKs are signed with the public test key and share the package name
  `com.droiddeck.launcher`; an official DroidDeck install must be removed first (this
  deletes its runtime and data).
- Device side: Android platform-tools (adb) on the Windows PC.

## 5. Bring-up sequence

Each phase is gated on device evidence; nothing is fixed on a prediction alone.

1. **Phase 0 — device facts (read-only adb).** `ro.build.fingerprint`;
   `ls -l /vendor/lib64/hw/vulkan.powervr.so /dev/dri/`; `getconf PAGE_SIZE`;
   `/proc/cpuinfo` features; Vulkan driver string; the `[system]` section of
   `/linkerconfig/ld.config.txt`. Developer option "Restrict child processes" off.
2. **Phase 1 — probes.** wallentx's libhybris offscreen-triangle probe
   (`tools/hybris/build_render_probe.py`; pass = 1152 red, 2944 blue, 0 other pixels)
   plus a property probe recording: HAL `common.version` (the carried patch accepts
   `0x00010000` and `0x00000001`), `VkPhysicalDeviceDrmPropertiesEXT` contents,
   `VK_ANDROID_native_buffer` spec version (ICD requires ≥ 8), swapchain gralloc usage
   width (ICD rejects > 32 bits), RGBA8/BGRA8 and BC format features.
3. **Phase 2 — merged fork, unmodified.** Install the CI APK, start a Steam session,
   pull the session logs.
4. **Phase 3 — fix loop.** One failure at a time; each fix is a small commit with a
   test; each device result is appended to `docs/pixel10/bringup-log.md` with build SHA,
   driver string and log reference.

Milestones:

- **M1** — Steam Big Picture renders and accepts touch and controller input.
- **M2** — DX9/DX11 games reach gameplay through stock Proton DXVK, no per-game settings.
- **M3** — a DX12 game reaches gameplay through stock Proton VKD3D-Proton.

The user supplies 2–3 test titles spanning DX9/11/12 at M2.

## 6. Changes

All libhybris changes are patch files under `tools/hybris/patches/`, applied by the
existing `tools/hybris/build_runtime.sh` to the pinned taowen revision.

### 6.1 DRM properties fill (M1)

Rail: the compat layer's `vkGetPhysicalDeviceProperties2` path. Before calling the
HAL, mark the chained `VkPhysicalDeviceDrmPropertiesEXT` with a sentinel; if the HAL
leaves it untouched and `/dev/dri/renderD128` stats as a character device, fill
`hasRender`, `renderMajor`, `renderMinor` from the stat (`hasPrimary` stays false).
Otherwise pass the HAL's answer through unchanged and emit a diagnostic. Preferred over
relaxing gamescope's `hasRender` check because every guest consumer gets a correct
answer.

### 6.2 BC texture support (M2, M3)

Rail: the compat layer's BC policy (`hybris/vulkan/compat/bc_*.c`).

- Remove the `apiVersion >= 1.4` exclusion for PowerVR so the policy runs on Pixel 10.
- Decode only formats the driver cannot sample natively (BC6H, BC7); BC1–BC5 stay native
  unless device tests show misrendering, in which case decode-all becomes the PowerVR
  default (wallentx's existing `dxvk` policy).
- Report `textureCompressionBC = true` only when every D3D BC format offers the
  spec-mandated optimal-tiling features (sampled, linear filter, blit source, transfer
  source/destination) natively or through emulation; otherwise keep the driver's
  `false`.
- Result: Proton's stock DXVK and VKD3D-Proton accept the device. wallentx's patched
  DXVK, `DROIDDECK_PROTON_WRAPPER` BC workflow and companion ZIPs are not used.

### 6.3 PowerVR session defaults

Rail: `SystemVulkanDriver.addEnvironment` (the existing PowerVR environment door). For
every PowerVR session set the BC policy and `DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1`
(Fossilize caused `VK_ERROR_SURFACE_LOST_KHR`). No per-game action required.

### 6.4 DX12

Hard VKD3D-Proton requirements are met by the driver. On device, test behaviour without
`VK_EXT_mutable_descriptor_type`. If the `IMG_vkCreatePipelineCache` crash reproduces,
the compat layer replaces pipeline-cache creation with an empty layer-owned cache for
driver 25.3@6908880 only.

### 6.5 Driver 25.3 workarounds (conditional)

Built only if M2/M3 evidence shows the render-pass load-op or `CmdClearAttachments` bugs
hit DXVK or VKD3D. Each workaround lives in the compat layer, is gated on the exact
driver version, and has a test.

### 6.6 Detection and UI

Consolidate the duplicated Adreno predicate (`core/DeviceSupport.kt` and
`gpu/GpuInfo.kt`) into one, and stop showing the "Not an Adreno GPU … black screen"
install dialog when PowerVR is detected (`SystemVulkanDriver.isPowerVr()`).

### 6.7 Left as-is unless a milestone is blocked

Steam's software CEF compositing, the old-frame replay glitch, wallentx's unrelated
audio/Box64/input changes, foldable handling.

## 7. Error handling

- **Value errors** (unreadable property, format that cannot be emulated): keep the
  driver's own answer and emit a diagnostic; never fabricate support.
- **Integrity**: every bundled component and pin is SHA-256 checked
  (`tools/hybris/check_runtime.py`, `release.env` pins); a mismatch fails the build.
  No proprietary vendor files are committed or bundled; the phone's own driver is loaded
  at runtime.
- **Diagnostics**: each workaround logs once per session with the driver version when it
  activates. The session report (`device.txt`) gains a PowerVR section: driver string,
  HAL version, BC mode, active workarounds.
- **Escape hatch**: each workaround can be disabled through the existing
  `Download/droiddeck-env` override for diagnosis; no new settings UI.
- **Per-game overrides** (last rung of D5) use the existing `droiddeck-game-env` hook;
  no new mechanism.

## 8. Testing

Host (CI):

- DRM fill: fake HAL that leaves the struct untouched → filled from stat; fake HAL that
  fills it → passed through; missing node → unchanged plus diagnostic.
- BC reporting: feature true only when all D3D BC formats are covered; false otherwise.
- JVM: consolidated detection (Adreno / PowerVR / neither) and PowerVR session defaults.
- Existing upstream and wallentx suites stay green; none are weakened.

Device (user-run, guided):

- `tools/pixel10/` adb scripts: facts dump, probes, install, log pull.
- Every run recorded in `docs/pixel10/bringup-log.md`.

Acceptance:

- M1: Steam Big Picture renders and takes input through a 10-minute session, no crash.
- M2: 2–3 DX9/DX11 titles reach gameplay with no per-game settings.
- M3: one DX12 title reaches gameplay.

## 9. Risks

- **GPU lockup → forced reboot** (seen on Pixel 11 under this stack). Expected
  occasionally during bring-up; keep no unsaved work on the phone while testing.
- **Driver 25.3 blocker without workaround**: fallback is the Android 17 QPR2 beta
  (driver 26.1). Enrolling keeps data; leaving the beta for stable wipes the device.
  The user decides if and when.
- **libhybris fragility** across Android updates (vendored Android linker, TLS patching):
  mitigated by pinning and recording the build fingerprint of every device run.
- **Stale fork tips**: wallentx and other forks move hourly; re-check before merging.
