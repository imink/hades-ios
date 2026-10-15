# Hades target: iOS graphics milestone, full port incomplete

中文：[README.zh-CN.md](README.zh-CN.md)

This target is **not a completed playable port**. V22 has reached an actual-device
graphics milestone: the user reports success, and both observed rendering windows
have zero zero-draw passes after the directory-order adjustment. Full-session
stability and background suspension remain unverified or incomplete.
Local extraction, four resident modules and
EOS NRO platform-ABI rewriting, SPIR-V-to-MSL translation, an iOS ARM64 embedding
object, macOS native AOT module/timer calls, and offscreen GPU readback using two
actual shaders have been verified. A separate iPhoneOS ARM64 diagnostic app now
links the five images; the complete RomFS is delivered as a separate private
local archive. Its local ad-hoc integrity
signature is not an Apple device-installation signature: re-signing is required.
The explicit host-service profile now completes all six SDK exports and four
resident constructors and enters actual `nnMain` on macOS. The Vulkan profile
reads resources, commits saves, passes native socket/HID initialization, creates
112 shader modules and executes indexed draws, queue submits and presents.
Real PCM output buffers complete and recycle through the native SDK.
Own-swapchain GPU readback and controlled keyboard input now verify the main
menu, options page and return to the menu. A separate fresh-save route runs for
125 seconds/14546 presents with zero SUBOPTIMAL at fixed 1280x720; its later
scenes are not validated gameplay. Without input the stop remains SelfController
command 9 (library-applet launchable event). The library-applet backend remains
incomplete. Device launch and the V22 user-reported visible graphics milestone
have passed the checkpoints below; a complete playable session remains unverified.
`target.json` defines the base application `0100535012974000`, content version 0.
Build IDs, hashes, imports and generated game content remain in private local
receipts, not versioned address tables.

## iPhone 15 Pro Max device checkpoint (2026-10-14)

Version 10 has been Apple development-signed, installed and launched on the
user-authorized iPhone 15 Pro Max / A17 Pro / iOS 26.7.1. The Personal Team
diagnostic does not request either extended memory entitlement; this establishes
only the stages reached on this device, not compatibility under other systems,
devices or memory pressure. Account/device identifiers and full logs remain private.

| Stage | Actual evidence | Not established |
|---|---|---|
| Native core | Two `CORE_OK`/status 0 runs; 200398 relocations, EOS calls, TSD/timer gates | Full guest concurrency correctness |
| Offscreen GPU | 72 MSL stages, three `FRAME_OK`, `RENDER_OK`, status 0 | Guest Vulkan game pixels/visible presentation |
| SDK | Six initializers, SDK `DT_INIT`, `HOST_BOOTSTRAP_RETURN`, status 0 | Complete gameplay |
| Game entry | Device SHA-256 checks of all 3066 resources, real `nnMain`, at least 2400 successful A17 Pro Vulkan presents | Screen contents, touch consumption or movement/combat |
| Storage/audio | `SAVE_COMMIT_OK`, actual audio service opened | Save recovery after relaunch or audible output |

At presentation 2400, observed resident usage was about 714 MB, physical footprint
1.77 GB and Metal allocation 903 MB. These overlap: do not sum them or interpret
them as a long-term stability/Jetsam limit. Verify menu/options, level entry,
movement/attack/dash, sound and persisted save recovery on the foreground device
before claiming playable. The user subsequently confirmed entering a scene and
playing for several minutes, but version 10 crashed as below: stable playability
is not established. The foreground-only diagnostic background-abort remains.

## Version 11 descriptor allocation fix

Version 10 recorded at least 10200 successful device presents before SIGSEGV.
Matching the installed binary's symbols and actual Mach-O load base identifies
the fault as a null `VkDescriptorSet` dereference in MoltenVK's
`mvkUpdateDescriptorSetWithTemplate`. Earlier `vkAllocateDescriptorSets` calls
returned `VK_ERROR_OUT_OF_POOL_MEMORY`. The CPU resource report took no termination
action; this was not the foreground-only abort, and footprint alone does not
establish Jetsam. Device saves were privately backed up over USB, not deleted/uploaded.

Actual guest batches request 6144/6528 sets with three uniform-buffer descriptors
per layout, whereas the respective original pool only budgets 8192 descriptors.
Version 11 computes exact demand from registered layouts and provisions a real
driver backing pool only after pool-capacity/fragmentation failure. It preserves
the guest maxSets budget, device/allocator ownership and free/reset/destroy.
There is no arbitrary size multiplier, fabricated handle or success; real memory
failures remain errors and null-set updates are explicitly rejected.

O0/O2 real Metal probes cover insufficient descriptor quotas, template updates,
maxSets exhaustion, duplicate-free rejection and free/reset/destroy. All 72
regressions passed. A visible native macOS run using a copy of the phone save and
controlled input reached at least 13200 presents without descriptor allocation
failure, SIGSEGV or unsupported services. This is not fixed-device stability
evidence: version 11 was development-signed and USB-updated, but device lock
prevented automatic launch. Unlock and foreground-test option 4 in the original scene.

## Version 12 iOS texture diagnostics

The user confirmed gray foreground/HUD silhouettes on the version 11 device,
with correct textures on Mac. Version 11 subsequently recorded at least 10800
successful presents without descriptor allocation failures; its final exit was
the foreground-only abort, not the previous SIGSEGV. Scene correctness remains open.

Version 12 preserves rendering behavior and adds bounded observations of actual
image formats/extents/mips/layers, view component mappings, Metal/Vulkan compressed
texture capabilities and BC format queries. The Mac run uses BC7_UNORM_BLOCK
(145), BGRA8, RGBA8 and R8, with R/G/B/A view components and BC feature support.
Do not extrapolate this to the iOS driver. Compare the device
`METAL_TEXTURE_CAPABILITY`, `TEXTURE_COMPRESSION_FEATURES`, `TEXTURE_FORMAT_QUERY`,
`TEXTURE_IMAGE_CREATED` and `TEXTURE_VIEW_CREATED` before attributing the issue to
compressed formats, resource selection or sampling. No speculative color replacement
is claimed as a fix. Version 12 passed regression, development signing and USB update,
but device lock prevented automatic launch. Unlock and foreground-run option 4 to
collect the device comparison; saves and RomFS are not recreated.

## Version 13 single-variable binding experiment

Version 12 device evidence is now available: A17 Pro Metal BC and Vulkan
BC/ETC2/ASTC capabilities are true. BC7 (145) supports sampling, linear filtering
and transfers; actual image formats and R/G/B/A view mappings match Mac, with no
driver allocation errors. Do not attribute gray silhouettes to absent BC7 support.
The user also reports flicker; resource binding and cross-frame synchronization
still need to be distinguished. Root cause remains open.

Version 13 explicitly selects `HadesVulkanBindingMode=direct`. Before the first
Vulkan initialization it sets MoltenVK's official
`MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0`, selecting discrete resource indexes.
`argument-buffers` is accepted for comparison; missing/invalid mode or environment
configuration failure rejects launch. Materials, shaders, texture data, queue
synchronization and compressed-format capability reporting are unchanged.
The Mac driver confirms discrete indexes; a 90-second controlled scene run reached
at least 9000 successful presents and four owned-GPU readbacks without descriptor
failure or SIGSEGV. All 72 regressions passed. Version 13 is Apple-signed,
USB-installed and launched, but the game entry has not yet been selected.
Validate both color and flicker in the original device scene before calling this
experimental configuration an iOS fix.

## Version 14 synchronization comparison (user confirms no improvement)

The user reports unchanged gray textures and flicker on version 13; device logs
confirm discrete resource indexes were actually used. That binding change did
not resolve either symptom. Version 14 restores the original argument-buffer
configuration and explicitly selects `HadesVulkanQueueCompletionProbe=true`.
It executes real `vkQueueWaitIdle` after each successful `vkQueueSubmit`, logging
`QUEUE_COMPLETION_PROBE_WAIT_OK` only on completion and returning real driver
errors on failure. This serializes GPU work and changes timing/performance:
it is a diagnostic for reuse/synchronization, not a final solution.

All 72 regressions passed; O0/O2 real GPU probes cover enabled/disabled completion
waits. Version 14 is development-signed with 15 embedded sections and 293 HadesData
resources verified. After reconnection, no Hades process was running; USB
installation and launch succeeded. The latest launch log confirms `build=14`
and argument-buffer configuration. All 10 save files have identical SHA-256
hashes before and after update; the app was not uninstalled. The user has entered
the real game. Current-build logs confirm all 3066 RomFS files verified, at least
12000 successful post-submit queue waits and 4800 successful presentations, with
no crash or driver error recorded in this observation. The user confirms both
missing colors and flicker persist: post-submit queue completion did not resolve
either symptom and must not be treated as a fix. This does not rule out
within-submit pass visibility, attachment stores or CPU upload problems.
These counts do not prove correct pixels or establish synchronization as the root cause.
An attempted low-level upload block readback could not access
valid source buffers and was removed; no texture pixel correctness is inferred
from it. Root causes of gray textures and flicker remain unconfirmed.

## Version 15 pixel diagnostics (device scene captured, unresolved)

This follows D2R's texture-dump and attachment/barrier investigation method,
not its NVN addresses, format numbers or Metal 4 replay patches. It retains the
actual Vulkan route, disables the unsuccessful queue-completion experiment,
restores the argument-buffer baseline, and explicitly enables
`HadesVulkanPixelCaptureProbe=true`. Only selecting the real game entry creates
a private `Documents/HadesRenderProbe-<UUID>` directory. Every 600 presentations,
at most four times, it reads this app's own GPU resources, never the desktop or
another app.

The diagnostic saves the final swapchain RGB image and an inventory of up to
512 live images with RGB thumbnails no larger than 64x64. Real GPU reads cover
R8/RG8/RGBA/BGRA/BC7 and layer zero of 2D arrays; sRGB reads undergo actual GPU
conversion. The manifest records serial, handle, format, dimensions, usage and
status. Unsupported formats, unavailable native textures and registry overflows
are explicit, not pixel-validation successes. Files are exclusively created
with mode 0600; destroying an image synchronously removes its diagnostic record.
Bounded logs also describe the first 32 render passes' load/store/layout and
dependencies and the first 64 pipeline barriers' stages, access and transitions,
without changing the guest commands.

Extra GPU work and waits perturb timing: this is not a production fix.
Thumbnails describe **frame-end base storage, mip zero/layer zero**, not snapshots
at each pass boundary. They omit alpha and do not establish correct guest views,
samplers, descriptors or fragment programs, or identify a missing object alone.

O0/O2 with completion waits enabled/disabled verify real Vulkan uploads and
exact known RGB from R8/RGBA/BGRA/BC7, array layer zero, same-image reuploads,
removal on destruction, exclusive-file rejection and pass/barrier log fields.
All 72 regressions passed. A bounded 45-second Mac game run from an independent
save copy recorded at least 4800 successful presentations and four snapshot
groups with 1340 PPM files. Each of the last three groups inventories 406 images:
338 sampled, 6 unsupported format/usage, and 62 unavailable/unsupported native
textures, with no registry overflow or recorded native crash/driver failure.
Those missing images remain investigation gaps, not complete draw coverage.

The final iPhoneOS package is Apple-signed; 15 embedded sections, 293 resources
and frozen source hashes are verified. After reconnection, no Hades process was
running; USB installation and launch succeeded. The launch log confirms build=15
and its frozen build receipt. All 10 save files have identical SHA-256 hashes
before and after update; the app was not uninstalled and no game was interrupted.
The user entered gameplay; USB retrieval verified four final frames and 1662
texture thumbnails (1666 PPM files total). The last three groups each record
444 sampled images, 6 unsupported and 62 unavailable native textures. The
registry overflow counter reaches 41, so this is incomplete resource coverage.
The final-frame images visibly contain large black silhouettes.

A second bounded Mac run using an independent copy of the pre-update save
entered the actual house scene rather than stopping at the menu. All 279 unique
BC7 RGB thumbnail hashes captured on iOS occur in that Mac scene capture, and
vice versa. This establishes agreement of the observed base-mip/layer-zero RGB
storage, not correct alpha, other mips/layers, actual draw sampling or attachment
composition. R8 movie planes and render targets vary across non-synchronized
animations/cameras and cannot be declared corrupt from different hashes alone.
The Mac scene capture also overflows the 512-image registry (counter 32).
Gray textures and flicker remain unresolved; alpha/view/descriptor/per-pass
evidence and complete resource registration are still needed.

### Version 16 alpha and inventory diagnostics

Version 16 raises the live-image inventory limit to 2048 and writes a separate
`image-<serial>-frame-<frame>.ppm.alpha.pgm` grayscale alpha thumbnail from the
same GPU readback as each RGB thumbnail. PGM files use exclusive mode-0600
creation; missing/failed alpha output fails the capture rather than reporting
RGB-only success. The base mip/layer-zero and nearest-downsampling limits still
apply. Capture-enabled image/view creation logs cover up to 2048 entries and
include view handles, allowing source-image/view correlation without assuming
identical creation order on Mac and iOS.

GPU regressions use non-opaque RGBA/BGRA alpha values and change alpha on a
same-image reupload. A 600-live-image inventory test exceeds the old 512 limit
and verifies removal of all entries after destruction. This is diagnostic
coverage, not a rendering fix. All 72 regressions passed. A bounded Mac scene
run produced 1770 RGB PPM files and 1766 alpha PGM files. Its last three groups
inventory 544 live images, with 476 sampled, 6 unsupported and 62 unavailable;
all four groups report zero registry overflows.

The final v16 Apple signature and all 15 embedded sections were verified.
After checking that Hades was not running, USB update and host launch succeeded;
all 10 save files have identical pre/post-update hashes. The launch log confirms
build 16 and its frozen receipt. After the user selected the fourth entry,
USB retrieval obtained 1770 RGB PPM and 1766 alpha PGM files. All four groups
report zero registry overflows; the final group inventories 568 images, with
500 sampled, 6 unsupported and 62 unavailable. All 279 unique BC7 RGB-plus-alpha
thumbnail pairs match the actual Mac scene capture exactly. Observed image
views also agree on BC7 format, identity component mapping and a single base
mip/layer. This still does not establish descriptor/draw or per-pass correctness.

### Version 17 attachment-load comparison

Both scene logs' first 32 render passes declare color `DONT_CARE` load with
`STORE`, color-attachment initial/final layouts and no pass dependencies. These
declarations alone are not a proven bug. Version 17 explicitly enables
`HadesVulkanAttachmentLoadProbe`: a comparison that changes only color-to-color
`DONT_CARE` loads to `LOAD` in a temporary copy passed to the real driver.
The original guest structures, clear/load operations, other layouts, depth,
stencil, store operations and dependencies remain unchanged. The runtime
defaults to disabled unless `HADES_VULKAN_ATTACHMENT_LOAD_PROBE=1`; iOS requires
an explicit boolean metadata value and logs each bounded change.

O0/O2 with both load and completion probes on/off verify real GPU color and
alpha preservation across an attachment pass, the exact selection rule and
unchanged caller structures. This tests a tiled-GPU content-discard hypothesis,
not a confirmed fix or a claim that undefined Vulkan attachment contents have
portable semantics. Device comparison and visual confirmation are required.

The signed v17 was installed only after the user exited the running game;
all 10 save hashes were preserved. The device log confirms 32 actual load
changes and four successful zero-overflow texture snapshots. The user reports
**no improvement in either missing colors or flicker**. This comparison is
negative, not a fix; the next package disables it.

### Version 18 pass-boundary pixels

The explicit `HadesVulkanPassCaptureProbe` selects a presentation-count window
at 1800, closing new pass selection when that presentation call begins.
This is not a guarantee of a complete logical frame across guest threads.
At each selected render-pass end it records a Vulkan image-to-buffer copy
inside that same guest command stream, before subsequent passes can overwrite
the attachment. It restores the declared final layout, waits for the actual
submitted queue, and writes full-resolution RGB plus alpha from coherent
readback memory. Logs correlate command, framebuffer, actual begin-time render
pass, image, attachment, draw count and snapshot serial. Capture-only metadata
also correlates shader-byte FNV-64 fingerprints, pipeline stages, attachment
blend/write masks, pass begins, pipeline binds and indexed draw arguments.
Fingerprints are for local correlation, not cryptographic verification.
Pipeline creation metadata is bounded to 2048, selected binds to 1024 and
selected indexed draws to 2048; reaching these limits is explicitly logged.

This is limited to 96 single-layer, single-sample RGBA8/BGRA8 UNORM attachment
outputs, at most 16 MiB per readback and 384 MiB of outstanding pixel buffers.
Depth, other formats/samples and layered output are explicitly skipped.
Copies require a capture-only TRANSFER_SRC image-usage addition; copy barriers
and waits perturb the selected frame, so a disappearance during capture is not
automatically a production fix. Reset/re-record of a pending copied command is
refused rather than writing a stale snapshot. Registries and allocation/write
failures are explicit.

Real GPU tests cover O0/O2 and all three diagnostic switches on/off. A known
attachment is copied at pass end, then overwritten later in the same command
buffer: the pass snapshot retains the previous RGB/alpha while the frame-end
snapshot contains the new values. All 72 regressions passed. This proves
pass-boundary capture rather than merely another frame-end sample. A window
regression also rejects new selection after presentation entry even when
successful-present completion has not advanced. The real Mac house scene
produced 74 full-resolution RGB/alpha pairs from one command buffer without
the former 64-output truncation. Early black attachments are followed by
progressively colored scene outputs; black intermediate images alone are not
evidence of the iOS defect. The final correlated Mac run wrote 73 pairs and
covered all 16 bound pipelines without metadata or pixel limits.

The signed build 18 was USB-installed with all 10 save hashes preserved and
its exact receipt confirmed at launch. The foreground device scene wrote 72
pass RGB/alpha pairs without truncation or readback errors; its final image
still has black silhouettes. The tracked command stream contains 394 indexed
draws and 36 zero-indexed-draw passes, versus 545 indexed draws and one such
pass in the Mac observation. Different animation and draw ordering prevent
blind serial matching; these counts are observations, not proof of a CPU or
depth-buffer root cause.

### Version 19 call-flow comparison

The same explicit probe additionally traces original Vulkan calls at
presentation-count windows 1800 and 3600. Only the first window inserts
pass-boundary copies; the second does not insert those copies/barriers/waits.
At the default iOS capture step the four frame-end inventories have also
finished before the second window. This separates a later call-stream
observation from the readback-frame intervention, not from all timing effects.

Tracing is bounded to 16384 calls and records arguments, guest LR/FPCR,
render-area/scissor declarations and pass-end callee-saved CPU registers.
Local module mappings permit ASLR-independent caller analysis; no guest
stack, asset or buffer payload is dumped. Enable via
`HADES_VULKAN_PASS_CAPTURE_FRAME=1800` with the existing private capture
configuration, or the iOS `HadesVulkanPassCaptureProbe` flag. This is a
diagnostic, not a fix.

Signed v19 is USB-installed with all 10 save hashes unchanged and its receipt
confirmed. The device reached at least 6000 successful presents. Raw indexed
draw counts were 392 and 403 in the two windows, without a call cap. The later
window had 73 passes, 34 with no indexed draw, and no pass-boundary copies.
The Mac later window had 557 indexed draws, 76 passes and one empty pass.
Begin/bind/draw/end callers match after ASLR normalization; their FPCR is zero
on both platforms. This establishes a submission difference in these scenes,
not an exact frame match or its cause. No rendering fix is claimed.

### Version 20 scalar-state observation

The same opt-in probe samples nearby static scalar loads at pipeline binding.
It decodes adjacent AArch64 `ADRP`/unsigned-immediate integer `LDR` pairs in
at most 576 bytes of registered readable executable guest code. Only 1/2/4/8
byte targets inside registered readable module data are sampled, at most
eight per call with an explicit limit diagnostic. No hardcoded game address,
instruction patch, stack/asset payload dump or GPU operation is added.
The output is a snapshot of a nearby decoded load's target, **not proof that
the guest executed that load**. It is intended to distinguish empty batching
state from later draw rejection.
Enable with the same `HADES_VULKAN_PASS_CAPTURE_FRAME=1800` and existing
private capture configuration, or `HadesVulkanPassCaptureProbe` on iOS.

Signed v20 is installed and its receipt confirmed, with all 10 save hashes
preserved. The device reached at least 7200 successful presents; both windows
produced scalar observations without truncation. In its later window 31 of
75 passes had no indexed draw despite nonempty pending batches (including
16, 34 and 30). The Mac later window submitted 544 indexed draws. This
rules out empty batching state for those observed device passes, not a
specific rejection condition or all causes of the image defects.

Version 21 additionally samples four scalar Forge shader-record fields at
vertex-buffer binding. It requires the observed adjacent instruction
fingerprint and registered readable code/module-data bounds before reading;
it does not use a fixed game address or change the record. Descriptor index,
per-slice descriptor capacity, vertex stride and layout are observations, not
proof that a capacity or missing-texture rejection occurred. The signed v21
artifact passed the existing 72-test suite and frozen-image/resource checks.
Its actual Mac scene produced 82 and 74 field observations in the two windows,
with 554 indexed draws in the later window and no call cap. V21 was subsequently
installed with user authorization, its launch receipt confirmed and all 10 save
hashes preserved. The device produced 143 field observations across both windows
and at least 4200 successful presents. Its later window had 77 passes, 480 indexed
draws and 25 zero-draw passes. The same main-scene shaders used layout 2 on iOS
and layout 0 on Mac, both with a 56-byte stride. The actual guest capacity table
limits layout 2 to 200000 bytes; its final vertex count stopped at 3571, leaving
24 bytes, less than one additional vertex. Guest allocation/checking uses the
layout registration index, assigned in shader first-load order; host directory
enumeration previously forwarded `readdir` order. This identifies the observed
layout's capacity bottleneck, not directory order as the proven cause of every
image defect. Mac values must not be substituted for iOS limits or treated as
an iOS rendering fix.

### Version 22 read-only directory-order comparison

`HADES_FS_DIRECTORY_ORDER_PROBE=1` sorts read-only directory snapshots by bytewise
filename after filtering and before paginated reads. It leaves save-directory
order, guest instructions, layout indices and capacity tables unchanged.
Unset or `0` preserves the baseline; other values fail explicitly. The iOS
package enables the experiment through the strict boolean `HadesDirectoryOrderProbe`,
with an explicit startup diagnostic, not a verified-repair claim.
Directory regressions cover on/off, bytewise case ordering, pagination,
file/directory filtering, metadata and invalid configuration. All 72 existing
tests pass. The actual Mac scene reached at least 9600 successful presents with
555 later-window indexed draws and main-scene layout 0. The signed artifact's
15 AOT sections, 291 resources and 42 source files were verified. It was then
installed with user authorization, its startup receipt confirmed and all 10
save hashes preserved. The actual-device log confirms the sorting configuration:
main-scene shaders 0/32/51 now use layout 0 at the unchanged 56-byte stride.
The two windows contain 64/83 passes and 467/661 indexed draws, with zero
zero-draw passes in either window. The later main-scene vertex count reaches
4641, beyond the former 3571-vertex limit, using layout 0's actual 4 MB allocation.
The log records at least 3600 successful presents. The user reports success;
this is a device graphics milestone, not exhaustive pixel, symptom-by-symptom
or full-session verification. Scenes differ between runs, so aggregate counts
are not a frame-aligned benchmark. Original Horizon directory ordering remains
unverified. The log also records the existing foreground-only abort on backgrounding;
background suspension is still unimplemented. Private logs, the signed-artifact
verification and the user's feedback are retained locally.

### Version 23 analog left stick

A fixed-base analog joystick replaces the four left-side direction buttons;
the eight action/shoulder buttons remain. It supports continuous direction and
magnitude, circular clamping and a 12% radial dead zone. Upward dragging publishes
positive Y, without adding D-pad bits. It tracks the original finger outside the
base and resets on release, cancellation or disabling without releasing
independently held action buttons. Opening diagnostics or deactivating the app
still releases all controls under the existing policy. Layout respects safe areas
and moves ZL rightward to avoid overlapping the stick.

V22 directory ordering and graphics settings are unchanged. The new joystick
header is included in source receipts and frozen Xcode projects. Core/joystick
and iOS packaging regressions passed; the Apple-signed build's 15 AOT sections,
291 resources and 43 source files were verified. It was installed with user
authorization, its new startup receipt confirmed and all 10 save hashes preserved.
The user confirmed actual-device movement, stopping on release and simultaneous
action-button input. This does not establish layout or interaction on other devices.

### Version 24 top-right FPS

A non-interactive FPS label sits at the top right, with R moved slightly downward
to avoid overlap. It samples the existing thread-safe Vulkan presentation counter
against a monotonic clock over windows of at least one second, checked every half
second. The value uses actual elapsed time, not the panel refresh rate, and is not
clamped to 60. No submissions produce 0; the first window shows `FPS --`.
Diagnostics, app deactivation and counter resets restart sampling.
This measures successful/SUBOPTIMAL presentation submissions, not GPU completion
or verified per-frame scanout.

The user selected the verified 1280x720 baseline. Runtime code, graphics settings
and rendering resources remain unchanged; no GPU readback or queue waits are added.
1080p has 2.25 times the pixels of 720p and requires separate verification of guest
render targets, swapchain dimensions, device frame rate, memory and stability.
Increasing drawable size alone does not establish native 1080p or stable 60 FPS.
FPS/joystick and package/project regressions passed; the signed build's 15 AOT
sections, 291 resources and 44 sources were verified. It was installed with user
authorization, its new receipt confirmed and all 10 save hashes preserved.
User verification of the top-right display and control layout remains pending.

### Version 25 opt-in native 1080p experiment

Increasing only the host drawable to 1920x1080 left the game's swapchains,
framebuffers and scene passes at 1280x720, producing SUBOPTIMAL presents.
Actual main-module analysis identified the bootstrap's packed window dimensions:
`mov x9,#1280` at `0x248238`, `movk x9,#720,lsl #32` at `0x248240`,
subsequently stored into the initial and current window descriptors.
`display.py` checks the specific main build ID, both constants and both stores
before adjusting the constants to 1920/1080 in **private copies** of the verified
AOT images. This is an intentional graphics behavior change, not ABI equivalence;
the original extraction, verified ABI baseline, NSP and save data are unchanged.
No docked mode, Switch clocks, forced driver extents or CPU JIT are introduced.

Both `link_modules.py` and `package_ios.py` accept `--render-resolution 1080p`;
their default remains `720p`. The generated manifest identifies the embedded
preset, and the iOS `HadesRenderResolution` must match it. The same dimensions
are used at initial launch and subsequent layouts. A 720p rebuild or the retained
v24 signed package provides the rollback path. Package receipts track the actual
embedded image hashes, and signing verification uses those experimental images.

The Mac isolated-save comparison reached at least 9600 successful presentations
with zero logged SUBOPTIMAL presents. Actual swapchains, main scene framebuffers,
later scene passes and native pixel captures were all 1920x1080; the captured
house scene retained its colored materials. Small lookup targets remain unchanged.
The v25 iPhone build is Apple-signed, with 15 AOT sections, 291 resources and
46 frozen source files verified. It was installed with explicit authorization
after two stopped-process checks and a full save backup; all 10 save hashes
remained identical. The device launch receipt and 1080p request were confirmed.
The user subsequently requested a return to 720p. After another full backup and
two stopped-process checks, the retained Apple-signed v24 was installed without
uninstalling; all 10 current save hashes were preserved, and a fresh v24 receipt
was confirmed after the v25 launch. **That rollback restored 1280x720 v24 with
joystick and FPS.** No reason for rollback or measured device FPS was reported;
native iPhone 1080p scene correctness, performance and stability remain unverified.
1080p is not a promise of 60 FPS, nor an increase in the original asset detail.

### Version 27 opt-in widescreen (installed, user confirms playable)

`--render-resolution wide-720p` selects 1560x720 for the current iPhone 15 Pro Max,
about 22% more pixels than the unchanged default `720p` preset. Following D2R's
separation of world viewport and UI, this uses Hades-specific evidence, not D2R
addresses. The Metal view follows the render aspect and fills this phone's landscape
bounds; mismatched screen ratios retain aspect-fit behavior.

A width-only negative control still drew the game's decorative pillars. Exact
build-ID/original-byte checks now guard private AOT changes to the bootstrap width,
shared virtual width and default view width. The 2340x1080 virtual canvas reveals
additional world at the sides without stretching central geometry. Original NSP,
ExeFS, ABI baseline and RomFS archive remain unchanged; executable code is not
modified at runtime.

31 private read-only UI overlays center absolute native-GUI positions, preserve
attached coordinates and expand full-width backgrounds. NX same-name inheritance
overrides the base file rather than forming a local cycle. Each source is checked
for size and SHA-256 on open; file sizes, directory sizes and partial reads expose
the transformed content consistently. The Lua canvas coordinates enter
`SaveIgnores`, because `Main.Load` otherwise restores old saved globals over the
new display configuration; experimental coordinates are not persisted to saves.

An actual Mac run using a separate old-save copy reached at least 9600 successful
presents with zero SUBOPTIMAL. Captured 1560x720 world pixels show the former pillar
areas; the location banner, pause and settings menus are centered. The Apple-signed
candidate has 15 verified AOT sections, 291 resources and 46 frozen sources.
The user authorized USB installation. An initial live-process check stopped the
operation; after the user saved and closed the entire app, two new stopped-process
checks and a complete backup preceded the same-bundle update. All 10 current save
hashes matched afterward; only then was v27 launched and its fresh receipt confirmed.
There was no uninstall or forced termination; the v24 rollback package is retained.
The user subsequently confirmed that v27 is playable on the phone. This is a
user-reported gameplay result, not independent device pixel verification or a
stable 60 FPS measurement. Full combat and long-run stability remain unverified.
Fixed-aspect artwork and transitions are not claimed to be newly authored ultrawide assets.

### Version 28 FPS monitor and larger action buttons (installed, interaction pending)

Tapping FPS toggles a nonmodal monitor, also dismissible through its header.
It does not pause the guest, leave the game surface or release held controls.
While visible and foregrounded, it samples once per second: submitted-present FPS,
current-process CPU, physical footprint, resident memory, Metal allocations,
process-available memory from `os_proc_available_memory()`, and system thermal state.
CPU is the delta of cumulative `getrusage(RUSAGE_SELF)` user/system CPU time divided
by elapsed time; 100% means one core, and multicore usage may exceed 100%.
Initial samples show a waiting state; failures display unavailable and log errors.
These are not whole-device CPU or free RAM, thermal state is not a Celsius reading,
overlapping memory accounts must not be summed, and Metal allocation is not GPU utilization.

A/B/X/Y grow from 52 pt to 72 pt with larger labels, preserving the diamond mapping.
Their rectangular hit areas do not overlap and remain inside the safe area.
The other four buttons and joystick are unchanged; FPS has a 44 pt-high tap target.
The v27 widescreen preset is retained with identical guest-image and resource hashes.
The Apple-signed, source-frozen v28 was installed over USB with explicit permission,
two stopped-process checks and a full backup. All 10 current save hashes matched
after the same-bundle update, followed by launch and fresh-receipt confirmation;
no uninstall or forced termination. Monitor toggling/values and enlarged-button
interaction still need device feedback; successful installation does not establish those results.

### Version 29 compact circular controls and vignette candidate (installed, touch issue reported)

A/B/X/Y retain their 72 pt diameter and mapping. Diamond-axis spacing decreases
from 80 to 56 pt, lowering the cluster center by 30 pt with a 6 pt bottom safe-area
margin. Adjacent circles remain about 79.2 pt apart. Hit testing rejects the
transparent rectangular corners; overlapping bounding boxes do not steal adjacent
circle taps. This applies only to these four in-game controls, not the diagnostic
page, other buttons or joystick.

The widescreen candidate also adds horizontal `ScreenWidth / 1920` scaling to the
existing `CreateVignette` object, guarded by original-source hash and unique anchor.
Vertical scale, opacity, blending and original resources are unchanged; read-only
overlays increase from 31 to 32. Before/after Mac runs from independent copies of
the same phone backup each reached at least 7200 presents without SUBOPTIMAL, with
per-pass captures retained. However, that backup loads the House rather than the
reported combat scene: **the old-aspect dark rectangle is not yet reproduced or
proven fixed**. A newer pre-update backup loads the weapon courtyard, which also
does not reproduce the reported combat scene. Apple-signed v29 was installed with
fresh user authorization, two stopped-process checks, a full backup, identical
hashes for all 10 save files, and fresh launch-receipt confirmation. No uninstall
or forced termination. The user subsequently reported that only the letter areas
responded: passing geometry tests did not establish correct whole-circle physical
touch. Combat-mask results remain unconfirmed.

### Version 30 whole-circle touch delivery and active appearance (device confirmed)

The four explicitly allocated custom UIButtons own in-game hit testing and the
touch lifecycle rather than relying on system-button content tracking. Letters
and other circle positions share press/release dispatch. A press brightens the
whole circle, thickens its border and reverses the label color. Dragging out
releases, re-entering restores, and release/cancellation/disable/diagnostic
switching clear held state. The v29 diameter, position and spacing are retained;
other buttons, joystick, guest images, original resources and widescreen overlays
are unchanged. **No haptics were added.**

The old v29 implementation passed a 20-point Mac UIKit hit test; this does not
reproduce the phone's letter-only symptom or prove a geometry bug. New O0/O2
regressions use actual Mac Catalyst UIKit to check 36 circle positions, input
bits, rendered active pixels, corner rejection, exit/re-entry, cancellation,
disable, a second finger and simultaneous buttons. Touch phases use test doubles,
not physical touchscreen input. All 81 regressions and iPhoneOS compilation pass.
The first enabled landscape layout logs 36 interior/16 corner routing checks,
and at most the first 16 real circle presses log local coordinates and selected
state.

Version 30 was installed over USB with user authorization, two stopped-process
checks and a complete backup. All 10 save-file paths and hashes were unchanged,
and the fresh launch receipt matched the signed package. No uninstall or forced
termination was used. The user confirmed that all four circles trigger actions
away from their letters and that press highlighting works. This confirmation
does not establish the separate combat-mask fix or long-run stability.

### Version 31 native button haptics (installed, physical feedback pending)

After user approval, the host uses a shared, lazily created
`UIImpactFeedbackGenerator` with `UIImpactFeedbackStyleLight`. The eight gameplay
buttons request one impact on touch-down; dragging out/back, holding, releasing
or cancelling does not request another. The joystick has no haptic binding.
FPS/monitor toggles, the log button and diagnostic-page buttons request one
impact on confirmed activation. Programmatic log refreshes do not vibrate.
This covers host controls, not guest-rendered menus or guest rumble. Requests
are foreground-only, and iOS settings/hardware can suppress physical feedback.

O0/O2 UIKit tests count generator-spy calls alongside the existing whole-circle
input/active-pixel regressions; they do not measure physical vibration. The
Apple-signed package has 15 verified AOT sections, 291 resources and 50 frozen
sources. Guest images, resources and display overlays match v30. Following
authorization, USB update, two stopped-process checks and a complete backup,
all 10 save-file hashes remained identical and the fresh v31 receipt matched.
The user has not yet tested the physical haptic response.

Version 9 logs a `build` number and `receipt` derived from code/header, five
rewritten-image, binding and pinned-framework hashes, independent of output paths.
This identifies the device test artifact, not Apple signing, pixel correctness or gameplay.
Following D2R's self-task Mach observations, diagnostics now record resident,
resident peak, physical footprint and virtual size; the first and every 600th
Vulkan presentation also records actual Metal `currentAllocatedSize`.
Launch, test begin/return and UIKit memory warnings produce bounded snapshots.
Unavailable Metal measurements are explicitly `unobserved`. These are
overlapping accounts: **do not sum them** or equate the 2 GiB virtual heap with
physical usage/Jetsam allowance. O0/O2 tests touch 32 MiB of real private pages
and require at least 24 MiB of resident/footprint growth; this does not establish
iPhone 15 Pro Max memory compatibility.

Version 10 accepts explicit local CoreDevice launch arguments `--hades-probe core`
or `--hades-probe gpu`, executing once after the diagnostic view appears in the
active foreground. Ordinary launch does not auto-test. Duplicate, missing and
unsupported modes are rejected; SDK/game autostart is deliberately unavailable.
Unlock the device and foreground the app: successful process launch is not probe
execution. Require the current build's `EXPLICIT_PROBE`, `TEST_BEGIN`, final
`CORE_OK`/`RENDER_OK` and return status, not a locked device's idle host process.

Never run the D2R `setup.sh`, `product.sh`, host or iOS project with Hades files.
Those assume D2R builds, addresses, resources and UI. The shared rewriter has a new,
explicit `--profile portable-abi`: it disables **all** D2R cuts, observations,
parameters, private rules and bootlist handling, even for a matching D2R build ID.
The D2R profile remains the default; its game-specific tables/rules are unchanged. This is platform adaptation, not full-game
equivalence.

## Reproduce the local preparation

Build NSTool and SPIRV-Cross **outside git**, using the upstream sources and
revisions in `toolchain.json`. NSTool's upstream build requires its declared
submodules, `make deps`, then `make`. SPIRV-Cross uses CMake with the options in the
manifest, followed by `cmake --build <build-directory> --target spirv-cross`.
No system-wide installation is needed.

From this repository root:

```sh
python3 -m venv "$HOME/hades-tools/venv"
"$HOME/hades-tools/venv/bin/python3" -m pip install -r hades/requirements.txt
PY="$HOME/hades-tools/venv/bin/python3"
WORK="$HOME/hades-work"
NSP="/absolute/path/to/Hades.nsp"
KEYS="/absolute/path/to/your/prod.keys"
NSTOOL="/absolute/path/to/nstool"
CROSS="/absolute/path/to/spirv-cross"
LLVM="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin"

"$PY" hades/tools/prepare.py --work "$WORK" extract \
  --package "$NSP" --keys "$KEYS" --nstool "$NSTOOL"
"$PY" hades/tools/prepare.py --work "$WORK" rewrite --llvm-bin "$LLVM" --jobs 2
"$PY" hades/tools/prepare.py --work "$WORK" rewrite-dynamic --llvm-bin "$LLVM" --jobs 2
"$PY" hades/tools/prepare.py --work "$WORK" embed
"$PY" hades/tools/translate_shaders.py \
  --source "$WORK/data/RomFS/Content/EffectsForgeSwitch" \
  --out "$WORK/build/shaders-msl" --spirv-cross "$CROSS"
"$PY" hades/tools/probe_runtime.py --work "$WORK" \
  --out "$WORK/build/runtime-core" --spirv-cross "$CROSS"
"$PY" -m unittest discover -s hades/tests -v
```

Extraction requires a **new** workspace and never overwrites existing game files.
It reads `prod.keys` only from the supplied absolute path. NSTool may also read
`title.keys` from that same directory when present, or use the matching ticket
already in the NSP. No key values are passed on command lines or echoed/saved.
All CNMT content sizes/SHA-256, NACP title, NPDM program ID and NSO segment hashes
are checked. The original NSP is hashed before and after extraction. The supplied
ticket was reported fake-signed: successful decryption and content hashes do **not**
establish ticket authenticity or licensing.

Rewriting requires a fresh `build/abi` directory. `rewrite.json` is produced only
after every module passes independent LLVM structural checks and randomized
Unicorn equivalence tests, with at least two effective trials per non-gate site.
D2R's original-game regression was not run: no D2R game files were used.
D2R-only output tables must be empty. The iOS embedding step rejects changed
source images, rewritten bytes or manifests, then compares every object section's
bytes and 16K layout with the verified image. **The object is not signed.**

`rewrite-dynamic` checks the original EOS NRO's membership in `.nrr/game.nrr`,
program ID and RSA-PSS/SHA-256 payload signature, then losslessly wraps its segments
in the shared rewriter's NSO input format. Loaded bytes, BSS, symbols and relocations
are unchanged. Verification with the certification's embedded public key does
**not** authenticate Nintendo's root certification chain. The native loader accepts
only the exact locally verified package's frozen NRR/NRO hashes, not arbitrary
self-signed modules. Rewriter changes require reverifying all five modules.

Shader translation inspects every SPIR-V instruction boundary and entry stage,
generates per-stage iOS MSL 2.1 and binding reflection, and records tool/input/output
hashes. Add `--compile` with a **new output directory** to invoke Xcode's Metal
compiler and generate metallibs. Missing Metal Toolchain is an explicit error,
not a success-shaped empty library. Runtime resource binding/rendering is not
verified by either translation or compilation.

`inspect_target.py check` remains a stdlib-only, read-only environment/container
check. It accepts explicit `--nstool`/`--spirv-cross` paths for isolated tools;
exit 1 means blocked, 2 means invalid input. `nso --exefs <path>` inspects only
headers; `analyze_modules.py --exefs <path> --romfs <path>` additionally verifies
NSO hashes and inventories static imports/exports, relocations, SVCs and dynamic
NROs. Inventory exit 0 is **not** runtime compatibility.

## Verified evidence and remaining boundaries

The actual package identifies **Hades / SupergiantGames, display version 1.0.34820**.
ExeFS contains `rtld`, `main`, `sdk`, `subsdk0`, `subsdk1`. The last four were
rewritten with zero D2R game-specific patches. The tests cover 27,519 non-gate
sites, each with at least two effective trials; 400 negative-control mutations
were detected. All 93 SVC/timer gate sites pass structural checks; the native probe
additionally executes the SDK timer gate and implemented entropy query, not all gate semantics.
Exclusive operations are normalized by the existing equivalence harness, so this
does not prove real concurrent atomics, HLE, TSD layout on-device or full gameplay.

RomFS also contains `nro/EOSSDK-Switch-Shipping.nro`, which provides the EOS imports.
The NRO is now rewritten and statically embedded. The native loader remaps signed
pages into the original layout without CPU JIT, RWX code or runtime code generation.
Its strict relocation planner resolves RELATIVE/ABS64/GLOB_DAT/JUMP_SLOT, weak
imports and visibility; unknown types, unresolved strong imports, code-page writes
and out-of-bounds targets fail. **200,398 relocations** were applied in the native probe.

`hades/runtime/core.c` provides a **decoded `ldr:ro` command core**, not guest
HIPC/SVC transport: process/NRR registration, authorized embedded EOS mapping,
unload and unregister, with negative lifecycle/PID/hash/BSS tests.
Native `EOS_GetVersion` returns **1.7.1-13992660**. The actual SDK timer export
crosses the rewritten host gate and returns a monotonic **19.2 MHz** clock,
including 8 pthreads each making 100 reads. This exercises only that host TSD/timer
path, not guest threading, exclusive instruction semantics or full concurrency.

SDK bootstrap now **natively returns from `_init_libc0`, `nnosInitialize` and
`_init_libc1`**. The memory layer serves real address-space/alias/stack queries,
queries mapped module permissions and the current pthread's full stack, and
uses NPDM's 39-bit address bounds. Unknown gaps are inaccessible rather than
invented code pages. NPDM supplies main-thread priority 44 and logical core 0;
its kernel capability allows priorities 28–59 and logical cores 0–2, which
generate the query masks rather than inheriting D2R's masks.
This is not proof of guest scheduling or host CPU affinity.

The explicit host probe policy reserves 256 MiB alias, 128 MiB stack and a
2048 MiB heap capacity with `mmap(PROT_NONE)`, aligned to 2 MiB. The same native
game fails with NULL allocations at 512/1024 MiB but progresses to audio and
controller-support initialization at 2048 MiB. D2R likewise accounts its
application pool instead of hiding allocator failure; its title-specific
3285 MiB constant is not copied. This is **not a
measured Hades/Switch memory budget**. `SetHeapSize` opens actual pages and
replaces a removed tail with anonymous inaccessible pages so regrowth is
zero-filled. Tests verify writes, shrink protection, retained prefix data,
regrowth zeroing and rejection of invalid sizes/read-only outputs.
These heap paths are tested separately, not observed during this SDK bootstrap.
Heap size/capacity and mapped image size are accounting values, not physical RSS.
Native-granule `MapMemory`/`UnmapMemory` now create real shared aliases, protect
the borrowed source, restore writes on unmap and reject shrinking/unloading
borrowed pages. Permission/attribute changes and guest scheduling remain incomplete.

`ConnectToNamedPort("sm:")` and actual SDK `SendSyncRequest` now use owned,
non-reused handles and the current thread's 256-byte HIPC buffer. The implemented
subset validates CMIF v0/v1, kernel-bound PID and current-process copy handles,
session control, domain object identities/closure, and copy/move replies.
Unsupported commands and descriptors fail explicitly.
The real bootstrap registers with `sm:`, acquires `lm`, `apm`, `appletOE`, `aoc:u`,
converts two sessions to domains and constructs their requested objects.
These are **partial endpoints**, not complete system services: logger submission,
performance configuration setters and unobserved applet methods remain unsupported.
APM's normal/handheld mode is an explicit virtual policy, not measured hardware.
Applet resource IDs are bound to the authenticated kernel process.

Suspension-tick and GPU-error reader handles have real wait/reset state and
monotonic timeouts. Host notification APIs are tested with pthread waiters but
are not wired to UIKit lifecycle or the guest Metal backend. No GPU error or
suspension is fabricated to get through initialization. The headless bootstrap
explicitly starts as one foreground virtual application and publishes its initial
focus message. Foreground rights and focus-handling policies are stored. Policies
requiring guest-thread suspension reject host suspension transitions explicitly;
this is not iOS lifecycle support. AOC reports only the mounted base-package set
(zero DLC) and exposes a real unsignaled content-change event; D2R's DLC identity
is not inherited and dynamic package installation is unsupported.

The SDK's **4 KiB** borrow from `sdk+0x11f4000` now passes on the **16 KiB** host
using an explicitly enabled D2R-style **copy-move compatibility policy**.
Only owned stack reservation pages are rounded open, requested bytes are copied,
and source permissions are revoked logically, not physically. Unmap copies bytes
back and clears the destination request without revoking its neighbors. This
depends on the guest not accessing the borrowed source: **not a live alias and
not physical 4 KiB protection**. Exact host-page aliases retain their precise path;
exact and copied mappings cannot share a destination host page.
An isolated anonymous-page probe demonstrates that `vm_remap` can return success
without satisfying a requested 4 KiB-offset alias, and protecting a 4 KiB source
also denies adjacent guest pages in the same 16 KiB host page.

`threads.c` follows D2R's stopped-pthread/start/SDK-stack strategy and corrected
condition-key publication before mutex release. The actual SDK background thread
`0x101` starts with priority 28 and logical core 2, using that borrowed 4 KiB stack;
its GPU-error event wait is real. Mutex handoff and condition reacquisition use
priority/FIFO ordering. Single-thread-handle waits support monotonic finite,
zero and infinite timeouts. Borrowed stacks and guest regions cannot be unmapped
or unregistered before the retaining pthreads are joined. This conservative
subset does not allow two pthreads to share one borrowed stack mapping.
It does not implement priority inheritance, address arbitration, mixed
thread/event waits, complete scheduling or prove all guest exclusive semantics.

The Hades-only `HADES_HOST_GATE_STACK` branch keeps the 800-byte guest register
frame but runs host C on a separate 64 KiB per-thread stack. The D2R default branch
is unchanged. A synthetic 4 KiB guest-stack probe executes a 24 KiB C frame and
checks GP/SIMD/NZCV/FPSR/FPCR restoration and edited outputs at O0/O2. Eight
synthetic pthread runs exercise 64 mutex/condition handoffs each at O0/O2; these
are bounded host tests, not proof of complete game concurrency. The HIPC/TLR
buffer is explicitly 16-byte aligned to match the SDK's absolute CMIF alignment.

The original IPC-only profile still stops at required `pctl`, providing a baseline.
The opt-in `--host-services` profile follows D2R's force-by-symbol approach, not
its addresses: exported zero-addend function references target fixed signed
trampolines. Both SVC and symbol gates preserve registers on the separate host
stack. All six SDK exports and four resident DT_INIT functions now return; each
DT_INIT handles its own init array and is not followed by duplicate array calls.
Three real runs enter Hades `nnMain`, initialize actual host time, enable its
128 MiB guest-owned immutable-ROM read cache, open a durable application-local
offline user and create a real persistent save namespace. Clock/calendar and
owned readonly file descriptors have actual backends; APM requests are stored
virtual policy, not a claim to change iOS CPU/GPU frequencies.

Save mounting, directory enumeration, writes, flush and atomic commit now have
real local backends. `working`/`pending`/`committed` snapshots, an exclusive lease,
file/directory synchronization and an atomic swap preserve the previous commit
on failure. Interrupted-process recovery is tested, not power-loss durability.
The ROM mount query returns an actual 4 KiB owned metadata/read window: returning
zero had caused the real game to skip mounting `Content:/`. The experimental
Vulkan profile now reads actual resources, starts four more SDK pthreads and
commits the game's `activeProfile`. The local user is not a Nintendo account.
The original host-only profile stops at `nvdrv`. The Vulkan profile's
**6,160,384-byte CodeData transfer pool** now has an actual live alias with
physically inaccessible donor pages. Explicit `rewrite --module-bias main:0x3000`
regenerates the portable branches, key/trampoline layout and signed zero prefix,
aligning this donor to the 16 KiB host without altering original internal offsets.
All four NSOs and EOS are reverified in a fresh private workspace; do not reuse
old rewrite receipts or simply shift a previously rewritten image.
The decoded BSD backend retains the real pool, owns a local monitor pipe,
supports domain session clones and explicitly refuses Internet sockets.
SDK version 7 returns a signed registration status, unlike the version 1 PID
response. Native socket initialization and its monitoring subscription now pass;
this is not D2R's success-only socket-initialization cut.

Native HID initialization also passes: an actual 256 KiB publisher is mapped
physically read-only into a free reserved non-heap guest range. Revision 3
activation, supported styles/IDs and Joy-Con orientation are stored; a real
256-byte receive buffer copies validated HIPC pointer inputs. Five owned style
events publish the initial disconnected state and support wait/reset/close.
No physical controller is claimed; a configured frontend connects the virtual
FullKey publisher described below. Without a frontend it remains disconnected.
Duplicate shared-capability acquisition, unsupported domains/revisions and
closing still-mapped shared memory are explicitly rejected. O0/O2 tests cover
publisher-to-receiver updates, physical write faults, ownership and teardown.
Synthetic tests cover the gate, files/cache, save recovery and invalid inputs,
not full guest concurrency.
Calling DT_INIT before SDK/thread initialization is invalid; the earlier
diagnostic confirmed a missing SDK thread object in ThreadLocalRegion.

Although `subsdk1` exports EGL/OpenGL interfaces, the actual main module imports
**100 Vulkan functions**, including `vkCreateViSurfaceNN`. The SDK exports did
not establish an EGL/NVN game rendering route; the earlier inference was wrong.
Actual Switch resources contain **53 fragment and 19 vertex SPIR-V binaries**;
all 72 translated to MSL with reflection. Desktop resources are not selected.
The D2R Maxwell/CASC extraction rules do not apply. `reflect_bindings.py` rechecks
SPIR-V reflection and MSL hashes against the actual emitted buffer/texture/sampler
parameters: 206 of 210 declared resources are emitted, with four explicitly
reported as absent from the MSL interface. Descriptor set/binding are **not NVN slots**.

`dm/gfx/nvn/nvn_spirv_bind.c` consumes existing NVN UBO/texture/sampler command
payloads and encodes through an explicit caller-supplied program map. Missing,
undersized or colliding bindings fail. The offscreen probe uses actual
`Default2D.vert` / `Default.frag`, synthetic vertices/texture and an explicit
test-client map. macOS's built-in `newLibraryWithSource` compiles all 72 stages
without installing Xcode components. The selected pair's three GPU submissions/readbacks produce
**1024 / 440 / 1024** matching pixels, exercising uniform and texture changes.
This test-client NVN/MSL probe is not evidence of the actual Vulkan game route.
The optional Vulkan profile uses pinned official public-API MoltenVK v1.4.2
static ARM64 libraries; archive SHA-256 and extracted library/header/license
bytes are checked. It forwards typed calls through signed AOT symbol gates,
including Switch stack arguments beyond register eight, and maps the NN VI
surface to a real CAMetalLayer. Vulkan host stacks use 1 MiB rather than the
ordinary 64 KiB. O0/O2 tests verify ten-argument forwarding, actual Metal queue
buffer copies/readback and mapping ownership/revocation. Typed allocation and
debug callbacks now use guarded per-thread reentrant stacks, retaining SDK
thread/TSD identity. Foreign driver threads and instance pNext callbacks remain
explicitly unsupported. Synthetic typed PFNs verify allocator ABI; the tested
MoltenVK path does not invoke them. Real driver debug notification is separately
verified. The actual game creates 112 shader modules and submits indexed draws
and presents. Success and SUBOPTIMAL presents are counted atomically without
rewriting driver results. Explicit frontend drawable geometry is preserved.
These submissions are not evidence of correct visible pixels or iOS gameplay.
An optional attached AppKit window belongs to the existing native probe
(`--window`); it is not a substitute for device verification.

The native `audout:u` route now opens a real 48 kHz PCM16 AudioQueue, snapshots
validated guest PCM, and returns completed opaque tags through native IPC.
Empty released-buffer outputs are zeroed (otherwise actual FMOD recycles a
stale tag). Completion generations drive release events with bounded 10 ms
host polling; audio callbacks never enter the SDK or acquire the IPC lock.
Stop/suspend pauses actual output, and close drains native callbacks before
freeing ownership. O0/O2 tests exercise real silent PCM completion, tag reuse,
events, volume, lifecycle, invalid descriptors and endpoint close. This is not
proof of audible game sound or device audio. Buffer flush and unobserved audio
commands remain unsupported rather than succeeding without implementation.
The configured AppKit keyboard and UIKit touch-button frontends now publish
public Horizon FullKey/SystemExt state rings in the real read-only shared alias.
Only supported ID 0/style FullKey is connected; no frontend remains disconnected.
O0/O2 tests check sampling identities, both rings, 40 updates across wrap,
attributes, analog limits, release and guest read-only mapping. This is not
proof of concurrent native SDK reads. Controlled native menu consumption is
verified separately below. Mac keys:
arrows (left stick only, not simultaneous d-pad), Return (A), Escape (B), X (X), Z (Y), Q/E (L/R), Space (ZL),
P (+); UIKit has corresponding buttons. Like D2R, the verified exported
vibration-handle query reports zero actuators for these virtual providers,
rather than allowing the SDK to infer physical rumble devices from FullKey.
Invalid extents/IDs/styles are rejected; vibration initialization is not replaced
by a successful empty operation. HID command 203 creates a device-list object,
not a permission Boolean; it remains unsupported. Permission command 205
correctly reports no virtual rumble capability.
With input configured the native SDK skips controller-selection applet launch;
a bounded 45-second run submits 4252 presents and continues PCM recycling.
Explicit own-swapchain readback subsequently verifies real 1280x720 menu pixels.
Only capture mode adds TRANSFER_SRC and waits for the render queue; normal play
does neither. The private images remain local: Apple Vision outputs only
preselected whole-word marker Booleans, not images or recognized text.
Controlled keys verify main menu → options (CONTROLS/CREDITS) → main menu.
A separate new-save route runs 125 seconds/14546 presents with zero SUBOPTIMAL
after matching native layer dimensions to the guest's fixed 1280x720 swapchain.
Later non-black scenes do not prove movement, combat, pixel correctness or iOS gameplay.
Headless mode still explicitly stops at library-applet launchable-event acquisition.
Library applets, verified gameplay, RO transport and lifecycle remain
incomplete. NIFM exposes an explicit stateful local-only policy, following D2R's
offline scope, without claiming Internet access or emulating its transfer pool.
Native socket initialization now passes with the live protected transfer pool.

The generated iPhoneOS ARM64 Mach-O object preserves all 15 embedded sections'
bytes and layout. A separate iPhoneOS app now also passes linked-section byte,
alignment and permission checks and local ad-hoc signature verification.
No Apple development/distribution signing, device installation or launch was performed.
Xcode/iPhoneOS SDK 27.0 is available, but the local Metal Toolchain and a valid
codesigning identity are missing. Do not install Xcode components or change the
global toolchain without approval.

Receipts (`extraction.json`, `dependencies.json`, `build/rewrite.json`,
`build/rewrite-dynamic.json`, `build/embed.json`, `build/shaders-msl/shaders.json`,
`build/runtime-core/runtime.json`, `build/runtime-ipc-verified/runtime.json`,
`build/runtime-copy-threads-verified/runtime.json`,
or `runtime.json` inside the selected fresh probe output)
and detailed validation logs
stay in the private external workspace. They may contain proprietary interface
metadata and local paths: do not publish them. Keep all game files, keys, shader
outputs, objects and packages out of git and uploads.

Next software work does not require Metal installation: verify gameplay beyond
the menu, controller-support/library-applet handling, required services and `ldr:ro`
transport, then trace actual Vulkan game startup.
Device signing and iPhoneOS metallib
compilation block only their respective stages, not this compatibility work.

## iOS device-validation IPA

`package_ios.py` uses the existing Xcode/iPhoneOS SDK to build an independent UIKit
app, not the D2R iOS project, addresses, DLC, HUD, cloud saves or download system.
It includes the real five AOT images, original startup NRR/NRO, all 72 generated
MSL stages and the verified binding catalog, with the complete extracted RomFS
in a separate private local archive. It is **not a playable Hades package**.

```sh
"$PY" hades/tools/package_ios.py --work "$WORK" \
  --bindings "$WORK/build/runtime-vulkan-era-verified/bindings/bindings.json" \
  --out "$WORK/build/ios26-vulkan-package" \
  --reuse-romfs-archive "$WORK/build/ios-side-load-package/Hades-RomFS-local-only.zip" \
  --moltenvk "$MOLTENVK_ROOT"
```

The output must be a new private directory outside git. Packaging verifies all
15 embedded linked sections, 16 KiB layout, a single executable `__TEXT`, no RWX
segments, iPhoneOS/ARM64/PIE metadata, copied resource hashes and every decompressed
IPA entry. `package.json` records source/package hashes and explicitly denies
Apple device signing, device runtime verification and on-device game entry.
The new package targets iOS 26, generic ARM64 and iPhone 15 Pro Max/A17 Pro
(not D2R's Apple A18 compiler policy), with the explicit host-service profile.
The optional reuse argument rechecks every archived file against current RomFS
and its inventory before referencing the existing ZIP; it does not recompress or
make another multi-GB copy.
Shaders compile through the device OS's Metal `newLibraryWithSource`; the missing
Metal CLI is not required and no global Xcode components are installed.

Bundling everything produces an approximately 4.31 GB app, above Apple's
documented 4 GB uncompressed build limit. The final
`HadesPortTest-resign-required.ipa` (exact sizes in `package.json`; Vulkan adds its static runtime)
therefore contains only startup/validation
resources, with `Hades-RomFS-local-only.zip` (approximately 3.9 GB) separately.
No online asset hosting is used. Packaging enforces a conservative 4,000,000,000
byte uncompressed app limit. Allow at least 10 GB for full asset extraction,
transfer, installation and caches. Re-sign locally with your own Apple identity and sideload;
the ad-hoc signature cannot install directly on a non-jailbroken device.
Do not send the IPA, game assets or keys to online signing services.
The requested increased-memory-limit and extended-virtual-addressing entitlements
must actually be authorized by the provisioning profile; do not force unauthorized
permissions. Some sideload tools/free accounts may remove them, so mapping/runtime
compatibility remains a physical-device question.

The core/GPU/SDK diagnostics do not require importing the full asset archive.
The fourth, real game-entry diagnostic requires the complete `RomFS` folder
transferred locally via USB to `Documents/RomFS`; it verifies every resource's
name, size and SHA-256 before launching, with no fallback to incomplete assets.
`Documents/HadesState` holds the durable offline local user and save namespace.
Run the core/EOS/timer test, then the 72-stage MSL/offscreen readback test, then
the real SDK bootstrap. Expected diagnostic markers are `CORE_OK`,
`MSL_COMPILE_OK`, three `FRAME_OK` lines and `RENDER_OK`; these are not game frames.
SDK/game initialization runs on a dedicated 64 MiB pthread, following D2R,
not a reusable GCD worker. After returning it retains the thread/TSD/guest state
until process exit and cannot run again in that process. The game-entry test
without input configuration stops at library-applet launchable-event acquisition;
the configured keyboard route has independently verified menu input and GPU pixels.
Bundle version 8 declares a fullscreen single-app diagnostic and requests landscape, preserving a fixed 1280x720
16:9 Metal surface, and overlays 12 touch controls plus a diagnostic button.
This is not verified device presentation or device input. The diagnostic button
releases held virtual controls and opens the scrollable log UI without pausing
the guest; return restores the surface. Inactive/active foreground transitions
publish real CommonState focus 2/1 and notification 15, release held controls,
and reject policies requiring unsupported guest-thread suspension. Repeating
the same focus state does not fabricate another notification.
Failed resource verification/launch restores diagnostics.
The log view reads at most 128 KiB from the tail; export preserves the full log.
Default traces retain first host calls and service commands, four initial waits
and periodic present counts; errors remain explicit. Set `HADES_TRACE=1` before
a local process launch to restore full dispatch/IPC traces (`0` is default).
Reopen the app to view/export
`Documents/hades-device.log`, or retrieve
it through local USB file sharing. No automatic uploads or keys/Apple-account logs.
Actual Hades requests focus policy 0/0/1, requiring background thread suspension.
Following D2R, complete suspend/resume needs blocked new GPU submissions, drained
in-flight work and a frozen guest clock; focus/audio changes alone cannot replace it.
These mechanisms are not implemented. Version 8 therefore logs
`FOREGROUND_ONLY_ABORT` and exits the entire diagnostic with status 75 when active
GPU/SDK/game testing enters background; idle/core-only states are excluded.
This is not suspension/resume or forced saving, and uncommitted progress may be lost.
Keep the diagnostic in the foreground.
Device evidence must establish signed-page remap, Apple TSD, the guest/host stacks
and GPU compatibility before claiming a device runtime milestone.

### Explicit local Apple re-signing

`sign_ios.py` uses an explicitly selected valid local certificate SHA-1, your own
`.mobileprovision`, and the target's provisioning UDID (not its CoreDevice discovery
UUID). It checks the certificate, team, application ID, expiration, device list
and memory grants before copying verified IPA members into a new private output.
The original package is not modified. Actual signed entitlements and all 15
embedded sections are checked afterwards. Missing credentials stop before copying
game files; the tool does not scan personal directories, register accounts or upload.

```sh
python3 hades/tools/sign_ios.py \
  --work "$HADES_ROOT" --package "$HADES_ROOT/build/ios-side-load-package" \
  --out "$HADES_ROOT/build/local-apple-signed" \
  --profile "/explicit/local/profile.mobileprovision" \
  --identity "<40-character valid local signing certificate SHA-1>" \
  --device "<target provisioning UDID>" --bundle-id "<profile-authorized bundle ID>"
```

D2R's `dm/ios/run.sh` explicitly removes unsupported memory capabilities for a
personal team. This tool rejects unauthorized capabilities by default; only
`--diagnostic-without-memory-entitlements` opts into a reduced-authorization
**diagnostic**, recording every omitted memory grant in private `signing.json`.
D2R's normal-address-space gameplay does not establish Hades's 2 GiB heap,
resident memory or device address-space compatibility. Six synthetic tests cover
authorization, stale/wrong credentials/devices, archive bytes and signed-entitlement
mismatch. Without a valid Apple identity, real Apple signing and installation
cannot be claimed. Neither local codesign verification nor an archive proves
physical-device gameplay.

### Native Xcode team-selection handoff

`xcode_project.py` generates a private native project for the same verified Hades
target, compiling the existing runtime, generated manifest and embedded images,
linking pinned MoltenVK and copying verified startup resources. It is not a
prebuilt-game shell, and inherits no D2R account, game address or download extension.
Source/resource receipts are checked before generation; no dependencies, accounts
or devices are downloaded/registered.

```sh
python3 hades/tools/xcode_project.py \
  --package "$HADES_ROOT/build/ios-side-load-package" \
  --out "$HADES_ROOT/build/xcode-handoff" \
  --moltenvk "/explicit/local/official-MoltenVK-workspace"
open "$HADES_ROOT/build/xcode-handoff/HadesPortTest.xcodeproj"
```

Select your own Team under HadesPortTest → Signing & Capabilities and a unique
bundle ID available to that team. Connect/unlock/trust the physical iPhone over
USB, enable Developer Mode, select that device (not the simulator), then Run.
Personal teams must explicitly request `--diagnostic-without-memory-entitlements`
when generating the project; omitting those two requested capabilities does not
prove Hades's 2 GiB heap will work on the device. Full mode requires actual profile
authorization, not silent entitlement removal. Version 8 freezes code, headers
and generated layout/dispatch sources into read-only snapshots, not mutable worktree
references. The project team/bundle settings remain configurable. Embedded images,
startup resources and pinned frameworks still reference private local paths:
preserve their directories and do not share.

The native ARM64/iOS 26 target has been built with actual `xcodebuild` without
signing, then checked for all 15 embedded sections/permissions and exact bytes of
296 startup/shader/icon resources. A direct-copy icon phase avoids Xcode PNG
processing changing verified bytes; differences are not ignored. This is not
Apple development signing, installation, A17 Pro execution or gameplay evidence.
