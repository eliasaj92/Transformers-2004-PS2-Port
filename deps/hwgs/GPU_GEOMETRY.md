# Experimental GPU geometry

This opt-in component builds the D3D12 interface needed to draw GPU-produced
vertices through the existing hardware renderer. It does not yet replace live
Transformers VIF/VU batches or establish a gameplay performance improvement.

Use a separate private checkout of the PCSX2 revision in `dependencies.json`
and an output directory outside this repository:

```powershell
cmake -S deps/hwgs -B <private-gpu-build> -G Ninja -DCMAKE_BUILD_TYPE=Release -DKZGS_PCSX2_DIR=<private-pcsx2-checkout> -DKZGS_DEPS_DIR=<private-dependencies> -DKZGS_ISA=AVX2 -DTF_HWGS_GPU_GEOMETRY=ON
cmake --build <private-gpu-build> --parallel 4
python -B testing/test_gpu_geometry.py --dll <private-gpu-build>/tf_hwgs.dll
```

Configuration verifies the pinned PCSX2 revision and applies
`pcsx2-gpu-vertices.patch`, or recognizes that it is already applied. It refuses
conflicting edits instead of resetting the checkout. The default build leaves
this feature off.

`GSDevice12::RenderHWFromGPU` takes a GPU buffer of 32-byte `GSVertex` records
and CPU indices. The caller supplies an already prepared `GSHWDrawConfig`,
transitions the output from unordered access to vertex input, and keeps it
alive through completion of every command list that uses it. Calls must run on
the GS thread. Indexed triangles are currently supported; vertex expansion,
undersized buffers and out-of-range indices are rejected before changing state.
The call uses the existing GS draw passes and restores the ordinary vertex
upload path afterward. Acceptance is not a GPU completion signal.

The synthetic diagnostic dispatches a matrix transform, draws directly from
its output buffer twice with overlapping colors, rejects invalid input, then
performs an ordinary GS draw. One final image download verifies the result.
Neither generated vertex buffer is CPU-mapped. On the RTX 3080 Laptop GPU the
first successful check produced 512 red, 224 green and 32 blue pixels.

`shaders/dual_transform.hlsl` and `tf_gpu_dual_probe.cpp` begin qualification of
real unclipped dual-transform mesh inputs against privately captured accelerated
output. Their readback API is an offline arithmetic diagnostic, not a proposed
per-frame runtime path. It must not be used to claim live offloading or speed.
Here a compute batch is one captured continuation's vertex chunk; identifying
the owning whole engine mesh submission is still required for runtime bypass.

The shader now matches all 37,136 output `GSVertex` records from 776 supported
captured continuations, using 18,568 source vertices and two transforms per
vertex. The first ordinary float implementation differed in XY by up to one
GS subpixel and also changed depth and STQ. The corrected implementation uses
double intermediates and explicit toward-zero float conversion, preserves
separate multiply/add steps, flushes subnormals, and refines reciprocal results
using exact products. Double-precision shader support is checked at runtime.
This arithmetic result covers the selected finite, unclipped inputs; it does
not qualify arbitrary floating-point inputs or clipped/expanded primitives.

The initial native-format test incorrectly retained the VU's fixed-four-bit Z
value as GS depth and omitted fog. The corrected version decodes PACKED XYZF2
Z from bits 4..27 and fog from bits 4..11 of the fourth word. An independent
decoder compiled against the pinned renderer's actual `GIFPackedReg` and
`GSVertex` declarations now checks every expected record before GPU execution.
Positive-zero Q follows the renderer's `FLT_MIN` substitution. Unused UV is
canonicalized to zero; this is not a comparison of inherited FST=1 coordinates.

Prepare and compare private fixtures from the enhanced static-VU capture:

```powershell
# First verify the capture's trace with testing/inspect_native_seam_capture.py.
python -B testing/test_gpu_dual_transform.py prepare --fixtures <private-pre-file-list> --program-manifest <private-extraction>/manifest.json --output <fresh-private-prepared-directory>
python -B testing/test_gpu_dual_transform.py run --prepared <private-prepared-directory> --dll <private-gpu-build>/tf_hwgs.dll --report <fresh-private-report.json> --gpu-capture <fresh-private-gpu.bin>
```

Preparation checks complete program identities, the supported input layout and
captured packet framing. Prepared bytes are hashed and remain private. The run
command fails on any output-word mismatch and reports aggregate differences.
The rejection check also detected an intentionally changed expected attribute
as exactly one mismatched word.
Reading vertices back is deliberately confined to this offline diagnostic;
the direct-draw interface above consumes GPU buffers without that step.

## Captured-window VU bypass

The diagnostic shader additionally writes raw STQ/XYZ when compiled with
`TF_GPU_DUAL_CAPTURE`. Both the native vertex records and these raw records
match all 37,136 captured outputs. The private GPU capture is bound to the
uploaded inputs and constants by a SHA-256 digest. Production shader builds
omit the diagnostic region.

`tf_dual_state.cpp` models packet headers, colors, padding, restart words,
VF17/VF18 and guest cycles for the selected unclipped continuation. It evaluates
at most one retained vertex on the CPU and leaves bulk STQ/XYZ unresolved.
Its preservation check passed all 776 selected calls, including the distinct
17- and 18-vertex tail behavior and restoration of the caller's floating-point
environment. It is not enabled in the game runtime.

The accelerated replay now supports actually skipping these calls. The driver
checks carried geometry and constants against the real GPU upload, applies
the preservation model, and composes diagnostic packets with GPU results.
It leaves guest geometry and unmodeled registers unresolved, applies observed
external writes, then executes the remaining calls using static VU code.

```powershell
cmake -S testing/vu1_replay -B <private-replay-build> -G Ninja -DCMAKE_BUILD_TYPE=Release -DPACKAGE_DIR=<private-static-vu-package>
cmake --build <private-replay-build> --parallel 4
python -B testing/test_gpu_dual_transform.py replay --prepared <private-prepared-directory> --gpu-capture <private-gpu.bin> --gpu-report <private-report.json> --fixtures <private-consecutive-pre-file-list> --runner <private-replay-build>/tf_vu1_replay.exe --report <fresh-private-replay.csv>
```

The measured window passed all **5,000 consecutive calls**, with **776 VU
calls replaced**, **11,998,416 packet bytes matched**, no fallback and no
pending transfer. Checked subsequent memory writes and control/cycle results
matched. Full register state differed on 1,240 calls, so this is explicitly
observable-output qualification for that window, not complete state equality
or proof for other readers/levels. Equal-value external writes cannot be
identified from these snapshots. Live integration still needs either a proved
ownership boundary or materialization of unresolved state before an uncovered
reader. Replay timing excludes GPU submission and cannot establish a speedup.

Before game integration, the remaining work includes complete mesh ownership,
GS state/texture/bounds preparation, ordered native commands alongside GIF,
buffer reuse protected by fences, and VU state consumed by uncovered batches.
Clipped geometry, expanded sprites, alpha/feedback cases and live rendering
still require qualification. See [the investigation](../../docs/findings/native-render-seam.md).
