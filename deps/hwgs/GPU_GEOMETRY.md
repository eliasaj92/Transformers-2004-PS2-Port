# Experimental GPU geometry

This opt-in component draws GPU-produced vertices through the existing hardware
renderer. Mode 1 retains full accelerated VU execution. Mode 2 replaces the bulk
of admitted dual-transform calls with a short CPU tail and deferred VU memory;
see [its qualification and limits](#guarded-cpu-tail-and-deferred-vu-memory).
`PS2X_TF_GPU_GROUPS=8` additionally enables ordered groups of resident-source
compute jobs; see [the group contract and measurements](#ordered-gpu-dispatch-groups).

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

Results at **2048 Ã— 1792 internal resolution**: 1,216 GPU draws and 408 deliberate
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
static-VU runtime, direct presentation, 16:9 and 4Ã— internal rendering. Opening
and arrival cinematics were skipped. Its gameplay-only window measured
**54.9299 physics updates/s**, median **17.736 ms**, p95 **22.2 ms**. Live native
geometry submission was disabled in that run. This single sample is a shared
renderer regression check, not an attributed GPU-offload speedup or a count of
distinct rendered frames.

## Ordered live geometry handoff

`PS2X_TF_GPU_DUAL=1` enables a guarded live draw qualification path for the
supported USA executable. It requires the GPU-enabled HWGS build and D3D12.
Use an enhanced static-VU build, direct presentation, and the saved Amazon
performance route with cinematics skipped. Rebuild runtime and VU core objects
after changing queue headers; their inline queue code must agree on layout.

This mode still executes the static VU call. Before execution, it authenticates
the manager and both microprograms, then snapshots the qualified source vertices
and transforms. At XGKICK it validates packet framing, colors and strip restart
flags and derives the nonconstant-depth witness from the computed packet.
Consequently this stage does **not** remove CPU geometry work.

The owned companion follows the original packet through the VIF worker, GIF
arbiter, threaded raw batch and GS command ring. Earlier coalesced packets are
flushed before its submission. Queues never retain guest pointers or associate
geometry by a later packet hash. The VIF queue accounts for companion storage
in its existing memory limit. GS submission is asynchronous; there is no
per-packet completion wait or geometry download.

On the GS thread, a 2,048-entry cache reuses byte-identical source geometry,
with a 192 MiB source pool and 128 MiB output pool. The
existing GPU pool computes vertices and supplies them to the textured native
frontend. Each field preserves its original state prefix and triangle order.
After a consumed field, two ADC-marked tail vertices update the ordinary GS
latches and strip leftovers without drawing again. Unsupported cases retain
the original GIF path; a renderer helper needing CPU vertices decodes the
retained accelerated output. GPU resources retain their existing fence-based
lifetime rules.

Compute dispatch is deferred until the frontend accepts a GPU draw. Culled
draws and cases already requiring CPU vertices do not dispatch compute.
Both fields share one output batch. A device preflight checks draw eligibility
before compute changes graphics state, preserving the fallback contract.

The output pool now resumes its search after the last reused page, instead of
rescanning the same in-flight prefix for every packet. `page_probes` reports
the actual candidate visits. Upload pages stay mapped for their lifetime;
only released pages with completed GPU fences can receive new CPU writes.
This uses D3D12's [persistent mapping contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map#advanced-usage-models).
Source uploads and draw ordering retain the existing ownership rules.
The revised pool passes the 224-group retained/in-flight/budget/reuse diagnostic
with 898 native draws and zero differing pixels. The textured frontend check
also passes all four modes with 1,216 GPU draws and zero differences across
14,680,064 full-resolution comparisons. These are correctness checks, not
frame-time measurements.

`[gpu-dual]` reports runtime selection/attachment counts, and `[gpu-dual:gs]`
reports GPU draws, CPU materializations, source reuse, uploads and capacity
declines. A route passing without nonzero `gpu_draws` does not qualify this path.
Removing the static VU call still requires preserved or lazily materialized
guest state, a source-derived depth witness, and fallback vertices independent
of that call. Dispatch grouping and live texture/state correctness must be
measured before enabling it as a performance default.

The first live run exposed source-cache thrashing: its 256 entries caused
125,208 uploads in 155,337 native packets. That enhanced run was stopped after
reaching Amazon; it was not a passing performance result. With the larger
bounded cache, the same enhanced level route passed, with 640,224 GS packets,
1,190,571 native GPU draw submissions, 6,297 CPU materializations and 595,374
compute dispatches. Source uploads fell to 1,687 (1,255,936 bytes), with 638,537
cache hits (99.7365%) and no source/output capacity declines. These totals
include navigation; the timed gameplay window is separate.

That gameplay window measured 49.9560 physics updates/s, median 18.965 ms and
p95 29.814 ms, with 16:9, 4x internal rendering and 1920x1080 direct output.
Only enhanced accelerated builds ran, with opening and arrival cinematics
skipped. The final frame was inspected; it does not establish pixel equality
throughout the level. CPU VU geometry still runs, and this is neither a net
offload speedup nor proof of distinct rendered 60 fps.

## Guarded CPU tail and deferred VU memory

`PS2X_TF_GPU_DUAL=2` selects the experimental bypass on enhanced static-VU builds
with D3D12 HWGS. It uses the exclusive VIF worker and the same program identities
and packet checks as mode 1. Other programs still use accelerated execution.
Small batches and the special 17/18-vertex tails retain their complete call.

For an admitted batch, the static package executes only its final three or four
vertices in scratch memory, with that scratch XGKICK suppressed. Native code
restores the original count/address bookkeeping, manager scratch registers,
retained registers and guest-cycle count. The original final vertices remain
available for GS latches and a conservative nonconstant-depth witness. This
shortcut requires saturated arithmetic sticky flags, disabled D/T stops, a
conservatively positive W bound for both fields, and nonoverlapping constants.
An unproved case takes the ordinary accelerated path.

Unresolved STQ/XYZ lanes retain immutable source geometry through a bounded
VU-memory ownership map. Full VIF/EE writes explicitly discard ownership, even
when they write an equal value. Hardware UNPACK can also discard ownership when
all XYZ coordinates are replaced and only W is protected: W is already valid.
A protected coordinate, partial byte write or unproved format still resolves
the affected quadwords first. Reads of unowned ranges avoid entering the strict
floating-point scope. Subsequent admitted calls resolve their input/constant/padding
reads; other VU programs and public EE reads resolve deferred memory before
access. Two authenticated setup programs read only qword 4.x (manager) or
matrix qwords 0..3 (context loader), so the remaining deferred data survives
their ordinary accelerated execution. New packet headers/colors also discard
old geometry ownership where
the layouts overlap. Scratch work never replaces unrelated deferred owners.

Version 2 GIF companions distinguish incomplete geometry from ordinary packets.
The hardware renderer and generic/decoded fallbacks reconstruct needed vertices
from the retained sources using strict round-toward-zero CPU arithmetic. They
do not download GPU geometry. The normal GPU draw consumes the existing resident
source cache and compute output; only the resolved tail is submitted for final
GS latch updates.

The accelerated offline check covers all 776 selected captured calls: complete
semantic VU state, complete memory after materialization, both fields' geometry,
caller FP-state restoration, equal-value overwrites and partial writes. It
passes with 18,568 source vertices and 3,088 static-tail vertices. The native
retained-register calculation is additional CPU work, so those counts describe
the shortened static pass rather than a measured speedup.

Build the check outside the checkout with the authenticated private static
package; no interpreter oracle is compiled:

```powershell
cmake -S testing/vu1_replay -B $private/tail-check -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DPACKAGE_DIR=$private/vu1-static-package `
  -DTF_REPLAY_DUAL_TAIL=ON
cmake --build $private/tail-check --parallel 4
& $private/tail-check/tf_dual_tail.exe $private/qualified-captures.txt $private/all-captures.txt
```

The optional second list checks both setup programs' complete read footprints.
Every data word outside the declared inputs is poisoned before accelerated
execution; all 1,784 captured calls (863 manager, 921 context) produce the exact
full semantic state, without memory writes, packets or fallback. Changed code,
entry points and continuation calls are rejected by these exceptions.

A separate private test of the actual accelerated UNPACK kernels checks 106,496
format/mask/mode combinations with poisoned old XYZ and preserved W. All 24,155
admitted complete-XYZ writes match full output memory and ROW state. Protected
coordinates expose the poison in 77,507 declined cases. The deferred-memory
fixture check also verifies XYZ-only replacement with W retained.

Runtime output distinguishes `VU bypass`, `tail_vertices`, `skipped_vertices`,
and deferred-memory materializations from actual GS `gpu_draws`.
`[gpu-dual:reads]` splits CPU reconstruction into EE, input, unpack and ordinary
program reads, and counts admitted setup calls. Capture or
hash diagnostics can force deferred reads and are unsuitable for performance
measurements. These offline checks alone do not establish a live frame-rate
improvement or distinct rendered 60 fps.

The first mode-2 enhanced level route passed at 48.0763 physics updates/s,
median 20.006 ms and p95 29.398 ms, with cinematics skipped and 4x internal
rendering to 1920x1080 direct output. Across the whole session, 576,735 calls
bypassed bulk CPU transforms, skipping 12,215,474 source vertices; the renderer
submitted 1,188,563 native GPU draws. Deferred CPU readers still reconstructed
20,554,663 vertex/field outputs, about 84.1% of those skipped outputs. That run
predates the setup exception above and establishes no net performance gain.

With the setup/UNPACK exceptions and revised GPU buffer pool, the next enhanced
Amazon run passed at **55.7436 physics updates/s**, median 17.1415 ms and p95
23.330 ms. The 2,280-interval gameplay window took 40.901536 seconds with the
same 0.0166833512-second timestep. Direct presentation acquired 2,273 frames
at 55.5695 handoffs/s, with no skipped publications between the first and last
acquisition. All measured images were 2048x1788 internally, presented at
1920x1080; there were no dimension changes in that window.

Whole-session counters recorded 577,765 shortened VU calls, 12,237,294 skipped
source vertices and 1,190,787 GPU draws. CPU readers reconstructed 3,930,994
vertex/field outputs (**16.06%** of the skipped two-field outputs), comprising
3,528,598 ordinary-program reads and 402,396 input reads. EE and UNPACK reads
reconstructed none. The pool performed 920,285 page probes for 595,482
dispatches, including initial allocations; source/output capacity declines
remained zero. These observations cover one desktop run, with a final frame
inspection. They do not isolate each change's speedup, qualify the whole game,
or establish distinct rendered 60 fps.

## Ordered GPU dispatch groups

With `PS2X_TF_GPU_GROUPS=8`, the GS command consumer groups up to eight adjacent,
already-published native packets with the same callback. It never waits to fill
a group. Other command types, another callback and ring wrap stop the group.
The ring retains every payload until the complete callback returns. Ordinary
GIF operations, synchronization, readbacks and draw order remain unchanged.

`DualGeometry::prepareGroup` binds each packet's existing source buffer as a
root SRV. A single dispatch writes concatenated output vertices into a retained
page, and each packet draws from its own output offset. Only transform records
and source-count/output-offset metadata are uploaded; geometry is neither
copied between source buffers nor downloaded. The shader shares the existing
strict arithmetic. Released source buffers remain protected through compute
completion, and the output page through the last draw's fence. Unsupported
groups retain ordinary per-packet submission. Grouped work can include a packet
later culled or materialized by GS preparation, so dispatch counts alone are
not a speed measurement.

The frontend diagnostic also checks the actual command ring: 95 synthetic
packets verify order, size guards, callback boundaries, the eight-packet limit,
wrap and reuse across more than a full ring. For captured enhanced geometry,
214 grouped dispatches cover 626 independent source buffers, with both fields,
textures, depth, blending, mipmaps and deliberate CPU fallback. All four modes
pass with zero differing pixels across 14,680,064 full-resolution comparisons.
Invalid ranges, offsets and foreign-pool handles decline before dispatch.

```powershell
$env:PS2X_TF_GPU_GROUPS = '8'
python -B testing/test_gpu_dual_transform.py draw --frontend --prepared <private-grouped-fixtures> --dll <private-gpu-build>/tf_hwgs.dll --report <fresh-private-report.json>
```

The enhanced Amazon route passed with the grouped path, cinematics skipped,
16:9, 4x internal rendering and 1920x1080 direct output. Its 2,280 gameplay
intervals took 39.732710 seconds: **57.3835 physics updates/s**, median 16.884 ms
and p95 21.715 ms. The direct trace measured 57.1753 handoffs/s, one skipped
publication, stable 2048x1788 internal dimensions and an uncapped clean close.
The final frame was inspected.

Whole-session counters record 172,005 dispatches, including 154,163 groups
covering 615,276 packets, and 1,190,975 GPU draws. There were no group, bounds or
capacity declines; source uploads stayed at 1,687. Output-page allocations fell
to 330. The previous enhanced run recorded 595,482 dispatches and 806 pages;
these desktop runs are not a controlled estimate of the frame-time improvement.
Runtime selection, CPU-tail and deferred-memory counters match that preceding
run exactly. This remains below the target and does not establish distinct
rendered 60 fps or whole-game reliability.

## Grass GPU emission and retained state

`shaders/grass_emit.hlsl` computes eight grass sprites for both fields from a
256-byte emitter record: clip scale/offset, billboard offset, two base/edge
triples, a color base/edge triple and four scatter weights. Strip traversal,
subdivision and packet allocation remain accelerated CPU work. The shader
emits 640 bytes of raw XYZF2 endpoints and colors per packet. It shares the
terrain shader's RTZ/denormal/reciprocal helpers in `vu_math.hlsl`; separate
multiply/add operations preserve the PS2 order.

The RTX 3080 diagnostic matches **17,096 packets, 136,768 sprites and 547,072
endpoints**, including raw colors and clipping flags, with zero differing
words. The existing 776-batch dual-transform arithmetic check still matches
all 37,136 native vertices and raw GIF attributes after sharing the helpers.
`PS2X_HWGS_ADAPTER` optionally selects an adapter by name for these surfaceless
checks; it does not change the default adapter choice.

The accelerated grass loop computes three pairs after a first-pair prologue.
A qualified boundary experiment skips the two middle iterations (118 guest
cycles) and executes the last iteration and epilogue normally. It reconstructs
only the previous pair's second-field clipping flags, which survive in VF7.w.
Advancing the existing pipeline calendar preserves an earlier XGKICK; merely
incrementing a cycle counter would not suffice. The experiment checks the
whole resident grass program fingerprint, saturated arithmetic sticky flags,
loop counter, control state and cycle budget.

State-only replay first compares the shortened path with the complete
accelerated loop, including packets emitted during an overlapping transfer.
GPU replay then removes that complete-loop reference: the two skipped
iterations never execute, and actual GPU output fills their packet geometry
before XGKICK. Each emitter input must match the original GPU upload exactly.
The driver binds that output to its input and diagnostic report with SHA-256.

Both the 5,000-call broad capture and the 374-call moving-camera capture pass
with complete semantic VU state, memory, code, guest cycles and ordered GIF
bytes intact: **17,096 shortened loops, 34,192 skipped iterations, 14,448
overlapping transfers and 29,915,408 packet bytes**. No interpreter oracle or
original game build is used. This is offline qualification: GPU results are
downloaded for replay and are not yet a live grass rendering path or an FPS
improvement. The resident sprite work below addresses direct GPU consumption;
retained guest-memory ownership and live fallback still gate the in-game shortcut.

Build the diagnostic with the authenticated private package. The chain wrapper
is generated only in the private build directory; game-derived C++ is never
copied into this checkout. It instruments only the qualified ordinary grass
program, not another shader with a similar loop layout.

```powershell
cmake -S testing/vu1_replay -B $private/grass-check -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DPACKAGE_DIR=$private/vu1-static-package `
  -DTF_REPLAY_GRASS_TAIL=ON
cmake --build $private/grass-check --parallel 2
$env:PS2X_GRASS_CAPTURE = "$private/grass-capture"
& $private/grass-check/tf_grass_tail.exe $private/all-captures.txt $private/grass-state.csv
Remove-Item Env:PS2X_GRASS_CAPTURE
$env:PS2X_HWGS_ADAPTER = 'NVIDIA GeForce RTX 3080 Laptop GPU'
python -B testing/test_gpu_grass_emit.py --prefix $private/grass-capture `
  --dll $private/gpu-build/tf_hwgs.dll --report $private/grass-gpu.json --capture $private/grass-gpu.bin
python -B testing/replay_gpu_grass.py --prefix $private/grass-capture `
  --gpu-report $private/grass-gpu.json --gpu-capture $private/grass-gpu.bin `
  --fixtures $private/all-captures.txt --runner $private/grass-check/tf_grass_tail.exe `
  --report $private/grass-replay.json
```

Use fresh private report/capture paths. The last driver writes the detailed
CSV and log next to its JSON report, and fails if any GPU result remains unused
or any state, memory, control or packet comparison differs.

## Resident grass sprites

The grass emitter also produces two contiguous arrays of 16 native GS vertices.
Its 320-byte input adds the incoming STQ and RGBA latches for each field. The
shared fenced output pool appends 64 bytes of per-field GS culling state before
dispatch. Both the VU ADC flag and the GS upscaled primitive-culling test suppress
sprites as zero-area pairs. The latter matters at screen edges: relying on the
GPU scissor alone left subpixels which the ordinary GIF frontend rejects.

D3D12 sprite expansion reads this output directly as a vertex-shader SRV. The
scoped binding restores the ordinary stream afterward. Retained output pages,
command-list rollover, bounded allocation and completed-page reuse use the same
ownership rules as the existing triangle path. The device diagnostic alternates
GPU and ordinary sprites, changes source colors, retains two outputs, checks
budget rejection, and reuses a completed page. It passes 1,026 native draws with
zero differences across 1,048,576 compared pixels.

`DrawNativeSprites` supplies the real GS texture/depth/blend frontend with
consecutive vertex pairs, pre-rounded ST, unit Q and a nonconstant-depth witness.
Only supported state is admitted. Any actual vertex read or mutation selects
CPU fallback; optional coverage tests conservatively preserve the target without
fetching geometry. Sprite upscaling no longer requests CPU vertices when the
coordinate adjustment is disabled. Unsupported APIs, depth/state cases and cold
targets retain ordinary processing.

`projectGrassBounds` computes conservative position/color ranges from the small
emitter input using monotone binary32 interval arithmetic. It restores the
caller's floating-point environment and does not emit, map or download vertices.
Both captures qualify 16,435 of 17,096 packets (96.1%); all 525,920 checked
vertices and their flat colors fall inside their bounds. The other 661 packets
decline, for example when a conservative W range crosses zero. Native GPU
packing still matches all 547,072 captured records exactly, including ADC
suppression and the incoming attribute latches.

Two 256-packet samples from broad and moving-camera captures pass all four
frontend modes at 2048x1792. Textures, depth, blending, alpha testing, mipmaps and
deliberate CPU fallback match across **29,360,128 full-resolution pixels**, with
zero differences. The first three modes issue 2,970 resident GPU draws with no
CPU vertex materialization; the fourth deliberately materializes 990 draws.
The existing grouped-triangle frontend also passes unchanged: 1,216 GPU draws
and zero differences across 14,680,064 full-resolution pixels.

```powershell
python -B testing/test_gpu_grass_native.py prepare --prefix $private/grass-capture `
  --output $private/grass-native
python -B testing/test_gpu_grass_native.py run --prepared $private/grass-native `
  --dll $private/gpu-build/tf_hwgs.dll --report $private/grass-native.json
python -B testing/test_gpu_grass_native.py draw --prepared $private/grass-native `
  --dll $private/gpu-build/tf_hwgs.dll --report $private/grass-resident.json
python -B testing/test_gpu_grass_native.py draw --frontend --prepared $private/grass-native `
  --dll $private/gpu-build/tf_hwgs.dll --report $private/grass-frontend.json
```

These renderer diagnostics use enhanced gameplay captures with synthetic GS
texture state. They do not measure gameplay speed. The ordered runtime path
below adds source ownership and a depth witness from the retained CPU tail.

## Deferred grass packets and runtime hook

`PS2X_VU1_GPU_GRASS=ON` wraps the supported private grass chain in the build
directory. The package and its generated instruction bodies remain intact.
At runtime, `PS2X_TF_GPU_GRASS=1` additionally requires the supported executable,
`PS2X_TF_GPU_DUAL=2`, the accelerated static VU worker and D3D12 HWGS.
Both options remain opt-in.

The shared runtime hook authenticates all 3,872 resident program bytes, caching
identity by owner, code pointer, generation and load address. It checks the
loop/control state, finite inputs, saturated sticky flags and enough cycle
budget for the retained iteration, epilogue and kick. The two middle iterations
are skipped using the existing transfer/pipeline calendar. The final iteration
and epilogue preserve registers and the last sprite pair.

Each iteration finishes stores for the previous pair. Consequently, skipping
two iterations leaves part of the first pair unfinished too. Version-2 grass
companions own geometry for **sprites 0..5**, not just the middle four sprites.
Sprites 6 and 7 remain resolved. Each field owns XYZF2/RGBA/XYZF2 qwords, including
clipping/color W: 36 qwords per packet. A masked XYZ write must resolve W before
replacing XYZ. Full-qword writes discard the old owner.

Every queued transfer owns its emitter constants and scatter records. Pending
transfers can finish while the following emitter advances, so a FIFO carries
the companions in order. Guest-memory owners are separate from queued packet
owners. Reusing a VU output buffer discards its old deferred words without
reconstructing them. For now, remaining output buffers are restored at every
VU-call boundary before another program or EE/VIF code can observe them.

The runtime packet path reconstructs initial STQ and RGBA from actual GS state
and the retained first-field tail. Source bounds and the two surviving tail
sprites provide draw eligibility without calculating skipped vertices. The
compute shader applies ADC and GS scissor/empty-box rejection, and D3D12 reads
the resulting vertex buffer directly. The ordinary PRE/STQ prefix establishes
state; an ADC-suppressed replay of the last pair restores latches and XY history.
Incomplete sprite primitives, unsupported state, flat/insufficient depth
witnesses, native-resolution culling and unbounded inputs retain ordinary GIF
processing. Both fallback paths use the shared CPU materializer, never a GPU
readback. Mixed packet groups preserve order and currently dispatch grass
packets individually; existing triangle groups remain available.

The accelerated replay now uses the **same runtime hook and memory tracker**.
It deliberately poisons all 36 unresolved qwords after each epilogue and checks
ordered fallback against authenticated actual GPU output. Both captures pass:
5,374 calls, 17,096 shortened emitters, 14,448 overlapping transfers and complete
semantic state, data, code, cycles and packet equality. Of 615,456 owned qwords,
600,300 are overwritten and 15,156 (2.46%) are restored in VU memory. Independent
partial-write/full-write checks also pass all 17,096 packets.

The ordered GS-thread packet diagnostic compares both fields in packet order,
including source-based ordinary fallback and deliberate native-draw fallback.
Two 256-packet samples cover four texture/depth/blend/mipmap modes. They produce
2,088 resident GPU draws, 696 deliberate native vertex materializations, 2,048
exact final GS vertex-latch checks, and zero differences across 29,360,128 pixels
at 2048x1792. Bounds/depth declines use ordinary reconstructed packets. Native
GPU modes make no unexpected vertex fetches. The existing grouped-triangle
frontend also passes with 1,216 GPU draws and zero differing pixels.

```powershell
# Add to the enhanced static-VU build configuration:
# -DPS2X_VU1_GPU_GRASS=ON
# Opt in only for the enhanced accelerated D3D12 route:
$env:PS2X_TF_GPU_GRASS = '1'

python -B testing/replay_gpu_grass.py --defer-memory --prefix $private/grass-capture `
  --gpu-report $private/grass-gpu.json --gpu-capture $private/grass-gpu.bin `
  --fixtures $private/all-captures.txt --runner $private/grass-check/tf_grass_tail.exe `
  --report $private/grass-deferred.json
python -B testing/test_gpu_grass_packets.py --prepared $private/grass-native `
  --prefix $private/grass-capture --dll $private/gpu-build/tf_hwgs.dll `
  --report $private/grass-packets.json
```

These checks qualify the captured windows and GS integration, not full-campaign
reliability or distinct rendered 60 fps. The live results below show why grass
offloading remains experimental.

## Grouped grass dispatches and live results

With `PS2X_TF_GPU_GROUPS=8`, homogeneous groups of two to eight adjacent grass
packets share one compute dispatch. Each packet keeps its own output offsets,
bounds and original ordered GS draws. Incoming color starts at the actual GS
latch and then follows each preceding packet's retained, byte-truncated color.
Mixed groups and unsupported packets retain individual processing. Rejected
groups consume neither GS state nor GPU output. Reconstructed GIF buffers have
explicit 16-byte alignment, including when the enclosing draw structure changes.

The two enhanced-capture comparisons pass 2,048 final latch checks and all
29,360,128 pixels at 2048x1792. They issue 2,088 GPU draws, including 1,716 from
grouped compute, with 386 grass dispatches for 1,786 emitter batches. Deliberate
CPU fallback and invalid group/framing checks pass. The triangle regression
also passes 1,216 GPU draws and 14,680,064 pixels with zero differences.

The saved Amazon route ran with accelerated static VU, native60 mode 3, 16:9,
2048x1788 internal output, 1920x1080 presentation and cinematics skipped:

| Enhanced configuration | Physics updates/s | Direct handoffs/s | Median / p95 interval |
|---|---:|---:|---:|
| Individual grass compute dispatches | 24.8854 | 24.8810 | 31.747 / 79.084 ms |
| Grouped grass compute dispatches | 38.9721 | 38.9582 | 24.586 / 48.993 ms |

Individual dispatches exhausted the output pool, causing 4,218,029 grass
native-draw materializations and about 4.40 billion page probes. Grouping removes
those capacity declines and reduces probes to 1,087,936. It executes 6,434,941
grass GPU draws, including 6,298,046 using grouped output. Ordinary packet
fallback still handles 2,321,549 field/depth declines and 189,361 bounds declines;
zero native-draw materializations does not mean zero CPU geometry work.

Both runs preserve 4,567,606 ordered emitter companions, skip 9,135,212 VU loop
iterations and finish with no unresolved grass memory. These desktop measurements
remain slower than the saved mesh-only enhanced result of 57.3835 updates/s;
that earlier build was not rerun. Grass remains opt-in. A fresh profile identifies
hardware-renderer/driver submission work as the next bottleneck; compute grouping
does not merge the individual GS draws. See [the profile](../../docs/findings/vu-performance.md#profile-after-grouped-grass).

To exercise grouped packets, set `PS2X_TF_GPU_GROUPS=8` before the existing
`test_gpu_grass_packets.py` command. Reports distinguish actual grouped GPU
draws from batches merely prepared by a compute dispatch.

## Merged grass draws and wider batches

`PS2X_TF_GPU_GRASS_MERGE=1` enables a guarded field-major output layout. Instead
of drawing each emitter's two fields separately, it concatenates the GPU
vertices and submits one sprite draw per field. Primitive order within each
field is unchanged. Admission requires matching primitive/context, scissor and
offset state, disjoint field scissors, nonaliasing color/depth targets, and no
texture, mipmap or palette feedback from either target. Unsupported state is
rejected before dispatch or GS state consumption. Ordinary and native CPU
fallbacks preserve incoming attributes and final latches without GPU readback.

The VRAM check covers page-aligned ranges, texture base misalignment and wrap.
An independent GS swizzle-page enumeration checks 36,267 admitted layouts,
including indexed and depth formats. This caught an unaligned texture spilling
into an extra page; the guard now includes that page. Negative checks cover
overlapping fields, changed state, invalid framing, and color/depth/CLUT/mipmap
feedback.

`PS2X_TF_GPU_GRASS_GROUPS=64` additionally permits up to 64 already-published
adjacent grass packets. It never waits to fill a batch. Callback and batch-limit
changes, ordinary GIF, sync/readback and ring-wrap commands end a group. Other
callers retain the eight-packet limit. A rejected wide batch splits in order
until smaller admissible groups or the established fallback are reached.
Queue diagnostics verify ownership, ordering, wrap and these boundaries across
181 packets. Scratch initialization scales with the attempted group size.

`PS2X_TF_GPU_GRASS_BOUNDS_CACHE=1` adds an 8,192-entry fixed cache of source-based
bounds, including rejection results. CRC selects a slot; comparison of all
288 relevant input bytes is required for a hit. Incoming RGBA and raster state
are not inputs to the projection calculation. A cache-only experiment hit
2,665,014 of 5,259,619 queries but did not demonstrate a frame-rate improvement;
the wider-batch experiment also uses it to avoid repeated projection when
splitting a rejected group.

All following runs use the same accelerated executable, saved Amazon route,
native60 mode 3, 16:9, 2048x1788 internal output and 1920x1080 direct presentation.
Cinematics are skipped and navigation is excluded from timing.

| Enhanced grass configuration | Physics updates/s | Direct handoffs/s | Median / p95 interval |
|---|---:|---:|---:|
| Compute grouping, separate draws (saved result) | 38.9721 | 38.9582 | 24.586 / 48.993 ms |
| Merged draws, up to 8 packets | 48.7675 | 48.6520 | 19.315 / 34.620 ms |
| Up to 8, with bounds cache | 48.6903 | 48.5454 | 18.740 / 38.769 ms |
| Up to 64, with bounds cache | 57.6059 | 56.7831 | 16.804 / 21.670 ms |

Enabling the existing EE scheduler fast path on the wide build gives 56.8230
updates/s and 56.5073 handoffs/s (17.104 / 21.793 ms), with no demonstrated
improvement. That option remains disabled for the selected configuration.

The wide run executes 175,465 grass dispatches and 334,894 grass GPU draws,
versus 596,121 and 6,434,941 in the saved compute-only grouping run. Its largest
accepted group is 64 packets; 4,124,902 packets use merged draws. There are no
output-capacity declines or native-draw materializations. Ordinary fallback
still handles 410,316 field declines and 190,089 bounds declines. Runtime
ownership finishes with no dirty deferred grass memory. Thirty-three published
frames are skipped by host acquisition, so simulation rate must not be reported
as presentation rate. This is roughly level with the saved mesh-only enhanced
result, and does not establish sustained true 60 fps. Grass remains opt-in.

The wide enhanced-capture frontend checks perform 504 GPU draws, including
132 merged draws, with 536 group/individual boundary latch comparisons. All
29,360,128 pixels at 2048x1792 match, including texture, depth, blending, mipmaps
and deliberate CPU fallback. The triangle/queue regression also passes.
Separately, both the eight-packet and final wide live camera-turn captures match
the saved enhanced output exactly: 32 complete images, fields 3027..3058,
117,178,368 RGBA pixels per comparison. No older executable is rerun. Image
capture is disabled for all performance measurements.

```powershell
$env:PS2X_TF_GPU_GROUPS = '8'
$env:PS2X_TF_GPU_GRASS_MERGE = '1'
$env:PS2X_TF_GPU_GRASS_GROUPS = '64'
$env:PS2X_TF_GPU_GRASS_BOUNDS_CACHE = '1'
python -B testing/test_gpu_grass_packets.py --merge `
  --prepared $private/grass-native --prefix $private/grass-capture `
  --dll $private/gpu-build/tf_hwgs.dll --report $private/grass-merged-result.json
```

Live use also requires the existing `PS2X_TF_GPU_DUAL=2`, `PS2X_TF_GPU_GRASS=1`,
accelerated static VU and D3D12 hardware-renderer prerequisites. The diagnostic
reports actual merged GPU draws, largest accepted group, cache hits, guard
checks and full-resolution pixel differences.

## Merged mesh packets

`PS2X_TF_GPU_DUAL_MERGE=1`, together with `PS2X_TF_GPU_GROUPS=8`, merges
compatible adjacent dual-transform packets into one triangle draw per field.
The existing resident source buffers feed one compute dispatch; its interleaved
GPU output is consumed directly through a combined index list. Indices retain
each strip's primitive order and restart masks without joining separate strips.
No transformed geometry is read back from the GPU.

Admission requires matching primitive/context and per-field scissor/offset,
disjoint field scissors, valid source bounds and a nonconstant-depth witness.
The shared grass/mesh VRAM guard rejects target overlap, wrap, and texture,
mipmap or palette feedback. Rejected groups consume no GS state. Accepted
groups retain source-based CPU fallback and restore final GS latches from the
last packet's resolved tail.

Textured IIP triangles qualify; blended groups additionally require one
identical constant vertex alpha across every packet and field. Combining alpha
ranges changed the backend's blending choices and produced small pixel
differences in the first diagnostic. Mixed or varying alpha now retains the
existing individual draw path. This restriction is part of the contract, not
an approximate image tolerance. Rejecting all blend-enabled draws was too
restrictive: every recorded dual-transform packet enables the GS blend flag.

The final captured-mesh check covers 740 packets in groups of 2, 3, 4 and 8.
Five render modes exercise opaque textures, mipmaps, deliberate native CPU
fallback, blended/alpha-tested and blended/mipmapped groups. Each GPU mode must
actually execute merged GPU draws. It performs 796 merged GPU draws and 875
boundary latch comparisons. All 18,350,080 full-resolution pixels match, with
nine rejection checks and 36,267
GS-address-range checks. The grass and existing triangle frontend checks also
retain zero differences. These diagnostics are separate from gameplay timing.

The accelerated Amazon route performs 878,731 mesh GPU draws and 1,018 native
CPU materializations, versus 1,188,551 and 6,297 in the saved wide-grass run.
It measures 57.4907 physics updates/s and 56.4953 direct handoffs/s; this does
not establish an FPS improvement. The feature remains opt-in. A separate
32-image camera-turn capture matches the saved enhanced output exactly across
117,178,368 pixels at 2048x1788. No older game executable is rerun.

```powershell
$env:PS2X_TF_GPU_GROUPS = '8'
python -B testing/test_gpu_dual_transform.py draw --merge `
  --prepared $private/dual-grouped --dll $private/gpu-build/tf_hwgs.dll `
  --report $private/dual-merged-result.json
```

## Bounded output-image cadence capture

`PS2X_TF_SCENE_CAPTURE` accepts a fresh absolute private output prefix;
`PS2X_TF_SCENE_RANGE=first:last` selects at most 64 consecutive presentation
fields. The diagnostic copies complete RGBA8 output textures into one bounded
readback buffer and writes `.rgba` pixels and `.json` metadata after GPU work
finishes. Mapping and file output happen at shutdown. Normal direct presentation
does not enable this readback, and diagnostic timings are not FPS measurements.

```powershell
python -B testing/test_scene_capture.py --dll $private/gpu-build/tf_hwgs.dll `
  --prefix $private/capture-probe --report $private/capture-probe-result.json
python -B testing/analyze_scene_capture.py --prefix $private/scene `
  --report $private/scene-analysis.json
```

The synthetic probe checks all 464,400 bytes, including padded GPU rows, an
intentional duplicated image and separate alternating-row updates. The enhanced
camera-turn capture contains 32 distinct 2048x1788 images at fields 3027..3058,
inside the scripted turn at 2997..3177. There are no adjacent or two-field full
duplicates; both output-row parities change on every pair. The minimum changed
central-crop area is 87.47%. This establishes different complete output textures
in that window, not 60 images per second or full-campaign timing correctness.
