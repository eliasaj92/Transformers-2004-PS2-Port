# VU performance after direct presentation

This investigation uses the USA disc, the private Trainer hub seed, 4x hardware rendering, 16:9, direct D3D12/OpenGL presentation and experimental native-timing variant 3. Every game run is visible, uses the static VU package, and skips opening and arrival cinematics. Normal play still uses original simulation timing.

## Measured bottleneck

The direct-presentation gameplay runs saturated the VU worker at approximately 98-100% of one CPU core. Perfscope attributed about 90% of its time to VU execution and 10% to VIF work. The game thread waited for this worker; the hardware bridge no longer performed full-frame CPU readbacks. The workload made approximately 2,600-3,000 VU calls per guest field.

A separate 20-second instruction-pointer sampling run collected 2,670 samples on each of the VU and game threads. Threads were resumed immediately after reading their instruction pointer, before symbol lookup. Sampling adds overhead, so this run is diagnostic evidence and not a speed benchmark. The matching executable/PDB and raw samples remain private.

| Compiled region | Disc registration name | VU samples |
|---|---|---|
| sr729 | vlShaderAssembly_Grass | 8.4% |
| sr752 | taShaderMicrocode_FirstPassReflection | 8.1% |
| sr686 | vlShaderAssembly_DualTransform | 6.9% |
| sr799 | taShaderMicrocode_FastDualTextureTransform | 4.3% |
| sr728 | vlShaderAssembly_Grass | 3.9% |

Other sampled work included region validation, graphics-packet transfer, pipeline commits and VIF unpacking. The evidence favors optimizing the compiled loops rather than assuming scheduler reset or missing compilation coverage is the dominant cost. Individual instruction samples do not distinguish instruction latency from the work preceding the sampled address.

## AVX2 build option

`PS2X_VU1_STATIC_AVX2=ON` compiles the external static VU package for AVX2. It is off by default and requires a compatible x86-64 CPU. Configure the existing private build with this option, then rebuild and stage the executable normally. It does not change generated source, VU scheduling, guest timing or the renderer.

The MSVC path requires VS 2022 or newer and retains `/fp:precise`. Microsoft documents that [VS 2022 disables floating-point contraction under this mode](https://learn.microsoft.com/en-us/cpp/build/reference/fp-specify-floating-point-behavior?view=msvc-170). Do not add `/fp:fast` or `/fp:contract`: a fused multiply-add would change intermediate rounding. The non-MSVC flags also explicitly disable contraction; only the Windows MSVC build is being qualified here.

On MSVC 19.44, disassembly of sr686 decreased from 3,665 to 3,222 machine instructions, approximately 12%. Neither version contained fused multiply-add instructions. This is a compiler-output observation, not a gameplay speed claim.

The correctness gate compares two accelerated builds with `PS2X_VU1AOT_HASH=1`, `PS2X_VU1_THREAD=0`, `PS2X_KERNEL_HEAP=0`, `PS2X_GLYPHS=off`, `PS2X_VU1NEW=0`, and perfscope/hardware profiling disabled. The ordinary threaded configuration is tested separately. Raw hashes, player probes, timings and captures are retained outside the checkout.

The SSE and AVX2 runs matched all 59 cumulative state hashes through call 3,866,624, every player position/mode, and all six terminal route counters. Compare hashes by VU call count: this logger's `vs` label comes from the asynchronous presenter clock, and nine labels differed while the corresponding hash values were identical. The full lines and label differences were retained. The tested candidate also includes the direct presenter's restart-only backend-selection cache and shutdown correction.

Three alternating threaded pairs used the same private timing route, with checkpoint captures disabled, 721 consecutive physics calls at guest fields 3020 through 3740, and a 0.0166833512-second timestep. Every run matched the six terminal counters and all player probes. These are desktop-run observations with browser activity, not isolated-machine benchmarks.

| Build | Gameplay updates/s, three runs | Script wall seconds, three runs |
|---|---|---|
| SSE static VU, direct presentation | 40.72, 38.38, 38.43 | 77.2, 79.5, 78.5 |
| AVX2 static VU, direct presentation | 43.50, 39.41, 40.86 | 74.7, 77.5, 76.6 |

The arithmetic mean increased from 39.18 to 41.26 updates/s (5.3%); paired gains ranged from 2.7% to 6.8%. This does not establish 60 fps, visual correctness for every program, or whole-campaign coverage.

Build 24 adds the default-off native-seam observation probe. With it enabled, both the deterministic hash run and the ordinary threaded run retained their reference results. With it disabled and original timing selected, the direct-presentation run matched the earlier original-speed reference: 4,793 fields, 1,796 pad reads, 23,559,028,117 cycles, 1,129 CD operations, 2,122 I5 events, 700,796 MSCALs and all eight player probes. Physics remained at 0.0333667025 seconds. The private launcher now selects this AVX2 build with direct presentation and original timing; saves and settings remain in their existing locations. The prior SSE build remains available.

## GPU geometry boundary

These VU routines perform geometry math and generate GIF command packets. `progressXgkick` observes VU memory in command order, and the current GS front end parses those packets on the CPU. A compute dispatch per VU call followed by a packet readback would introduce a new synchronization boundary at thousands of calls per field.

The useful GPU route is to identify complete mesh batches and their constants, verify native transforms against the existing output, then carry vertices directly into the hardware renderer. [BT3-Recomp documents a comparable native-renderer approach](https://github.com/z3xox/BT3-Recomp/blob/main/docs/NATIVE-RENDER-SEAM.md), including shared VU state and ordered multipass rendering. Its game-specific kernels cannot be substituted for Transformers' programs. This is an architectural direction, not implemented GPU VU execution in this port.

The concrete Transformers investigation and observation probe are documented in [Native render seam](native-render-seam.md).

That investigation now includes a handwritten grass geometry and packet model. Offline comparisons match 442 captured calls and 17,096 sprite groups exactly, including complete GS tags/state and transport metadata. Opt-in live comparison also passed the accelerated route in deterministic and threaded modes without changing game-state checks. It still executes both the native calculation and the reference VU code; the measured diagnostic timings are not a performance improvement. Persistent VU effects and a native hardware draw interface remain prerequisites for replacing the grass path.

The standalone accelerated replay now reproduces 5,000 consecutive captured calls exactly, including registers, memory, cycles and GIF output. In one replay's host-thread cycle accounting, grass continuations account for about 7.1% of measured VU execution. This small captured window is not a gameplay benchmark and differs from the sampled scene above. It reinforces that completing only grass cannot be assumed to deliver the needed whole-frame speedup; dual-transform/reflection batches remain important coverage targets.

A private SIMD experiment also matched a simplified saturating float-to-integer conversion against the current helper on 40,388,608 normalized float inputs, all four VU scale factors, and nearest/toward-zero rounding. It was subsequently built into an enhanced experimental executable; the level measurements below do not establish a performance benefit. It has not been promoted into the main toolchain.

## Focused level performance work

Current testing uses enhanced accelerated builds only, including for bug investigation. Do not launch original builds or repeat menu/equipment checks. `testing/enhancements/level_performance.tfs` loads the private save, enters Amazon directly, skips opening and arrival cinematics, then measures movement, a camera turn and sustained level rendering. Navigation is excluded from `perf_begin` through `perf_end`. Captures, memory cards and generated binaries remain private.

Use static VU, direct D3D12 presentation, 4x internal rendering, 1920x1080 output, 16:9, native-timing variant 3 and the movement-blend correction. Disable diagnostic hashing, grass comparison, perfscope and hardware profiling while measuring speed. Pass `-ProcessPriority Normal` to `testing/run_test.ps1`; the generic runner otherwise uses `BelowNormal`. The chosen priority is recorded in the private environment log. Profile collectors also use visible windows.

`python -B testing/summarize_level_performance.py <private-run-directory>` checks the enhanced configuration and successful route, rejects missing/duplicate field samples, and reports host-time intervals for the in-level physics updates. These are simulation updates, not proof of distinct rendered images or sustained 60 fps. The route needs 16.7 ms per update to reach that target.

The first level-only experiments each covered 2,281 physics samples over fields 2697 through 4977, with a 0.0166833512-second timestep:

| Enhanced experiment | Process priority | Updates/s | Median / p95 interval |
|---|---|---:|---:|
| Four-lane float-to-integer conversion | BelowNormal | 45.73 | 21.29 / 27.19 ms |
| Reuse equal affine transforms | BelowNormal | 45.44 | 21.37 / 27.31 ms |
| Equal transforms plus existing fast EE checkpoints | BelowNormal | 41.72 | 23.15 / 31.73 ms |
| Combined FMAC arithmetic checks in grass/reflection | BelowNormal | 42.41 | 22.82 / 30.06 ms |
| Combined FMAC arithmetic checks in grass/reflection | Normal | 41.22 | 23.27 / 31.81 ms |
| Full static-VU profile-guided optimization | Normal | 47.18 | 20.10 / 30.20 ms |

These are unpaired desktop measurements, affected by other activity and power/thermal conditions. None demonstrates a speed improvement, so the arithmetic candidates and fast checkpoint setting remain experimental. The combined-check candidate matched 5,374 recorded accelerated calls exactly (registers, data, code, cycles and ordered packets), with no interpreter fallback or pending transfer. Its synthetic boundary check also passed 50,000 vector inputs across 72 operation, lane-mask and normalization cases. Correctness alone did not make it faster.

A separate profile of the enhanced matrix build attributed 89.4% of VU worker time to VU execution and 8.5% to VIF work; the VU thread consumed approximately 97% of one CPU core. Grass and reflection remained the largest sampled regions. The GS submission thread mostly waited, which does not measure GPU utilization. That profile's final wait exceeded its script timeout; the valid 20-second sample preceded that test-script failure, and the extended wait now has an explicit longer timeout. No original build was launched to investigate it.

The final CPU experiment compiled all 580 verified static-VU translation units
with MSVC `/GL`, trained `/FASTGENPROFILE:COUNTER64` on the enhanced level route,
merged the completed profile, then linked `/LTCG /USEPROFILE`. Other runtime
objects and precise AVX2 arithmetic remained unchanged. The instrumented run
passed but supplies no speed measurement. The optimized run also passed,
including one physics sample at every field 2697..4977: 48.323659 seconds for
2,280 intervals, or 47.181858 updates/s. Compiler/linker jobs had finished before
the run. This single desktop result is not a paired speedup claim, a distinct
rendered-frame count, or 60 fps. The private prototype has not replaced the
normal launcher. Subsequent performance work focuses on GPU geometry batches.

The next GPU step replaced 776 selected dual-transform VU calls in a 5,000-call
offline enhanced replay. Real GPU output plus a small CPU preservation model
retained all 11,998,416 packet bytes and checked later memory/control effects.
This establishes a captured-window bypass, not a game FPS result: GPU dispatch
was performed separately, unresolved registers remain, and the live renderer
does not yet submit these batches. Details and limitations are recorded in
[the native-render investigation](native-render-seam.md#dual-transform-state-preservation-and-bypass).

## Profile after live GPU bypass

After the guarded dual-transform tail, setup/UNPACK read-footprint improvements
and GPU buffer-pool changes, a fresh enhanced level run passed at 55.7436 physics
updates/s. A separate extended level route then passed with a 20-second
instruction-pointer sample. It collected 10,110 samples per target thread;
this instrumented run is not a frame-rate measurement.

| VU region | Role | Samples |
|---|---|---:|
| sr729 | Grass | 12.36% |
| sr728 | Grass | 2.34% |
| sr752 | First-pass reflection | 3.78% |
| sr799 | Fast dual-texture transform | 2.84% |
| commitReadyPipelines | Retained VU execution | 3.40% |
| progressXgkick | Graphics-packet progression | 2.98% |

The game thread also sampled guest dispatch, checkpoints, memory copies and
IOP work. The runtime GS submission thread spent 92.10% of its samples waiting;
that is a separate thread from the hardware renderer and does not establish
GPU utilization. These observations identify remaining CPU work after the
dual-transform offload, rather than qualifying removal of another VU program.

The following grouped GPU-dispatch build passed the same uninstrumented level
route at 57.3835 physics updates/s and 57.1753 direct-frame handoffs/s. It issued
172,005 compute dispatches over the whole session. See the
[GPU group contract](../../deps/hwgs/GPU_GEOMETRY.md#ordered-gpu-dispatch-groups)
for correctness checks and limitations. Grass remains the next geometry lead;
distinct rendered 60 fps is not yet established.

The following grass investigation now has an exact GPU emitter and a qualified
shortened CPU loop in accelerated offline replay. It removes two of sr729's
three iterations while retaining the last iteration, the prior pair's live
clipping flags and pending graphics transfers. Across the broad and moving
captures, 17,096 loops skip 34,192 iterations and consume actual GPU geometry
with complete state/memory/packet equality. The shader checks 547,072 endpoints
plus colors and clipping flags exactly. These results do not update the live
frame-rate measurement above. They precede the resident sprite work below;
guest-memory ownership still gates the live shortcut. See the
[grass emission contract](../../deps/hwgs/GPU_GEOMETRY.md#grass-gpu-emission-and-retained-state).

The following renderer work supplies direct GPU sprite consumption, source-based
bounds and GS-compatible edge culling. All native packing checks remain exact;
the two textured frontend samples produce zero differing pixels and perform no
CPU vertex materialization in their GPU modes. This qualifies the renderer path;
live VU/packet ownership remains unfinished. The shortened grass loop is still offline and no
new game frame-rate measurement has been taken. The current next step is joining
that loop to ordered grass companions and deferred VU memory, then measuring the
enhanced Amazon route. See [resident grass sprites](../../deps/hwgs/GPU_GEOMETRY.md#resident-grass-sprites).


## Ordered grass runtime integration

The opt-in runtime now joins the shortened grass loop to owned GPU packets and
an exact CPU fallback. Accelerated replay uses the same runtime hook: both
captures preserve state, memory and packet order with the skipped geometry
poisoned. Only 2.46% of deferred VU qwords need restoration; the rest are
superseded by later output. The first pair must also be deferred because its
stores finish during the next loop iteration.

The GS packet path draws both fields in order, obtains its initial attributes
from real GS state, and restores final latches using the retained last pair.
GPU/ordinary fallback comparisons pass 2,048 latch checks and 29,360,128 pixels
at 2048x1792 with zero differences. It uses source-derived bounds and a depth
witness from resolved tail sprites, without rebuilding the omitted geometry
just to qualify native drawing. See the [runtime contract and commands](../../deps/hwgs/GPU_GEOMETRY.md#deferred-grass-packets-and-runtime-hook).

The live individual-dispatch implementation measures 24.8854 physics updates/s;
grouping grass compute improves this to 38.9721, while eliminating output-pool
capacity fallback. Both remain below the saved mesh-only enhanced measurement.
See the [full live counters](../../deps/hwgs/GPU_GEOMETRY.md#grouped-grass-dispatches-and-live-results).

## Profile after grouped grass

A separate enhanced Amazon run sampled the VU, EE, submission and busiest worker
threads for 20 seconds, collecting 10,168 instruction-pointer samples per thread.
The hardware renderer was included explicitly; sampling only the runtime GS
submission thread would mostly reveal its wait. The renderer's linker map was
matched to the loaded DLL's exact code bytes and addresses. This instrumented
run passed and is not a frame-rate measurement.

The VU thread spends 29.25% of samples waiting, with 3.47% in the retained grass
region `sr729`. EE waiting accounts for 31.54%; the runtime GS submission thread
waits for 91.82%. On the active hardware-renderer thread, `GSRendererHW::Draw`
accounts for 8.50%, grass bounds and its principal helper for 6.69%, packed grass
GIF handling for 3.31%, and texture-cache invalidation plus its helper for 5.70%.
Driver and D3D12 functions are also prominent. The next optimization should
reduce grass draw preparation/submission work while preserving state and order.
Adding further VU shortcuts alone does not address this measured bottleneck.

## Profile after eight-packet merged draws

Merging the actual sprite draws raises the measured level rate to 48.7675
physics updates/s. A separate 20-second instrumented run collects 10,094
samples per selected thread, with renderer symbols again matched to the loaded
DLL's exact code bytes. It is not used for frame-rate claims.

VU waiting drops to 14.42% and EE waiting to 11.37%. The active renderer spends
6.95% in `GSRendererHW::Draw`, 9.69% in grass bounds and its principal helper,
3.24% in packed grass GIF handling, and 1.86% in grass framing checks. Single
object waits account for 11.53% of that thread. The runtime GS submission
thread still mostly waits (90.41%).

A bounded bounds cache hits 50.67% of queries in the following level experiment,
but measures 48.6903 updates/s, so no speed gain is established. Wider grass
batches subsequently reduce dispatch and draw counts enough to reach 57.6059
updates/s and 56.7831 direct handoffs/s. These results remain below sustained
true 60 fps. Full timings and correctness evidence are recorded in the
[merged grass contract](../../deps/hwgs/GPU_GEOMETRY.md#merged-grass-draws-and-wider-batches).

## Profile after wide grass batches

The final 64-packet configuration passes another instrumented Amazon run,
collecting 10,083 samples per selected thread over 20 seconds. Its map again
matches the loaded renderer's exact code section. Hardware-renderer waiting is
now 18.83%, while VU waiting is 6.10%; the runtime GS submission thread waits
94.98%. The renderer samples include `Draw` at 7.54%, bounds plus its principal
helper at 6.09%, and the cache lookup at 1.83%.

The busiest EE symbols include `sub_0025DE90` (7.34%), full scheduler checkpoints
(6.05%) and `memmove` (5.92%). The first is the already-identified game frame-wait
routine, not a newly discovered geometry calculation. VU samples include
`sr729` at 4.71%, `sr752` at 4.15%, pending-pipeline commits at 3.35%, `sr799` at
3.25% and `aotFast` at 3.14%. These are the next timing/remaining-VU candidates;
the profile does not prove that moving another individual operation to the GPU
will improve frame rate. Enabling the existing scheduler fast path separately
measures 56.8230 updates/s and provides no demonstrated improvement.
