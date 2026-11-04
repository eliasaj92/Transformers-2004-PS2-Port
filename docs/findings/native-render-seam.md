# Transformers native render seam investigation

Status: observation tooling and executable analysis, November 2026. No geometry is replaced or executed on the GPU by this probe. Direct GPU presentation and hardware resolution scaling are separate, already implemented features. Normal play retains the accelerated VU path.

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

The filled grass payload is 0x380 bytes:

| Qwords | Observed writer inputs |
|---|---|
| 0..2 | Values derived from scratchpad view parameters and the game's field/interpolation values. Semantic names still need an output oracle. |
| 3..6 | Matrix product from scratchpad `0x700006a0` and `0x70000d40`, with an additional adjustment in the last vector. |
| 7..10 | Matrix copied through the pointer at scratchpad `0x7000067c`, with a corresponding adjustment. |
| 11..22 | Shader constants copied or constructed by the writer. |
| 23 | Four game-global float parameters. |
| 24..55 | 512 bytes from the table at `0x713850`, initialized by the grass constructor. |

The existing VIF field-2 repair can alter an invalid second matrix after upload. An oracle must capture effective VU input after that repair and any VIF mode operation, rather than assuming the raw EE payload is always the final input.

Two concrete batch leads precede GPU work. `0x260a70` checks visibility, calls the context dispatcher, then appends a DMA CALL using a list pointer at its second argument +0x14. `0x221b80` appends the same CALL template after resolving a list through virtual method +0x10. Their template contains MSCAL at entry zero and FLUSH. These are static-analysis leads; the first probe does not yet establish which one owns the observed grass draws. Other callers construct dynamic packets instead of using this list path.

## Opt-in observation probe

`PS2X_TF_SEAM_TRACE=<fresh private CSV path>` enables the CRC-gated probe. `PS2X_TF_SEAM_RANGE=first:last` restricts collection by the asynchronous presenter field label; the default is 0:120 and the maximum interval is 600. A hard cap of 250,000 rows reports truncation to stderr. Parent directories must already exist, and existing traces are never overwritten.

The probe records:

- Allocator caller, destination, size and returned guest address.
- Completed grass context blocks, their caller/object and a 64-bit FNV-1a payload fingerprint. Continuation aliases are wrapped, and return address plus restored stack distinguish completion from a scheduler yield.
- Raw unmasked 56-qword V4-32 uploads, including VIF mode/cycle and resolved VU destination. The EE and VIF fingerprints can be compared without copying game data into the source tree.
- MSCAL/MSCALF entry PC, TOP/ITOP and a 64-byte code fingerprint; MSCNT has no explicit entry PC. MPG records identify code upload ranges and fingerprints.

It never reads live VU data/registers, adds a VU fence, edits guest state, suppresses DMA/GIF, or changes timing settings. Existing VU code is read on the VIF consumer side. Worker events have no EE field value; presenter labels are approximate and must not be used as unique batch identities. Repeated constants and shared code prefixes can be ambiguous. Use stream order, destination, complete code identity and eventually captured bytes to resolve them before substituting any batch.

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

## Implementation sequence

1. Attribute the grass constant writer to its calling context dispatcher and following VU launch stream. Identify the complete batch root and its mesh/VIF-list pointer; add static mesh families using the same method. Measure coverage instead of assuming every launch belongs to the last grass upload.
2. Capture bounded fixtures at the established batch boundary: mesh list, effective constants, relevant VU entry/exit state, ordered GIF output and GS state. Build a parser cached by content and source generation, invalidating it on guest writes or level unload.
3. Implement a host transform for one family, preferably a simple static mesh if its dependency map is smaller than grass. Compare geometry packets and persistent VU state against the accelerated reference, including clipping, strip restarts, two-field transforms and transitions into uncovered programs. Keep fallback for every unqualified case.
4. Replace only qualified whole batches with that host transform, still producing ordered GS primitives. This separates mesh/transform correctness from graphics backend work and can already remove interpreted VIF/VU overhead.
5. Add native vertex buffers, constants and a vertex shader to the hardware renderer. Extend the ordered GS command stream with native draw packets and preserve current texture/CLUT, target/depth, alpha, scissor, multipass and VRAM dependency handling. Keep transformed vertices on the GPU; reuse the existing direct presentation path.

`deps/hwgs/tf_hwgs.h` currently accepts GIF and VRAM operations plus presentation surfaces, not mesh draws. The PCSX2 integration is therefore a backend extension, not an existing API that merely needs enabling. Compare extending its draw/texture-cache path against a dedicated renderer only after the measured GS feature set is known. BT3's eventual separate renderer implements its own game's post-processing; importing it wholesale would not implement Transformers' shadows, foliage or reflections.

In the pinned PCSX2 source, `GSRendererHW::Draw`/`DrawPrims` prepare texture-cache targets and a `GSHWDrawConfig` for `GSDevice::RenderHW`. That structure takes CPU `GSVertex` arrays containing already transformed GS XYZ/ST/RGBAQ values. GPU mesh transforms therefore need an explicit native vertex-buffer/constant path, plus a conservative way to supply bounds and texture dependencies to the existing renderer. Merely invoking `RenderHW` with the original mesh cannot provide that.

Exit gates remain distinct: measured CPU savings, picture equivalence at base scale, actual high-resolution scene detail, preserved mixed native/emulated ordering and readbacks, and real-time gameplay equivalence at 60 Hz. Native geometry alone does not fix the outstanding simulation-timing differences.
