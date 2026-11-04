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
Here a transform record describes one continuation's vertex chunk; a compute
dispatch can handle many chunks. Complete list sequences can now be recovered
from an EE snapshot as described below. Live ownership is still required for
runtime bypass.

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
and VU state consumed by uncovered batches.
Clipped geometry, expanded sprites, alpha/feedback cases and live rendering
still require qualification. See [the investigation](../../docs/findings/native-render-seam.md).

## GPU-resident batches and native draw submission

`tf_gpu_dual_geometry.cpp` provides the reusable compute/draw component. A
dispatch transforms many mesh chunks into one GPU vertex buffer. Each pooled
allocation contains a combined source/constant upload buffer and a GPU output
buffer, within a default 128 MiB budget. A batch handle prevents reuse while a caller retains it; after release,
the last draw's fence must complete before reuse. Completed smaller allocations
can be retired when a larger batch needs the budget. Capacity exhaustion returns
an empty handle instead of forcing a GPU wait. Destruction defers resources
still referenced by queued commands.

The pool does not map, download, or copy transformed vertices back to the CPU.
It adds no forced command-list execution or wait per batch. The underlying GS
renderer can still submit its normal command lists while drawing. All pool and
handle operations belong to the GS thread; the device must outlive them.

The device patch also exposes `SetNativeDrawHandler`. A handler can submit a
qualified GPU batch using the already prepared `GSHWDrawConfig`, then return
true to suppress the ordinary upload/draw. Returning false retains ordinary
submission and must leave configuration and state unchanged. `RenderHWFromGPU`
bypasses the handler to prevent recursion. This interface still requires a
way to prepare the game's configuration without CPU-transformed geometry.

```powershell
python -B testing/test_gpu_dual_transform.py draw --prepared <private-prepared-directory> --dll <private-gpu-build>/tf_hwgs.dll --report <fresh-private-draw-report.json>
```

The diagnostic draws the 776 captured chunks through this handler using
GPU-resident output. A second outstanding batch changes colors, and a third
dispatch reuses a completed allocation. Both fields, restart masks, return to
ordinary GS drawing, pool exhaustion, in-flight retention and growth are
checked. **3,106 native draws and 3,106 ordinary comparison draws produced zero
differing pixels, with 1,628,705 painted pixels.** Two allocations and one reuse
served the main image comparison. Only final diagnostic images are downloaded;
GPU vertices are never read back. The comparison uses untextured diagnostic
state and fitted tile viewports, so it does not establish live scene equivalence.

## In-level draw-state trace

`PS2X_TF_GPU_DRAW_TRACE=<fresh private CSV path>` installs a read-only handler.
`PS2X_TF_GPU_DRAW_RANGE=3000:3010` selects hardware presentation fields; the
default is the same and the maximum span is 120. The trace has a 100,000-row cap
and reports completion/capping on shutdown. It records draw sizes, shader keys,
target/texture dimensions and depth/blend/LOD features, without changing draws.
It introduces diagnostic overhead and is not a performance measurement.

```powershell
python -B testing/summarize_gpu_draw_trace.py <private-enhanced-level-run-directory>
```

The first enhanced PGO run with this backend passed the cutscene-skipping
Amazon route at field 4977 (3,989 pad reads; 1,194 CD events). The 11-field
in-level trace contains 67,546 draws with no cap: 36,254 ordinary triangle draws,
36,107 textured, 35,792 with depth, 30,908 using automatic LOD and 1,222 using
manual LOD. The two most common triangle shader groups contain 25,450 draws.
No feedback barriers, DATE, second alpha pass or blend multipass were observed
among these ordinary triangles; that does not establish their absence in other
levels or identify which calls belong to the dual-transform family. Palettes
and software/hardware blending remain relevant. Full engine-batch attribution,
texture/bounds preparation and unresolved VU-state handling are still required
before enabling live geometry offload.

## Decoded whole meshes, grouped draws and reusable GPU inputs

`testing/qualify_dual_mesh_lists.py` scans the private EE snapshot for the
observed CALL-list form, then imports single-RET lists with the qualified VIF
formats. The importer now includes V4-8 input. It checks every imported word
against accelerated captures, requires each complete list to match consecutive
VU tickets with unchanged transforms/bias, and rejects ambiguous roots and
overlapping matches. GPU position, STQ, color, bias and restart masks come from
the decoded list; only the view transforms still come from the capture.

```powershell
python -B testing/qualify_dual_mesh_lists.py --snapshot <private-capture>-ee.bin --prepared <private-prepared-directory> --fixtures <private-consecutive-pre-file-list> --output <fresh-private-grouped-directory>
python -B testing/test_gpu_dual_transform.py run --prepared <private-grouped-directory> --dll <private-gpu-build>/tf_hwgs.dll --report <fresh-private-arithmetic-report.json>
python -B testing/test_gpu_dual_transform.py draw --prepared <private-grouped-directory> --dll <private-gpu-build>/tf_hwgs.dll --report <fresh-private-draw-report.json>
```

The captured window yields **740 chunks in 224 complete mesh instances**, with
17,596 source vertices. All **35,192 GPU output vertices** match the independent
packed-register decode and captured raw geometry. Grouped drawing preserves
each chunk's restart boundaries and primitive order. Both field outputs are
checked against separately submitted CPU-reference chunks in the same fitted
viewport: **898 grouped GPU draws versus 2,966 individual comparison draws**,
**489,730 painted pixels and zero differing pixels** with the conservative
metadata viewports described below, including two outstanding
color variants and completed-output reuse. This is an untextured diagnostic,
not permission to reorder live textured draws between fields.

`DualGeometry::uploadMesh` now copies immutable inputs into device-local memory
once. Its `Mesh` handle can be reused with new transform records; the resident
`prepare` overload checks chunk offsets without rereading all source vertices.
The source pool defaults to 64 MiB, independently of the 128 MiB transform/output
pool. Released meshes can be evicted only after their last compute fence has
completed. Initial staging uploads are deferred through the copy fence and may
temporarily add up to another source-pool budget. No upload/dispatch forces a
GPU wait. Callers own asset identity, invalidation and handle release.

The grouped diagnostic performs two source uploads and three dispatches:
1,126,144 source bytes total, 319,680 transform bytes total, and **no source
upload on reuse**. It also checks retained/in-flight source protection, budget
exhaustion, completed-source eviction, foreign handles and changed chunk-offset
rejection. Diagnostic export `_v3` prevents accidental calls through the older
argument layout. Reports bind both payloads and the grouping manifest by hash.
Reuse now swaps the second field's projected X/Y matrix rows, compared against swapped
captured X/Y coordinates, to check transform updates on an unchanged resident
mesh. The previous field-matrix swap did not establish this: those matrices
were equal in the captured inputs. The diagnostic now requires changed output
coordinates before accepting its transform-update check.

The runtime still needs ordered engine-to-GS metadata and draw-state preparation
without bulk CPU geometry. The current GS front end derives texture bounds,
LOD, depth and blend decisions from its CPU vertex trace and also reads vertices
in special-case paths. Passing only the GPU buffer does not remove that work.
These changes neither enable live VU bypass nor establish an FPS improvement.

## Bounds preparation without transformed CPU vertices

`tf_gpu_dual_bounds.cpp` computes source position/STQ extents and color ranges
once, during mesh upload. `DualGeometry::bounds` merges those cached extents for
a complete mesh, requires shared matrices/bias and matching chunk offsets, and
produces both fields' draw metadata. It does not retain source vertices on the
CPU, read GPU output, or evaluate every vertex again.

Each projection evaluates eight object-box corners. Bounds on separate VU
multiply/add, reciprocal and final multiply rounding expand the projected box.
They include RTZ and flush-to-zero error. Linear-fractional corner extrema are
used only with a strictly positive homogeneous denominator. Texture coverage
accounts for STQ bias and division rounding while cancelling the shared
perspective factor. Packed X/Y/Z wrap conservatively covers the full affected
axis. Nonfinite/overflowing data, a box reaching W=0, or nonpositive output Q
decline preparation; callers must retain the ordinary path for these cases.

The helper builds as a separate strict-floating-point object, since the GS
library's `/fp:contract` is incompatible with MSVC `/fp:strict`. It saves and
restores the caller's environment, including VU-style RTZ/FTZ/DAZ controls.

All **224 whole-mesh instances** qualify in the private enhanced capture.
Every one of their **35,192 output vertices** lies within the computed packed
XYZ, STQ, normalized texture and color bounds; no coordinate wrap occurs in this
set. Zero-W and NaN inputs are rejected. The largest box has **10.15244 times**
the exact projected vertex-box area, so conservative preparation can enlarge
texture-cache work; this has not been measured in a live scene.

The grouped GPU diagnostic now constructs its viewport from these cached
metadata bounds, without consulting transformed CPU vertices. Only the
independent comparison draws use the captured vertices. The image comparison
still passes with zero differing pixels, including the corrected X/Y transform
update test. The fitted viewports and fixed untextured draw state remain
diagnostic; they do not implement the game's texture/depth/blending decisions.

The scoped renderer integration below now feeds these bounds into GS trace/LOD
preparation and handles per-vertex special cases. Supplying bounds alone is
still insufficient for live bypass: ordered engine/VIF/GIF metadata and
uncovered VU-state readers remain required.

## Reusing identical field transforms

All 740 chunks in the grouped set have bit-identical first/second field
matrices. The compute shader now compares their words and reuses the first
field's transformed record when equal, avoiding a second set of RTZ transform,
reciprocal and perspective calculations. Both output records are still written,
and their GS SCISSOR/XYOFFSET state remains separate. Different matrices execute
both transforms.
The CPU metadata helper also reuses its first bounds result for identical
matrices, avoiding a duplicate corner projection.

The updated shader matches all **37,136 native and raw output vertices** in the
larger 776-chunk arithmetic set. The resident-draw check changes X/Y only in the
second field on reuse, so it exercises both equal-matrix reuse and distinct
matrix computation in one run. It still has zero pixel differences. This is
validated elimination of redundant arithmetic in the GPU component, not a
measured frame-rate gain or live geometry offload.

## GS frontend preparation from native mesh metadata

The opt-in PCSX2 patch now adds `GSRendererHW::DrawNativeTriangles`.
`prepareDualTrace` converts cached mesh bounds into GS screen/texture units,
rounding texture ranges outward. The native trace uses the ordinary filter/LOD
logic and conservative coverage/overlap answers. The existing texture cache,
depth, alpha-test and blend preparation then produce the actual draw config.
`SetupIA` binds the native vertex path without copying transformed CPU vertices.

CPU vertex reads and mutations in hardware renderer helpers go through a lazy
materialization accessor. The first such access invokes the caller's fallback
and selects ordinary CPU drawing for that entire submission. A declined final
GPU submission also materializes and draws normally. The callback contract
requires exact vertices, no exceptions, and no device/config mutation on a
declined GPU submission. No geometry output is read back from the GPU.

This is a synchronous **GS-thread draw-only seam**, not a per-mesh cross-thread
wait. It flushes preceding ordinary draws and preserves stream-buffer leftovers;
it deliberately does not synthesize final GIF or VU latches. The caller still
owns those updates and ordered delivery. The native callback is placed at the
current draw's submission, so it cannot accidentally replace an older deferred
channel-shuffle draw.

Eligibility currently requires textured IIP triangles, finite bounds, positive
Q, no coordinate wrap, supported depth handling, and an established exact
render target covering the scissor. Pending shuffles, split clears, game-specific
draw hooks, AA1, FST, and large-ST rewriting decline. Cold target creation is
excluded: conservative bounds can allocate a different target size and alter
floating-point interpolation. **Nonconstant depth must be proven independently**;
a loose depth interval cannot prove it. Flat-depth texel rounding is unsupported.
The diagnostic obtains this witness from captured reference vertices. A live
caller must derive it from source metadata before using this seam.

```powershell
python -B testing/test_gpu_dual_transform.py draw --frontend --prepared <private-grouped-directory> --dll <private-gpu-build>/tf_hwgs.dll --report <fresh-private-frontend-report.json>
```

The frontend diagnostic uses all 224 captured mesh instances through the real
GS frontend, with generated checkerboard textures and four rendering modes:
depth-tested texture rendering, alpha testing/blending, mipmap filtering/blending,
and deliberate final-submission fallback. Each comparison uses independently
submitted GIF reference triangles. Both targets are established at the same size
before drawing. The first three modes require **zero CPU materializations**.

Results at **2048 × 1792 internal resolution**: 1,216 GPU draws and 408 deliberate
CPU fallbacks, 400 mipmap-filtered submission callbacks, **zero differing pixels
across 14,680,064 full-resolution comparisons**, and zero differences in the
reduced GS VRAM comparisons. The diagnostic also rejects nonfinite bounds,
out-of-range indices, and missing nonconstant-depth proof before callbacks run.
Texture data and render state are synthetic; this does not qualify arbitrary
game texture feedback, palettes, GS culling, or unfinished primitive sequences.

The next runtime step is ordered mesh/source ownership and VIF/GIF handoff,
including final-latch updates and fallback materialization from retained source
data. Live VU bypass remains disabled. These renderer results do not establish
a gameplay FPS gain or true rendered 60 fps.

The enhanced Amazon route also passed with this patched renderer and the PGO,
static-VU runtime, direct presentation, 16:9 and 4× internal rendering. Opening
and arrival cinematics were skipped. Its gameplay-only window measured
**54.9299 physics updates/s**, median **17.736 ms**, p95 **22.2 ms**. Live native
geometry submission was disabled in that run. This single sample is a shared
renderer regression check, not an attributed GPU-offload speedup or a count of
distinct rendered frames.
