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
whitespace checks. Implementation and staging were completed before testing.
The subsequent single authorized level measurement is recorded below.

Private provenance is in `hwgs-mesh-staging-01/build.json` and the candidate's
`local-build.json`. The renderer SHA-256 is
`e94264c6a3bcec5bd85300fa9e120cd4d919f56a9783fc5ef76e691cae5908cb`.
The executable SHA-256 remains
`a3a8a09440270f5835e07516e326951250eec7dafc2691ff4aaca1413aed2640`.

## Single enhanced level measurement

On 2026-11-05, `level-mesh-staging-windowed-01` ran `game-66` exactly once.
The comparison uses saved `level-quiet-timing-windowed-01` evidence from
`game-65`; no comparison build was launched. Both use the same executable,
saved Amazon route, settings, combined GPU-geometry preset and Normal process
priority. There was no concurrent compilation or sampling profiler. The runtime
ceiling remained 60, with the host cap and VSync off.

The visible window was 1280 x 720, with 2048 x 1788 internal output and Hor+
16:9. Opening movies and the level arrival cinematic were skipped. Measurement
covered fields 2697 through 4976 inside Amazon: three seconds of movement,
three seconds of camera turning, and about 32 seconds stationary. Boot, menus,
loading and cinematics were excluded. This is not a sustained combat test.

| Metric | Saved game-65 | game-66 staging pool |
| --- | ---: | ---: |
| Simulation updates/s | 59.596 | 59.252 |
| Renderer output submissions/s | 59.622 | 59.280 |
| Host frame deliveries/s | 59.622 | 59.280 |
| Simulation p95 / p99 (ms) | 18.693 / 22.375 | 19.517 / 22.715 |
| Simulation maximum gap (ms) | 44.781 | 28.112 |
| Simulation gaps >20 / >25 / >33.333 ms | 57 / 7 / 1 | 83 / 8 / 0 |
| Delivery p95 / p99 (ms) | 18.628 / 21.554 | 18.945 / 21.864 |
| Delivery maximum gap (ms) | 28.075 | 28.922 |
| Delivery gaps >20 / >25 / >33.333 ms | 43 / 5 / 0 | 50 / 3 / 0 |
| Game time / wall time (s) | 38.021 / 38.241 | 38.021 / 38.463 |
| Game-time / wall-time ratio | 0.99427 | 0.98852 |

Both runs recorded 2,280 simulation updates, 2,280 output submissions and
2,280 host acquisitions, with no skipped submissions. These are separate
pipeline counts; this measurement does not establish GPU completion timestamps,
unique scene images or physical scanout. The timestep was unchanged at
0.0166833512 seconds. The route passed with matching player probes and script
counters. Buffered timing remained unwritten during measurement and exported
15,487 records at shutdown with zero drops or write errors. The save seed,
normal settings and normal launcher remained unchanged.

The new path serviced all 1,687 source uploads using two 1 MiB staging pages,
with zero transient fallbacks and no page wraps. Periodic summaries immediately
before the measurement markers recorded 165 additional uploads and zero new
staging allocations during that span. These are periodic observations, not
exact marker-time counter snapshots. The device-local source allocations
remain; only their temporary upload staging allocation was eliminated.

The pool demonstrably reduced staging allocation work, but this run did not
show an overall performance improvement. Average delivery rate and p99 were
slightly worse, and more delivery gaps exceeded 20 ms. The previous isolated
44.781 ms simulation gap did not recur; that alone does not establish a fix.
One comparison cannot establish repeatability or attribute the differences to
the pool. This result does not validate smooth 60 fps.

`game-66` remains an experimental candidate; the normal launcher stays on
`game-65`. No second run, further optimization or automatic retest followed.
The route exercised multiple uploads sharing pages, but did not exercise
page wrapping, a full pool with pending copies or oversized fallback. Those
correctness cases and pixel-level geometry comparison remain unverified.

Private evidence is in `runs/level-mesh-staging-windowed-01/comparison.json`,
`measurement-run.json`, `performance.json` and the raw buffered traces. The
candidate's separate `mesh-staging-qualification.json` records this limited
route result without changing its immutable build provenance.
