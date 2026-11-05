# Scripted routes

The runtime's `.tfs` scripts isolate guest pad input from the host. Scripts wait for guest events, emit checkpoint counters and end with a verdict. All generated logs, dumps, cards and images must stay outside this checkout.

From PowerShell, use explicit private paths:

```powershell
./testing/run_test.ps1 -Exe ../private-game/ps2EntryRunner.exe -Elf ../private-disc/SLUS_206.68 -Iso ../private-disc/game.iso -Script ./testing/mvp/level_mvp.tfs -Work ../private-runs/mvp-01 -Seconds 1200
```

`-McSeed` accepts your own card folder and copies it to a fresh card. No seeds are included. `-Env` accepts semicolon-separated `KEY=value` overrides. Output/card/script paths and silent audio remain enforced. Test windows are visible. For accelerated runs, pass `PS2X_VU1_AOT=1;PS2X_VU1_AOT_STATIC=1` with a build containing the private static VU package. Set `PS2X_SCRIPT_PARSE_ONLY=1` to print the parsed program and exit before guest execution. Optional `-BusyFlag` paths delay launch while another workload is active; `-SlotFile` holds an exclusive shared admission file throughout the run. Participants must use the same slot path. The portable wrapper does not integrate with private workstation admission helpers.

| Exit | Meaning |
|---|---|
| 0 | Script PASS (confirm `[script] RESULT PASS` and counters) |
| 1 | Runtime error |
| 2 | Guest/host stall |
| 3 | Assertion or route failure |
| 4 | Script wall budget exceeded |
| 5 / exception code | Crash; inspect private crash report |
| 6 | Script parse error |
| 7 | Aborted without verdict |
| 124 | Wrapper timeout; only its own child is stopped |

Each work directory contains environment, route copy, stdout/stderr, script verdict and sampled total CPU load. Do not count a busy run as quiet timing. Preserve the crash reporter; a counter pass is not a visual/audio pass.

## Route selection

- `mvp/level_mvp.tfs`: fresh card, boot/menu/first arrival.
- `mvp/level_mvp_cine.tfs`: fresh card, intro cinematics and gameplay.
- `mvp/level2_save.tfs`: requires a save at the second Amazon warp gate.
- `forest/l2x_forest.tfs`: legacy forest route; the death-menu tail is not gameplay.
- `forest/l2x_forest_v2.tfs`: survivable forest route with objective-speech validation.
- `antarctica/ant_research_camp.tfs`: requires a save with the Research Center gate available.

Additional scouting and development scripts are exploratory; do not assume they carry accepted references. See [references](REFERENCES.md).

## Language essentials

One statement per line; `#` introduces a comment. Labels end in `:`. `budget` bounds wall time; `stall` and `hard` bound guest inactivity. `wait`, `press ... until`, `stick` and `mark` express event-driven routes. `assert`, `pass`, `fail` and watchdogs determine verdicts. Runtime `Kernel/TestScript.cpp` is the authoritative parser; parse-check a changed route before measuring it. Disc file paths within scripts refer to the user's ISO; they do not imply distributed assets.

Current enhancement tests skip both opening movies and in-game cinematics. Replace an opening `wait seen /FMV/NATTRACT.PSS` with `press start until seen /FMV/NATTRACT.PSS every 10 max 300 hold=2 gap=3 timeout 4000vs`. After entering a level, use `press start until player_ready every 10 max 300 hold=2 gap=3 timeout 9000vs` instead of a long cinematic wait. `player_ready` requires the validated Transformers controller to remain in robot or vehicle mode for 60 consecutive guest fields. Presses stop immediately when control appears while that stability check completes, avoiding a Start press that would pause gameplay. The stability interval also rejects the brief controllable state before the first level movie starts. These routes need the updated runtime; old reference counters do not apply after changing their input sequence.


The `enhancements/objective_guidance.tfs` route uses a private Amazon hub seed with Aftershock unlocked and `game.objective_guidance=on` in the test INI. It walks through the first guidance trigger, then disables the setting. Verify the logged Training area -> Combat training -> hidden transitions and inspect host captures. Ordinary comparison screenshots contain only guest output, so use `PS2X_HIRES_CAPTURE_DIR`, `PS2X_HIRES_CAPTURE_VS`, and `PS2X_HIRES_CAPTURE_KEEP_SIZE=1` to capture the host marker while preserving the requested window size. All captures and seed cards belong outside the source checkout.

`enhancements/direct_presentation.tfs` uses the same seed and exercises GPU presentation, 4x/8x/4x internal resolution, FXAA on/off and objective guidance. Select `PS2X_GS_BACKEND=hwgs;PS2X_HWGS_DIRECT=1`, use the accelerated package and an explicit private settings INI. Check actual dimensions and zero full-frame readbacks in the logs, not just the terminal PASS. Direct-path script captures retain the full internal image; host captures include the overlay. See [presentation qualification](../docs/findings/direct-presentation.md) for limitations and timing methodology.

`enhancements/native_mesh_host.tfs`, driven by `run_native_mesh_host.py`, tests the
whole-mesh native renderer on the enhanced game's device/queue after entering the
level. It requires a renderer built with `TF_HWGS_NATIVE_MESH_PROBE=ON`, private
native fixture assets, the Amazon save seed and explicit 1280x720 windowed
settings. One run only, Normal priority, cinematics skipped, no menu checks or
performance claim. The native pass renders to separate 1080p targets; it does
not replace a world submission. `verify_native_mesh_host.py` checks saved native
images, byte-exact preservation of the host image, and gameplay continuation
without relaunching. See [native renderer tests](../deps/native_renderer/README.md).
