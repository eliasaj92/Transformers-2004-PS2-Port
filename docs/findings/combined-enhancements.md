# Combined enhanced play preset

The current private launcher selects `game-65`, which adds optional checkpoint
logging and buffered timing capture to the combined preview below. Its single
windowed run reduced frequent spikes but retained a worse isolated simulation
outlier; see [the logging-fix comparison](spike-attribution.md#one-windowed-run-after-the-fix).
Saves and video/trainer settings remain unchanged.

The private play launcher was still selecting `game-24`, while newer PGO,
GPU geometry and frame-ready presentation builds were used only by the test
drivers. The initial integration selected the combined `game-63` preview through
`launch/play.ps1 -Enhanced`. Saves, trainer settings, input preferences and
objective guidance retain their existing locations and values. The old private
launcher and settings were backed up before changing the selection.

`launch/enhanced_env.txt` is the shared runtime preset for play and the integrated
measurement. It combines the existing PGO/static-VU runtime, direct D3D12
presentation, grouped/merged dual geometry, GPU grass with bounds caching,
grouped GPU reflections, pending-work VU idle handling, native timing variant 3,
movement correction and frame-ready waiting. The scheduler ceiling stays at
60 Hz; the guest timestep remains 0.0166833512 seconds. Fast EE checkpoints
remain off because earlier measurements did not show a benefit.

The enhanced launcher requires a compatible private build. It verifies the
executable and renderer hashes against `local-build.json`, checks the required
runtime capabilities and selects hardware GS with direct presentation. It clears
the presenter diagnostic environment and uses Normal process priority. This is
an experimental combined preview, not a whole-campaign or sustained-60 release.

The user's persisted video settings now select 4x internal rendering, 16:9 Hor+,
FXAA, VSync off and host cap off. The frame-ready wait limits host polling while
the independent simulation ceiling stays at 60. The current 1920x1080 desktop
uses a **1280x720 window** so the decorated window remains fully visible;
internal scene output is **2048x1788**. Internal rendering and window size are
separate settings. Trainer cheats remain as the user had configured them.

## Single integrated measurement

At the user's request, future measurements are **one windowed enhanced run,
then wait for the user's direction**. Do not launch fullscreen tests or automatic
alternating batches. Earlier paired measurements remain historical evidence.

`level-integrated-windowed-01` passed the saved Amazon route with opening and
arrival cinematics skipped, all runtime options matching the shared play
preset, Normal priority, and no compilation or profiling. The test copied its
save seed and disabled cheats; it did not modify the user's playable saves or
trainer settings. Player probes match the saved enhanced reference.

| Measurement | Result |
|---|---:|
| Simulation updates/s | 59.411 |
| Full-frame output submissions/s | 59.427 |
| Host deliveries/s | 59.427 |
| Submitted / acquired images | 2280 / 2280 |
| Skipped submissions | 0 |
| Simulation p99 / maximum | 23.259 / 30.305 ms |
| Host-delivery p99 / maximum | 21.896 / 28.532 ms |
| Simulation / delivery gaps over 25 ms | 13 / 9 |
| Mean publish-to-acquire delay | 0.016 ms |
| Game time / wall time | 38.021 / 38.360 seconds (0.9912x) |

This closes the integration gap but does not improve upon the best previously
measured prototype throughput. Processing spikes still prevent smooth 60.
The window, FXAA and overlay settings differ from older measurements, so this
single run does not establish an isolated speedup for the combination. Output
submissions and host acquisitions are not independent GPU-completion or unique
scanout counts. The previous complete-image capture qualified the same binary
and runtime geometry combination separately.

The preceding diagnostic `level-frame-pipeline-02` measured actual GPU output
completion timestamps: approximately 59.005 completions/s, tracking CPU output
submission cadence. Its renderer CPU worker consumed 97.21% of one core, with
0.467 ms mean queue-idle time per field. GPU completion followed publication by
11.431 ms on average; that interval includes GPU work and waiting, not just
active GPU execution. Only one of 2280 frames observed an outstanding GL-release
fence before reuse. This supports focusing the next combined change on CPU frame
production rather than removing more presentation waits. The profiler's
same-field physics/output timestamp difference is not a causal simulation-to-
render latency: the asynchronous pipeline can produce an output before the
physics entry carrying the same field number.

Private evidence: `integrated-launcher-01.json`,
`runs/level-integrated-windowed-01/{measurement-run,performance,combined-result,window-position}.json`,
and `runs/level-frame-pipeline-02/pipeline-analysis.json`. The first pipeline
bring-up run failed its window check after the desktop changed; it is excluded
from controlled comparisons. No original build was launched.
