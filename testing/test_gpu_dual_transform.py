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
    inputs, batches, expected = bytearray(), bytearray(), bytearray()
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
        constants = state[16:80] + data[context * 16:(context + 4) * 16] + struct.pack("<4I", descriptor[2], 0, 0, 0)
        if not all(math.isfinite(x) for x in struct.unpack("<36f", constants)):
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
                expected.extend(struct.pack("<8I", stq[0], stq[1], rgba, stq[2],
                    (xyz[0] & 65535) | ((xyz[1] & 65535) << 16), xyz[2], 0, 0))
        selected.append({"fixture": pre.name, "pre_sha256": digest(pre.read_bytes()),
                         "packet_sha256": digest(packet), "vertices": count})
    if not selected:
        raise ValueError("No supported real dual-transform fixtures found")
    payloads = {"inputs.bin": inputs, "batches.bin": batches, "expected.bin": expected}
    meta = {"version": 1, "batches": len(selected), "vertices": len(inputs) // 32,
            "sha256": {name: digest(value) for name, value in payloads.items()},
            "skipped": dict(skipped), "selected": selected}
    destination.mkdir(parents=True)
    for name, value in payloads.items():
        (destination / name).write_bytes(value)
    (destination / "manifest.json").write_text(json.dumps(meta, indent=2))
    print(f"Prepared {len(selected)} captured chunks / {meta['vertices']} source vertices; private captures only.")


def run(directory, dll, report_path):
    directory, dll, report_path = private(directory), private(dll), private(report_path)
    if report_path.exists():
        raise FileExistsError("Use a fresh private report path")
    meta = json.loads((directory / "manifest.json").read_text())
    inputs, batches, expected = ((directory / name).read_bytes() for name in ("inputs.bin", "batches.bin", "expected.bin"))
    for name, value in (("inputs.bin", inputs), ("batches.bin", batches), ("expected.bin", expected)):
        if digest(value) != meta["sha256"][name]:
            raise ValueError("Prepared private fixture hash mismatch")
    count, batch_count = meta["vertices"], meta["batches"]
    if (meta["version"] != 1 or not 0 < count <= 1024 * 1024 or not 0 < batch_count <= 65536 or
            len(inputs) != count * 32 or len(batches) != batch_count * 144 or len(expected) != count * 64):
        raise ValueError("Invalid prepared geometry layout")
    os.environ["PS2X_HWGS_API"] = "d3d12"
    os.environ["PS2X_HWGS_DIRECT"] = "0"
    os.chdir(dll.parent)
    with os.add_dll_directory(str(dll.parent)):
        gs = c.CDLL(str(dll))
        gs.tf_gs_open.argtypes = [c.c_int, c.c_int, c.c_char_p, c.c_void_p, c.c_uint32]
        gs.tf_gs_open.restype = c.c_int
        gs.tf_gs_close.argtypes = []
        gs.tf_gs_close.restype = None
        probe = gs.tf_gs_gpu_dual_transform_probe
        probe.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32]
        probe.restype = c.c_int
        error, output = c.create_string_buffer(4096), c.create_string_buffer(len(expected))
        source, constants = c.create_string_buffer(inputs), c.create_string_buffer(batches)
        if not gs.tf_gs_open(1, 0, str(dll.parent / "resources").encode(), error, len(error)):
            raise RuntimeError(error.value.decode(errors="replace"))
        try:
            if not probe(source, count, constants, batch_count, output, len(output), error, len(error)):
                raise RuntimeError(error.value.decode(errors="replace"))
        finally:
            gs.tf_gs_close()
    actual = output.raw
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
    report = {"exact": actual == expected, "batches": batch_count, "source_vertices": count,
              "output_vertices": count * 2, "mismatched_words": dict(mismatches), "max_delta": dict(max_delta),
              "dll_sha256": digest(dll.read_bytes()), "fixture_sha256": meta["sha256"],
              "scope": "Offline GPU arithmetic only; no live VU bypass or performance claim"}
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return 0 if report["exact"] else 1


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
    args = parser.parse_args()
    if args.command == "prepare":
        prepare(args.fixtures, args.program_manifest, args.output)
    else:
        raise SystemExit(run(args.prepared, args.dll, args.report))
