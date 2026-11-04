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

A private SIMD experiment also matched a simplified saturating float-to-integer conversion against the current helper on 40,388,608 normalized float inputs, all four VU scale factors, and nearest/toward-zero rounding. That arithmetic candidate is not integrated into the game build and carries no gameplay performance claim.
