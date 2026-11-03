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

The first live hardware run applied 1920x1080 host output and produced actual scaled menu/hub targets, including 2048x1784 and 2560x1792. It matched the baseline guest counters through the warp gate, then stalled during level loading. Investigation found that the existing loading-vortex workaround was restricted to the literal `PGS` environment value. The enhancement normalizes the configured backend and permits both backends that implement the complete-frame hold tag. A successful representative-level retest is required before calling hardware graphics qualified.

Normal hardware shutdown and parse-only shutdown were tested after correcting the DLL lifetime ordering. A forced watchdog exit is distinct from orderly shutdown.

## Remaining release gates

Representative hardware gameplay, resolution/AA switching, and visual comparison with PCSX2 remain required. Optional static VU acceleration needs matching OFF/ON hash traces and guest counters before it can be recommended as a default. No full-campaign, sustained 30 fps, or true 60 Hz simulation claim is made by the results above.

## Subsequent accelerated checks

The accelerated full fresh-card route passed twice with all six upstream counters exactly matching. The equipped-weapon route passed: Optimus's weapon energy fell from 1 to 0 with the cheat off, stayed at 1 while enabled, and fell to 0 after disabling it. One player health subtraction was blocked. Robot/vehicle transformations completed and the movement factor returned from 1.5 to 1. The static VU coverage check recorded 5,505,024 accepted calls and zero interpreter calls.

The accelerated hardware route passed through the Amazon level, switched internal resolution between 1x, 2x and 4x, toggled FXAA, and restored 4x with FXAA off. The bridge now selects PCSX2's pinned SLUS-20668 graphics fixes (half-pixel 5, texture-inside-target 1, native scaling 2, adaptive BFF 9). Some field/combing artifacts remain visible in captured gameplay, so visual qualification is incomplete.

A second Autobot exploration route passed with a 100-point health resource, energy hooks active and movement restored after disabling the multiplier. No enemy-hit event was observed; outgoing damage remains validated by the native fixture only, not live combat.

Per the user's updated instructions, tests use visible windows and accelerated builds only. The unaccelerated hash comparison was stopped; no complete OFF/ON hash-gate claim is made. Earlier unaccelerated runs are historical evidence only. Full campaign reliability, all destinations/equipment, remaining character coverage and true 60 Hz simulation remain unqualified.
