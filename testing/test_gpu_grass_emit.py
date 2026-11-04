#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare GPU grass emission with private accelerated packet captures.

Input records contain only emitter constants and scatter weights. Expected
packets are never uploaded. The diagnostic reads results back for comparison;
this does not claim a live renderer bypass or a performance improvement.
"""
import argparse
from collections import Counter
import ctypes as c
import hashlib
import json
import math
import os
from pathlib import Path
import struct

from test_gpu_dual_transform import private, digest


def packet_geometry(packet):
    for offset, loops, registers, count in ((0, 2, 0xe, 1), (3, 1, 2, 1), (5, 8, 0x4124, 4),
                                             (38, 2, 0xe, 1), (41, 1, 2, 1), (43, 8, 0x4124, 4)):
        low, high = struct.unpack_from("<2Q", packet, offset * 16)
        if (low & 0x7fff, low >> 58 & 3, low >> 60, high) != (loops, 0, count, registers):
            raise ValueError("Unqualified grass GIF framing")
    result = bytearray()
    for field in range(2):
        for sprite in range(8):
            base = (6 + field * 38 + sprite * 4) * 16
            result.extend(packet[base:base + 16])
            result.extend(packet[base + 48:base + 64])
    for sprite in range(8):
        first, second = (8 + sprite * 4) * 16, (46 + sprite * 4) * 16
        if packet[first:first + 16] != packet[second:second + 16]:
            raise ValueError("Field-dependent grass color")
        result.extend(packet[first:first + 16])
    return bytes(result)


def run(prefix, dll, report_path, capture_path):
    prefix, dll, report_path, capture_path = (private(p) for p in (prefix, dll, report_path, capture_path))
    if report_path.exists() or capture_path.exists():
        raise FileExistsError("Use fresh private report and capture paths")
    inputs = Path(str(prefix) + "-inputs.bin").read_bytes()
    packets = Path(str(prefix) + "-packets.bin").read_bytes()
    count = len(inputs) // 256
    if not 0 < count <= 65536 or len(inputs) != count * 256 or len(packets) != count * 1216:
        raise ValueError("Invalid grass emitter fixture lengths")
    if not all(math.isfinite(x[0]) for x in struct.iter_unpack("<f", inputs)):
        raise ValueError("Non-finite grass emitter input")
    expected = b"".join(packet_geometry(packets[i:i + 1216]) for i in range(0, len(packets), 1216))
    os.environ["PS2X_HWGS_API"] = "d3d12"
    os.environ["PS2X_HWGS_DIRECT"] = "0"
    os.chdir(dll.parent)
    with os.add_dll_directory(str(dll.parent)):
        gs = c.CDLL(str(dll))
        gs.tf_gs_open.argtypes = [c.c_int, c.c_int, c.c_char_p, c.c_void_p, c.c_uint32]
        gs.tf_gs_open.restype = c.c_int
        gs.tf_gs_close.argtypes = []
        gs.tf_gs_close.restype = None
        probe = gs.tf_gs_gpu_grass_emit_probe
        probe.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32]
        probe.restype = c.c_int
        error, output = c.create_string_buffer(4096), c.create_string_buffer(count * 640)
        source = c.create_string_buffer(inputs)
        if not gs.tf_gs_open(1, 0, str(dll.parent / "resources").encode(), error, len(error)):
            raise RuntimeError(error.value.decode(errors="replace"))
        try:
            if not probe(source, count, output, len(output), error, len(error)):
                raise RuntimeError(error.value.decode(errors="replace"))
        finally:
            gs.tf_gs_close()
    actual = output.raw
    mismatches, first = Counter(), []
    for i in range(0, len(expected), 4):
        a, b = struct.unpack_from("<I", actual, i)[0], struct.unpack_from("<I", expected, i)[0]
        if a != b:
            word = i // 4 % 160
            mismatches[("position_" if word < 128 else "color_") + str(word % 4)] += 1
            if len(first) < 12:
                first.append({"batch": i // 640, "word": word, "actual": a, "expected": b})
    report = {"exact": actual == expected, "batches": count, "sprites": count * 8,
              "endpoints": count * 32, "mismatched_words": dict(mismatches), "first_mismatches": first,
              "dll_sha256": digest(dll.read_bytes()), "inputs_sha256": digest(inputs),
              "packets_sha256": digest(packets), "output_sha256": digest(actual),
              "scope": "Offline GPU grass emission; no live geometry bypass or FPS claim"}
    if report["exact"]:
        header = struct.pack("<4I32s", 0x47524750, 1, count, 640, hashlib.sha256(inputs).digest())
        with capture_path.open("xb") as stream:
            stream.write(header + actual)
        report["gpu_capture_sha256"] = digest(header + actual)
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return 0 if report["exact"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", required=True, type=Path)
    parser.add_argument("--dll", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--capture", required=True, type=Path)
    args = parser.parse_args()
    return run(args.prefix, args.dll, args.report, args.capture)


if __name__ == "__main__":
    raise SystemExit(main())
