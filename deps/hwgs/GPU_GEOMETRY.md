# Experimental GPU geometry

This opt-in component draws GPU-produced vertices through the existing hardware
renderer. Mode 1 retains full accelerated VU execution. Mode 2 replaces the bulk
of admitted dual-transform calls with a short CPU tail and deferred VU memory;
see [its qualification and limits](#guarded-cpu-tail-and-deferred-vu-memory).

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
