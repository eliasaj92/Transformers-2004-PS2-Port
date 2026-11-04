# Experimental Transformers enhancements

The local enhancement branch starts at Transformers port revision `00fffeab98013e599bb8baf93d89e4b5d71ba91d`. It supports the original USA executable, **SLUS-20668**, SHA-256 `ab88780324b9492eca59016923d200dd1d95785fd725dd85f233c071805b4acb`. No game assets, extracted executable, generated game C++, saves, or build products belong in this checkout.

## Build and launch

From PowerShell, with Python, CMake, Ninja, Git, 7-Zip and MSVC x64 Build Tools available:

```powershell
.\launch\setup-enhanced.ps1 -Iso 'D:\Discs\Transformers (USA).iso' -Work 'D:\Private\Transformers' -Jobs 4
.\launch\play.ps1 -GameFolder 'D:\Private\Transformers\port\generation-<id>\game'
```

The setup command pins PCSX2 and verifies the downloaded Windows dependency bundle against `deps/hwgs/dependencies.json`. Alternatively, pass existing `-Pcsx2Source` and `-Pcsx2Dependencies` directories. This build uses AVX2, supported by the target Ryzen 6900HX. Vulkan headers and shader compilation libraries come from the dependencies; a system Vulkan SDK is not required. A Vulkan-capable graphics driver is required.

Open **F10** (or hold **Select + Start**) for the PC settings overlay. The launcher starts on parallel-gs for comparison. Under Presentation, choose **Hardware graphics (experimental)**, save settings, and restart. A one-run override is `-Backend hwgs`, which locks the renderer selection for that run.

- **Hardware internal scale:** scales the actual GS raster, independently of the window. At 4x, a 640x448 source renders at 2560x1792 before presentation; active scene dimensions can differ. This is distinct from enlarging the final image.
- **Output resolution:** select 1920x1080 in Display. Choose the aspect ratio separately; original 4:3 content retains bars unless a widescreen option is selected.
- **Hardware FXAA:** optional post-process antialiasing. Rendering above the output resolution also provides supersampling when downsampled. Bilinear presentation is suitable for this mode; FSR/CAS remain optional.
- **VSync:** controls host presentation. The simulation retains its original speed. Field output and the host FPS counter do not establish 60 Hz gameplay.

The hardware backend consumes ordered GIF packets and privileged-register snapshots through an isolated `tf_hwgs.dll`. Local-to-host transfers flush queued rendering and synchronize the relevant GS VRAM before CPU readback. The existing host window and overlay remain in use.

On Windows, `launch/play.ps1 -GameFolder <private-game-folder> -Presentation direct` selects hardware D3D12 rendering and GPU-resident presentation. Shared textures and GPU fences replace the full-frame CPU download/upload. It requires OpenGL external-memory/fence support and the same GPU for rendering and presentation. Unsupported configurations fail with a diagnostic; `-Presentation readback` explicitly selects the original Vulkan readback path. Internal scale, output resolution, FXAA, aspect ratio, the overlay and objective guidance remain available. Direct presentation does not enable the experimental 60 Hz simulation. See [direct-presentation details and qualification](findings/direct-presentation.md).

## Trainer and saves

All cheats default off. The Trainer page exposes infinite health, unlimited weapon energy, outgoing damage presets 1/2/4/8 and movement presets 1/1.25/1.5/2. The executable CRC must match before hooks become available. Guest changes occur on the game thread.

Health protection targets the player's health resource. Energy changes require an equipped player ability. Damage requires both a player source and another recipient; the wrapper handles nested and suspended damage calls and restores caller-owned event storage. Movement adjusts the player's movement factor without changing the global game clock; switching to 1 restores the prior factor.

For unlocks, select the **Trainer** save profile, save the settings, and restart. On first use, it copies the last saved normal card into `saves/profiles/trainer/mc0`. Subsequent launches reuse that copy. Normal saves remain at `saves/mc0`; both cards receive launcher backups. Settings persist outside generation folders. Unsaved in-game progress is not part of the initial copy.

**Unlock all Mini-Cons** and **Unlock all levels** are enabled only on the hub's main ring in the Trainer profile. Resume after requesting an unlock, reopen the equipment/mission screen, and save through the game. Unlocks change availability and the first gate of each campaign level. Mission completion flags and pickup-history counters are preserved. Changing profile takes effect on restart.

## Qualification

Use fresh private output directories. Existing routes under `testing/mvp` remain unchanged. `testing/test_trainer.ps1 -Work <private-directory>` runs the production trainer against synthetic RAM with a small fake runtime boundary. `python -B testing/test_hwgs.py --dll <private-build>/tf_hwgs.dll` renders a synthetic scene and tests VRAM transfers on the real GPU. These checks do not substitute for gameplay testing.

The route harness additionally accepts:

```text
trainer_probe
setting trainer.infinite_health=on
setting trainer.unlimited_energy=on
setting trainer.damage_multiplier=4
setting trainer.movement_multiplier=1.5
unlock minicons
unlock levels
setting renderer.hw_scale=4
setting renderer.hw_fxaa=on
```

Only implemented live trainer/hardware settings are accepted. Select a profile before boot using a private INI and `PS2X_SETTINGS`; scripts cannot switch profiles mid-session. Unlock requests fail the route outside the permitted hub/profile. `trainer_probe` emits read-only identity, resource, progression and hook-counter evidence. `PS2X_CAPTURE_INTERNAL=1` adds `.internal.png` captures alongside native script screenshots.

Current tests use visible accelerated builds and skip opening and in-game cinematics. Hardware image comparisons (foliage, transparency, bloom, shadows and HUD), all three Autobots and transformations, toggles, deaths, save/reload and progression navigation remain qualification requirements. Whole-campaign reliability requires a complete campaign run. True 60 Hz simulation is a separate milestone; no such claim is made by this implementation.

## Reverse-engineering notes

All offsets below are specific to the verified USA executable. No generated functions are edited.

| Behavior | Verified static lead |
|---|---|
| Player health | `godModePlayer` references the caller of resource subtractor `0x350650`; player resource `0x716fb0` |
| Player ownership | Controller/creature/actor/context backlinks used by the existing car-stick detector |
| Weapon energy | Equipped slots via `0x2867d0`; update `0x3ee400`, setter `0x3ef580`, count `+0x3c`, capacity `+0x50` |
| Outgoing damage | Hit builders `0x505580`, `0x505740`, `0x505ab0`; source `+0x30`, recipient `+0x38`, damage `+0x54`; recipient virtual slot `+0x18` |
| Movement | Player context `+0x1fc`; original code reads this factor in movement routines |
| Mini-Cons | Owned mask at progress `0x7170e0 + 0x2e0`; valid mask derived from original episode grants plus starter ability |
| Level availability | Eight campaign records at progress `+0x34 + index*0x4c`; bit 0 available, bit 1 complete; first gate `+4` |
| Hub gate | Active `taScreenPrGRing` vtable `0x6bd8f0`, owner/manager backlinks and active state |

Static identification establishes where to test; game qualification establishes actual behavior across the campaign.


## Objective guidance preview

`game.objective_guidance` appears on the Display page as **Objective guidance (Amazon preview)**. It draws a compass arrow relative to the Autobot's heading and the distance in game world units to the next mapped Amazon guidance trigger. Camera-only rotation does not rotate this compass.

The preview follows the live training, combat, dropship, ruins, waterfall, mountain-road and mountain-ruins trigger chain. It reads trigger positions, remaining activations and prerequisite links; it does not change mission progress. It hides when no mapped target is available, in other episodes, and for stale or invalid player state. This is route guidance, not yet a campaign-wide mission-objective system. Branches, later saved checkpoints, and all other missions require mapping and live qualification. The setting defaults off.

## Timing investigation

The 16:9 Hor+ camera mode already exists as `display.aspect=16:9`, with `display.aspect_method=horplus`. It is separate from hardware rendering scale and host output resolution.

True 60 fps remains unfinished. The current private experiment selects the native full-rate governor and disables a separate half-rate simulation setter before scene initialization. Physics receives a 16.683 ms timestep, but movement equivalence and sustained real-time performance remain unqualified. Do not interpret 60 guest updates per guest second as 60 rendered frames per wall second. The original gameplay mode remains the normal launch configuration. See [the timing investigation](findings/native60-timing.md).

The optional [AVX2 static VU build](findings/vu-performance.md) has passed the covered hash and route comparisons. The [native render seam investigation](findings/native-render-seam.md) begins mapping mesh batches for future GPU transforms; its opt-in probe observes the original accelerated path and does not replace geometry.

For private reverse engineering, `PS2X_RESEARCH_RAM_DIR` optionally writes a 32 MiB RAM snapshot whenever a scripted `trainer_probe` runs. Create this directory outside the repository; snapshots contain private game data and must not be published. The option is unset in ordinary launches. Probe JSON also reports the governor's 30 Hz byte and the two-field frame-wait byte. `PS2X_HIRES_CAPTURE_KEEP_SIZE=1` preserves explicit window dimensions when using the existing host screenshot diagnostic.
