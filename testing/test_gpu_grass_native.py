#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check native GS sprite records emitted on the GPU from enhanced grass captures."""
import argparse
from collections import Counter
import ctypes as c
import json
import os
from pathlib import Path
import struct

from test_gpu_grass_emit import packet_geometry
from test_gpu_dual_transform import private, digest


def prepare(prefix, destination):
    prefix, destination = private(prefix), private(destination)
    if destination.exists():
        raise FileExistsError("Use a fresh private native-grass fixture directory")
    inputs = Path(str(prefix) + "-inputs.bin").read_bytes()
    packets = Path(str(prefix) + "-packets.bin").read_bytes()
    count = len(inputs) // 256
    if not 0 < count <= 65536 or len(inputs) != count * 256 or len(packets) != count * 1216:
        raise ValueError("Invalid captured grass lengths")
    native_inputs, expected = bytearray(), bytearray()
    prior_color = struct.pack("<4I", 27, 59, 113, 127)
    clipped = 0
    for batch in range(count):
        packet = packets[batch * 1216:(batch + 1) * 1216]
        packet_geometry(packet)  # Authenticate the six-tag framing and field colors.
        initial_colors = (prior_color, packet[36 * 16:37 * 16])
        native_inputs.extend(inputs[batch * 256:(batch + 1) * 256])
        native_inputs.extend(packet[4 * 16:5 * 16] + packet[42 * 16:43 * 16])
        native_inputs.extend(b"".join(initial_colors))
        for field in range(2):
            stq = struct.unpack_from("<4I", packet, (4 + field * 38) * 16)
            color = struct.unpack("<4I", initial_colors[field])
            for sprite in range(8):
                base = (6 + field * 38 + sprite * 4) * 16
                first, last = struct.unpack_from("<4I", packet, base), struct.unpack_from("<4I", packet, base + 48)
                if first[3] != last[3]:
                    raise ValueError("Grass endpoints have different clipping flags")
                if first[3] & 0x8000:
                    last = first  # GPU native mode suppresses ADC sprites with zero area.
                    clipped += 1
                for endpoint, position in enumerate((first, last)):
                    if endpoint:
                        stq = struct.unpack_from("<4I", packet, base + 16)
                        color = struct.unpack_from("<4I", packet, base + 32)
                        x, y = ((0.5, 0.5) if sprite & 1 else
                                (float(sprite in (2, 4)), float(sprite in (4, 6))))
                        if stq[:3] != struct.unpack("<3I", struct.pack("<3f", x, y, 1.0)):
                            raise ValueError("Unexpected grass texture-coordinate pattern")
                    rgba = sum((channel & 255) << (8 * i) for i, channel in enumerate(color))
                    expected.extend(struct.pack("<8I", stq[0], stq[1], rgba, stq[2] or 0x00800000,
                        (position[0] & 65535) | ((position[1] & 65535) << 16),
                        (position[2] >> 4) & 0xffffff, 0, (position[3] >> 4) & 255))
        prior_color = packet[74 * 16:75 * 16]
    destination.mkdir(parents=True)
    payloads = {"inputs.bin": native_inputs, "expected.bin": expected}
    for name, data in payloads.items():
        (destination / name).write_bytes(data)
    meta = {"version": 1, "batches": count, "clipped_field_sprites": clipped,
            "capture_inputs_sha256": digest(inputs), "capture_packets_sha256": digest(packets),
            "sha256": {name: digest(data) for name, data in payloads.items()}}
    (destination / "manifest.json").write_text(json.dumps(meta, indent=2))
    print(json.dumps({key: meta[key] for key in ("batches", "clipped_field_sprites")}, indent=2))


def load(prepared):
    prepared = private(prepared)
    meta = json.loads((prepared / "manifest.json").read_text())
    inputs, expected = ((prepared / name).read_bytes() for name in ("inputs.bin", "expected.bin"))
    count = meta["batches"]
    if (meta["version"] != 1 or not 0 < count <= 65536 or len(inputs) != count * 320 or len(expected) != count * 1024 or
            meta["sha256"] != {"inputs.bin": digest(inputs), "expected.bin": digest(expected)}):
        raise ValueError("Native grass fixture identity or layout changed")
    return meta, inputs, expected


def run(prepared, dll, report_path):
    prepared, dll, report_path = (private(p) for p in (prepared, dll, report_path))
    if report_path.exists():
        raise FileExistsError("Use a fresh private diagnostic report")
    meta, inputs, expected = load(prepared)
    count = meta["batches"]
    os.environ["PS2X_HWGS_API"], os.environ["PS2X_HWGS_DIRECT"] = "d3d12", "0"
    os.chdir(dll.parent)
    with os.add_dll_directory(str(dll.parent)):
        gs = c.CDLL(str(dll))
        gs.tf_gs_open.argtypes = [c.c_int, c.c_int, c.c_char_p, c.c_void_p, c.c_uint32]
        gs.tf_gs_open.restype = c.c_int
        gs.tf_gs_close.argtypes, gs.tf_gs_close.restype = [], None
        probe = gs.tf_gs_gpu_grass_native_probe
        probe.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32]
        probe.restype = c.c_int
        error, output, source = c.create_string_buffer(4096), c.create_string_buffer(len(expected)), c.create_string_buffer(inputs)
        bounds = gs.tf_gs_grass_bounds_probe
        bounds.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.POINTER(c.c_uint64), c.c_void_p, c.c_uint32]
        bounds.restype = c.c_int
        bounds_stats, reference = (c.c_uint64 * 3)(), c.create_string_buffer(expected)
        if not bounds(source, count, reference, bounds_stats, error, len(error)):
            raise RuntimeError(error.value.decode(errors="replace"))
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
            word = i // 4 % 8
            mismatches[str(word)] += 1
            if len(first) < 12:
                first.append({"batch": i // 1024, "vertex": i // 32 % 32, "word": word, "actual": a, "expected": b})
    report = {"exact": actual == expected, "batches": count, "native_vertices": count * 32,
              "bounded_packets": bounds_stats[0], "bounds_declined": bounds_stats[1], "bounds_checked_vertices": bounds_stats[2],
              "clipped_field_sprites": meta["clipped_field_sprites"], "mismatched_words": dict(mismatches),
              "first_mismatches": first, "dll_sha256": digest(dll.read_bytes()), "fixture_sha256": meta["sha256"],
              "scope": "Offline GPU native sprite packing and ADC suppression; not live rendering or FPS"}
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return 0 if report["exact"] else 1


def draw(prepared, dll, report_path, limit, frontend=False):
    prepared, dll, report_path = (private(p) for p in (prepared, dll, report_path))
    if report_path.exists() or not 1 <= limit <= 1024:
        raise ValueError("Use a fresh private report and a draw limit of 1..1024")
    meta, inputs, expected = load(prepared)
    count = min(limit, meta["batches"])
    selected = [i * meta["batches"] // count for i in range(count)]
    inputs = b"".join(inputs[i * 320:(i + 1) * 320] for i in selected)
    expected = b"".join(expected[i * 1024:(i + 1) * 1024] for i in selected)
    os.environ["PS2X_HWGS_API"], os.environ["PS2X_HWGS_DIRECT"] = "d3d12", "0"
    os.chdir(dll.parent)
    with os.add_dll_directory(str(dll.parent)):
        gs = c.CDLL(str(dll))
        gs.tf_gs_open.argtypes = [c.c_int, c.c_int, c.c_char_p, c.c_void_p, c.c_uint32]
        gs.tf_gs_open.restype = c.c_int
        gs.tf_gs_close.argtypes, gs.tf_gs_close.restype = [], None
        probe = gs.tf_gs_gpu_grass_frontend_probe if frontend else gs.tf_gs_gpu_grass_draw_probe
        probe.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.POINTER(c.c_uint64), c.c_void_p, c.c_uint32]
        probe.restype = c.c_int
        error, stats = c.create_string_buffer(4096), (c.c_uint64 * 18)()
        source, reference = c.create_string_buffer(inputs), c.create_string_buffer(expected)
        if not gs.tf_gs_open(4 if frontend else 1, 0, str(dll.parent / "resources").encode(), error, len(error)):
            raise RuntimeError(error.value.decode(errors="replace"))
        try:
            passed = bool(probe(source, count, reference, stats, error, len(error)))
        finally:
            gs.tf_gs_close()
    metrics = dict(zip(("native_submissions", "submit_callbacks", "cpu_materializations", "textured_callbacks",
                       "depth_callbacks", "blend_callbacks", "lod_callbacks", "painted_pixels", "differing_pixels",
                       "completed_modes", "full_resolution_differing_pixels", "full_resolution_compared_pixels",
                       "gpu_draws", "mipmap_filtered_callbacks", "grass_dispatches", "grass_batches",
                       "bounded_packets", "bounds_declined") if frontend else
                      ("allocations", "reuses", "dispatches", "gpu_draws", "painted_pixels", "differing_pixels",
                        "handler_replacements", "ordinary_draws", "grass_dispatches", "grass_batches", "compared_pixels"), stats))
    report = {"passed": passed, "batches": count, **metrics, "selected_indices": selected,
              "dll_sha256": digest(dll.read_bytes()), "fixture_sha256": meta["sha256"],
              "scope": ("Real GS sprite frontend with source-derived bounds and a diagnostic depth witness; no live offload or FPS claim"
                        if frontend else "GPU-resident sprite draws with decoded enhanced reference geometry; no geometry readback or FPS claim")}
    if error.value:
        report["error"] = error.value.decode(errors="replace")
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps({k: v for k, v in report.items() if k != "selected_indices"}, indent=2))
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prep = commands.add_parser("prepare")
    prep.add_argument("--prefix", type=Path, required=True)
    prep.add_argument("--output", type=Path, required=True)
    check = commands.add_parser("run")
    check.add_argument("--prepared", type=Path, required=True)
    check.add_argument("--dll", type=Path, required=True)
    check.add_argument("--report", type=Path, required=True)
    render = commands.add_parser("draw")
    render.add_argument("--prepared", type=Path, required=True)
    render.add_argument("--dll", type=Path, required=True)
    render.add_argument("--report", type=Path, required=True)
    render.add_argument("--limit", type=int, default=256)
    render.add_argument("--frontend", action="store_true")
    args = parser.parse_args()
    if args.command == "prepare":
        prepare(args.prefix, args.output)
        return 0
    if args.command == "draw":
        return draw(args.prepared, args.dll, args.report, args.limit, args.frontend)
    return run(args.prepared, args.dll, args.report)


if __name__ == "__main__":
    raise SystemExit(main())
