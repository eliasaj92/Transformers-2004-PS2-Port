#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify GPU reflection lighting through all four ordered projection sections.

Reads private accelerated captures. Expected geometry is never a compute input.
The native and raw readbacks are diagnostic only, not a live rendering path.
"""
import argparse
from collections import Counter
import ctypes as c
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess

from inspect_native_seam_capture import gif_packets, read_vu
from test_gpu_dual_transform import digest, private
from test_gpu_reflection import prepare as prepare_lighting, replay_environment


RECORD_BYTES = 34128


def record(data, index, magic):
    part = data[index * RECORD_BYTES:(index + 1) * RECORD_BYTES]
    if len(part) != RECORD_BYTES or struct.unpack_from('<4I', part) != (magic, 1, index, 16384):
        raise ValueError('Invalid paired projection capture header/order')
    first, last = struct.unpack_from('<2Q', part, 16)
    if last <= first:
        raise ValueError('Invalid paired projection cycle interval')
    return part[32:696], part[696:17080], part[17080:17744], part[17744:]


def prepare(prepared, fixtures, ends=None):
    manifest = json.loads((prepared / 'manifest.json').read_text())
    if manifest['version'] != 1 or digest(fixtures.read_bytes()) != manifest['fixtures_list_sha256']:
        raise ValueError('Projection fixture list/manifest changed')
    if set(manifest['sha256']) != {'raw.bin', 'inputs.bin', 'expected.bin', 'replay.csv', 'replay.log', 'projection.bin'}:
        raise ValueError('Use a complete paired reflection capture prepared with --projection')
    for name, expected_hash in manifest['sha256'].items():
        if digest((prepared / name).read_bytes()) != expected_hash:
            raise ValueError('Projection fixture changed: ' + name)
    raw, project = ((prepared / name).read_bytes() for name in ('raw.bin', 'projection.bin'))
    lighting, _, batches = prepare_lighting(raw)
    if len(project) != len(raw) or batches != manifest['captures']:
        raise ValueError('Unpaired reflection projection capture')
    inputs, native, packed = bytearray(), bytearray(), bytearray()
    index = vertex_index = 0
    for line in fixtures.read_text().splitlines():
        path = private(Path(line))
        head, code, memory, _ = read_vu(path)
        first = struct.unpack_from('<I', memory, 80)[0] * 8
        if (head[0] not in (0x80000010, 0x800000f8) or digest(code[first:first + 1248]) !=
                '283b4e4c8543eda7eb411a9082e6a943f23b870c8935b6dca423852399671f41'):
            continue
        second = struct.unpack_from('<I', memory, 96)[0] * 8
        if digest(code[second:second + 1728]) != '33de5693fb3ae7b14c87c063944c2cfdc61c4d267ff55dee2fc704813eb8f067':
            raise ValueError('Unexpected reflection projection microprogram')
        ls, _, _, lm = record(raw, index, 0x5245464c)
        ps, pm, _, output = record(project, index, 0x5250524a)
        lvi, pvi = (struct.unpack_from('<16i', state, 512) for state in (ls, ps))
        base, count, context = pvi[1], pvi[10], pvi[11]
        section_bytes = (4 + 3 * count) * 16
        if (not 1 <= count <= 28 or base < 4 or (base - 4) * 16 + 4 * section_bytes > 16384 or
                context < 0 or context + 17 > 1024 or base != lvi[1] or count != lvi[10] or
                struct.unpack_from('<I', pm, 21 * 16)[0] != 0 or
                pm[base * 16:(base + 3 * count) * 16] != lm[base * 16:(base + 3 * count) * 16]):
            raise ValueError('Unsupported projection range/clipping or changed reflection output')
        packets = gif_packets(Path(str(path)[:-7] + 'gif.bin'))
        packet = output[(base - 4) * 16:(base - 4) * 16 + 4 * section_bytes]
        if packets != [(1, packet)]:
            raise ValueError('Four-section projection differs from ordered captured GIF')
        matrices = ps[16:80] + pm[context * 16:(context + 4) * 16]
        alpha = pm[(context + 16) * 16 + 12:(context + 17) * 16]
        if not all(math.isfinite(x[0]) for x in struct.iter_unpack('<f', matrices + alpha)):
            raise ValueError('Non-finite projection constants')
        masks = struct.unpack_from('<2I', pm, (base + 3 * count + 3) * 16)
        for section in range(4):
            offset = section * section_bytes
            low, high = struct.unpack_from('<2Q', packet, offset)
            if (low & 32767, low >> 58 & 3, low >> 60, high) != (2, 0, 1, 14):
                raise ValueError('Unexpected projection state tag')
            low, high = struct.unpack_from('<2Q', packet, offset + 48)
            if ((low & 32767, low >> 58 & 3, low >> 60, high, low >> 47 & 7) != (count, 0, 3, 0x412, 4) or
                    bool(low & 32768) != (section == 3)):
                raise ValueError('Unexpected projection strip or end marker')
        for vertex in range(count):
            flags = (masks[vertex // 16] << (vertex % 16)) & 65535
            source = lighting[vertex_index * 224:(vertex_index + 1) * 224]
            inputs.extend(source + matrices + alpha + struct.pack('<3I', flags, 0, 0))
            for section in range(4):
                offset = section * section_bytes + 64 + vertex * 48
                stq = struct.unpack_from('<3I', packet, offset)
                rgba = struct.unpack_from('<4I', packet, offset + 16)
                xyz = struct.unpack_from('<4I', packet, offset + 32)
                if xyz[3] != (flags | 0xffff0000 if flags & 32768 else flags):
                    raise ValueError('Unexpected projection strip restart flags')
                packed.extend(struct.pack('<12I', *stq, 0, *rgba, *xyz))
                color = sum((x & 255) << (8 * i) for i, x in enumerate(rgba))
                native.extend(struct.pack('<8I', stq[0], stq[1], color, stq[2] or 0x00800000,
                    (xyz[0] & 65535) | ((xyz[1] & 65535) << 16), (xyz[2] >> 4) & 0xffffff, 0, (xyz[3] >> 4) & 255))
            vertex_index += 1
        index += 1
        if ends is not None:
            ends.append(vertex_index)
    if index != batches or vertex_index != manifest['vertices'] or len(inputs) != vertex_index * 368:
        raise ValueError('Incomplete paired projection fixtures')
    return bytes(inputs), bytes(native + packed), index, vertex_index


def run(prepared, fixtures, dll, report_path, adapter, capture_path):
    prepared, fixtures, dll, report_path = (private(p) for p in (prepared, fixtures, dll, report_path))
    if report_path.exists():
        raise FileExistsError('Use a fresh private projection report')
    if capture_path:
        capture_path = private(capture_path)
        if capture_path.exists() or capture_path == report_path:
            raise FileExistsError('Use distinct fresh private projection capture/report paths')
    inputs, expected, batches, count = prepare(prepared, fixtures)
    os.environ.update(PS2X_HWGS_API='d3d12', PS2X_HWGS_DIRECT='0')
    if adapter:
        os.environ['PS2X_HWGS_ADAPTER'] = adapter
    os.chdir(dll.parent)
    with os.add_dll_directory(str(dll.parent)):
        gs = c.CDLL(str(dll))
        gs.tf_gs_open.argtypes = [c.c_int, c.c_int, c.c_char_p, c.c_void_p, c.c_uint32]
        gs.tf_gs_open.restype = c.c_int
        gs.tf_gs_close.argtypes, gs.tf_gs_close.restype = [], None
        probe = gs.tf_gs_gpu_reflection_projection_probe
        probe.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32]
        probe.restype = c.c_int
        error, source = c.create_string_buffer(4096), c.create_string_buffer(inputs)
        output = c.create_string_buffer(len(expected))
        # The same native register decoder is used by the renderer. This also
        # checks raw Q/color/depth packing independently of Python's conversion.
        decode = gs.tf_gs_decode_dual_vertices_probe
        decode.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32]
        decode.restype = c.c_int
        native, packed = c.create_string_buffer(count * 128), c.create_string_buffer(expected[count * 128:])
        if not decode(packed, count * 4, native, len(native)) or native.raw != expected[:count * 128]:
            raise ValueError('Projection native layout differs from GS register definitions')
        if not gs.tf_gs_open(1, 0, str(dll.parent / 'resources').encode(), error, len(error)):
            raise RuntimeError(error.value.decode(errors='replace'))
        try:
            if not probe(source, count, output, len(output), error, len(error)):
                raise RuntimeError(error.value.decode(errors='replace'))
        finally:
            gs.tf_gs_close()
    actual, bad, first = output.raw, Counter(), []
    for index, (x, y) in enumerate(zip(struct.iter_unpack('<I', actual), struct.iter_unpack('<I', expected))):
        if x != y:
            raw = index >= count * 32
            offset, stride = (index - count * 32, 48) if raw else (index, 32)
            bad[('packed' if raw else 'native') + ':' + str(offset % stride)] += 1
            if len(first) < 12:
                first.append(dict(region='packed' if raw else 'native', vertex=offset // stride,
                                  word=offset % stride, actual=x[0], expected=y[0]))
    report = dict(exact=actual == expected, batches=batches, vertices=count, draw_sections=batches * 4,
        compared_words=len(expected) // 4, mismatches=dict(bad), first_mismatches=first,
        adapter_requested=os.environ.get('PS2X_HWGS_ADAPTER'), dll_sha256=digest(dll.read_bytes()),
        prepared_manifest_sha256=digest((prepared / 'manifest.json').read_bytes()),
        fixtures_list_sha256=digest(fixtures.read_bytes()), inputs_sha256=digest(inputs),
        expected_sha256=digest(expected), actual_sha256=digest(actual),
        scope='Offline combined GPU lighting/reflection/projection of original source vertices. Four ordered GIF sections and native vertex format; diagnostic readback only, no live offload/FPS claim.')
    if report['exact'] and capture_path:
        captured = struct.pack('<4I32s', 0x52504750, 1, count, 320, hashlib.sha256(inputs).digest()) + actual
        with capture_path.open('xb') as stream:
            stream.write(captured)
        report['gpu_capture_sha256'] = digest(captured)
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return 0 if report['exact'] else 1


def draw(prepared, fixtures, dll, report_path, adapter):
    prepared, fixtures, dll, report_path = (private(p) for p in (prepared, fixtures, dll, report_path))
    if report_path.exists():
        raise FileExistsError('Use a fresh private reflection draw report')
    ends = []
    inputs, expected, batches, count = prepare(prepared, fixtures, ends)
    os.environ.update(PS2X_HWGS_API='d3d12', PS2X_HWGS_DIRECT='0')
    if adapter:
        os.environ['PS2X_HWGS_ADAPTER'] = adapter
    os.chdir(dll.parent)
    records = []
    with os.add_dll_directory(str(dll.parent)):
        gs = c.CDLL(str(dll))
        gs.tf_gs_open.argtypes = [c.c_int, c.c_int, c.c_char_p, c.c_void_p, c.c_uint32]
        gs.tf_gs_open.restype = c.c_int
        gs.tf_gs_close.argtypes, gs.tf_gs_close.restype = [], None
        probe = gs.tf_gs_gpu_reflection_draw_probe
        probe.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32, c.c_void_p,
                          c.c_uint32, c.c_void_p, c.c_void_p, c.c_uint32]
        probe.restype = c.c_int
        source, reference = c.create_string_buffer(inputs), c.create_string_buffer(expected[:count * 128])
        boundaries = (c.c_uint32 * len(ends))(*ends)
        error = c.create_string_buffer(4096)
        if not gs.tf_gs_open(1, 0, str(dll.parent / 'resources').encode(), error, len(error)):
            raise RuntimeError(error.value.decode(errors='replace'))
        try:
            for mode, name in enumerate(('vertex_color', 'textured_nearest', 'textured_linear', 'blended_two_pass')):
                stats = (c.c_uint64 * 13)()
                success = bool(probe(source, count, boundaries, batches, reference, mode, stats, error, len(error)))
                record = dict(mode=name, passed=success, error=None if success else error.value.decode(errors='replace'))
                record.update(zip(('allocations', 'reuses', 'dispatches', 'draws', 'painted_pixels', 'differing_pixels',
                                   'compared_pixels', 'reflection_dispatches', 'reflection_vertices', 'ordinary_draws',
                                   'changed_coordinates_on_reuse', 'colored_pixels', 'image_fingerprint'), stats))
                records.append(record)
                if not success:
                    break
        finally:
            gs.tf_gs_close()
    report = dict(passed=len(records) == 4 and all(r['passed'] for r in records) and
        len({r['image_fingerprint'] for r in records}) == 4, batches=batches, vertices=count,
        modes=records, adapter_requested=os.environ.get('PS2X_HWGS_ADAPTER'), dll_sha256=digest(dll.read_bytes()),
        prepared_manifest_sha256=digest((prepared / 'manifest.json').read_bytes()), inputs_sha256=digest(inputs),
        expected_sha256=digest(expected[:count * 128]),
        scope='Resident compute-to-native-draw diagnostic; only final images read back. Qualified source lighting/projection, four sections, texture/filter/blend modes and draw-fence reuse. Diagnostic viewport uses captured bounds; live frontend metadata and VU ownership are not yet integrated.')
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


def tail(prepared, fixtures, lighting_report, lighting_capture, projection_report, projection_capture, runner, report_path):
    prepared, fixtures, lighting_report, lighting_capture, projection_report, projection_capture, runner, report_path = (
        private(p) for p in (prepared, fixtures, lighting_report, lighting_capture, projection_report, projection_capture, runner, report_path))
    log_path, csv_path = report_path.with_suffix('.log'), report_path.with_suffix('.csv')
    if len({report_path, log_path, csv_path}) != 3 or any(p.exists() for p in (report_path, log_path, csv_path)):
        raise FileExistsError('Use a fresh private .json tail report and associated .log/.csv paths')
    manifest = json.loads((prepared / 'manifest.json').read_text())
    lighting_inputs, lighting_expected, batches = prepare_lighting((prepared / 'raw.bin').read_bytes())
    project_inputs, project_expected, project_batches, vertices = prepare(prepared, fixtures)
    if batches != project_batches:
        raise ValueError('Unpaired lighting/projection batches')
    for report_file, capture_file, magic, stride, inputs, expected in (
        (lighting_report, lighting_capture, 0x52464750, 48, lighting_inputs, lighting_expected),
        (projection_report, projection_capture, 0x52504750, 320, project_inputs, project_expected),
    ):
        result = json.loads(report_file.read_text())
        captured = capture_file.read_bytes()
        if (not result['exact'] or result['vertices'] != vertices or
                result['prepared_manifest_sha256'] != digest((prepared / 'manifest.json').read_bytes()) or
                result['inputs_sha256'] != digest(inputs) or result['expected_sha256'] != digest(expected) or
                result['actual_sha256'] != digest(expected) or result['gpu_capture_sha256'] != digest(captured) or
                len(captured) != 48 + vertices * stride or captured[48:] != expected or
                struct.unpack_from('<4I32s', captured) != (magic, 1, vertices, stride, hashlib.sha256(inputs).digest())):
            raise ValueError('Paired GPU capture/provenance mismatch')
    env = dict(replay_environment(), TF_REFLECTION_GPU_CAPTURE=str(lighting_capture),
        TF_REFLECTION_GPU_RAW=str(prepared / 'raw.bin'), TF_REFLECTION_PROJECT_GPU=str(projection_capture),
        TF_REFLECTION_PROJECT_RAW=str(prepared / 'projection.bin'))
    with log_path.open('w') as stream:
        process = subprocess.run([str(runner), str(fixtures), str(csv_path)], env=env,
                                 stdout=stream, stderr=subprocess.STDOUT)
    with csv_path.open() as stream:
        rows = list(csv.DictReader(stream))
    log = log_path.read_text()
    lighting_summary = re.search(r'REFLECTION tail calls=(\d+) replaced=(\d+) overlapping=(\d+) skipped_vertices=(\d+)', log)
    projection_summary = re.search(r'REFLECTION projection_tail calls=(\d+) replaced=(\d+) skipped_vertices=(\d+) poisoned_vertices=(\d+)', log)
    passed = (process.returncode == 0 and len(rows) == manifest['replay_fixtures'] and
        all(row[key] == '1' for row in rows for key in ('data', 'state', 'code', 'packets', 'control')) and
        lighting_summary is not None and projection_summary is not None and
        int(lighting_summary[1]) == batches and int(projection_summary[1]) == batches and
        int(projection_summary[2]) > 0 and int(projection_summary[4]) == int(lighting_summary[4]) > 0 and
        'REPLAY final mode=exact' in log and 'fallback=0 pending=0' in log)
    report = dict(passed=passed, replay_fixtures=len(rows), batches=batches, gpu_vertices=vertices,
        lighting_tail=dict(zip(('calls', 'replaced', 'overlapping', 'skipped_vertices'), map(int, lighting_summary.groups()))) if lighting_summary else {},
        projection_tail=dict(zip(('calls', 'replaced', 'skipped_vertices', 'poisoned_vertices'), map(int, projection_summary.groups()))) if projection_summary else {},
        runner_sha256=digest(runner.read_bytes()), lighting_report_sha256=digest(lighting_report.read_bytes()),
        lighting_capture_sha256=digest(lighting_capture.read_bytes()), projection_report_sha256=digest(projection_report.read_bytes()),
        projection_capture_sha256=digest(projection_capture.read_bytes()), log_sha256=digest(log_path.read_bytes()),
        csv_sha256=digest(csv_path.read_bytes()),
        scope='Paired accelerated lighting/projection CPU tails with poisoned deferred lighting geometry and actual GPU output. Complete state, memory, timing and ordered GIF; offline readback, no live offload/FPS claim.')
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    command = commands.add_parser('gpu')
    for name in ('prepared', 'fixtures', 'dll', 'report'):
        command.add_argument('--' + name, type=Path, required=True)
    command.add_argument('--adapter', help='Exact D3D adapter name')
    command.add_argument('--capture', type=Path, help='Write exact GPU output for the retained CPU-tail experiment')
    command = commands.add_parser('draw')
    for name in ('prepared', 'fixtures', 'dll', 'report'):
        command.add_argument('--' + name, type=Path, required=True)
    command.add_argument('--adapter', help='Exact D3D adapter name')
    command = commands.add_parser('tail')
    for name in ('prepared', 'fixtures', 'lighting-report', 'lighting-capture', 'projection-report', 'projection-capture', 'runner', 'report'):
        command.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'tail':
        return tail(args.prepared, args.fixtures, args.lighting_report, args.lighting_capture,
                    args.projection_report, args.projection_capture, args.runner, args.report)
    if args.command == 'draw':
        return draw(args.prepared, args.fixtures, args.dll, args.report, args.adapter)
    return run(args.prepared, args.fixtures, args.dll, args.report, args.adapter, args.capture)


if __name__ == '__main__':
    raise SystemExit(main())
