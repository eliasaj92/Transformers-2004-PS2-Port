# Slow-frame attribution in the combined enhanced build

The single `level-timed-spikes-01` profile identifies synchronous writes on the
EE/game thread and graphics-driver allocation/synchronization on the renderer
thread as specific targets. It does not justify another general geometry
optimization or establish that one function explains every spike.

## Conditions and limits

This was one saved Amazon route in the enhanced `game-64` build, using the shared
combined preset, static VU, direct D3D12 presentation, GPU geometry paths, FXAA,
16:9, and 2048x1788 internal output. The decorated window was 1280x720. The runtime
ceiling remained 60 Hz; the separate host cap and VSync were off. Opening and
arrival cinematics were skipped, process priority was Normal, no compilation ran
concurrently, and the route passed. The normal launcher still selects `game-63`.

The 30-second profiler timestamped instruction-pointer samples from the three
busiest CPU threads. Renderer symbols were resolved against a map whose `.text`
bytes match the measured DLL. Renderer production intervals and simulation
update intervals were analyzed separately. Matching guest field numbers across
asynchronous stages is not a measurement of causal latency.

The sampler briefly suspended each thread: total suspension was 1.49% of its
window for EE, 0.95% for the renderer, and 0.91% for VU. The largest single EE
suspension was 2.474 ms near field 4192. Thus this is an attribution profile,
not a speedup benchmark, and the nearby 4193 spike is particularly susceptible
to profiling disturbance. Samples identify leaf locations, not full call stacks
or file handles. Per-function milliseconds below are statistical estimates.

## Game-thread writes correlate with simulation spikes

Using complete simulation intervals inside the sampling window:

| Interval group | Intervals | Mean interval | EE samples in NtWriteFile | Estimated write time per interval |
|---|---:|---:|---:|---:|
| Under 17 ms | 1041 | 15.735 ms | 2 / 8398 (0.024%) | 0.004 ms |
| Over 20 ms | 249 | 21.389 ms | 430 / 2723 (15.791%) | 3.378 ms |

`EeScheduler::perfProbeLine` writes a chained `std::cerr` diagnostic on the game
thread every 1,048,576 checkpoints. It is called without an enable flag, including
with `PS2X_PERFSCOPE=0`, and runs before draining the pending VU output in that
checkpoint. There were 413 `[perf]` lines during the measured gameplay route,
approximately one every six fields.

Across the full route, 279 of 299 simulation gaps over 20 ms had a `[perf]` line
in their same or preceding guest field; this was true for only 290 of 1385 gaps
under 17 ms. This strengthens the earlier field-label association with actual
wall-time samples in the write syscall. It still does not prove that every
sampled write was this particular logger: `tfTimingRecord` also performs
buffered CSV writes on the game thread, and other diagnostics can write there.

The new renderer pipeline trace and direct-presentation trace accumulate their
records in memory and write at shutdown. They do not perform per-frame file I/O.

## Renderer spikes have two patterns

Among renderer intervals inside the sampling window:

| Group | Count | Mean interval | Mean input-queue idle time |
|---|---:|---:|---:|
| Under 17 ms | 957 | 16.285 ms | 0.137 ms |
| Over 20 ms, at least 1 ms idle | 47 | 20.759 ms | 3.068 ms |
| Over 20 ms, less than 1 ms idle | 26 | 21.675 ms | 0.096 ms |

The first slow group is partly waiting for upstream work. The second has work
available and spends its interval in renderer processing or internal waits.

Two of the three renderer intervals over 25 ms directly sampled driver resource
management:

- Field 3090 took 27.358 ms with only 0.051 ms queue idle. At offsets 12.970,
  14.949 and 16.951 ms, the renderer was in
  `NtGdiDdDDIWaitForSynchronizationObjectFromCpu`, following a sample in
  `NtGdiDdDDICreateAllocation` at 10.967 ms.
- Field 3121 took 25.774 ms with 0.063 ms queue idle. Samples at 9.288, 13.282
  and 16.291 ms were in `NtGdiDdDDICreateAllocation`; samples also hit the driver
  lock and CPU synchronization calls.

Repeated samples show where the worker was caught, not the exact duration of a
single continuous call. The allocation's resource type and caller remain
unidentified. Texture-pool misses, upload allocations, stream-buffer exhaustion
and explicit readback waits must not be treated as interchangeable explanations.
The third >25 ms interval mixed normal draw, bounds and texture-cache work and
is adjacent to the sampler's largest EE suspension.

GPU completion after publication did not increase on the worst intervals:
11.316 ms and 8.340 ms for the two examples, versus 11.684 ms over the route.
There were no outstanding GL-release fences at the recorded reuse observations.
This argues against another presentation-cap change. It does not exclude GPU
synchronization earlier inside frame construction: the completion marker only
measures work remaining after publication, not total active GPU duration.

## Cadence, separately from attribution

The instrumented route recorded 38.021 seconds of game time in 38.935 seconds
of wall time (0.9765x), 58.534 simulation updates/s, 58.561 GPU output
completions/s and 58.559 host deliveries/s. All 2280 output submissions were
acquired. Simulation p99 was 23.610 ms; host-delivery p99 was 21.429 ms, with a
26.918 ms maximum and three gaps over 25 ms. GPU completion markers do not
constitute a new unique-image or display-scanout qualification.

## Smallest next experiment

Make the recurring checkpoint report explicitly opt-in, retaining checkpoint
accounting, scheduler/drain behavior and errors. Keep timing collection in
bounded memory and write it after the measured interval so the measurement
does not introduce synchronous game-thread writes. Measure this change with
one otherwise-identical enhanced, windowed run, then stop. This was the proposed
experiment at the end of the attribution profile; implementation follows below.

The observed write samples suggest several milliseconds may be removable from
affected simulation updates, but they do not establish the achieved improvement
or promise sustained 60 fps. After that, target the renderer's actual allocation
caller and wait reason with narrow duration records before changing resource
reuse. Do not start another broad geometry rewrite from aggregate sample counts.

Private evidence under `runs/level-timed-spikes-01`: `profile-run.json`,
`focused-diagnosis.json`, `spike-attribution.json`, `pipeline-analysis.json`,
`samples-timed-symbolized.csv`, `pipeline-frames.csv`, the raw CPU/GPU traces,
`sample-targets.json`, and `window-position.json`. The exact symbol-map proof is
`hwgs-frame-pipeline-symbols-01/mapping-proof.json`. No second game run was made.

## Logging fix

`PS2X_EE_PERF_LOG=1` now explicitly enables the recurring `[perf]` report; it is
off by default. Checkpoint counts, idle-skip boundaries, scheduling and VU
draining retain their existing behavior. `PS2X_EETRIM_STATS=1` still enables its
own diagnostic output independently. Error reporting is unchanged.

`PS2X_TF_TIMING_TRACE` retains its CSV schema, but now collects fixed-size records
in a preallocated 262144-record buffer (12 MiB on Windows x64). The buffer is
initialized before the hooks are installed. Recording performs no formatting,
allocation or file writes. When full, it counts dropped records without growing
or flushing during play. The explicit shutdown path writes and closes the file
after joining the game thread, before the runner's `_Exit`; a destructor alone
would not suffice. The final summary reports row count, drops and write errors.

The candidate uses the same enhanced PGO/static-VU objects and renderer as the
saved unprofiled `level-integrated-windowed-01` run. Only `EeScheduler.cpp` and
`ps2_runtime.cpp` are recompiled. The one-run comparison also verifies that the
timing file remains empty during the measured gameplay interval and exports all
recorded rows at shutdown. The timestamped sampling run above is not the
performance reference because its instrumentation adds overhead.

### One windowed run after the fix

`level-quiet-timing-windowed-01` passed using `game-65`. The executable changes
are the two logging translation units; the renderer SHA-256 is identical to
`game-63`. The route, guest counters, player probes, timestep, settings, runtime
enhancement options, window position and internal dimensions match the saved
unprofiled run. There was no concurrent compilation or CPU/GPU sampling.

| Measurement | Saved enhanced run | Logging fix, one run |
|---|---:|---:|
| Simulation updates/s | 59.411 | 59.596 |
| Host deliveries/s | 59.427 | 59.622 |
| Simulation p95 | 21.226 ms | 18.693 ms |
| Simulation p99 | 23.259 ms | 22.375 ms |
| Simulation maximum | 30.305 ms | 44.781 ms |
| Simulation gaps over 20 ms | 209 | 57 |
| Simulation gaps over 25 ms | 13 | 7 |
| Delivery p99 | 21.896 ms | 21.554 ms |
| Delivery maximum | 28.532 ms | 28.075 ms |
| Delivery gaps over 25 ms | 9 | 5 |
| Game time / wall time | 38.021 / 38.360 s | 38.021 / 38.241 s |

Frequent simulation spikes decreased substantially in this observation, while
the delivery tail improved modestly. One simulation outlier was worse, exceeding
33.333 ms; no delivery gap exceeded that threshold. This does not establish
repeatability or sustained 60 fps, and the new outlier's cause was not profiled.
Both runs submitted and acquired 2280 complete-frame outputs with no skips;
these are host handoff counts, not independent GPU-completion or scanout counts.

There were zero recurring `[perf]` lines. The timing file stayed at zero bytes
in all 19 observations during the measurement, then exported all 15460 rows
after the game thread stopped, with zero drops and zero write errors. The buffer
and logging change therefore behaved as intended. No second test was launched.

Private evidence: `runs/level-quiet-timing-windowed-01/{comparison,measurement-run,combined-result,performance,window-position}.json`,
the raw timing/presentation CSVs and logs, and
`quiet-timing-build-01/build-summary.json`. The previous unprofiled run remains
the comparison; the instrumented spike profile is not used for gain estimates.
