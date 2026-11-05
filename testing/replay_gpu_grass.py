#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Remove middle grass iterations and carry actual GPU output through accelerated replay."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess

from test_gpu_dual_transform import private, digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("prefix", "gpu-capture", "gpu-report", "fixtures", "runner", "report"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--defer-memory", action="store_true",
                        help="Keep skipped VU words unresolved and check ordered source-based reconstruction")
    parser.add_argument("--require-prologue", action="store_true",
                        help="Require the earlier GPU emission hook and its rejection checks")
    args = parser.parse_args()
    if args.require_prologue and not args.defer_memory:
        parser.error("--require-prologue requires --defer-memory")
    prefix, gpu, gpu_report, fixtures, runner, report = (
        private(p) for p in (args.prefix, args.gpu_capture, args.gpu_report, args.fixtures, args.runner, args.report))
    csv_path, log_path = report.with_suffix(".csv"), report.with_suffix(".log")
    if any(p.exists() for p in (report, csv_path, log_path)):
        raise FileExistsError("Use fresh private replay report paths")
    inputs_path = Path(str(prefix) + "-inputs.bin")
    inputs, packets, capture = (inputs_path.read_bytes(), Path(str(prefix) + "-packets.bin").read_bytes(), gpu.read_bytes())
    proof = json.loads(gpu_report.read_text())
    count = len(inputs) // 256
    header = struct.pack("<4I32s", 0x47524750, 1, count, 640, hashlib.sha256(inputs).digest())
    if (not proof.get("exact") or proof["batches"] != count or len(inputs) != count * 256 or
            proof["inputs_sha256"] != digest(inputs) or proof["packets_sha256"] != digest(packets) or
            proof["gpu_capture_sha256"] != digest(capture) or capture[:48] != header or
            len(capture) != 48 + count * 640 or proof["output_sha256"] != digest(capture[48:])):
        raise ValueError("Grass GPU output is not bound to these uploaded inputs and reference packets")
    environment = os.environ.copy()
    environment.pop("PS2X_GRASS_CAPTURE", None)
    environment.pop("PS2X_GRASS_DEFER", None)
    environment.pop("PS2X_TF_GPU_GRASS", None)
    environment.update(PS2X_GRASS_GPU_CAPTURE=str(gpu), PS2X_GRASS_GPU_INPUTS=str(inputs_path))
    if args.defer_memory:
        environment["PS2X_GRASS_DEFER"] = "1"
        environment["PS2X_TF_GPU_GRASS"] = "1"
    with log_path.open("xb") as log:
        result = subprocess.run([str(runner), str(fixtures), str(csv_path)], env=environment,
                                stdout=log, stderr=subprocess.STDOUT, check=False)
    text = log_path.read_text(errors="replace")
    match = re.search(r"GRASS_TAIL calls=(\d+) active_transfers=(\d+) emitted=(\d+) gpu_replaced=(\d+) "
                      r"skipped_iterations=(\d+)", text)
    rows = []
    if csv_path.exists():
        with csv_path.open(newline="") as file:
            rows = list(csv.DictReader(file))
    passed = (result.returncode == 0 and match is not None and rows and
              all(all(row[key] == "1" for key in ("data", "state", "code", "packets", "control")) for row in rows))
    metrics = dict(zip(("calls", "active_transfers", "emitted", "gpu_replaced", "skipped_iterations"),
                       map(int, match.groups()))) if match else {}
    passed = bool(passed and metrics["calls"] == count and metrics["emitted"] == count and
                  metrics["gpu_replaced"] == count and metrics["skipped_iterations"] == count * 2)
    ownership = {}
    prologue = {}
    if args.defer_memory:
        match = re.search(r"GRASS_DEFER packets=(\d+) recorded=(\d+) overwritten_qwords=(\d+) "
                          r"materialized_sprites=(\d+) materialized_qwords=(\d+) dirty=(\d+)", text)
        if match:
            ownership = dict(zip(("packets", "recorded", "overwritten_qwords", "materialized_sprites",
                                  "materialized_qwords", "dirty"), map(int, match.groups())))
        passed = bool(passed and ownership and ownership["packets"] == count and
                      ownership["recorded"] == count and ownership["dirty"] == 0)
        match = re.search(r"GRASS_PROLOGUE skipped=(\d+) guard_checks=(\d+)", text)
        if match:
            prologue = dict(zip(("skipped", "guard_checks"), map(int, match.groups())))
    if args.require_prologue:
        passed = bool(passed and prologue and prologue["skipped"] == count and prologue["guard_checks"] == 15)
    report.write_text(json.dumps({"passed": passed, "fixtures": len(rows), **metrics, "deferred_memory": ownership,
        "prologue": prologue,
        "exit_code": result.returncode, "inputs_sha256": digest(inputs), "gpu_capture_sha256": digest(capture),
        "gpu_report_sha256": digest(gpu_report.read_bytes()), "runner_sha256": digest(runner.read_bytes()),
        "fixture_list_sha256": digest(fixtures.read_bytes()), "csv_sha256": digest(csv_path.read_bytes()) if rows else None,
        "scope": ("Accelerated shortcut with unresolved VU geometry, ordered CPU fallback checked against actual GPU output; not live FPS"
                  if args.defer_memory else "Accelerated replay with actual GPU geometry and no full CPU emission oracle; not live rendering or FPS")
    }, indent=2))
    print(report.read_text())
    if not passed:
        print(text[-4000:])
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
