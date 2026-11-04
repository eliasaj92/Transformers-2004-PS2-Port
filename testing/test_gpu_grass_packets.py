#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check ordered deferred grass packets against enhanced captured GIF at 4x resolution."""
import argparse
import ctypes as c
import json
import os
from pathlib import Path

from test_gpu_dual_transform import private, digest
from test_gpu_grass_native import load


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("prepared", "prefix", "dll", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--limit", type=int, default=256)
    args = parser.parse_args()
    prepared, prefix, dll, report = (private(p) for p in (args.prepared, args.prefix, args.dll, args.report))
    if report.exists() or not 128 <= args.limit <= 1024:
        raise ValueError("Use a fresh report and a packet limit of 128..1024")
    meta, inputs, expected = load(prepared)
    captured_inputs = Path(str(prefix) + "-inputs.bin").read_bytes()
    packets = Path(str(prefix) + "-packets.bin").read_bytes()
    if (len(packets) != meta["batches"] * 1216 or len(captured_inputs) != meta["batches"] * 256 or
            digest(captured_inputs) != meta["capture_inputs_sha256"] or
            digest(packets) != meta["capture_packets_sha256"] or
            any(inputs[i * 320:i * 320 + 256] != captured_inputs[i * 256:(i + 1) * 256]
                for i in range(meta["batches"]))):
        raise ValueError("Raw packets do not belong to the authenticated enhanced capture")
    count = min(args.limit, meta["batches"])
    indices = [i * meta["batches"] // count for i in range(count)]
    selected = lambda data, stride: b"".join(data[i * stride:(i + 1) * stride] for i in indices)
    inputs, expected, packets = selected(inputs, 320), selected(expected, 1024), selected(packets, 1216)
    os.environ["PS2X_HWGS_API"], os.environ["PS2X_HWGS_DIRECT"] = "d3d12", "0"
    os.chdir(dll.parent)
    with os.add_dll_directory(str(dll.parent)):
        gs = c.CDLL(str(dll))
        gs.tf_gs_open.argtypes = [c.c_int, c.c_int, c.c_char_p, c.c_void_p, c.c_uint32]
        gs.tf_gs_open.restype = c.c_int
        gs.tf_gs_close.argtypes, gs.tf_gs_close.restype = [], None
        probe = gs.tf_gs_gpu_grass_packet_probe
        probe.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_void_p, c.POINTER(c.c_uint64), c.c_void_p, c.c_uint32]
        probe.restype = c.c_int
        error, stats = c.create_string_buffer(4096), (c.c_uint64 * 18)()
        source, reference, raw = (c.create_string_buffer(data) for data in (inputs, expected, packets))
        if not gs.tf_gs_open(4, 0, str(dll.parent / "resources").encode(), error, len(error)):
            raise RuntimeError(error.value.decode(errors="replace"))
        try:
            passed = bool(probe(source, count, reference, raw, stats, error, len(error)))
        finally:
            gs.tf_gs_close()
    names = ("native_packets", "gpu_draws", "cpu_materializations", "declined_fields", "bounds_declined",
             "output_declined", "latch_checks", "painted_pixels", "differing_pixels", "completed_modes",
             "full_resolution_differing_pixels", "full_resolution_compared_pixels", "pool_draws", "reserved",
             "grass_dispatches", "grass_batches", "invalid_framing_checks", "ordinary_packets")
    result = {"passed": passed, "batches": count, **dict(zip(names, stats)), "selected_indices": indices,
              "dll_sha256": digest(dll.read_bytes()), "capture_inputs_sha256": meta["capture_inputs_sha256"],
              "capture_packets_sha256": meta["capture_packets_sha256"], "fixture_sha256": meta["sha256"],
              "scope": "GS-thread packet handoff, GPU-resident grass and source fallback from enhanced captures; not live FPS"}
    if error.value:
        result["error"] = error.value.decode(errors="replace")
    report.write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items() if k != "selected_indices"}, indent=2))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
