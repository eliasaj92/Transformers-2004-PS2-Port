# Reusable staging pages for mesh uploads

New immutable meshes previously created a device-local buffer and a temporary
upload buffer on every source-cache miss. The temporary buffer was mapped,
copied, unmapped and handed to deferred destruction for every mesh. The saved
enhanced level traces showed additional source uploads during the measured
route, and the earlier spike profile sampled driver allocation/synchronization.
Those samples did not identify the exact resource responsible for each stall.

The implementation now shares persistently mapped staging pages across mesh
uploads. Each page is 1 MiB, with at most eight pages retained. Pages are created
on demand, and each upload uses a distinct range aligned to 256 bytes. Appending
to unused space is allowed while earlier ranges are in use; wrapping a page
requires its last copy fence to have completed. The pool checks the device's
known completion value without introducing a CPU or GPU wait.

Uploads over 1 MiB, or uploads for which all eight pages lack safe space, retain
the existing transient-buffer path. A page-allocation or mapping failure
disables further pool attempts and falls back to transient uploads. The error
is reported once at the failure. Pool destruction defers resources whose copy
fences are still outstanding. Device-local mesh allocation, immutable mesh
identity, draw order, source-cache invalidation and geometry calculations retain
their previous behavior.

The reusable path removes the per-mesh staging resource creation, map/unmap and
deferred-release entry when a page has space. It does not eliminate the
device-local mesh allocation or establish a frame-time improvement by itself.
Retained staging memory is bounded at 8 MiB; transient fallback memory follows
the pre-existing lifetime rules.

`PS2X_TF_GPU_STAGING_POOL=0` restores transient staging for all mesh uploads.
Reuse is enabled by default in the new renderer. The existing GPU-dual summary
includes `staging_allocations` (pages created), `staging_uploads` (copies through
pooled pages), `staging_reuses` (completed pages wrapped) and
`staging_fallbacks` (transient uploads). There is no additional periodic log call.

## Build status

The enhanced candidate is staged privately as `game-66`, retaining the exact
`game-65` executable, its logging fix, and the combined GPU-geometry features.
The presentation wrappers match the committed `game-65` versions; the unrelated
uncommitted frame-pipeline profiler is omitted from this candidate. The normal
play launcher still selects the measured `game-65` build.

The renderer compiled and linked successfully, and the source diff passed
whitespace checks. No game, synthetic GPU probe, profile or benchmark was
launched for this change, following the user's request to implement an
improvement before testing again. Runtime correctness, observed allocation
reduction and FPS benefit remain unmeasured.

Private provenance is in `hwgs-mesh-staging-01/build.json` and the candidate's
`local-build.json`. The renderer SHA-256 is
`e94264c6a3bcec5bd85300fa9e120cd4d919f56a9783fc5ef76e691cae5908cb`.
The executable SHA-256 remains
`a3a8a09440270f5835e07516e326951250eec7dafc2691ff4aaca1413aed2640`.

The next correctness check should exercise several distinct source uploads in
one page, page wrapping after completion, a full pool with pending copies,
oversized fallback and shutdown with outstanding work. A subsequent enhanced
windowed level run can then measure the staging counters and frame-time tail.
