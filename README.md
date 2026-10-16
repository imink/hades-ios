# Hades iOS

An experimental native ARM64 compatibility layer for running your own Nintendo
Switch copy of **Hades** on iPhone. Based on
[D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile), with a separate Hades
runtime, UIKit app, resource preparation and validation pipeline.

中文：[README.zh-CN.md](README.zh-CN.md)

![Hades running on iPhone with a touch joystick, action buttons and the FPS display](docs/images/hades-ios-gameplay.png)

*User-provided gameplay screenshot. The displayed FPS is a single observation,
not a sustained-performance guarantee.*

**This is a source-and-tools project, not a downloadable game or a completed
commercial-quality port.** Bring your own legally obtained game files and keys.
Do not upload game programs, resources, saves, keys or locally built packages.
This project is not affiliated with Supergiant Games, Nintendo or Apple.

## What it is, and what it is not

**It is:** a Hades-specific platform adaptation that executes the game's ARM64
code natively, supplies the required host services and adds an iPhone touch
interface. Graphics use **Vulkan → MoltenVK → Metal**.

**It is not:** CPU emulation, cloud gaming, a game download, a general Switch
compatibility layer or the D2R port with different branding. D2R's game-specific
patches, cloud saves, device coverage and performance claims do not carry over.

## Battle-tested

The latest recorded device checkpoint is **build 37**, on an **iPhone 15 Pro Max
(A17 Pro), iOS 26.7.1**. Device logs and user feedback establish the following
bounded results. Unlike the upstream D2R project's broad test matrix, this is
**single-device evidence**, not full-game compatibility:

| Area | Verified checkpoint |
|---|---|
| Game and graphics | Native game entry and visible gameplay; Hades uses **Vulkan → MoltenVK → Metal**, not the upstream D2R NVN renderer |
| Controls | Analog touch stick, simultaneous movement/action input, whole-circle action-button hit targets and pressed appearance |
| Display | Installed `wide-720p` profile at 1560×720; left trait HUD inset confirmed on the tested phone |
| App switching | Build 34 resumed the same scene, audio and controls through two logged app-switch cycles |
| Startup | Build 36 automatically verifies resources and enters the game; loading ends only after a successful presentation |
| Diagnostics | Tap the top-right **FPS** display, then **View / export local logs**; logs stay local unless explicitly exported |
| App identity | Build 37 displays **Hades** and the original game icon prepared from the user's local game files; confirmed on the Home Screen |
| Updates | The recorded build-37 installation preserved all 12 state/save files by before/after hash comparison |

Full-game completion, long sessions, all interruptions and other devices remain
unverified. The upstream D2R project's device matrix, 60 fps claims, iCloud
features and screenshots **do not describe Hades**.

## Fast, smooth, and easy on the battery

Native execution avoids CPU emulation, but this project does **not** yet have
validated sustained frame-rate, power or battery-life measurements. The screenshot
above is a real user-provided example, not a benchmark.

| Setting or observation | Hades status |
|---|---|
| Default render size | 1280×720 |
| Tested widescreen profile | 1560×720 (`wide-720p`), with build-specific HUD overlays |
| 1080p | Experimental; rolled back on the tested phone, not the recommended baseline |
| FPS display | Counts successful Vulkan presentations over a monotonic time window |
| Foreground stability check | Build 33 passed a user-reported ten-minute play session; logs reached 34,200 presentations without the earlier capture-budget failure |
| Power and thermals | No published Hades measurement or battery-life guarantee |

Heavy GPU pixel/pass capture is off by default. It is a diagnostic mode that
changes timing and can hit a readback memory limit, not a normal gameplay feature.
The full resource archive is approximately 3.9 GB and is transferred separately
from the app; do not reuse the D2R package-size figures.

## Redesigned for fingers

An analog stick on the left supports movement and diagonals. Eight action buttons
publish game input, and movement can be combined with actions. The main circular
buttons respond across their whole hit area, with a visible pressed state;
release, cancellation and leaving the foreground clear held input.

The top-right FPS display expands a monitor panel containing the local-log entry.
There is no separate bottom-center log button. Native light button haptics are
implemented, but physical feedback still needs verification and is not the
game's controller-rumble protocol.

### Controller

The touch frontend publishes virtual controller input to the guest. This does
not establish external Bluetooth/USB controller compatibility or inherit D2R's
automatic controller/touch switching. Physical controllers and guest rumble are
not claimed as verified.

## New ways to play, on top of the original

The changes here adapt startup, display and controls to iPhone; they do not add
D2R's Greater Rifts, loot rules, stash systems or other game-specific modes.
The goal is to preserve Hades gameplay rather than redesign its rules.

### Direct launch and in-game diagnostics

Normal launch shows resource-verification progress and starts the game once in
the active foreground. Controls become available after the first successful
frame. Failures expose diagnostics instead of silently skipping checks.
Logs can be viewed/exported through the FPS monitor; opening diagnostics does
not pause or restart the running game.

### Widescreen and HUD layout

The opt-in `wide-720p` profile adjusts the verified build's virtual canvas and
uses hash-guarded, read-only UI overlays. The tested phone's left trait HUD is
shifted away from the screen edge. This is not universal notch/safe-area support,
and the separate vignette-mask candidate remains unconfirmed.

### Storage: local saves and safe updates

`Documents/HadesState` contains the local offline identity and save namespace.
The host uses snapshot transactions and recovery checks; this is not iCloud
sync or a guarantee against every power-loss scenario.
Updating the same authorized bundle ID in place preserves the container, but
each update should still be backed up and checked by file hashes.

### Everyday play and app switching

Cooperative pause/resume parks guest execution at safe boundaries, drains GPU
work and pauses audio. Returning to the foreground restores the surface, audio,
input and guest clock. The tested app-switch cycles continued in the same scene.
This does not run the game in the background, force a save or prevent iOS from
reclaiming the process. A failed safe transition retains the explicit exit guard.

### English and Chinese

This repository maintains matching English and Chinese documentation. The
Hades package's own content determines available game languages; the D2R
custom-language menu and its translations are not part of this port.

## How it works

The original ARM64 program is adapted ahead of time for Apple's execution
environment. The `portable-abi` rewriter profile excludes D2R-specific gameplay
patches. A native host supplies the required subset of platform services,
threading, local storage, audio and input. Vulkan calls are forwarded through
pinned public MoltenVK frameworks to Metal; shaders and resource bindings are
prepared locally. No CPU JIT, remote game streaming or D2R executable is used.

| Path | Purpose |
|---|---|
| [`hades/ios/`](hades/ios/) | UIKit startup/loading UI, touch controls, FPS monitor and diagnostics |
| [`hades/runtime/`](hades/runtime/) | Native host, Vulkan bridge, audio, local saves and cooperative lifecycle |
| [`hades/tools/`](hades/tools/) | Extraction, ABI rewriting, shader preparation, packaging and signing |
| [`hades/tests/`](hades/tests/) | Synthetic ABI, runtime, UIKit, packaging and signing regressions |
| [`hades/toolchain.json`](hades/toolchain.json) | Pinned public tool/framework revisions and archive hashes |
| [`hades/target.json`](hades/target.json) | Accepted game identity and recorded compatibility checkpoints |
| `dm/` | Retained upstream code, including shared rewriting/embedding helpers; not a Hades setup entry point |

## Getting started

**Do not run `setup.sh`, `product.sh` or the D2R iOS project with Hades files.**
Use the Hades tools below. Prepare the following before building:

| Requirement | Details |
|---|---|
| Build machine | Apple silicon Mac with full Xcode and the iPhoneOS 26 SDK selected; native probes also require macOS frameworks |
| Python | Python 3 in an isolated virtual environment, with [`hades/requirements.txt`](hades/requirements.txt) installed |
| Build tools | Git, Make and CMake for the pinned NSTool/SPIRV-Cross builds; Xcode's `clang`, `actool`, `codesign` and LLVM tools; macOS `sips` for local icon generation |
| Graphics | Official public-API **MoltenVK v1.4.2** static frameworks and original release archives, matching `hades/toolchain.json` |
| Game input | Your own Hades base NSP: title ID `0100535012974000`, content version `0`; the recorded display version is `1.0.34820` |
| Keys | Your own matching keys at an explicit local path; never commit them or paste key values into commands/logs |
| Phone | Current target is ARM64, **iOS 26+**; only the iPhone 15 Pro Max checkpoint above is established |
| Signing | Your own valid Apple signing identity, matching provisioning profile and authorized device; ad-hoc signing is insufficient for installation |
| Storage | Plan for at least **10 GB** for asset extraction/transfer/installation/caches, plus tools and retained build/backup copies |

Older Hades revisions, updates and other package layouts are not interchangeable:
the pipeline checks identity, hashes and build-specific inputs. Stop on a mismatch
rather than bypassing a check. The default build is **1280×720**; widescreen is
opt-in. The iOS package compiles MSL through the device's Metal runtime; a separate
Metal CLI is needed only for the optional offline `--compile` shader path.

### With a Mac: three steps

The following commands run from this repository's root. Keep every game-derived
output in a private directory **outside the checkout**. Output directories must
be fresh; do not delete working builds or saves just to reuse a path.

**1. Prepare tools and a private workspace.**

```sh
git clone https://github.com/imink/hades-ios.git
cd hades-ios
python3 -m venv "$HOME/hades-tools/venv"
PY="$HOME/hades-tools/venv/bin/python3"
"$PY" -m pip install -r hades/requirements.txt

WORK="$HOME/hades-work"
NSP="/absolute/path/to/your/Hades.nsp"
KEYS="/absolute/path/to/your/prod.keys"
NSTOOL="/absolute/path/to/nstool"
CROSS="/absolute/path/to/spirv-cross"
LLVM="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin"
MOLTENVK_ROOT="/absolute/path/to/private/MoltenVK-workspace"
```

Build NSTool and SPIRV-Cross outside this repository at the exact revisions in
[`hades/toolchain.json`](hades/toolchain.json). NSTool requires its pinned
submodules, `make deps`, then `make`; SPIRV-Cross uses the manifest's CMake options.
Adjust `LLVM` if Xcode is installed elsewhere.

The MoltenVK workspace must retain `MoltenVK-ios.tar` and, for Mac Vulkan tests,
`MoltenVK-macos.tar`, with their original contents extracted beneath `ios/` and
`macos/` respectively. For example, the iOS library is under
`ios/MoltenVK/MoltenVK/static/MoltenVK.xcframework/ios-arm64/`.
The tools verify archive hashes and extracted members; an arbitrary framework
directory or a private-API build is not a substitute.

**2. Extract, adapt and validate your game.**

```sh
"$PY" hades/tools/prepare.py --work "$WORK" extract \
  --package "$NSP" --keys "$KEYS" --nstool "$NSTOOL"
"$PY" hades/tools/prepare.py --work "$WORK" rewrite --llvm-bin "$LLVM" --jobs 2
"$PY" hades/tools/prepare.py --work "$WORK" rewrite-dynamic --llvm-bin "$LLVM" --jobs 2
"$PY" hades/tools/prepare.py --work "$WORK" embed
```

Extraction validates the base title, content sizes and hashes. Rewriting checks
the resident modules and dynamic EOS module, then verifies rewritten instructions
and embedded bytes. Successful extraction or embedding alone does not prove
gameplay, licensing or device compatibility.

Prepare shaders and their binding catalog, then run the regressions:

```sh
"$PY" hades/tools/translate_shaders.py \
  --source "$WORK/data/RomFS/Content/EffectsForgeSwitch" \
  --out "$WORK/build/shaders-msl" --spirv-cross "$CROSS"
"$PY" hades/tools/reflect_bindings.py --work "$WORK" \
  --msl "$WORK/build/shaders-msl" --spirv-cross "$CROSS" \
  --out "$WORK/build/bindings"
HADES_MOLTENVK_ROOT="$MOLTENVK_ROOT" \
  "$PY" -m unittest discover -s hades/tests -v
```

Tests use synthetic inputs where possible; native UIKit/Metal tests need the
corresponding Mac environment. The Vulkan test needs the pinned macOS framework.
Report skipped tests explicitly rather than treating them as verification.
Additional real-game native probes are described in the
[engineering guide](hades/README.md#reproduce-the-local-preparation).

**3. Package, sign locally and install.**

```sh
"$PY" hades/tools/package_ios.py --work "$WORK" \
  --bindings "$WORK/build/bindings/bindings.json" \
  --out "$WORK/build/ios-package" --moltenvk "$MOLTENVK_ROOT" \
  --render-resolution wide-720p
```

Omit `--render-resolution wide-720p` for the 1280×720 default. Packaging derives
the Home Screen icon from the verified extraction's
`data/control/icon_AmericanEnglish.dat` and uses **Hades** as the display name;
missing or invalid local artwork is an error, not a downloaded fallback.
This creates an
ad-hoc-signed `HadesPortTest-resign-required.ipa` and a separate
`Hades-RomFS-local-only.zip`. A successful build is **not** an Apple-signed or
device-validated release. Keep intrusive `--gpu-capture-probe` diagnostics off
for ordinary play; they can terminate at their readback memory limit.

Use your own signing certificate/profile:

```sh
"$PY" hades/tools/sign_ios.py --work "$WORK" \
  --package "$WORK/build/ios-package" --out "$WORK/build/apple-signed" \
  --profile "/absolute/path/to/profile.mobileprovision" \
  --identity "<40-character signing certificate SHA-1>" \
  --device "<provisioning UDID>" --bundle-id "<profile-authorized bundle ID>"
```

The tool validates identity, expiry, team, device, bundle ID and memory
entitlements. Do not silently strip unsupported grants. Personal-Team diagnostic
mode requires the explicit `--diagnostic-without-memory-entitlements` option;
success on the recorded phone does not prove reduced-entitlement compatibility
elsewhere. Alternatively, use the documented
[Xcode team-selection handoff](hades/README.md#native-xcode-team-selection-handoff).

Connect, unlock and trust the phone over USB, enable Developer Mode, and install
the locally Apple-signed app using Xcode or local Apple device tools. Transfer the
**extracted full `RomFS` directory** to the app's `Documents/RomFS` through local
USB file sharing; copying only the ZIP or startup resources is insufficient.
The separate archive avoids exceeding the app bundle's size limit.

Launch in the foreground. The app verifies every resource, starts the game once,
and enables controls after the first successful frame. Verification/startup
failures reveal diagnostics rather than skipping checks.

**Preserve saves when updating.**

Close the app normally and confirm its process has stopped before copying data.
Back up all of `Documents/HadesState` privately, along with any other state/save
files in the existing container. Record file hashes, keep the same authorized
bundle ID, and update in place **without uninstalling**. Compare state/save hashes
after installation before continuing play. Retain the previous signed build and
backup for rollback. A successful install alone is not proof that saves survived.

### Without a Mac: the shell plus the packaging tool

That route belongs to the upstream D2R project. **There is currently no supported
Mac-free Hades packaging route or downloadable content-free Hades shell.**
The Hades workflow requires the Apple toolchain and local signing described
above. Do not submit game files or keys to online build/signing services.

## Known limitations

- **Device coverage:** no general iPad, older-iPhone, older-iOS, simulator or
  macOS end-user support claim. Mac probes are engineering evidence, not a
  supported desktop release.
- **Stability and performance:** no full-game completion, sustained 60 fps,
  battery-life or thermal guarantee. The experimental 1080p profile was rolled
  back on the tested phone; it is not the recommended baseline.
- **Background behavior:** cooperative pause/resume is implemented for game
  mode, not background gameplay. A failed 1.5-second transition still exits with
  status 75. GPU/SDK-only diagnostics remain foreground-only. Lock-screen and
  other interruption scenarios need separate verification.
- **Memory and progress:** iOS can reclaim a suspended app. Pausing neither
  forces a save nor reduces memory use; unsaved progress can be lost. The 2 GiB
  guest heap policy is not a measured physical-memory requirement or a guarantee
  against termination.
- **Display and input:** widescreen overlays and the fixed trait-HUD inset are
  build/device-specific, not universal safe-area handling. The vignette-mask
  candidate is not confirmed fixed. Native button haptics are implemented, but
  physical feedback remains unverified; this is not guest controller rumble.
- **Platform services:** the host implements a Hades-focused subset, not all
  Switch services. The headless/no-input library-applet route is incomplete.
  Local saves do not imply iCloud sync or compatibility with arbitrary imported
  saves; upstream D2R cloud/controller features are not inherited.
- **Distribution:** no game assets, keys, generated game binaries, IPA downloads
  or online signing/asset service. Built packages and logs may contain private
  content; inspect logs before sharing.

## Contributing and feedback

Read [CONTRIBUTING.md](CONTRIBUTING.md) for the inherited repository rules, but
use the Hades commands above rather than the D2R-specific setup/acceptance flow.
Keep English and Chinese documentation in sync and preserve bilingual code
comments where required. Changes to the shared rewriter must keep D2R-specific
behavior isolated from the `portable-abi` profile.

Reports should identify the commit/build, device and iOS version, render
profile, exact reproduction steps and whether the failure occurs during
preparation, launch, gameplay or resume. State which checks ran and which were
skipped. Share only reviewed, sanitized log excerpts; keep game assets, saves,
credentials, device identifiers and full private receipts out of public issues.

## Documentation by part

| Document | Contents |
|---|---|
| [Hades engineering guide](hades/README.md) / [中文](hades/README.zh-CN.md) | Detailed build history, device evidence, probes, signing and troubleshooting |
| [Engineering lessons](hades/LESSONS.zh-CN.md) | Investigation findings and pitfalls, in Chinese |
| [Target manifest](hades/target.json) | Accepted title/version and recorded checkpoints |
| [Toolchain manifest](hades/toolchain.json) | Pinned public dependencies and framework hashes |
| [Regression sources](hades/tests/) | ABI, runtime, UIKit, packaging and signing checks |
| [Contributing](CONTRIBUTING.md) / [中文](CONTRIBUTING.zh-CN.md) | Repository contribution rules; D2R-specific commands are not Hades instructions |

The engineering guide retains historical per-build observations, including
limitations that later builds addressed. Use the latest relevant checkpoint
rather than interpreting an early diagnostic result as current acceptance.

## Legal

Use only game files and keys you lawfully possess. This repository does not
provide game programs, extracted resources, game-derived build outputs or keys.
The user-provided screenshot documents the running interface; it is not a
redistributable game asset pack. Locally generated app icons and packages remain
private. Hades and its content belong to their respective rights holders.
This is an independent project, not an official Supergiant Games, Nintendo or
Apple release.

## License

[MIT](LICENSE). Based on
[D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile); preserve its attribution
and the existing notices. Third-party components retain their own licenses,
including MoltenVK's Apache-2.0 license. The source-code license does not grant
rights to distribute Hades game content.
