# Native 60 Hz timing investigation

This is an unfinished experiment for USA ELF CRC `74c982c0`. Normal launch settings remain at original game speed. A 1/60-second timestep and one update per emulated field do not establish 60 rendered frames per wall second.

## Three independent controls

- The native governor at `0x25b670` uses mode byte `0x6cd984`. Mode 0 selects the full-rate branch and interpolation factor 1 at `0x6cd6f0`; mode 1 selects half rate and factor 0.5. Its emulated-cycle threshold can revert the mode even when the host is fast enough.
- Frame synchronization at `0x25de90` reads the wait flags at `gp-0x40e0`, `gp-0x40dc`, and `gp-0x40d0`. The native adaptive branch admitted repeated calls with no new field. Waiting for the next current field removes those repeated updates.
- The half-rate setter at `0x32e8a0` writes `0x6cf654`. The fps getter at `0x32e8b0` and reciprocal timestep getter at `0x32e910` consult it independently of the governor. Scene initialization at `0x294950` caches that timestep at scene +0x5c. Scene update at `0x292de0` passes the scaled timestep to physics `0x230100` (caller return address `0x292fb0`). Changing only the governor/frame wait leaves this cached physics timestep at 1/30 second and speeds up simulation.

Do not blanket-replace floating-point 1/30 constants. Some are initialization, animation warm-up, or mutable state rather than fixed frame durations.

## Private qualification switches

`PS2X_TF_NATIVE60` is CRC-gated and absent from normal presets/settings:

| Value | Experiment |
|---|---|
| Unset / 0 | Original behavior |
| 1 | Keep the native full-rate governor branch active |
| 2 | Also wait for the next current field |
| 3 | Also disable the simulation half-rate setter before scene initialization |

Use `PS2X_FIELD60=0` to keep field presentation separate. `PS2X_TF_TIMING_TRACE=<private.csv>` records host time, guest field, pad count, timing slot, timestep and caller at the physics/timing entry points. `trainer_probe` additionally reports player position, heading and speed. Its `frame_delta` is timing slot 7, not proof of the timestep actually received by physics.

`PS2X_HWGS_PROFILE=1` attributes bridge time to GIF enqueueing, synchronization, vsync enqueueing, output readback, rectangular guest readback and pixel reads. `rect_work` measures the work inside the GS callback separately from the outer queue wait. Readback time includes waiting for the separate kzgs thread and GPU; it is not isolated GPU execution time. `PS2X_HWGS_API=d3d12` selects a private API comparison; Vulkan remains the default. [Direct presentation](direct-presentation.md) removes full-frame readback from this path.

## Observations on the Ryzen 6900HX / RTX 3080 Laptop

All listed runs used the accelerated VU package, visible hardware-renderer windows, 4x internal resolution and 16:9. Saves and raw evidence remain private.

- `native60-governor-01` and `native60-cached-01`: the full-rate governor alone produced repeated zero slot-7 deltas and redundant rendering work. Reusing output download allocations did not establish a performance gain.
- `native60-nextfield-01`: one update per guest field and slot 7 about 0.0166833 seconds, but physics still received 0.0333667 seconds. Two hundred calls integrated 6.67334 seconds over 200 NTSC fields: double speed.
- `native60-fixedstep-01`: variant 3 passed the input route. Two hundred physics calls over 200 fields received 0.0166833512 seconds each, integrating 3.33667024 seconds. Wall-clock gameplay remained roughly 22–25 updates/second.
- `native60-skip-vulkan-03` and `native60-skip-d3d12-01`: opening movies and the Amazon movie/arrival cinematics skipped; both reached playable Optimus and returned through robot/vehicle/robot states. Each ended at 3,763 fields, 2,775 pad reads and 1,119 CD operations. Script wall times were 95.6 and 94.7 seconds respectively; whole-route times include loading and menus, so they are not gameplay fps. The single Direct3D 12 run does not establish a repeatable improvement or visual equivalence.

The first skipping experiment was stopped. The next emitted PASS while still in a movie because the player briefly appeared controllable before that movie started; it is invalid gameplay evidence. The corrected `player_ready` condition requires 60 consecutive fields of control and suppresses further skip presses as soon as control appears. The two completed skip runs used this correction. Future tests must skip opening and in-game cinematics.

The original-speed comparison `original-skip-vulkan-01` passed with 4,793 fields, 1,796 pad reads and 1,129 CD operations. Over the same 120 guest fields, original speed performed 60 physics updates at 0.0333667 seconds; variant 3 performed 120 at 0.01668335 seconds. Both integrate about 2.002 seconds. The short walk nevertheless displaced the player about 28.70 versus 30.14 world units, a roughly 5% difference requiring acceleration/animation investigation. Vehicle-stick input did not propel the vehicle and cannot qualify driving speed.

Build 18 corrected the CSV's field label to read the EE scheduler directly. Older traces used the presentation-thread counter, which can lag by a field; do not infer missing/extra physics updates from those older labels. Guest counters and recorded positions are unaffected by that diagnostic change.

## Dense acceleration comparison

`movement-dense-original-01` and `movement-dense-native-01` use the same accelerated build 30, direct 4x hardware presentation, 16:9, a fresh copy of the private Trainer save and the cinematic-skipping route. Grass comparison is off. They record player state every two guest fields for 180 fields after applying the same full-forward stick input; these are simulation comparisons, not speed benchmarks.

Original timing performs 90 physics updates at 0.0333667025 seconds; variant 3 performs 180 at 0.0166833512 seconds. Both integrate 3.0030032 seconds. Original speed reaches about 21.9969 world units/s and variant 3 reaches 21.9968. The three-second displacement is 51.3733 versus 52.8288 units. Thus the persistent distance difference arises during acceleration, while terminal running speed agrees.

The first nonzero speed appears at relative field 4 in the original run and field 2 in variant 3. A further divergence occurs during the transition near fields 28..42: original speeds are 6.99445, 8.26128, 9.27460 and 10.0851 at fields 28, 30, 32 and 34; variant 3 records 9.02881, 10.3360, 11.1724 and 11.7075. The original increments in that segment shrink by roughly 0.8 per update, versus roughly 0.64 over two native updates. This suggests a per-update blend in the walk/run transition; its writer and timing inputs have not yet been traced. Treat this as a lead, not a qualified correction, and do not compensate by changing maximum movement speed.

## Remaining work

Compare movement, acceleration, firing, animation and physics against original speed using equivalent guest-time intervals. The existing timing route's vehicle stick input verifies steering/transformation, not acceleration. Measure distinct complete scene images per wall second, investigate GS readback/queue costs, and qualify demanding areas and transitions. Do not promote the experimental flags or claim true 60 fps from pad counters, field output, or the timestep trace alone.
