# Prepared native geometry batches

The grass renderer now has an opt-in command batch that prepares each owned
packet once, merges consecutive compatible runs, and reuses the preparation
when a draw must fall back. This is the first bounded implementation of larger
native geometry batches with less repeated PS2 bookkeeping. It operates on
grass; triangle and reflection rendering retain their existing paths.

## Architecture and safety

Previously, an unsuccessful 64-packet merge recursively split in half. Each
attempt repeated packet checks and source bounds preparation, before reaching
the eight-packet compute fallback or individual packet draws. The saved enhanced
level measurement recorded 5,464,525 bounds queries for 4,300,829 input packets,
including the 182,534 packets rejected by source bounds.

The new GS-thread callback authenticates all owned packet framing before
consuming state. It prepares source constants, raster state, conservative bounds
and incoming color once per packet. Preparation records include rejected bounds.
They live only for this immutable command's callback, with identity checks and
an incoming-color check before reuse; they are not a cross-frame pointer cache.

Within the existing 64-packet queue limit, consecutive valid packets with the
same SCISSOR, XYOFFSET and PRIM form a run. Invalid bounds and state changes
end a run instead of forcing arbitrary binary splits. The existing merge guards
still check disjoint fields, target aliasing, texture/CLUT feedback, depth
witnesses and source-derived traces. Accepted merges retain primitive order
within each field and restore the final GS latches.

When merging is unsafe, up to 64 prepared packets can share one packet-major
geometry compute dispatch, while their original draws execute in order. Failed
GPU allocation/dispatch preparation retains individual draws. Rejected source
bounds replay the fully materialized original packet without repeating the
bounds calculation. Existing per-field materialization and draw fallback remain.
Nothing changes the simulation timestep, field wait or presentation limiter.

Enable `PS2X_TF_GPU_GRASS_BATCHES=1` alongside the combined enhanced preset's
`PS2X_TF_GPU_GROUPS=8`, `PS2X_TF_GPU_GRASS_GROUPS=64` and
`PS2X_TF_GPU_GRASS_MERGE=1`. Unset or set the new flag to `0` to retain the
previous scheduler. It is deliberately not in the normal-play preset yet.
Counters append to the existing periodic grass summary; no new logging call
or per-frame trace was added.

## Correctness check

The renderer compiled and linked with the existing MSVC Release configuration.
The exact candidate DLL passed `testing/test_gpu_grass_packets.py --batches`
using 512 authenticated enhanced capture packets at 4x resolution. Four modes
exercise ordinary merging, overlapping fields requiring ordered separate draws,
state boundaries every 13 packets with intermittent invalid scissors, and
forced CPU materialization. Deferred coordinates are poisoned before native
submission to catch unintended reads.

The diagnostic found zero differences across 14,680,064 full-resolution pixel
comparisons and passed 32 final-latch comparisons, 16 malformed-command/guard
checks and 36,267 VRAM range checks. It exercised 64-packet merged draws,
64-packet compute-only groups and 64 rejected-bounds fallbacks. Every command
was limited to at most one source-bounds query per packet. This checks the
production batch scheduler against the existing enhanced GIF reference path;
it does not establish full-campaign correctness or measure gameplay speed.

## Single enhanced level measurement, 2026-11-05

Exactly one new game run, `level-native-batches-windowed-01`, compared `game-67`
with the saved `game-66` staging-pool run. No comparison build was launched.
The executable, route, settings, Normal process priority and combined enhanced
features match; the new renderer and its batch flag are the candidate changes.
The unrelated uncommitted frame-pipeline profiler was excluded from the DLL.
There was no concurrent compilation or sampling profiler.

The game used a visible 1280 x 720 window, 2048 x 1788 internal output, Hor+
16:9 and direct GPU presentation. The runtime ceiling stayed at 60, with the
host cap and VSync off. Opening and arrival cinematics were skipped. Fields
2697 through 4976 were measured inside Amazon, excluding boot, loading and
menus: three seconds moving, two stationary, three turning the camera and
30 stationary. This is a short route, not sustained combat.

| Metric | Saved enhanced game-66 | Prepared batches game-67 |
| --- | ---: | ---: |
| Simulation updates/s | 59.252 | 59.513 |
| Renderer output submissions/s | 59.280 | 59.539 |
| Host frame deliveries/s | 59.280 | 59.539 |
| Simulation p95 / p99 gap (ms) | 19.517 / 22.715 | 18.375 / 21.286 |
| Simulation maximum gap (ms) | 28.112 | 26.075 |
| Simulation gaps >20 / >25 / >33.333 ms | 83 / 8 / 0 | 38 / 5 / 0 |
| Delivery p95 / p99 gap (ms) | 18.945 / 21.864 | 18.410 / 20.107 |
| Delivery maximum gap (ms) | 28.922 | 28.956 |
| Delivery gaps >20 / >25 / >33.333 ms | 50 / 3 / 0 | 24 / 1 / 0 |
| Game time / wall time (s) | 38.021 / 38.463 | 38.021 / 38.294 |
| Game-time / wall-time ratio | 0.98852 | 0.99288 |
| Movement deliveries/s | 55.260 | 56.072 |
| Camera-turn deliveries/s | 57.752 | 59.066 |
| Final stationary deliveries/s | 60.082 | 60.071 |

Both runs recorded 2,280 simulation updates, 2,280 renderer output submissions
and 2,280 host acquisitions, with zero skipped submissions. These are separate
pipeline observations, not GPU completion timestamps or physical scanout counts.
The timestep stayed 0.0166833512 seconds. The route passed with matching player
probes and script counters. Buffered timing wrote nothing during measurement
and exported 15,464 records on shutdown, with no dropped records or write errors.
The save seed, normal settings and normal launcher were unchanged. Visual review
of the saved final level images found no obvious missing grass or scene geometry;
the gameplay screenshots are not a pixel-exact equivalence test.

The periodic grass summaries preceding the route markers recorded:

| Work counter | Saved game-66 | Prepared batches | Change |
| --- | ---: | ---: | ---: |
| Accepted grass packets | 4,118,295 | 4,118,295 | Same |
| Source-bounds rejected packets | 182,534 | 182,534 | Same |
| Bounds queries | 5,464,525 | 4,300,829 | -21.3% |
| Geometry compute dispatches | 162,336 | 108,622 | -33.1% |
| Successful native draw submissions | 311,732 | 172,279 | -44.7% |
| Packets rendered through merged groups | 3,868,458 | 3,989,401 | +3.1% |
| Mean packets per successful merged group | 39.87 | 46.50 | +16.6% |

These are deltas between periodic summaries, not exact marker-time snapshots.
Native draw counters exclude ordinary fallback draws and do not count every
low-level API draw. The new path prepared exactly 4,300,829 packets and made
exactly that many bounds queries, including negative results. Repeated bounds
preparation was removed from this measured path. The input packet and rejection
counts match the saved run.

The single run showed fewer frequent delivery spikes and a modestly higher
delivery rate; the maximum delivery gap did not improve. It cannot establish a
repeatable or causal speedup. Movement still falls below 60, and the long
stationary interval dominates the overall average. Smooth sustained 60 fps has
not been achieved. The change remains opt-in, retained as the prepared-command
foundation with the above correctness and single-route evidence. The normal
launcher still selects `game-65`.

## Remaining work identified by this result

The subsequent user direction prioritizes the
[engine-level native renderer](engine-native-renderer.md). The fallback-dispatch
experiment below is recorded as a finding and is deferred, not the next task.

The present trace does not re-profile the critical CPU/GPU stage. It shows that
moving through the scene still misses the delivery budget despite the 60 Hz cap;
it cannot attribute the remaining time to a specific function. The earlier
renderer profile identifies CPU geometry preparation as a substantial cost, but
its sample shares must not be presented as new measurements of this candidate.

One concrete inefficiency is now isolated: 22,483 compute-only fallback groups
covering 113,527 packets produced only one successful grouped native draw in
the periodic measurement span. Most of that dispatch preparation did not lead
to a native draw. The smallest subsequent experiment would defer a group's
compute dispatch until its first accepted GPU draw callback, retaining the same
prepared inputs and ordinary fallback. Its expected direct benefit is avoiding
unused dispatch/upload work, not a defensible numerical FPS gain yet. It should
be a separate opt-in change with a correctness check and, if requested, one
new level measurement. No such change or additional run was made here.

Reflection packets were considered before choosing grass. In 706 saved enhanced
packets, an optimistic exact-screen-bounds overlap check admitted just 63 packets
into disjoint multi-packet runs; conservative live bounds could admit fewer.
Their four alternating textured sections cannot simply be reordered across
overlapping packets without proving depth/blend equivalence. This change does
not relax those ordering requirements.

## Private provenance

The staged candidate is `game-67`. Renderer SHA-256:
`1fb2a366c09662ab406603d762fde2891e5a19673ab6d4c81d23bbf2f0bbb914`.
The unchanged executable SHA-256 is
`a3a8a09440270f5835e07516e326951250eec7dafc2691ff4aaca1413aed2640`.

Private artifacts under `C:/Users/dounh/tf-work` include
`hwgs-native-batches-01/build.json`, `native-batches-probe-01.json`,
`reflection-batch-feasibility-01.json`, and
`runs/level-native-batches-windowed-01/{measurement-run,comparison}.json`.
The candidate's separate `native-batches-qualification.json` records the limited
qualification without rewriting its immutable build provenance. Captured game
assets, screenshots, generated code, saves and binaries are not committed.
