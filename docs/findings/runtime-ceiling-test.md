# Enhanced runtime ceiling diagnostic, 2026-11-07

One windowed Amazon test raised the runtime ceiling from 60 to **1,000 Hz**.
The working enhanced build delivered **57.95 frames/s** and performed **57.93
gameplay updates/s** over the measured route. This run did not expose sustained
throughput above 60. The normal launcher, settings and save seed were unchanged.

## Configuration

The existing executable accepts `PS2X_REALTIME_HZ`, so no rebuild or executable
patch was necessary. The diagnostic used the exact same `game-67` accelerated
PGO/static-VU executable and renderer as the saved capped measurement. The
rejected non-grass batching experiment was not enabled.

The only runtime-option change, applied after the combined enhanced preset:

```text
PS2X_REALTIME=1
PS2X_REALTIME_HZ=1000
```

This removes the **60 Hz** ceiling for the test, not every possible pacing or
synchronization mechanism. A 1 ms runtime slot remains. The startup log confirms
`[realtime] cap ON (PS2X_REALTIME) fieldUs=1000 (NTSC) catchUpMs=50`.
`PS2X_REALTIME=0` would instead select legacy host pacing; it is not an uncap.

Host presentation cap and VSync were already off. The host log confirms a zero
target frame interval. Guest cycles per field, one-field game synchronization,
the simulation timestep of 0.0166833512 seconds and frame-ready synchronization
remain unchanged. This is a throughput diagnostic, not correctly timed unlocked
gameplay or 120 FPS support.

The game ran in a visible bordered 1280 x 720 window with 2048 x 1788 internal
output, Hor+ 16:9, direct GPU presentation and Normal process priority. Opening
and arrival cinematics were skipped. No menu checks, original build, comparison
relaunch, concurrent compilation, graphics validation or sampling profiler were
used. Timing data remained buffered until shutdown, with zero dropped records.

## In-level result

Guest fields 2697 through 4976 cover 2,280 updates: movement, a short stationary
interval, a camera turn and a longer stationary interval. Loading and menu
navigation are excluded. The script passed with matching player probes, route
counters and timestep against the saved enhanced run.

| Measurement | Saved 60 Hz run | 1,000 Hz diagnostic |
| --- | ---: | ---: |
| Gameplay updates/s | 59.513 | 57.934 |
| Render-output submissions/s | 59.539 | 57.953 |
| Host frame deliveries/s | 59.539 | 57.955 |
| Delivery p95 | 18.410 ms | 21.657 ms |
| Delivery p99 | 20.107 ms | 26.256 ms |
| Maximum delivery gap | 28.956 ms | 34.573 ms |
| Delivery gaps above 25 ms | 1 | 38 |
| Delivery gaps above 33.333 ms | 0 | 1 |
| Game time over timed update intervals | 38.021 s | 38.021 s |
| Wall time over those intervals | 38.294 s | 39.338 s |
| Game-time/wall-time ratio | 0.99288 | 0.96654 |

The diagnostic recorded 2,280 render-output submissions and 2,279 host
acquisitions in the measured field range. The unacquired output was the final
measured field, 4976, before shutdown; there were no missing interior acquisitions.
These are separately recorded pipeline
events, not proof of unique scene images or physical monitor scanouts.
The host-loop FPS overlay is not used as the gameplay rate.

| Diagnostic phase | Host deliveries/s |
| --- | ---: |
| Movement | 52.704 |
| Stationary before camera turn | 55.538 |
| Camera turn | 55.966 |
| Final stationary interval | 58.924 |

## Limits on the comparison

The desktop display changed between the saved measurement and this run:
Windows reported `DISPLAY1`, 1920 x 1080, previously and `DISPLAY6`,
2560 x 1440, now. The game window, internal resolution, settings file, executable,
renderer, route and process priority match, but this is **not a controlled paired
speedup measurement**. There was exactly one new game run and no fresh capped
comparison, as requested. The difference from the saved result must not be
attributed solely to relaxing the limiter.

The test does establish that the configured 60 Hz runtime ceiling was removed
and that this route still did not sustain 60 updates or deliveries per second.
It does not identify which current CPU work, GPU work or synchronization wait
accounts for the remaining time. The historical renderer profile remains useful
evidence, not a fresh attribution of this run. No further optimization or game
run was started after the measurement.

## Private evidence

All raw data and saves remain outside the source checkout in
`runs/level-runtime-ceiling-windowed-01`:

- `comparison.json` and `combined-result.json`: independently calculated rates,
  game-time/wall-time, phase timings and comparison limitations.
- `measurement-run.json`: input hashes, one-run limit and protected-file checks.
- `env.txt`, `stderr.log`, `stdout.log`, `window-position.json`: applied settings,
  limiter confirmation, route markers and window/display evidence.
- `timing.csv`, `presentation.csv`, `presentmon.csv`: separate timing streams.

The inherited private helper had a hardcoded runtime-60 description in its
summary. That description was corrected from the startup log and environment;
numeric measurements were unchanged. The original summary is preserved as
`performance-wrapper.json` alongside corrected `performance.json`.

Executable SHA-256:
`a3a8a09440270f5835e07516e326951250eec7dafc2691ff4aaca1413aed2640`.
Renderer SHA-256:
`1fb2a366c09662ab406603d762fde2891e5a19673ab6d4c81d23bbf2f0bbb914`.
