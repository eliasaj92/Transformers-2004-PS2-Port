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

These are unpaired desktop measurements, affected by other activity and power/thermal conditions. None demonstrates a speed improvement, so the arithmetic candidates and fast checkpoint setting remain experimental. The combined-check candidate matched 5,374 recorded accelerated calls exactly (registers, data, code, cycles and ordered packets), with no interpreter fallback or pending transfer. Its synthetic boundary check also passed 50,000 vector inputs across 72 operation, lane-mask and normalization cases. Correctness alone did not make it faster.

A separate profile of the enhanced matrix build attributed 89.4% of VU worker time to VU execution and 8.5% to VIF work; the VU thread consumed approximately 97% of one CPU core. Grass and reflection remained the largest sampled regions. The GS submission thread mostly waited, which does not measure GPU utilization. That profile's final wait exceeded its script timeout; the valid 20-second sample preceded that test-script failure, and the extended wait now has an explicit longer timeout. No original build was launched to investigate it.
