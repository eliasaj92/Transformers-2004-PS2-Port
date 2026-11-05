# Native whole-mesh renderer prototype

The first complete textured, non-grass mesh now renders through an owned
mesh/material/scene interface directly into native D3D12 color and depth targets.
This target does not link to `tf_hwgs`, PCSX2, generated EE code or the PS2 runtime.
It cannot call their GS frontend. It is an offscreen correctness prototype, not
an enabled replacement for live game submissions or a gameplay FPS result.

## Implemented boundary

```text
one-time private VBIN/ITF import
    -> Mesh / Texture / Material
    -> persistent default-heap vertex, index and texture resources

owned SceneSnapshot (camera + instance transforms/tints + resource handles)
    -> native vertex/pixel shaders
    -> one DrawIndexedInstanced per complete mesh instance
    -> GPU color target + completion fence
```

`renderer.h` is the common interface. It does not accept PS2 addresses, VU
programs, GIF packets, GS registers or guest VRAM. Positions are transformed in
the native vertex shader. Texture decoding and strip conversion happen once in
the asset importer, not during frame rendering. The caller's CPU mesh/texture
data is destroyed before the probe renders any frame.

The first material pass is explicitly opaque, two-sided, bilinear repeat,
vertex color multiplied by albedo/tint, with conventional D32 depth testing.
It has no game-specific lighting, fog, bloom, animation or feedback handling.
The narrow importer rejects anything outside the two hash-qualified USA assets;
it is not a general VBIN/ITF importer. The rendering interface itself is shared
and is not specific to rocks or any VU shader family.

`render()` returns a GPU resource in `PIXEL_SHADER_RESOURCE` state and a fence.
It does not read pixels back. `readbackForValidation()` is a separate, explicitly
diagnostic operation used to check the output. The standalone prototype owns its
device/queue and permits one frame in flight. An overload now accepts the
enhanced game's device and queue, retaining them through COM ownership while
using separate lists, heaps and targets. This path passed the in-process test
below. Shared world depth/color ownership and independent render cadence remain
subsequent work. The renderer creates no window or swapchain.

## Actual game asset

Private input from the user's USA disc, archive `/TFA.ZIP`:

- `LEVELS/EPISODES/01_AMAZON_1/ECOSYSTEM/ROCKS/ROCK_J3.VBIN`
  (`obj34_7`): all 87 vertices, one material element, 21 triangle strips,
  168 nondegenerate triangles.
- Its explicit `Jungle_Cliff` material binding resolves to `JUNGLE_CLIFF.ITF`,
  a 128x128 RGB24 texture. The importer adds opaque alpha; it does not substitute
  a checkerboard, invented material or texture chosen by visual resemblance.

Asset parsing was informed by the primary
[TF2004 File Editor source](https://github.com/bbatts15/TF2004-File-Editor/blob/main/Mesh.cpp).
The importer independently validates the binary sections, counts, index bounds,
finite attributes, source hashes and material binding. No game assets are stored
in this repository.

## Reproduce the correctness probe

Use an x64 MSVC developer shell. All output paths must be outside the checkout;
the example uses a fresh private directory. The probe currently selects the
RTX 3080 Laptop GPU by name and refuses software fallback. `--debug` requires
the Windows D3D12 debug layer. Build Release, and finish compilation before
running the probe.

```powershell
$nativeWork = 'C:/path/to/private/native-prototype'
cmake -S deps/native_renderer -B "$nativeWork/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$nativeWork/build" -j2
python testing/prepare_native_mesh.py `
  --iso 'C:/path/to/Transformers (USA).iso' `
  --isomap 'C:/path/to/generation/isomap.tsv' `
  --output "$nativeWork/fixture"
& "$nativeWork/build/tf_native_mesh_probe.exe" `
  "$nativeWork/fixture/asset.bin" "$nativeWork/render" `
  "$nativeWork/build/mesh.hlsl" --debug
python testing/verify_native_mesh.py --asset "$nativeWork/fixture" --output "$nativeWork/render"
```

The verifier requires NumPy and Pillow. It reads saved results only and does not
start another probe or game. Four views are rendered in the single invocation:
original framing, changed camera/model, two overlapping instances with the far
one drawn last, and a near plane intersecting the mesh. Three malformed
asset/handle submissions must be rejected without mutating GPU state.

## Recorded result, 2026-11-05

MSVC 19.44 Release, NVIDIA GeForce RTX 3080 Laptop GPU, D3D12 debug layer enabled.
One probe invocation, four 1920x1080 frames, five complete-mesh draws, no game
launches. One mesh, texture and material created. **70,684 asset bytes uploaded
once**, unchanged after every camera/model update. No D3D12 validation errors.

An independent NumPy reference implements homogeneous clipping, perspective
interpolation, bilinear sampling and depth. It independently expands the source
strips, including alternating winding and restart boundaries. Results:

| View | Coverage intersection/union | Interior RGB RMSE (0..255) | Max interior RGB error | Max depth error |
| --- | ---: | ---: | ---: | ---: |
| Asset | 0.99999513 | 0.1870 | 1 | 5.24e-7 |
| Camera/model update | 0.99999587 | 0.1948 | 1 | 3.30e-7 |
| Depth overlap | 0.99999565 | 0.1850 | 1 | 5.24e-7 |
| Near clipping | 0.99999469 | 0.1824 | 1 | 2.11e-5 |

Color/depth comparisons exclude a two-pixel triangle boundary determined from
the reference primitive IDs, not from observed GPU errors. Coverage compares the
whole silhouette. Disabling depth in the reference changes 134,248 checked
pixels in the overlap view, demonstrating that the case actually exercises
occlusion. The near-plane case clips 86 source triangles. Thresholds were set
before the run and all passed.

Private evidence locations on the implementation machine:

- `C:/Users/dounh/tf-work/native-mesh-fixture-01/manifest.json`
- `C:/Users/dounh/tf-work/native-mesh-render-01/probe.json`
- `C:/Users/dounh/tf-work/native-mesh-render-01/verification.json`
- `C:/Users/dounh/tf-work/native-mesh-render-01/contact-sheet.png`

The executable dependency inspection contained only D3D12, DXGI, D3DCompiler,
Windows and C/C++ runtime libraries. There is no legacy draw fallback inside
this prototype. The normal enhanced game and its fallback remain untouched.

## Enhanced-game host test, 2026-11-05

The same four native views also passed **inside one enhanced Amazon level run**,
using the game's actual D3D12 device and DIRECT queue. The renderer accepts those
interfaces directly and retains its own command lists, heaps, assets and targets.
The shared probe code is used by both the standalone executable and host adapter.

Build the enhanced renderer with `TF_HWGS_NATIVE_MESH_PROBE=ON` (default OFF).
The native diagnostic remains disabled until `PS2X_TF_NATIVE_HOST_PROBE` names a
fresh private output directory. `PS2X_TF_NATIVE_HOST_ASSET` and
`PS2X_TF_NATIVE_HOST_SHADER` supply absolute input paths. The runner creates the
`PS2X_TF_NATIVE_HOST_ARM` file only after the stable in-level `player_ready`
checkpoint. The host enables standard D3D12 validation before device creation;
GPU-instrumented validation of the entire legacy renderer is not enabled.

```powershell
python testing/run_native_mesh_host.py `
  --exe 'C:/path/to/enhanced-probe-build/ps2EntryRunner.exe' `
  --elf 'C:/path/to/generation/SLUS_206.68' `
  --iso 'C:/path/to/Transformers (USA).iso' `
  --seed 'C:/path/to/private/Amazon-save/mc0' `
  --settings 'C:/path/to/private/windowed-1280x720.ini' `
  --asset "$nativeWork/fixture" --shader 'deps/native_renderer/mesh.hlsl' `
  --work "$nativeWork/host-run"
```

Observed result in private `runs/level-native-host-windowed-01`:

- One game-68 invocation, unchanged accelerated/static-VU game-67 executable,
  test renderer DLL `23820260d438a6c33e288d4d790979118da60f55df5c97d803dea733ff1263b3`.
- Normal process priority, no concurrent compilation, visible bordered
  1280x720 window, 4x internal scene output, opening/arrival cinematics skipped.
  No menu checks or original-build tests.
- Native target: 1920x1080, four views/five complete-mesh draws, one-time 70,684
  byte asset upload, standard debug layer, no native-pass validation errors.
  All color/depth/coverage results match the standalone table above.
- The game's completed 2048x1788 RGBA output was byte-identical before/after
  the native pass: **14,647,296 bytes** checked. Subsequent gameplay output was
  captured 60 renderer ticks later and visually inspected. The route exercised
  movement/camera input and passed; private input save/settings hashes matched.
- The game's route marks were EE fields 2697..3477. The native pass was renderer
  tick 2691, followed by continuation at 2751. These counters have different
  origins and asynchronous delivery. The initial verifier incorrectly compared
  their values; it was corrected to check the arming/log order and each clock
  separately. Re-verification used saved files only, with **no second game run**.

`host-verification.json` records the final PASS. `host-run.json` records the game
exit code, visible-window verification and protected input hashes result.
The run is diagnostic correctness evidence, not a speed measurement. Readbacks
occur only for the bounded native/host validation images. The normal launcher
and gameplay settings were not changed.

## What this establishes, and the next boundary

This proves a complete original game mesh can be retained and textured using
the native interface without any GS frontend. It does **not** prove equivalence
to a live game's effective camera/material/lighting state, campaign coverage,
60/120 fps, or safe suppression of a guest submission. The reference is the
authored asset plus an independent rasterizer, not a matching PCSX2 screenshot.

The host test validates device/queue sharing during gameplay. It still renders
the authored fixture into separate native targets; it does not replace a world
object or prove the live game's material/lighting state. The smallest subsequent
integration experiment is one identified rigid-mesh submission with an owned
live instance/camera/material snapshot and an explicit world color/depth
ownership boundary on the now-tested host device. Compare that output
and preserve required guest side effects before suppressing its legacy draw.
Unsupported material/animation/feedback cases stay on the enhanced renderer.
Only measure a speedup after this removes work from live frame delivery. No
timestep or frame cap was changed by this prototype.
