"""Summarize a completed enhanced level run; never launches a game executable."""
import argparse
import csv
import json
import re
import statistics
from pathlib import Path


def summarize(work: Path) -> dict:
    result = json.loads((work / "script_result.json").read_text())
    if result.get("result") != "PASS":
        raise ValueError("The level route did not complete successfully")
    env = dict(line.split("=", 1) for line in (work / "env.txt").read_text(encoding="utf-8-sig").splitlines() if "=" in line)
    if "PGOSWEEP_EXIT" in env:
        raise ValueError("Instrumented profile training is not a speed measurement")
    for key, required in {"PS2X_VU1_AOT": "1", "PS2X_VU1_AOT_STATIC": "1",
                          "PS2X_HWGS_DIRECT": "1", "PS2X_TF_NATIVE60": "3",
                          "PS2X_HIDDEN_WINDOW": "0", "PS2X_PERFSCOPE": "0",
                          "PS2X_HWGS_PROFILE": "0", "PS2X_VU1AOT_HASH": "0",
                          "PS2X_TF_GRASS_VERIFY": "0"}.items():
        if env.get(key) != required:
            raise ValueError(f"Requires enhanced visible configuration: {key}={required}")
    log = (work / "stderr.log").read_text(errors="replace")
    boundaries = [re.findall(r"\[script\] mark " + name + r" vs=(\d+)", log)
                  for name in ("perf_begin", "perf_end")]
    if any(len(matches) != 1 for matches in boundaries):
        raise ValueError("Expected one pair of level measurement boundaries")
    first, last = (int(matches[0]) for matches in boundaries)
    if first >= last:
        raise ValueError("Invalid level measurement interval")
    with (work / "timing.csv").open(newline="") as file:
        rows = [row for row in csv.DictReader(file)
                if row["function"] == "physics" and row["caller"] == "00292fb0"
                and first <= int(row["vs"]) <= last]
    if len(rows) < 2000 or any(not 0 < float(row["delta"]) < 0.017 for row in rows):
        raise ValueError("Insufficient sustained native60 physics samples")
    if [int(row["vs"]) for row in rows] != list(range(first, last + 1)):
        raise ValueError("Expected one physics update per field throughout the measurement")
    intervals = [(int(b["host_us"]) - int(a["host_us"])) / 1000
                 for a, b in zip(rows, rows[1:])]
    if any(interval <= 0 for interval in intervals):
        raise ValueError("Non-increasing host timestamps")
    elapsed = (int(rows[-1]["host_us"]) - int(rows[0]["host_us"])) / 1e6
    return dict(process_priority=env.get("RUN_PROCESS_PRIORITY", "unrecorded"),
                first_field=first, last_field=last, updates=len(rows), wall_seconds=elapsed,
                updates_per_second=(len(rows) - 1) / elapsed,
                median_ms=statistics.median(intervals), p95_ms=sorted(intervals)[int(len(intervals) * .95)],
                timesteps=sorted({row["delta"] for row in rows}),
                scope="Enhanced level only. Physics updates are not a distinct rendered-frame measurement.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("work", type=Path, help="Private completed run directory")
    args = parser.parse_args()
    print(json.dumps(summarize(args.work), indent=2))
