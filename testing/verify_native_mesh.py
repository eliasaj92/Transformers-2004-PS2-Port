# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent CPU projection/clipping/rasterization oracle for the native probe.

This consumes already-written GPU results. It never launches the game or the
probe. Readback and the CPU reference are validation only, not frame delivery.
Requires numpy and Pillow. Thresholds exclude a two-pixel triangle boundary,
where D3D12 subpixel rasterization rules differ from floating-point reference.
"""
import argparse
import json
from pathlib import Path
import struct

import numpy as np
from PIL import Image

from prepare_native_mesh import digest, require


def load_asset(directory):
    manifest = json.loads((directory / "manifest.json").read_text())
    data = (directory / "asset.bin").read_bytes()
    require(digest(data) == manifest["asset_sha256"], "Native asset changed")
    magic, nv, ni, width, height, flags, reserved = struct.unpack_from("<8s6I", data)
    require(magic == b"TFNMESH1" and flags == reserved == 0 and
            len(data) == 32 + nv * 36 + ni * 4 + width * height * 4, "Invalid native asset")
    vertices = np.frombuffer(data, dtype="<f4", count=nv * 9, offset=32).reshape(nv, 9).astype(np.float64)
    indices = np.frombuffer(data, dtype="<u4", count=ni, offset=32 + nv * 36).reshape(-1, 3)
    texture = np.frombuffer(data, dtype=np.uint8, count=width * height * 4,
                            offset=32 + nv * 36 + ni * 4).reshape(height, width, 4).astype(np.float64) / 255
    # Independently reconstruct all triangle strips from the source manifest.
    # This detects bridging separate strips, missing triangles, or parity loss.
    reference_indices = []
    for strip in manifest["triangle_strips"]:
        for i in range(len(strip) - 2):
            a, b, c = strip[i:i + 3]
            if len({a, b, c}) == 3:
                reference_indices.append([a, b, c] if i % 2 == 0 else [b, a, c])
    require(np.array_equal(indices, reference_indices), "Whole-mesh topology differs from authored strips")
    require(nv == manifest["vertices"] and len(indices) == manifest["triangles"], "Incomplete asset")
    return vertices, indices, texture, manifest


PLANES = np.array([[1, 0, 0, 1], [-1, 0, 0, 1], [0, 1, 0, 1],
                   [0, -1, 0, 1], [0, 0, 1, 0], [0, 0, -1, 1]], dtype=np.float64)


def clip_triangle(vertices):
    polygon = list(vertices)
    for plane in PLANES:
        result = []
        for start, end in zip(polygon, polygon[1:] + polygon[:1]):
            ds, de = start[:4] @ plane, end[:4] @ plane
            if (ds >= 0) != (de >= 0):
                result.append(start + (end - start) * (ds / (ds - de)))
            if de >= 0:
                result.append(end)
        polygon = result
        if len(polygon) < 3:
            return []
    return [np.array([polygon[0], polygon[i], polygon[i + 1]]) for i in range(1, len(polygon) - 1)]


def sample(texture, uv):
    height, width = texture.shape[:2]
    texel = uv * [width, height] - 0.5
    base = np.floor(texel).astype(np.int64)
    f = texel - base
    x, y = base[:, 0], base[:, 1]
    fx, fy = f[:, 0, None], f[:, 1, None]
    return ((texture[y % height, x % width] * (1 - fx) + texture[y % height, (x + 1) % width] * fx) * (1 - fy) +
            (texture[(y + 1) % height, x % width] * (1 - fx) + texture[(y + 1) % height, (x + 1) % width] * fx) * fy)


def reference(vertices, indices, texture, frame, width, height, depth_test=True):
    color = np.broadcast_to(frame["clear"], (height, width, 4)).copy()
    depth = np.ones((height, width), dtype=np.float64)
    owner = np.full((height, width), -1, dtype=np.int32)
    clip_count = 0
    vp = np.array(frame["view_projection"]).reshape(4, 4)
    positions = np.column_stack([vertices[:, :3], np.ones(len(vertices))])
    for instance_index, instance in enumerate(frame["instances"]):
        model = np.array(instance["model"]).reshape(4, 4)
        clip = positions @ model @ vp
        attributes = np.column_stack([clip, vertices[:, 3:5], vertices[:, 5:9] * instance["tint"]])
        for triangle_index, triangle in enumerate(indices):
            original = attributes[triangle]
            if np.any(original[:, :4] @ PLANES.T < 0):
                clip_count += 1
            for v in clip_triangle(original):
                ndc = v[:, :3] / v[:, 3, None]
                sx = (ndc[:, 0] + 1) * width * 0.5
                sy = (1 - ndc[:, 1]) * height * 0.5
                x0, x1, x2 = sx
                y0, y1, y2 = sy
                denominator = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
                if abs(denominator) < 1e-10:
                    continue
                left, right = max(0, int(np.floor(sx.min()))), min(width, int(np.ceil(sx.max())))
                top, bottom = max(0, int(np.floor(sy.min()))), min(height, int(np.ceil(sy.max())))
                if right <= left or bottom <= top:
                    continue
                y, x = np.mgrid[top:bottom, left:right].astype(np.float64) + 0.5
                a = ((y1 - y2) * (x - x2) + (x2 - x1) * (y - y2)) / denominator
                b = ((y2 - y0) * (x - x2) + (x0 - x2) * (y - y2)) / denominator
                c = 1 - a - b
                z = a * ndc[0, 2] + b * ndc[1, 2] + c * ndc[2, 2]
                visible = (a >= 0) & (b >= 0) & (c >= 0) & (z >= 0) & (z <= 1)
                if depth_test:
                    visible &= z <= depth[top:bottom, left:right]
                if not np.any(visible):
                    continue
                weights = np.column_stack([a[visible], b[visible], c[visible]]) / v[:, 3]
                weights /= weights.sum(axis=1, keepdims=True)
                uv = weights @ v[:, 4:6]
                tint = weights @ v[:, 6:10]
                color[top:bottom, left:right][visible] = sample(texture, uv) * tint
                depth[top:bottom, left:right][visible] = z[visible]
                owner[top:bottom, left:right][visible] = instance_index * len(indices) + triangle_index
    return np.rint(np.clip(color, 0, 1) * 255).astype(np.uint8), depth, owner, clip_count


def interior_mask(owner):
    # Exclude primitive boundaries, including occlusion transitions, without
    # deciding which pixels to ignore from the measured GPU color error.
    result = owner >= 0
    for axis in (0, 1):
        for distance in (-2, -1, 1, 2):
            result &= np.roll(owner, distance, axis) == owner
    result[:2] = result[-2:] = False
    result[:, :2] = result[:, -2:] = False
    return result


def verify(asset_dir, output_dir):
    vertices, indices, texture, manifest = load_asset(asset_dir)
    probe = json.loads((output_dir / "probe.json").read_text())
    width, height = probe["width"], probe["height"]
    require((width, height) == (1920, 1080), "Probe did not render at 1080p")
    require("RTX 3080 Laptop GPU" in probe["adapter"], "Unexpected adapter")
    require(probe["invalid_inputs_rejected"] == 3, "Missing input rejection checks")
    expected_bytes = len(vertices) * 36 + indices.size * 4 + texture.size
    require(probe["statistics"] == {"meshes": 1, "textures": 1, "materials": 1,
            "asset_upload_bytes": expected_bytes, "rendered_frames": 4, "mesh_draws": 5}, "Unexpected asset/draw counts")
    require([f["name"] for f in probe["frames"]] == ["asset", "camera_and_model", "depth_overlap", "near_clip"], "Incomplete probe")
    reports, images = [], []
    for frame in probe["frames"]:
        name = frame["name"]
        require(frame["asset_upload_bytes_after_frame"] == expected_bytes, "Per-frame asset upload")
        gpu = np.fromfile(output_dir / (name + ".rgba"), dtype=np.uint8).reshape(height, width, 4)
        gpu_depth = np.fromfile(output_dir / (name + ".depth32f"), dtype="<f4").reshape(height, width)
        require(np.isfinite(gpu_depth).all(), "Invalid GPU depth")
        cpu, depth, owner, clipped = reference(vertices, indices, texture, frame, width, height)
        covered, gpu_covered = owner >= 0, gpu_depth < 1
        overlap, union = covered & gpu_covered, covered | gpu_covered
        interior = interior_mask(owner) & gpu_covered
        pixels = int(covered.sum())
        require(pixels > 10000 and int(interior.sum()) > pixels * 0.8, "Insufficient mesh coverage")
        error = np.abs(cpu.astype(np.int16) - gpu.astype(np.int16))[interior, :3]
        depth_error = np.abs(depth - gpu_depth)[interior]
        iou = float(overlap.sum() / union.sum())
        record = {"name": name, "covered_pixels": pixels, "interior_pixels": int(interior.sum()),
                  "coverage_iou": iou, "rgb_rmse_255": float(np.sqrt(np.mean(error.astype(float) ** 2))),
                  "rgb_p99_255": float(np.quantile(error, 0.99)), "rgb_max_255": int(error.max()),
                  "depth_max_error": float(depth_error.max()), "triangles_requiring_clip": clipped}
        record["pass"] = (iou > 0.999 and record["rgb_rmse_255"] < 1 and record["rgb_p99_255"] <= 2
                          and record["rgb_max_255"] <= 10 and record["depth_max_error"] < 5e-5)
        if name == "depth_overlap":
            wrong, _, _, _ = reference(vertices, indices, texture, frame, width, height, depth_test=False)
            differs = np.max(np.abs(wrong.astype(np.int16) - cpu.astype(np.int16)), axis=2) > 10
            record["pixels_detecting_disabled_depth"] = int((differs & interior).sum())
            record["pass"] &= record["pixels_detecting_disabled_depth"] > 1000
        if name == "near_clip":
            record["pass"] &= clipped > 0
        Image.fromarray(gpu).save(output_dir / (name + ".png"))
        Image.fromarray(cpu).save(output_dir / (name + "-reference.png"))
        images.append(Image.fromarray(gpu))
        reports.append(record)
        print(json.dumps(record), flush=True)
    require(not np.array_equal(np.asarray(images[0]), np.asarray(images[1])), "Transform/camera update reused old image")
    preview = Image.new("RGB", (1920, 1080))
    for index, picture in enumerate(images):
        preview.paste(picture.resize((960, 540)), ((index % 2) * 960, (index // 2) * 540))
    preview.save(output_dir / "contact-sheet.png")
    report = {"pass": all(r["pass"] for r in reports), "scope": "Offscreen native mesh correctness, not gameplay performance",
              "asset_sha256": manifest["asset_sha256"], "probe_sha256": digest((output_dir / "probe.json").read_bytes()),
              "frames": reports}
    (output_dir / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    require(report["pass"], "Native mesh validation failed; see verification.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asset", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    verify(args.asset.resolve(), args.output.resolve())
