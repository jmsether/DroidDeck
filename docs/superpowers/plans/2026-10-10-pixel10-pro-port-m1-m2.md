# Pixel 10 Pro Port (M1 + M2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Steam Big Picture (M1) and Direct3D 9/10/11 games through Proton's stock DXVK (M2) running on a Pixel 10 Pro (Tensor G5, PowerVR DXT-48-1536, stable Android 17, driver 25.3@6908880) from the `pixel10` branch, with no per-game settings.

**Architecture:** Start from `wallentx/dev` (libhybris loads the phone's PowerVR Vulkan driver into the proot glibc runtime) merged with upstream `main`. Fix the two driver gaps once, in the libhybris compat layer, as patch files CI already applies: fill `VkPhysicalDeviceDrmPropertiesEXT` (gamescope needs `hasRender`) and a `d3d` BC policy that decodes BC6H/BC7 and reports `textureCompressionBC`. Game launches on PowerVR get that policy through the existing game-environment defaults (the `forceSsbs` pattern), so gamescope and the Steam client never see it. Device evidence gates each step.

**Tech Stack:** Kotlin/Java (Android app, JUnit4/Robolectric), C11 (libhybris compat layer, host test harnesses), Python 3 unittest (tools/tests), bash, GitHub Actions, adb.

**Spec:** `docs/superpowers/specs/2026-10-10-pixel10-pro-port-design.md` (Task 3 corrects it with the facts found while planning).

**Out of scope (Plan 2, written after Task 15):** DX12/M3. VKD3D-Proton hard-requires `VK_KHR_push_descriptor`, which the compat layer hides whenever it emulates any BC format, because its compute-state journal cannot replay push descriptors around the BC decoder. M3 needs push-descriptor journaling in `hybris/vulkan/compat/bc_command_state.c`; it is designed from M2 evidence. The driver-25.3 render-pass / clear-attachment workarounds (spec §6.5) are also Plan 2, built only if M2 shows they hit DXVK.

---

## Conventions for every task

- Repository: `E:\projects\PixelDroidDeck`, branch `pixel10`. Shell commands are Git Bash unless marked **PowerShell**; WSL commands run in `Ubuntu-24.04` at `/mnt/e/projects/PixelDroidDeck`.
- After Task 1 the working tree is LF (`core.autocrlf=input`). New files must be LF.
- Commit messages end with the trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Never open a pull request on the fork: `.github/workflows/build.yml` "Check co-authors" fails PRs carrying that trailer. Builds run on pushes to `pixel10` (Task 4 Step 3 explains why not `workflow_dispatch`).
- Narrow checks:
  - JVM (PowerShell): `.\gradlew.bat -PskipRust=true :app:testDebugUnitTest --tests "<class>"`
  - Python/C (WSL): `VULKAN_INCLUDE=$HOME/.cache/droiddeck/vulkan-headers/include python3 -m unittest tools.tests.<module> -v` (the C-backed tests need Vulkan headers; WSL has neither the NDK nor libvulkan-dev, so Task 5 Step 5 keeps pinned headers in `$HOME/.cache/droiddeck/vulkan-headers`)
- Standard checks before every commit that touches code:
  - JVM (PowerShell): about 88 tests fail on Windows on every tree, upstream included (symlink privilege, POSIX permissions, Windows paths); Task 2 recorded them in `%USERPROFILE%\.cache\droiddeck-pixel10\jvm-baseline.txt`. The fork's Linux CI is the all-green gate. Locally, compare against the baseline:
    ```powershell
    $env:JAVA_HOME='C:\Program Files\Eclipse Adoptium\jdk-17.0.20.101-hotspot'; $env:ANDROID_HOME='C:\Android\Sdk'
    $base = Get-Content "$env:USERPROFILE\.cache\droiddeck-pixel10\jvm-baseline.txt"
    .\gradlew.bat -PskipRust=true :app:testDebugUnitTest --continue --console=plain 2>&1 | ForEach-Object { "$_".Trim() } | Where-Object { $_ -match ' FAILED$' -and $_ -notin $base } | Sort-Object -Unique
    ```
    Expected: no output (no failing test outside the baseline). Narrow `--tests` runs of the classes this plan adds or changes must pass outright.
  - Python/C (WSL): the suite is red on this base for environment reasons (no D-Bus session for test_login1/test_netmanager, a flaky signal test in test_appimage_run), so compare against the baseline Task 2 Step 6 records:
    ```bash
    VULKAN_INCLUDE=$HOME/.cache/droiddeck/vulkan-headers/include python3 -m unittest discover -s tools/tests -p 'test_*.py' 2>&1 | grep -E '^(FAIL|ERROR):' | sort -u | comm -13 ~/.cache/droiddeck-pixel10/py-baseline.txt -
    ```
    Expected: no output (no failure outside the baseline). A line that is also flaky on a rerun of the base may be ignored; say so in the commit body.
- Device steps are run by the user; the agent prepares commands and reads the outputs. Record every device run in `docs/pixel10/bringup-log.md` (created in Task 7).
- Large verified file contents are in the Appendices at the end of this plan; tasks name the appendix to copy verbatim.

## File map

| Path | Responsibility | Task |
|---|---|---|
| `.gitattributes` | LF for patches, shell scripts, C/Python tools | 1 |
| `.gitignore` | ignore `pixel10-runs/` device outputs | 6 |
| `tools/hybris/source.env` | pins: libhybris, arlinux-rootfs, Vulkan-Headers | 5 |
| `tools/hybris/prepare_source.sh` (new) | the one fetch + patch mechanism (CI build and host tests) | 5 |
| `tools/hybris/build_runtime.sh` | uses `prepare_source.sh`; ships new files in the provenance patch | 5 |
| `.github/workflows/build.yml` | hybris cache key includes `prepare_source.sh` | 5 |
| `tools/tests/test_hybris_compat.py` (new) | host tests of the libhybris patches | 5, 8, 9 |
| `tools/tests/test_vk_external_image_sync.py` | `vulkan_include_args()` honours `VULKAN_INCLUDE` | 6 |
| `tools/pixel10/*` (new) | phone-side probe and adb CLI | 6 |
| `tools/tests/{test_pvr_props_probe,test_pixel10_build,test_pixel10_cli}.py`, `pvr_props_probe_test.c`, `fake_powervr_hal.c` (new) | tests of the phone-side tooling | 6 |
| `docs/pixel10/bringup-log.md` (new) | evidence trail of every device run | 7 |
| `tools/hybris/patches/0002-drm-properties.patch` (new) | DRM properties fill in the compat layer | 8 |
| `tools/tests/hybris_drm_properties_test.c` (new) | host harness for 0002 | 8 |
| `tools/hybris/patches/0003-bc-d3d-profile.patch` (new) | `d3d` BC policy + honest `textureCompressionBC` | 9 |
| `tools/tests/hybris_bc_rules_check.c`, `hybris_bc_policy_fake.c` (new) | host harnesses for 0003 | 9 |
| `app/.../core/GameEnvironment.kt`, `session/GameEnvironmentStore.kt`, `ui/GameEnvironmentEditor.kt` | PowerVR game defaults | 10 |
| `app/.../gpu/SystemVulkanDriver.java`, `gpu/GpuInfo.kt`, `core/DeviceSupport.kt`, `MainActivity.kt` | one Adreno predicate, no wrong warning on PowerVR | 11 |
| `app/.../core/DeviceReport.kt` | PowerVR section in `device.txt` | 12 |
| `tools/hybris/BC-PROFILE.md` | docs match the new default | 13 |

---

### Task 1: Workstation and repository setup

Everything in this task changes the user's machine. Ask before each install or download and wait for a yes.

**Files:**
- Modify: `.gitattributes`
- Create: `local.properties` (gitignored, not committed)

- [ ] **Step 1: Confirm a clean tree, then switch the clone to LF**

```bash
cd /e/projects/PixelDroidDeck
test -z "$(git status --porcelain --untracked-files=no)" && echo clean
git status --short
git config core.longpaths true
git config core.autocrlf input
# checkout-index --force skips files whose stat matches the index; clearing the index makes
# reset rewrite every tracked file (untracked files such as this plan are untouched).
git rm --cached -r -q . && git reset -q --hard HEAD
git ls-files --eol tools/hybris/build_runtime.sh
```
Expected: `clean`, then exactly `?? docs/superpowers/plans/` (this plan; Task 3 commits it), then `i/lf    w/lf    attr/...    tools/hybris/build_runtime.sh`. If any tracked file is modified, stop and ask the user.

- [ ] **Step 2: Make LF explicit for files bash and git-apply read**

Append to `.gitattributes`:
```
# Read by bash and git apply on Linux and WSL; a CRLF checkout breaks both.
*.sh text eol=lf
*.patch text eol=lf
tools/hybris/** text eol=lf
tools/tests/*.c text eol=lf
tools/tests/*.py text eol=lf
tools/pixel10/** text eol=lf
```
Run: `git add .gitattributes && git status --short`
Expected: `M  .gitattributes` plus the untracked plan folder. Do not run `git add --renormalize .`: it would also convert `gradlew.bat`, `gradle/wrapper/gradle-wrapper.properties` and `tools/msitools/licenses/7zip-License` (CRLF in the index on purpose) and commit them.

- [ ] **Step 3: Install JDK 17 (ask first)**

PowerShell:
```powershell
winget install --id EclipseAdoptium.Temurin.17.JDK -e
$jdk = (Get-ChildItem 'C:\Program Files\Eclipse Adoptium' -Directory -Filter 'jdk-17*' | Select-Object -First 1).FullName
[Environment]::SetEnvironmentVariable('JAVA_HOME', $jdk, 'User')
& "$jdk\bin\java.exe" -version
```
Expected: `openjdk version "17.…"`. If the winget id is not found, run `winget search Temurin.17` and use the JDK id it lists.

- [ ] **Step 4: Install the Android SDK packages (ask first; the license prompt is the user's)**

Download (ask first) the "Command line tools only" zip for Windows from https://developer.android.com/studio#command-tools into a new empty folder, then PowerShell:
```powershell
New-Item -ItemType Directory -Force C:\Android\Sdk\cmdline-tools | Out-Null
Expand-Archive <downloaded-zip> -DestinationPath C:\Android\Sdk\cmdline-tools\tmp
Move-Item C:\Android\Sdk\cmdline-tools\tmp\cmdline-tools C:\Android\Sdk\cmdline-tools\latest
Remove-Item C:\Android\Sdk\cmdline-tools\tmp
[Environment]::SetEnvironmentVariable('ANDROID_HOME', 'C:\Android\Sdk', 'User')
```
The user runs, in a new PowerShell window: `C:\Android\Sdk\cmdline-tools\latest\bin\sdkmanager.bat --licenses` and accepts the licenses themselves. Then:
```powershell
C:\Android\Sdk\cmdline-tools\latest\bin\sdkmanager.bat "platforms;android-34" "build-tools;34.0.0" "ndk;27.3.13750724" "platform-tools"
```
Expected: `done` for each package.

- [ ] **Step 5: Point Gradle at the SDK**

Create `local.properties` (already in `.gitignore`):
```
sdk.dir=C\:\\Android\\Sdk
```

- [ ] **Step 6: Linux compiler in WSL (ask first)**

PowerShell: `wsl -d Ubuntu-24.04 -u root -- apt-get install -y build-essential zstd unzip` (`unzip` is used by test_steam_install)
Expected: `gcc --version` in that distro prints gcc 13.

- [ ] **Step 7: Verify both test runners on the untouched base**

PowerShell (new window so the env vars apply):
```powershell
cd E:\projects\PixelDroidDeck
.\gradlew.bat -PskipRust=true :app:testDebugUnitTest --tests "com.droiddeck.launcher.gpu.SystemVulkanSelectionTest"
```
Expected: `BUILD SUCCESSFUL`.

WSL:
```bash
cd /mnt/e/projects/PixelDroidDeck && python3 -m unittest tools.tests.test_drm_handles -v
```
Expected: `OK`.

- [ ] **Step 8: Commit**

```bash
git add .gitattributes
git commit -m "Keep tool scripts and patches LF in every checkout

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Refresh the base and merge upstream

`wallentx/dev` and upstream `main` move daily. The dry run on 2026-10-10 (wallentx `8016a145`, upstream `0e225d0c`) produced exactly the three conflicts below; if newer tips produce others, stop and ask.

**Files:** merge commit; conflicts in `docs/progress-log.md`, `app/src/main/java/com/droiddeck/launcher/core/LogRedactor.kt`, `tools/release/runtime_inputs.py`.

- [ ] **Step 1: Fetch and rebuild `pixel10` on the newest wallentx tip**

```bash
git fetch wallentx && git fetch origin
git log --format='%h %s' wallentx/dev..pixel10
```
Expected: exactly two commits, the design spec and Task 1's `.gitattributes` commit (the old base `abe58618` is an ancestor of the new `wallentx/dev`). Carry them onto the new tip:
```bash
CARRY=$(git rev-list --reverse wallentx/dev..pixel10)
git switch -c pixel10-next wallentx/dev
git cherry-pick $CARRY
git branch -m pixel10 pixel10-old && git branch -m pixel10-next pixel10
git log --oneline -3
```
Expected: the two carried commits on top of the newest `wallentx/dev` commit.

- [ ] **Step 2: Merge upstream**

```bash
git merge --no-ff origin/main -m "Merge upstream main into pixel10"
git status --short | grep -E '^(UU|AA)'
```
Expected:
```
UU app/src/main/java/com/droiddeck/launcher/core/LogRedactor.kt
UU docs/progress-log.md
UU tools/release/runtime_inputs.py
```

- [ ] **Step 3: Resolve `LogRedactor.kt` with wallentx's side**

Both sides rewrote `redact()` internally for speed/memory (upstream: per-rule `Learned` prefilters; wallentx: one combined regex plus a bounded native-matcher cache and `MAX_LINE_LENGTH`). The public API (`learnAccounts`, `learnFromRuntime`, `learnOwnAddresses`, `describeAddresses`, `isText`, `RULES_VERSION = 3`, `scrubTo`, `redactForShare`, `scrubForShare`, `redact`) is identical and upstream changed no tests. Keep one coherent implementation:
```bash
git checkout --ours app/src/main/java/com/droiddeck/launcher/core/LogRedactor.kt
```

- [ ] **Step 4: Resolve `runtime_inputs.py` as the union**

```bash
python3 - <<'EOF'
import re
p = 'tools/release/runtime_inputs.py'
t = open(p).read()
t = re.sub(r"<<<<<<< HEAD\n.*?=======\n.*?>>>>>>> [^\n]*\n",
           "    for tree in ['linuxfs', 'directaudio', 'droiddeck-esync', 'graphics-profile', 'desktop-removal']:\n",
           t, flags=re.S)
open(p, 'w').write(t)
EOF
grep -n "for tree in" tools/release/runtime_inputs.py
```
Expected: `    for tree in ['linuxfs', 'directaudio', 'droiddeck-esync', 'graphics-profile', 'desktop-removal']:`

- [ ] **Step 5: Resolve `docs/progress-log.md` keeping both sides, upstream first**

```bash
awk '/^<<<<<<< /{s=1;H="";U="";next} /^=======$/{if(s==1){s=2;next}} /^>>>>>>> /{if(s==2){printf "%s\n%s", U, H; s=0; next}} s==1{H=H $0 "\n";next} s==2{U=U $0 "\n";next} {print}' docs/progress-log.md > /tmp/pl.md && mv /tmp/pl.md docs/progress-log.md
grep -c '^<<<<<<<\|^=======$\|^>>>>>>>' docs/progress-log.md app/src/main/java/com/droiddeck/launcher/core/LogRedactor.kt tools/release/runtime_inputs.py
```
Expected: `0` for all three files.

- [ ] **Step 6: Run the standard checks on the merge**

Record the Python baseline once, on the merge, before any plan change (WSL; the Vulkan-header tests join the passing set after Task 6):
```bash
cd /mnt/e/projects/PixelDroidDeck && mkdir -p ~/.cache/droiddeck-pixel10 && python3 -m unittest discover -s tools/tests -p 'test_*.py' 2>&1 | grep -E '^(FAIL|ERROR):' | sort -u > ~/.cache/droiddeck-pixel10/py-baseline.txt; wc -l < ~/.cache/droiddeck-pixel10/py-baseline.txt
```
Then run the JVM standard check. Expected: JVM `BUILD SUCCESSFUL`; the Python baseline lists only environment failures (D-Bus, network, Vulkan headers). A compile error in `OnScreenControls.kt` or `SessionActivity.kt` means the auto-merge broke; fix it by combining both sides' added parameters and calls, then rerun.

- [ ] **Step 7: Commit the merge and drop the old branch**

```bash
git add app/src/main/java/com/droiddeck/launcher/core/LogRedactor.kt docs/progress-log.md tools/release/runtime_inputs.py
git commit --no-edit --cleanup=strip --trailer "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git branch -D pixel10-old
git log --oneline -3
```

---

### Task 3: Correct the design spec

**Files:** Modify `docs/superpowers/specs/2026-10-10-pixel10-pro-port-design.md`

- [ ] **Step 1: Apply these replacements**

The spec wraps long lines; match each target with whitespace normalised (item 1's text spans a line break).

1. In §1 Goal, replace "Windows DX9/10/11 games through DXVK, and DX12 games through VKD3D-Proton" with "Windows DX9/10/11 games through DXVK (M2), then DX12 games through VKD3D-Proton (M3, Plan 2)".
2. In §3 table, replace D6's reason with: "Personal fork. Builds use `workflow_dispatch`; never open PRs on the fork, because the workflow's PR-only co-author check rejects the trailer."
3. In §4, replace the bullet that starts "Component releases (gamescope, hybris runtime inputs) publish…" with: "CI downloads gamescope p12 and DirectAudio from `wallentx/DroidDeck` releases (`GAMESCOPE_REPO` in `tools/gamescope/release.env` overrides `BUILD_ASSETS_REPOSITORY`); both are public. Mirroring them to the fork is optional resilience, not required."
4. Add to §2 after the prior-art table: "`wallentx/dev` `b85da722` (2026-10-10) added an optional launcher choice, Standard or Experimental graphics. Experimental installs a patched DXVK and `HYBRIS_BC_TEXTURES=dxvk` for game launches. This plan makes Standard work: stock DXVK accepts the GPU once the compat layer reports `textureCompressionBC`."
5. Replace §6.2's first bullet with: "Add a `d3d` policy (`HYBRIS_BC_TEXTURES=d3d`): Vulkan 1.4 devices allowed and presented as 1.3, as wallentx's `dxvk` policy does; formats natively sampleable with all of SAMPLED, FILTER_LINEAR, BLIT_SRC, TRANSFER_SRC, TRANSFER_DST stay native; the rest of the 14 D3D formats (BC1 RGBA through BC7; BC1 RGB excluded for its border-alpha defect) are decoded."
6. Replace §6.3 with: "### 6.3 PowerVR game defaults\n\nRail: `GameEnvironment.defaults(preset, forceSsbs, powerVr)`, published by `GameEnvironmentStore.publish` into the guest's `game-environment.json` and applied by `droiddeck-game-env` to game launches only. On PowerVR: `HYBRIS_BC_TEXTURES=d3d`, `DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1`, `ENABLE_VK_LAYER_VALVE_steam_fossilize_1` unset. Gamescope, Xwayland and the Steam client never see the BC policy, whose 1.3 cap and hidden extensions would otherwise reach them. A game's own entries, the shared scope, and the Experimental profile all override these."
7. Replace §6.4 with: "### 6.4 DX12 (Plan 2)\n\nVKD3D-Proton requires `VK_KHR_push_descriptor`, `maintenance5` and `maintenance6`. The compat layer hides `VK_KHR_push_descriptor` whenever it emulates a BC format, because `hybris_bc_restore_compute` cannot replay push descriptors around the decoder dispatch. M3 therefore needs push-descriptor journaling for the compute bind point. It is designed in Plan 2 from M2 evidence."
8. In §7, replace the "Escape hatch" bullet with: "**Escape hatch**: game defaults are changed in the Game environment editor (shared scope or one game). Session-wide switches such as `HYBRIS_VULKAN_COMPAT_DRM_PROPERTIES=0` go in `Android/data/com.droiddeck.launcher/files/droiddeck-env` (Android 11+; `Download/droiddeck-env` is ignored)."
9. In §5 Phase 0, replace "Developer option \"Restrict child processes\" off." with "Turn on the developer option that disables child-process restrictions (DroidDeck's Setup explains it)."
10. In §5 Phase 1, replace "wallentx's libhybris offscreen-triangle probe (`tools/hybris/build_render_probe.py`; pass = 1152 red, 2944 blue, 0 other pixels) plus a property probe recording:" with "a bionic property probe (`tools/pixel10/pvr_props_probe.c`) recording:", and after "and BC format features." add "wallentx's offscreen-triangle probe (`tools/hybris/build_render_probe.py`) needs a glibc toolchain and an isolated runtime on the phone, so Phase 2's Steam session stands in for it."
11. §6.2 heading "(M2, M3)" becomes "(M2)"; replace "Proton's stock DXVK and VKD3D-Proton accept the device." with "Proton's stock DXVK accepts the device (M2); VKD3D-Proton also needs push descriptors (§6.4)."
12. §7: replace "gains a PowerVR section: driver string, HAL version, BC mode, active workarounds." with "gains a PowerVR section: system driver, Vulkan device, driver string, the driver's own BC feature and the game defaults in effect; the compat layer's `HYBRIS_DRM_PROPERTIES` and `HYBRIS_BC` lines stay in the logs."
13. §4: replace "`pixel10` starts at `abe58618`. First change: merge `origin/main`; resolve `docs/progress-log.md`;" with "`pixel10` starts at the newest `wallentx/dev` tip (`8016a145` on 2026-10-10). First change: merge `origin/main`, resolving `LogRedactor.kt` (wallentx's side), `tools/release/runtime_inputs.py` (union) and `docs/progress-log.md` (both);".

- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/specs/2026-10-10-pixel10-pro-port-design.md docs/superpowers/plans/2026-10-10-pixel10-pro-port-m1-m2.md
git commit -m "docs: correct the Pixel 10 design with planning findings; add the M1/M2 plan

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Fork and baseline CI build

Every step here acts on the user's GitHub account or downloads a file. Ask before Step 1 (create the fork), Step 2 (push), Step 3 (disable workflows), Step 4 (start a CI run) and Step 5 (download `DroidDeck-*.apk`, about 46 MB, from the `jmsether/DroidDeck` run artifacts).

- [ ] **Step 1: Create the fork without touching local remotes**

```bash
gh repo fork Droid-Deck/DroidDeck --clone=false --default-branch-only
git remote add fork https://github.com/jmsether/DroidDeck.git
```
Expected: `jmsether/DroidDeck` exists (`gh repo view jmsether/DroidDeck --json name`).

- [ ] **Step 2: Push before Actions is enabled**

```bash
git push -u fork pixel10
```
Pushing first keeps the path-filtered side workflows (gamescope, desktop-kde, box64, game-graphics, aaudio-sink) from firing on this push.

- [ ] **Step 3: Disable the side workflows**

On 2026-10-10 the new fork already had Actions enabled, so the Step 2 push ran the path-filtered side workflows (cancel any still running: `gh run list -R jmsether/DroidDeck` then `gh run cancel <id> -R jmsether/DroidDeck`). `build.yml` was not registered: the fork's default branch never receives a push, so `workflow_dispatch` returns 404. The fix, committed on `pixel10` as "ci: build pixel10 pushes on the fork", adds `pixel10` to `build.yml`'s `push.branches`; every non-docs push to `pixel10` then builds an APK. Disable the side workflows:
```bash
for w in build-aaudio-sink build-box64 build-desktop-kde build-directaudio build-game-graphics build-gamescope; do
  gh workflow disable "$w.yml" -R jmsether/DroidDeck || echo "$w.yml is not on the fork's default branch; nothing to disable"
done
```
Expected: build-aaudio-sink, build-desktop-kde and build-gamescope disabled; the other three exist only on `pixel10` and need no action.

- [ ] **Step 4: Push and wait for the build**

Push `pixel10` (`git push fork pixel10`); the push starts Build APK. Find that run and wait:

```bash
SHA=$(git rev-parse HEAD); RUN=0
for i in $(seq 1 24); do
  RUN=$(gh run list -R jmsether/DroidDeck -w build.yml -b pixel10 -e push -L 5 --json databaseId,headSha \
        -q "[.[] | select(.headSha==\"$SHA\")][0].databaseId // 0")
  [ "$RUN" != "0" ] && break
  sleep 5
done
[ "$RUN" != "0" ] || { echo "no Build APK run for $SHA after 2 minutes" >&2; exit 1; }
gh run watch "$RUN" -R jmsether/DroidDeck --exit-status --compact
```
Expected: `build` succeeds; `sign`, `announce` and `catalog` are skipped. The first run has no caches (estimate 12–20 min). If it fails, read `gh run view "$RUN" -R jmsether/DroidDeck --log-failed`, fix the cause on `pixel10`, push, and dispatch again.

- [ ] **Step 5: Download the APK**

```bash
mkdir -p pixel10-runs/apk && gh run download "$RUN" -R jmsether/DroidDeck -D pixel10-runs/apk || {
  AID=$(gh api "repos/jmsether/DroidDeck/actions/runs/$RUN/artifacts" --jq '.artifacts[0].id')
  curl -L -H "Authorization: Bearer $(gh auth token)" -o pixel10-runs/apk/DroidDeck.apk \
    "https://api.github.com/repos/jmsether/DroidDeck/actions/artifacts/$AID/zip"; }
find pixel10-runs/apk -type f -name '*.apk' -exec ls -l {} +
```
Expected: one `*.apk` of about 46 MB, possibly inside a sub-folder named for the artifact. This is the **baseline APK** for Task 7; use the path `find` prints wherever this plan says `<apk>`.

---

### Task 5: One libhybris source mechanism and the host test harness

The CI runtime build and the host tests must patch the same pinned source the same way. Extract the fetch-and-patch logic from `build_runtime.sh` into one script, and start the host test module with a characterization test of today's behaviour.

**Files:**
- Modify: `tools/hybris/source.env`
- Create: `tools/hybris/prepare_source.sh`
- Modify: `tools/hybris/build_runtime.sh`
- Modify: `.github/workflows/build.yml` (hybris cache key)
- Create: `tools/tests/test_hybris_compat.py`

- [ ] **Step 1: Pin Vulkan-Headers**

Append to `tools/hybris/source.env` (tag v1.4.317, the driver's API version; Ubuntu 24.04's libvulkan-dev 1.3.275 is too old for `VkPhysicalDeviceVulkan14Features`):
```
VULKAN_HEADERS_REVISION=2642d51e1e9230720a74d8c76bc7b301e69881bf
```

- [ ] **Step 2: Create `tools/hybris/prepare_source.sh`**

```bash
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
```
Run: `chmod +x tools/hybris/prepare_source.sh && git add --chmod=+x tools/hybris/prepare_source.sh`

- [ ] **Step 3: Make `build_runtime.sh` use it**

In `tools/hybris/build_runtime.sh`, replace this block:
```bash
fetch() {
  git init -q "$3"
  git -C "$3" fetch --depth=1 "$1" "$2"
  git -C "$3" checkout -q --detach FETCH_HEAD
  test "$(git -C "$3" rev-parse HEAD)" = "$2"
}
fetch https://github.com/taowen/libhybris.git "$HYBRIS_REVISION" "$work/libhybris"
fetch https://github.com/taowen/arlinux-rootfs.git "$PROTOCOL_REVISION" "$work/rootfs"
git -C "$work/libhybris" apply --check "$root/tools/hybris/powervr-hal-version.patch"
git -C "$work/libhybris" apply "$root/tools/hybris/powervr-hal-version.patch"
for patch in "$root"/tools/hybris/patches/*.patch; do
  git -C "$work/libhybris" apply --check "$patch"
  git -C "$work/libhybris" apply "$patch"
done
```
with:
```bash
"$root/tools/hybris/prepare_source.sh" libhybris "$work/libhybris"
"$root/tools/hybris/prepare_source.sh" rootfs "$work/rootfs"
```
Leave the `. "$root/tools/hybris/source.env"` line: `$HYBRIS_REVISION` is still used later in the script.

- [ ] **Step 4: Add the helper to the CI cache key**

In `.github/workflows/build.yml`, replace:
```
key: hybris-runtime-v1-${{ hashFiles('tools/hybris/source.env', 'tools/hybris/build_runtime.sh', 'tools/hybris/powervr-hal-version.patch', 'tools/hybris/patches/*.patch') }}
```
with:
```
key: hybris-runtime-v1-${{ hashFiles('tools/hybris/source.env', 'tools/hybris/build_runtime.sh', 'tools/hybris/prepare_source.sh', 'tools/hybris/powervr-hal-version.patch', 'tools/hybris/patches/*.patch') }}
```

- [ ] **Step 5: Verify the helper reproduces today's tree**

Ask first: this fetches the pinned public sources taowen/libhybris and KhronosGroup/Vulkan-Headers from github.com, and `test_hybris_compat` repeats the libhybris fetch on every run. The headers go under `~/.cache` because WSL wipes `/tmp` when the distro restarts. WSL:
```bash
cd /mnt/e/projects/PixelDroidDeck && rm -rf /tmp/h && bash tools/hybris/prepare_source.sh libhybris /tmp/h \
  && git -C /tmp/h diff --stat | tail -1 \
  && { test -d ~/.cache/droiddeck/vulkan-headers || bash tools/hybris/prepare_source.sh headers ~/.cache/droiddeck/vulkan-headers; } \
  && grep 'define VK_HEADER_VERSION ' ~/.cache/droiddeck/vulkan-headers/include/vulkan/vulkan_core.h
```
Expected: ` 9 files changed, 191 insertions(+), 31 deletions(-)` (the two existing patches) and `#define VK_HEADER_VERSION 317`.

- [ ] **Step 6: Write the host test module with a characterization test**

Create `tools/tests/test_hybris_compat.py`:
```python
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
```

- [ ] **Step 7: Run it**

WSL: `cd /mnt/e/projects/PixelDroidDeck && python3 -m unittest tools.tests.test_hybris_compat -v`
Expected: `test_patched_tree_compiles_the_bc_policy ... ok`, `OK`.

- [ ] **Step 8: Standard checks, then commit**

```bash
git add tools/hybris/source.env tools/hybris/prepare_source.sh tools/hybris/build_runtime.sh .github/workflows/build.yml tools/tests/test_hybris_compat.py
git commit -m "hybris: one fetch-and-patch script for the runtime build and host tests

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Phone-side tooling (`tools/pixel10`)

A bionic property probe that asks the phone's own driver (through the Android loader and directly through the HAL) the questions the bring-up depends on, plus one CLI for facts, probe, logs and install. It only reads the device, except `probe` (pushes one file to `/data/local/tmp/pixel10`) and `install` (replaces the app in place; never uninstalls).

**Files:**
- Modify: `tools/tests/test_vk_external_image_sync.py` (`vulkan_include_args`)
- Create: `tools/pixel10/pvr_props_probe.c` (Appendix F), `tools/pixel10/hwvulkan_abi.h` (Appendix G), `tools/pixel10/build_props_probe.py` (Appendix H), `tools/pixel10/pixel10.py` (Appendix I)
- Create: `tools/tests/pvr_props_probe_test.c`, `tools/tests/fake_powervr_hal.c`, `tools/tests/test_pvr_props_probe.py`, `tools/tests/test_pixel10_build.py`, `tools/tests/test_pixel10_cli.py` (Appendix J)
- Modify: `.gitignore`

- [ ] **Step 1: Let `VULKAN_INCLUDE` name the headers for host C tests**

In `tools/tests/test_vk_external_image_sync.py`, replace the first two lines of `vulkan_include_args()`:
```python
def vulkan_include_args():
    candidates = []
```
with:
```python
def vulkan_include_args():
    # An explicit Vulkan-Headers include directory wins (WSL and hosts without the NDK).
    if os.environ.get("VULKAN_INCLUDE") and (Path(os.environ["VULKAN_INCLUDE"]) / "vulkan/vulkan.h").is_file():
        return ["-idirafter", os.environ["VULKAN_INCLUDE"]]
    candidates = []
```

- [ ] **Step 2: Add the tests first (Appendix J), run them, see them fail**

Copy the five files of Appendix J verbatim into `tools/tests/`.
WSL: `VULKAN_INCLUDE=$HOME/.cache/droiddeck/vulkan-headers/include python3 -m unittest tools.tests.test_pvr_props_probe tools.tests.test_pixel10_build tools.tests.test_pixel10_cli -v`
Expected: the run fails. test_pixel10_build and test_pixel10_cli stop at import with `FileNotFoundError: ... tools/pixel10/build_props_probe.py` (and `.../pixel10.py`), so no test runs; test_pvr_props_probe on its own reports `fatal error: .../tools/pixel10/pvr_props_probe.c: No such file or directory`.

- [ ] **Step 3: Add the tool (Appendices F–I)**

Copy Appendices F, G, H and I verbatim to `tools/pixel10/`. Then:
```bash
git add --chmod=+x tools/pixel10/pixel10.py tools/pixel10/build_props_probe.py
```

- [ ] **Step 4: Run the tests again**

WSL: `VULKAN_INCLUDE=$HOME/.cache/droiddeck/vulkan-headers/include python3 -m unittest tools.tests.test_pvr_props_probe tools.tests.test_pixel10_build tools.tests.test_pixel10_cli -v`
Expected: `Ran 30 tests`, `OK`.

- [ ] **Step 5: Build the real probe with the NDK**

PowerShell: `python tools\pixel10\build_props_probe.py pixel10-runs\probe`
Expected: `Built …\pixel10-runs\probe\pvr_props_probe`.

- [ ] **Step 6: Ignore device outputs**

Append to `.gitignore`:
```
# Pixel 10 bring-up outputs (tools/pixel10); results worth keeping go in docs/pixel10/bringup-log.md
pixel10-runs/
```

- [ ] **Step 7: Standard checks, then commit**

```bash
git add .gitignore tools/pixel10 tools/tests/test_vk_external_image_sync.py tools/tests/pvr_props_probe_test.c tools/tests/fake_powervr_hal.c tools/tests/test_pvr_props_probe.py tools/tests/test_pixel10_build.py tools/tests/test_pixel10_cli.py
git commit -m "pixel10: device facts, PowerVR property probe and adb CLI for bring-up

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Device baseline (phases 0–2) and the bring-up log

The user runs every device command. Make sure the phone has no unsaved work open (a GPU lockup can force a reboot).

**Files:** Create `docs/pixel10/bringup-log.md`

- [ ] **Step 1: Create the log**

```markdown
# Pixel 10 Pro bring-up log

Newest entry first. Every entry: date, build (commit + APK name), device build fingerprint, driver
string, what ran, result, and where the raw output lives (pixel10-runs/... on the dev PC).
Facts here are measured on the device; anything inferred says so.

---
```

- [ ] **Step 2: Phase 0, device facts (user)**

The user connects the Pixel 10 Pro with USB debugging on and accepts the PC's key. PowerShell:
```powershell
$env:ADB = 'C:\ADB\adb.exe'   # or C:\Android\Sdk\platform-tools\adb.exe
python tools\pixel10\pixel10.py facts
```
`$env:ADB` lasts for one PowerShell window; set it again in every new window (or rely on `ANDROID_HOME\platform-tools`, which the tool also searches). Expected: `facts written to pixel10-runs\<stamp>\facts\facts.txt`. Read it and record in the log:
- `ro.build.fingerprint`, `ro.build.version.sdk`;
- whether `/vendor/lib64/hw/vulkan.powervr.so` exists (the app's PowerVR detection depends on this exact path);
- the `/dev/dri` listing and `renderD128` major:minor (expected 226:128);
- `PAGE_SIZE` (expected 4096) and the CPU `Features` line.

**Gate:** if `vulkan.powervr.so` is not at that path, stop and report; `SystemVulkanDriver.HAL` must change before anything else.

- [ ] **Step 3: Phase 1, property probe (user)**

```powershell
python tools\pixel10\pixel10.py probe pixel10-runs\probe\pvr_props_probe
```
Expected: `probe output written to pixel10-runs\<stamp>\probe\props.txt`. Record in the log:
- loader and HAL `driverInfo` (expected to contain `25.3`), `apiVersion` (1.4.317);
- HAL `common.version` (the carried hybris patch accepts `0x00010000` and `0x00000001`);
- the DRM properties result: `untouched`, `zeroed`, or `filled`;
- `VK_ANDROID_native_buffer` spec version (the ICD needs >= 8) and the `hal.native_buffer.gralloc_fns` line (`v2=1` means `vkGetSwapchainGrallocUsage2ANDROID` is exported);
- every `hal.native_buffer.usage.*` line (`exceeds_32_bits`; the ICD rejects usage above 32 bits);
- `textureCompressionBC`, and the optimal features of BC1–BC7 and RGBA8/BGRA8;
- the `hal.queue.family` lines (flags, count, granularity). BC emulation is refused if any graphics/compute/transfer family lacks compute (flags bit 0x2) or has a granularity other than 1x1x1.

**Gates:**
- DRM `filled` or `not_advertised`: Task 8 is not needed for M1 (gamescope never reads the structure when the extension is not listed); still build it (a no-op) and note it.
- DRM `zeroed`: do Task 8 Step 6.
- DRM `partial`: stop and report the `hal.drm.fields` line. Task 8's sentinel protocol counts any changed byte as the driver's answer and would not fill; the likely fix is the same patch with the predicate `!drm->hasPrimary && !drm->hasRender` instead of all-zero.
- HAL version outside the two accepted values, native_buffer spec < 8, no `v2=1` in `hal.native_buffer.gralloc_fns`, or any `exceeds_32_bits=1`: stop; libhybris needs a fix before any session can draw. Report to the user with the probe lines.

- [ ] **Step 4: Phase 2, baseline APK (user)**

If the official DroidDeck is installed, the user decides whether to uninstall it (this deletes its runtime and data); the fork APK cannot install over it. Then:
```powershell
python tools\pixel10\pixel10.py install <apk path printed by find in Task 4 Step 5>
```
The user opens DroidDeck, turns on the developer option Setup asks for, installs the Linux runtime, chooses **Standard** graphics if asked, presses **Play**, waits up to two minutes, then opens the session drawer, taps **Share logs**, and saves the zip to Downloads. Then:
```powershell
python tools\pixel10\pixel10.py logs
```
The tool pulls the newest `DroidDeck-<session>.zip` from Download into `pixel10-runs\<stamp>\logs\`; the CI build is not debuggable, so this is the only route. Unpack it:
```powershell
Expand-Archive -LiteralPath <that zip> -DestinationPath pixel10-runs\<stamp>\logs\unpacked
```
The session folder inside holds `device.txt`, `session.log` (proot, gamescope and the client's stdout), `wayland.log` and `steam.log`. Record: how far the session got (black screen, gamescope exit, Steam UI), and the first error lines of `session.log`. Expected (prediction): gamescope exits with "physical device has no render node" when the probe said `untouched`/`zeroed`.

- [ ] **Step 5: Commit the log**

```bash
git add docs/pixel10/bringup-log.md
git commit -m "docs: Pixel 10 Pro baseline facts, probe and first session

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: DRM properties fill (`0002-drm-properties.patch`)

New compat module `hybris/vulkan/compat/drm_properties.{c,h}` and an outermost `vkGetPhysicalDeviceProperties2[KHR]` handler in `layer/physical.c`. Protocol: paint the chained `VkPhysicalDeviceDrmPropertiesEXT` payload with a sentinel, call down, and if the driver left it untouched, fill `hasRender`/`renderMajor`/`renderMinor` from `stat("/dev/dri/renderD128")` (`hasPrimary` stays false: PowerVR sessions use gamescope's SDL backend). No node: restore the application's bytes. Driver answered: pass through. One stderr line per outcome per process (`HYBRIS_DRM_PROPERTIES outcome=…`). `HYBRIS_VULKAN_COMPAT_DRM_PROPERTIES=0` disables it.

**Files:**
- Create: `tools/tests/hybris_drm_properties_test.c` (Appendix C)
- Modify: `tools/tests/test_hybris_compat.py`
- Create: `tools/hybris/patches/0002-drm-properties.patch` (Appendix A)

- [ ] **Step 1: Add the harness (Appendix C) and its test method**

Copy Appendix C to `tools/tests/hybris_drm_properties_test.c`. Add to `HybrisCompatTest`:
```python
    def test_drm_properties_fill(self):
        program = self.build("drm-properties", [TESTS / "hybris_drm_properties_test.c",
                                                self.vulkan / "compat/drm_properties.c"])
        self.run_program(program)
        self.run_program(program, HYBRIS_VULKAN_COMPAT_DRM_PROPERTIES="0", EXPECT_DISABLED="1")
```

- [ ] **Step 2: Run it and see it fail**

WSL: `python3 -m unittest tools.tests.test_hybris_compat.HybrisCompatTest.test_drm_properties_fill -v`
Expected: FAIL, compiler error `compat/drm_properties.h: No such file or directory`.

- [ ] **Step 3: Add the patch (Appendix A)**

Copy Appendix A verbatim to `tools/hybris/patches/0002-drm-properties.patch`. Check that it is LF: `tr -cd '\r' < tools/hybris/patches/0002-drm-properties.patch | wc -c` prints `0` (`grep -c $'\r'` cannot see CRs in Git Bash).

- [ ] **Step 4: Run it again**

WSL: `python3 -m unittest tools.tests.test_hybris_compat -v`
Expected: both tests `ok` (the test captures the harness output, `drm_properties: all checks passed` for each of its two runs; it is shown only when a test fails).

- [ ] **Step 5: Standard checks, then commit**

```bash
git add tools/hybris/patches/0002-drm-properties.patch tools/tests/hybris_drm_properties_test.c tools/tests/test_hybris_compat.py
git commit -m "hybris: fill DRM properties the PowerVR driver leaves empty

gamescope requires hasRender when VK_EXT_physical_device_drm is listed.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 6 (only if Task 7 recorded `zeroed`): treat an all-zero answer as ignored**

A driver that zero-fills the structure while advertising the extension names no node, and a driver that has a node always sets at least one field, so an all-zero payload counts as 'ignored' exactly like the untouched sentinel. Appendix C's case 3 (all zeros -> pass through) is replaced; the 'real answer without a render node' case moves to a new mode 3 (primary node only). Edit 0002 in place from the repo root (Git Bash); the script recounts the new-file hunk header, which `git apply` trusts:

```bash
python3 - <<'EOF'
import pathlib, re

def sub(text, old, new):
    assert text.count(old) == 1, "anchor not found exactly once: " + old[:60]
    return text.replace(old, new)

# 1. 0002: drm_properties.c treats a zero-filled payload as "driver ignored it".
p = pathlib.Path("tools/hybris/patches/0002-drm-properties.patch")
t = p.read_bytes().decode().replace("\r\n", "\n")
t = sub(t, "+ * survive, the driver's answer passes through unchanged. With no node the\n",
           "+ * survive, the driver's answer passes through unchanged; a payload the driver\n"
           "+ * zero-filled counts as ignored too (it names no node). With no node the\n")
t = sub(t, "+enum hybris_drm_outcome hybris_drm_query(PFN_vkGetPhysicalDeviceProperties2 query,\n"
           "+    VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out, hybris_drm_node_fn render_node)\n",
           "+/* The driver gave no answer: the payload is still the sentinel, or all zero. */\n"
           "+static int payload_unanswered(const unsigned char *payload, const unsigned char *painted)\n"
           "+{\n"
           "+    static const unsigned char zero[PAYLOAD_BYTES];\n"
           "+    return !memcmp(payload, painted, PAYLOAD_BYTES) || !memcmp(payload, zero, PAYLOAD_BYTES);\n"
           "+}\n"
           "+\n"
           "+enum hybris_drm_outcome hybris_drm_query(PFN_vkGetPhysicalDeviceProperties2 query,\n"
           "+    VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out, hybris_drm_node_fn render_node)\n")
t = sub(t, "+    if (!memcmp(payload, (unsigned char *)&painted + PAYLOAD, PAYLOAD_BYTES)) {\n",
           "+    if (payload_unanswered(payload, (unsigned char *)&painted + PAYLOAD)) {\n")
# git apply trusts the hunk header: recount the new-file hunk of drm_properties.c.
m = re.search(r"(diff --git a/hybris/vulkan/compat/drm_properties\.c .*?\n@@ -0,0 \+1,)(\d+)( @@\n)(.*?)(?=^diff --git )", t, re.S | re.M)
t = t[:m.start(2)] + str(m.group(4).count("\n")) + t[m.end(2):]
p.write_bytes(t.encode())

# 2. Harness: case 3 (all zeros) now fills; the "real answer, no render node" case becomes mode 3.
p = pathlib.Path("tools/tests/hybris_drm_properties_test.c")
t = p.read_text()
t = sub(t, '/* Fake driver: mode 0 ignores the DRM structure, 1 answers hasRender, 2 answers "no render node". */',
           '/* Fake driver: mode 0 ignores the DRM structure, 1 answers hasRender, 2 zero-fills it, 3 answers "primary node only". */')
t = sub(t, '    else { d->hasPrimary = d->hasRender = VK_FALSE; d->primaryMajor = d->primaryMinor = d->renderMajor = d->renderMinor = 0; }\n',
           '    else if (hal_mode == 3) { d->hasPrimary = VK_TRUE; d->hasRender = VK_FALSE; d->primaryMajor = 226; d->primaryMinor = 0;\n'
           '                              d->renderMajor = d->renderMinor = 0; }\n'
           '    else { d->hasPrimary = d->hasRender = VK_FALSE; d->primaryMajor = d->primaryMinor = d->renderMajor = d->renderMinor = 0; }\n')
t = sub(t, '''    /* 3. Driver explicitly answers "no render": still the driver's answer (design: pass through). */
    drm = fresh(); hal_mode = 2;
    assert(hybris_drm_query(fake_hal, pd, &out, node_226_128) == HYBRIS_DRM_DRIVER_ANSWERED);
    assert(drm.hasRender == VK_FALSE && drm.renderMajor == 0);
    assert(node_calls == 0);

    /* 4. Driver ignores it and there is no node: the application's own bytes come back. */
    drm = fresh(); hal_mode = 0;
    drm.hasRender = 0; drm.renderMajor = 42;                      /* application pre-initialised */
    VkPhysicalDeviceDrmPropertiesEXT before = drm;
    assert(hybris_drm_query(fake_hal, pd, &out, node_missing) == HYBRIS_DRM_NO_NODE);
    assert(!memcmp(&before, &drm, sizeof drm));
    assert(node_calls == 1);
''', '''    /* 3. Driver zero-fills the structure: that names no node, so it counts as ignored and the node fills it. */
    drm = fresh(); hal_mode = 2; node_calls = 0;
    assert(hybris_drm_query(fake_hal, pd, &out, node_226_128) == HYBRIS_DRM_FILLED);
    assert(drm.hasRender == VK_TRUE && drm.renderMajor == 226 && drm.renderMinor == 128 && node_calls == 1);

    /* 3b. Driver answers "primary node, no render node": a real answer, passed through, node never stat'ed. */
    drm = fresh(); hal_mode = 3; node_calls = 0;
    assert(hybris_drm_query(fake_hal, pd, &out, node_226_128) == HYBRIS_DRM_DRIVER_ANSWERED);
    assert(drm.hasPrimary == VK_TRUE && drm.primaryMajor == 226 && drm.hasRender == VK_FALSE && drm.renderMajor == 0);
    assert(node_calls == 0);

    /* 4. Driver ignores it (mode 0) or zero-fills it (mode 2) and there is no node: the application's own bytes come back. */
    for (hal_mode = 0; hal_mode <= 2; hal_mode += 2) {
        drm = fresh(); node_calls = 0;
        drm.hasRender = 0; drm.renderMajor = 42;                  /* application pre-initialised */
        VkPhysicalDeviceDrmPropertiesEXT before = drm;
        assert(hybris_drm_query(fake_hal, pd, &out, node_missing) == HYBRIS_DRM_NO_NODE);
        assert(!memcmp(&before, &drm, sizeof drm));
        assert(node_calls == 1);
    }
''')
p.write_text(t)
print("0002 and hybris_drm_properties_test.c updated")
EOF
```

Rerun Step 4 (`python3 -m unittest tools.tests.test_hybris_compat -v`). Expected: all tests `ok`, `OK`. Commit: `git add tools/hybris/patches/0002-drm-properties.patch tools/tests/hybris_drm_properties_test.c` with message 'hybris: fill DRM properties the driver zeroes' and the usual trailer. (Do this before Task 9; 0003 applies unchanged on top, verified.)

---

### Task 9: `d3d` BC policy (`0003-bc-d3d-profile.patch`)

Moves the policy table into a pure, Vulkan-free module `compat/bc_rules.{c,h}` and adds `HYBRIS_BC_TEXTURES=d3d` (policy 4): native-first selection of the 14 D3D formats, Vulkan 1.4 devices allowed and presented as 1.3 (as `dxvk` does), maintenance6 allowed, `textureCompressionBC` reported TRUE only when every one of the 14 formats has SAMPLED, FILTER_LINEAR, BLIT_SRC, TRANSFER_SRC and TRANSFER_DST natively or through emulation, and stripped before the driver sees it at `vkCreateDevice`. Policies 0–3 select exactly as before (proved by a 20,000-round equivalence test); `dxvk` now also reports the feature when complete. Diagnostics, once per process: `HYBRIS_BC policy=… emulated=0x… texture_compression=… gap_format=… lacking=0x…` and `HYBRIS_BC refused: <reason>`.

**Files:**
- Create: `tools/tests/hybris_bc_rules_check.c` (Appendix D), `tools/tests/hybris_bc_policy_fake.c` (Appendix E)
- Modify: `tools/tests/test_hybris_compat.py`
- Create: `tools/hybris/patches/0003-bc-d3d-profile.patch` (Appendix B)

- [ ] **Step 1: Add the harnesses and their test methods**

Copy Appendices D and E to `tools/tests/`. Add to `HybrisCompatTest`:
```python
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
```
Also replace the characterization test's source list (it must cover the new module too):
```python
        objects = [self.vulkan / "compat/bc_policy.c", self.vulkan / "compat/bc_image.c"]
```
with:
```python
        objects = [self.vulkan / "compat/bc_policy.c", self.vulkan / "compat/bc_image.c",
                   *sorted(self.vulkan.glob("compat/bc_rules.c"))]
```

- [ ] **Step 2: Run and see them fail**

WSL: `python3 -m unittest tools.tests.test_hybris_compat -v`
Expected: `test_bc_rules` FAILs with `compat/bc_rules.h: No such file or directory`; `test_bc_policy_against_a_vulkan_1_4_driver` FAILs with a compile error (`implicit declaration of function hybris_bc_texture_compression` and `compat/bc_rules.c: No such file or directory`); the other two tests stay `ok`.

- [ ] **Step 3: Add the patch (Appendix B)**

Copy Appendix B verbatim to `tools/hybris/patches/0003-bc-d3d-profile.patch` (LF; it is generated against the tree with 0002 applied).

- [ ] **Step 4: Run again**

WSL: `python3 -m unittest tools.tests.test_hybris_compat -v`
Expected: all four tests `ok` (the captured harness output contains `BC_RULES_CHECK checks=60063 failures=0` and one `BC_POLICY_FAKE policy=<p> … failures=0` per policy; it is shown only when a test fails).

- [ ] **Step 5: Standard checks, then commit**

```bash
git add tools/hybris/patches/0003-bc-d3d-profile.patch tools/tests/hybris_bc_rules_check.c tools/tests/hybris_bc_policy_fake.c tools/tests/test_hybris_compat.py
git commit -m "hybris: d3d BC policy reports textureCompressionBC when every D3D format is covered

Decodes only what the driver cannot sample (BC6H and BC7 on PowerVR) so
Proton's own DXVK accepts the GPU.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: PowerVR game defaults

Game launches on PowerVR start with the `d3d` policy and without Steam's Fossilize layer. The defaults ride the existing `GameEnvironment.defaults()` rail (the `forceSsbs` pattern), which `GameEnvironmentStore.publish` writes into the guest's `game-environment.json` and `droiddeck-game-env` applies to game launches only. The editor shows them, so a user can override them per game or in the shared scope; the Experimental graphics profile overrides them too.

**Files:**
- Modify: `app/src/main/java/com/droiddeck/launcher/core/GameEnvironment.kt`
- Modify: `app/src/main/java/com/droiddeck/launcher/session/GameEnvironmentStore.kt`
- Modify: `app/src/main/java/com/droiddeck/launcher/ui/GameEnvironmentEditor.kt`
- Test: `app/src/test/java/com/droiddeck/launcher/core/GameEnvironmentTest.kt`

- [ ] **Step 1: Write the failing tests**

Add to `GameEnvironmentTest`:
```kotlin
    @Test fun powerVrGamesStartWithTheD3dBcPolicyAndWithoutFossilize() {
        val plain = GameEnvironment.defaults("")
        GameEnvironment.POWERVR.keys.forEach { assertFalse(it, plain.containsKey(it)) }
        val pvr = GameEnvironment.defaults("", powerVr = true)
        assertEquals("d3d", pvr["HYBRIS_BC_TEXTURES"])
        assertEquals("1", pvr["DISABLE_VK_LAYER_VALVE_steam_fossilize_1"])
        assertTrue(pvr.containsKey("ENABLE_VK_LAYER_VALVE_steam_fossilize_1"))
        assertNull(pvr["ENABLE_VK_LAYER_VALVE_steam_fossilize_1"])
    }

    @Test fun theSharedScopeOrAGameOverridesThePowerVrDefaults() {
        val config = GameEnvironment.Config(shared = mapOf("HYBRIS_BC_TEXTURES" to "dxvk"))
            .withEntries("42", mapOf("HYBRIS_BC_TEXTURES" to "0"))
        assertEquals("0", GameEnvironment.effective(config, "", "42", powerVr = true)["HYBRIS_BC_TEXTURES"])
        assertEquals("dxvk", GameEnvironment.effective(config, "", "43", powerVr = true)["HYBRIS_BC_TEXTURES"])
        assertEquals("d3d", GameEnvironment.effective(GameEnvironment.Config(), "", "43", powerVr = true)["HYBRIS_BC_TEXTURES"])
    }
```

- [ ] **Step 2: Run them and see them fail**

PowerShell: `.\gradlew.bat -PskipRust=true :app:testDebugUnitTest --tests "com.droiddeck.launcher.core.GameEnvironmentTest"`
Expected: compilation failure, `Unresolved reference: POWERVR` / `No parameter with name 'powerVr'`.

- [ ] **Step 3: Implement in `GameEnvironment.kt`**

Replace:
```kotlin
    fun defaults(preset: String, forceSsbs: Boolean = false): Map<String, String?> = linkedMapOf<String, String?>(
```
with:
```kotlin
    fun defaults(preset: String, forceSsbs: Boolean = false, powerVr: Boolean = false): Map<String, String?> = linkedMapOf<String, String?>(
```
Replace:
```kotlin
        if (forceSsbs) put(FORCE_SSBS, "1")
    }
```
with:
```kotlin
        if (forceSsbs) put(FORCE_SSBS, "1")
        if (powerVr) putAll(POWERVR)
    }

    /**
     * Every game launch on PowerVR, so no game needs settings of its own. "d3d" is the hybris
     * compat layer's Direct3D BC policy: it decodes the BC formats the driver cannot sample and
     * reports textureCompressionBC, so Proton's own DXVK accepts the GPU. Steam's Fossilize layer
     * made DXVK lose its surface. Only game launches read these (droiddeck-game-env), never
     * gamescope or the Steam client, which the policy's 1.3 cap would otherwise reach.
     */
    val POWERVR: Map<String, String?> = linkedMapOf(
        "HYBRIS_BC_TEXTURES" to "d3d",
        "DISABLE_VK_LAYER_VALVE_steam_fossilize_1" to "1",
        "ENABLE_VK_LAYER_VALVE_steam_fossilize_1" to null,
    )
```
Replace:
```kotlin
    fun effective(config: Config, preset: String, scope: String, forceSsbs: Boolean = false): Map<String, String?> =
        defaults(preset, forceSsbs) + config.shared + if (scope.isEmpty()) emptyMap() else config.games[scope].orEmpty()
```
with:
```kotlin
    fun effective(config: Config, preset: String, scope: String, forceSsbs: Boolean = false, powerVr: Boolean = false): Map<String, String?> =
        defaults(preset, forceSsbs, powerVr) + config.shared + if (scope.isEmpty()) emptyMap() else config.games[scope].orEmpty()
```

- [ ] **Step 4: Publish them on PowerVR (`GameEnvironmentStore.kt`)**

Add the import `import com.droiddeck.launcher.gpu.SystemVulkanDriver`. Replace:
```kotlin
        val resolved = config.copy(shared = GameEnvironment.defaults(SessionPrefs.fexPreset(context), SessionPrefs.forceSsbs(context)) + config.shared)
```
with:
```kotlin
        val resolved = config.copy(shared = GameEnvironment.defaults(SessionPrefs.fexPreset(context),
            SessionPrefs.forceSsbs(context), SystemVulkanDriver.isPowerVr()) + config.shared)
```

- [ ] **Step 5: Show them in the editor (`GameEnvironmentEditor.kt`)**

Add the import `import com.droiddeck.launcher.gpu.SystemVulkanDriver`. Replace:
```kotlin
    val forceSsbs = SessionPrefs.forceSsbs(context)
```
with:
```kotlin
    val forceSsbs = SessionPrefs.forceSsbs(context)
    // One file check per editor, not per recomposition.
    val powerVr = remember { SystemVulkanDriver.isPowerVr() }
```
Replace:
```kotlin
            val entries = GameEnvironment.effective(current, preset, scope, forceSsbs).toSortedMap()
```
with:
```kotlin
            val entries = GameEnvironment.effective(current, preset, scope, forceSsbs, powerVr).toSortedMap()
```

- [ ] **Step 6: Run the tests**

PowerShell: `.\gradlew.bat -PskipRust=true :app:testDebugUnitTest --tests "com.droiddeck.launcher.core.GameEnvironmentTest" --tests "com.droiddeck.launcher.session.GameEnvironmentStoreTest"`
Expected: `BUILD SUCCESSFUL`, all tests pass.

- [ ] **Step 7: Standard checks, then commit**

```bash
git add app/src/main/java/com/droiddeck/launcher/core/GameEnvironment.kt app/src/main/java/com/droiddeck/launcher/session/GameEnvironmentStore.kt app/src/main/java/com/droiddeck/launcher/ui/GameEnvironmentEditor.kt app/src/test/java/com/droiddeck/launcher/core/GameEnvironmentTest.kt
git commit -m "PowerVR games start with the d3d BC policy and without Fossilize

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 11: One Adreno predicate; no "black screen" warning on PowerVR

The Adreno check exists twice (`GpuInfo.detect()` and `DeviceSupport.adreno()`), and `MainActivity` shows "Not an Adreno GPU … black screen" before the runtime download on PowerVR, which draws through libhybris. Keep `GpuInfo.Family.NOT_ADRENO` for PowerVR: `TurnipDriver` relies on it to choose the system driver.

**Files:**
- Modify: `app/src/main/java/com/droiddeck/launcher/gpu/SystemVulkanDriver.java`
- Modify: `app/src/main/java/com/droiddeck/launcher/gpu/GpuInfo.kt`
- Modify: `app/src/main/java/com/droiddeck/launcher/core/DeviceSupport.kt`
- Modify: `app/src/main/java/com/droiddeck/launcher/MainActivity.kt`
- Test (new): `app/src/test/java/com/droiddeck/launcher/core/DeviceSupportTest.kt`

- [ ] **Step 1: Write the failing test**

Create `app/src/test/java/com/droiddeck/launcher/core/DeviceSupportTest.kt`:
```kotlin
package com.droiddeck.launcher.core

import com.droiddeck.launcher.gpu.GpuInfo
import com.droiddeck.launcher.gpu.SystemVulkanDriver
import org.junit.After
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.nio.file.Files

class DeviceSupportTest {
    private lateinit var root: File

    @Before fun setUp() { root = Files.createTempDirectory("device-support").toFile() }
    @After fun tearDown() { root.deleteRecursively() }

    private fun touch(path: String) { File(root, path).apply { parentFile!!.mkdirs(); createNewFile() } }

    @Test fun neitherStackCannotDraw() {
        assertFalse(GpuInfo.isAdreno(root))
        assertFalse(SystemVulkanDriver.isPowerVr(root))
        assertFalse(DeviceSupport.canDraw(root))
    }

    @Test fun aKgslNodeOrTheAdrenoDriverIsAnAdreno() {
        File(root, "sys/class/kgsl/kgsl-3d0").mkdirs()
        assertTrue(GpuInfo.isAdreno(root))
        assertTrue(DeviceSupport.canDraw(root))
        File(root, "sys").deleteRecursively()
        touch("vendor/lib64/hw/vulkan.adreno.so")
        assertTrue(GpuInfo.isAdreno(root))
    }

    @Test fun powerVrIsNotAnAdrenoButCanDraw() {
        touch("vendor/lib64/hw/vulkan.powervr.so")
        assertFalse(GpuInfo.isAdreno(root))
        assertTrue(SystemVulkanDriver.isPowerVr(root))
        assertTrue(DeviceSupport.canDraw(root))
    }

    @Test fun aDirectoryNamedLikeTheDriverIsNotOne() {
        File(root, "vendor/lib64/hw/vulkan.powervr.so").mkdirs()
        assertFalse(SystemVulkanDriver.isPowerVr(root))
        assertFalse(DeviceSupport.canDraw(root))
    }
}
```

- [ ] **Step 2: Run it and see it fail**

PowerShell: `.\gradlew.bat -PskipRust=true :app:testDebugUnitTest --tests "com.droiddeck.launcher.core.DeviceSupportTest"`
Expected: compilation failure, `Unresolved reference: isAdreno` / `canDraw`.

- [ ] **Step 3: `SystemVulkanDriver.java`**

Replace:
```java
    private static final String HAL = "/vendor/lib64/hw/vulkan.powervr.so";
```
with:
```java
    /** Public for the session report (core.DeviceReport). */
    public static final String HAL = "/vendor/lib64/hw/vulkan.powervr.so";
```
Replace:
```java
    public static boolean isPowerVr() { return new File(HAL).isFile(); }
```
with:
```java
    public static boolean isPowerVr() { return isPowerVr(new File("/")); }

    /** [root] is "/" on a device; a test points it at a directory holding a fake /vendor. */
    public static boolean isPowerVr(File root) { return new File(root, HAL).isFile(); }
```

- [ ] **Step 4: `GpuInfo.kt`**

In the `companion object`, replace:
```kotlin
        fun detect(): GpuInfo {
            val adreno = File("/sys/class/kgsl/kgsl-3d0").exists() || File("/vendor/lib64/hw/vulkan.adreno.so").exists()
```
with:
```kotlin
        private const val KGSL = "/sys/class/kgsl/kgsl-3d0"
        private const val ADRENO_HAL = "/vendor/lib64/hw/vulkan.adreno.so"

        /** Qualcomm's stack: the KGSL node, or the vendor's own Vulkan driver at its usual path.
         *  The app's one Adreno test; [root] is "/" on a device. */
        fun isAdreno(root: File = File("/")): Boolean = File(root, KGSL).exists() || File(root, ADRENO_HAL).exists()

        fun detect(): GpuInfo {
            val adreno = isAdreno()
```

- [ ] **Step 5: `DeviceSupport.kt`**

Add imports `import com.droiddeck.launcher.gpu.GpuInfo` and `import com.droiddeck.launcher.gpu.SystemVulkanDriver`. Replace the doc comment and `adreno()`:
```kotlin
/**
 * Whether this device can draw a Linux session at all.
 *
 * The runtime draws with Turnip, an Adreno driver. On Mali, Xclipse and PowerVR the compositor
 * gets no usable Vulkan device and a session comes up as sound over a black screen - a failure
 * with nothing in it to read. The app cannot fix that, so it says so before the download rather
 * than after it. Adreno is recognised by what only Qualcomm's stack has: the KGSL node, or the
 * vendor's own Vulkan driver at its usual path.
 */
object DeviceSupport {
    fun adreno(): Boolean =
        File("/sys/class/kgsl/kgsl-3d0").exists() || File("/vendor/lib64/hw/vulkan.adreno.so").exists()
```
with:
```kotlin
/**
 * Whether this device can draw a Linux session at all.
 *
 * Adreno draws with Turnip; PowerVR draws with the phone's own driver through libhybris. On Mali
 * and Xclipse the compositor gets no usable Vulkan device and a session comes up as sound over a
 * black screen - a failure with nothing in it to read. The app cannot fix that, so it says so
 * before the download rather than after it.
 */
object DeviceSupport {
    /** [root] is "/" on a device; a test points it at a directory holding a fake /sys and /vendor. */
    fun canDraw(root: File = File("/")): Boolean = GpuInfo.isAdreno(root) || SystemVulkanDriver.isPowerVr(root)
```

- [ ] **Step 6: `MainActivity.kt` (two call sites)**

Replace:
```kotlin
        // The runtime draws with Turnip, an Adreno driver: on Mali, Xclipse or PowerVR the
        // compositor gets no usable Vulkan device and a session is sound over a black screen.
        // Said before the download, not after it; the user may still go ahead.
        if (installed == null && !com.droiddeck.launcher.core.DeviceSupport.adreno()) { showNonAdreno = release; return }
```
with:
```kotlin
        // On Mali or Xclipse the compositor gets no usable Vulkan device and a session is sound
        // over a black screen. Said before the download, not after it; the user may still go ahead.
        if (installed == null && !com.droiddeck.launcher.core.DeviceSupport.canDraw()) { showNonAdreno = release; return }
```
Replace:
```kotlin
        val warn = installed == null && !com.droiddeck.launcher.core.DeviceSupport.adreno()
```
with:
```kotlin
        val warn = installed == null && !com.droiddeck.launcher.core.DeviceSupport.canDraw()
```
Then confirm no caller is left: `git grep -n 'DeviceSupport.adreno' -- app/src` prints nothing.

- [ ] **Step 7: Run the tests**

PowerShell: `.\gradlew.bat -PskipRust=true :app:testDebugUnitTest --tests "com.droiddeck.launcher.core.DeviceSupportTest" --tests "com.droiddeck.launcher.gpu.*"`
Expected: `BUILD SUCCESSFUL` (DriverPairsTest, GpuModelFallbackTest, TurnipDriverSelectionTest and SystemVulkanSelectionTest unchanged and passing).

- [ ] **Step 8: Standard checks, then commit**

```bash
git add app/src/main/java/com/droiddeck/launcher/gpu/SystemVulkanDriver.java app/src/main/java/com/droiddeck/launcher/gpu/GpuInfo.kt app/src/main/java/com/droiddeck/launcher/core/DeviceSupport.kt app/src/main/java/com/droiddeck/launcher/MainActivity.kt app/src/test/java/com/droiddeck/launcher/core/DeviceSupportTest.kt
git commit -m "One Adreno test; PowerVR no longer gets the black-screen warning

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 12: PowerVR section in the session report

`device.txt` (written on the session-start worker thread by `SessionService`) gains a PowerVR block, so one Share-logs zip shows the driver and the defaults in effect. `VulkanInfo.query()` blocks on first use; it is only called here when PowerVR is detected, off the UI thread.

**Files:**
- Modify: `app/src/main/java/com/droiddeck/launcher/core/DeviceReport.kt`
- Test (new): `app/src/test/java/com/droiddeck/launcher/core/DeviceReportPowerVrTest.kt`

- [ ] **Step 1: Write the failing test**

```kotlin
package com.droiddeck.launcher.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class DeviceReportPowerVrTest {
    @Test fun unknownFactsReadUnknownNeverAGuess() {
        val rows = DeviceReport.powerVrRows(emptyMap(), 3_185_046).toMap()
        assertEquals("unknown", rows["Vulkan device"])
        assertEquals("unknown", rows["Driver"])
        assertEquals("unknown", rows["Driver BC feature"])
        assertTrue(rows.getValue("System driver").endsWith("(3185046 bytes)"))
    }

    @Test fun driverFactsAndGameDefaultsAreListed() {
        val vk = mapOf("device" to "PowerVR D-Series DXT-48-1536 MC1", "api" to "1.4.317",
            "driver_name" to "PowerVR", "driver_info" to "25.3@6908880", "texture_bc" to "no")
        val rows = DeviceReport.powerVrRows(vk, 1).toMap()
        assertEquals("PowerVR D-Series DXT-48-1536 MC1 · API 1.4.317", rows["Vulkan device"])
        assertEquals("PowerVR 25.3@6908880", rows["Driver"])
        assertEquals("no", rows["Driver BC feature"])
        assertEquals("HYBRIS_BC_TEXTURES=d3d DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1 -ENABLE_VK_LAYER_VALVE_steam_fossilize_1",
            rows["Game defaults"])
    }

    @Test fun keysFitTheReportColumn() {
        DeviceReport.powerVrRows(emptyMap(), 0).forEach { (key, _) -> assertTrue(key, key.length < 24) }
    }
}
```

- [ ] **Step 2: Run it and see it fail**

PowerShell: `.\gradlew.bat -PskipRust=true :app:testDebugUnitTest --tests "com.droiddeck.launcher.core.DeviceReportPowerVrTest"`
Expected: `Unresolved reference: powerVrRows`.

- [ ] **Step 3: Implement in `DeviceReport.kt`**

Add the import `import com.droiddeck.launcher.gpu.SystemVulkanDriver`. Replace:
```kotlin
        k("Runtime's own ICD", LinuxRuntime.vulkanIcd(context)?.path)

        h("Runtime")
```
with:
```kotlin
        k("Runtime's own ICD", LinuxRuntime.vulkanIcd(context)?.path)

        if (gpu.powerVr) {
            h("PowerVR")
            // query() blocks on first use; this report is written on the session-start worker.
            powerVrRows(com.droiddeck.launcher.gpu.VulkanInfo.query(), File(SystemVulkanDriver.HAL).length())
                .forEach { (key, value) -> k(key, value) }
        }

        h("Runtime")
```
Before `private fun readSys(`, add:
```kotlin
    /** The PowerVR block's rows. Keys stay under k()'s 24 columns; absent facts read "unknown". */
    internal fun powerVrRows(vk: Map<String, String>, halBytes: Long): List<Pair<String, String>> = listOf(
        "System driver" to "${SystemVulkanDriver.HAL} ($halBytes bytes)",
        "Vulkan device" to listOfNotNull(vk["device"], vk["api"]?.let { "API $it" }).joinToString(" · ").ifEmpty { "unknown" },
        "Driver" to listOfNotNull(vk["driver_name"], vk["driver_info"]).joinToString(" ").ifEmpty { "unknown" },
        // The phone's own answer through Android's loader; the compat layer's answer is in session.log.
        "Driver BC feature" to (vk["texture_bc"] ?: "unknown"),
        "Game defaults" to GameEnvironment.POWERVR.entries.joinToString(" ") { (name, value) ->
            if (value == null) "-$name" else "$name=$value" },
        "Compat layer" to "session.log: HYBRIS_DRM_PROPERTIES and HYBRIS_BC lines",
    )
```

- [ ] **Step 4: Run the test**

PowerShell: `.\gradlew.bat -PskipRust=true :app:testDebugUnitTest --tests "com.droiddeck.launcher.core.DeviceReportPowerVrTest"`
Expected: `BUILD SUCCESSFUL`, 3 tests pass.

- [ ] **Step 5: Standard checks, then commit**

```bash
git add app/src/main/java/com/droiddeck/launcher/core/DeviceReport.kt app/src/test/java/com/droiddeck/launcher/core/DeviceReportPowerVrTest.kt
git commit -m "Session report: PowerVR driver and game defaults in device.txt

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 13: Documentation

**Files:** Modify `tools/hybris/BC-PROFILE.md`

- [ ] **Step 1: Correct the stale sentence, then add a section at the top of `tools/hybris/BC-PROFILE.md`, after the title**

Replace "The global `textureCompressionBC` feature remains false." with "The global `textureCompressionBC` feature is reported true only when all fourteen Direct3D BC formats are covered (see the `texture_compression=` field of the `HYBRIS_BC` log line)." Then add:

```markdown
## Default on PowerVR: `HYBRIS_BC_TEXTURES=d3d`

Every game launch on a PowerVR device starts with `HYBRIS_BC_TEXTURES=d3d` and without Steam's
Fossilize layer (`GameEnvironment.POWERVR`, published to `game-environment.json`). The `d3d`
policy keeps BC1-BC5 native where the driver samples, filters, blits and copies them, decodes
the rest of the fourteen Direct3D formats (BC6H and BC7 on the Pixel 10), presents Vulkan 1.4
devices as 1.3, and reports `textureCompressionBC` only when all fourteen are covered. Proton's
own DXVK then accepts the GPU, so the **Standard** graphics choice runs Direct3D 9-11 games.
Gamescope and the Steam client never see the policy. The **Experimental** choice below still
overrides it with `dxvk` and the format-checking DXVK.

The layer prints, once per process, `HYBRIS_BC policy=... emulated=0x... texture_compression=...`
or `HYBRIS_BC refused: <reason>`. DX12 is not covered yet: VKD3D-Proton needs push descriptors,
which the layer hides while it emulates a format.
```

- [ ] **Step 2: Commit**

```bash
git add tools/hybris/BC-PROFILE.md
git commit -m "docs: the d3d BC policy is the PowerVR game default

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 14: CI build and M1 on the phone

Ask the user before Step 1 (push and start a CI run) and before Step 2 (download the APK, about 46 MB).

- [ ] **Step 1: Push and build**

```bash
git push fork pixel10
SHA=$(git rev-parse HEAD); RUN=0
for i in $(seq 1 24); do
  RUN=$(gh run list -R jmsether/DroidDeck -w build.yml -b pixel10 -e push -L 5 --json databaseId,headSha \
        -q "[.[] | select(.headSha==\"$SHA\")][0].databaseId // 0")
  [ "$RUN" != "0" ] && break
  sleep 5
done
[ "$RUN" != "0" ] || { echo "no Build APK run for $SHA after 2 minutes" >&2; exit 1; }
gh run watch "$RUN" -R jmsether/DroidDeck --exit-status --compact
```
Expected: success. If it fails, read `gh run view "$RUN" -R jmsether/DroidDeck --log-failed`, fix the cause on `pixel10`, push, and dispatch again. The hybris step rebuilds (the patches changed the cache key; about 3 minutes) and `check_runtime.py` passes. CI's Python step runs `test_hybris_compat` with network fetches; a failure there blocks the APK.

- [ ] **Step 2: Download and install (user)**

```bash
RUN=$(gh run list -R jmsether/DroidDeck -w build.yml -b pixel10 -e push -L 1 --json databaseId -q '.[0].databaseId')
mkdir -p pixel10-runs/apk-m1 && gh run download "$RUN" -R jmsether/DroidDeck -D pixel10-runs/apk-m1
find pixel10-runs/apk-m1 -type f -name '*.apk' -exec ls -l {} +
```
Then the user runs `python tools\pixel10\pixel10.py install <path printed by find>`.

- [ ] **Step 3: M1 session (user)**

The user starts Steam (Play), signs in, and uses Big Picture for 10 minutes with touch and a controller if available, then shares logs to Downloads; `python tools\pixel10\pixel10.py logs`, then unpack the zip as in Task 7 Step 4.

**Acceptance (M1):** Big Picture renders and responds for 10 minutes with no crash. Record in the bring-up log: build, `HYBRIS_DRM_PROPERTIES outcome=` line, gamescope backend, any errors.

- [ ] **Step 4: If M1 fails**

Use superpowers:systematic-debugging on the first failure in `session.log`. Each fix is a small commit with a test, then repeat Steps 1–3. Typical first suspects in order: the DRM outcome line (Task 8), the hybris ICD's swapchain checks (Task 7 probe lines), gamescope patches 0120–0125 behaviour on D-Series (vendor 0x1010 gates).

- [ ] **Step 5: Commit the log entry**

```bash
git add docs/pixel10/bringup-log.md
git commit -m "docs: Pixel 10 Pro M1 result

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 15: M2 on the phone, then the Plan 2 gate

- [ ] **Step 1: Pick the test games (user)**

Ask the user for 2–3 titles from their library: at least one Direct3D 9 and one Direct3D 11 title, no kernel anti-cheat. Record them in the log.

- [ ] **Step 2: Run each with Standard graphics and no per-game settings (user)**

For each title: launch from Big Picture, play 5 minutes, share logs to Downloads, `python tools\pixel10\pixel10.py logs`, unpack the zip as in Task 7 Step 4. Record per title: reached gameplay (yes/no), rough FPS from the overlay, the game's `HYBRIS_BC policy=… texture_compression=…` line (expected `policy=4 … texture_compression=1`), DXVK's adapter line, and any `IMG_vk…` frames in a crash.

**Acceptance (M2):** 2–3 DX9/DX11 titles reach gameplay with no per-game settings.

- [ ] **Step 3: If a title fails, classify before fixing**

- `HYBRIS_BC refused: …` or `texture_compression=0 gap_format=… lacking=…`: the device's format or queue answers differ from the host fakes; fix in 0003 with a harness case reproducing the device's answer.
- A crash in `IMG_vkCmdClearAttachments`, `IMG_vkCmdBeginRenderPass*` or `IMG_vkDestroyPipeline`: a driver-25.3 bug (spec §6.5); record it for Plan 2, do not patch blind.
- Corrupt or wrong textures while the log shows `policy=4` and native BC1–BC5: decode everything. Change `HYBRIS_BC_TEXTURES` in `GameEnvironment.POWERVR` from `d3d` to `dxvk` (update the Task 10 tests, Task 12's expected row and Task 13's text in the same commit), re-run that title, and record both results.
- Anything else: superpowers:systematic-debugging.

- [ ] **Step 4: Commit the log and write Plan 2**

```bash
git add docs/pixel10/bringup-log.md
git commit -m "docs: Pixel 10 Pro M2 results

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
Then use superpowers:writing-plans to write `docs/superpowers/plans/<date>-pixel10-pro-port-m3.md`: push-descriptor journaling for the compute bind point in `bc_command_state.c` (so VKD3D-Proton can create a device while BC6H/BC7 are emulated), and any driver-25.3 workarounds M2 proved necessary.

---

## Appendices

Each appendix is the exact content of one file. Copy it verbatim (LF line endings, no trailing changes). They were verified on the host on 2026-10-10:
- A + B apply in build order on the pin (`bc8cb358`) after `powervr-hal-version.patch` and `0001-dxvk-bc-profile.patch`.
- C–E compile with `-Wall -Wextra -Werror` and pass against fake drivers (60,063 rule checks; every policy).
- F–J compile and pass against a fake PowerVR HAL.

Nothing in them ran on a device.

### Appendix A: `tools/hybris/patches/0002-drm-properties.patch`

~~~~diff
diff --git a/docs/compatibility-defaults.md b/docs/compatibility-defaults.md
index 4a31207..e7f5551 100644
--- a/docs/compatibility-defaults.md
+++ b/docs/compatibility-defaults.md
@@ -14,6 +14,7 @@ Native format support takes precedence; forced emulation remains opt-in.
 | HYBRIS_VULKAN_COMPAT_INOUT | 1 | 0 |
 | HYBRIS_VULKAN_COMPAT_IMAGE_BOUNDS | 1 | 0 |
 | HYBRIS_VULKAN_COMPAT_VERTEX_STORES | 1, restricted to the inspected driver | 0 |
+| HYBRIS_VULKAN_COMPAT_DRM_PROPERTIES | 1, only when the driver ignores the structure | 0 |
 | HYBRIS_MALI_MMUD_SKIP_LOADER_CHECK | 1, restricted to the matching driver build | 0 |
 
 The first seven switches remain disabled during secure execution. Existing
diff --git a/hybris/vulkan/Makefile.am b/hybris/vulkan/Makefile.am
index e033f7b..2fa943f 100644
--- a/hybris/vulkan/Makefile.am
+++ b/hybris/vulkan/Makefile.am
@@ -13,6 +13,7 @@ libVkLayer_hybris_compat_la_SOURCES = layer/entry.c layer/device.c layer/physica
     compat/bc_image.c compat/bc_image.h compat/bc_command_state.c compat/bc_command_state.h compat/bc_transfer.c compat/bc_transfer.h \
     compat/bc_context.c compat/bc_context.h compat/bc_commands.c compat/bc_resources.c compat/bc_record.c compat/bc_barriers.c compat/bc_image_copy.c compat/bc_policy.c compat/bc_policy.h \
     compat/application_policy.c compat/application_policy.h \
+    compat/drm_properties.c compat/drm_properties.h \
     compat/memory_visibility.c compat/memory_visibility.h compat/rendering_segments.c compat/rendering_segments.h \
     compat/readback_record.c compat/readback_submit.c compat/readback.h \
     compat/vertex_stores.c compat/vertex_stores.h compat/spirv_discard.c \
diff --git a/hybris/vulkan/compat/drm_properties.c b/hybris/vulkan/compat/drm_properties.c
new file mode 100644
index 0000000..ee0fbe3
--- /dev/null
+++ b/hybris/vulkan/compat/drm_properties.c
@@ -0,0 +1,108 @@
+/* SPDX-License-Identifier: Apache-2.0 */
+/* Rail contract: a VkPhysicalDeviceDrmPropertiesEXT in a properties2 chain is
+ * painted with a sentinel before the driver runs. If the sentinel survives the
+ * driver ignored the structure: the render node (stat, injected) fills
+ * hasRender/renderMajor/renderMinor and hasPrimary stays false. If it did not
+ * survive, the driver's answer passes through unchanged. With no node the
+ * application's own bytes are restored: absence is reported, never invented.
+ * The node is only stat'ed when the driver ignored the structure. The chain
+ * walk is bounded. One diagnostic line per outcome per process. Disable with
+ * HYBRIS_VULKAN_COMPAT_DRM_PROPERTIES=0 (ignored in secure processes). */
+#define _GNU_SOURCE
+#define VK_NO_PROTOTYPES
+#include "drm_properties.h"
+#include <errno.h>
+#include <pthread.h>
+#include <stdatomic.h>
+#include <stddef.h>
+#include <stdio.h>
+#include <stdlib.h>
+#include <string.h>
+#include <sys/auxv.h>
+#include <sys/stat.h>
+#include <sys/sysmacros.h>
+
+/* The payload (two VkBool32 and four int64_t) follows sType/pNext and ends the
+ * structure. VkBool32 is 0 or 1, so a driver never writes it all 0xA5. */
+#define PAYLOAD offsetof(VkPhysicalDeviceDrmPropertiesEXT, hasPrimary)
+#define PAYLOAD_BYTES (sizeof(VkPhysicalDeviceDrmPropertiesEXT) - PAYLOAD)
+_Static_assert(offsetof(VkPhysicalDeviceDrmPropertiesEXT, renderMinor) + sizeof(int64_t) ==
+               sizeof(VkPhysicalDeviceDrmPropertiesEXT), "the payload ends the structure");
+enum { SENTINEL_BYTE = 0xA5, CHAIN_LIMIT = 64 };
+#define RENDER_NODE "/dev/dri/renderD128"
+
+static _Atomic unsigned reported;
+static pthread_once_t once = PTHREAD_ONCE_INIT;
+static int enabled = 1;
+static void configure(void)
+{
+    const char *value = getauxval(AT_SECURE) ? NULL : getenv("HYBRIS_VULKAN_COMPAT_DRM_PROPERTIES");
+    enabled = !value || strcmp(value, "0");
+}
+
+struct hybris_drm_node hybris_drm_stat(const char *path)
+{
+    struct stat st;
+    if (!path) return (struct hybris_drm_node){.error = EINVAL};
+    if (stat(path, &st)) return (struct hybris_drm_node){.error = errno};
+    if (!S_ISCHR(st.st_mode)) return (struct hybris_drm_node){.error = ENOTSUP};
+    return (struct hybris_drm_node){.valid = 1, .major = major(st.st_rdev), .minor = minor(st.st_rdev)};
+}
+
+struct hybris_drm_node hybris_drm_render_node(void)
+{
+    pthread_once(&once, configure);
+    return enabled ? hybris_drm_stat(RENDER_NODE) : (struct hybris_drm_node){.error = ECANCELED};
+}
+
+static VkPhysicalDeviceDrmPropertiesEXT *find_drm(const VkPhysicalDeviceProperties2 *out)
+{
+    VkBaseOutStructure *node = out->pNext;
+    for (unsigned i = 0; node && i < CHAIN_LIMIT; ++i, node = node->pNext)
+        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT)
+            return (VkPhysicalDeviceDrmPropertiesEXT *)node;
+    return NULL;
+}
+
+static void report(enum hybris_drm_outcome outcome, const VkPhysicalDeviceProperties2 *out,
+                   const VkPhysicalDeviceDrmPropertiesEXT *drm, int error)
+{
+    static const char *const names[] = {"not-requested", "driver-answered", "filled", "no-node"};
+    unsigned bit = 1u << outcome;
+    if (atomic_fetch_or(&reported, bit) & bit) return;
+    fprintf(stderr, "HYBRIS_DRM_PROPERTIES outcome=%s vendor=0x%04x device=0x%08x driver=0x%08x "
+            "hasRender=%u render=%lld:%lld hasPrimary=%u primary=%lld:%lld error=%d\n",
+            names[outcome], out->properties.vendorID, out->properties.deviceID, out->properties.driverVersion,
+            drm->hasRender, (long long)drm->renderMajor, (long long)drm->renderMinor,
+            drm->hasPrimary, (long long)drm->primaryMajor, (long long)drm->primaryMinor, error);
+}
+
+enum hybris_drm_outcome hybris_drm_query(PFN_vkGetPhysicalDeviceProperties2 query,
+    VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out, hybris_drm_node_fn render_node)
+{
+    VkPhysicalDeviceDrmPropertiesEXT *drm = find_drm(out);
+    if (!drm) { query(physical, out); return HYBRIS_DRM_NOT_REQUESTED; }
+    VkPhysicalDeviceDrmPropertiesEXT original = *drm, painted;
+    unsigned char *payload = (unsigned char *)drm + PAYLOAD;
+    memset(payload, SENTINEL_BYTE, PAYLOAD_BYTES);
+    painted = *drm;
+    query(physical, out);
+    enum hybris_drm_outcome outcome = HYBRIS_DRM_DRIVER_ANSWERED;
+    struct hybris_drm_node render = {0};
+    if (!memcmp(payload, (unsigned char *)&painted + PAYLOAD, PAYLOAD_BYTES)) {
+        render = render_node();
+        if (render.valid) {
+            drm->hasPrimary = VK_FALSE;
+            drm->primaryMajor = drm->primaryMinor = 0;
+            drm->hasRender = VK_TRUE;
+            drm->renderMajor = render.major;
+            drm->renderMinor = render.minor;
+            outcome = HYBRIS_DRM_FILLED;
+        } else {
+            memcpy(payload, (unsigned char *)&original + PAYLOAD, PAYLOAD_BYTES);
+            outcome = HYBRIS_DRM_NO_NODE;
+        }
+    }
+    report(outcome, out, drm, render.error);
+    return outcome;
+}
diff --git a/hybris/vulkan/compat/drm_properties.h b/hybris/vulkan/compat/drm_properties.h
new file mode 100644
index 0000000..e38c83d
--- /dev/null
+++ b/hybris/vulkan/compat/drm_properties.h
@@ -0,0 +1,28 @@
+/* SPDX-License-Identifier: Apache-2.0 */
+#ifndef HYBRIS_DRM_PROPERTIES_H
+#define HYBRIS_DRM_PROPERTIES_H
+#include <vulkan/vulkan.h>
+#include <stdint.h>
+
+/* Render node of the GPU as stat(2) reported it. The zero value is the named
+ * default: no usable node. `error` is an errno value, meaningful only when !valid. */
+struct hybris_drm_node { int valid; int error; int64_t major, minor; };
+/* Supplies the node on demand, so the stat(2) only happens when it is needed. */
+typedef struct hybris_drm_node (*hybris_drm_node_fn)(void);
+
+enum hybris_drm_outcome {
+    HYBRIS_DRM_NOT_REQUESTED,   /* no VkPhysicalDeviceDrmPropertiesEXT in the chain */
+    HYBRIS_DRM_DRIVER_ANSWERED, /* the driver wrote the structure; passed through */
+    HYBRIS_DRM_FILLED,          /* driver left it untouched; render node filled from stat */
+    HYBRIS_DRM_NO_NODE,         /* driver left it untouched and no node: application bytes restored */
+};
+
+/* The only stat(2): a character device yields valid=1 with its major/minor. */
+struct hybris_drm_node hybris_drm_stat(const char *path);
+/* The PowerVR render node, or the named default (no node, error=ECANCELED)
+ * when HYBRIS_VULKAN_COMPAT_DRM_PROPERTIES=0. */
+struct hybris_drm_node hybris_drm_render_node(void);
+/* Runs `query` (the next handler or the driver) around the sentinel protocol. */
+enum hybris_drm_outcome hybris_drm_query(PFN_vkGetPhysicalDeviceProperties2 query,
+    VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out, hybris_drm_node_fn render_node);
+#endif
diff --git a/hybris/vulkan/layer/physical.c b/hybris/vulkan/layer/physical.c
index 29a295e..0d21695 100644
--- a/hybris/vulkan/layer/physical.c
+++ b/hybris/vulkan/layer/physical.c
@@ -6,6 +6,7 @@
 #include "../compat/shader_policy.h"
 #include "../compat/bc_policy.h"
 #include "../compat/clip_distance.h"
+#include "../compat/drm_properties.h"
 #include <stdlib.h>
 #include <string.h>
 
@@ -87,8 +88,27 @@ static VkResult VKAPI_CALL enumerate_device_extensions(VkPhysicalDevice physical
     return properties && written < kept ? VK_INCOMPLETE : VK_SUCCESS;
 }
 
+/* Outermost properties2 handler: a chained VkPhysicalDeviceDrmPropertiesEXT the
+ * driver ignored is filled from the render node. Everything else is the clip,
+ * shader and BC handlers' chain, unchanged. */
+static void properties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out, const char *name)
+{
+    struct hybris_layer_physical context;
+    if (!hybris_layer_lookup_physical(physical, &context)) return;
+    PFN_vkGetPhysicalDeviceProperties2 query = (void *)hybris_clip_policy_proc(name);
+    if (!query) query = (void *)context.resolver(context.instance, name);
+    if (!query) return;
+    hybris_drm_query(query, physical, out, hybris_drm_render_node);
+}
+static void VKAPI_CALL properties2_core(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out)
+{ properties2(physical, out, "vkGetPhysicalDeviceProperties2"); }
+static void VKAPI_CALL properties2_khr(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out)
+{ properties2(physical, out, "vkGetPhysicalDeviceProperties2KHR"); }
+
 PFN_vkVoidFunction hybris_layer_physical_proc(const char *name)
 {
+    if (!strcmp(name, "vkGetPhysicalDeviceProperties2")) return (PFN_vkVoidFunction)properties2_core;
+    if (!strcmp(name, "vkGetPhysicalDeviceProperties2KHR")) return (PFN_vkVoidFunction)properties2_khr;
     PFN_vkVoidFunction function = hybris_clip_policy_proc(name);
     if (!function) function = hybris_shader_policy_proc(name);
     if (!function) function = hybris_bc_policy_proc(name);
~~~~

### Appendix B: `tools/hybris/patches/0003-bc-d3d-profile.patch`

~~~~diff
diff --git a/hybris/vulkan/Makefile.am b/hybris/vulkan/Makefile.am
index 2fa943f..69e3cd1 100644
--- a/hybris/vulkan/Makefile.am
+++ b/hybris/vulkan/Makefile.am
@@ -11,7 +11,7 @@ libVkLayer_hybris_compat_la_SOURCES = layer/entry.c layer/device.c layer/physica
     layer/device_features.c layer/device_features.h layer/device_create_sizes.inc \
     compat/bc_decode.c compat/bc_decode.h compat/shaders/bc_decode.inc \
     compat/bc_image.c compat/bc_image.h compat/bc_command_state.c compat/bc_command_state.h compat/bc_transfer.c compat/bc_transfer.h \
-    compat/bc_context.c compat/bc_context.h compat/bc_commands.c compat/bc_resources.c compat/bc_record.c compat/bc_barriers.c compat/bc_image_copy.c compat/bc_policy.c compat/bc_policy.h \
+    compat/bc_context.c compat/bc_context.h compat/bc_commands.c compat/bc_resources.c compat/bc_record.c compat/bc_barriers.c compat/bc_image_copy.c compat/bc_policy.c compat/bc_policy.h compat/bc_rules.c compat/bc_rules.h \
     compat/application_policy.c compat/application_policy.h \
     compat/drm_properties.c compat/drm_properties.h \
     compat/memory_visibility.c compat/memory_visibility.h compat/rendering_segments.c compat/rendering_segments.h \
diff --git a/hybris/vulkan/compat/bc_policy.c b/hybris/vulkan/compat/bc_policy.c
index 3124a25..2bd5bea 100644
--- a/hybris/vulkan/compat/bc_policy.c
+++ b/hybris/vulkan/compat/bc_policy.c
@@ -3,31 +3,38 @@
 #define VK_NO_PROTOTYPES
 #include "bc_policy.h"
 #include "bc_context.h"
+#include "bc_rules.h"
 #include "../layer/layer.h"
 #include <stdlib.h>
+#include <stdio.h>
 #include <stddef.h>
 #include <string.h>
 #include <sys/auxv.h>
 
+/* bc_rules.h is Vulkan-free; keep its constants equal to the Vulkan ones. */
+_Static_assert(HYBRIS_BC_SAMPLED == VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT &&
+    HYBRIS_BC_BLIT_SRC == VK_FORMAT_FEATURE_BLIT_SRC_BIT &&
+    HYBRIS_BC_LINEAR == VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT &&
+    HYBRIS_BC_TRANSFER_SRC == VK_FORMAT_FEATURE_TRANSFER_SRC_BIT &&
+    HYBRIS_BC_TRANSFER_DST == VK_FORMAT_FEATURE_TRANSFER_DST_BIT, "BC feature bits");
+_Static_assert(HYBRIS_BC_API_1_4 == VK_MAKE_API_VERSION(0, 1, 4, 0) &&
+    HYBRIS_BC_FORMATS == VK_FORMAT_BC7_SRGB_BLOCK - VK_FORMAT_BC1_RGB_UNORM_BLOCK + 1, "BC rule constants");
+
 static pthread_once_t policy_once = PTHREAD_ONCE_INIT;
 static int policy;
 static void initialize_policy(void)
 {
     if (getauxval(AT_SECURE)) return;
-    const char *value = getenv("HYBRIS_BC_TEXTURES");
-    if (!value || !strcmp(value, "missing") || !strcmp(value, "1")) policy = 1;
-    else if (value && !strcmp(value, "force")) policy = 2;
-    else if (value && !strcmp(value, "dxvk")) policy = 3;
+    policy = hybris_bc_parse_policy(getenv("HYBRIS_BC_TEXTURES"));
 }
-int hybris_bc_enabled(void)
+static const struct hybris_bc_traits *traits(void)
 {
     pthread_once(&policy_once, initialize_policy);
-    return policy != 0;
+    return hybris_bc_traits(policy);
 }
-static int dxvk_profile(void)
+int hybris_bc_enabled(void)
 {
-    pthread_once(&policy_once, initialize_policy);
-    return policy == 3;
+    return traits()->enabled;
 }
 #define PHYSICAL_PROC(name) PFN_vk##name name = (PFN_vk##name)context.resolver(context.instance, "vk" #name)
 /* Preserve three-channel border replacement when the native image format
@@ -49,23 +56,37 @@ static unsigned physical_rgb_mask(VkPhysicalDevice physical)
     }
     return mask;
 }
-unsigned hybris_bc_physical_mask(VkPhysicalDevice physical)
+/* The driver's own optimal features for every BC format and for its decoded
+ * backing (0 when none), plus the driver version for the diagnostic. Returns 0
+ * when emulation cannot run on this device; the answers are then unused. */
+struct bc_probe { uint32_t native[HYBRIS_BC_FORMATS], backing[HYBRIS_BC_FORMATS], api, driver; };
+/* A silent mask of 0 hides why emulation is absent: log the first refusal per process. */
+static int refuse(const char *reason)
+{
+    static int logged;
+    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED)) fprintf(stderr, "HYBRIS_BC refused: %s\n", reason);
+    return 0;
+}
+static int probe(VkPhysicalDevice physical, struct bc_probe *out)
 {
     struct hybris_layer_physical context;
-    if (!hybris_bc_enabled() || !hybris_layer_lookup_physical(physical, &context) ||
-        context.api_version < VK_API_VERSION_1_1) return 0;
+    if (!hybris_bc_enabled() || !hybris_layer_lookup_physical(physical, &context)) return 0;
+    if (context.api_version < VK_API_VERSION_1_1) return refuse("application requested Vulkan 1.0");
     PHYSICAL_PROC(GetPhysicalDeviceProperties); PHYSICAL_PROC(GetPhysicalDeviceQueueFamilyProperties);
     PHYSICAL_PROC(GetPhysicalDeviceFormatProperties);
     VkPhysicalDeviceProperties properties;
     GetPhysicalDeviceProperties(physical, &properties);
-    /* Core 1.4 includes descriptor-state commands that are not intercepted yet. */
-    if (properties.apiVersion < VK_API_VERSION_1_1 || (!dxvk_profile() && properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 4, 0)) ||
-        properties.limits.maxStorageBufferRange < 256 ||
-        properties.limits.minStorageBufferOffsetAlignment >= properties.limits.maxStorageBufferRange) return 0;
+    /* Core 1.4 includes descriptor-state commands that are not intercepted yet;
+     * only a profile that presents 1.3 and masks them may run on such a device. */
+    if (properties.apiVersion < VK_API_VERSION_1_1) return refuse("device below Vulkan 1.1");
+    if (!hybris_bc_api_allowed(traits(), properties.apiVersion)) return refuse("Vulkan 1.4 device needs the dxvk or d3d policy");
+    if (properties.limits.maxStorageBufferRange < 256 ||
+        properties.limits.minStorageBufferOffsetAlignment >= properties.limits.maxStorageBufferRange)
+        return refuse("storage buffer limits too small for the decoder");
     uint32_t count = 0;
     GetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
     VkQueueFamilyProperties queues[32];
-    if (!count || count > sizeof(queues) / sizeof(queues[0])) return 0;
+    if (!count || count > sizeof(queues) / sizeof(queues[0])) return refuse("unsupported queue family count");
     GetPhysicalDeviceQueueFamilyProperties(physical, &count, queues);
     for (uint32_t i = 0; i < count; ++i) {
         if (!(queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))) continue;
@@ -74,31 +95,70 @@ unsigned hybris_bc_physical_mask(VkPhysicalDevice physical)
         if (!(queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) ||
             queues[i].minImageTransferGranularity.width != 1 ||
             queues[i].minImageTransferGranularity.height != 1 ||
-            queues[i].minImageTransferGranularity.depth != 1) return 0;
+            queues[i].minImageTransferGranularity.depth != 1)
+            return refuse("a queue family lacks compute or single-texel transfer granularity");
     }
-    unsigned mask = 0;
-    for (unsigned i = 0; i < 16; ++i) {
+    out->api = properties.apiVersion;
+    out->driver = properties.driverVersion;
+    unsigned rgb = physical_rgb_mask(physical);
+    for (unsigned i = 0; i < HYBRIS_BC_FORMATS; ++i) {
         VkFormat format = VK_FORMAT_BC1_RGB_UNORM_BLOCK + i;
-        /* D3D uses BC1 RGBA. Keep RGB-only BC1 unadvertised until absent-alpha
-         * border semantics are implemented on devices without RGB8 images. */
-        if (dxvk_profile() && i < 2) continue;
-        VkFormat backing = hybris_bc_image_format(format, physical_rgb_mask(physical));
-        if (backing == VK_FORMAT_UNDEFINED) continue;
-        VkFormatProperties native, decoded;
+        VkFormat backing = hybris_bc_image_format(format, rgb);
+        VkFormatProperties native, decoded = {0};
         GetPhysicalDeviceFormatProperties(physical, format, &native);
-        if (policy == 1 && (native.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) continue;
-        GetPhysicalDeviceFormatProperties(physical, backing, &decoded);
-        VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
-            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
-        if ((decoded.optimalTilingFeatures & required) == required) mask |= 1u << i;
+        if (backing != VK_FORMAT_UNDEFINED) GetPhysicalDeviceFormatProperties(physical, backing, &decoded);
+        out->native[i] = native.optimalTilingFeatures;
+        out->backing[i] = decoded.optimalTilingFeatures;
+    }
+    return 1;
+}
+/* Emulated-format mask, and in *feature whether textureCompressionBC may be
+ * reported, from one probe. Logs the decision once per process. */
+static unsigned physical_state(VkPhysicalDevice physical, int *feature)
+{
+    static int reported;
+    struct bc_probe seen;
+    const struct hybris_bc_traits *rules = traits();
+    if (feature) *feature = 0;
+    if (!probe(physical, &seen)) return 0;
+    unsigned mask = hybris_bc_select(rules, seen.native, seen.backing);
+    int complete = hybris_bc_feature_complete(rules, mask, seen.native, seen.backing);
+    if (feature) *feature = complete;
+    if (!__atomic_exchange_n(&reported, 1, __ATOMIC_RELAXED)) {
+        uint32_t lacking;
+        unsigned gap = hybris_bc_first_gap(rules, mask, seen.native, seen.backing, &lacking);
+        fprintf(stderr, "HYBRIS_BC policy=%d api=0x%x driver=0x%x emulated=0x%x texture_compression=%d"
+            " gap_format=%u lacking=0x%x\n", policy, seen.api, seen.driver, mask, complete,
+            gap < HYBRIS_BC_FORMATS ? (unsigned)VK_FORMAT_BC1_RGB_UNORM_BLOCK + gap : 0u, lacking);
     }
     return mask;
 }
+unsigned hybris_bc_physical_mask(VkPhysicalDevice physical)
+{
+    return physical_state(physical, NULL);
+}
+int hybris_bc_texture_compression(VkPhysicalDevice physical)
+{
+    int feature;
+    physical_state(physical, &feature);
+    return feature;
+}
+/* True when the layer reports textureCompressionBC and the driver does not: the
+ * device-create path must then clear the bit before the driver sees it. */
+int hybris_bc_strip_texture_compression(VkPhysicalDevice physical)
+{
+    struct hybris_layer_physical context;
+    if (!hybris_bc_texture_compression(physical) || !hybris_layer_lookup_physical(physical, &context)) return 0;
+    PHYSICAL_PROC(GetPhysicalDeviceFeatures);
+    VkPhysicalDeviceFeatures native;
+    GetPhysicalDeviceFeatures(physical, &native);
+    return !native.textureCompressionBC;
+}
 /* The emulation profile implements Vulkan 1.3 plus maintenance6. Do not expose
  * 1.4 core host-copy/push-descriptor operations that bypass our state tracking. */
 static void cap_properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties *out)
 {
-    if (dxvk_profile() && hybris_bc_physical_mask(physical) && out->apiVersion > VK_API_VERSION_1_3)
+    if (traits()->profile13 && hybris_bc_physical_mask(physical) && out->apiVersion > VK_API_VERSION_1_3)
         out->apiVersion = VK_API_VERSION_1_3;
 }
 static void VKAPI_CALL properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties *out)
@@ -134,9 +194,8 @@ int hybris_bc_format_properties(VkPhysicalDevice physical, VkFormat format, VkFo
     PHYSICAL_PROC(GetPhysicalDeviceFormatProperties);
     VkFormatProperties decoded;
     GetPhysicalDeviceFormatProperties(physical, hybris_bc_image_format(format, physical_rgb_mask(physical)), &decoded);
-    *properties = (VkFormatProperties){.optimalTilingFeatures = decoded.optimalTilingFeatures &
-        (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
-         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)};
+    /* The application's image is the decoded one, so a blit reads it directly. */
+    *properties = (VkFormatProperties){.optimalTilingFeatures = hybris_bc_emulated_features(decoded.optimalTilingFeatures)};
     return 1;
 }
 int hybris_bc_extension_allowed(const char *name)
@@ -150,7 +209,7 @@ int hybris_bc_extension_allowed(const char *name)
     };
     for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i)
         if (!strcmp(name, unsupported[i])) {
-            if (dxvk_profile() && !strcmp(name, "VK_KHR_maintenance6")) continue;
+            if (traits()->profile13 && !strcmp(name, "VK_KHR_maintenance6")) continue;
             return 0;
         }
     return 1;
@@ -162,9 +221,9 @@ static unsigned unsupported_feature_count(VkStructureType type)
     switch (type) {
     case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT: return 4;
     case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES:
-        return dxvk_profile() ? (offsetof(VkPhysicalDeviceVulkan14Features, pushDescriptor) -
+        return traits()->profile13 ? (offsetof(VkPhysicalDeviceVulkan14Features, pushDescriptor) -
             offsetof(VkPhysicalDeviceVulkan14Features, globalPriorityQuery)) / sizeof(VkBool32) + 1 : 0;
-    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR: return dxvk_profile() ? 0 : 1;
+    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR: return traits()->profile13 ? 0 : 1;
     case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PER_STAGE_DESCRIPTOR_SET_FEATURES_NV: return 2;
     case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT:
     case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT:
@@ -180,10 +239,13 @@ static unsigned unsupported_feature_count(VkStructureType type)
 }
 VkResult hybris_bc_prepare_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info)
 {
-    if (!hybris_bc_physical_mask(physical)) return VK_SUCCESS;
+    int feature;
+    if (!physical_state(physical, &feature)) return VK_SUCCESS;
     for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
         if (!hybris_bc_extension_allowed(info->ppEnabledExtensionNames[i])) return VK_ERROR_EXTENSION_NOT_PRESENT;
-    if (info->pEnabledFeatures && (info->pEnabledFeatures->pipelineStatisticsQuery || info->pEnabledFeatures->textureCompressionBC)) return VK_ERROR_FEATURE_NOT_PRESENT;
+    /* textureCompressionBC may be enabled exactly when the layer reports it. */
+    if (info->pEnabledFeatures && (info->pEnabledFeatures->pipelineStatisticsQuery ||
+        (info->pEnabledFeatures->textureCompressionBC && !feature))) return VK_ERROR_FEATURE_NOT_PRESENT;
     for (const VkBaseInStructure *node = info->pNext; node; node = node->pNext) {
         unsigned count = unsupported_feature_count(node->sType);
         for (unsigned i = 0; i < count; ++i) {
@@ -195,7 +257,7 @@ VkResult hybris_bc_prepare_device(VkPhysicalDevice physical, const VkDeviceCreat
             ((const VkDeviceGroupDeviceCreateInfo *)node)->physicalDeviceCount != 1) return VK_ERROR_FEATURE_NOT_PRESENT;
         if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 &&
             (((const VkPhysicalDeviceFeatures2 *)node)->features.pipelineStatisticsQuery ||
-             ((const VkPhysicalDeviceFeatures2 *)node)->features.textureCompressionBC)) return VK_ERROR_FEATURE_NOT_PRESENT;
+             (((const VkPhysicalDeviceFeatures2 *)node)->features.textureCompressionBC && !feature))) return VK_ERROR_FEATURE_NOT_PRESENT;
         if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES &&
             ((const VkPhysicalDeviceVulkan11Features *)node)->protectedMemory) return VK_ERROR_FEATURE_NOT_PRESENT;
         if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES &&
@@ -241,13 +303,24 @@ static void VKAPI_CALL queue_properties2_core(VkPhysicalDevice physical, uint32_
 { queue_properties2(physical, count, out, "vkGetPhysicalDeviceQueueFamilyProperties2"); }
 static void VKAPI_CALL queue_properties2_khr(VkPhysicalDevice physical, uint32_t *count, VkQueueFamilyProperties2 *out)
 { queue_properties2(physical, count, out, "vkGetPhysicalDeviceQueueFamilyProperties2KHR"); }
+/* Hide what emulation cannot honor. textureCompressionBC is raised only when the
+ * policy's formats are complete, and lowered while emulating anything else. */
+static unsigned filter_features(VkPhysicalDevice physical, VkPhysicalDeviceFeatures *out)
+{
+    int feature;
+    unsigned mask = physical_state(physical, &feature);
+    if (mask) out->pipelineStatisticsQuery = VK_FALSE;
+    if (feature) out->textureCompressionBC = VK_TRUE;
+    else if (mask) out->textureCompressionBC = VK_FALSE;
+    return mask;
+}
 static void VKAPI_CALL features(VkPhysicalDevice physical, VkPhysicalDeviceFeatures *out)
 {
     struct hybris_layer_physical context;
     if (!hybris_layer_lookup_physical(physical, &context)) return;
     PHYSICAL_PROC(GetPhysicalDeviceFeatures);
     GetPhysicalDeviceFeatures(physical, out);
-    if (hybris_bc_physical_mask(physical)) out->pipelineStatisticsQuery = out->textureCompressionBC = VK_FALSE;
+    filter_features(physical, out);
 }
 static void features2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out, const char *name)
 {
@@ -255,8 +328,7 @@ static void features2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out,
     if (!hybris_layer_lookup_physical(physical, &context)) return;
     PFN_vkGetPhysicalDeviceFeatures2 query = (PFN_vkGetPhysicalDeviceFeatures2)context.resolver(context.instance, name);
     query(physical, out);
-    if (!hybris_bc_physical_mask(physical)) return;
-    out->features.pipelineStatisticsQuery = out->features.textureCompressionBC = VK_FALSE;
+    if (!filter_features(physical, &out->features)) return;
     for (VkBaseOutStructure *node = out->pNext; node; node = node->pNext) {
         unsigned count = unsupported_feature_count(node->sType);
         if (count) memset((char *)node + sizeof(VkBaseOutStructure), 0, count * sizeof(VkBool32));
diff --git a/hybris/vulkan/compat/bc_policy.h b/hybris/vulkan/compat/bc_policy.h
index a2aff46..784a9db 100644
--- a/hybris/vulkan/compat/bc_policy.h
+++ b/hybris/vulkan/compat/bc_policy.h
@@ -4,6 +4,8 @@
 #include <vulkan/vulkan.h>
 int hybris_bc_enabled(void);
 unsigned hybris_bc_physical_mask(VkPhysicalDevice physical);
+int hybris_bc_texture_compression(VkPhysicalDevice physical);
+int hybris_bc_strip_texture_compression(VkPhysicalDevice physical);
 int hybris_bc_format_properties(VkPhysicalDevice physical, VkFormat format, VkFormatProperties *properties);
 int hybris_bc_extension_allowed(const char *name);
 VkResult hybris_bc_prepare_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info);
diff --git a/hybris/vulkan/compat/bc_rules.c b/hybris/vulkan/compat/bc_rules.c
new file mode 100644
index 0000000..e361f6f
--- /dev/null
+++ b/hybris/vulkan/compat/bc_rules.c
@@ -0,0 +1,90 @@
+/* SPDX-License-Identifier: Apache-2.0 */
+#include "bc_rules.h"
+#include <string.h>
+
+/* One row per policy; this table is the only place policies differ.
+ * missing: emulate formats lacking native sampling; no Vulkan 1.4.
+ * force:   emulate every BC format; no Vulkan 1.4.
+ * dxvk:    force + Direct3D set only + Vulkan 1.4 presented as 1.3.
+ * d3d:     dxvk's profile, but a native format stays native when it already
+ *          has every feature textureCompressionBC needs. */
+static const struct hybris_bc_traits table[HYBRIS_BC_POLICIES] = {
+    [HYBRIS_BC_OFF] = {0},
+    [HYBRIS_BC_MISSING] = {.enabled = 1, .native_first = 1, .native_keeps = HYBRIS_BC_SAMPLED},
+    [HYBRIS_BC_FORCE] = {.enabled = 1},
+    [HYBRIS_BC_DXVK] = {.enabled = 1, .profile13 = 1, .skip = HYBRIS_BC_BC1_RGB_FORMATS,
+        .feature_formats = HYBRIS_BC_D3D_FORMATS},
+    [HYBRIS_BC_D3D] = {.enabled = 1, .profile13 = 1, .native_first = 1, .native_keeps = HYBRIS_BC_REQUIRED,
+        .skip = HYBRIS_BC_BC1_RGB_FORMATS, .feature_formats = HYBRIS_BC_D3D_FORMATS},
+};
+static const uint32_t no_features[HYBRIS_BC_FORMATS];
+
+int hybris_bc_parse_policy(const char *text)
+{
+    static const struct { const char *text; int policy; } names[] = {
+        {"missing", HYBRIS_BC_MISSING}, {"1", HYBRIS_BC_MISSING}, {"force", HYBRIS_BC_FORCE},
+        {"dxvk", HYBRIS_BC_DXVK}, {"d3d", HYBRIS_BC_D3D}};
+    if (!text) return HYBRIS_BC_MISSING;
+    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
+        if (!strcmp(text, names[i].text)) return names[i].policy;
+    return HYBRIS_BC_OFF;
+}
+
+const struct hybris_bc_traits *hybris_bc_traits(int policy)
+{
+    return &table[policy > 0 && policy < HYBRIS_BC_POLICIES ? policy : HYBRIS_BC_OFF];
+}
+
+int hybris_bc_api_allowed(const struct hybris_bc_traits *traits, uint32_t device_api)
+{
+    return traits->profile13 || device_api < HYBRIS_BC_API_1_4;
+}
+
+uint32_t hybris_bc_emulated_features(uint32_t backing)
+{
+    return backing & (HYBRIS_BC_SAMPLED | HYBRIS_BC_LINEAR | HYBRIS_BC_BLIT_SRC | HYBRIS_BC_TRANSFER);
+}
+
+unsigned hybris_bc_select(const struct hybris_bc_traits *traits,
+    const uint32_t native[HYBRIS_BC_FORMATS], const uint32_t backing[HYBRIS_BC_FORMATS])
+{
+    /* The decoded backing must itself be sampleable and copyable. */
+    const uint32_t need = HYBRIS_BC_SAMPLED | HYBRIS_BC_TRANSFER;
+    if (!traits->enabled) return 0;
+    if (!native) native = no_features;
+    if (!backing) backing = no_features;
+    unsigned mask = 0;
+    for (unsigned i = 0; i < HYBRIS_BC_FORMATS; ++i) {
+        if ((traits->skip >> i) & 1) continue;
+        if ((backing[i] & need) != need) continue;
+        if (traits->native_first && (native[i] & traits->native_keeps) == traits->native_keeps) continue;
+        mask |= 1u << i;
+    }
+    return mask;
+}
+
+unsigned hybris_bc_first_gap(const struct hybris_bc_traits *traits, unsigned mask,
+    const uint32_t native[HYBRIS_BC_FORMATS], const uint32_t backing[HYBRIS_BC_FORMATS], uint32_t *lacking)
+{
+    uint32_t sink;
+    if (!lacking) lacking = &sink;
+    if (!native) native = no_features;
+    if (!backing) backing = no_features;
+    *lacking = 0;
+    for (unsigned i = 0; i < HYBRIS_BC_FORMATS; ++i) {
+        if (!((traits->feature_formats >> i) & 1)) continue;
+        uint32_t have = ((mask >> i) & 1) ? hybris_bc_emulated_features(backing[i]) : native[i];
+        if ((have & HYBRIS_BC_REQUIRED) != HYBRIS_BC_REQUIRED) {
+            *lacking = HYBRIS_BC_REQUIRED & ~have;
+            return i;
+        }
+    }
+    return HYBRIS_BC_FORMATS;
+}
+
+int hybris_bc_feature_complete(const struct hybris_bc_traits *traits, unsigned mask,
+    const uint32_t native[HYBRIS_BC_FORMATS], const uint32_t backing[HYBRIS_BC_FORMATS])
+{
+    return traits->enabled && traits->feature_formats &&
+        hybris_bc_first_gap(traits, mask, native, backing, NULL) == HYBRIS_BC_FORMATS;
+}
diff --git a/hybris/vulkan/compat/bc_rules.h b/hybris/vulkan/compat/bc_rules.h
new file mode 100644
index 0000000..0337331
--- /dev/null
+++ b/hybris/vulkan/compat/bc_rules.h
@@ -0,0 +1,63 @@
+/* SPDX-License-Identifier: Apache-2.0 */
+#ifndef HYBRIS_BC_RULES_H
+#define HYBRIS_BC_RULES_H
+#include <stdint.h>
+
+/* Pure BC-emulation rules: no Vulkan headers, calls or global state, so every
+ * decision below is host-testable. bc_policy.c supplies the driver's answers
+ * and converts the results back to Vulkan types.
+ *
+ * Rail contract: HYBRIS_BC_TEXTURES text maps to a policy (NULL selects
+ * "missing", unknown text selects off); an unknown policy number resolves to
+ * the off traits; feature words are used as given; format arrays hold exactly
+ * HYBRIS_BC_FORMATS entries indexed by VkFormat - VK_FORMAT_BC1_RGB_UNORM_BLOCK
+ * and a NULL array reads as "no features". No events, no allocation. */
+enum { HYBRIS_BC_OFF, HYBRIS_BC_MISSING, HYBRIS_BC_FORCE, HYBRIS_BC_DXVK, HYBRIS_BC_D3D, HYBRIS_BC_POLICIES };
+#define HYBRIS_BC_FORMATS 16u
+/* Index set 2..15: the Direct3D formats, i.e. every BC format except BC1 RGB. */
+#define HYBRIS_BC_D3D_FORMATS 0xFFFCu
+#define HYBRIS_BC_BC1_RGB_FORMATS 0x0003u
+/* Values equal VK_FORMAT_FEATURE_*_BIT; bc_policy.c asserts that at build time. */
+#define HYBRIS_BC_SAMPLED 0x00000001u
+#define HYBRIS_BC_BLIT_SRC 0x00000400u
+#define HYBRIS_BC_LINEAR 0x00001000u
+#define HYBRIS_BC_TRANSFER_SRC 0x00004000u
+#define HYBRIS_BC_TRANSFER_DST 0x00008000u
+#define HYBRIS_BC_TRANSFER (HYBRIS_BC_TRANSFER_SRC | HYBRIS_BC_TRANSFER_DST)
+/* Optimal-tiling features each BC format needs before textureCompressionBC
+ * can be reported (Vulkan: sampled, blit source, linear filter; transfer
+ * source/destination follow from sampled for Vulkan 1.1 and newer). */
+#define HYBRIS_BC_REQUIRED (HYBRIS_BC_SAMPLED | HYBRIS_BC_LINEAR | HYBRIS_BC_BLIT_SRC | HYBRIS_BC_TRANSFER)
+/* VK_MAKE_API_VERSION(0, 1, 4, 0) */
+#define HYBRIS_BC_API_1_4 0x00404000u
+
+struct hybris_bc_traits {
+    int enabled;               /* the policy intercepts anything at all */
+    int profile13;             /* runs on Vulkan 1.4 devices, presented as 1.3 plus maintenance6 */
+    int native_first;          /* a native format that has native_keeps stays native */
+    uint32_t native_keeps;
+    unsigned skip;             /* formats that are never emulated */
+    unsigned feature_formats;  /* formats that must be complete to report textureCompressionBC; 0 = never report */
+};
+
+/* NULL -> missing, "missing"/"1" -> missing, "force", "dxvk", "d3d", anything else -> off. */
+int hybris_bc_parse_policy(const char *text);
+/* Total: an unknown policy resolves to the off traits. */
+const struct hybris_bc_traits *hybris_bc_traits(int policy);
+/* Device-level gate: 1.4 and newer devices need a profile13 policy. */
+int hybris_bc_api_allowed(const struct hybris_bc_traits *traits, uint32_t device_api);
+/* Bit i set = format i is emulated. native/backing are optimalTilingFeatures of
+ * the format and of its decoded backing format (0 when no backing exists). */
+unsigned hybris_bc_select(const struct hybris_bc_traits *traits,
+    const uint32_t native[HYBRIS_BC_FORMATS], const uint32_t backing[HYBRIS_BC_FORMATS]);
+/* Features an emulated format exposes, derived from its backing format. */
+uint32_t hybris_bc_emulated_features(uint32_t backing);
+/* First feature format (index) that lacks HYBRIS_BC_REQUIRED once emulation is
+ * applied, with the missing bits in *lacking (may be NULL);
+ * HYBRIS_BC_FORMATS means none. */
+unsigned hybris_bc_first_gap(const struct hybris_bc_traits *traits, unsigned mask,
+    const uint32_t native[HYBRIS_BC_FORMATS], const uint32_t backing[HYBRIS_BC_FORMATS], uint32_t *lacking);
+/* 1 only when the traits report the feature and no format has a gap. */
+int hybris_bc_feature_complete(const struct hybris_bc_traits *traits, unsigned mask,
+    const uint32_t native[HYBRIS_BC_FORMATS], const uint32_t backing[HYBRIS_BC_FORMATS]);
+#endif
diff --git a/hybris/vulkan/layer/device_features.c b/hybris/vulkan/layer/device_features.c
index b7cdfba..b18490b 100644
--- a/hybris/vulkan/layer/device_features.c
+++ b/hybris/vulkan/layer/device_features.c
@@ -2,6 +2,7 @@
 #define VK_NO_PROTOTYPES
 #include "device_features.h"
 #include "../compat/vertex_stores.h"
+#include "../compat/bc_policy.h"
 #include "../compat/clip_distance.h"
 #include "../compat/scaled_vertex.h"
 #include <vulkan/vk_layer.h>
@@ -36,11 +37,14 @@ VkResult hybris_device_features_prepare(VkPhysicalDevice physical,
     *out = (struct hybris_device_features){.info = *info};
     int clip = hybris_clip_active(physical);
     int stores = hybris_vertex_stores_active(physical);
-    if (!clip && !stores) return VK_SUCCESS;
+    /* Reported by the layer, not the driver: the driver must not see it enabled. */
+    int bc = hybris_bc_strip_texture_compression(physical);
+    if (!clip && !stores && !bc) return VK_SUCCESS;
     if (info->pEnabledFeatures) {
         out->features = *info->pEnabledFeatures;
         if (clip) out->features.shaderClipDistance = VK_FALSE;
         if (stores) out->features.vertexPipelineStoresAndAtomics = VK_FALSE;
+        if (bc) out->features.textureCompressionBC = VK_FALSE;
         out->info.pEnabledFeatures = &out->features;
     }
     const VkBaseInStructure *target = info->pNext;
@@ -48,7 +52,8 @@ VkResult hybris_device_features_prepare(VkPhysicalDevice physical,
         target = target->pNext;
     if (!target) return VK_SUCCESS;
     const VkPhysicalDeviceFeatures *requested = &((const VkPhysicalDeviceFeatures2 *)target)->features;
-    if (!(clip && requested->shaderClipDistance) && !(stores && requested->vertexPipelineStoresAndAtomics))
+    if (!(clip && requested->shaderClipDistance) && !(stores && requested->vertexPipelineStoresAndAtomics) &&
+        !(bc && requested->textureCompressionBC))
         return VK_SUCCESS;
 
     /* Only copy the prefix needed to replace Features2. Preserve every other
@@ -75,6 +80,7 @@ VkResult hybris_device_features_prepare(VkPhysicalDevice physical,
         if (node == target) {
             if (clip) ((VkPhysicalDeviceFeatures2 *)copy)->features.shaderClipDistance = VK_FALSE;
             if (stores) ((VkPhysicalDeviceFeatures2 *)copy)->features.vertexPipelineStoresAndAtomics = VK_FALSE;
+            if (bc) ((VkPhysicalDeviceFeatures2 *)copy)->features.textureCompressionBC = VK_FALSE;
             copy->pNext = (void *)out->suffix;
             break;
         }
~~~~

### Appendix C: `tools/tests/hybris_drm_properties_test.c`

~~~~c
#define VK_NO_PROTOTYPES
#include "compat/drm_properties.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fake driver: mode 0 ignores the DRM structure, 1 answers hasRender, 2 answers "no render node". */
static int hal_mode, hal_calls, node_calls;
static VkPhysicalDeviceDrmPropertiesEXT *drm_in(VkPhysicalDeviceProperties2 *out)
{
    for (VkBaseOutStructure *n = out->pNext; n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT) return (void *)n;
    return NULL;
}
static void VKAPI_CALL fake_hal(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out)
{
    (void)physical;
    ++hal_calls;
    out->properties.vendorID = 0x1010; out->properties.deviceID = 0x1234; out->properties.driverVersion = 0x19030000;
    VkPhysicalDeviceDrmPropertiesEXT *d = drm_in(out);
    if (!d || !hal_mode) return;
    if (hal_mode == 1) { d->hasPrimary = VK_FALSE; d->hasRender = VK_TRUE; d->renderMajor = 7; d->renderMinor = 9;
                         d->primaryMajor = d->primaryMinor = 0; }
    else { d->hasPrimary = d->hasRender = VK_FALSE; d->primaryMajor = d->primaryMinor = d->renderMajor = d->renderMinor = 0; }
}
static void VKAPI_CALL quiet_hal(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out)
{ (void)physical; (void)out; }
static struct hybris_drm_node node_226_128(void)
{ ++node_calls; return (struct hybris_drm_node){.valid = 1, .major = 226, .minor = 128}; }
static struct hybris_drm_node node_missing(void)
{ ++node_calls; return (struct hybris_drm_node){.error = ENOENT}; }
static VkPhysicalDeviceDrmPropertiesEXT fresh(void)
{ return (VkPhysicalDeviceDrmPropertiesEXT){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT}; }

int main(void)
{
    VkPhysicalDevice pd = (VkPhysicalDevice)(uintptr_t)1;
    const char *path = "drm_test_stderr.txt";
    assert(freopen(path, "w", stderr));

    /* 1. Driver ignores the structure: filled from the node, hasPrimary false, rest of chain intact. */
    VkPhysicalDeviceDrmPropertiesEXT drm = fresh();
    VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, .pNext = &drm};
    VkPhysicalDeviceProperties2 out = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
    hal_mode = 0;
    assert(hybris_drm_query(fake_hal, pd, &out, node_226_128) == HYBRIS_DRM_FILLED);
    assert(drm.hasRender == VK_TRUE && drm.renderMajor == 226 && drm.renderMinor == 128);
    assert(drm.hasPrimary == VK_FALSE && drm.primaryMajor == 0 && drm.primaryMinor == 0);
    assert(drm.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT && drm.pNext == NULL);
    assert(out.pNext == &id && id.pNext == &drm && out.properties.vendorID == 0x1010);
    assert(node_calls == 1);

    /* 2. Driver fills it: passed through, and the node is never stat'ed. */
    drm = fresh(); hal_mode = 1; node_calls = 0;
    assert(hybris_drm_query(fake_hal, pd, &out, node_226_128) == HYBRIS_DRM_DRIVER_ANSWERED);
    assert(drm.hasRender == VK_TRUE && drm.renderMajor == 7 && drm.renderMinor == 9);
    assert(node_calls == 0);

    /* 3. Driver explicitly answers "no render": still the driver's answer (design: pass through). */
    drm = fresh(); hal_mode = 2;
    assert(hybris_drm_query(fake_hal, pd, &out, node_226_128) == HYBRIS_DRM_DRIVER_ANSWERED);
    assert(drm.hasRender == VK_FALSE && drm.renderMajor == 0);
    assert(node_calls == 0);

    /* 4. Driver ignores it and there is no node: the application's own bytes come back. */
    drm = fresh(); hal_mode = 0;
    drm.hasRender = 0; drm.renderMajor = 42;                      /* application pre-initialised */
    VkPhysicalDeviceDrmPropertiesEXT before = drm;
    assert(hybris_drm_query(fake_hal, pd, &out, node_missing) == HYBRIS_DRM_NO_NODE);
    assert(!memcmp(&before, &drm, sizeof drm));
    assert(node_calls == 1);

    /* 5. No DRM structure in the chain: driver called once, node never stat'ed, nothing touched. */
    node_calls = 0;
    VkPhysicalDeviceIDProperties id2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 no_drm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id2};
    int calls = hal_calls;
    assert(hybris_drm_query(fake_hal, pd, &no_drm, node_226_128) == HYBRIS_DRM_NOT_REQUESTED);
    assert(hal_calls == calls + 1 && node_calls == 0);
    VkPhysicalDeviceProperties2 bare = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    assert(hybris_drm_query(fake_hal, pd, &bare, node_226_128) == HYBRIS_DRM_NOT_REQUESTED);

    /* 6. A cyclic chain without the DRM structure terminates (bounded walk). */
    VkPhysicalDeviceIDProperties loop = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    loop.pNext = &loop;
    VkPhysicalDeviceProperties2 cyc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &loop};
    assert(hybris_drm_query(quiet_hal, pd, &cyc, node_226_128) == HYBRIS_DRM_NOT_REQUESTED);

    /* 7. stat adapter: null, missing path and a directory are errors; a character device is a node. */
    struct hybris_drm_node n = hybris_drm_stat(NULL);
    assert(!n.valid && n.error == EINVAL);
    n = hybris_drm_stat("definitely/not/here");
    assert(!n.valid && n.error == ENOENT);
    n = hybris_drm_stat(".");
    assert(!n.valid && n.error == ENOTSUP);
#ifdef _WIN32
    n = hybris_drm_stat("NUL");
    assert(n.valid && n.error == 0);
#else
    n = hybris_drm_stat("/dev/null");
    assert(n.valid && n.major == 1 && n.minor == 3);
#endif

    /* 7b. The escape hatch collapses into the missing-node case. */
    n = hybris_drm_render_node();
    if (getenv("EXPECT_DISABLED")) assert(!n.valid && n.error == ECANCELED);
    else assert(n.valid || n.error != ECANCELED);   /* CI hosts have no renderD128: ENOENT is fine */

    /* 8. One diagnostic line per outcome per process (3 outcomes, each reported more than once). */
    fflush(stderr);
    FILE *log = fopen(path, "r");
    assert(log);
    char line[512];
    unsigned lines = 0, filled = 0, answered = 0, nonode = 0;
    while (fgets(line, sizeof line, log)) {
        ++lines;
        filled += !!strstr(line, "outcome=filled");
        answered += !!strstr(line, "outcome=driver-answered");
        nonode += !!strstr(line, "outcome=no-node");
    }
    fclose(log);
    assert(lines == 3 && filled == 1 && answered == 1 && nonode == 1);
    printf("drm_properties: all checks passed\n");
    return 0;
}
~~~~

### Appendix D: `tools/tests/hybris_bc_rules_check.c`

~~~~c
/* SPDX-License-Identifier: Apache-2.0 */
/* Host check for the pure BC rules (no Vulkan, no device):
 *   cc -std=c11 -Wall -Wextra -Werror tools/tests/hybris_bc_rules_check.c \
 *      <tree>/hybris/vulkan/compat/bc_rules.c -I <tree>/hybris/vulkan -o bc_rules_check && ./bc_rules_check */
#include "compat/bc_rules.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); } } while (0)

#define FULL HYBRIS_BC_REQUIRED
#define SAMPLED_ONLY HYBRIS_BC_SAMPLED
/* Extra feature bits an emulated format must never inherit from its backing. */
#define STORAGE 0x2u
#define COLOR_ATTACHMENT 0x80u

/* Pixel 10 shape: BC1 RGB..BC5 (0..11) fully native, BC6H/BC7 (12..15) absent. */
static void pixel10(uint32_t native[HYBRIS_BC_FORMATS], uint32_t backing[HYBRIS_BC_FORMATS])
{
    for (unsigned i = 0; i < HYBRIS_BC_FORMATS; ++i) {
        native[i] = i < 12 ? FULL : 0;
        backing[i] = FULL | STORAGE | COLOR_ATTACHMENT;
    }
}

static void parsing(void)
{
    CHECK(hybris_bc_parse_policy(NULL) == HYBRIS_BC_MISSING);
    CHECK(hybris_bc_parse_policy("missing") == HYBRIS_BC_MISSING);
    CHECK(hybris_bc_parse_policy("1") == HYBRIS_BC_MISSING);
    CHECK(hybris_bc_parse_policy("force") == HYBRIS_BC_FORCE);
    CHECK(hybris_bc_parse_policy("dxvk") == HYBRIS_BC_DXVK);
    CHECK(hybris_bc_parse_policy("d3d") == HYBRIS_BC_D3D);
    const char *off[] = {"0", "", "off", "D3D", "missing ", "forced", "2"};
    for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); ++i) CHECK(hybris_bc_parse_policy(off[i]) == HYBRIS_BC_OFF);
    CHECK(!hybris_bc_traits(HYBRIS_BC_OFF)->enabled);
    CHECK(!hybris_bc_traits(-1)->enabled);
    CHECK(!hybris_bc_traits(HYBRIS_BC_POLICIES)->enabled);
    CHECK(!hybris_bc_traits(1 << 20)->enabled);
    for (int p = HYBRIS_BC_MISSING; p < HYBRIS_BC_POLICIES; ++p) CHECK(hybris_bc_traits(p)->enabled);
}

static void api_gate(void)
{
    const uint32_t v13 = (1u << 22) | (3u << 12) | 296, v14 = HYBRIS_BC_API_1_4, v14_317 = v14 | 317;
    CHECK(hybris_bc_api_allowed(hybris_bc_traits(HYBRIS_BC_MISSING), v13));
    CHECK(!hybris_bc_api_allowed(hybris_bc_traits(HYBRIS_BC_MISSING), v14));
    CHECK(!hybris_bc_api_allowed(hybris_bc_traits(HYBRIS_BC_FORCE), v14_317));
    CHECK(hybris_bc_api_allowed(hybris_bc_traits(HYBRIS_BC_DXVK), v14_317));
    CHECK(hybris_bc_api_allowed(hybris_bc_traits(HYBRIS_BC_D3D), v14_317));
    CHECK(hybris_bc_api_allowed(hybris_bc_traits(HYBRIS_BC_D3D), v13));
}

static void selection(void)
{
    uint32_t native[HYBRIS_BC_FORMATS], backing[HYBRIS_BC_FORMATS];
    pixel10(native, backing);
    /* Only the formats the driver cannot sample are emulated. */
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_MISSING), native, backing) == 0xF000u);
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_D3D), native, backing) == 0xF000u);
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_FORCE), native, backing) == 0xFFFFu);
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_DXVK), native, backing) == 0xFFFCu);
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_OFF), native, backing) == 0);
    /* Native sampling without filtering/transfer: "missing" trusts it, "d3d" decodes it. */
    native[8] = native[9] = SAMPLED_ONLY;
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_MISSING), native, backing) == 0xF000u);
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_D3D), native, backing) == 0xF300u);
    /* BC1 RGB is never selected by the Direct3D policies, even when absent. */
    native[0] = native[1] = 0;
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_D3D), native, backing) == 0xF300u);
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_MISSING), native, backing) == 0xF003u);
    /* A backing format that cannot be sampled and copied vetoes emulation. */
    backing[12] = FULL & ~HYBRIS_BC_TRANSFER_DST;
    backing[13] = HYBRIS_BC_SAMPLED | HYBRIS_BC_TRANSFER;
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_D3D), native, backing) == 0xE300u);
    backing[14] = 0;
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_D3D), native, backing) == 0xA300u);
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_D3D), NULL, NULL) == 0);
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_D3D), native, NULL) == 0);
    /* No native answers at all: everything with a usable backing is emulated, BC1 RGB still skipped. */
    CHECK(hybris_bc_select(hybris_bc_traits(HYBRIS_BC_D3D), NULL, backing) == 0xAFFCu);
}

static void exposed_features(void)
{
    CHECK(hybris_bc_emulated_features(FULL) == FULL);
    CHECK(hybris_bc_emulated_features(FULL | STORAGE | COLOR_ATTACHMENT) == FULL);
    CHECK(hybris_bc_emulated_features(HYBRIS_BC_SAMPLED | HYBRIS_BC_TRANSFER) ==
        (HYBRIS_BC_SAMPLED | HYBRIS_BC_TRANSFER));
    CHECK(hybris_bc_emulated_features(0) == 0);
}

static void feature(void)
{
    uint32_t native[HYBRIS_BC_FORMATS], backing[HYBRIS_BC_FORMATS], lacking = 99;
    const struct hybris_bc_traits *d3d = hybris_bc_traits(HYBRIS_BC_D3D);
    pixel10(native, backing);
    unsigned mask = hybris_bc_select(d3d, native, backing);
    CHECK(hybris_bc_first_gap(d3d, mask, native, backing, &lacking) == HYBRIS_BC_FORMATS && lacking == 0);
    CHECK(hybris_bc_feature_complete(d3d, mask, native, backing));
    /* Without emulation BC6H/BC7 are the gap. */
    CHECK(hybris_bc_first_gap(d3d, 0, native, backing, &lacking) == 12 && lacking == FULL);
    CHECK(!hybris_bc_feature_complete(d3d, 0, native, backing));
    /* Emulated BC7 whose backing cannot blit or filter: not complete. */
    backing[14] = backing[15] = HYBRIS_BC_SAMPLED | HYBRIS_BC_TRANSFER;
    mask = hybris_bc_select(d3d, native, backing);
    CHECK(mask == 0xF000u);
    CHECK(hybris_bc_first_gap(d3d, mask, native, backing, &lacking) == 14 &&
        lacking == (HYBRIS_BC_LINEAR | HYBRIS_BC_BLIT_SRC));
    CHECK(!hybris_bc_feature_complete(d3d, mask, native, backing));
    /* Native BC4 missing blit: "d3d" decodes it and recovers; "missing" never reports. */
    pixel10(native, backing);
    native[8] = FULL & ~HYBRIS_BC_BLIT_SRC;
    mask = hybris_bc_select(d3d, native, backing);
    CHECK(mask == 0xF100u && hybris_bc_feature_complete(d3d, mask, native, backing));
    CHECK(!hybris_bc_feature_complete(hybris_bc_traits(HYBRIS_BC_MISSING), 0xF000u, native, backing));
    CHECK(!hybris_bc_feature_complete(hybris_bc_traits(HYBRIS_BC_FORCE), 0xFFFFu, native, backing));
    CHECK(!hybris_bc_feature_complete(hybris_bc_traits(HYBRIS_BC_OFF), 0xFFFFu, native, backing));
    /* BC1 RGB is outside the Direct3D set but inside Vulkan's: a spec-strict mask sees it. */
    pixel10(native, backing);
    native[0] = native[1] = 0;
    mask = hybris_bc_select(d3d, native, backing);
    CHECK(hybris_bc_feature_complete(d3d, mask, native, backing));
    struct hybris_bc_traits strict = *d3d;
    strict.feature_formats = 0xFFFFu;
    CHECK(!hybris_bc_feature_complete(&strict, mask, native, backing));
    CHECK(hybris_bc_first_gap(&strict, mask, native, backing, NULL) == 0);
    /* Forced emulation of everything (dxvk) is complete only through the backing formats. */
    pixel10(native, backing);
    const struct hybris_bc_traits *dxvk = hybris_bc_traits(HYBRIS_BC_DXVK);
    mask = hybris_bc_select(dxvk, native, backing);
    CHECK(mask == 0xFFFCu && hybris_bc_feature_complete(dxvk, mask, native, backing));
    backing[8] = FULL & ~HYBRIS_BC_LINEAR;
    CHECK(!hybris_bc_feature_complete(dxvk, mask, native, backing));
    CHECK(hybris_bc_first_gap(dxvk, mask, native, backing, &lacking) == 8 && lacking == HYBRIS_BC_LINEAR);
    CHECK(!hybris_bc_feature_complete(d3d, 0, NULL, NULL));
}

/* The pre-existing missing/force/dxvk selection, restated per format, must not change. */
static unsigned legacy_select(int policy, const uint32_t native[HYBRIS_BC_FORMATS], const uint32_t backing[HYBRIS_BC_FORMATS])
{
    const uint32_t required = HYBRIS_BC_SAMPLED | HYBRIS_BC_TRANSFER;
    unsigned mask = 0;
    for (unsigned i = 0; i < HYBRIS_BC_FORMATS; ++i) {
        if (policy == HYBRIS_BC_DXVK && i < 2) continue;
        if (policy == HYBRIS_BC_MISSING && (native[i] & HYBRIS_BC_SAMPLED)) continue;
        if ((backing[i] & required) == required) mask |= 1u << i;
    }
    return mask;
}

static void legacy_equivalence(void)
{
    uint32_t seed = 0x2545F491u, native[HYBRIS_BC_FORMATS], backing[HYBRIS_BC_FORMATS];
    for (unsigned round = 0; round < 20000; ++round) {
        for (unsigned i = 0; i < HYBRIS_BC_FORMATS; ++i) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            native[i] = seed & 0xFFFFu;
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            backing[i] = seed & 0xFFFFu;
        }
        for (int policy = HYBRIS_BC_MISSING; policy <= HYBRIS_BC_DXVK; ++policy)
            CHECK(hybris_bc_select(hybris_bc_traits(policy), native, backing) == legacy_select(policy, native, backing));
    }
}

int main(void)
{
    parsing();
    api_gate();
    selection();
    exposed_features();
    feature();
    legacy_equivalence();
    printf("BC_RULES_CHECK checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
~~~~

### Appendix E: `tools/tests/hybris_bc_policy_fake.c`

~~~~c
/* SPDX-License-Identifier: Apache-2.0 */
/* Host check of bc_policy.c against a fake Vulkan 1.4 driver (no device, no loader).
 * The policy is read once per process, so run it once per policy:
 *   for p in unset missing force dxvk d3d 0; do ... HYBRIS_BC_TEXTURES=$p ./bc_policy_fake $p; done
 * (unset: leave the variable out). Build:
 *   cc -std=gnu11 -Wall -Wextra tools/tests/hybris_bc_policy_fake.c hybris/vulkan/compat/bc_policy.c \
 *      hybris/vulkan/compat/bc_rules.c hybris/vulkan/compat/bc_image.c -pthread -o bc_policy_fake */
#define VK_NO_PROTOTYPES
#include "compat/bc_policy.h"
#include "compat/bc_context.h"
#include "layer/layer.h"
#include <stdio.h>
#include <string.h>

#define FULL (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | \
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
    VK_FORMAT_FEATURE_TRANSFER_DST_BIT)
static unsigned checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); } } while (0)

/* The fake driver: Pixel 10 shape. BC1 RGB..BC5 native, BC6H/BC7 absent. */
static VkFormatFeatureFlags bc_native[16], decoded_features;
static VkFormatFeatureFlags rgba16f_features, rgba8_features;
static uint32_t device_api;
static VkBool32 native_texture_compression;
static VkQueueFamilyProperties family;

static void reset_driver(void)
{
    for (unsigned i = 0; i < 16; ++i) bc_native[i] = i < 12 ? FULL : 0;
    rgba8_features = rgba16f_features = decoded_features = FULL | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    device_api = VK_MAKE_API_VERSION(0, 1, 4, 317);
    native_texture_compression = VK_FALSE;
    family = (VkQueueFamilyProperties){.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
        VK_QUEUE_TRANSFER_BIT, .queueCount = 1, .minImageTransferGranularity = {1, 1, 1}};
}
static void fake_properties(VkPhysicalDevice p, VkPhysicalDeviceProperties *out)
{
    (void)p;
    memset(out, 0, sizeof(*out));
    out->apiVersion = device_api;
    out->driverVersion = 0x1903;
    out->limits.maxStorageBufferRange = 0x7fffffffu;
    out->limits.minStorageBufferOffsetAlignment = 64;
}
static void fake_queues(VkPhysicalDevice p, uint32_t *count, VkQueueFamilyProperties *out)
{
    (void)p;
    if (out && *count) out[0] = family;
    *count = 1;
}
static void fake_format(VkPhysicalDevice p, VkFormat format, VkFormatProperties *out)
{
    (void)p;
    memset(out, 0, sizeof(*out));
    if (format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK)
        out->optimalTilingFeatures = bc_native[format - VK_FORMAT_BC1_RGB_UNORM_BLOCK];
    else if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB) out->optimalTilingFeatures = rgba8_features;
    else if (format == VK_FORMAT_R16G16B16A16_SFLOAT) out->optimalTilingFeatures = rgba16f_features;
    else if (format == VK_FORMAT_R16_UNORM || format == VK_FORMAT_R16_SNORM || format == VK_FORMAT_R16G16_UNORM ||
        format == VK_FORMAT_R16G16_SNORM) out->optimalTilingFeatures = decoded_features;
}
static void fake_features(VkPhysicalDevice p, VkPhysicalDeviceFeatures *out)
{
    (void)p;
    memset(out, 0, sizeof(*out));
    out->textureCompressionBC = native_texture_compression;
    out->pipelineStatisticsQuery = VK_TRUE;
}
static void fake_features2(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *out)
{ fake_features(p, &out->features); }
static PFN_vkVoidFunction VKAPI_CALL fake_resolver(VkInstance instance, const char *name)
{
    (void)instance;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties")) return (PFN_vkVoidFunction)fake_properties;
    if (!strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties")) return (PFN_vkVoidFunction)fake_queues;
    if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties")) return (PFN_vkVoidFunction)fake_format;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures")) return (PFN_vkVoidFunction)fake_features;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2")) return (PFN_vkVoidFunction)fake_features2;
    return NULL;
}
/* Stubs for everything outside bc_policy.c / bc_rules.c / bc_image.c. */
int hybris_layer_lookup_physical(VkPhysicalDevice physical, struct hybris_layer_physical *out)
{
    (void)physical;
    *out = (struct hybris_layer_physical){.instance = (VkInstance)(uintptr_t)1, .generation = 1,
        .api_version = VK_API_VERSION_1_3, .resolver = fake_resolver};
    return 1;
}
VkResult hybris_bc_device_add(VkDevice h, PFN_vkGetDeviceProcAddr r, const VkPhysicalDeviceMemoryProperties *m,
    const VkPhysicalDeviceProperties *p, unsigned f, unsigned g, const VkAllocationCallbacks *a)
{ (void)h; (void)r; (void)m; (void)p; (void)f; (void)g; (void)a; return VK_SUCCESS; }
PFN_vkVoidFunction hybris_bc_commands_proc(const char *n) { (void)n; return NULL; }
PFN_vkVoidFunction hybris_bc_resources_proc(const char *n) { (void)n; return NULL; }
PFN_vkVoidFunction hybris_bc_record_proc(const char *n) { (void)n; return NULL; }
PFN_vkVoidFunction hybris_bc_barriers_proc(const char *n) { (void)n; return NULL; }
PFN_vkVoidFunction hybris_bc_image_copy_proc(const char *n) { (void)n; return NULL; }

static const VkPhysicalDevice physical = (VkPhysicalDevice)(uintptr_t)8;
static VkBool32 reported_bc(void)
{
    PFN_vkGetPhysicalDeviceFeatures2 query =
        (PFN_vkGetPhysicalDeviceFeatures2)hybris_bc_policy_proc("vkGetPhysicalDeviceFeatures2");
    VkPhysicalDeviceFeatures2 out = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if (!query) return native_texture_compression;
    query(physical, &out);
    return out.features.textureCompressionBC;
}
static VkResult create_with_bc(void)
{
    VkPhysicalDeviceFeatures enabled = {.textureCompressionBC = VK_TRUE};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pEnabledFeatures = &enabled};
    return hybris_bc_prepare_device(physical, &info);
}
static uint32_t reported_api(void)
{
    PFN_vkGetPhysicalDeviceProperties query =
        (PFN_vkGetPhysicalDeviceProperties)hybris_bc_policy_proc("vkGetPhysicalDeviceProperties");
    VkPhysicalDeviceProperties out;
    if (!query) { fake_properties(physical, &out); return out.apiVersion; }
    query(physical, &out);
    return out.apiVersion;
}

static void pixel10_d3d(unsigned expected_mask)   /* d3d and dxvk profiles on the Pixel 10 shape */
{
    reset_driver();
    CHECK(hybris_bc_physical_mask(physical) == expected_mask);
    CHECK(hybris_bc_texture_compression(physical) == 1);
    CHECK(reported_bc() == VK_TRUE);
    CHECK(reported_api() == VK_API_VERSION_1_3);
    CHECK(create_with_bc() == VK_SUCCESS);
    CHECK(hybris_bc_strip_texture_compression(physical) == 1);
    VkFormatProperties bc7;
    CHECK(hybris_bc_format_properties(physical, VK_FORMAT_BC7_UNORM_BLOCK, &bc7) == 1 && bc7.optimalTilingFeatures == FULL &&
        !bc7.linearTilingFeatures && !bc7.bufferFeatures);
    CHECK(hybris_bc_extension_allowed("VK_KHR_maintenance6") && !hybris_bc_extension_allowed("VK_KHR_push_descriptor"));
    /* Emulated BC7 backing without BLIT_SRC: the claim must be withdrawn and enabling refused. */
    rgba8_features = FULL & ~VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    CHECK(hybris_bc_texture_compression(physical) == 0 && reported_bc() == VK_FALSE);
    CHECK(create_with_bc() == VK_ERROR_FEATURE_NOT_PRESENT && hybris_bc_strip_texture_compression(physical) == 0);
    /* A driver that already reports the feature keeps it and sees it enabled. */
    reset_driver();
    native_texture_compression = VK_TRUE;
    CHECK(reported_bc() == VK_TRUE && hybris_bc_strip_texture_compression(physical) == 0);
    /* Compute-less queue family: emulation cannot run, nothing is touched. */
    reset_driver();
    family.queueFlags = VK_QUEUE_TRANSFER_BIT;
    CHECK(hybris_bc_physical_mask(physical) == 0 && reported_bc() == VK_FALSE && reported_api() == device_api);
}
static void check_policy(const char *name)
{
    reset_driver();
    if (!strcmp(name, "d3d")) {
        CHECK(hybris_bc_enabled());
        pixel10_d3d(0xF000u);
        /* Natively sampleable but incomplete BC4: d3d decodes it, so the claim survives. */
        reset_driver();
        bc_native[8] = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        CHECK(hybris_bc_physical_mask(physical) == 0xF100u && hybris_bc_texture_compression(physical) == 1);
    } else if (!strcmp(name, "dxvk")) {
        pixel10_d3d(0xFFFCu);
    } else if (!strcmp(name, "missing") || !strcmp(name, "unset") || !strcmp(name, "force")) {
        /* Unchanged behavior: Vulkan 1.4 stays excluded, and the feature is never raised. */
        CHECK(hybris_bc_enabled() && hybris_bc_physical_mask(physical) == 0);
        CHECK(hybris_bc_texture_compression(physical) == 0 && reported_bc() == VK_FALSE);
        CHECK(reported_api() == device_api && hybris_bc_strip_texture_compression(physical) == 0);
        device_api = VK_API_VERSION_1_3;
        const unsigned want = !strcmp(name, "force") ? 0xFFFFu : 0xF000u;
        CHECK(hybris_bc_physical_mask(physical) == want);
        CHECK(hybris_bc_texture_compression(physical) == 0 && reported_bc() == VK_FALSE);
        CHECK(create_with_bc() == VK_ERROR_FEATURE_NOT_PRESENT);
    } else {
        CHECK(!hybris_bc_enabled() && hybris_bc_physical_mask(physical) == 0);
        CHECK(!hybris_bc_policy_proc("vkGetPhysicalDeviceFeatures2"));
    }
}

int main(int argc, char **argv)
{
    check_policy(argc > 1 ? argv[1] : "unset");
    printf("BC_POLICY_FAKE policy=%s checks=%u failures=%u\n", argc > 1 ? argv[1] : "unset", checks, failures);
    return failures ? 1 : 0;
}
~~~~

### Appendix F: `tools/pixel10/pvr_props_probe.c`

~~~~c
/* pvr_props_probe.c - what the phone's Vulkan stack says about its PowerVR GPU, asked twice:
 *   loader  Android's own libvulkan.so (what an Android app sees)
 *   hal     the vendor HAL called directly through HMI / open("vk0") (what libhybris sees)
 * Both go through the one probe_source() chain; they differ only in where the entry points come from.
 *
 * Run as the adb shell user from /data/local/tmp. No app, no root, no link-time Vulkan dependency.
 *   usage: pvr_props_probe [HAL_PATH]        default /vendor/lib64/hw/vulkan.powervr.so
 * Output: one "scope.key=value" fact per line on stdout. Pipe it to a file and diff runs.
 *
 * Rail contract: strings from the driver are sanitized and length-capped before printing; every
 * enumeration is bounded (MAX_EXT, MAX_PD, MAX_NODES); a missing entry point or a failing call is
 * a printed fact, never a crash. Each section runs in a forked child with a 60 s alarm, so a driver
 * crash or hang costs that section only and is reported as "section.<name>=killed signal=N".
 * Exit status: 0 when every section ran to its end, 1 when one could not run (no libvulkan.so, no HAL,
 * no instance) or died. What the facts say never changes the status.
 *
 * ABI: hw_module_t / hw_device_t / hwvulkan_device_t are LP64 copies of AOSP hardware.h and
 * hwvulkan.h (the NDK ships neither); static asserts pin the offsets the 25.3 HAL was measured at.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#ifdef __ANDROID__
#include <android/dlext.h>
#endif

#define DEFAULT_HAL "/vendor/lib64/hw/vulkan.powervr.so"
#define MAX_EXT 1024
#define MAX_PD 4
#define MAX_NODES 64
#define SECTION_TIMEOUT_S 60

#include "hwvulkan_abi.h"

/* ---- output (the rail for everything the driver can hand us) ------------------------------ */
static void P(const char *scope, const char *key, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void P(const char *scope, const char *key, const char *fmt, ...) {
    va_list ap;
    printf("%s.%s=", scope, key);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}
/* Driver text: printable ASCII only, at most `max` bytes, NUL-terminated by us, never by trust. */
static void Pstr(const char *scope, const char *key, const char *s, size_t max) {
    char out[300];
    size_t n = 0;
    if (!s) s = "(null)";
    if (max >= sizeof out) max = sizeof out - 1;
    for (; n < max && s[n]; n++) out[n] = (s[n] >= 0x20 && s[n] < 0x7f) ? s[n] : '?';
    out[n] = 0;
    P(scope, key, "%s", out);
}

/* ---- pure helpers (host-testable: tools/tests/pvr_props_probe_test.c includes this file) --- */
#define DRM_POISON 0xCD

/* Imagination's driverVersion is the DDK build (changelist) number: 6908880 for DDK "25.3@6908880".
 * The Vulkan version macros decode the same number as 1.662.3024, which is what Android and
 * gpuinfo.org print. The DDK release ("25.3") exists only in driverInfo. */
static void fmt_driver_version(uint32_t v, char *out, size_t n) {
    snprintf(out, n, "%u (0x%x) vk_decoded=%u.%u.%u", v, v, v >> 22, (v >> 12) & 0x3ffu, v & 0xfffu);
}

/* "25.3@6908880" -> release "25.3", build 6908880. Returns 0 when the string is not that shape. */
static int parse_driver_info(const char *info, char *release, size_t n, uint32_t *build) {
    const char *at = info ? strchr(info, '@') : NULL;
    if (!at || at == info || (size_t)(at - info) >= n || at[1] < '0' || at[1] > '9') return 0;
    uint64_t v = 0;
    const char *p = at + 1;
    for (; *p >= '0' && *p <= '9'; p++) {
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > UINT32_MAX) return 0;
    }
    memcpy(release, info, (size_t)(at - info));
    release[at - info] = 0;
    *build = (uint32_t)v;
    return 1;
}

enum drm_state { DRM_UNTOUCHED, DRM_ZEROED, DRM_FILLED, DRM_PARTIAL };
static const char *const DRM_STATE_NAME[] = {"untouched", "zeroed", "filled", "partial"};

static void drm_poison(VkPhysicalDeviceDrmPropertiesEXT *d) {
    memset(d, DRM_POISON, sizeof *d);
    d->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
    d->pNext = NULL;
}
/* Classify the payload (everything after pNext). UNTOUCHED = still all poison: the HAL ignored the
 * struct, so a caller that zero-initialises reads hasRender=0. ZEROED = the HAL wrote only zeros. */
static enum drm_state drm_classify(const VkPhysicalDeviceDrmPropertiesEXT *d) {
    const unsigned char *p = (const unsigned char *)d + offsetof(VkPhysicalDeviceDrmPropertiesEXT, hasPrimary);
    size_t n = sizeof *d - offsetof(VkPhysicalDeviceDrmPropertiesEXT, hasPrimary), poison = 0, zero = 0;
    for (size_t i = 0; i < n; i++) {
        poison += p[i] == DRM_POISON;
        zero += p[i] == 0;
    }
    if (poison == n) return DRM_UNTOUCHED;
    if (zero == n) return DRM_ZEROED;
    if (poison) return DRM_PARTIAL;
    return d->hasPrimary <= 1 && d->hasRender <= 1 ? DRM_FILLED : DRM_PARTIAL;
}

/* What the Vulkan spec ("Required Format Support") requires of every BC format in optimalTilingFeatures
 * once textureCompressionBC is true. */
#define BC_MANDATED (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |      \
                     VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |                          \
                     VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)
static int bc_complete(VkFormatFeatureFlags f) { return (f & BC_MANDATED) == BC_MANDATED; }
static int bc_sampled(VkFormatFeatureFlags f) { return (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0; }

struct fmt { VkFormat id; const char *name; int d3d_bc; };
#define F(x, bc) {VK_FORMAT_##x, #x, bc}
static const struct fmt FORMATS[] = {
    F(R8G8B8A8_UNORM, 0), F(R8G8B8A8_SRGB, 0), F(B8G8R8A8_UNORM, 0), F(B8G8R8A8_SRGB, 0),
    F(BC1_RGB_UNORM_BLOCK, 0), F(BC1_RGB_SRGB_BLOCK, 0), /* not a D3D format */
    F(BC1_RGBA_UNORM_BLOCK, 1), F(BC1_RGBA_SRGB_BLOCK, 1), F(BC2_UNORM_BLOCK, 1), F(BC2_SRGB_BLOCK, 1),
    F(BC3_UNORM_BLOCK, 1), F(BC3_SRGB_BLOCK, 1), F(BC4_UNORM_BLOCK, 1), F(BC4_SNORM_BLOCK, 1),
    F(BC5_UNORM_BLOCK, 1), F(BC5_SNORM_BLOCK, 1), F(BC6H_UFLOAT_BLOCK, 1), F(BC6H_SFLOAT_BLOCK, 1),
    F(BC7_UNORM_BLOCK, 1), F(BC7_SRGB_BLOCK, 1),
};
#define NFORMATS (sizeof FORMATS / sizeof *FORMATS)

static void list_add(char *buf, size_t size, const char *name) {
    size_t used = strlen(buf);
    if (used + strlen(name) + 2 < size) snprintf(buf + used, size - used, "%s%s", used ? "," : "", name);
}

/* ---- device nodes ------------------------------------------------------------------------ */
static void report_nodes(void) {
    DIR *dir = opendir("/dev/dri");
    if (!dir) {
        P("nodes", "dri_dir", "unreadable errno=%d", errno);
    } else {
        struct dirent *e;
        for (int n = 0; n < MAX_NODES && (e = readdir(dir)); n++) {
            struct stat st;
            char path[300];
            if (e->d_name[0] == '.') continue;
            snprintf(path, sizeof path, "/dev/dri/%.200s", e->d_name);
            if (stat(path, &st)) {
                P("nodes", "dri", "%.200s stat_errno=%d", e->d_name, errno);
                continue;
            }
            P("nodes", "dri", "%.200s %s %u:%u mode=%04o uid=%u gid=%u", e->d_name, S_ISCHR(st.st_mode) ? "chr" : "other",
              major(st.st_rdev), minor(st.st_rdev), (unsigned)(st.st_mode & 07777), (unsigned)st.st_uid, (unsigned)st.st_gid);
        }
        closedir(dir);
    }
    struct stat st;
    if (!stat("/dev/dri/renderD128", &st) && S_ISCHR(st.st_mode))
        P("nodes", "renderD128", "chr %u:%u", major(st.st_rdev), minor(st.st_rdev));
    else
        P("nodes", "renderD128", "absent_or_not_chr errno=%d", errno);
}

/* ---- one source, one chain ---------------------------------------------------------------- */
struct source {
    const char *label;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkCreateInstance create;
    int is_hal;
};

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name, uint32_t *spec) {
    for (uint32_t i = 0; i < n; i++)
        if (!strncmp(e[i].extensionName, name, VK_MAX_EXTENSION_NAME_SIZE)) {
            if (spec) *spec = e[i].specVersion;
            return 1;
        }
    return 0;
}

static void report_native_buffer(const struct source *s, VkInstance inst, VkPhysicalDevice pd) {
    const char *l = s->label;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qfp =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)s->gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)s->gipa(inst, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)s->gipa(inst, "vkGetDeviceProcAddr");
    if (!qfp || !create || !gdpa) {
        P(l, "native_buffer.device", "skipped missing_entry_point");
        return;
    }
    uint32_t nq = 0, family = UINT32_MAX;
    VkQueueFamilyProperties q[16];
    qfp(pd, &nq, NULL);
    if (nq > 16) nq = 16;
    qfp(pd, &nq, q);
    for (uint32_t i = 0; i < nq && family == UINT32_MAX; i++)
        if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = i;
    if (family == UINT32_MAX) {
        P(l, "native_buffer.device", "skipped no_graphics_queue");
        return;
    }
    float prio = 1.0f;
    const char *names[] = {"VK_ANDROID_native_buffer"};
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = names};
    VkDevice dev = VK_NULL_HANDLE;
    VkResult r = create(pd, &dci, NULL, &dev);
    P(l, "native_buffer.device", "vkCreateDevice=%d family=%u", r, family);
    if (r != VK_SUCCESS || !dev) return;
    PFN_gralloc1 g1 = (PFN_gralloc1)gdpa(dev, "vkGetSwapchainGrallocUsageANDROID");
    PFN_gralloc2 g2 = (PFN_gralloc2)gdpa(dev, "vkGetSwapchainGrallocUsage2ANDROID");
    PFN_gralloc3 g3 = (PFN_gralloc3)gdpa(dev, "vkGetSwapchainGrallocUsage3ANDROID");
    PFN_gralloc4 g4 = (PFN_gralloc4)gdpa(dev, "vkGetSwapchainGrallocUsage4ANDROID");
    P(l, "native_buffer.gralloc_fns", "v1=%d v2=%d v3=%d v4=%d (icd.c calls v2)", g1 != NULL, g2 != NULL, g3 != NULL, g4 != NULL);
    const VkFormat fmts[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    const char *const fmt_names[] = {"RGBA8", "BGRA8"};
    const VkImageUsageFlags uses[] = {
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT};
    for (size_t f = 0; f < 2; f++)
        for (size_t u = 0; u < 3; u++) {
            char key[48];
            snprintf(key, sizeof key, "native_buffer.usage.%s.use0x%x", fmt_names[f], (unsigned)uses[u]);
            if (g2) {
                uint64_t consumer = 0, producer = 0;
                VkResult rr = g2(dev, fmts[f], uses[u], 0, &consumer, &producer);
                P(l, key, "v2 result=%d consumer=0x%llx producer=0x%llx exceeds_32_bits=%d", rr, (unsigned long long)consumer,
                  (unsigned long long)producer, ((consumer | producer) >> 32) != 0);
            }
            if (g1) {
                int usage = 0;
                VkResult rr = g1(dev, fmts[f], uses[u], &usage);
                P(l, key, "v1 result=%d usage=0x%x", rr, (unsigned)usage);
            }
            if (g3) {
                GrallocInfo3 gi = {ANB_ENUM(3), NULL, fmts[f], uses[u]};
                uint64_t usage = 0;
                VkResult rr = g3(dev, &gi, &usage);
                P(l, key, "v3 result=%d usage=0x%llx exceeds_32_bits=%d", rr, (unsigned long long)usage, (usage >> 32) != 0);
            }
            if (g4) {
                GrallocInfo4 gi = {ANB_ENUM(4), NULL, fmts[f], uses[u], 0};
                uint64_t usage = 0;
                VkResult rr = g4(dev, &gi, &usage);
                P(l, key, "v4 result=%d usage=0x%llx exceeds_32_bits=%d", rr, (unsigned long long)usage, (usage >> 32) != 0);
            }
        }
    PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)gdpa(dev, "vkDestroyDevice");
    if (destroy) destroy(dev, NULL);
}

static void report_device(const struct source *s, VkInstance inst, VkPhysicalDevice pd, unsigned index) {
    const char *l = s->label;
    PFN_vkGetPhysicalDeviceProperties gpp = (PFN_vkGetPhysicalDeviceProperties)s->gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceProperties2 gpp2 = (PFN_vkGetPhysicalDeviceProperties2)s->gipa(inst, "vkGetPhysicalDeviceProperties2");
    if (!gpp2) gpp2 = (PFN_vkGetPhysicalDeviceProperties2)s->gipa(inst, "vkGetPhysicalDeviceProperties2KHR");
    PFN_vkGetPhysicalDeviceFeatures gpf = (PFN_vkGetPhysicalDeviceFeatures)s->gipa(inst, "vkGetPhysicalDeviceFeatures");
    PFN_vkGetPhysicalDeviceFormatProperties gfp =
        (PFN_vkGetPhysicalDeviceFormatProperties)s->gipa(inst, "vkGetPhysicalDeviceFormatProperties");
    PFN_vkEnumerateDeviceExtensionProperties edep =
        (PFN_vkEnumerateDeviceExtensionProperties)s->gipa(inst, "vkEnumerateDeviceExtensionProperties");
    if (!gpp || !edep) {
        P(l, "device", "%u skipped missing_entry_point", index);
        return;
    }

    VkPhysicalDeviceProperties p;
    memset(&p, 0, sizeof p);
    gpp(pd, &p);
    char ver[96];
    P(l, "device.index", "%u", index);
    Pstr(l, "device.name", p.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);
    P(l, "device.vendorID", "0x%x", p.vendorID);
    P(l, "device.deviceID", "0x%x", p.deviceID);
    P(l, "device.type", "%d", (int)p.deviceType);
    P(l, "device.apiVersion", "%u.%u.%u", VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
      VK_API_VERSION_PATCH(p.apiVersion));
    fmt_driver_version(p.driverVersion, ver, sizeof ver);
    P(l, "device.driverVersion", "%s", ver);

    static VkExtensionProperties ext[MAX_EXT];
    uint32_t ne = 0;
    VkResult er = edep(pd, NULL, &ne, NULL);
    if (ne > MAX_EXT) ne = MAX_EXT;
    if (er == VK_SUCCESS || er == VK_INCOMPLETE) er = edep(pd, NULL, &ne, ext);
    if (er != VK_SUCCESS && er != VK_INCOMPLETE) ne = 0;
    P(l, "ext.count", "%u (enumerate result=%d)", ne, er);
    for (uint32_t i = 0; i < ne; i++) {
        ext[i].extensionName[VK_MAX_EXTENSION_NAME_SIZE - 1] = 0;
        Pstr(l, "ext", ext[i].extensionName, VK_MAX_EXTENSION_NAME_SIZE);
    }
    static const char *const WATCH[] = {"VK_EXT_physical_device_drm", "VK_ANDROID_native_buffer", "VK_KHR_driver_properties",
                                        "VK_EXT_mutable_descriptor_type", "VK_EXT_memory_budget", "VK_EXT_external_memory_host",
                                        "VK_EXT_map_memory_placed", "VK_KHR_dynamic_rendering", "VK_EXT_image_drm_format_modifier",
                                        "VK_EXT_external_memory_dma_buf", "VK_KHR_external_memory_fd"};
    for (size_t w = 0; w < sizeof WATCH / sizeof *WATCH; w++) {
        uint32_t spec = 0;
        int has = has_ext(ext, ne, WATCH[w], &spec);
        P(l, "has", "%s=%d spec=%u", WATCH[w], has, has ? spec : 0);
    }

    if (gpp2 && (p.apiVersion >= VK_API_VERSION_1_2 || has_ext(ext, ne, "VK_KHR_driver_properties", NULL))) {
        VkPhysicalDeviceDriverProperties drv;
        memset(&drv, 0, sizeof drv);
        drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
        gpp2(pd, &p2);
        drv.driverName[VK_MAX_DRIVER_NAME_SIZE - 1] = 0;
        drv.driverInfo[VK_MAX_DRIVER_INFO_SIZE - 1] = 0;
        P(l, "driver.id", "%d", (int)drv.driverID);
        Pstr(l, "driver.name", drv.driverName, VK_MAX_DRIVER_NAME_SIZE);
        Pstr(l, "driver.info", drv.driverInfo, VK_MAX_DRIVER_INFO_SIZE);
        char release[32];
        uint32_t build = 0;
        if (parse_driver_info(drv.driverInfo, release, sizeof release, &build))
            P(l, "driver.release", "%s build=%u build_matches_driverVersion=%d", release, build, build == p.driverVersion);
        else
            P(l, "driver.release", "unparsed");
    } else {
        P(l, "driver.info", "unavailable");
    }

    if (gpp2 && has_ext(ext, ne, "VK_EXT_physical_device_drm", NULL)) {
        VkPhysicalDeviceDrmPropertiesEXT drm;
        drm_poison(&drm);
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drm};
        gpp2(pd, &p2);
        P(l, "drm.state", "%s", DRM_STATE_NAME[drm_classify(&drm)]);
        P(l, "drm.fields", "hasPrimary=0x%x hasRender=0x%x primary=%lld:%lld render=%lld:%lld", drm.hasPrimary, drm.hasRender,
          (long long)drm.primaryMajor, (long long)drm.primaryMinor, (long long)drm.renderMajor, (long long)drm.renderMinor);
    } else {
        P(l, "drm.state", "not_advertised");
    }

    if (gpf) {
        VkPhysicalDeviceFeatures f;
        memset(&f, 0, sizeof f);
        gpf(pd, &f);
        P(l, "features", "textureCompressionBC=%u textureCompressionETC2=%u textureCompressionASTC_LDR=%u", f.textureCompressionBC,
          f.textureCompressionETC2, f.textureCompressionASTC_LDR);
    }

    if (gfp) {
        char incomplete[320] = "", unsampled[320] = "";
        unsigned complete = 0, d3d = 0;
        for (size_t i = 0; i < NFORMATS; i++) {
            VkFormatProperties fp;
            memset(&fp, 0, sizeof fp);
            gfp(pd, FORMATS[i].id, &fp);
            P(l, "fmt", "%s optimal=0x%08x linear=0x%08x buffer=0x%08x sampled=%d bc_mandated_complete=%d", FORMATS[i].name,
              fp.optimalTilingFeatures, fp.linearTilingFeatures, fp.bufferFeatures, bc_sampled(fp.optimalTilingFeatures),
              bc_complete(fp.optimalTilingFeatures));
            if (!FORMATS[i].d3d_bc) continue;
            d3d++;
            if (bc_complete(fp.optimalTilingFeatures)) complete++;
            else if (bc_sampled(fp.optimalTilingFeatures)) list_add(incomplete, sizeof incomplete, FORMATS[i].name);
            else list_add(unsampled, sizeof unsampled, FORMATS[i].name);
        }
        P(l, "bc.d3d_native_complete", "%u/%u", complete, d3d);
        P(l, "bc.d3d_sampled_but_incomplete", "%s", incomplete);
        P(l, "bc.d3d_not_sampleable", "%s", unsampled);
    }

    /* What the BC policy checks: it refuses emulation when any graphics/compute/transfer family lacks
     * compute or reports a transfer granularity other than 1x1x1. */
    PFN_vkGetPhysicalDeviceQueueFamilyProperties gqf =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)s->gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (gqf) {
        VkQueueFamilyProperties qf[16];
        uint32_t nq = 0;
        memset(qf, 0, sizeof qf);
        gqf(pd, &nq, NULL);
        if (nq > 16) nq = 16;
        gqf(pd, &nq, qf);
        P(l, "queue.count", "%u", nq);
        for (uint32_t i = 0; i < nq; i++)
            P(l, "queue.family", "%u flags=0x%x count=%u granularity=%ux%ux%u", i, qf[i].queueFlags, qf[i].queueCount,
              qf[i].minImageTransferGranularity.width, qf[i].minImageTransferGranularity.height,
              qf[i].minImageTransferGranularity.depth);
    }

    uint32_t anb_spec = 0;
    if (has_ext(ext, ne, "VK_ANDROID_native_buffer", &anb_spec)) {
        P(l, "native_buffer.spec_version", "%u (icd needs >= 8)", anb_spec);
        report_native_buffer(s, inst, pd);
    }
}

/* Request Vulkan 1.4 first (what the system loader gives an app), then 1.1 (what the HAL accepted
 * from Fizzolas's probe). The ladder is data, not two code paths. */
static int probe_source(const struct source *s) {
    const char *l = s->label;
    static const uint32_t LADDER[] = {VK_MAKE_API_VERSION(0, 1, 4, 0), VK_MAKE_API_VERSION(0, 1, 1, 0)};
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (!s->gipa || !s->create) {
        P(l, "instance", "no_entry_points");
        return 2;
    }
    for (size_t i = 0; i < 2 && inst == VK_NULL_HANDLE; i++) {
        VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "pvr_props_probe",
                                 .apiVersion = LADDER[i]};
        VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
        r = s->create(&ici, NULL, &inst);
        P(l, "instance", "vkCreateInstance(api %u.%u)=%d", VK_API_VERSION_MAJOR(LADDER[i]), VK_API_VERSION_MINOR(LADDER[i]), r);
        if (r != VK_SUCCESS) inst = VK_NULL_HANDLE;
    }
    if (inst == VK_NULL_HANDLE) return 2;
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)s->gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)s->gipa(inst, "vkDestroyInstance");
    VkPhysicalDevice pds[MAX_PD];
    uint32_t n = MAX_PD;
    int rc = 0;
    if (!enumerate) {
        P(l, "devices", "no vkEnumeratePhysicalDevices");
        rc = 2;
    } else {
        r = enumerate(inst, &n, pds);
        P(l, "devices", "count=%u result=%d", n, r);
        for (uint32_t i = 0; i < n && i < MAX_PD; i++) report_device(s, inst, pds[i], i);
    }
    if (destroy) destroy(inst, NULL);
    return rc;
}

/* ---- the two sources ----------------------------------------------------------------------- */
static int run_loader(const char *unused) {
    (void)unused;
    void *lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        Pstr("loader", "load_error", dlerror(), 200);
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    struct source s = {"loader", gipa, gipa ? (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance") : NULL, 0};
    PFN_vkEnumerateInstanceVersion eiv = gipa ? (PFN_vkEnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion") : NULL;
    uint32_t v = 0;
    if (eiv && eiv(&v) == VK_SUCCESS)
        P("loader", "instance_version", "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
    return probe_source(&s);
}

/* Plain dlopen first. If the shell domain's namespace refuses the vendor path, take the exported
 * "sphal" namespace the way libhybris's ICD and Android's own loader do. */
static void *load_hal(const char *path, const char **how) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h) {
        *how = "plain";
        return h;
    }
    Pstr("hal", "load_plain_error", dlerror(), 250);
#ifdef __ANDROID__
    typedef struct android_namespace_t *(*get_ns_fn)(const char *);
    void *dla = dlopen("libdl_android.so", RTLD_NOW);
    get_ns_fn get_ns = dla ? (get_ns_fn)dlsym(dla, "__loader_android_get_exported_namespace") : NULL;
    struct android_namespace_t *ns = get_ns ? get_ns("sphal") : NULL;
    P("hal", "sphal_namespace", "%s", ns ? "found" : "unavailable");
    if (ns) {
        android_dlextinfo ext = {.flags = ANDROID_DLEXT_USE_NAMESPACE, .library_namespace = ns};
        h = android_dlopen_ext(path, RTLD_NOW | RTLD_LOCAL, &ext);
        if (h) {
            *how = "sphal";
            return h;
        }
        Pstr("hal", "load_sphal_error", dlerror(), 250);
    }
#endif
    *how = "failed";
    return NULL;
}

static int run_hal(const char *path) {
    const char *how = "failed";
    P("hal", "path", "%s", path);
    void *h = load_hal(path, &how);
    P("hal", "load", "%s", how);
    if (!h) return 2;
    hw_module_t *m = (hw_module_t *)dlsym(h, "HMI");
    if (!m) {
        P("hal", "module", "no HMI symbol");
        return 2;
    }
    P("hal", "module.tag", "0x%08x (%s)", m->tag, m->tag == HW_MODULE_TAG ? "HWMT ok" : "unexpected");
    Pstr("hal", "module.id", m->id, 64);
    Pstr("hal", "module.name", m->name, 128);
    Pstr("hal", "module.author", m->author, 128);
    P("hal", "module.api", "module_api_version=0x%04x hal_api_version=0x%04x", m->module_api_version, m->hal_api_version);
    if (!m->methods || !m->methods->open) {
        P("hal", "module", "no methods->open");
        return 2;
    }
    hwvulkan_device_t *dev = NULL;
    int rc = m->methods->open(m, "vk0", (hw_device_t **)&dev);
    P("hal", "open_vk0", "rc=%d dev=%s", rc, dev ? "non-null" : "null");
    if (rc || !dev) return 2;
    uint32_t v = dev->common.version;
    P("hal", "device.tag", "0x%08x (%s)", dev->common.tag, dev->common.tag == HW_DEVICE_TAG ? "HWDT ok" : "unexpected");
    P("hal", "device.version", "0x%08x accepted_by_upstream_hybris=%d accepted_by_powervr_patch=%d", v, v == HWVK_DEVICE_API_0_1,
      v == HWVK_DEVICE_API_0_1 || v == HW_DEVICE_API_0_1);
    P("hal", "device.entry_points", "CreateInstance=%d GetInstanceProcAddr=%d EnumerateInstanceExtensionProperties=%d",
      dev->CreateInstance != NULL, dev->GetInstanceProcAddr != NULL, dev->EnumerateInstanceExtensionProperties != NULL);
    if (!dev->CreateInstance || !dev->GetInstanceProcAddr) return 2;
    if (dev->EnumerateInstanceExtensionProperties) {
        static VkExtensionProperties ie[MAX_EXT];
        uint32_t n = MAX_EXT;
        VkResult r = dev->EnumerateInstanceExtensionProperties(NULL, &n, ie);
        if (n > MAX_EXT) n = MAX_EXT;
        P("hal", "instance_ext.count", "%u (result=%d)", n, r);
        for (uint32_t i = 0; i < n && r == VK_SUCCESS; i++) {
            ie[i].extensionName[VK_MAX_EXTENSION_NAME_SIZE - 1] = 0;
            Pstr("hal", "instance_ext", ie[i].extensionName, VK_MAX_EXTENSION_NAME_SIZE);
        }
    }
    struct source s = {"hal", dev->GetInstanceProcAddr, dev->CreateInstance, 1};
    return probe_source(&s);
}

/* ---- orchestration -------------------------------------------------------------------------- */
static int isolated(const char *name, int (*fn)(const char *), const char *arg) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        P("section", name, "fork_failed errno=%d", errno);
        return 1;
    }
    if (pid == 0) {
        alarm(SECTION_TIMEOUT_S);
        int rc = fn(arg);
        fflush(stdout);
        _exit(rc);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (WIFSIGNALED(st)) {
        P("section", name, "killed signal=%d", WTERMSIG(st));
        return 1;
    }
    P("section", name, "exit=%d", WEXITSTATUS(st));
    return WEXITSTATUS(st) != 0;
}

#ifndef PVR_PROBE_NO_MAIN
int main(int argc, char **argv) {
    const char *hal = argc > 1 && argv[1][0] ? argv[1] : DEFAULT_HAL;
    setvbuf(stdout, NULL, _IOLBF, 0);
    P("probe", "version", "1");
    P("probe", "uid", "%u", (unsigned)getuid());
    FILE *f = fopen("/proc/self/attr/current", "r");
    char ctx[128] = "";
    if (f) {
        if (!fgets(ctx, sizeof ctx, f)) ctx[0] = 0;
        fclose(f);
    }
    Pstr("probe", "selinux_context", ctx, 100);
    P("probe", "page_size", "%ld", sysconf(_SC_PAGESIZE));
    report_nodes();
    int bad = isolated("loader", run_loader, NULL);
    bad |= isolated("hal", run_hal, hal);
    return bad;
}
#endif
~~~~

### Appendix G: `tools/pixel10/hwvulkan_abi.h`

~~~~c
/* hwvulkan_abi.h - the Android Vulkan HAL ABI as an LP64 C header, for tools that call the vendor HAL
 * directly (tools/pixel10/pvr_props_probe.c) or stand in for it (tools/tests/fake_powervr_hal.c).
 * hw_module_t / hw_device_t are copies of AOSP hardware/libhardware include_all/hardware/hardware.h;
 * hwvulkan_device_t is frameworks/native vulkan/include/hardware/hwvulkan.h. The NDK ships neither.
 * Include after <vulkan/vulkan.h>. The static asserts pin the offsets measured on the Pixel 10 HAL
 * (close +0x70, CreateInstance +0x80, GetInstanceProcAddr +0x88, 0x90 bytes). It also carries the
 * VK_ANDROID_native_buffer gralloc-usage entry points (vk_android_native_buffer.h, spec versions 6-11).
 */
#ifndef PIXEL10_HWVULKAN_ABI_H
#define PIXEL10_HWVULKAN_ABI_H
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/* ---- Android hardware ABI (LP64) -------------------------------------------------------- */
typedef struct hw_module_t hw_module_t;
typedef struct hw_device_t hw_device_t;
typedef struct { int (*open)(const hw_module_t *, const char *, hw_device_t **); } hw_module_methods_t;
struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version, hal_api_version;
    const char *id, *name, *author;
    hw_module_methods_t *methods;
    void *dso;
    uint64_t reserved[32 - 7];
};
struct hw_device_t {
    uint32_t tag, version;
    hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(hw_device_t *);
};
typedef struct {
    hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} hwvulkan_device_t;
_Static_assert(sizeof(void *) == 8, "LP64 only: the reserved[] widths above are the __LP64__ ones");
_Static_assert(sizeof(hw_module_t) == 0xf8 && offsetof(hw_module_t, methods) == 0x20,
               "hw_module_t layout");
_Static_assert(offsetof(hw_device_t, close) == 0x70 && sizeof(hwvulkan_device_t) == 0x90 &&
                   offsetof(hwvulkan_device_t, GetInstanceProcAddr) == 0x88,
               "hwvulkan_device_t layout (close +0x70, CreateInstance +0x80, GetInstanceProcAddr +0x88)");
#define HW_MODULE_TAG 0x48574d54u  /* 'HWMT' */
#define HW_DEVICE_TAG 0x48574454u  /* 'HWDT' */
#define HWVK_DEVICE_API_0_1 0x00010000u /* HARDWARE_DEVICE_API_VERSION_2(0, 1, 0): upstream hybris */
#define HW_DEVICE_API_0_1 0x00000001u   /* HARDWARE_DEVICE_API_VERSION(0, 1): what powervr-hal-version.patch adds */

/* ---- VK_ANDROID_native_buffer, only what the gralloc-usage queries need ------------------ */
#define ANB_ENUM(id) ((VkStructureType)(1000000000 + 1000 * (11 - 1) + (id)))
typedef VkFlags VkSwapchainImageUsageFlagsANDROID;
typedef struct { VkStructureType sType; const void *pNext; VkFormat format; VkImageUsageFlags imageUsage; } GrallocInfo3;
typedef struct { VkStructureType sType; const void *pNext; VkFormat format; VkImageUsageFlags imageUsage;
                 VkSwapchainImageUsageFlagsANDROID swapchainImageUsage; } GrallocInfo4;
typedef VkResult (*PFN_gralloc1)(VkDevice, VkFormat, VkImageUsageFlags, int *);
typedef VkResult (*PFN_gralloc2)(VkDevice, VkFormat, VkImageUsageFlags, VkSwapchainImageUsageFlagsANDROID,
                                 uint64_t *, uint64_t *);
typedef VkResult (*PFN_gralloc3)(VkDevice, const GrallocInfo3 *, uint64_t *);
typedef VkResult (*PFN_gralloc4)(VkDevice, const GrallocInfo4 *, uint64_t *);

#endif
~~~~

### Appendix H: `tools/pixel10/build_props_probe.py`

~~~~python
#!/usr/bin/env python3
"""Build the bionic PowerVR property probe with the Android NDK; does not build an APK.

The NDK is found through ANDROID_NDK_HOME, ANDROID_NDK_ROOT, ANDROID_HOME/ndk/<version> or
ANDROID_SDK_ROOT/ndk/<version> (the version app/build.gradle pins), then Android Studio's default
SDK folder on Windows. The probe has no link-time Vulkan dependency; it dlopens libvulkan.so.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import struct
import subprocess

NDK_VERSION = "27.3.13750724"  # ndkVersion in app/build.gradle
API = 29
HOSTS = {"Windows": ("windows-x86_64", ".exe"), "Linux": ("linux-x86_64", ""), "Darwin": ("darwin-x86_64", "")}


def ndk_roots(environ):
    for name in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"):
        if environ.get(name):
            yield Path(environ[name])
    for name in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        if environ.get(name):
            yield Path(environ[name]) / "ndk" / NDK_VERSION
    if environ.get("LOCALAPPDATA"):
        yield Path(environ["LOCALAPPDATA"]) / "Android" / "Sdk" / "ndk" / NDK_VERSION


def find_clang(environ, system):
    tag, suffix = HOSTS[system]
    for root in ndk_roots(environ):
        clang = root / "toolchains" / "llvm" / "prebuilt" / tag / "bin" / ("clang" + suffix)
        if clang.is_file():
            return clang
    raise SystemExit("Android NDK %s not found; set ANDROID_NDK_HOME to its folder" % NDK_VERSION)


def compile_command(clang, source, output):
    # No -Werror: a warning from a newer NDK header must not cost a device iteration.
    return [str(clang), "--target=aarch64-linux-android%d" % API, "-O2", "-std=c11", "-Wall", "-Wextra",
            str(source), "-ldl", "-o", str(output)]


def is_aarch64_elf(header):
    return header[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", header, 18)[0] == 183


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source = Path(__file__).resolve().parent / "pvr_props_probe.c"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    command = compile_command(find_clang(os.environ, platform.system()), source, output / "pvr_props_probe")
    subprocess.run(command, check=True)
    if not is_aarch64_elf((output / "pvr_props_probe").read_bytes()[:64]):
        raise SystemExit("Compiler did not produce an ELF64 AArch64 executable")
    (output / "build-command.json").write_text(json.dumps(command, indent=2) + "\n")
    print("Built", output / "pvr_props_probe")


if __name__ == "__main__":
    main()
~~~~

### Appendix I: `tools/pixel10/pixel10.py`

~~~~python
#!/usr/bin/env python3
"""Pixel 10 bring-up over ADB: device facts (phase 0), the property probe (phase 1), session logs (phase 2).

    python tools/pixel10/pixel10.py facts   [--out DIR]
    python tools/pixel10/pixel10.py probe   PVR_PROPS_PROBE [--hal PATH] [--out DIR]
    python tools/pixel10/pixel10.py logs    [--out DIR]
    python tools/pixel10/pixel10.py install APK

adb is found through --adb, ADB, PATH or ANDROID_HOME/ANDROID_SDK_ROOT; the device through --serial,
ADB_SERIAL / ANDROID_SERIAL or the only authorized one (tools/adb_device.py). `facts` only reads.
`probe` pushes one file to /data/local/tmp/pixel10. `install` replaces the app in place and never
uninstalls anything. Exit codes: 0 done, 2 bad arguments, 3 adb or device error, 4 needs the user.
"""
from __future__ import annotations

import argparse
import datetime
import io
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from adb_device import DeviceResolutionError, resolve_serial  # noqa: E402

PACKAGE = "com.droiddeck.launcher"
REMOTE = "/data/local/tmp/pixel10"
SESSION_FOLDER = re.compile(r"^\d{4}-\d{2}-\d{2}-\d{2,}-.+$")
SHARE_ZIP = re.compile(r"^DroidDeck-.+\.zip$")

# Identity of the build under test: no serial number, account or network name.
PROPS = (
    "ro.build.fingerprint ro.build.id ro.build.display.id ro.build.type ro.build.tags ro.build.version.release "
    "ro.build.version.sdk ro.build.version.security_patch ro.build.version.incremental ro.product.manufacturer "
    "ro.product.model ro.product.name ro.product.device ro.product.board ro.board.platform ro.hardware "
    "ro.hardware.vulkan ro.hardware.egl ro.soc.manufacturer ro.soc.model ro.opengles.version "
    "ro.vendor.build.fingerprint ro.boot.verifiedbootstate ro.boot.flash.locked ro.product.cpu.abilist "
    "persist.sys.fflag.override.settings_enable_monitor_phantom_procs"
)

# (heading, command run by the device's shell). Every command is read-only and may fail on its own.
FACTS = [
    ("build properties", f'for p in {PROPS}; do echo "$p=$(getprop $p)"; done'),
    ("kernel", "uname -a; cat /proc/version"),
    ("page size", "getconf PAGE_SIZE; grep -m1 KernelPageSize /proc/self/smaps"),
    ("kernel config", "zcat /proc/config.gz | grep -E '^CONFIG_(ARM64_(4K|16K|64K)_PAGES|ARM64_VA_BITS(_[0-9]+)?"
                      "|ARM64_PA_BITS(_[0-9]+)?|DRM_POWERVR|DRM_IMAGINATION)='"),
    ("cpu", "grep -m1 Features /proc/cpuinfo; grep -c ^processor /proc/cpuinfo"),
    ("vulkan hal", "ls -lZ /vendor/lib64/hw/vulkan.powervr.so; ls -l /vendor/lib64/hw | head -40"),
    ("drm nodes", "ls -lZ /dev/dri/; stat -c '%n %F %t:%T hex' /dev/dri/*; ls -l /sys/class/drm/; "
                  "cat /sys/class/drm/renderD128/device/uevent"),
    ("pvr sync and modules", "ls -lZ /dev/pvr_sync; grep -i -E 'pvr|powervr' /proc/modules"),
    ("vulkan and gles features", "pm list features | grep -i -E 'vulkan|opengles'"),
    ("selinux and identity", "getenforce; id; cat /proc/self/attr/current"),
    ("linker namespace directories", "grep -n '^dir\\.' /linkerconfig/ld.config.txt"),
    ("phantom process limit", "settings get global settings_enable_monitor_phantom_procs; "
                              "device_config get activity_manager max_phantom_processes"),
    ("storage", "df -h /data /sdcard"),
    ("droiddeck packages", "pm list packages | grep -i droiddeck"),
    ("droiddeck package", f"dumpsys package {PACKAGE} | grep -E 'versionName|versionCode|targetSdk|flags=|pkgFlags|"
                          "firstInstallTime|lastUpdateTime|dataDir|primaryCpuAbi'"),
]
# Large outputs kept as files of their own.
RAW = [
    ("ld.config.txt", "cat /linkerconfig/ld.config.txt"),
    ("cpuinfo.txt", "cat /proc/cpuinfo"),
    ("vkjson.json", "cmd gpu vkjson"),
    ("dumpsys-gpu.txt", "dumpsys gpu"),
]


class Adb:
    """The one door to adb: every call is argv, bytes in and out."""

    def __init__(self, exe: str, serial: str) -> None:
        self.exe, self.serial = exe, serial

    def run(self, *args: str, timeout: float = 60) -> subprocess.CompletedProcess:
        try:
            return subprocess.run([self.exe, "-s", self.serial, *args], capture_output=True, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise SystemExit(f"pixel10: adb failed: {error}") from error

    def shell(self, command: str, timeout: float = 60) -> subprocess.CompletedProcess:
        return self.run("shell", command, timeout=timeout)


def text(result: subprocess.CompletedProcess) -> str:
    return (result.stdout + result.stderr).decode("utf-8", "replace").replace("\r\n", "\n")


def section(config: str, name: str) -> str:
    """One [name] section of a linkerconfig file: the header line up to the next header."""
    out, inside = [], False
    for line in config.splitlines():
        if line.startswith("["):
            inside = line.strip() == f"[{name}]"
        if inside:
            out.append(line)
    return "\n".join(out) + ("\n" if out else "")


def facts(adb: Adb, out: Path) -> int:
    out.mkdir(parents=True, exist_ok=True)
    report = []
    for heading, command in FACTS:
        result = adb.shell(command)
        report.append(f"### {heading}\n$ {command}\n{text(result)}[exit {result.returncode}]\n")
    for name, command in RAW:
        result = adb.shell(command, timeout=120)
        (out / name).write_bytes(result.stdout)
        report.append(f"### {name}\n$ {command}\n[{len(result.stdout)} bytes saved, exit {result.returncode}]\n")
    config = (out / "ld.config.txt").read_text("utf-8", "replace")
    (out / "ld.config.system.txt").write_text(section(config, "system"), "utf-8")
    (out / "facts.txt").write_text("\n".join(report), "utf-8")
    print(f"facts written to {out / 'facts.txt'}")
    return 0


def probe(adb: Adb, binary: Path, hal: str | None, out: Path) -> int:
    if not binary.is_file():
        print(f"pixel10: no such probe binary: {binary}", file=sys.stderr)
        return 2
    out.mkdir(parents=True, exist_ok=True)
    adb.shell(f"mkdir -p {REMOTE}")
    pushed = adb.run("push", str(binary), f"{REMOTE}/pvr_props_probe", timeout=120)
    if pushed.returncode:
        print("pixel10: adb push failed:\n" + text(pushed), file=sys.stderr)
        return 3
    adb.shell(f"chmod 755 {REMOTE}/pvr_props_probe")
    ran = adb.shell(f"cd {REMOTE} && ./pvr_props_probe {shlex.quote(hal) if hal else ''}", timeout=240)
    (out / "props.txt").write_bytes(ran.stdout)
    (out / "props.stderr.txt").write_bytes(ran.stderr)
    for line in text(ran).splitlines():
        if re.match(r"(section\.|hal\.(driver\.|drm\.state|device\.version|bc\.|native_buffer\.(spec|gralloc)))", line):
            print(line)
    print(f"probe output written to {out / 'props.txt'} (adb exit {ran.returncode})")
    return 0


def latest_session(names: list[str]) -> str | None:
    folders = sorted(n for n in names if SESSION_FOLDER.match(n))
    return folders[-1] if folders else None


def extract(data: bytes, out: Path) -> None:
    """Unpack a tar from the device; a member that would land outside `out` stops the whole unpack."""
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        for member in archive.getmembers():
            if member.name.startswith(("/", "\\")) or ".." in Path(member.name).parts:
                raise SystemExit(f"pixel10: refusing tar member outside the output folder: {member.name}")
        if hasattr(tarfile, "data_filter"):
            archive.extractall(out, filter="data")
        else:
            archive.extractall(out)


def logs(adb: Adb, out: Path) -> int:
    """Newest session folder. Pulled with run-as when the installed app is debuggable; otherwise the
    Share logs zip, if the user saved it to Download; otherwise the steps that make one."""
    out.mkdir(parents=True, exist_ok=True)
    if adb.shell(f"run-as {PACKAGE} true").returncode == 0:
        listing = text(adb.shell(f"run-as {PACKAGE} ls -1 files/logs")).split()
        name = latest_session(listing)
        if name:
            packed = adb.run("exec-out", f"run-as {PACKAGE} tar -cf - -C files/logs {shlex.quote(name)}", timeout=300)
            if packed.returncode == 0 and packed.stdout:
                extract(packed.stdout, out)
                print(f"session logs {name} written to {out / name}")
                return 0
        print("pixel10: run-as works but no session folder could be read", file=sys.stderr)
        return 3
    names = text(adb.shell("ls -t /sdcard/Download")).splitlines()
    zips = [n for n in names if SHARE_ZIP.match(n)]
    if zips:
        pulled = adb.run("pull", f"/sdcard/Download/{zips[0]}", str(out / zips[0]), timeout=300)
        print(text(pulled).strip())
        return 0 if pulled.returncode == 0 else 3
    print("pixel10: this build is not debuggable, so adb cannot read the app's private logs.\n"
          "  1. In the app, end the session and tap Share logs (or Setup > Latest logs > Share logs).\n"
          "  2. Send the zip to the PC (Quick Share, Drive, e-mail) or save it to Download, then run this again.",
          file=sys.stderr)
    return 4


def install(adb: Adb, apk: Path) -> int:
    if not apk.is_file():
        print(f"pixel10: no such APK: {apk}", file=sys.stderr)
        return 2
    result = adb.run("install", "-r", "--no-incremental", str(apk), timeout=600)
    output = text(result)
    print(output.strip())
    if "INSTALL_FAILED_UPDATE_INCOMPATIBLE" in output or "signatures do not match" in output:
        print(f"pixel10: another build of {PACKAGE} with a different signature is installed. Uninstalling it deletes\n"
              f"  its runtime and data, so this tool does not: run `adb uninstall {PACKAGE}` yourself, then install again.",
              file=sys.stderr)
        return 4
    return 0 if result.returncode == 0 and "Success" in output else 3


def adb_path(configured: str | None) -> str:
    found = configured or os.environ.get("ADB") or shutil.which("adb")
    for variable in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        sdk = os.environ.get(variable)
        if not found and sdk:
            found = shutil.which("adb", path=str(Path(sdk) / "platform-tools"))
    if not found:
        raise SystemExit("pixel10: adb not found; set ADB to adb.exe or put platform-tools on PATH")
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--adb")
    parser.add_argument("--serial")
    commands = parser.add_subparsers(dest="command", required=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    for name in ("facts", "probe", "logs"):
        commands.add_parser(name).add_argument("--out", type=Path, default=Path("pixel10-runs") / stamp / name)
    commands.choices["probe"].add_argument("binary", type=Path)
    commands.choices["probe"].add_argument("--hal")
    commands.add_parser("install").add_argument("apk", type=Path)
    args = parser.parse_args(argv)
    exe = adb_path(args.adb)
    try:
        adb = Adb(exe, resolve_serial(exe, args.serial, log=True))
    except DeviceResolutionError as error:
        print(f"pixel10: {error}", file=sys.stderr)
        return 3
    if args.command == "facts":
        return facts(adb, args.out)
    if args.command == "probe":
        return probe(adb, args.binary, args.hal, args.out)
    if args.command == "logs":
        return logs(adb, args.out)
    return install(adb, args.apk)


if __name__ == "__main__":
    raise SystemExit(main())
~~~~

### Appendix J: phone-side tooling tests (five files in `tools/tests/`)

#### `tools/tests/pvr_props_probe_test.c`

~~~~c
/* Host test for the pure helpers of tools/pixel10/pvr_props_probe.c: the driver-version decode, the
 * driverInfo parse, the DRM "did the HAL touch it" classifier and the BC format rules. The probe is
 * included whole, minus its main, so the test sees exactly the code that ships. */
#define PVR_PROBE_NO_MAIN
#include "../pixel10/pvr_props_probe.c"
#include <assert.h>

static void test_driver_version(void) {
    char text[96];
    fmt_driver_version(6908880u, text, sizeof text); /* DDK 25.3 on the Pixel 10 */
    assert(!strcmp(text, "6908880 (0x696bd0) vk_decoded=1.662.3024"));
    fmt_driver_version(0, text, sizeof text);
    assert(!strcmp(text, "0 (0x0) vk_decoded=0.0.0"));
    fmt_driver_version(UINT32_MAX, text, sizeof text);
    assert(!strcmp(text, "4294967295 (0xffffffff) vk_decoded=1023.1023.4095"));
}

static void test_driver_info(void) {
    char release[32];
    uint32_t build = 7;
    assert(parse_driver_info("25.3@6908880", release, sizeof release, &build));
    assert(!strcmp(release, "25.3") && build == 6908880u);
    assert(parse_driver_info("25.1@6794074", release, sizeof release, &build) && build == 6794074u);
    assert(parse_driver_info("26.1@4294967295", release, sizeof release, &build) && build == UINT32_MAX);
    build = 7;
    assert(!parse_driver_info(NULL, release, sizeof release, &build));
    assert(!parse_driver_info("", release, sizeof release, &build));
    assert(!parse_driver_info("25.3", release, sizeof release, &build));
    assert(!parse_driver_info("@6908880", release, sizeof release, &build));
    assert(!parse_driver_info("25.3@", release, sizeof release, &build));
    assert(!parse_driver_info("25.3@x", release, sizeof release, &build));
    assert(!parse_driver_info("25.3@4294967296", release, sizeof release, &build)); /* overflows a u32 */
    assert(!parse_driver_info("25.3@6908880", release, 4, &build));                 /* "25.3" does not fit in 4 bytes */
    assert(build == 7); /* a rejected string leaves the output alone */
}

static void test_drm_classifier(void) {
    VkPhysicalDeviceDrmPropertiesEXT d;
    drm_poison(&d);
    assert(d.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT && d.pNext == NULL);
    assert(drm_classify(&d) == DRM_UNTOUCHED);

    memset((unsigned char *)&d + offsetof(VkPhysicalDeviceDrmPropertiesEXT, hasPrimary), 0,
           sizeof d - offsetof(VkPhysicalDeviceDrmPropertiesEXT, hasPrimary));
    assert(drm_classify(&d) == DRM_ZEROED);

    d.hasPrimary = d.hasRender = 1;
    d.primaryMajor = d.renderMajor = 226;
    d.primaryMinor = 2;
    d.renderMinor = 128;
    assert(drm_classify(&d) == DRM_FILLED);

    d.hasPrimary = 0; /* a render-only HAL answer is still a filled one */
    d.primaryMajor = d.primaryMinor = 0;
    assert(drm_classify(&d) == DRM_FILLED);

    d.hasRender = 2; /* not a VkBool32 anyone wrote on purpose */
    assert(drm_classify(&d) == DRM_PARTIAL);

    drm_poison(&d);
    d.hasRender = 1; /* one word written, the rest still poison */
    assert(drm_classify(&d) == DRM_PARTIAL);
}

static void test_bc_rules(void) {
    assert(bc_complete(0xd401u) && bc_sampled(0xd401u)); /* BC1-BC5 as the Pixel 10 reports them */
    assert(!bc_complete(0x1u) && bc_sampled(0x1u));      /* sampled but no blit/filter/transfer */
    assert(!bc_complete(0) && !bc_sampled(0));           /* BC6H / BC7: nothing */
    assert(!bc_complete(0xd401u & ~VK_FORMAT_FEATURE_TRANSFER_DST_BIT));

    unsigned d3d = 0;
    for (size_t i = 0; i < NFORMATS; i++) d3d += (unsigned)FORMATS[i].d3d_bc;
    assert(d3d == 14);
    assert(!FORMATS[4].d3d_bc && !FORMATS[5].d3d_bc && !strcmp(FORMATS[4].name, "BC1_RGB_UNORM_BLOCK"));
    assert(FORMATS[NFORMATS - 1].id == VK_FORMAT_BC7_SRGB_BLOCK);

    char list[16] = "";
    list_add(list, sizeof list, "AB");
    list_add(list, sizeof list, "CD");
    list_add(list, sizeof list, "this name does not fit");
    assert(!strcmp(list, "AB,CD"));
}

int main(void) {
    test_driver_version();
    test_driver_info();
    test_drm_classifier();
    test_bc_rules();
    puts("pvr_props_probe helpers: ok");
    return 0;
}
~~~~

#### `tools/tests/fake_powervr_hal.c`

~~~~c
/* fake_powervr_hal.c - a stand-in for /vendor/lib64/hw/vulkan.powervr.so that host tests can load.
 *
 * It exports the HMI module and answers the handful of Vulkan entry points tools/pixel10/pvr_props_probe.c
 * calls, shaped like the Pixel 10 (driver 25.3@6908880) as measured by gpuinfo.org and the Fizzolas probes:
 * Vulkan 1.4.317, BC1-BC5 sampleable with the spec-mandated features, BC6H/BC7 absent,
 * textureCompressionBC false, VK_EXT_physical_device_drm advertised, HAL device version 0x00000001.
 *
 * The behaviours the tests need to vary come from the environment:
 *   FAKE_HAL_DRM=untouched|zeroed|filled   what the HAL does with VkPhysicalDeviceDrmPropertiesEXT
 *   FAKE_HAL_WIDE_USAGE=1                  gralloc producer usage needs more than 32 bits
 *   FAKE_HAL_VERSION=0x00010000            the hw_device_t version (default 0x00000001)
 *   FAKE_HAL_CRASH=1                       vkGetPhysicalDeviceProperties2 dies with SIGSEGV
 *   FAKE_HAL_DRIVER_INFO=text              VkPhysicalDeviceDriverProperties::driverInfo (default 25.3@6908880)
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "../pixel10/hwvulkan_abi.h"

#define EXPORT __attribute__((visibility("default")))
static int g_instance, g_physical, g_device;
static const VkExtensionProperties EXTENSIONS[] = {
    {"VK_KHR_driver_properties", 1}, {"VK_EXT_physical_device_drm", 1}, {"VK_ANDROID_native_buffer", 11},
    {"VK_KHR_dynamic_rendering", 1},
};
#define NEXT (sizeof EXTENSIONS / sizeof *EXTENSIONS)

static const char *env(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return v && v[0] ? v : fallback;
}

static VkResult enumerate_extensions(uint32_t *count, VkExtensionProperties *out) {
    if (!out) {
        *count = NEXT;
        return VK_SUCCESS;
    }
    uint32_t n = *count < NEXT ? *count : (uint32_t)NEXT;
    memcpy(out, EXTENSIONS, n * sizeof *out);
    *count = n;
    return n < NEXT ? VK_INCOMPLETE : VK_SUCCESS;
}
static VkResult enumerate_instance_extensions(const char *layer, uint32_t *count, VkExtensionProperties *out) {
    (void)layer;
    *count = 0;
    (void)out;
    return VK_SUCCESS;
}
static VkResult create_instance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    (void)ci;
    (void)a;
    *out = (VkInstance)&g_instance;
    return VK_SUCCESS;
}
static void destroy_instance(VkInstance i, const VkAllocationCallbacks *a) { (void)i; (void)a; }
static VkResult enumerate_physical(VkInstance i, uint32_t *count, VkPhysicalDevice *out) {
    (void)i;
    if (out && *count) out[0] = (VkPhysicalDevice)&g_physical;
    *count = 1;
    return VK_SUCCESS;
}
static void get_properties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p) {
    (void)pd;
    memset(p, 0, sizeof *p);
    p->apiVersion = VK_MAKE_API_VERSION(0, 1, 4, 317);
    p->driverVersion = 6908880;
    p->vendorID = 0x1010;
    p->deviceID = 0x71061212;
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    strcpy(p->deviceName, "PowerVR D-Series DXT-48-1536 MC1");
}
static void get_properties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p2) {
    if (!strcmp(env("FAKE_HAL_CRASH", "0"), "1")) raise(SIGSEGV);
    get_properties(pd, &p2->properties);
    const char *drm = env("FAKE_HAL_DRM", "untouched");
    for (VkBaseOutStructure *n = p2->pNext; n; n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
            VkPhysicalDeviceDriverProperties *d = (VkPhysicalDeviceDriverProperties *)n;
            d->driverID = VK_DRIVER_ID_IMAGINATION_PROPRIETARY;
            strcpy(d->driverName, "Imagination Technologies");
            strncpy(d->driverInfo, env("FAKE_HAL_DRIVER_INFO", "25.3@6908880"), sizeof d->driverInfo - 1);
        } else if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT) {
            VkPhysicalDeviceDrmPropertiesEXT *d = (VkPhysicalDeviceDrmPropertiesEXT *)n;
            if (!strcmp(drm, "zeroed")) {
                d->hasPrimary = d->hasRender = 0;
                d->primaryMajor = d->primaryMinor = d->renderMajor = d->renderMinor = 0;
            } else if (!strcmp(drm, "filled")) {
                d->hasPrimary = d->hasRender = 1;
                d->primaryMajor = d->renderMajor = 226;
                d->primaryMinor = 2;
                d->renderMinor = 128;
            }
        }
    }
}
static void get_features(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) {
    (void)pd;
    memset(f, 0, sizeof *f);
}
static void get_format_properties(VkPhysicalDevice pd, VkFormat f, VkFormatProperties *out) {
    (void)pd;
    memset(out, 0, sizeof *out);
    int bc = f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK;
    int missing = f >= VK_FORMAT_BC6H_UFLOAT_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK;
    if (bc && !missing)
        out->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                     VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                     VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    else if (!bc)
        out->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
}
static void get_queue_families(VkPhysicalDevice pd, uint32_t *count, VkQueueFamilyProperties *out) {
    (void)pd;
    if (out && *count) {
        memset(out, 0, sizeof *out);
        out[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        out[0].queueCount = 2;
        out[0].minImageTransferGranularity = (VkExtent3D){1, 1, 1};
    }
    *count = 1;
}
static VkResult enumerate_device_extensions(VkPhysicalDevice pd, const char *layer, uint32_t *count, VkExtensionProperties *out) {
    (void)pd;
    (void)layer;
    return enumerate_extensions(count, out);
}
/* Like the real HAL, a device only exists when VK_ANDROID_native_buffer is asked for. */
static VkResult create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    (void)pd;
    (void)a;
    if (ci->enabledExtensionCount != 1 || strcmp(ci->ppEnabledExtensionNames[0], "VK_ANDROID_native_buffer"))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    *out = (VkDevice)&g_device;
    return VK_SUCCESS;
}
static void destroy_device(VkDevice d, const VkAllocationCallbacks *a) { (void)d; (void)a; }
static VkResult gralloc_usage2(VkDevice d, VkFormat f, VkImageUsageFlags u, VkSwapchainImageUsageFlagsANDROID s, uint64_t *consumer,
                               uint64_t *producer) {
    (void)d;
    (void)f;
    (void)u;
    (void)s;
    *consumer = 0x100;
    *producer = !strcmp(env("FAKE_HAL_WIDE_USAGE", "0"), "1") ? 0x100000000ull | 0x200 : 0x200;
    return VK_SUCCESS;
}

static PFN_vkVoidFunction device_proc(VkDevice d, const char *name) {
    (void)d;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)destroy_device;
    if (!strcmp(name, "vkGetSwapchainGrallocUsage2ANDROID")) return (PFN_vkVoidFunction)gralloc_usage2;
    return NULL;
}
static PFN_vkVoidFunction instance_proc(VkInstance i, const char *name) {
    static const struct { const char *name; PFN_vkVoidFunction fn; } TABLE[] = {
        {"vkDestroyInstance", (PFN_vkVoidFunction)destroy_instance},
        {"vkEnumeratePhysicalDevices", (PFN_vkVoidFunction)enumerate_physical},
        {"vkGetPhysicalDeviceProperties", (PFN_vkVoidFunction)get_properties},
        {"vkGetPhysicalDeviceProperties2", (PFN_vkVoidFunction)get_properties2},
        {"vkGetPhysicalDeviceFeatures", (PFN_vkVoidFunction)get_features},
        {"vkGetPhysicalDeviceFormatProperties", (PFN_vkVoidFunction)get_format_properties},
        {"vkGetPhysicalDeviceQueueFamilyProperties", (PFN_vkVoidFunction)get_queue_families},
        {"vkEnumerateDeviceExtensionProperties", (PFN_vkVoidFunction)enumerate_device_extensions},
        {"vkCreateDevice", (PFN_vkVoidFunction)create_device},
        {"vkGetDeviceProcAddr", (PFN_vkVoidFunction)device_proc},
        {"vkCreateInstance", (PFN_vkVoidFunction)create_instance},
    };
    (void)i;
    for (size_t k = 0; k < sizeof TABLE / sizeof *TABLE; k++)
        if (!strcmp(name, TABLE[k].name)) return TABLE[k].fn;
    return NULL;
}

static int hal_open(const hw_module_t *m, const char *id, hw_device_t **out) {
    static hwvulkan_device_t dev;
    if (strcmp(id, "vk0")) return -22;
    memset(&dev, 0, sizeof dev);
    dev.common.tag = HW_DEVICE_TAG;
    dev.common.version = (uint32_t)strtoul(env("FAKE_HAL_VERSION", "0x00000001"), NULL, 0);
    dev.common.module = (hw_module_t *)m;
    dev.EnumerateInstanceExtensionProperties = enumerate_instance_extensions;
    dev.CreateInstance = create_instance;
    dev.GetInstanceProcAddr = instance_proc;
    *out = &dev.common;
    return 0;
}
static hw_module_methods_t g_methods = {hal_open};
EXPORT hw_module_t HMI = {.tag = HW_MODULE_TAG, .module_api_version = 1, .hal_api_version = 0, .id = "vulkan",
                          .name = "PowerVR D-Series Vulkan HAL (fake)", .author = "tests", .methods = &g_methods};
~~~~

#### `tools/tests/test_pvr_props_probe.py`

~~~~python
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_vk_external_image_sync import vulkan_include_args  # noqa: E402  the NDK header lookup

TESTS = Path(__file__).resolve().parent
PROBE = TESTS.parent / "pixel10" / "pvr_props_probe.c"
STRICT = ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-UNDEBUG"]


def facts(output):
    """Probe output as {key: [values]}: one 'scope.key=value' fact per line, keys may repeat."""
    result = {}
    for line in output.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            result.setdefault(key, []).append(value)
    return result


class PvrPropsProbeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler:
            raise AssertionError("A C compiler is required for the PowerVR property probe tests")
        cls.temp = tempfile.TemporaryDirectory(prefix="droiddeck-pvr-probe-")
        tmp = Path(cls.temp.name)
        cls.probe, cls.fake, cls.helpers = tmp / "pvr_props_probe", tmp / "fake_hal.so", tmp / "helpers"
        include = vulkan_include_args()
        for command in (
            [compiler, *STRICT, *include, str(PROBE), "-ldl", "-o", str(cls.probe)],
            [compiler, *STRICT, *include, "-shared", "-fPIC", str(TESTS / "fake_powervr_hal.c"), "-o", str(cls.fake)],
            # the helper test includes the probe whole, so the probe's own main-only functions are unused there
            [compiler, *STRICT, "-Wno-unused-function", *include, str(TESTS / "pvr_props_probe_test.c"), "-ldl",
             "-o", str(cls.helpers)],
        ):
            built = subprocess.run(command, capture_output=True, text=True)
            if built.returncode:
                raise AssertionError(" ".join(command) + "\n" + built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_probe(self, hal=None, **env):
        ran = subprocess.run([str(self.probe), str(hal or self.fake)], capture_output=True, text=True, timeout=120,
                             env={**os.environ, **env})
        return ran.returncode, facts(ran.stdout)

    def test_helpers(self):
        ran = subprocess.run([str(self.helpers)], capture_output=True, text=True, timeout=30)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_hal_identity_and_driver(self):
        _, f = self.run_probe()
        self.assertEqual(f["section.hal"], ["exit=0"])
        self.assertEqual(f["hal.load"], ["plain"])
        self.assertEqual(f["hal.module.id"], ["vulkan"])
        self.assertIn("HWMT ok", f["hal.module.tag"][0])
        self.assertIn("HWDT ok", f["hal.device.tag"][0])
        self.assertEqual(f["hal.device.vendorID"], ["0x1010"])
        self.assertEqual(f["hal.device.deviceID"], ["0x71061212"])
        self.assertEqual(f["hal.device.apiVersion"], ["1.4.317"])
        self.assertEqual(f["hal.device.driverVersion"], ["6908880 (0x696bd0) vk_decoded=1.662.3024"])
        self.assertEqual(f["hal.driver.name"], ["Imagination Technologies"])
        self.assertEqual(f["hal.driver.info"], ["25.3@6908880"])
        self.assertEqual(f["hal.driver.release"], ["25.3 build=6908880 build_matches_driverVersion=1"])
        self.assertIn("VK_EXT_physical_device_drm", f["hal.ext"])
        self.assertIn("VK_EXT_physical_device_drm=1 spec=1", f["hal.has"])
        self.assertIn("VK_EXT_mutable_descriptor_type=0 spec=0", f["hal.has"])

    def test_hal_device_version_is_reported_against_both_encodings(self):
        for version, upstream, patched in (("0x00000001", 0, 1), ("0x00010000", 1, 1), ("0x00020000", 0, 0)):
            with self.subTest(version=version):
                _, f = self.run_probe(FAKE_HAL_VERSION=version)
                self.assertEqual(
                    f["hal.device.version"],
                    [f"{int(version, 16):#010x} accepted_by_upstream_hybris={upstream} accepted_by_powervr_patch={patched}"])

    def test_drm_properties_state(self):
        for mode, state in (("untouched", "untouched"), ("zeroed", "zeroed"), ("filled", "filled")):
            with self.subTest(mode=mode):
                _, f = self.run_probe(FAKE_HAL_DRM=mode)
                self.assertEqual(f["hal.drm.state"], [state])
        _, f = self.run_probe(FAKE_HAL_DRM="filled")
        self.assertIn("render=226:128", f["hal.drm.fields"][0])
        _, f = self.run_probe(FAKE_HAL_DRM="untouched")
        self.assertIn("hasRender=0xcdcdcdcd", f["hal.drm.fields"][0])

    def test_bc_summary_counts_only_d3d_formats(self):
        _, f = self.run_probe()
        self.assertEqual(f["hal.features"], ["textureCompressionBC=0 textureCompressionETC2=0 textureCompressionASTC_LDR=0"])
        self.assertEqual(f["hal.bc.d3d_native_complete"], ["10/14"])
        self.assertEqual(f["hal.bc.d3d_sampled_but_incomplete"], [""])
        self.assertEqual(f["hal.bc.d3d_not_sampleable"],
                         ["BC6H_UFLOAT_BLOCK,BC6H_SFLOAT_BLOCK,BC7_UNORM_BLOCK,BC7_SRGB_BLOCK"])
        self.assertEqual(len(f["hal.fmt"]), 20)

    def test_queue_families_are_listed_for_the_bc_policy(self):
        _, f = self.run_probe()
        self.assertEqual(f["hal.queue.count"], ["1"])
        self.assertEqual(f["hal.queue.family"], ["0 flags=0x7 count=2 granularity=1x1x1"])

    def test_native_buffer_gralloc_usage_width(self):
        _, f = self.run_probe()
        self.assertEqual(f["hal.native_buffer.spec_version"], ["11 (icd needs >= 8)"])
        self.assertEqual(f["hal.native_buffer.gralloc_fns"], ["v1=0 v2=1 v3=0 v4=0 (icd.c calls v2)"])
        usage = [v for k, vs in f.items() if k.startswith("hal.native_buffer.usage.") for v in vs]
        self.assertEqual(len(usage), 6)
        self.assertTrue(all("exceeds_32_bits=0" in v for v in usage))
        _, f = self.run_probe(FAKE_HAL_WIDE_USAGE="1")
        usage = [v for k, vs in f.items() if k.startswith("hal.native_buffer.usage.") for v in vs]
        self.assertTrue(all("exceeds_32_bits=1" in v for v in usage), usage)

    def test_missing_hal_is_a_fact_and_the_other_section_still_runs(self):
        rc, f = self.run_probe(hal="/nonexistent/vulkan.powervr.so")
        self.assertNotEqual(rc, 0)
        self.assertEqual(f["hal.load"], ["failed"])
        self.assertEqual(f["section.hal"], ["exit=2"])
        self.assertIn("section.loader", f)  # ran (and, on a CI box without a loader, reported why it could not)

    def test_a_crashing_hal_costs_its_section_only(self):
        rc, f = self.run_probe(FAKE_HAL_CRASH="1")
        self.assertNotEqual(rc, 0)
        self.assertEqual(f["section.hal"], ["killed signal=11"])
        self.assertEqual(f["hal.device.name"], ["PowerVR D-Series DXT-48-1536 MC1"])  # printed before the crash
        self.assertIn("section.loader", f)

    def test_driver_text_is_sanitized(self):
        # Control bytes and newlines in a driver string must not forge extra fact lines.
        _, f = self.run_probe(FAKE_HAL_DRIVER_INFO="25.3@1\nforged.key=1")
        self.assertNotIn("forged.key", f)
        self.assertEqual(f["hal.driver.info"], ["25.3@1?forged.key=1"])


if __name__ == "__main__":
    unittest.main()
~~~~

#### `tools/tests/test_pixel10_build.py`

~~~~python
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "build_props_probe", Path(__file__).resolve().parents[1] / "pixel10/build_props_probe.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class BuildPropsProbeTest(unittest.TestCase):
    def fake_ndk(self, root, tag="linux-x86_64", suffix=""):
        clang = Path(root) / "toolchains/llvm/prebuilt" / tag / "bin" / ("clang" + suffix)
        clang.parent.mkdir(parents=True)
        clang.write_bytes(b"")
        return clang

    def test_ndk_home_is_found(self):
        with tempfile.TemporaryDirectory() as tmp:
            clang = self.fake_ndk(tmp)
            self.assertEqual(build.find_clang({"ANDROID_NDK_HOME": tmp}, "Linux"), clang)

    def test_sdk_layout_uses_the_version_the_app_pins(self):
        with tempfile.TemporaryDirectory() as tmp:
            clang = self.fake_ndk(Path(tmp) / "ndk" / build.NDK_VERSION)
            self.assertEqual(build.find_clang({"ANDROID_HOME": tmp}, "Linux"), clang)
            with self.assertRaises(SystemExit):  # another NDK version in the same SDK is not accepted
                build.find_clang({"ANDROID_SDK_ROOT": str(Path(tmp) / "elsewhere")}, "Linux")

    def test_windows_host_runs_clang_exe(self):
        with tempfile.TemporaryDirectory() as tmp:
            clang = self.fake_ndk(tmp, "windows-x86_64", ".exe")
            self.assertEqual(build.find_clang({"ANDROID_NDK_ROOT": tmp}, "Windows"), clang)

    def test_missing_ndk_is_a_clear_exit(self):
        with self.assertRaisesRegex(SystemExit, "NDK 27.3.13750724 not found"):
            build.find_clang({}, "Linux")

    def test_command_targets_the_device(self):
        command = build.compile_command(Path("clang"), Path("p.c"), Path("out"))
        self.assertIn("--target=aarch64-linux-android29", command)
        self.assertEqual(command[-3:], ["-ldl", "-o", "out"])
        self.assertNotIn("-Werror", command)

    def test_only_aarch64_elf_is_accepted(self):
        elf = bytearray(64)
        elf[:6] = b"\x7fELF\x02\x01"
        elf[18:20] = b"\xb7\x00"
        self.assertTrue(build.is_aarch64_elf(bytes(elf)))
        elf[18:20] = b"\x3e\x00"
        self.assertFalse(build.is_aarch64_elf(bytes(elf)))
        self.assertFalse(build.is_aarch64_elf(b"MZ" + bytes(62)))


if __name__ == "__main__":
    unittest.main()
~~~~

#### `tools/tests/test_pixel10_cli.py`

~~~~python
"""tools/pixel10/pixel10.py against a fake adb: what it asks the device and what it writes down."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("pixel10", TOOLS / "pixel10" / "pixel10.py")
pixel10 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pixel10)

LD_CONFIG = b"dir.system = /system/bin/\n[system]\nnamespace.default.isolated = false\n[vendor]\nnamespace.default.isolated = true\n"


def done(stdout=b"", returncode=0, stderr=b""):
    return subprocess.CompletedProcess([], returncode, stdout, stderr)


def tar_of(files):
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w") as archive:
        for name, data in files.items():
            member = tarfile.TarInfo(name)
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
    return raw.getvalue()


class FakeAdb(pixel10.Adb):
    """Answers the first (needle, result) whose needle is in the command line; records every call."""

    def __init__(self, answers=()):
        super().__init__("adb", "fake-1")
        self.answers, self.calls = list(answers), []

    def run(self, *args, timeout=60):
        self.calls.append(args)
        line = " ".join(args)
        for needle, result in self.answers:
            if needle in line:
                return result
        return done()


class Pixel10Test(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.out = Path(self.temp.name) / "out"

    def quiet(self, call, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = call(*args)
        return code, out.getvalue(), err.getvalue()

    def test_facts_are_read_only_and_identify_nothing(self):
        commands = " ".join(command for _, command in pixel10.FACTS + pixel10.RAW)
        self.assertNotRegex(commands, r"serial|imei|meid|android_id|mac_addr|wifi|bluetooth|\bput\b|\bset\b|\brm\b|reboot")

    def test_facts_report_and_raw_files(self):
        adb = FakeAdb([("cat /linkerconfig/ld.config.txt", done(LD_CONFIG)), ("getprop", done(b"ro.board.platform=laguna\n")),
                       ("cmd gpu vkjson", done(b"{}"))])
        code, out, _ = self.quiet(pixel10.facts, adb, self.out)
        self.assertEqual(code, 0)
        report = (self.out / "facts.txt").read_text()
        self.assertIn("### build properties", report)
        self.assertIn("ro.board.platform=laguna", report)
        self.assertEqual((self.out / "vkjson.json").read_bytes(), b"{}")
        self.assertEqual((self.out / "ld.config.system.txt").read_text(), "[system]\nnamespace.default.isolated = false\n")
        self.assertEqual(len(adb.calls), len(pixel10.FACTS) + len(pixel10.RAW))

    def test_section_of_a_missing_header_is_empty(self):
        self.assertEqual(pixel10.section("[a]\nx\n", "system"), "")
        self.assertEqual(pixel10.section("", "system"), "")

    def test_probe_pushes_runs_and_keeps_the_output(self):
        binary = Path(self.temp.name) / "pvr_props_probe"
        binary.write_bytes(b"\x7fELF")
        adb = FakeAdb([("./pvr_props_probe", done(b"section.hal=exit=0\nhal.drm.state=untouched\nhal.fmt=BC7\n"))])
        code, out, _ = self.quiet(pixel10.probe, adb, binary, "/vendor/lib64/hw/vulkan.powervr.so", self.out)
        self.assertEqual(code, 0)
        self.assertIn("hal.drm.state=untouched", out)
        self.assertNotIn("hal.fmt", out)  # the summary is a few lines; the file has everything
        self.assertIn("hal.fmt=BC7", (self.out / "props.txt").read_text())
        self.assertEqual([c[0] for c in adb.calls], ["shell", "push", "shell", "shell"])
        self.assertEqual(adb.calls[1][2], "/data/local/tmp/pixel10/pvr_props_probe")
        self.assertEqual(adb.calls[3][1], "cd /data/local/tmp/pixel10 && ./pvr_props_probe /vendor/lib64/hw/vulkan.powervr.so")

    def test_probe_without_a_binary_is_a_usage_error(self):
        code, _, err = self.quiet(pixel10.probe, FakeAdb(), Path(self.temp.name) / "missing", None, self.out)
        self.assertEqual(code, 2)
        self.assertIn("no such probe binary", err)

    def test_logs_from_a_debuggable_build_take_the_newest_session(self):
        packed = tar_of({"2026-10-10-02-steam/session.log": b"hello"})
        adb = FakeAdb([("ls -1 files/logs", done(b"2026-10-10-01-steam\n2026-10-10-02-steam\ntools\nstores\n")),
                       ("exec-out", done(packed))])
        code, out, _ = self.quiet(pixel10.logs, adb, self.out)
        self.assertEqual(code, 0)
        self.assertEqual((self.out / "2026-10-10-02-steam" / "session.log").read_bytes(), b"hello")
        self.assertIn("-C files/logs 2026-10-10-02-steam", [c for c in adb.calls if c[0] == "exec-out"][0][1])

    def test_logs_refuse_a_tar_that_escapes_the_output_folder(self):
        evil = tar_of({"../evil.txt": b"x"})
        adb = FakeAdb([("ls -1 files/logs", done(b"2026-10-10-01-steam\n")), ("exec-out", done(evil))])
        with self.assertRaisesRegex(SystemExit, "outside the output folder"):
            self.quiet(pixel10.logs, adb, self.out)
        self.assertFalse((self.out.parent / "evil.txt").exists())

    def test_logs_from_a_release_build_use_the_shared_zip(self):
        adb = FakeAdb([("run-as", done(b"run-as: package not debuggable", 1)),
                       ("ls -t /sdcard/Download", done(b"notes.txt\nDroidDeck-2026-10-10-02-steam.zip\nDroidDeck-old.zip\n"))])
        code, _, _ = self.quiet(pixel10.logs, adb, self.out)
        self.assertEqual(code, 0)
        pull = [c for c in adb.calls if c[0] == "pull"][0]
        self.assertEqual(pull[1], "/sdcard/Download/DroidDeck-2026-10-10-02-steam.zip")

    def test_logs_without_any_route_say_how_to_make_one(self):
        adb = FakeAdb([("run-as", done(b"not debuggable", 1))])
        code, _, err = self.quiet(pixel10.logs, adb, self.out)
        self.assertEqual(code, 4)
        self.assertIn("Share logs", err)

    def test_install_never_uninstalls_a_differently_signed_app(self):
        apk = Path(self.temp.name) / "a.apk"
        apk.write_bytes(b"PK")
        adb = FakeAdb([("install", done(b"Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: signatures do not match]", 1))])
        code, _, err = self.quiet(pixel10.install, adb, apk)
        self.assertEqual(code, 4)
        self.assertIn("adb uninstall com.droiddeck.launcher", err)
        self.assertEqual([c[0] for c in adb.calls], ["install"])
        self.assertEqual(adb.calls[0][:3], ("install", "-r", "--no-incremental"))

    def test_install_reports_success(self):
        apk = Path(self.temp.name) / "a.apk"
        apk.write_bytes(b"PK")
        self.assertEqual(self.quiet(pixel10.install, FakeAdb([("install", done(b"Performing Streamed Install\nSuccess\n"))]), apk)[0], 0)

    def test_adb_is_found_in_the_sdk_platform_tools(self):
        tools = Path(self.temp.name) / "sdk" / "platform-tools"
        tools.mkdir(parents=True)
        adb = tools / ("adb.exe" if os.name == "nt" else "adb")
        adb.write_bytes(b"")
        adb.chmod(0o755)
        env = {k: v for k, v in os.environ.items() if k not in ("ADB", "ANDROID_HOME", "ANDROID_SDK_ROOT")}
        with mock.patch.dict(os.environ, {**env, "PATH": "", "ANDROID_SDK_ROOT": str(tools.parent)}, clear=True):
            self.assertEqual(Path(pixel10.adb_path(None)), adb)

    def test_logs_take_the_newest_zip_even_when_its_name_has_spaces(self):
        adb = FakeAdb([("run-as", done(b"not debuggable", 1)),
                       ("ls -t /sdcard/Download", done(b"DroidDeck-2026-10-10-02-steam (1).zip" + bytes([10]) + b"DroidDeck-2026-10-10-02-steam.zip" + bytes([10])))])
        self.assertEqual(self.quiet(pixel10.logs, adb, self.out)[0], 0)
        self.assertEqual([c for c in adb.calls if c[0] == "pull"][0][1], "/sdcard/Download/DroidDeck-2026-10-10-02-steam (1).zip")

    def test_adb_path_order(self):
        with mock.patch.dict(os.environ, {"ADB": "from-env"}), mock.patch("shutil.which", return_value="from-path"):
            self.assertEqual(pixel10.adb_path("from-arg"), "from-arg")
            self.assertEqual(pixel10.adb_path(None), "from-env")
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch("shutil.which", return_value="from-path"):
            self.assertEqual(pixel10.adb_path(None), "from-path")
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch("shutil.which", return_value=None):
            with self.assertRaises(SystemExit):
                pixel10.adb_path(None)


if __name__ == "__main__":
    unittest.main()
~~~~
