# Engine-level native renderer

Updated 2026-11-05 following the user's request for a general native rendering
architecture and headroom above 60 fps. This supersedes further grass/packet
micro-optimizations. The first offscreen whole-mesh prototype is now implemented:
an original Amazon rock and its named texture render through a shared native
mesh/material/snapshot interface, with no GS frontend. Four 1080p diagnostic
views passed in one Release D3D12 probe invocation. A subsequent single enhanced,
windowed Amazon run exercised the same native pass on the game's actual device
and queue. Its separate targets passed the same reference checks, preserved the
game's completed output byte-for-byte, and allowed gameplay to continue.
See [implementation and evidence](../../deps/native_renderer/README.md).
Replacing live world submissions and any performance gain remain unproven.

## The boundary must move upstream

The current enhanced path offloads geometry calculations but still attaches
owned source data to individual PATH1 packets. `gsnative::DualPacket` represents
at most 28 dual-transform vertices, 22 reflection vertices or eight grass
sprites. Rendering still processes GIF prefixes, prepares GS-compatible traces,
calls `GSRendererHW::DrawNativeTriangles`/`DrawNativeSprites`, looks up emulated
textures and render targets, and restores GS latches. Reflection repeats this
across four sections of each packet. Larger groups reduce some costs but do not
replace that architecture.

The earlier detailed profile attributed about 29.45% of renderer samples to
native-geometry CPU preparation, 17.56% to draw/device setup, 13.57% to texture,
palette and guest-VRAM handling, and 9.28% to GIF/GS state. Those are historical
sample shares, not current timings or additive guaranteed savings. They support
targeting the entire CPU rendering path rather than another shader-family loop.

The desired path is:

```text
Game's mesh/material/pose submission
    -> owned scene snapshot + persistent mesh/texture resources
    -> native D3D12 passes and shaders
    -> existing direct GPU presentation
```

Covered submissions should not traverse VIF execution, VU execution, GIF
decoding, GS primitive tracing or per-packet VRAM/texture reconstruction. A
parser may import a static authored VIF list as an asset format once; interpreting
that list every frame and emitting GIF would not satisfy this objective.

This follows the engine-boundary target in
[BT3's design](https://github.com/z3xox/BT3-Recomp/blob/main/docs/NATIVE-RENDER-SEAM.md).
That document distinguishes its GIF-output intermediate stage from native
rasterization. Its engine layouts and completion status are not evidence of
Transformers support.

## Concrete entry points found

Static inspection of the supported USA executable and generated code confirms:

- `0x260a70` performs visibility checks using the record at argument 2 + `0x20`,
  selects context state using record + `0x08` (including an indexed case using
  + `0x0c`), conditionally updates shader contexts, and emits the DMA CALL for
  the mesh list at record + `0x14`. It also updates/restores guest state, so
  replacing the entire function with an early return would be incorrect.
- `0x23aa90` walks the shader-context pointers at scratchpad `0x700005f0`, with
  the count at `0x70000604`. It uses dirty state and invokes virtual method
  + `0x0c`. Unchanged contexts can be reused without invoking a writer, so a
  new renderer must retain complete effective state, not just recent uploads.
- `0x26f790` is another CALL/MSCAL/FLUSH emission helper. `0x221b80` obtains a
  list through virtual method + `0x10` and appends its call. Their relevant live
  coverage still needs attribution.

The retained type names and RTTI/vtable chains identify these candidate context
writer slots among 18 mapped types:

| Context type | Vtable | Method +0x0c |
| --- | --- | --- |
| `vlShader_DualTransform` | `0x6b3c70` | `0x248660` |
| `vlShader_TransformClip` | `0x6b43b0` | `0x262000` |
| `vlShader_MeshEnvelope` | `0x6b3a30` | `0x242120` |
| `vlShader_MeshVertexTween` | `0x6b3bd0` | `0x247880` |
| `vlShader_ConstantMaterial` | `0x6b3a10` | `0x241f20` |
| `vlShader_ConstantMaterialWithVertexColor` | `0x6b3a00` | `0x241d30` |
| `vlShader_TextureTransform` | `0x6b3b00` | `0x243570` |
| `vlShader_LightQuadric` | `0x6b3720` | `0x23b500` |
| `vlShader_AlphaFog` | `0x6b3bf0` | `0x247a20` |
| `vlShader_InstanceGroup` | `0x6b2dd0` | `0x215970` |
| `vlShader_DualTransformInstancer` | `0x6b4a70` | `0x2744d0` |

These are static candidates, not recovered parameter layouts or proof that each
type appears on the chosen level. All three existing whole-list EE snapshots
have just `vlShader_Grass` active. They cannot qualify a non-grass whole-mesh
replacement. The existing source probe itself filters completed mesh submissions
through a grass context; this capture bias must be removed for the new work.
The private audit is `C:/Users/dounh/tf-work/engine-native-seam-audit-01.json`.

## Shared renderer design

1. **Persistent assets:** import complete mesh lists into native vertex/index
   buffers, resolve textures and materials, and cache by asset identity plus
   generation. Loader changes, palette changes, dynamic geometry and reused
   guest addresses invalidate the affected resources. No per-packet content
   hashing or reconstruction after a resource is known unchanged.
2. **Owned scene submissions:** capture stable instance IDs, resource handles,
   model/camera matrices, light/material parameters and bone/morph state into
   immutable frame snapshots. No live guest pointers cross to the renderer.
   Static world meshes, actors and effects share this interface; specialized
   shader math does not require a separate compatibility pipeline.
3. **Native passes:** use the current D3D12 device with explicit depth, blend,
   texture and target state. Cache pipeline/material bindings and submit whole
   meshes/instances. Preserve transparent and feedback ordering; reorder only
   where proven safe. Instancing and later indirect submission apply here.
   [D3D12 indirect drawing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing)
   can move command generation/culling to the GPU, but cannot remove the current
   compatibility costs by itself.
4. **Bounded legacy integration:** unsupported work retains the existing enhanced
   renderer. Hybrid passes need explicit shared color/depth ownership, texture
   visibility and resource barriers at real pass boundaries. An old completed
   image cannot simply be overlaid where objects must depth-test against native
   geometry. Replace a complete proven ownership interval; preserve guest
   side effects and later VU-state dependencies before suppressing its DMA.
5. **Independent presentation:** retain direct GPU output, but allow native scene
   rendering to have its own cadence. Today's one-output-per-`RawVsync` model
   cannot provide independent higher-rate scene rendering.

## First prototype and acceptance

The implemented first stage imports the entire authored `ROCK_J3.VBIN` mesh and
its explicit `Jungle_Cliff` texture binding from the private USA disc. It owns all
87 vertices/168 triangles and reuses persistent GPU buffers across camera/model
updates. Its 1080p native shader output passes independent projection, texture,
clipping and depth checks. This establishes the replacement interface without
another game capture. It does not recover a live effective shader context or
claim a match to a captured game frame.

Before live integration, recover one coherent non-grass submission and its
effective camera/material state. Compare against its matching captured reference
and establish shared target/depth ownership. Only then suppress the corresponding
guest submission with its required state/ordering preserved. If new evidence is
needed, use one bounded enhanced, windowed, cutscene-skipping level capture;
this is correctness work, not a benchmark or a test of the original build.

Acceptance requires a textured non-grass mesh, reuse across camera/transform
updates, matching topology/material/depth behavior, and no VIF/VU/GIF/GS frontend
calls for the native draw. A renamed packet wrapper, an empty renderer API or
another per-packet shader optimization does not meet this milestone. The
existing small GPU kernels, reference comparisons and direct-present code are
reusable tools, not the ownership boundary.

Expected benefit: eliminate the complete compatibility path for covered mesh
submissions, and make rendering from a retained scene possible. Coverage and
measured CPU/GPU costs must establish the actual speedup. No numerical FPS gain
is yet supportable.

## Target beyond 60

Use 120 Hz rendering, an 8.33 ms interval, as the initial architectural target.
This is a design budget, not a hardware performance promise. Measure CPU scene
production, GPU pass completion and delivered frames separately against it.
Current 60-capped benchmarks cannot establish the maximum native renderer rate.

Keep the validated simulation timestep, currently 0.0166833512 seconds, while
rendering from previous/current snapshots. Interpolate camera and object
transforms, then bone/morph poses as those representations are recovered.
Teleports, spawns, scene changes and topology changes require explicit history
resets. Rendering the same image twice or changing the game timestep is not
higher-rate gameplay rendering. The current two-field projection convention
also needs to become a validated progressive camera projection.

The first native mesh prototype is intentionally smaller than this cadence
change: prove the replacement boundary and output before building independent
render scheduling. Further grass tuning and the proposed lazy fallback-dispatch
optimization are deferred.
