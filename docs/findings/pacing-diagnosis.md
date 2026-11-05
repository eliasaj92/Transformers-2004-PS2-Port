# Pacing versus processing diagnosis — 2026-11-05

The current `game-66` measurement still uses a 60 Hz runtime ceiling. Its
59.28 deliveries/s is not an uncapped-capacity measurement. See the
[current-build audit](#current-build-audit-after-the-staging-pool-test) for the
source trace, the movement/stationary split and a new analysis of saved samples.
The relaxed-cap benchmarks below are historical `game-62` results, not new tests
of the logging fix or staging pool.

The approximately 58 FPS result has two causes. The CPU production pipeline has
little throughput above 60 updates/s, and the independent 60 Hz host poll loses
or delays some of the frames it produces. Relaxing caps does not reveal 120 FPS
headroom. It also speeds up game time, so it is a throughput diagnostic, not
correct unlocked gameplay.

The initial twelve-run diagnosis introduced no micro-optimization or normal-play
setting. It used the same enhanced, accelerated, static-VU `game-62` executable
and renderer throughout. The follow-up below tests a small, default-off
frame-ready wait in `game-63`, with the same executable and a new renderer DLL.
Every comparison uses one binary pair for both conditions. Original builds were
not run, and the normal launcher and settings remain unchanged.

Both stages include the provisional shared reflection-field cache held constant
in these tests. This investigation does not qualify that cache's performance
benefit or infer one from its reduction in arithmetic work.

## Controls and measurement boundaries

- Six alternating runtime-cap runs: scheduler 60/240 Hz, effective host cap
  fixed at 60 Hz. Then six alternating host-cap runs: host 60/240 Hz, scheduler
  fixed at diagnostic 240 Hz. Each intervention has three capped/relaxed pairs.
- Same saved Amazon route, save seed, inputs, Normal process priority, enhanced
  GPU geometry settings, direct D3D12/OpenGL presentation, 4x internal rendering
  (2048x1788), 1920x1080 window and Hor+ 16:9. All cheats off. Opening and level
  cinematics skipped. No menu checks and no concurrent compilation.
- Windows were visible, at the same position entirely on the primary monitor.
  Earlier captures straddled monitors; their display-event rates are not used
  as controls here. Both detailed profile windows were foreground and unoccluded.
- Executable SHA-256:
  `fec75007e8df6e2000984cfa63dc657d89041396bbb3f23d81509917caf0dc4a`.
  Renderer SHA-256:
  `e1a313d76e17d0d66d53012490f97c30310946774a44c50237a09e0d259fabab`.
  Manifests also pin settings, route, launcher, save seed and tools.
- Image capture, continuous CPU sampling, GPU profiling and native detailed
  presentation tracing were disabled in the twelve benchmarks. Every benchmark
  used the same PresentMon display-only ETW collection. Profiles were separate.
- All twelve runs matched both player probes and full route-boundary counters.
  The unchanged physics delta was **0.0166833512 seconds**. No timestep was edited.

The physics measurement uses the same half-open field range, `[2697,4977)`, in
all twelve runs: 2,280 entry samples and 2,279 timed intervals, representing
**38.021357 seconds of game time**. One run exited before the physics entry for
field 4977; the old helper's inclusive-end assertion failed despite the game
route passing. The corrected boundary is applied to every run, with no discarded
runs or overwritten raw evidence. Output and handoff rates use their first/last
timestamps in that same field range. Display ETW analysis uniformly trims 250 ms
from each end of the physics wall-time window because process-exit tracing can
omit the last unfinished presents.

## The four pacing mechanisms

1. **Game field wait:** `0x0025DE90` waits on the guest field counter. Existing
   `PS2X_TF_NATIVE60=3` selects one-field rather than two-field waits and full-rate
   simulation. The wait remains intact; `PS2X_TF_NOFRAMEWAIT` was not used.
2. **Runtime limiter:** `EeScheduler::processDueDeadlines` requires both guest
   cycle progress and a host deadline. `PS2X_REALTIME=0` retains the legacy host
   deadline; it is not uncapped. The test uses `PS2X_REALTIME=1` in both arms and
   changes only its existing `PS2X_REALTIME_HZ` ceiling. Guest cycles per field
   and the simulation delta remain unchanged.
3. **Host presentation cap:** initialization calls `SetTargetFPS(60)`, separately
   from the runtime. `framerate.fps_cap` can override this. However, script mode
   skips `applyLiveSettings`, so merely loading `fps_cap=240` does not apply it.
   The first series therefore measures runtime relaxation only. In the second
   series, the existing settings path is allowed in both arms, using a menu
   autotest trigger at field 1,000,000,000, far beyond the run's lifetime. No
   menu action executes; logs assert that no menu opened. The FPS overlay is
   off in both arms. Logs prove target intervals of 16.667 and 4.167 ms.
4. **VSync and VRR:** raylib initializes GLFW swap interval to zero; the game
   initialization supplies no VSync hint. The second series explicitly applies
   `display.vsync=off` in both arms. Native tracing finds approximately 0.06 ms
   in `SwapBuffers`, not a blocking 16.7 ms VSync wait. Desktop composition still
   governs display delivery. `vrr=on` only adjusts the *auto* host cap here;
   explicit 60/240 caps bypass that adjustment. `PS2X_PRESENT_RATE=60` controls
   field-output selection, not the host loop's FPS limiter.

Relevant code: `ps2_runtime.cpp` (window initialization and `tfFrameSync`),
`Kernel/EeScheduler.cpp` (`realtimeField`, `processDueDeadlines`),
`ui/ps2_pc_menu.cpp` (`initialize`, `applyLiveSettings`), `src/main.cpp`
(script-mode UI gate), and `present_trace.cmake` (actual swap/wait boundaries).

## Repeated benchmark results

Medians of three runs per condition. These two series have separate controls;
paired effects are calculated within each series, not between their medians.

| Series | Runtime / host ceilings | Wall seconds for 38.021 game seconds | Game / wall | Simulation updates/s | Full-frame outputs submitted/s | Host deliveries/s | Reported display changes/s |
|---|---|---:|---:|---:|---:|---:|---:|
| Runtime cap | 60 / 60 | 38.680 | 0.9830x | 58.920 | 58.934 | 57.431 | 40.330 |
| Runtime cap | 240 / 60 | 37.527 | 1.0132x | 60.729 | 60.739 | 58.703 | 43.123 |
| Host cap | 240 / 60 | 37.162 | 1.0231x | 61.327 | 61.319 | 58.649 | 43.184 |
| Host cap | 240 / 240 | 37.682 | 1.0090x | 60.480 | 60.473 | 60.473 | 62.502 |

Runtime relaxation changed simulation throughput by **+3.07%, +0.68%, +5.58%**.
Its relaxed runs reached only 59.845–61.765 updates/s. Game speed ranged from
0.9984x to 1.0305x real time. This is a small processing reserve, not correct
support for a higher simulation rate.

Host relaxation changed simulation throughput by **-0.12%, -1.38%, -1.49%**,
while host deliveries improved by **+3.49%, +2.98%, +3.07%**. Acquisition delay
fell from a median **7.905 ms to 2.103 ms**. Skipped output submissions changed
from **80/97/101 to 0/0/0**. Faster polling helps delivery but adds work; it is
not a simulation optimization and is not enabled for normal play.

### Frame-time spikes, separate from average rates

Values below are medians of the three per-run p99 values. Counts list all three
runs in order, over the same field range.

| Series / condition | Simulation p99 ms | Output-submission p99 ms | Host-delivery p99 ms | Display-event p99 ms | Simulation gaps >25 ms | Host-delivery gaps >25 ms |
|---|---:|---:|---:|---:|---|---|
| Runtime capped | 26.140 | 24.478 | 33.336 | 62.519 | 35 / 10 / 29 | 99 / 46 / 106 |
| Runtime relaxed | 22.747 | 20.876 | 33.332 | 69.433 | 7 / 25 / 8 | 48 / 70 / 41 |
| Host capped | 22.193 | 20.645 | 33.334 | 69.448 | 15 / 2 / 1 | 52 / 47 / 50 |
| Host relaxed | 23.852 | 21.846 | 21.440 | 48.664 | 12 / 20 / 6 | 20 / 13 / 15 |

The 33.33 ms host-delivery quantization improves when its polling cap is relaxed.
Simulation p99 remains about 24 ms with both caps relaxed, above the 16.7 ms
budget. An average near 60 is therefore not sustained, evenly paced 60 FPS.

### Complete images, submissions and display events are different evidence

Separate captures validate **32 distinct complete 2048x1788 scene images** at
consecutive fields 3027–3058, with no adjacent or two-field duplicate images and
both row parities changing. The fully relaxed capture matches the saved enhanced
capture byte-for-byte across all 117,178,368 pixels. No original executable was
launched for that comparison.

The output trace's `p` event records submission of full-frame GPU work, `a`
records host acquisition, and `r` records CPU release submission. None is a GPU
completion timestamp. The table therefore labels output *submissions*, not
independently measured GPU completions. The image evidence validates full-image
cadence in the captured interval; it does not prove every benchmark image is
unique. PresentMon display events likewise are not unique game-frame counts.
There is still a per-frame GPU-completion attribution gap; do not equate all four
counters or use the display-event number as the game's FPS.

## Active work versus waiting

Two separate twenty-second profiles used continuous CPU samples, native scopes,
GPU/display ETW and exact raylib swap/wait boundaries. Their reduced update rate
(about 55/s) is not included in the benchmark results.
Scope percentages average three complete five-second windows fully contained
within each profile interval; host call timings use the detailed trace window.

| Profile measurement | Both caps 60 Hz | Both ceilings 240 Hz |
|---|---:|---:|
| VU CPU use, percent of one core | 96.71% | 96.07% |
| EE CPU use, percent of one core | 87.12% | 87.36% |
| EE wall time waiting for VU queue capacity | 11.22% | 10.91% |
| EE intentional cap / spin / legacy wait | 0.00% | 0.00% |
| Host CPU use, percent of one core | 6.95% | 15.31% |
| Intentional host frame wait per loop | 16.266 ms | 3.958 ms |
| Host loop interval | 16.670 ms | 4.188 ms |
| `SwapBuffers` wall time per loop | 0.060 ms | 0.062 ms |

The native trace recorded 5/61 lost records respectively and no capped output;
complete matched scope pairs cover almost all host-loop time. CPU time is not
the same as the wall time attributed to a wait function, which can include its
final busy-spin portion.

To check that sampling did not create the apparent CPU bottleneck, a further
attribution run disabled continuous sampling, perfscope and GPU profiling.
`GetThreadTimes` measured an entire 20.064-second gameplay window. A separate
one-second symbol sample *after* that window identified the threads:

| Thread | CPU use, percent of one core |
|---|---:|
| VU worker | 98.51% |
| Hardware renderer CPU worker | 96.02% |
| EE/game | 91.58% |
| Host presentation | 14.56% |
| OpenGL driver worker | 12.38% |
| Runtime GS forwarding | 10.98% |

This supports a CPU production limit involving VU and hardware-renderer command
processing, not just intentional 60 Hz sleeping. The EE-to-VU queue-capacity wait
places VU on the dependency path. The hardware renderer is close to saturation
too; optimizing VU alone can expose that next limit. Hot-function sample counts
are not sufficient to select the next optimization.

For GPU work, the capped profile's PresentMon busy duration was **11.433 ms
mean, 13.628 ms p99**, with a 24.456 ms maximum; its separate GPU-wait duration
was **5.337 ms mean**. GPU time including waits averaged 16.770 ms. The relaxed
profile has many more presentation events, including repeated host presentations:
its mean busy duration of 3.332 ms must **not** be called a per-game-frame GPU
speed-up. Its busy p99 was 13.271 ms. These event-level numbers suggest ordinary
GPU work has more headroom than the two CPU workers, while allowing GPU spikes
and synchronization delays. They do not isolate every D3D12/OpenGL fence or prove
a per-image completion deadline. The native swap trace, rather than OpenGL
PresentMon CPU-wait fields, establishes intentional host waiting.

## Follow-up: keep simulation capped, then test frame-ready waiting

The proposed fixed-simulation experiment is now complete. Three further series
each used three alternating pairs, the same route and controls above, with the
runtime held at **60 Hz** and the guest timestep unchanged. None is an unlocked
simulation test. All eighteen runs passed, with identical player probes and
route-boundary counters. No compilation ran concurrently.

1. `game-62`: host cap 60 versus 240, using the existing presenter.
2. `game-63`: frame-ready wait off versus on, both caps 60.
3. `game-63`: frame-ready wait on in both arms, host cap 60 versus off.

`game-63` has the same executable hash listed above. Its renderer SHA-256 is
`1f02b7b0ac82ac4f57f1be4ca2d551b825c9cbe811f445f20a2be4b7bfa09338`.
The only new renderer behavior is selected by `PS2X_HWGS_WAIT_FRAME=1`.
It uses a condition variable when no output slot is ready, with a requested
16.667 ms timeout so a stalled producer does not indefinitely stop host input
and shutdown. Publication wakes the host; GPU fence ownership is unchanged.
The timeout is not a new simulation timestep. The old path remains the default.

Medians of three runs, with separate controls for each series:

| Series / condition | Wall seconds for 38.021 game seconds | Game / wall | Updates/s | Full-frame outputs submitted/s | Host deliveries/s | Mean acquisition delay ms |
|---|---:|---:|---:|---:|---:|---:|
| Fixed simulation, host 60 | 38.267 | 0.9936x | 59.555 | 59.581 | 58.741 | 7.048 |
| Fixed simulation, host 240 | 38.361 | 0.9911x | 59.409 | 59.443 | 59.442 | 2.093 |
| Both caps 60, wait off | 38.357 | 0.9913x | 59.416 | 59.432 | 58.099 | 7.593 |
| Both caps 60, wait on | 38.509 | 0.9873x | 59.182 | 59.202 | 59.020 | 4.247 |
| Wait on, host 60 | 38.268 | 0.9935x | 59.553 | 59.578 | 59.337 | 3.197 |
| Wait on, host off | 38.383 | 0.9906x | 59.375 | 59.398 | 59.398 | 0.015 |

Host 240 recovered deliveries in all three fixed-simulation pairs:
**+2.91%, +1.19%, +0.92%**. Simulation changed by -0.36%, -0.01%, -0.07%.
Frame-ready waiting with both caps 60 also recovered deliveries in every pair:
**+3.51%, +1.65%, +1.21%**, while simulation changes were mixed
(-0.39%, +0.44%, -0.54%). These are delivery improvements, not processing gains.

With waiting on, removing the remaining host timer eliminates skipped
submissions and lowers acquisition delay in all three pairs. Delivery-rate
changes are small and mixed (**+0.45%, +0.10%, -0.03%**), as are simulation
changes (+0.17%, -0.31%, -0.39%). Do not claim a repeatable FPS gain for this
last step. It makes the existing production delay visible without the
independent host timer, which is useful for the next diagnostic.

| Series / condition | Simulation p99 ms | Output-submission p99 ms | Host-delivery p99 ms | Skipped outputs, three runs | Simulation gaps >25 ms | Host-delivery gaps >25 ms |
|---|---:|---:|---:|---|---|---|
| Fixed simulation, host 60 | 22.410 | 20.797 | 33.333 | 72 / 27 / 22 | 8 / 9 / 5 | 88 / 48 / 38 |
| Fixed simulation, host 240 | 22.486 | 20.710 | 21.266 | 0 / 0 / 0 | 8 / 6 / 3 | 9 / 8 / 10 |
| Both caps 60, wait off | 23.254 | 22.254 | 33.336 | 92 / 39 / 46 | 4 / 12 / 11 | 114 / 73 / 67 |
| Both caps 60, wait on | 23.081 | 21.462 | 20.975 | 7 / 12 / 7 | 10 / 12 / 7 | 4 / 6 / 9 |
| Wait on, host 60 | 22.319 | 20.882 | 20.446 | 6 / 9 / 8 | 5 / 5 / 6 | 2 / 4 / 1 |
| Wait on, host off | 22.668 | 20.901 | 20.903 | 0 / 0 / 0 | 3 / 10 / 20 | 3 / 6 / 17 |

The last pair's production spikes worsen despite near-immediate acquisition.
Removing host delay does not eliminate processing spikes. Average rates alone
would conceal this distinction.

The host-off image capture separately passed: 32 consecutive complete
2048x1788 images, all unique, with both row parities changing. All 117,178,368
pixels match the saved enhanced reference byte-for-byte (SHA-256
`5788f151bc239078fd4b33cf3588cac86bf2ec0ca54fe0e41bc8d17508e0c1b4`).
This validates image correctness in the captured interval, not per-image GPU
completion timing throughout the benchmarks.

PresentMon display-event rates differed between sessions and include repeated
host presentations. For example, the final host-off series reports roughly
68-69 display changes/s while acquiring only 59.35-59.51 game frames/s. These
are not 69 unique rendered frames/s. Compare paired native counters; do not
combine absolute display-event rates from different series into a game-FPS gain.

### Separate host-cost profiles

Two additional profiles used `GetThreadTimes` across 20.063-second gameplay
windows and native host wall-time scopes, without continuous CPU sampling or
GPU profiling. They compare the original 60 Hz poll with waiting on and the
host cap off, both in `game-63` with the runtime held at 60. They establish the
waiting mechanism and cost; they are not another benchmark pair.

| Measurement | Original host poll, cap 60 | Frame-ready wait, host cap off |
|---|---:|---:|
| Host CPU, percent of one core | 6.70% | 3.66% |
| Host frame interval, mean ms | 16.671 | 13.808 |
| Intentional host limiter wait, mean ms | 16.256 | No calls |
| Acquire/copy scope, mean ms | 0.239 | 13.624 |
| Acquire/copy scope, percent of wall window | 1.43% | 98.60% |
| `SwapBuffers`, mean ms | 0.061 | 0.066 |

The acquisition scope includes condition-variable waiting and GPU copy
submission; its 13.624 ms is not active CPU work. The CPU clocks independently
measure only 3.66% of a core on the host thread. Host frames include timeout
redraws of the previous image, so their approximately 72 Hz loop is not the
unique-image rate. GPU waits remain GPU-side. Trace loss was 1/5 records, no
output cap was hit, and complete host scopes cover over 99.8% of each window.
Trace thread IDs are logical ordinals, not Windows thread IDs; OS CPU clocks
use the window-owner thread identified by `GetWindowThreadProcessId`.

## Recommendation and smallest next experiment

**Stable 60 needs CPU processing headroom, plus a separate host-pacing fix.**
Prioritize the demonstrated VU/renderer CPU path for subsequent processing work.
Do not start another geometry rewrite or choose a function solely from its sample
frequency. Rendering above 60 with interpolation is not the next experiment:
the existing full-frame production path has little reserve, and fixed-rate
game-time behavior still needs confirmation under the final presentation policy.

The smallest proposed pacing experiment has passed: frame-ready waiting
repeatedly improves deliveries and removes about **12 ms from delivery p99**
with both caps held at 60. Retain it as an opt-in candidate with the original
path as fallback. The no-host-cap variant reliably removes acquisition delay
and skips, but has not shown better production throughput or spike timing.
Neither configuration is promoted to normal play here.

The smallest **next** experiment is narrowly scoped per-frame timing, not
another geometry rewrite: correlate VU completion and queue waits, the existing
hardware-renderer queue/GS-work trace points, and actual GPU fence completion
against the same guest field and delivered image. Use bounded diagnostic
records and nonblocking fence observations; do not add a CPU fence wait to the
normal presentation path. Separate this profiling from benchmarks.

This will distinguish late VU production, late CPU command processing, and GPU
or cross-API synchronization on the 22-23 ms spike frames. The present evidence
does not bound a specific function's optimization payoff. Report the late stage
and its removable milliseconds before selecting a substantial processing
change; then test that change with at least three alternating pairs.

The approximately 0.1% NTSC timestep/60 Hz calibration difference remains
explicit: the guest delta is 0.0166833512 seconds, not 1/60. The fixed-cap runs
are still below real time because production cannot consistently meet the
deadline. Do not edit the delta to 1/120 or call cap relaxation 120 FPS support.
Neither cap relaxation nor the current measurements establish 120 FPS support,
correct unlocked gameplay, stable whole-level 60, or full-campaign reliability.

## Evidence locations (private workspace, not repository assets)

- `pacing-paired-bench-01/{manifest,analysis}.json`: three runtime-cap pairs;
  `level-pacing-{capped,relaxed}-01..03` raw run directories.
- `host-pacing-paired-bench-01/{manifest,analysis}.json`: three host-cap pairs;
  `level-host-pacing-{capped,relaxed}-01..03` raw run directories.
- `pacing-diagnosis-summary-01.json`: cross-run player/counter checks and medians.
- `full-pacing-profiles-01/manifest.json` and
  `level-full-pacing-{capped,relaxed}-sample-01`: separate native/CPU/GPU profiles,
  `frame-delivery-analysis.json`, `profile-summary.json`, raw trace and ETW CSVs.
- `level-pacing-os-cpu-01/{os-thread-cpu,os-thread-attribution,attribution}.json`:
  CPU clocks and subsequent thread identification. The earlier late-started CPU
  observer attached after its target exited and yielded no samples; it is not
  used as evidence.
- `level-full-pacing-relaxed-scene-01/{cadence,scene-comparison}.json`: complete
  image cadence and equality against the saved enhanced capture. Pixel files
  remain private and outside the source checkout.
- `fixed60-host-pacing-paired-01/{manifest,analysis}.json`: three host-cap pairs
  with the runtime fixed at 60 Hz, in `game-62`.
- `frame-ready-paired-01/{manifest,analysis}.json`: three wait-off/on pairs with
  both caps 60, in `game-63`.
- `frame-wait-hostcap-paired-01/{manifest,analysis}.json`: three host-60/off pairs
  with frame-ready waiting on and the runtime fixed at 60, in `game-63`.
- `level-frame-ready-scene-01/{capture-run,cadence,scene-comparison}.json`:
  hash-bound complete-image qualification of waiting on with the host cap off.
- `frame-ready-cost-profiles-01/manifest.json` and
  `level-frame-ready-cost-{poll60,frame-ready}-01/{os-thread-cpu,host-cost-analysis}.json`:
  separate CPU-clock and native wall-time attribution. Raw trace files remain
  alongside the reports; no game assets are committed.

### Individual benchmark runs

| Run | Game/wall | Updates/s | Outputs submitted/s | Host deliveries/s | Display changes/s | Simulation p99 ms | Host p99 ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| `level-pacing-capped-01` | 0.9830x | 58.920 | 58.934 | 57.431 | 41.084 | 26.140 | 33.336 |
| `level-pacing-relaxed-01` | 1.0132x | 60.729 | 60.739 | 58.703 | 44.670 | 22.559 | 33.332 |
| `level-pacing-capped-02` | 0.9917x | 59.440 | 59.472 | 58.789 | 40.306 | 23.098 | 33.331 |
| `level-pacing-relaxed-02` | 0.9984x | 59.845 | 59.834 | 58.126 | 42.776 | 25.231 | 33.334 |
| `level-pacing-capped-03` | 0.9760x | 58.502 | 58.507 | 57.203 | 40.330 | 26.955 | 33.341 |
| `level-pacing-relaxed-03` | 1.0305x | 61.765 | 61.771 | 58.880 | 43.123 | 22.747 | 33.331 |
| `level-host-pacing-capped-01` | 1.0133x | 60.734 | 60.730 | 58.608 | 43.660 | 23.619 | 33.334 |
| `level-host-pacing-relaxed-01` | 1.0121x | 60.662 | 60.656 | 60.656 | 61.814 | 23.852 | 24.991 |
| `level-host-pacing-capped-02` | 1.0231x | 61.327 | 61.319 | 58.725 | 41.986 | 22.193 | 33.334 |
| `level-host-pacing-relaxed-02` | 1.0090x | 60.480 | 60.473 | 60.473 | 62.502 | 24.641 | 21.440 |
| `level-host-pacing-capped-03` | 1.0238x | 61.367 | 61.364 | 58.649 | 43.184 | 22.036 | 33.334 |
| `level-host-pacing-relaxed-03` | 1.0086x | 60.454 | 60.453 | 60.452 | 63.554 | 23.051 | 21.315 |

## Current-build audit after the staging-pool test

This audit launched no game, benchmark, profiler or compilation. It reads the
`game-66` level measurement, the previous detailed `game-64` profile, the
historical relaxed-cap evidence above, and the implementation. The private
analysis and input hashes are in `frame-critical-path-audit-01.json`; the
read-only analyzer is `audit-frame-critical-path-01.py`.

### The latest measurement deliberately retains a runtime ceiling

`launch/enhanced_env.txt` sets `PS2X_REALTIME=1` and
`PS2X_REALTIME_HZ=60`. The actual `game-66` log confirms
`cap ON (PS2X_REALTIME) fieldUs=16666.7 (NTSC) catchUpMs=50`.
`EeScheduler::processDueDeadlines` gates a field on both guest cycle progress
and its host deadline. The next deadline advances by the configured field
period, with at most 50 ms of catch-up debt. This is an average pacing ceiling,
not a minimum 16.667 ms gap between every pair of outputs.

`tfFrameSync` retains the guest next-field wait in native60 mode. Each
`HardwareGS::RawVsync` publishes one output. Rendering is still tied to guest
field production; turning off the host FPS cap does not detach rendering from
that clock. `fps_cap=off` applies `SetTargetFPS(0)`, and `PS2X_PRESENT_RATE=60`
selects output fields rather than overriding the runtime ceiling.

The frame-ready acquisition waits only if no new output is available. It wakes
on notification immediately; 16667 microseconds is its timeout, not a sleep
charged to every frame. In the latest run, publication-to-host acquisition
averaged **0.0195 ms**, with **0.267 ms p99**, **0.819 ms maximum**, and no skipped
submissions. There is no evidence here that host acquisition is the limiting
stage. These are host handoffs, not GPU-completion or physical scanout times.

### The whole-route average hides lower throughput during movement

The same `game-66` trace was divided by the script's fixed field ranges. No new
run was needed. Rates use timestamp differences within each range; the interval
crossing each range boundary is excluded.

| Route portion | Fields | Deliveries | Deliveries/s | Delivery p99 ms |
| --- | --- | ---: | ---: | ---: |
| Forward movement | 2697–2876 | 180 | 55.260 | 21.442 |
| Pause before camera turn | 2877–2996 | 120 | 56.321 | 22.574 |
| Camera turn | 2997–3176 | 180 | 57.752 | 21.357 |
| Stationary after turn | 3177–4976 | 1800 | 60.082 | 20.897 |

The final portion's slight excess over 60 is consistent with bounded catch-up
and interval boundaries; it is not evidence of sustained unlocked headroom.
The 59.28 whole-route average is dominated by the 1800 stationary fields. The
movement and camera segments establish that this short route is not delivering
smooth 60 throughout, even though most of its duration approaches the cap.

### CPU production, including the CPU side of GPU offloading

The saved detailed `game-64` profile measured the hardware-renderer worker at
95.875% of one CPU core, averaging **16.372 ms of CPU time per output**, with
only **0.513 ms input-queue idle time**. CPU clock readings are quantized per
frame; these aggregates are meaningful, individual per-frame CPU readings are
not. The separate older OS-clock measurement also found the VU worker at 98.51%
of a core and EE at 91.58%, with EE-to-VU capacity waits measured separately.
These workers overlap; their times must not be added into a serial frame cost.

New grouping of 15,396 saved renderer leaf samples across 1,744 complete
production intervals gives the following distribution. The symbols come from
the existing byte-matched renderer map. These are statistical elapsed-time
estimates, not instrumented inclusive function durations or removable budgets.

| Renderer work category | Sample share | Estimated elapsed ms/output |
| --- | ---: | ---: |
| CPU preparation, validation and bounds for native GPU geometry | 29.45% | 5.06 |
| Draw setup and device/texture commands | 17.56% | 3.02 |
| Texture cache, palettes and guest VRAM handling | 13.57% | 2.33 |
| Graphics driver CPU work or internal waits | 12.01% | 2.06 |
| GIF decoding and GS state | 9.28% | 1.59 |
| Other renderer/runtime, copies, wait leaves and allocation leaves | 18.13% | 3.12 |

These timings precede the logging fix and staging pool; they do not establish
the exact current-build cost of any one function. They do show substantial
CPU work remains around the offloaded geometry. GPU transforms do not replace
packet qualification, conservative bounds, GS state/texture tracking, fallback
reconstruction and draw submission.

Current `game-66` periodic summaries spanning the measured route record roughly
5.46 million grass bounds queries, 4.12 million native reflection-path draw
submissions, and only 165 new immutable source uploads. These are deltas between
periodic reports before the markers, not exact marker-time counters. Reflection
compute is grouped, but `drawReflectionPacket` still processes four sections
with state prefixes, indices, trace preparation and native draw calls per
packet. Its `gpu_draws` counts successful native submissions, not a measured
count of low-level API draw commands. The staging pool addresses a comparatively
infrequent operation and does not remove this steady CPU workload.

### Locate the wait before choosing another allocation optimization

Pairing the existing direct-publication call's start/end records gives
**0.443 ms mean**, **1.115 ms p99** and **2.831 ms maximum**. Its mean splits
into 0.289 ms before the worker callback reaches its publication marker,
0.041 ms to the GPU query record, and 0.114 ms from that record to caller return.
This does not explain the earlier 25–27 ms renderer intervals. Most of their
work occurred while processing frame commands before the final publication.

There are distinct waits in the implementation:

- Host frame-ready wait: downstream waiting for a producer, not an independent
  60 Hz sleep once the host cap is off.
- EE-to-VU queue/fence waits: upstream work depends on the busy VU worker.
- Renderer queue throttling: bounds producer lead; increasing it can add latency
  without increasing the consumer's processing capacity.
- D3D12 resource reuse: `ExecuteCommandList(WaitType::None)` still calls
  `MoveToNextCommandList`, which waits if its next allocator's fence is pending.
  Texture upload stream exhaustion also calls `WaitForFence`. The absence of
  an explicit submission-completion wait does not make every submission
  nonblocking.
- Cross-API presentation release uses `ID3D12CommandQueue::Wait`, which queues
  a GPU-side dependency and returns to the CPU. It differs from the CPU event
  waits above. [Microsoft's API documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-wait)

The older sampled driver allocation/CPU-sync stalls are real leads, but their
resource caller was not captured. The staging result does not identify that
caller. The 11–12 ms publication-to-GPU-completion interval overlaps pipeline
work and includes queued work; it is not 11–12 ms of removable CPU idle time.

The recommendation remains to obtain CPU production headroom, not remove more
presentation waits or alter the simulation timestep. A smallest useful next
attribution experiment is one buffered call-site profile of native geometry
preparation/draw submission and D3D12 allocation, stream-space and command-list
reuse waits, tagged to renderer intervals. Preserve the already-correct fallback
and fence ordering. Report the critical caller and its cost before implementing
another optimization; leaf frequency alone is insufficient. A future relaxed
runtime-cap measurement would be isolated throughput testing, potentially
speeding up game time, and would not establish correct higher-FPS gameplay.
