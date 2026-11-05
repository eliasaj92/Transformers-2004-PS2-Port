"""Prepare private enhanced-build fixtures, then compare GPU dual transforms.

Run inspect_native_seam_capture.py first to verify capture/trace integrity.
This is an offline arithmetic check, not live VU replacement or an FPS test.
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
import subprocess

from inspect_native_seam_capture import gif_packets, read_vu


ROOT = Path(__file__).resolve().parents[1]


def private(path):
    path = path.resolve()
    if path == ROOT or ROOT in path.parents:
        raise ValueError("Keep captures, shader caches, builds and reports outside the checkout")
    return path


def digest(data):
    return hashlib.sha256(data).hexdigest()


def prepare(fixtures, manifest, destination):
    """Import raw mesh attributes; all geometry arithmetic runs in the shader."""
    destination = private(destination)
    if destination.exists():
        raise FileExistsError("Use a fresh private fixture directory")
    programs = {}
    wanted = ("vlShaderAssembly_ShaderManager", "vlShaderAssembly_DualTransform",
              "vlShaderAssembly_TransformClip_PCT")
    for item in json.loads(manifest.read_text())["programs"]:
        if item["name"] in wanted:
            data = (manifest.parent / item["file"]).read_bytes()
            if digest(data) != item["sha256"]:
                raise ValueError("Program does not match extraction manifest")
            programs[item["name"]] = data
    manager, dual, clip = (programs[name] for name in wanted)
    inputs, batches, expected, packed = bytearray(), bytearray(), bytearray(), bytearray()
    selected, skipped = [], Counter()
    for line in fixtures.read_text().splitlines():
        pre = private(Path(line))
        if not pre.name.endswith("-pre.bin"):
            raise ValueError("Expected a private accelerated -pre.bin capture")
        head, code, data, state = read_vu(pre)
        if (head[0] not in (0x80000010, 0x800000f8) or code[:len(manager)] != manager or
                struct.unpack_from("<I", data, 64)[0] != 2):
            skipped["other_chain"] += 1
            continue
        first_pc, context = struct.unpack_from("<2I", data, 80)
        second_pc = struct.unpack_from("<I", data, 96)[0]
        if code[first_pc * 8:first_pc * 8 + len(dual)] != dual or code[second_pc * 8:second_pc * 8 + len(clip)] != clip:
            skipped["other_program"] += 1
            continue
        if head[2] >= 1024 or context > 1020:
            raise ValueError("Invalid captured VU data address")
        descriptor = struct.unpack_from("<4I", data, head[2] * 16)
        if descriptor[1] != 3 or any(struct.unpack_from("<4I", data, 21 * 16)):
            skipped["clipped_or_other_layout"] += 1
            continue
        count = descriptor[0] & 0x7fff
        if not 3 <= count <= 28 or head[2] + 1 + count * 3 > 1024:
            raise ValueError("Unsupported source vertex range")
        packets = gif_packets(Path(str(pre)[:-7] + "gif.bin"))
        if len(packets) != 1 or packets[0][0] != 1 or len(packets[0][1]) != (8 + count * 6) * 16:
            raise ValueError("Unexpected dual-transform packet framing")
        packet = packets[0][1]
        for field in range(2):
            base = field * (4 + count * 3) * 16
            low, high = struct.unpack_from("<2Q", packet, base)
            if (low & 32767, low >> 58 & 3, low >> 60, high) != (2, 0, 1, 14):
                raise ValueError("Unexpected dual-transform state tag")
            low, high = struct.unpack_from("<2Q", packet, base + 48)
            if (low & 32767, low >> 58 & 3, low >> 60, high, low >> 47 & 7) != (count, 0, 3, 0x412, 4):
                raise ValueError("Unexpected dual-transform triangle strip")
        batch_index = len(batches) // 144
        masks = struct.unpack_from("<2I", data, (head[2] + count * 3 + 4) * 16)
        constants = state[16:80] + data[context * 16:(context + 4) * 16] + struct.pack("<4I", descriptor[2], *masks, len(inputs) // 32)
        if not all(math.isfinite(x) for x in struct.unpack("<33f", constants[:132])):
            raise ValueError("Non-finite transform constants are not qualified")
        batches.extend(constants)
        for vertex in range(count):
            base = (head[2] + 1 + vertex * 3) * 16
            tex, position = data[base:base + 12], data[base + 32:base + 44]
            if not all(math.isfinite(x) for x in struct.unpack("<6f", tex + position)):
                raise ValueError("Non-finite input attributes are not qualified")
            color = struct.unpack_from("<4I", data, base + 16)
            if any(x > 255 for x in color):
                raise ValueError("Unexpected source color format")
            rgba = sum(x << (8 * i) for i, x in enumerate(color))
            inputs.extend(position + struct.pack("<I", batch_index) + tex + struct.pack("<I", rgba))
            for field in range(2):
                output = (field * (4 + count * 3) + 4 + vertex * 3) * 16
                stq = struct.unpack_from("<3I", packet, output)
                reference_color = struct.unpack_from("<4I", packet, output + 16)
                xyz = struct.unpack_from("<4I", packet, output + 32)
                if reference_color != color:
                    raise ValueError("Unexpected field-dependent vertex color")
                packed.extend(packet[output:output + 48])
                expected.extend(struct.pack("<8I", stq[0], stq[1], rgba, stq[2] or 0x00800000,
                    (xyz[0] & 65535) | ((xyz[1] & 65535) << 16), (xyz[2] >> 4) & 0xffffff, 0, (xyz[3] >> 4) & 255))
        selected.append({"fixture": pre.name, "pre_sha256": digest(pre.read_bytes()),
                         "packet_sha256": digest(packet), "vertices": count})
    if not selected:
        raise ValueError("No supported real dual-transform fixtures found")
    payloads = {"inputs.bin": inputs, "batches.bin": batches, "expected.bin": expected, "packed.bin": packed}
    meta = {"version": 2, "batches": len(selected), "vertices": len(inputs) // 32,
            "sha256": {name: digest(value) for name, value in payloads.items()},
            "skipped": dict(skipped), "selected": selected}
    destination.mkdir(parents=True)
    for name, value in payloads.items():
        (destination / name).write_bytes(value)
    (destination / "manifest.json").write_text(json.dumps(meta, indent=2))
    print(f"Prepared {len(selected)} captured chunks / {meta['vertices']} source vertices; private captures only.")


def run(directory, dll, report_path, capture_path=None):
    directory, dll, report_path = private(directory), private(dll), private(report_path)
    if report_path.exists():
        raise FileExistsError("Use a fresh private report path")
    if capture_path is not None:
        capture_path = private(capture_path)
        if capture_path.exists():
            raise FileExistsError("Use a fresh private GPU capture path")
    meta = json.loads((directory / "manifest.json").read_text())
    inputs, batches, expected, packed = ((directory / name).read_bytes() for name in ("inputs.bin", "batches.bin", "expected.bin", "packed.bin"))
    for name, value in (("inputs.bin", inputs), ("batches.bin", batches), ("expected.bin", expected), ("packed.bin", packed)):
        if digest(value) != meta["sha256"][name]:
            raise ValueError("Prepared private fixture hash mismatch")
    count, batch_count = meta["vertices"], meta["batches"]
    if (meta["version"] != 2 or not 0 < count <= 1024 * 1024 or not 0 < batch_count <= 65536 or
            len(inputs) != count * 32 or len(batches) != batch_count * 144 or len(expected) != count * 64 or len(packed) != count * 96):
        raise ValueError("Invalid prepared geometry layout")
    os.environ["PS2X_HWGS_API"] = "d3d12"
    os.environ["PS2X_HWGS_DIRECT"] = "0"
    os.chdir(dll.parent)
    with os.add_dll_directory(str(dll.parent)):
        gs = c.CDLL(str(dll))
        decode = gs.tf_gs_decode_dual_vertices_probe
        decode.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32]
        decode.restype = c.c_int
        decoded, packet_input = c.create_string_buffer(len(expected)), c.create_string_buffer(packed)
        if not decode(packet_input, count * 2, decoded, len(decoded)) or decoded.raw != expected:
            raise ValueError("Prepared vertex packing differs from native GS register definitions")
        gs.tf_gs_open.argtypes = [c.c_int, c.c_int, c.c_char_p, c.c_void_p, c.c_uint32]
        gs.tf_gs_open.restype = c.c_int
        gs.tf_gs_close.argtypes = []
        gs.tf_gs_close.restype = None
        probe = gs.tf_gs_gpu_dual_transform_probe
        probe.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32]
        probe.restype = c.c_int
        error, output = c.create_string_buffer(4096), c.create_string_buffer(count * 128)
        source, constants = c.create_string_buffer(inputs), c.create_string_buffer(batches)
        if not gs.tf_gs_open(1, 0, str(dll.parent / "resources").encode(), error, len(error)):
            raise RuntimeError(error.value.decode(errors="replace"))
        try:
            if not probe(source, count, constants, batch_count, output, len(output), error, len(error)):
                raise RuntimeError(error.value.decode(errors="replace"))
        finally:
            gs.tf_gs_close()
    actual, raw = output.raw[:len(expected)], output.raw[len(expected):]
    raw_expected = b"".join(packed[i:i + 12] + bytes(4) + packed[i + 32:i + 48]
                            for i in range(0, len(packed), 48))
    mismatches, max_delta = Counter(), Counter()
    for i in range(0, len(expected), 4):
        a, b = struct.unpack_from("<I", actual, i)[0], struct.unpack_from("<I", expected, i)[0]
        if a != b:
            word = i // 4 % 8
            mismatches[str(word)] += 1
            if word == 4:
                max_delta["x_fixed4"] = max(max_delta["x_fixed4"], abs((a & 65535) - (b & 65535)))
                max_delta["y_fixed4"] = max(max_delta["y_fixed4"], abs((a >> 16) - (b >> 16)))
            else:
                max_delta[str(word)] = max(max_delta[str(word)], abs(a - b))
    report = {"exact": actual == expected and raw == raw_expected, "batches": batch_count, "source_vertices": count,
              "raw_geometry_exact": raw == raw_expected,
              "native_register_decode_exact": decoded.raw == expected,
              "output_vertices": count * 2, "mismatched_words": dict(mismatches), "max_delta": dict(max_delta),
              "dll_sha256": digest(dll.read_bytes()), "fixture_sha256": meta["sha256"],
              "scope": "Offline GPU arithmetic only; no live VU bypass or performance claim"}
    if capture_path is not None and report["exact"]:
        # Private diagnostic only: bind this result to its actual uploaded input.
        header = struct.pack("<4I32s", 0x44554750, 1, count * 2, 32,
                             hashlib.sha256(inputs + batches).digest())
        capture = header + raw
        with capture_path.open("xb") as stream:
            stream.write(capture)
        report["gpu_capture_sha256"] = digest(capture)
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return 0 if report["exact"] else 1


def replay(directory, capture_path, gpu_report_path, fixtures, runner, report_path):
    """Skip selected VU calls, then replay their successors with carried state."""
    directory, capture_path, gpu_report_path, fixtures, runner, report_path = (
        private(path) for path in (directory, capture_path, gpu_report_path, fixtures, runner, report_path))
    selection_path = report_path.with_suffix(".selected.txt")
    proof_path = report_path.with_suffix(".json")
    if any(path.exists() for path in (report_path, selection_path, proof_path)):
        raise FileExistsError("Use a fresh private replay report path")
    meta = json.loads((directory / "manifest.json").read_text())
    gpu_report = json.loads(gpu_report_path.read_text())
    if (meta["version"] != 2 or not gpu_report.get("exact") or not gpu_report.get("raw_geometry_exact") or
            not gpu_report.get("native_register_decode_exact") or gpu_report["fixture_sha256"] != meta["sha256"]):
        raise ValueError("GPU report does not qualify these prepared fixtures")
    for name in ("inputs.bin", "batches.bin", "expected.bin", "packed.bin"):
        if digest((directory / name).read_bytes()) != meta["sha256"][name]:
            raise ValueError("Prepared private fixture hash mismatch")
    inputs, batches, capture = ((directory / "inputs.bin").read_bytes(),
                                (directory / "batches.bin").read_bytes(), capture_path.read_bytes())
    header = struct.pack("<4I32s", 0x44554750, 1, meta["vertices"] * 2, 32,
                         hashlib.sha256(inputs + batches).digest())
    if (capture[:48] != header or len(capture) != 48 + meta["vertices"] * 64 or
            digest(capture) != gpu_report.get("gpu_capture_sha256")):
        raise ValueError("GPU capture is not bound to these uploaded inputs")
    available = {}
    for line in fixtures.read_text().splitlines():
        if not line.strip():
            continue
        path = private(Path(line))
        if path.name in available:
            raise ValueError("Replay fixture names must be unique")
        available[path.name] = path
    selected = []
    for item in meta["selected"]:
        path = available[item["fixture"]]
        if digest(path.read_bytes()) != item["pre_sha256"]:
            raise ValueError("Replay source changed since the GPU upload was prepared")
        packets = gif_packets(Path(str(path)[:-7] + "gif.bin"))
        if len(packets) != 1 or digest(packets[0][1]) != item["packet_sha256"]:
            raise ValueError("Replay reference packet changed")
        selected.append(str(path))
    with selection_path.open("x") as stream:
        stream.write("\n".join(selected) + "\n")
    result = subprocess.run([str(runner), str(fixtures), str(report_path), "--gpu-dual",
                             str(selection_path), str(directory / "inputs.bin"),
                             str(directory / "batches.bin"), str(capture_path)], check=False)
    proof = {"exit_code": result.returncode, "selected_calls": len(selected), "fixtures": len(available),
             "runner_sha256": digest(runner.read_bytes()), "gpu_capture_sha256": digest(capture),
             "prepared_manifest_sha256": digest((directory / "manifest.json").read_bytes()),
             "scope": "Offline GPU bypass with carried state; captured window only, not live offload or FPS"}
    proof_path.write_text(json.dumps(proof, indent=2))
    return result.returncode


def draw(directory, dll, report_path, frontend=False, merge=False):
    """Render captured meshes from GPU buffers; compare final diagnostic images."""
    directory, dll, report_path = private(directory), private(dll), private(report_path)
    if report_path.exists():
        raise FileExistsError("Use a fresh private draw report path")
    meta = json.loads((directory / "manifest.json").read_text())
    frontend = frontend or merge
    inputs, batches, expected = ((directory / name).read_bytes() for name in ("inputs.bin", "batches.bin", "expected.bin"))
    packed = (directory / "packed.bin").read_bytes() if merge else None
    if merge and (digest(packed) != meta["sha256"]["packed.bin"] or len(packed) != meta["vertices"] * 96):
        raise ValueError("Prepared packed geometry changed")
    for name, value in (("inputs.bin", inputs), ("batches.bin", batches), ("expected.bin", expected)):
        if digest(value) != meta["sha256"][name]:
            raise ValueError("Prepared private fixture hash mismatch")
    count, batch_count = meta["vertices"], meta["batches"]
    if (meta["version"] != 2 or not 3 <= count <= 32768 or not 0 < batch_count <= 1024 or
            len(inputs) != count * 32 or len(batches) != batch_count * 144 or len(expected) != count * 64):
        raise ValueError("Invalid prepared draw layout")
    groups = meta.get("groups", [{"first_batch": i, "end_batch": i + 1} for i in range(batch_count)])
    end = 0
    for group in groups:
        if group["first_batch"] != end or not end < group["end_batch"] <= batch_count:
            raise ValueError("Invalid or noncontiguous prepared mesh groups")
        end = group["end_batch"]
    if end != batch_count:
        raise ValueError("Prepared mesh groups do not cover every chunk")
    group_ends = (c.c_uint32 * len(groups))(*(group["end_batch"] for group in groups))
    os.environ["PS2X_HWGS_API"] = "d3d12"
    os.environ["PS2X_HWGS_DIRECT"] = "0"
    os.chdir(dll.parent)
    with os.add_dll_directory(str(dll.parent)):
        gs = c.CDLL(str(dll))
        gs.tf_gs_open.argtypes = [c.c_int, c.c_int, c.c_char_p, c.c_void_p, c.c_uint32]
        gs.tf_gs_open.restype = c.c_int
        gs.tf_gs_close.argtypes = []
        gs.tf_gs_close.restype = None
        probe = gs.tf_gs_gpu_dual_merged_probe if merge else gs.tf_gs_gpu_frontend_probe_v1 if frontend else gs.tf_gs_gpu_dual_draw_probe_v3
        probe.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32, c.POINTER(c.c_uint32), c.c_uint32, c.c_void_p,
                          c.POINTER(c.c_uint64), c.c_void_p, c.c_uint32]
        probe.restype = c.c_int
        if merge:
            probe.argtypes += [c.c_void_p]
        error, stats = c.create_string_buffer(4096), (c.c_uint64 * (28 if merge else 17))()
        source, constants, reference = (c.create_string_buffer(value) for value in (inputs, batches, expected))
        if not gs.tf_gs_open(4 if frontend else 1, 0, str(dll.parent / "resources").encode(), error, len(error)):
            raise RuntimeError(error.value.decode(errors="replace"))
        try:
            extra = [c.create_string_buffer(packed)] if merge else []
            passed = bool(probe(source, count, constants, batch_count, group_ends, len(groups),
                                reference, stats, error, len(error), *extra))
        finally:
            gs.tf_gs_close()
    metrics = ({"native_submissions": stats[0], "submit_callbacks": stats[1], "cpu_materializations": stats[2],
                "textured_callbacks": stats[3], "depth_callbacks": stats[4], "blend_callbacks": stats[5],
                "lod_callbacks": stats[6], "painted_pixels": stats[7], "differing_pixels": stats[8],
                "completed_modes": stats[9], "full_resolution_differing_pixels": stats[10],
                "full_resolution_compared_pixels": stats[11], "gpu_draws": stats[12],
                "mipmap_filtered_callbacks": stats[13], "internal_scale": 4,
                "grouped_dispatches": stats[14], "grouped_meshes": stats[15]} if frontend else
               {"allocations": stats[0], "reuses": stats[1], "dispatches": stats[2], "native_draws": stats[3],
              "painted_pixels": stats[4], "differing_pixels": stats[5],
              "handler_replacements": stats[6], "ordinary_draws": stats[7],
              "mesh_uploads": stats[8], "resident_mesh_bytes": stats[9],
              "source_upload_bytes": stats[10], "constant_upload_bytes": stats[11],
              "bounded_groups": stats[12], "declined_bounds": stats[13], "bounded_vertices": stats[14],
              "wrapped_bounds": stats[15], "max_bounds_area_ratio": stats[16] / 1000000})
    if merge:
        metrics = {key: metrics[key] for key in ("painted_pixels", "differing_pixels", "completed_modes",
                   "full_resolution_differing_pixels", "full_resolution_compared_pixels", "gpu_draws", "internal_scale")}
        metrics.update(zip(("latch_checks", "rejection_checks", "address_range_checks", "merged_groups", "merged_packets",
                            "merged_gpu_draws", "cpu_materializations", "ordinary_fields", "state_declined", "feedback_declined",
                            "bounds_declined", "output_declined"), stats[16:28]))
    report = {"passed": passed, "merge": merge, "resident_mesh_batches": os.environ.get("PS2X_TF_GPU_DUAL_BATCHES") == "1",
              "batches": batch_count, "groups": len(groups), "source_vertices": count, **metrics,
              "dll_sha256": digest(dll.read_bytes()), "fixture_sha256": meta["sha256"],
              "prepared_manifest_sha256": digest((directory / "manifest.json").read_bytes()),
              "scope": ("Real GS frontend with synthetic texture/depth/blend state and enhanced captured meshes; "
                        "not live game integration or FPS" if frontend else
                        "GPU-resident captured-mesh draws with diagnostic GS state; not live game integration or FPS")}
    if error.value:
        report["error"] = error.value.decode(errors="replace")
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return 0 if passed else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prep = commands.add_parser("prepare")
    prep.add_argument("--fixtures", type=Path, required=True)
    prep.add_argument("--program-manifest", type=Path, required=True)
    prep.add_argument("--output", type=Path, required=True)
    compare = commands.add_parser("run")
    compare.add_argument("--prepared", type=Path, required=True)
    compare.add_argument("--dll", type=Path, required=True)
    compare.add_argument("--report", type=Path, required=True)
    compare.add_argument("--gpu-capture", type=Path)
    bypass = commands.add_parser("replay")
    bypass.add_argument("--prepared", type=Path, required=True)
    bypass.add_argument("--gpu-capture", type=Path, required=True)
    bypass.add_argument("--gpu-report", type=Path, required=True)
    bypass.add_argument("--fixtures", type=Path, required=True)
    bypass.add_argument("--runner", type=Path, required=True)
    bypass.add_argument("--report", type=Path, required=True)
    resident = commands.add_parser("draw")
    resident.add_argument("--prepared", type=Path, required=True)
    resident.add_argument("--dll", type=Path, required=True)
    resident.add_argument("--report", type=Path, required=True)
    resident.add_argument("--frontend", action="store_true", help="Exercise GS texture/depth/blend preparation")
    resident.add_argument("--merge", action="store_true", help="Exercise ordered merged mesh packets and rejection guards")
    resident.add_argument("--mesh-batches", action="store_true", help="Qualify persistent non-grass geometry batches up to 64 packets")
    args = parser.parse_args()
    if args.command == "prepare":
        prepare(args.fixtures, args.program_manifest, args.output)
    elif args.command == "run":
        raise SystemExit(run(args.prepared, args.dll, args.report, args.gpu_capture))
    elif args.command == "replay":
        raise SystemExit(replay(args.prepared, args.gpu_capture, args.gpu_report,
                               args.fixtures, args.runner, args.report))
    else:
        if args.mesh_batches:
            if not args.frontend or not args.merge:
                parser.error("--mesh-batches requires --frontend --merge")
            os.environ["PS2X_TF_GPU_DUAL_BATCHES"] = "1"
        raise SystemExit(draw(args.prepared, args.dll, args.report, args.frontend, args.merge))
