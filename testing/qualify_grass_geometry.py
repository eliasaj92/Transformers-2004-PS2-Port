#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Select private grass fixtures and compare native geometry with captured GIF.

This checks emitted attributes and complete ordered GIF records independently.
It does not qualify persistent VU state, renderer behavior or a runtime bypass.
Run inspect_native_seam_capture.py first for capture/trace integrity checks.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct

from import_grass_seam import GRASS_SHA256
from inspect_native_seam_capture import gif_packets, read_vu


def select(prefix, trace):
    with trace.open(newline="", encoding="utf-8") as file:
        rows = list(csv.DictReader(file))
    begins = {int(r["capture"]): r for r in rows if r["event"] == "vu_begin"}
    ends = {int(r["capture"]): r for r in rows if r["event"] == "vu_end"}
    if not begins or begins.keys() != ends.keys():
        raise ValueError("Missing VU capture begin/end pairs")
    fixtures = []
    for ticket in sorted(begins):
        if int(ends[ticket]["cycle"]) or int(ends[ticket]["mode"]):
            continue
        path = Path(f"{prefix}-vu-{ticket:06d}-pre.bin")
        header, code, data, _ = read_vu(path)
        if header[0] not in (0x80000010, 0x800000f8):
            continue
        if struct.unpack_from("<I", data, 64)[0] != 1 or struct.unpack_from("<2I", data, 80) != (624, 53):
            continue
        if hashlib.sha256(code[0x1380:0x1380 + 3872]).hexdigest() != GRASS_SHA256:
            continue
        fixtures.append(path)
    if not fixtures:
        raise ValueError("No qualified grass inputs in capture")
    return fixtures


def geometry_from_gif(path):
    """Decode the measured six-tag layout, preserving exact attribute bits."""
    result, empty = bytearray(), 0
    for source, packet in gif_packets(path):
        if source != 1:
            raise ValueError("Unqualified grass GIF path")
        if len(packet) == 16 and struct.unpack_from("<Q", packet)[0] & 0x7fff == 0:
            empty += 1
            continue
        if len(packet) != 1216:
            raise ValueError("Unqualified grass packet size")
        for offset, loops, registers, count in ((0, 2, 0xe, 1), (3, 1, 2, 1), (5, 8, 0x4124, 4),
                                                 (38, 2, 0xe, 1), (41, 1, 2, 1), (43, 8, 0x4124, 4)):
            low, high = struct.unpack_from("<2Q", packet, offset * 16)
            if (low & 0x7fff, low >> 58 & 3, low >> 60, high) != (loops, 0, count, registers):
                raise ValueError("Unqualified grass GIF tag")
        for base in (6, 44):
            for sprite in range(8):
                for vertex in (0, 3):
                    start = (base + sprite * 4 + vertex) * 16
                    result.extend(packet[start:start + 16])
        for sprite in range(8):
            first, second = (8 + sprite * 4) * 16, (46 + sprite * 4) * 16
            if packet[first:first + 16] != packet[second:second + 16]:
                raise ValueError("Unexpected field-dependent grass color")
            result.extend(packet[first:first + 16])
    return bytes(result), empty


def compare(fixtures, native_directory):
    results = []
    for pre in fixtures:
        native = native_directory / (pre.stem + "-native.bin")
        raw = native.read_bytes()
        if len(raw) < 32:
            raise ValueError("Truncated native geometry header")
        magic, version, groups, triangles, culled, subdivisions, depth, stride = struct.unpack_from("<8I", raw)
        if (magic, version, stride) != (0x53415247, 1, 640) or len(raw) != 32 + groups * stride:
            raise ValueError("Invalid native geometry output")
        expected, empty = geometry_from_gif(Path(str(pre)[:-7] + "gif.bin"))
        actual = raw[32:]
        expected_packets = Path(str(pre)[:-7] + "gif.bin").read_bytes()
        actual_packets = (native_directory / (pre.stem + "-native-gif.bin")).read_bytes()
        row = {"fixture": pre.name, "groups": groups, "reference_groups": len(expected) // 640,
               "empty_reference_packets": empty, "exact": actual == expected,
               "complete_packets_exact": actual_packets == expected_packets,
               "packet_bytes": len(expected_packets),
               "triangles": triangles, "culled": culled, "subdivisions": subdivisions, "max_depth": depth}
        if actual != expected:
            first = next((index for index in range(0, min(len(actual), len(expected)), 4)
                          if actual[index:index + 4] != expected[index:index + 4]), None)
            row["first_mismatched_word"] = None if first is None else first // 4
        results.append(row)
    return {"fixtures": len(results), "exact_fixtures": sum(r["exact"] for r in results),
            "exact_packet_fixtures": sum(r["complete_packets_exact"] for r in results),
            "packet_bytes": sum(r["packet_bytes"] for r in results),
            "sprite_groups": sum(r["groups"] for r in results),
            "sprite_pairs_per_group": 8, "fields_per_group": 2,
            "pass": bool(results) and all(r["exact"] and r["complete_packets_exact"] for r in results), "results": results,
            "limitations": ["Compares complete emitted GIF records; surrounding renderer state is not captured.",
                            "No persistent VU-state or runtime-bypass qualification.",
                            "Coverage is restricted to these captured calls."]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--write-list", type=Path, help="Create a fresh private fixture list for the C++ driver")
    parser.add_argument("--native-directory", type=Path)
    args = parser.parse_args()
    if not args.write_list and not args.native_directory:
        parser.error("Provide --write-list or --native-directory")
    fixtures = select(args.prefix, args.trace)
    if args.write_list:
        with args.write_list.open("x", encoding="utf-8") as file:
            file.write("".join(str(path.resolve()) + "\n" for path in fixtures))
    if args.native_directory:
        report = compare(fixtures, args.native_directory)
        print(json.dumps(report, indent=2))
        raise SystemExit(0 if report["pass"] else 1)
    print(json.dumps({"selected_fixtures": len(fixtures)}))
