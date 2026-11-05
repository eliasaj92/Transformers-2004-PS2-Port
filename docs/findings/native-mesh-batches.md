# Resident non-grass mesh batching: rejected performance experiment

**Do not enable this candidate for normal play.** Its one enhanced Amazon run
delivered 54.78 frames/s versus 59.54 in the saved previous enhanced run.
Correctness checks passed, but the measured result does not justify promotion.
The implementation is archived on `codex/native-mesh-batch-experiment`; the
working `codex/gpu-geometry` branch retains the preceding renderer.

## Change and intended benefit

The candidate caches immutable non-grass vertex data and triangle-strip indices
in batches of up to 64 adjacent compatible packets. It uploads a resident source
buffer once, updates transforms per submission, and calculates bounds once for
adjacent chunks sharing transforms. Whole-source attribute bounds are cached.
This targets repeated CPU preparation observed in earlier renderer samples.

`PS2X_TF_GPU_DUAL_BATCHES=1` selects the candidate alongside the existing
`PS2X_TF_GPU_GROUPS=8` and `PS2X_TF_GPU_DUAL_MERGE=1` options. The default is off.
The test combines it with the previous grass, reflection, timing and direct
presentation enhancements. It does not change the timestep or pacing.

Cache hits require exact source bytes, vertex counts and strip restart masks;
hash equality alone is insufficient. Camera matrices are dynamic. The cache
holds at most 512 entries, with GPU buffers retired using the existing fence
tracking. The existing guards for field overlap, texture/CLUT feedback, source
bounds, alpha ranges and depth witnesses remain. Unsupported groups fall back
to the existing enhanced path. Final GS vertex state is restored.

This is a packet-to-mesh bridge, not an engine object interface. GS material,
texture and render-target preparation still run. It does not connect the
standalone native mesh prototype to a live scene draw, and does not establish
engine-level ownership of an entire mesh.

## Correctness evidence

The exact Release candidate DLL passed the captured-mesh test:

```powershell
python -B testing/test_gpu_dual_transform.py draw `
  --prepared <private-qualified-mesh-directory> `
  --dll <private-game-69-directory>/tf_hwgs.dll `
  --report <fresh-private-report.json> --frontend --merge --mesh-batches
```

The fixture contains 740 captured chunks and 17,596 source vertices. Five modes
exercise texture, depth, alpha/blending, mipmaps and forced materialization.
Deferred coordinates are poisoned before native submission. There were zero
differences across 18,350,080 full-resolution pixels and the GS-memory checks.
The diagnostic passed 210 final-state comparisons, 13 rejection/cache identity
checks and 36,267 VRAM-range checks. It exercised 64-packet batches, 171 cache
hits, 44 uploads, changed source positions, changed strip restarts and changed
camera transforms. This is bounded rendering correctness, not campaign proof.

## One in-level measurement, 2026-11-05

Exactly one windowed `game-69` run was made, compared with the saved `game-67`
run. No original build or previous comparison build was launched. Both use the
same accelerated PGO/static-VU executable, saved Amazon route, settings, Normal
priority, 1280 x 720 bordered window, 2048 x 1788 internal output, Hor+ 16:9 and
direct GPU presentation. The runtime ceiling is 60 Hz; host cap and VSync are
off. Cinematics are skipped. No concurrent compilation, sampling profiler,
graphics validation, host mesh probe or frame-pipeline profiler was used.

Fields 2697 through 4976 cover 2,280 gameplay updates: movement, a stationary
interval, a camera turn, then a longer stationary interval. Boot, loading and
menu navigation are excluded. The route, final player probes and update
counters match. Timing data remained buffered during measurement, with zero
dropped records. Normal settings, launcher and seed saves remained unchanged.

| In-level measurement | Saved enhanced build | Candidate |
| --- | ---: | ---: |
| Gameplay updates/s | 59.513 | 54.775 |
| Render-output submissions/s | 59.539 | 54.777 |
| Host frame deliveries/s | 59.539 | 54.777 |
| Delivery p95 | 18.410 ms | 24.550 ms |
| Delivery p99 | 20.107 ms | 27.999 ms |
| Maximum delivery gap | 28.956 ms | 41.965 ms |
| Delivery gaps over 25 ms | 1 | 102 |
| Delivery gaps over 33.333 ms | 0 | 2 |
| Game time represented by update intervals | 38.021 s | 38.021 s |
| Wall time for those updates | 38.294 s | 41.606 s |
| Game-time/wall-time ratio | 0.99288 | 0.91384 |

Each configuration has 2,280 output submissions and 2,280 deliveries, with no
skipped submissions. These counters distinguish simulation and output cadence;
they do not prove every stationary image is visually unique. The host loop's
FPS counter is not used as a gameplay measurement. The earlier debug-enabled
`game-68` native host probe is not a performance comparison.

## Diagnosis and decision

Periodic counter snapshots nearest the level markers show approximately
754,458 versus 754,474 non-grass GPU draws. Merged groups remain approximately
42,266 versus 42,267. Rejected merge attempts rise from 86,981 to 167,100.
These snapshots precede the exact markers, so small count differences are not
interpreted as changes in scene coverage.

The candidate successfully reuses sources, but does not reduce the level's
draw volume. It introduces another preparation attempt before many draws
fall back. This is consistent with the observed slowdown; the single
unprofiled run cannot attribute every lost millisecond to that work or establish
repeatability. The result is sufficient to reject promotion, not to claim a
processing speedup from reduced dispatch counts.

The next architectural experiment should bind one engine mesh instance to an
owned camera/material snapshot and replace its live draw through the native
renderer. That accepted draw needs to bypass GS frontend preparation while
preserving shared depth, target ownership and readbacks. Increasing the packet
batch limit again is not supported by this evidence. No further implementation
or gameplay run followed this measurement.

## Private evidence and provenance

Assets and captures remain outside the repository. Evidence names:

- `native-mesh-batches-probe-01.json` and its log: correctness check.
- `runs/level-native-mesh-batches-windowed-01/comparison.json`: measurements,
  matching-input checks, counters and rejection decision.
- `measurement-run.json`: one-run limit, hashes and protected-input checks.
- `timing.csv`, `presentation.csv`, `presentmon.csv`: separate timing streams.
- `hwgs-native-mesh-batches-01/build.json`: exact source/object/link provenance.

Unchanged executable SHA-256:
`a3a8a09440270f5835e07516e326951250eec7dafc2691ff4aaca1413aed2640`.
Candidate renderer SHA-256:
`d6fdee51820da373fa1ae4a478d272eed3f86c1f977caadecf87d9d62d08d1aa`.

Unrelated reflection-cache and frame-profiler working changes were not included
in the experimental commit. The frame profiler was excluded from the tested
DLL; renderer source changes against the preceding build were limited to the
five mesh implementation/probe files recorded in its build manifest.
