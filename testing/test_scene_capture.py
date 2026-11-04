#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify complete-image GPU capture using synthetic pixels and a deliberate duplicate."""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path

from test_gpu_dual_transform import private
from analyze_scene_capture import analyze


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    dll, prefix, report = (private(p) for p in (args.dll, args.prefix, args.report))
    if report.exists():
        raise ValueError("Use a fresh diagnostic report")
    os.environ["PS2X_HWGS_API"], os.environ["PS2X_HWGS_DIRECT"] = "d3d12", "0"
    os.environ.pop("PS2X_TF_SCENE_CAPTURE", None)
    os.environ.pop("PS2X_TF_SCENE_RANGE", None)
    os.chdir(dll.parent)
    with os.add_dll_directory(str(dll.parent)):
        gs = c.CDLL(str(dll))
        gs.tf_gs_open.argtypes = [c.c_int, c.c_int, c.c_char_p, c.c_void_p, c.c_uint32]
        gs.tf_gs_open.restype = c.c_int
        gs.tf_gs_close.argtypes, gs.tf_gs_close.restype = [], None
        gs.tf_gs_scene_capture_probe.argtypes = [c.c_char_p, c.c_void_p, c.c_uint32]
        gs.tf_gs_scene_capture_probe.restype = c.c_int
        error = c.create_string_buffer(4096)
        if not gs.tf_gs_open(4, 0, str(dll.parent / "resources").encode(), error, len(error)):
            raise RuntimeError(error.value.decode(errors="replace"))
        try:
            if not gs.tf_gs_scene_capture_probe(str(prefix).encode(), error, len(error)):
                raise RuntimeError(error.value.decode(errors="replace"))
        finally:
            gs.tf_gs_close()
    meta = json.loads(Path(str(prefix) + ".json").read_text())
    pixels = Path(str(prefix) + ".rgba").read_bytes()
    width, height = meta["width"], meta["height"]
    assert (width, height, len(meta["frames"])) == (225, 129, 4)
    assert [row["tick"] for row in meta["frames"]] == [1, 2, 3, 4]
    assert [row["sequence"] for row in meta["frames"]] == [101, 102, 103, 104]
    assert [row["index"] for row in meta["frames"]] == [0, 1, 2, 3]
    assert all(row["scene"] == 7 for row in meta["frames"])
    assert [row["display_fbp"] for row in meta["frames"]] == [14, 0, 14, 0]
    assert all(a["host_us"] <= b["host_us"] for a, b in zip(meta["frames"], meta["frames"][1:]))
    expected = bytearray()
    for phase in (1, 2, 2, 4):
        for y in range(height):
            for x in range(width):
                expected.extend(((x + phase * 17) & 255, (y + phase * 11) & 255,
                                 ((x ^ y) + phase * 7) & 255, 255))
    assert pixels == expected, "GPU capture differs from its exact synthetic source pixels"
    frame_bytes = width * height * 4
    hashes = [hashlib.sha256(pixels[i * frame_bytes:(i + 1) * frame_bytes]).hexdigest() for i in range(4)]
    assert hashes[0] != hashes[1] == hashes[2] != hashes[3]
    cadence = analyze(prefix)
    assert cadence["unique_full_images"] == 3 and cadence["adjacent_full_duplicates"] == 1
    assert cadence["adjacent_unchanged_scene_crops"] == 1
    # Distinct images made by alternating row updates must remain distinguishable
    # from complete-picture motion. This fixture contains no game data.
    import numpy as np
    half_prefix = Path(str(prefix) + "-halfrows")
    half_meta, half_raw = Path(str(half_prefix) + ".json"), Path(str(half_prefix) + ".rgba")
    assert not half_meta.exists() and not half_raw.exists()
    half = np.zeros((3, 64, 64, 4), dtype=np.uint8)
    half[:, :, :, 3] = 255
    half[1, ::2, :, :3] = 128
    half[2] = half[1]
    half[2, 1::2, :, :3] = 64
    half_raw.write_bytes(half.tobytes())
    half_meta.write_text(json.dumps(dict(version=1, format="RGBA8", width=64, height=64,
        frames=[dict(index=i, sequence=i + 1, tick=i + 1, scene=1, display_fbp=0) for i in range(3)])))
    half_result = analyze(half_prefix)
    assert half_result["unique_full_images"] == 3 and half_result["adjacent_full_duplicates"] == 0
    assert [(p["changed_even_rows"], p["changed_odd_rows"]) for p in half_result["pairs"]] == [(1, 0), (0, 1)]
    result = dict(passed=True, frames=4, compared_bytes=len(pixels), unique_images=len(set(hashes)),
                  deliberate_adjacent_duplicates=1, alternating_row_changes_detected=True,
                  dll_sha256=hashlib.sha256(dll.read_bytes()).hexdigest(),
                  scope="Synthetic bounded GPU image readback; not gameplay FPS")
    report.write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
