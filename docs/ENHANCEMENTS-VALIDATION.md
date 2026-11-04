# Local enhancement qualification

Test machine: Windows x64, Ryzen 9 6900HX, RTX 3080 Laptop GPU, 32 GB RAM. Disc: verified original USA SLUS-20668. Work started from `00fffeab98013e599bb8baf93d89e4b5d71ba91d`. These results describe local tests on 2026-11-04, not whole-campaign compatibility.

All disc-derived files, generated C++, builds, saves, screenshots and complete logs are retained in a private directory outside this checkout. The original ISO is unchanged. No game files have been uploaded.

## Baseline

The unmodified port built and passed the fresh-card `level_mvp_cine.tfs` route. Terminal counters exactly matched the upstream reference: `vs=9604 pads=4121 cyc=47206531601 cd=3443 i5=4526 mscal=981847`. Its embedded short-route checkpoint also matched all six counters. Main menu, hub, mission globe, introductory sequence and Optimus in the Amazon level were captured and inspected.

Unaccelerated gameplay measured about **6.7 game frames/s** over the final 300 pad reads. Compilation overlapped part of this run, so this is diagnostic, not a controlled performance benchmark. The host presentation rate is not the game's simulation rate.

## Trainer and persistence

- The production trainer passed the native host fixture: unsupported executable rejection; player/NPC ownership; health and energy toggles; movement restoration on character switches; nested and resumed damage calls; profile isolation; hub-only unlock requests.
- The live hub detector accepted the hub's main ring. The normal profile correctly disabled unlock actions.
- All eight overlay pages were captured. Controller page navigation and pause/resume were exercised; the Trainer page showed cheats off and multipliers at 1 by default.
- A fresh normal save was created through the game's Autobot confirmation screen. The Trainer profile copied that saved card into a separate directory.
- Unlocking Mini-Cons and levels, triggering the game's autosave, and restarting restored the owned mask `0x0000f7ff7dbf4dff` and all eight level availability flags. Mission completion flags remained zero.
- The normal card's files remained byte-identical while the Trainer card changed. Aftershock and Corona appeared in the unlocked equipment picker.

Gameplay hook coverage, all three Autobots, every equipment item, deaths, and every unlocked destination require additional live qualification. A script reaching its final `PASS` instruction alone does not prove that a cheat affected the intended gameplay event; probe counters and captured game behavior must support that conclusion.

## Graphics

The hardware bridge passed real-GPU tests for native/4x rasterization, additional subpixel detail, FXAA switching, rendered VRAM readback, a full 4 MiB VRAM round trip, and 32/24/16/8/4-bit transfer layouts including odd packed-nibble boundaries.

The first live hardware run applied 1920x1080 host output and produced actual scaled menu/hub targets, including 2048x1784 and 2560x1792. It matched the baseline guest counters through the warp gate, then stalled during level loading. Investigation found that the existing loading-vortex workaround was restricted to the literal `PGS` environment value. The enhancement normalizes the configured backend and permits both backends that implement the complete-frame hold tag. The subsequent accelerated representative-level retest passed, as recorded below; image-quality qualification remains incomplete.

Normal hardware shutdown and parse-only shutdown were tested after correcting the DLL lifetime ordering. A forced watchdog exit is distinct from orderly shutdown.

## Remaining release gates

Representative hardware gameplay and resolution/AA switching have passed. Visual comparison with PCSX2 remains required. Optional static VU acceleration needs matching OFF/ON hash traces and guest counters before it can be recommended as a default. No full-campaign, sustained 30 fps, or true 60 Hz simulation claim is made by the results above.

## Subsequent accelerated checks

The accelerated full fresh-card route passed twice with all six upstream counters exactly matching. The equipped-weapon route passed: Optimus's weapon energy fell from 1 to 0 with the cheat off, stayed at 1 while enabled, and fell to 0 after disabling it. One player health subtraction was blocked. Robot/vehicle transformations completed and the movement factor returned from 1.5 to 1. The static VU coverage check recorded 5,505,024 accepted calls and zero interpreter calls.

The accelerated hardware route passed through the Amazon level, switched internal resolution between 1x, 2x and 4x, toggled FXAA, and restored 4x with FXAA off. The bridge now selects PCSX2's pinned SLUS-20668 graphics fixes (half-pixel 5, texture-inside-target 1, native scaling 2, adaptive BFF 9). Some field/combing artifacts remain visible in captured gameplay, so visual qualification is incomplete.

A second Autobot exploration route passed with a 100-point health resource, energy hooks active and movement restored after disabling the multiplier. No enemy-hit event was observed; outgoing damage remains validated by the native fixture only, not live combat.

Per the user's updated instructions, tests use visible windows and accelerated builds only. The unaccelerated hash comparison was stopped; no complete OFF/ON hash-gate claim is made. Earlier unaccelerated runs are historical evidence only. Full campaign reliability, all destinations/equipment, remaining character coverage and true 60 Hz simulation remain unqualified.

## Player confirmation

On 2026-11-04, the player confirmed taking no damage in live enemy encounters with infinite health enabled, and confirmed that the energy cheat works. This is player-observed evidence in addition to the automated checks. It establishes incoming-damage protection; it does not measure the outgoing damage multiplier.

## Reloaded mission selection

A visible accelerated hardware-renderer run reloaded the Trainer card and successfully selected all eight destinations: Amazon, Antarctica, Deep Amazon, Mid-Atlantic, Alaska, Starship, Pacific Island and Cybertron. Captures were inspected individually. Availability remained 255, completion remained 0, and the normal seed card was unchanged. This verifies destination selection after reload; it does not yet verify entering or completing every destination.

## Third-character energy behavior

The third Autobot route completed with a 142-point health resource, robot and vehicle states observed, and movement restored from 1.5 to 1. No player-damage or outgoing enemy-hit event was recorded in that route. While firing in robot mode, sampled energy dropped between refills; vehicle samples showed full energy. The player confirmed that weapons reload instantly and requested preserving this behavior. The energy implementation is unchanged; this evidence supports instant replenishment, not a claim that the displayed counter never decreases.

## Combined hardware and trainer check

The visible accelerated `trainer-hardware-1080-01` route passed (11,210 vsyncs, 5,004 pad reads, 3,479 CD operations, 218.6 seconds reported by the script). Hardware Vulkan rendered the captured Amazon gameplay image at 2048x1788 with 4x internal scale; the host output surface was configured and allocated at 1920x1080. The harness's ordinary 640x447 screenshot is a reduced comparison image; its `.internal.png` capture retains the scaled image.

Aftershock energy fell from 1 to 0 with the cheat off, replenished to 1 while firing with it on, and fell to 0 again after disabling it. One health subtraction was blocked. Robot-to-vehicle-to-robot transitions completed; movement scale returned from 1.5 to 1. No outgoing damage event was observed. Field/combing artifacts remain visible, so higher resolution does not establish complete visual correctness.

The player's existing local settings now select hardware Vulkan, 4x internal scale, and 1920x1080 output. Existing gameplay and input preferences were preserved. The energy implementation was not changed.


## Widescreen and timing investigation

The accelerated `wide-hardware-1080-01` hardware run passed at 16:9 Hor+ and 4x internal resolution (11,210 vsyncs, 5,004 pad reads, 3,479 CD operations). The gameplay capture was inspected against the matching 4:3 capture: the camera exposes additional horizontal scene content with HUD correction. Local launch preferences now explicitly select 16:9.

`objective-capture-01` passed on diagnostic build 07. Private checkpoint snapshots confirmed the original governor byte at `0x6cd984` is 1, the frame-wait byte at `0x6cf010` is 1, and timing slot 7 at `0x6f851c` is approximately 0.0333667 seconds. Player context position at +0xf0 changed consistently with the scripted walk. Amazon trigger 13's remaining activations at +0x78 changed from 1 to 0; trigger 294 links to it through +0x80. Their event identifiers and positions matched the user's disc database, extracted privately with archive CRC verification.

The first timing experiment ended before gameplay and is not a passing test. `timing-native60-02` completed with `PS2X_FIELD60=0`, `PS2X_FIELD60_PIN30=0`, and `PS2X_TF_NOFRAMEWAIT=5`. Over 480 guest vsyncs the game made 480 pad reads; timing slot 7 was 0.0166833 seconds. The governor remained 1, however, and wall time for that interval was approximately 19 seconds (about 25 updates/second). Perfscope showed GS saturation and significant EE queue waiting. This does not establish true 60 fps rendering, real-time performance, or equivalent physics, and those experimental flags were not put in normal launch settings.


The build-09 `objective-marker-02` route passed (10,870 vsyncs, 4,833 pad reads, 3,460 CD operations). Host captures and logs verified Training area -> Combat training as the first live trigger was consumed, and hidden after `game.objective_guidance=off`. The three host captures were inspected individually. The scene retained a 2048x1788 internal image, but the host capture was only 796x448 because the harness does not apply live display settings without its explicit resolution-test switch. Code inspection separately identified a startup persistence issue: saved windowed resolutions required confirmation on every normal launch. The follow-up display change retains a validated saved windowed resolution automatically; interactive changes and fullscreen/borderless changes retain confirmation and rollback.


The final build-11 `objective-marker-04` route passed with the same 10,870 vsyncs, 4,833 pad reads and 3,460 CD operations (218.5 script seconds). The explicit host sequence applied and validated 1920x1080 at startup without reverting. At gameplay capture time the actual window was 2560x1369; all three host images were inspected at that size and the internal scene was 2048x1788. They show Training area, Combat training, then no marker after disabling the setting. This establishes above-1080p rendering and the first Amazon guidance transition, not campaign-wide objectives or sustained real-time performance. Gameplay intervals still showed GS saturation and roughly 16-20 pad updates per wall second. Build 11 was promoted to the private launcher; user trainer/input preferences were retained, with 16:9 Hor+, 1920x1080 requested output, 4x internal rendering and the Amazon preview enabled locally.

## GPU-resident presentation

Build 21 uses shared D3D12 textures and GPU fences to feed the existing OpenGL presenter without a full-frame CPU round trip. It is now selected by the private launcher. The six alternating native-timing comparison runs matched all six route counters and player probes; measured gameplay updates ranged from 36.7-41.1/s with direct presentation versus 24.6-25.3/s with CPU readback. These desktop measurements include background application activity and do not establish true 60 fps. The feature route passed internal 4x/8x/4x scaling, FXAA toggles, menu pause/resume, objective guidance and shutdown. See [methodology, dimensions and limits](findings/direct-presentation.md).

## VU optimization and native mesh investigation

The optional AVX2 static VU build matched 59 cumulative state hashes and every route counter/player probe against SSE. Three alternating threaded pairs increased mean gameplay update rate from 39.18 to 41.26/s (5.3%), with individual paired gains of 2.7-6.8%. This remains below the true-60 target. See [VU performance](findings/vu-performance.md) for flags and measurement limits.

Build 24 also passed the original-speed direct-presentation route with all six original reference counters, all eight player probes and the 0.0333667025-second timestep. It is now selected by the private launcher, with direct presentation active, research probes off and original gameplay timing. Existing save/settings paths and trainer preferences were retained.

An opt-in, CRC-gated [native render seam probe](findings/native-render-seam.md) identifies the grass constant writer, observes completed uploads and records VU launch fingerprints without replacing geometry. Its corrected deterministic run matched all 59 hashes and the route probes; the threaded run also matched counters and player probes. GPU mesh execution is not implemented by this diagnostic change.
