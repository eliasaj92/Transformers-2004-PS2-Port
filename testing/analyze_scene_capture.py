#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure image changes in a bounded direct-output capture, independently of host FPS."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from test_gpu_dual_transform import private


def analyze(prefix):
    metadata = Path(str(prefix) + ".json")
    raw = Path(str(prefix) + ".rgba")
    meta = json.loads(metadata.read_text())
    width, height, frames = meta["width"], meta["height"], meta["frames"]
    if meta["version"] != 1 or meta["format"] != "RGBA8" or not 2 <= len(frames) <= 64:
        raise ValueError("Unsupported or incomplete scene capture")
    if width < 32 or height < 32 or raw.stat().st_size != len(frames) * width * height * 4:
        raise ValueError("Image dimensions or complete-pixel count do not match metadata")
    if [f["index"] for f in frames] != list(range(len(frames))):
        raise ValueError("Capture indices are not consecutive")
    if [f["tick"] for f in frames] != list(range(frames[0]["tick"], frames[-1]["tick"] + 1)):
        raise ValueError("Capture fields are not consecutive")
    if any(a["sequence"] >= b["sequence"] for a, b in zip(frames, frames[1:])):
        raise ValueError("Publication sequence did not advance")
    images = np.memmap(raw, dtype=np.uint8, mode="r", shape=(len(frames), height, width, 4))
    hashes = [hashlib.sha256(image).hexdigest() for image in images]
    # Exclude the outer HUD/borders. Keep row parities separate so alternate-row
    # changes are visible instead of being mistaken for whole-picture updates.
    y0, y1 = height // 10, height * 9 // 10
    x0, x1 = width // 10, width * 9 // 10
    pairs = []
    for i in range(1, len(frames)):
        a, b = images[i - 1, y0:y1, x0:x1, :3], images[i, y0:y1, x0:x1, :3]
        changed = np.any(a != b, axis=2)
        changed_rows = np.any(changed, axis=1)
        tiles = [float(part.mean()) for rows in np.array_split(changed, 2, axis=0)
                 for part in np.array_split(rows, 2, axis=1)]
        pairs.append(dict(first_tick=frames[i - 1]["tick"], last_tick=frames[i]["tick"],
                          identical_full_image=hashes[i - 1] == hashes[i],
                          changed_crop_pixels=float(changed.mean()), quadrant_change_fractions=tiles,
                          changed_even_rows=float(changed_rows[y0 % 2::2].mean()),
                          changed_odd_rows=float(changed_rows[1 - y0 % 2::2].mean())))
    full_hash = hashlib.sha256()
    with raw.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            full_hash.update(chunk)
    return dict(width=width, height=height, frames=len(frames), first_tick=frames[0]["tick"],
                last_tick=frames[-1]["tick"], unique_full_images=len(set(hashes)),
                adjacent_full_duplicates=sum(a == b for a, b in zip(hashes, hashes[1:])),
                two_field_full_duplicates=sum(a == b for a, b in zip(hashes, hashes[2:])),
                adjacent_unchanged_scene_crops=sum(p["changed_crop_pixels"] == 0 for p in pairs),
                minimum_changed_even_rows=min(p["changed_even_rows"] for p in pairs),
                minimum_changed_odd_rows=min(p["changed_odd_rows"] for p in pairs),
                minimum_changed_crop_pixels=min(p["changed_crop_pixels"] for p in pairs),
                scenes=sorted({f["scene"] for f in frames}), display_fbps=sorted({f["display_fbp"] for f in frames}),
                metadata_sha256=hashlib.sha256(metadata.read_bytes()).hexdigest(), pixels_sha256=full_hash.hexdigest(),
                image_sha256=hashes, pairs=pairs,
                scope="Complete output-image cadence during a diagnostic capture; combine with separate timing traces "
                      "collected with image capture and sampling disabled, plus simulation-timing evidence. "
                      "These measurements alone do not establish true 60 FPS.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    prefix, report = private(args.prefix), private(args.report)
    if report.exists():
        raise ValueError("Use a fresh image-cadence report")
    result = analyze(prefix)
    report.write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items() if k not in ("pairs", "image_sha256")}, indent=2))


if __name__ == "__main__":
    main()
