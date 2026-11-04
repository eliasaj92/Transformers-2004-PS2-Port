# Transformers native render seam investigation

Status: live GPU dual-transform drawing and an experimental guarded CPU-tail bypass, November 2026. The bypass has captured-state, geometry and deferred-memory checks; whole-game reliability and distinct rendered 60 fps remain unverified. Direct GPU presentation and hardware resolution scaling are separate, already implemented features. Earlier sections retain the evidence and limitations of each preceding stage.

## Why this boundary

[BT3-Recomp's native render seam](https://github.com/z3xox/BT3-Recomp/blob/main/docs/NATIVE-RENDER-SEAM.md) captures an engine mesh batch, imports its prebuilt VIF list, verifies a host transform against VU output, then moves that transform into a vertex shader. Its [Phase 0 measurements](https://github.com/z3xox/BT3-Recomp/blob/main/docs/SEAM-PHASE0-BATCHES.md) attribute constant uploads and VU launches to batch builders. This is the useful architectural reference; BT3's addresses, mesh layouts and rendering effects are game-specific.

The Transformers [VU profile](vu-performance.md) points to grass, dual transforms and reflections. Thousands of VU calls per field make a GPU dispatch followed by CPU GIF readback an unattractive boundary. A whole-batch replacement can instead retain vertex buffers on the GPU and reduce CPU VIF/VU work. It must preserve the state read by uncovered batches, as well as draw order and guest VRAM readbacks.

The older [native renderer comparison document](../NATIVE_RENDERER.md) describes historical experiments and picture defects. It does not supply a qualified, integrated GPU mesh renderer in this checkout.

## Verified static map: USA ELF CRC 74c982c0

| Address | Evidence / role |
|---|---|
| `0x268bf0` | Constructs the grass shader context; writes vtable `0x6b4820` and constant count 56 at object+8. |
| `0x6b4820` | Context vtable. Method at +0x0c is `0x2688f0`; its type-information chain names `vlShader_Grass`. |
| `0x2688f0` | Builds the 56-qword grass constant upload. Uses object+8 for count and object+0x0c for destination. Observe after the full guest return. |
| `0x221cc0` | Leaf packet allocator/emitter. Uses scratchpad cursor `0x70000d30`, emits a DMA CNT tag, STCYCL 4/4 and V4-32 UNPACK, then returns payload pointer. This is not itself the mesh-batch root. |
| `0x23aa90` | Context dispatcher: walks the shader-context list at scratchpad `0x700005f0`, checks dirty flags and invokes virtual method +0x0c. The observed grass return addresses are `0x23aaf4` and `0x23ab24`. |
| `0x1b0d50` | Four-vector matrix multiply used to populate the first transform. |
| `0x261c40` | Registration callsite for `vlShaderAssembly_Grass`, disc program 38, 3,872 bytes. A registration callsite is not a draw callsite. |
| `0x260a70` | Visibility renderer. Its second argument contains a mesh-list pointer at +0x14. The observed grass path updates contexts at `0x260bfc` and appends CALL/MSCAL(0)/FLUSH at `0x260c20..0x260c2c`. |
| `0x360818` | Observed return address of the caller submitting all 27 grass lists in the three-field Amazon capture. |
| `0x26f790` | Separate leaf submission helper used by `0x270480`: emits CALL/MSCAL(0)/FLUSH for its first argument. Static lead only; this helper was not used in the measured Amazon window. |

The filled grass payload is 0x380 bytes:

| Qwords | Observed writer inputs |
|---|---|
| 0..2 | Clip scale, clip offset and billboard offset, derived from scratchpad view parameters and the game's field/interpolation values. |
| 3..6 | Matrix product from scratchpad `0x700006a0` and `0x70000d40`, with an additional adjustment in the last vector. |
| 7..10 | Matrix copied through the pointer at scratchpad `0x7000067c`, with a corresponding adjustment. |
| 11..22 | Shader constants copied or constructed by the writer. |
| 23 | Depth bias in x, density in z and distance scale in w. The model does not use y. |
| 24..55 | 32 scatter vectors from the table at `0x713850`, initialized by the grass constructor. |

The existing VIF field-2 repair can alter an invalid second matrix after upload. An oracle must capture effective VU input after that repair and any VIF mode operation, rather than assuming the raw EE payload is always the final input.

The first probe identified `0x260a70` and `0x221b80` as batch leads. The extended capture below verifies `0x260a70` for this scene. `0x221b80` resolves a list through virtual method +0x10 and remains unqualified. Other callers construct dynamic packets instead of using a prebuilt list.

## Opt-in observation probe

`PS2X_TF_SEAM_TRACE=<fresh private CSV path>` enables the CRC-gated probe. `PS2X_TF_SEAM_RANGE=first:last` restricts collection by the asynchronous presenter field label; the default is 0:120 and the maximum interval is 600. A hard cap of 250,000 rows reports truncation to stderr. Parent directories must already exist, and existing traces are never overwritten.

The probe records:

- Allocator caller, destination, size and returned guest address.
- Completed grass context blocks, their caller/object and a 64-bit FNV-1a payload fingerprint. Continuation aliases are wrapped, and return address plus restored stack distinguish completion from a scheduler yield.
- Raw unmasked 56-qword V4-32 uploads, including VIF mode/cycle and resolved VU destination. The EE and VIF fingerprints can be compared without copying game data into the source tree.
- MSCAL/MSCALF entry PC, TOP/ITOP and a 64-byte code fingerprint; MSCNT has no explicit entry PC. MPG records identify code upload ranges and fingerprints.

Trace-only mode does not read live VU data/registers. The optional capture below reads them on their executing thread, inside the accelerated backend boundary. Neither mode adds a VU fence, edits guest state, suppresses DMA/GIF, or changes timing settings. Existing VU code is read on the VIF consumer side. Worker events have no EE field value; presenter labels are approximate and must not be used as unique batch identities. Repeated constants and shared code prefixes can be ambiguous. Use stream order, destination, complete code identity and captured bytes to resolve them before substituting any batch.

The display-list allocator uses the EE's `0x20000000` uncached RAM alias. The probe validates the physical span and reads the corresponding backing RAM, while preserving the original guest address in its record. Scratchpad/MMIO addresses are rejected. The first diagnostic iteration rejected these aliases and emitted `invalid_constants`; it is retained privately as a failed attribution attempt, not evidence of missing or invalid game constants.

For the current private Trainer Amazon seed, use the cutscene-skipping timing route with `PS2X_TF_NATIVE60=3`, static VU enabled, and a short range around 3000:3040. These field numbers are specific to that route. Probe timings are diagnostic overhead, not benchmark results. All raw logs, meshes, matrices and captures stay outside the checkout.

`python -B testing/summarize_native_seam.py <private.csv> --program-manifest <private-extraction/manifest.json>` summarizes fingerprints and checks the named program files against their manifest. Unknown/ambiguous code prefixes stay unresolved. The first trace enters a shared shader dispatcher at VU PC 0, plus an unresolved prefix at 0x450; raw MSCAL counts cannot yet be labeled as grass or reflection counts.

## First measured grass trace

The corrected build-24 hash run covered presenter labels 3000..3040 on the private Amazon route. It matched all 59 cumulative VU state hashes, all six terminal route counters and all eight player probes against build 22. The observer recorded 123,348 rows without reaching its cap:

- 369 completed grass constant blocks, with 82 distinct fingerprints. The context dispatcher used return address `0x23aaf4` 41 times and `0x23ab24` 328 times.
- 369 raw 56-qword uploads to VU qword 53, STCYCL 4/4 and mode 0. Of these, 360 matched a captured grass payload fingerprint; nine unmatched uploads were all at the opening presenter label 3000, consistent with blocks built before the bounded capture began. This is not a claim that those nine have been independently attributed.
- Every one of the 360 matching uploads was followed in the observed stream by a launch whose code prefix matches `vlShaderAssembly_ShaderManager`, at entry 0. This confirms that the dispatcher must be followed to identify the actual shader chain.
- 37,210 MSCAL starts, 71,123 MSCNT continuations and 6,806 MPG upload pieces were observed in this window. These are trace-window counts, not cost shares; the asynchronous window can include different partial frame boundaries across runs.

This establishes a live constant-update path and a reusable observation tool. It does not yet identify a complete replaceable mesh batch, prove byte-exact output, or justify skipping the grass VU program. No native mesh speedup is claimed.

The ordinary threaded run also matched the route's six terminal counters and eight player probes. It captured 369 completed blocks and 369 uploads, with 342 matching fingerprints; the 27 unmatched uploads were restricted to the opening labels 3000..3002. All 342 matches again led to the manager entry. Worker lag changes which earlier EE blocks fall outside a presenter-bounded capture, so later attribution should use submission sequence identifiers rather than treating these labels as synchronized EE/VU frame numbers.

## Whole-list and accelerated output capture

In addition to `PS2X_TF_SEAM_TRACE`, set `PS2X_TF_SEAM_CAPTURE=<fresh private filename prefix>`. `PS2X_TF_SEAM_CALLS` bounds the number of accelerated invocations, default 256, maximum 6,000. Total binary output has a separate 512 MiB hard limit. Use a very short range; the measured capture used 3000:3002 and 5,000 calls. Existing files are never overwritten. Truncated captures, write errors, missing begin/end pairs and fingerprint mismatches must not be accepted as fixtures.

`PS2X_TF_SEAM_FAMILY=grass` restricts binary VU captures to manager continuations selecting the complete known grass program. It checks the full code fingerprint at the stage table's dynamic address on the VU execution thread. The default, `all`, preserves the broader capture. This filter does not restrict trace events or the first EE snapshot. It makes a wider camera-motion window practical without collecting unrelated VU programs.

The observer produces:

- `<prefix>-ee.bin`: the first validated grass submission's EE RAM, scratchpad, low GPR words and boundary metadata. The generic renderer is wrapped across continuation aliases. A newly completed grass context from inside that invocation, restored stack/return PC, and the exact emitted CALL tag are required. Culled objects are not reported as submitted meshes. This snapshot is not globally atomic across other emulated devices.
- `<prefix>-vu-NNNNNN-pre.bin` and `post.bin`: the existing vu1dump layout, with header `{entry/resume-PC, TOP, ITOP, sizeof(VU1State)}`, 16 KiB code, 16 KiB data and state. Captures surround the accelerated backend, after VIF operations and field-2 repair. They do not force interpreter execution.
- `<prefix>-vu-NNNNNN-gif.bin`: ordered records `{path, bytes, drainImmediately, path2DirectHl}` followed by packet bytes. A thread-local invocation ticket records packets before worker buffering; later EE replay has no ticket and is not recorded twice. This is VU output, not a complete GS-state snapshot.

The CSV adds `parent`, `context_hash` and `capture` columns. `constants.parent` identifies the context dispatcher's saved caller. `mesh_call` associates a completed context with its following validated CALL. `vu_begin`/`vu_end` share a capture ticket and fingerprint the complete code/data. On `vu_end`, `destination` counts packets; `cycle` marks a pending-pipeline transfer and `mode` marks interpreter fallback. Either flag disqualifies that invocation as a self-contained replacement fixture. Captured state padding is not meaningful for future register comparisons.

`python -B testing/inspect_native_seam_capture.py <prefix> --trace <private.csv> --program-manifest <private-extraction/manifest.json>` validates capture pairs and fingerprints, walks the captured DMA list through its return, parses VIF boundaries, identifies complete disc microprograms in resident code, and summarizes GIF output. It emits metadata only. It is deliberately bounded and rejects unqualified formats rather than inventing data. Raw RAM, mesh data, microcode and GIF files remain private.

The corrected build-26 capture passed the accelerated cutscene-skipping route and matched all 59 cumulative VU hashes, all six route counters, and all eight player probes/modes against the prior baseline. Its 19,070 CSV rows included 27 completed grass contexts and 27 validated mesh submissions, covering nine distinct list pointers. All used parent `0x260c04` and caller `0x360818`. The earlier build-25 capture preserved the same hashes but found no submissions through the alternative leaf helper; that negative result redirected the hook to the live generic renderer.

The 5,000 complete accelerated fixtures contain nine distinct code images, no pipeline transfers and no interpreter fallbacks. Full-byte matching resolves grass at VU `0x1380`; the shader table selects one stage with constants at qword 53. There are 68 manager continuations with that selection, plus 24 manager initializations carrying the same table. These counts must not be conflated with 92 executions of the grass transform. The continuations emitted 2,590 PATH1 packets totaling 3,111,040 bytes. Every captured packet stream parsed completely, but this alone does not establish GS-state or visual equivalence.

The previously unresolved VU entry `0x450` is the full-byte-matched `vlShaderAssembly_TransformClip_PCT_LoadContext`, immediately after the 0x450-byte manager. The manager's entry zero initializes state and stops; continuations at `0x10`/`0xf8` read ITOP, an input descriptor, and the stage table at qwords 4 and 5 onward. The actual shader chain therefore cannot be inferred from the MSCAL entry alone.

The ordinary threaded capture also passed the route, matching the six counters and eight player probes/modes. Its first list has the same content fingerprint, and every imported chunk matches a VU input there too. Across the single-threaded hash run and the normal worker run, **3,663 identical captured inputs have identical outputs**, including all meaningful VU state bytes, full data memory and exact ordered GIF records. Compiler padding is excluded; cycles are retained. `python -B testing/compare_native_seam_captures.py <left-prefix> <right-prefix>` reproduces this comparison after validating both captures with the inspector. Diagnostic runs are not performance benchmarks.

## First mesh-input importer

`python -B testing/import_grass_seam.py <prefix> --trace <private.csv>` imports the observed single-RET list into word patches and raw position triples per geometry chunk. It admits only the captured absolute-address layouts, 1/1 or 3/1 cycles, and required unpack/mask/row operations. It handles signed packed inputs and the V2/V3 lane behavior, including the real DMA quadword phase. It rejects unsupported layouts. Matching requires the exact grass microprogram fingerprint and the observed manager stage table.

The first captured root has one RET tag with 183 payload qwords: 2,936 VIF bytes including TTE. It contains five chunks, 31 UNPACK commands and five MSCNT continuations. ITOP cycles through qwords 180, 452 and 724. Descriptor counts are 28, 28, 28, 28 and 25. The first two vertices seed a triangle strip; each later vertex completes a source triangle, subject to restart and clipping decisions. There are 127 candidate source triangles across the list. The earlier bounds/instance interpretation was incorrect: the grass shader generates many billboards within each source triangle.

The imported patches match **every written 32-bit word** in all five chunks against accelerated pre-execution fixtures: 352, 344, 344, 344 and 308 words respectively. Each chunk matched twice in the bounded capture, demonstrating reuse across submissions. The importer does not compare unwritten/preserved lanes or prove a unique submission identity. The matching bytes establish the input format; they do not qualify transformed output, persistent VU registers/memory, or a runtime bypass.

Grass emits multiple GS sprite passes, not merely one triangle mesh. A replacement must reproduce clipping, density selection, both field transforms, packet state changes and persistent VU outputs. Native GPU work should consume these imported source triangles and retain the existing ordered renderer dependencies. No speedup from this observation/import work is claimed.

## Handwritten grass geometry and output comparison

`PS2Recomp/ps2xRuntime/src/lib/native/tf_grass_geometry.cpp` implements the observed algorithm using typed source vertices, colors, two matrices and grass constants. It transforms the triangle strip, applies restart and coarse clipping decisions, derives density from triangle area, vertex alpha and depth, generates scatter groups, and recursively subdivides dense triangles. It produces eight sprites in each of two fields per group, preserving the alternating endpoint order. This geometry output contains exact XYZ fixed-point words, clipping words and colors. The separate packet model described below supplies texture coordinates and emitted GS state updates.

The arithmetic uses separate multiply/add steps, rounding toward zero, and x86 flush-to-zero/denormals-are-zero behavior. The model restores the caller's floating environment. Compile it with `/fp:strict` on MSVC; do not enable contraction or reassociation. Recursion, group count, finite inputs and supported formats are bounded. Unsupported input declines comparison rather than suppressing reference execution. Non-x86 floating behavior is not qualified.

The first 68 accelerated fixtures matched **all 2,558 nonempty sprite groups**, bit for bit for geometry, clip words and colors, including count and order. The separate moving-camera capture at presenter labels 3140..3150 added **374 fixtures and 14,538 exact groups**. Its observer run also matched all 59 cumulative VU hashes, six terminal counters and eight player position/mode probes. Empty reference packets are counted separately, because they contain no geometry. These comparisons do not establish complete GIF or persistent VU-state equivalence.

Reproduce the offline check using private captures and a build/output directory outside the checkout:

```powershell
python -B testing/inspect_native_seam_capture.py <prefix> --trace <trace.csv> --program-manifest <manifest.json>
python -B testing/qualify_grass_geometry.py <prefix> --trace <trace.csv> --write-list <fresh-fixtures.txt>
# In an x64 Visual Studio developer shell, with the working directory outside the checkout:
cl /nologo /std:c++20 /O2 /EHsc /fp:strict /W4 <repo>/testing/native_grass_geometry.cpp <repo>/PS2Recomp/ps2xRuntime/src/lib/native/tf_grass_geometry.cpp <repo>/PS2Recomp/ps2xRuntime/src/lib/native/tf_grass_packets.cpp /Fe:native_grass_geometry.exe
./native_grass_geometry.exe --self-test
./native_grass_geometry.exe <fixtures.txt> <fresh-private-output-directory> 1
python -B <repo>/testing/qualify_grass_geometry.py <prefix> --trace <trace.csv> --native-directory <private-output-directory>
```

The C++ driver checks floating-environment preservation and repeatability and writes private output for the independent captured-GIF comparison. Its optional repetition count measures thread cycles, not whole-game performance. Synthetic tests exercise strip restarts, signaling-NaN rejection, rounding restoration, unsupported packet layouts, field-dependent color rejection and deliberate geometry corruption. No captured game bytes are included in those tests.

`PS2X_TF_GRASS_VERIFY=1` enables an opt-in live comparison for the supported USA executable. Before each recognized grass continuation it calculates native geometry; the accelerated VU code then runs unchanged. Its PATH1 packets are compared before worker buffering, and a mismatch stops the run visibly. The check supports the dynamic stage address and constant base, reports matched/declined calls and group counts, and retains accelerated execution for every unsupported case. Timing output includes diagnostic overhead and must not be interpreted as a gameplay speedup. Leave raw seam capture off when collecting these timings.

Build 28 passed the complete accelerated route with live comparison in both deterministic and ordinary threaded modes. Each run recorded at least 37,888 matched calls and 1,482,439 exact sprite groups with zero declines and no mismatches. These are lower bounds from periodic progress lines: the runner uses `_Exit`, so an initial destructor-based final report was skipped. The deterministic run retained all 59 cumulative VU state hashes; both runs retained all six terminal counters and all eight player position/mode probes. This covers the private hub-to-Amazon route, walking and transformation, not all levels or a campaign playthrough.

Build 29 reports explicitly after worker shutdown. Its threaded route passed with **38,071 matched grass calls, 1,490,434 exact sprite groups, 17,894 empty reference packets, zero declines and zero mismatches**. All six route counters and eight player position/mode probes remained unchanged. The math and packet comparison are unchanged from build 28. The normal launcher continues to use the previous accelerated build with comparison disabled.

## Complete grass packets

`tf_grass_packets.cpp` reconstructs the full 1,216-byte sprite packets from effective VU input and the geometry model. The grass program initializes two alternating 76-qword buffers at `ITOP+85` and `ITOP+161`. Each field's six-qword prefix comes from context qwords 11..16 or 17..22. ST coordinates alternate corners and center, while the shader's unwritten ST.w padding is preserved separately for each buffer. Geometry and colors fill the remaining slots, with colors repeated in both fields. A draw with no sprites emits the unchanged tag at VU qword 1023 exactly once. No template bytes from the game are embedded in source.

The offline check now compares complete ordered records, including PATH1, byte count, drain/DIRECTHL flags, all tags, GS register writes, ST, colors, XYZ and preserved padding. Both original capture sets pass: **442 calls, 17,096 sprite packets and 225 empty packets, totaling 21,069,472 bytes including record headers**. Deliberate GS-state and transport-header mutations are rejected even when geometry still matches.

Build 30 extends `PS2X_TF_GRASS_VERIFY=1` to the same complete-packet comparison. The deterministic route matched 37,945 calls, 1,485,364 sprite packets and 17,840 empty packets, with all 59 cumulative VU state hashes unchanged. The threaded route matched 38,239 calls, 1,497,194 sprite packets and 17,966 empty packets. Both had zero declines/mismatches and retained all six route counters and all eight player probes. Counts after the route endpoint differ because the worker can finish additional submissions while the final screenshot and shutdown complete.

This closes the emitted-packet gap for the measured route. It does not yet qualify replacing VU execution. Captures also show changes to the input descriptor/attribute stream, both packet buffers, the recursion stack near the top of VU memory, scalar/vector registers, status/clip/MAC flags and the cycle counter. The packet model does not write these persistent effects. A register changing is not proof that its value is consumed by a later shader; that distinction requires dependency analysis and accelerated replay across program transitions.

## Persistent-state replay and the first dependency

`testing/vu1_replay` builds a standalone Release/AVX2 runner from the verified private static package. It never links the interpreter oracle. The [usage notes](../../testing/vu1_replay/README.md) describe capture integrity checks, output comparisons and a deliberately separate register experiment. Ordinary replay compares every semantic state field, including cycles, all VU data/code and full framed GIF output, rejecting fallback and pending transfers.

Exact replay passed all 5,000 consecutive broad-capture calls across nine resident code images: 11,998,416 GIF record bytes, zero data/state/code/packet mismatches and zero fallback or pending transfers. A second exact run after the dependency-mode changes retained those results. This qualifies the replay tool against this accelerated capture, not a native replacement.

The dependency mode restores selected VF registers to each grass call's input values after that call, then runs subsequent accelerated calls with the altered state. It applies observed external component writes between calls and stops at the first changed data write, packet or control result. At capture ticket 1824, a three-stage continuation (`0x2150`, `0x1410`, `0x480` in that resident image) changes scratch data when a preceding grass result is omitted:

| Restored grass result | First changed output | Calls before stopping |
|---|---|---|
| VF27 | VU qword 1007 | 1,824 |
| VF31 | VU qword 1003 | 1,824 |
| All other VF5..VF31 together | No data, GIF or control change in this capture; 958 register-state differences | 5,000 |

Both failing experiments had restored 34 grass calls; their GIF bytes and control/cycles still matched at the stopping point. This is a concrete reason to preserve shared state even after matching the visible geometry. It does not establish whether either scratch value ultimately affects a visible primitive. Equal-value external writes are not observable in snapshots, so the experiment can conservatively retain a perturbation through such a write. The passing mask is limited to this capture and these input-value perturbations; it does not prove those registers are globally dead.

A private post-memory model also reproduces all 442 grass fixtures outside qwords 987..1022. It accounts for the manager's descriptor/attribute writes and alternating packet buffers. That remaining range contains recursion/scratch state. The model is exploratory and not integrated. Neither a simple cycle-count fit nor complete register/flag reconstruction is qualified. Full persistent effects or a larger independently qualified batch boundary remain required before removing VU execution.

## Implementation sequence

1. Extend the observed grass boundary and exact first-list importer to more visible lists and static mesh families. Measure coverage instead of assuming every launch belongs to the last grass upload.
2. Complete the output oracle with surrounding GS state and explicit submission identities. The existing fixtures contain mesh input, effective constants, VU entry/exit state and ordered VU GIF output. Add a runtime mesh cache keyed by content and source generation, invalidating it on guest writes or level unload.
3. Extend the now-complete grass packet comparison to persistent VU state, including transitions into uncovered programs. Use accelerated replay to reproduce scratch/register dependencies and cycle effects. Investigate a simpler static mesh family when useful. Keep fallback for every unqualified case.
4. Replace only qualified whole batches with that host transform, still producing ordered GS primitives. This separates mesh/transform correctness from graphics backend work and can already remove interpreted VIF/VU overhead.
5. Add native vertex buffers, constants and a vertex shader to the hardware renderer. Extend the ordered GS command stream with native draw packets and preserve current texture/CLUT, target/depth, alpha, scissor, multipass and VRAM dependency handling. Keep transformed vertices on the GPU; reuse the existing direct presentation path.

`deps/hwgs/tf_hwgs.h` currently accepts GIF and VRAM operations plus presentation surfaces, not mesh draws. The PCSX2 integration is therefore a backend extension, not an existing API that merely needs enabling. Compare extending its draw/texture-cache path against a dedicated renderer only after the measured GS feature set is known. BT3's eventual separate renderer implements its own game's post-processing; importing it wholesale would not implement Transformers' shadows, foliage or reflections.

In the pinned PCSX2 source, `GSRendererHW::Draw`/`DrawPrims` prepare texture-cache targets and a `GSHWDrawConfig` for `GSDevice::RenderHW`. That structure takes CPU `GSVertex` arrays containing already transformed GS XYZ/ST/RGBAQ values. GPU mesh transforms therefore need an explicit native vertex-buffer/constant path, plus a conservative way to supply bounds and texture dependencies to the existing renderer. Merely invoking `RenderHW` with the original mesh cannot provide that.

GPU arithmetic is a separate qualification gate. Vulkan exposes device properties for [32-bit rounding and denormal controls](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFloatControlsProperties.html), and SPIR-V provides [entry-point rounding modes](https://github.khronos.org/SPIRV-Registry/extensions/KHR/SPV_KHR_float_controls.html). A local `vulkaninfo` query on the RTX 3080 Laptop GPU with NVIDIA driver 617.42 reports `shaderRoundingModeRTZFloat32=true` but `shaderDenormFlushToZeroFloat32=false`. Toward-zero arithmetic is therefore a promising Vulkan prototype route; explicit handling or exclusion of denormal cases still needs measurement. These capabilities do not establish exact division/square-root behavior or a working shader. Direct3D's [floating-point rules](https://learn.microsoft.com/en-us/windows/win32/direct3d11/floating-point-rules) likewise are not evidence that translating the CPU model to HLSL will reproduce every intermediate bit. Keep the captured attribute oracle when testing any GPU implementation.

Exit gates remain distinct: measured CPU savings, picture equivalence at base scale, actual high-resolution scene detail, preserved mixed native/emulated ordering and readbacks, and real-time gameplay equivalence at 60 Hz. Native geometry alone does not fix the outstanding simulation-timing differences.

## D3D12 compute-to-draw component

The opt-in [GPU geometry build](../../deps/hwgs/GPU_GEOMETRY.md) patches the
pinned PCSX2 D3D12 device with `RenderHWFromGPU`. A compute shader can now
write the renderer's vertex format and pass that buffer directly to its
existing draw pipeline. Synthetic overlapping draws passed on the RTX 3080
Laptop GPU, including return to the ordinary vertex upload path. The generated
vertices remain on the GPU; a final image download is only a diagnostic check.

This resolves the first backend interface question, not the full engine seam.
Real batch inputs, exact transform arithmetic, GS draw configuration, texture
dependencies and persistent VU state remain separate qualification steps.
The experimental dual-transform shader consumes captured mesh inputs in an
offline GPU diagnostic. No live VU work is suppressed by these components.

The first real shader qualification covers the two-stage
`DualTransform -> TransformClip_PCT` chain when the captured clip-state block
is zero and the manager input layout is the supported three-qword format.
Of 916 captured continuations with that program chain, 776 qualify; clipped
and alternate-layout inputs remain outside the implementation. They contain
18,568 source vertices, each transformed twice. All 37,136 resulting 32-byte
`GSVertex` records now match captured accelerated output exactly on the RTX 3080
Laptop GPU. Ordinary shader arithmetic initially changed 850 packed XY words
by at most one GS subpixel, plus STQ and depth values; explicit toward-zero
arithmetic removed every mismatch in this set.

The first test packed depth incorrectly by retaining the VU fixed-four-bit Z
and omitted fog. The corrected version implements PACKED XYZF2 decoding and
checks its expected format with the renderer's native register declarations.
All records still match, including raw transformed STQ/XYZ and restart words.
Unused UV is canonicalized because this path uses STQ.

## Dual-transform state preservation and bypass

Dependency experiments across the same 5,000-call enhanced capture found two
required tail registers: later clipped draws consume VF17/VF18. They also found
observable color copies and ST.w padding. Omitting only transformed STQ.xyz
and XYZ.xyz, plus registers other than VF17/VF18, preserved every packet and
checked later memory write across the window. This is a bounded dependency
observation, not proof that the omitted state is globally dead.

The new `tf_dual_state.cpp` preservation model reproduces packet headers,
colors, padding, restart words, VF17/VF18 and guest cycles for all 776 selected
calls. It computes at most one retained vertex on the CPU; the bulk geometry
stays unresolved. Counts 17 and 18 have distinct register tails, which the
model handles explicitly. The game runtime does not yet invoke this model.

The offline replay now **skips the 776 selected accelerated VU calls**, supplies
their actual GPU-transformed geometry to diagnostic output packets, and carries
the unresolved guest state forward. Before each replacement it requires the
carried inputs/constants to equal those uploaded to the GPU. The remaining
calls execute through the static VU package. All **5,000 calls passed**, with
**11,998,416 identical framed packet bytes**, matching checked later memory
writes and control/cycles, zero interpreter fallback and zero pending transfer.
Full register state differs on 1,240 calls; it is intentionally not reported
as an exact-state replay. Snapshot comparison also cannot identify equal-value
external writes. An uncovered live reader needs state materialization or a
proved engine ownership boundary before this can become a runtime bypass.

See [the GPU build instructions](../../deps/hwgs/GPU_GEOMETRY.md) for the hashed
private GPU capture and replay workflow. The diagnostic reads back GPU results
for comparison; production drawing must keep them on the GPU. GS state,
textures, bounds, actual strip indices, ordered submission and buffer lifetime
remain integration work. No live offload or FPS gain is claimed from this replay.

## Resident GPU submission and observed draw states

The next component keeps the actual dual-transform shader output on the GPU
through drawing. Pooled upload/output allocations are protected by both retained
batch handles and the final draw fence. The D3D12 device's native handler can
consume this buffer with an existing `GSHWDrawConfig`, then suppress the normal
CPU vertex upload. Captured-mesh diagnostic rendering passed 3,106 native draw
substitutions and 3,106 ordinary comparison draws with zero pixel differences;
it also exercised pool exhaustion, completed-buffer reuse and allocation growth.
This uses fitted, untextured diagnostic viewports, not the game's live scene.

An enhanced PGO Amazon run with a read-only native handler passed the level-only
route with cinematics skipped, direct presentation, 4x internal rendering and
1920x1080 output. It recorded **67,546 draws over fields 3000..3010**, uncapped.
Of 36,254 ordinary triangle draws, 36,107 use textures, 35,792 depth, 30,908
automatic LOD, 1,222 manual LOD and 3,735 palettes. There are 44 shader/depth/mask
groups; the two largest account for 25,450 draws. Both software and hardware
blending occur frequently. No feedback/barrier, DATE, second-alpha-pass or
blend-multipass cases occur in this triangle subset. These observations do not
attribute particular draws to a VU family or cover other camera/level states.

The count supports grouping native mesh work before compute submission; it is
not itself a cost profile. The next integration boundary must preserve texture
selection/LOD, target/depth ownership and blend state while supplying bounds
without requiring bulk CPU geometry. The pool and device handler now support
that submission once the upstream boundary is identified. No live VU bypass
or new game performance result is claimed. See [commands and scope](../../deps/hwgs/GPU_GEOMETRY.md).

## Whole-list grouping and source residency

The EE snapshot contains 208 candidate roots in the observed CALL form. After
qualifying V4-8 UNPACK, 125 lists import with the bounded decoder. Matching every
written word and requiring complete consecutive sequences identifies 740
selected chunks as 224 whole mesh instances. Ambiguous physical roots and
overlapping capture ranges are excluded. This recovers actual list-decoded
geometry, but the snapshot can contain old buffers, so it does not establish a
unique live engine caller.

Their 35,192 GPU output vertices match captured geometry exactly. Grouped
GPU drawing uses 898 submissions where the separate reference chunks use 2,966,
with identical diagnostic images. The GPU mesh source can now remain in local
video memory across dispatches; repeated preparation uploads only transforms.
Source and output pools both preserve in-flight resources without forcing waits.
The grouping diagnostic also exercises source-budget eviction and invalid-offset
rejection. See [the detailed results](../../deps/hwgs/GPU_GEOMETRY.md).

All 776 selected packet pairs in the earlier capture set use the same two
SCISSOR/XYOFFSET prefixes, covering separate y ranges 0..223 and 224..447. This
supports investigating per-mesh field grouping; it does not prove that live
texture feedback or interleaved commands permit reordering. The remaining
integration work is still draw-state preparation, ordered native submission,
and unresolved VU-state handling. No additional game run or FPS claim follows
from these offline checks.

## CPU-independent mesh bounds

Resident meshes now cache source extents and color ranges. The native component
can project eight corners per whole mesh, with conservative bounds on VU
rounding, to obtain packed XYZ, STQ and normalized texture coverage without
transforming all source vertices on the CPU. All 224 qualified mesh instances
pass containment checks for all 35,192 captured output vertices. Nonfinite,
overflowing, nonpositive-Q and W=0-crossing cases are declined.

The grouped draw diagnostic uses these bounds for viewport preparation and
still matches its separate reference draws exactly. The maximum conservative
box area is 10.15244 times the exact projected vertex box in this window; its
effect on texture-cache work remains unmeasured. The bounds provider feeds the
scoped GS frontend described below; it does not qualify live VU bypass.

The earlier transform-update diagnostic swapped the two view matrices, which
were equal in these captures. It therefore did not establish that updated
matrices were consumed. The replacement swaps only the second field's X/Y rows and compares
against independently swapped captured packed coordinates, requiring nonzero
coordinate changes. That corrected diagnostic also passes. See
[the implementation and limitations](../../deps/hwgs/GPU_GEOMETRY.md).

All 740 grouped chunks have identical field matrices. The GPU shader now reuses
the first transformed record in that case while retaining separate field draws.
All 37,136 outputs in the larger 776-chunk arithmetic set still match, and the
resident-draw test explicitly exercises distinct matrices after its update.
No live gameplay measurement is implied by this shader optimization.

## Textured GS frontend from cached bounds

`DrawNativeTriangles` now runs the ordinary GS texture-cache, depth, alpha-test,
blend and LOD preparation from native bounds. Hardware renderer helpers lazily
materialize exact CPU vertices if a special case needs to inspect or mutate
them; that submission then stays on the CPU draw path. Warm render targets and
independent proof of nonconstant depth are required. Final GIF/VU latches and
ordered cross-thread ownership remain the caller's responsibility.

The enhanced-capture diagnostic submitted 1,216 GPU draws with no CPU geometry
materialization, plus 408 intentionally declined submissions checking fallback.
It exercised generated textures, depth, blending, alpha testing and mipmaps at
2048 Ã— 1792. All 14,680,064 full-resolution pixel comparisons against independent
GIF reference draws matched. This closes the earlier fixed-untextured-state
limitation, but does not establish game-specific texture feedback, live VU
bypass, or a frame-rate gain. See [eligibility and commands](../../deps/hwgs/GPU_GEOMETRY.md).

The enhanced Amazon route passed with the shared renderer changes and live VU
bypass disabled. Its measured gameplay window reached 54.9299 physics updates/s
(17.736 ms median, 22.2 ms p95). Only the enhanced accelerated build ran, with
cinematics skipped. This sample does not attribute a speedup to GPU offload or
establish distinct rendered 60 fps.

## Ordered live GPU drawing

The opt-in live handoff now carries owned source geometry with its original
GIF packet through every worker/GS queue. It preserves state prefixes, strip
ordering and final GS latches, with ordinary accelerated packet data available
for fallback. GPU compute runs only when a draw actually reaches the native
submission callback; both fields reuse its result.

The first enhanced Amazon attempt exposed severe source-cache thrashing and
was stopped. Expanding the cache from 256 to 2,048 entries eliminated that
churn in the subsequent passing route: 1,687 source uploads, 99.7365% source
reuse, 1,190,571 native GPU submissions and no pool-capacity declines. All
640,436 selected VU calls attached their companions; GS totals can be lower
because runtime rewrites/fallback and shutdown delivery are separate stages.
This is live GPU rendering, but static VU still computes the retained CPU
fallback and guest state.

The gameplay-only measurement was 49.9560 physics updates/s at 4x internal
resolution and 1920x1080 direct presentation, with cinematics skipped. The
final frame was inspected, not qualified for exact pixel equality. Actual VU
bypass, CPU-independent fallback/state preservation and distinct rendered
60 fps remain unfinished. See [the handoff contract and results](../../deps/hwgs/GPU_GEOMETRY.md#ordered-live-geometry-handoff).

## Guarded bulk-transform bypass

Mode 2 now shortens eligible static VU calls to a three/four-vertex CPU tail,
while the existing GPU path transforms their full source geometry. Native
bookkeeping preserves complete final registers and guest cycles. Deferred
STQ/XYZ ownership distinguishes overwritten memory from CPU readers; explicit
VIF/EE write hooks cover equal-value writes as well as masked/partial writes.
Fallback rendering reconstructs geometry from immutable sources without GPU
readback. The final resolved tail supplies ordinary GS latches and the depth
witness needed by the native draw path.

The accelerated capture check passes all 776 selected calls, comparing every
semantic VU-state field, all memory after materialization and both fields'
geometry. It also checks FP restoration and deferred-memory overwrite behavior.
Its 18,568 source vertices require 3,088 static-tail vertices, plus native
register bookkeeping. This is a reduction in the static transform pass. Other VU
programs, small batches, special 17/18-vertex tails and unproved arithmetic
remain on the complete accelerated path.

The enhanced Amazon route passed with mode 2, 16:9, 4x internal rendering,
1920x1080 direct output and cinematics skipped. Its gameplay window measured
48.0763 physics updates/s, median 20.006 ms and p95 29.398 ms. Whole-session
counters recorded 576,735 shortened VU calls, 12,215,474 skipped source vertices
and 1,188,563 native GPU draws. However, later CPU reads reconstructed 20,554,663
vertex/field outputs: 84.1% of the skipped two-field output. No net performance
improvement or distinct rendered 60 fps was established.

The next change retains deferred geometry across two complete setup programs:
the manager reads qword 4.x, and the context loader reads matrix qwords 0..3.
Complete program fingerprints identify these read footprints; accelerated
execution still updates the registers and halts normally. All 1,784 captured
setup calls (863 manager, 921 context) match their full expected state with
every other data word poisoned, without data/code writes, GIF output or fallback. Runtime
read-reason counters distinguish EE, input, unpack and ordinary-program
materializations.

Hardware UNPACK now discards old deferred geometry when its mask replaces all
three owned coordinates, even if W remains protected. Partial or protected
coordinates retain materialization. Poisoning the old XYZ values in 106,496
accelerated unpack cases leaves every admitted write's output memory and ROW
state exact. Reads that touch no deferred words also avoid FP-mode setup.

The subsequent enhanced Amazon run passed at 55.7436 physics updates/s and
55.5695 GPU-frame handoffs/s, with 16:9, 4x internal rendering, 1920x1080 direct
output and cinematics skipped. Deferred CPU reconstruction fell to 16.06% of
skipped two-field outputs (3,930,994 outputs); ordinary-program reads account
for 3,528,598 and input reads for 402,396. EE and UNPACK reconstruction were
zero. The final frame was inspected. This single run remains below the target
and does not prove distinct rendered 60 fps; the remaining work needs a new
in-level profile rather than an assumption that the old bottleneck persists.

See [mode 2's admission rules and test command](../../deps/hwgs/GPU_GEOMETRY.md#guarded-cpu-tail-and-deferred-vu-memory).

## Group resident GPU transforms

The next live enhancement combines up to eight adjacent native packets into
one compute dispatch, reading their independently cached source buffers.
Output offsets preserve each packet's vertices, and all GS state changes and
draws remain in order. Grouping does not cross another command or wait for a
larger batch. It adds no geometry readback.

The grouped enhanced level route passed at 57.3835 physics updates/s and
57.1753 direct-frame handoffs/s. Whole-session compute dispatches decreased to
172,005, with 615,276 packets using grouped work. There were no group, bounds or
capacity declines. Runtime bypass/deferred-memory counters match the preceding
enhanced route; the independent textured frontend comparison has zero differing
pixels. These results remain below true 60 fps. The pre-grouping profile still
attributes 14.71% of VU instruction-pointer samples to the two hottest grass
regions, making grass a concrete next GPU geometry target. Its persistent
VU-state dependencies remain a required part of that replacement.

See [the grouped GPU contract and diagnostics](../../deps/hwgs/GPU_GEOMETRY.md#ordered-gpu-dispatch-groups).

## Grass emission boundary

The grass candidate now offloads billboard endpoint projection, perspective
division, clipping flags and color interpolation in an offline GPU shader.
All 17,096 captured packets match exactly. An accelerated replay then skips
two middle emission iterations, executes the last iteration to retain VU
state, and uses the actual GPU geometry instead of running the full CPU loop.
Both capture sets pass complete state, memory and ordered packet checks,
including 14,448 overlapping graphics transfers.

The surviving state dependency is the preceding pair's second-field clipping
flags, retained in VF7.w. A small calculation preserves those flags while the
existing pipeline calendar progresses the earlier transfer. This is narrower
than replacing the whole grass program and avoids assuming its registers are
dead. It is still an offline experiment with diagnostic GPU readback. The
native renderer at that stage accepted indexed triangles. The sprite extension
below addresses direct GPU consumption; deferred memory/fallback handling still
gates the live shortcut.

See the [grass contract and reproducible diagnostics](../../deps/hwgs/GPU_GEOMETRY.md#grass-gpu-emission-and-retained-state).

The next renderer milestone qualifies GPU-resident grass sprites through the
real GS frontend. Source-derived bounds replace CPU vertex tracing; GPU culling
matches both ADC and GS screen-edge rejection. Two enhanced capture samples pass
textures/depth/blending/mipmaps with 2,970 GPU draws, no CPU vertex materialization
in the native modes, and zero differences across 29,360,128 full-resolution
pixels. Deliberate CPU fallback and the existing grouped triangle path also pass.
Live grass companions, deferred-memory ownership and the CPU fallback materializer
remain to be connected to the shortened accelerated loop. No new live FPS claim
follows from these diagnostics. See [resident grass sprites](../../deps/hwgs/GPU_GEOMETRY.md#resident-grass-sprites).


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

These are correctness results from enhanced captures; the last live measurement
above remains the performance reference until the new enhanced level run.
