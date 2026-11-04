# GPU-resident hardware presentation

The Windows hardware renderer can present without downloading and re-uploading each complete frame. Use `launch/play.ps1 -GameFolder <private-game-folder> -Presentation direct`, or set `PS2X_GS_BACKEND=hwgs` and `PS2X_HWGS_DIRECT=1` before a direct executable launch. This selects D3D12; an explicit incompatible `PS2X_HWGS_API` is rejected. `-Presentation readback` restores the Vulkan CPU-copy path. These switches do not change simulation timing.

## Ownership and synchronization

The PCSX2 GS worker copies the merged RGBA8 output into one of three shared D3D12 textures. Each texture has a shared fence. The existing OpenGL window imports the texture and fence, waits on the GPU, copies the active rectangle into its presentation texture and sets opacity on the GPU. The existing scaling, output-resolution, overlay and objective-guidance code then draws normally. There are GPU-to-GPU copies; this is not a claim of zero GPU copies.

Each slot transitions through free, writing, ready and acquired states under a mutex. The producer never overwrites an acquired slot. It signals an odd ready value; OpenGL signals the following even release value and flushes before freeing the slot. D3D12 waits on that release on the GPU before reuse. Unacquired old frames can be replaced in D3D queue order, and acquiring the newest frame retires older queued frames so they cannot be redisplayed later.

Texture capacity accommodates alternating field dimensions. Increasing internal resolution can allocate larger resources after a bounded CPU fence wait; normal reuse does not perform that wait. A generation number invalidates old OpenGL imports. Shutdown finishes OpenGL work and releases imports before closing the GS device and shared handles.

The implementation uses Khronos [external objects](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_external_objects.txt) and [Win32 external handles](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_external_objects_win32.txt). It verifies both required extensions and matching adapter LUIDs. The DLL retains ownership of its NT handles. There is no silent CPU-copy fallback after an import or synchronization failure.

## Readbacks that remain

Guest-requested VRAM transfers retain their existing behavior. The covered Amazon route makes small 64x64 readbacks for game logic; these are separate from the removed full-frame presentation download. Explicit screenshots also read pixels once when requested and never re-upload that image. Direct-path script screenshots contain the full internal image instead of the legacy 640-pixel comparison stride. Use the existing host-capture options to inspect the final window and overlays.

`PS2X_HWGS_PROFILE=1` should report `readback=0/0.0ms` during direct presentation. `[hwgs:direct]` records actual output dimensions and zero full-frame CPU bytes. The runtime also bypasses the legacy CPU mailbox and `UpdateTexture` upload. Display-buffer flip counters retain their original meaning; they are not proof of distinct 60 Hz scene images.

`PS2X_TF_PRESENT_TRACE=<fresh-private.csv>` optionally records publish, acquire
and release events, with host timestamps, frame sequence, guest presentation
tick, active dimensions and scene metadata. It stores bounded metadata in memory
and writes the CSV after producer shutdown; no frame pixels are read back.
The close summary must report `capped=0 write_error=0`. Acquired-frame intervals
measure delivery to the window, including skipped publications and dimension
changes. They do not establish unique scene images or monitor scanout.

The enhanced GPU-geometry level run recorded 2,273 acquisitions at 55.5695
handoffs/s during its gameplay window, with no skipped publications between
the first and last acquisition. Internal dimensions stayed at 2048x1788;
there were no per-frame size changes in this window. Physics updated at
55.7436 Hz. The trace closed uncapped and without write errors. These rates
remain below 60 and measure different asynchronous boundaries.

## Qualification

Tests use the private USA disc and Trainer hub seed, visible accelerated builds, and skip both opening and in-game cinematics. Raw logs, captures, saves and generated code remain outside the repository. Qualification so far covers the RTX 3080 Laptop on Windows, not other drivers, multi-GPU configurations or Proton.

The initial direct run matched all six terminal counters and player probes from the readback timing route: 3,763 fields, 2,775 pad reads, 18,496,270,155 cycles, 1,119 CD operations, 3,100 I5 events and 1,118,322 MSCALs. It passed robot/vehicle/robot transitions and orderly shutdown. Captures show the correctly oriented hub and playable Amazon scene.

The feature route passed 4x -> 8x -> 4x internal scaling, FXAA on/off, objective guidance, menu pause/resume and shutdown. Actual internal outputs were 2048x1788 and 4096x3576. Host captures were 2560x1369 on this desktop with 16:9 fitted content; the configured output surface was 1920x1080. A requested 2560x1440 window failed the existing physical-client validation and rolled back, so this test does not qualify 1440p output selection. The menu stopped guest time and resumed it afterward. Existing field/combing artifacts remain.

The route is `testing/enhancements/direct_presentation.tfs`. For this exact private seed and native-timing variant 3, `PS2X_PCMENU_AUTOTEST=3000` opens the settings menu during playable gameplay. `PS2X_HIRES_CAPTURE_DIR=<private-run>`, `PS2X_HIRES_CAPTURE_KEEP_SIZE=1`, and `PS2X_HIRES_CAPTURE_VS=3810,3895,3980,4110,4200` request final-window captures around the feature changes. These field numbers are seed/timing-specific; do not reuse them to pause a different route during a cinematic.

Six alternating runs used the same build, D3D12 renderer, 4x scale, 16:9, seed, route and experimental native-timing variant 3. Checkpoint screenshots were disabled. Each matched all six counters above, every recorded player position and robot/vehicle mode, and a 0.0166833512-second physics timestep. The measured interval was guest fields 3020 through 3740: 720 intervals between 721 consecutive physics calls.

| Presentation | Gameplay updates/s, three runs | Script wall seconds, three runs |
|---|---|---|
| CPU readback | 24.63, 25.35, 24.81 | 94.1, 92.1, 96.3 |
| GPU shared texture | 41.09, 36.71, 37.53 | 76.1, 82.0, 79.6 |

These are observed desktop-run results, not an isolated-machine benchmark. System CPU samples averaged 25-31% in the first four runs, with background ChatGPT/browser activity observed. Report the spread rather than the fastest run. Direct presentation reported zero full-frame readbacks throughout. Gameplay samples showed the VU worker at about 100% CPU, approximately 90% in VU execution and 10% in VIF work; the hardware bridge was no longer saturated. Pad/physics update rates still do not establish distinct 60 Hz scene images or equivalent 60 Hz gameplay timing.

The verified direct build was selected in the user's private launcher, retaining the existing save/settings locations. The original build remains available. Full-campaign reliability and true 60 fps remain separate gates.
