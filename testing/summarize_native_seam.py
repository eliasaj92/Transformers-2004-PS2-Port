#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarize a private observation trace; fingerprints are leads, not batch proof.

An optional private transformers_extract manifest supplies microprogram names.
No game data is emitted. Keep the input trace and extracted programs private.
"""
import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
from pathlib import Path


def fingerprint(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f"{value:016x}"


def program_index(manifest):
    index = defaultdict(set)
    if manifest is None:
        return index
    content = json.loads(manifest.read_text(encoding="utf-8"))
    for program in content["programs"]:
        data = (manifest.parent / program["file"]).read_bytes()
        if hashlib.sha256(data).hexdigest() != program["sha256"]:
            raise ValueError(f"Program does not match manifest: {program['name']}")
        for offset in range(0, len(data) - 63, 8):
            index[fingerprint(data[offset:offset + 64])].add((program["name"], offset))
    return index


def summarize(path, manifest=None):
    with path.open(newline="", encoding="utf-8") as file:
        rows = list(csv.DictReader(file))
    if not rows or any(not row.get("mode") for row in rows):
        raise ValueError("Empty or incomplete trace; wait for orderly game shutdown")
    if [int(r["sequence"]) for r in rows] != list(range(1, len(rows) + 1)):
        raise ValueError("Trace sequence has gaps or duplicate records")
    programs = program_index(manifest)
    constants = [r for r in rows if r["event"] == "constants"]
    fingerprints = {r["hash"] for r in constants}
    uploads = [r for r in rows if r["event"] == "unpack56"]
    matched = [r for r in uploads if r["hash"] in fingerprints]
    starts = Counter(r["code_hash"] for r in rows if r["event"] == "mscal")
    first_after = Counter()
    candidate = False
    for row in rows:
        if row["event"] == "unpack56":
            candidate = row["hash"] in fingerprints
        elif row["event"] == "mpg":
            candidate = False
        elif row["event"] in ("mscal", "mscnt"):
            if candidate and row["event"] == "mscal":
                first_after[row["code_hash"]] += 1
            candidate = False
    return {
        "rows": len(rows),
        "at_record_limit": len(rows) >= 250000,
        "presenter_fields": [min(int(r["present_vs"]) for r in rows),
                             max(int(r["present_vs"]) for r in rows)],
        "events": dict(Counter(r["event"] for r in rows)),
        "constant_callers": dict(Counter(r["caller"] for r in constants)),
        "unique_grass_fingerprints": len(fingerprints),
        "unpack56_matched_grass_fingerprint": len(matched),
        "unpack56_unmatched_grass_fingerprint": len(uploads) - len(matched),
        "matched_upload_modes": dict(Counter(r["mode"] for r in matched)),
        "allocator_callers": dict(Counter(r["caller"] for r in rows
                                          if r["event"] == "allocation").most_common()),
        "launch_fingerprints": [
            {"fingerprint": value, "starts": count,
             "first_start_after_observed_grass_upload": first_after[value],
             "disc_candidates": [{"name": name, "offset": offset}
                                 for name, offset in sorted(programs[value])]}
            for value, count in starts.most_common()
        ],
        "limitations": [
            "Presenter fields are asynchronous labels, not batch identities.",
            "Repeated payloads and shared or relocated code can make fingerprints ambiguous.",
            "First-start adjacency omits other UNPACK sizes and is not ownership or coverage proof.",
            "No output geometry, persistent VU state, or mesh boundaries are verified by this report.",
        ],
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--program-manifest", type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.trace, args.program_manifest), indent=2))
