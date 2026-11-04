#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare already validated private VU captures by exact invocation input.

Run inspect_native_seam_capture.py first to validate each capture's trace,
files and transfer/fallback flags. Presenter labels are deliberately excluded.
This comparison does not establish surrounding GS or hidden pipeline state.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from inspect_native_seam_capture import read_vu


def canonical(path):
    header, code, data, state = read_vu(path)
    # VU1State's validated 664-byte ABI: omit only compiler padding. Preserve
    # registers, flags, PC, absolute cycles, TOP/ITOP and branch state.
    state = state[:638] + state[640:649] + state[652:660]
    return struct.pack("<4I", *header) + code + data + state


def inventory(prefix):
    records = {}
    stem = prefix.name + "-vu-"
    files = sorted(p for p in prefix.parent.iterdir() if p.name.startswith(stem) and p.name.endswith("-pre.bin"))
    if not files:
        raise ValueError(f"No VU capture files for {prefix}")
    for pre in files:
        base = str(pre)[:-7]
        key = hashlib.sha256(canonical(pre)).hexdigest()
        output = hashlib.sha256(canonical(Path(base + "post.bin")) + Path(base + "gif.bin").read_bytes()).hexdigest()
        if key in records and records[key][0] != output:
            raise ValueError(f"Identical captured input has different outputs: {pre.name}")
        records[key] = output, pre.name
    return records


def compare(left, right):
    first, second = inventory(left), inventory(right)
    common = first.keys() & second.keys()
    differences = [{"left": first[key][1], "right": second[key][1]}
                   for key in sorted(common) if first[key][0] != second[key][0]]
    return {"left_unique_inputs": len(first), "right_unique_inputs": len(second),
            "shared_inputs": len(common), "different_outputs": differences,
            "comparison": "Full code/data and semantic VU state, including cycles; exact ordered GIF bytes. Struct padding excluded.",
            "pass": bool(common) and not differences}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left_prefix", type=Path)
    parser.add_argument("right_prefix", type=Path)
    args = parser.parse_args()
    report = compare(args.left_prefix, args.right_prefix)
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if report["pass"] else 1)
