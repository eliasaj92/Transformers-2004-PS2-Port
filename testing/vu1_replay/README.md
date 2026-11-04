# Accelerated VU fixture replay

This Windows x64 tool runs a locally generated static VU package against private
captured inputs. It compares data memory, every semantic register/state field
(including cycles), code memory and complete ordered GIF records with captured
outputs. It rejects interpreter fallback and pending pipeline transfers. The
interpreter oracle is not linked. This is an offline CPU test; it does not start
the game or test the graphics backend.

Use a Visual Studio 2022 x64 developer shell and an AVX2-capable CPU. Keep the
generated package, build, fixtures and reports outside this checkout. Configure
verifies the package's USA ELF identity and all manifest file hashes; changes to
those inputs force reconfiguration. An existing compiled package can be reused
for subsequent driver changes.

```powershell
cmake -S testing/vu1_replay -B C:/private/vu-replay -G Ninja -DCMAKE_BUILD_TYPE=Release -DPACKAGE_DIR=C:/private/vu-package
cmake --build C:/private/vu-replay --parallel 4
C:/private/vu-replay/tf_vu1_replay.exe C:/private/fixtures.txt C:/private/replay.csv
```

`fixtures.txt` contains one absolute `*-pre.bin` path per line. Matching
`*-post.bin` and `*-gif.bin` files must be beside it. Use a fresh report path.
Run `testing/inspect_native_seam_capture.py` on the original capture and CSV
first: snapshots alone do not record whether the original call fell back or
transferred unfinished work. Exclude such calls. The normal replay reports
`mode=exact` and exits nonzero for any mismatch. Its CSV includes guest-cycle
increments and host thread cycles; file loading and comparison are outside that
host measurement. These isolated timings are not gameplay fps.

## Register dependency experiment

An optional hexadecimal mask restores selected VF5..VF31 registers to their
input values **after** each recognized grass call, then carries the changed
state into subsequent calls. For example, `08000000` restores VF27:

```powershell
C:/private/vu-replay/tf_vu1_replay.exe C:/private/all-fixtures.txt C:/private/vf27.csv 08000000
```

First pass the same list in exact mode. Use a consecutive, unfiltered capture
(`PS2X_TF_SEAM_FAMILY=all`), with no truncated, fallback or pending calls.
Consecutive filenames are checked, but they cannot reveal a family filter that
omitted other VU programs. Do not use a grass-only fixture list for this mode.

The experiment applies observed external writes between captured calls, one
semantic field/component at a time. A write of the same value is invisible to
snapshots; carrying a perturbation through it may produce a conservative false
failure. It resets data/code to the captured input each time and stops at the
first changed data write, packet, cycle count or control state, before such a
difference could be lost at the next reset. Other state differences are counted
and allowed in this mode. `mode=register-experiment` passing therefore means
only that the selected perturbation did not reach those outputs within this
capture; it is not exact state equivalence or permission to bypass VU work.

On the 5,000-call broad Amazon capture, exact replay matches every output.
Restoring VF27 first changes VU data qword 1007 at ticket 1824; restoring VF31
first changes qword 1003 at the same ticket. Neither has changed GIF output or
control state by that point. Restoring all other VF5..VF31 registers together
(`77ffffe0`) retains data, packets and control through the capture, with 958
different register states. See [the investigation](../../docs/findings/native-render-seam.md)
for coverage and the remaining replacement gates.

## Short CPU tail and deferred memory

Configure with `-DTF_REPLAY_DUAL_TAIL=ON` to build `tf_dual_tail` instead.
Pass the selected dual-transform capture list and optionally the broad list:

```powershell
C:/private/tail-check/tf_dual_tail.exe C:/private/qualified-captures.txt C:/private/all-captures.txt
```

The selected calls compare complete final semantic state, deferred-memory
materialization, strict CPU fallback geometry and caller floating-point state.
The optional broad list also verifies the authenticated manager and context
setup programs' read footprints by poisoning all data outside their inputs. Both use only
the accelerated static package. See [the contract and recorded results](../../deps/hwgs/GPU_GEOMETRY.md#guarded-cpu-tail-and-deferred-vu-memory).

## Reflection input capture and GPU tail experiment

Configure a separate private build with `-DTF_REPLAY_REFLECTION_CAPTURE=ON`
to build `tf_reflection_capture`. The chain wrapper authenticates the resident
reflection program and records its input/output memory plus semantic state.
The enclosing accelerated replay must still match every original captured call.
Use the Python driver to select a fresh output path, verify that replay, prepare
vertex inputs, compare the optimized GPU kernel and run the retained-tail
experiment:

```powershell
cmake -S testing/vu1_replay -B C:/private/reflection -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DPACKAGE_DIR=C:/private/vu-package `
  -DTF_REPLAY_REFLECTION_CAPTURE=ON
cmake --build C:/private/reflection --parallel 4
```

The capture/GPU/tail commands and result scope are documented in the
[reflection contract](../../deps/hwgs/GPU_GEOMETRY.md#reflection-lighting-kernel-and-retained-cpu-tail).
The tail mode restores omitted vertices from an authenticated **actual GPU**
capture, compares final chain state/memory/cycles, and carries the candidate's
pending pipeline state through the rest of each call. It declines active GIF
transfers and batches of four or fewer vertices. This diagnostic does not enable
a live readback path or establish a gameplay speedup.
