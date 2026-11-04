# Transformers native render seam investigation

Status: whole-list capture, exact mesh-input import and a handwritten grass geometry model, November 2026. The model can be compared with accelerated VU output offline or during gameplay. It does not replace VU execution or execute geometry on the GPU. Direct GPU presentation and hardware resolution scaling are separate, already implemented features. Normal play retains the accelerated VU path.

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

`PS2Recomp/ps2xRuntime/src/lib/native/tf_grass_geometry.cpp` implements the observed algorithm using typed source vertices, colors, two matrices and grass constants. It transforms the triangle strip, applies restart and coarse clipping decisions, derives density from triangle area, vertex alpha and depth, generates scatter groups, and recursively subdivides dense triangles. It produces eight sprites in each of two fields per group, preserving the alternating endpoint order. Output contains exact XYZ fixed-point words, clipping words and colors; texture coordinates, GIF templates and GS state are deliberately outside this model's output.

The arithmetic uses separate multiply/add steps, rounding toward zero, and x86 flush-to-zero/denormals-are-zero behavior. The model restores the caller's floating environment. Compile it with `/fp:strict` on MSVC; do not enable contraction or reassociation. Recursion, group count, finite inputs and supported formats are bounded. Unsupported input declines comparison rather than suppressing reference execution. Non-x86 floating behavior is not qualified.

The first 68 accelerated fixtures matched **all 2,558 nonempty sprite groups**, bit for bit for geometry, clip words and colors, including count and order. The separate moving-camera capture at presenter labels 3140..3150 added **374 fixtures and 14,538 exact groups**. Its observer run also matched all 59 cumulative VU hashes, six terminal counters and eight player position/mode probes. Empty reference packets are counted separately, because they contain no geometry. These comparisons do not establish complete GIF or persistent VU-state equivalence.

Reproduce the offline check using private captures and a build/output directory outside the checkout:

```powershell
python -B testing/inspect_native_seam_capture.py <prefix> --trace <trace.csv> --program-manifest <manifest.json>
python -B testing/qualify_grass_geometry.py <prefix> --trace <trace.csv> --write-list <fresh-fixtures.txt>
# In an x64 Visual Studio developer shell, with the working directory outside the checkout:
cl /nologo /std:c++20 /O2 /EHsc /fp:strict /W4 <repo>/testing/native_grass_geometry.cpp <repo>/PS2Recomp/ps2xRuntime/src/lib/native/tf_grass_geometry.cpp /Fe:native_grass_geometry.exe
./native_grass_geometry.exe --self-test
./native_grass_geometry.exe <fixtures.txt> <fresh-private-output-directory> 1
python -B <repo>/testing/qualify_grass_geometry.py <prefix> --trace <trace.csv> --native-directory <private-output-directory>
```

The C++ driver checks floating-environment preservation and repeatability and writes private output for the independent captured-GIF comparison. Its optional repetition count measures thread cycles, not whole-game performance. Synthetic tests exercise strip restarts, signaling-NaN rejection, rounding restoration, unsupported packet layouts, field-dependent color rejection and deliberate geometry corruption. No captured game bytes are included in those tests.

`PS2X_TF_GRASS_VERIFY=1` enables an opt-in live comparison for the supported USA executable. Before each recognized grass continuation it calculates native geometry; the accelerated VU code then runs unchanged. Its PATH1 packets are compared before worker buffering, and a mismatch stops the run visibly. The check supports the dynamic stage address and constant base, reports matched/declined calls and group counts, and retains accelerated execution for every unsupported case. Timing output includes diagnostic overhead and must not be interpreted as a gameplay speedup. Leave raw seam capture off when collecting these timings.

Build 28 passed the complete accelerated route with live comparison in both deterministic and ordinary threaded modes. Each run recorded at least 37,888 matched calls and 1,482,439 exact sprite groups with zero declines and no mismatches. These are lower bounds from periodic progress lines: the runner uses `_Exit`, so an initial destructor-based final report was skipped. The deterministic run retained all 59 cumulative VU state hashes; both runs retained all six terminal counters and all eight player position/mode probes. This covers the private hub-to-Amazon route, walking and transformation, not all levels or a campaign playthrough.

Build 29 reports explicitly after worker shutdown. Its threaded route passed with **38,071 matched grass calls, 1,490,434 exact sprite groups, 17,894 empty reference packets, zero declines and zero mismatches**. All six route counters and eight player position/mode probes remained unchanged. The math and packet comparison are unchanged from build 28. The normal launcher continues to use the previous accelerated build with comparison disabled.

## Implementation sequence

1. Extend the observed grass boundary and exact first-list importer to more visible lists and static mesh families. Measure coverage instead of assuming every launch belongs to the last grass upload.
2. Complete the output oracle with surrounding GS state and explicit submission identities. The existing fixtures contain mesh input, effective constants, VU entry/exit state and ordered VU GIF output. Add a runtime mesh cache keyed by content and source generation, invalidating it on guest writes or level unload.
3. Extend the grass host model's geometry comparison to complete packet state and persistent VU state, including transitions into uncovered programs. Investigate a simpler static mesh family in parallel with this dependency analysis when useful. Keep fallback for every unqualified case.
4. Replace only qualified whole batches with that host transform, still producing ordered GS primitives. This separates mesh/transform correctness from graphics backend work and can already remove interpreted VIF/VU overhead.
5. Add native vertex buffers, constants and a vertex shader to the hardware renderer. Extend the ordered GS command stream with native draw packets and preserve current texture/CLUT, target/depth, alpha, scissor, multipass and VRAM dependency handling. Keep transformed vertices on the GPU; reuse the existing direct presentation path.

`deps/hwgs/tf_hwgs.h` currently accepts GIF and VRAM operations plus presentation surfaces, not mesh draws. The PCSX2 integration is therefore a backend extension, not an existing API that merely needs enabling. Compare extending its draw/texture-cache path against a dedicated renderer only after the measured GS feature set is known. BT3's eventual separate renderer implements its own game's post-processing; importing it wholesale would not implement Transformers' shadows, foliage or reflections.

In the pinned PCSX2 source, `GSRendererHW::Draw`/`DrawPrims` prepare texture-cache targets and a `GSHWDrawConfig` for `GSDevice::RenderHW`. That structure takes CPU `GSVertex` arrays containing already transformed GS XYZ/ST/RGBAQ values. GPU mesh transforms therefore need an explicit native vertex-buffer/constant path, plus a conservative way to supply bounds and texture dependencies to the existing renderer. Merely invoking `RenderHW` with the original mesh cannot provide that.

GPU arithmetic is a separate qualification gate. Vulkan exposes device properties for [32-bit rounding and denormal controls](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFloatControlsProperties.html), and SPIR-V provides [entry-point rounding modes](https://github.khronos.org/SPIRV-Registry/extensions/KHR/SPV_KHR_float_controls.html). A local `vulkaninfo` query on the RTX 3080 Laptop GPU with NVIDIA driver 617.42 reports `shaderRoundingModeRTZFloat32=true` but `shaderDenormFlushToZeroFloat32=false`. Toward-zero arithmetic is therefore a promising Vulkan prototype route; explicit handling or exclusion of denormal cases still needs measurement. These capabilities do not establish exact division/square-root behavior or a working shader. Direct3D's [floating-point rules](https://learn.microsoft.com/en-us/windows/win32/direct3d11/floating-point-rules) likewise are not evidence that translating the CPU model to HLSL will reproduce every intermediate bit. Keep the captured attribute oracle when testing any GPU implementation.

Exit gates remain distinct: measured CPU savings, picture equivalence at base scale, actual high-resolution scene detail, preserved mixed native/emulated ordering and readbacks, and real-time gameplay equivalence at 60 Hz. Native geometry alone does not fix the outstanding simulation-timing differences.
