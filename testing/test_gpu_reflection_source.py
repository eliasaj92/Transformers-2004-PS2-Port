#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify compact reflection sources, fallback and bounds against enhanced captures."""
import argparse
import ctypes as c
import json
import os
from pathlib import Path
import struct

from test_gpu_dual_transform import digest, private
from test_gpu_reflection_projection import prepare, record


def compact(prepared, fixtures):
    ends = []
    inputs, expected, batches, count = prepare(prepared, fixtures, ends)
    projection = (prepared / 'projection.bin').read_bytes()
    lighting = (prepared / 'expected.bin').read_bytes()
    result, first = [], 0
    for index, end in enumerate(ends):
        vertices = end - first
        source = inputs[first * 368:end * 368]
        state, memory, _, output = record(projection, index, 0x5250524a)
        vi = struct.unpack_from('<16i', state, 512)
        base, n = vi[1], vi[10]
        if vertices != n or not 3 <= n <= 22:
            raise ValueError('Compact reflection source count outside supported range')
        packet = bytearray(1072)
        struct.pack_into('<4I2Q', packet, 0, 2, n, 15, 2, index + 1, 1)
        packet[32 + 720:32 + 896] = source[:176]
        packet[928:1060] = source[224:356]
        packet[1060:1068] = memory[(base + 3 * n + 3) * 16:(base + 3 * n + 3) * 16 + 8]
        for vertex in range(n):
            v = source[vertex * 368:(vertex + 1) * 368]
            if v[:176] != source[:176] or v[224:356] != source[224:356]:
                raise ValueError('Source constants changed inside one packet')
            offset = 32 + vertex * 32
            packet[offset:offset + 32] = v[208:220] + v[176:180] + v[192:204] + v[180:184]
        size = 4 * (4 + n * 3) * 16
        result.append((bytes(packet), lighting[first * 48:end * 48], output[(base - 4) * 16:(base - 4) * 16 + size]))
        first = end
    return result, inputs, expected, count


def run(prepared, fixtures, dll, report_path, adapter):
    prepared, fixtures, dll, report_path = map(private, (prepared, fixtures, dll, report_path))
    if report_path.exists():
        raise FileExistsError('Use a fresh private reflection source report')
    packets, original, expected, count = compact(prepared, fixtures)
    records, expanded = [], bytearray()
    os.environ.update(PS2X_HWGS_API='d3d12', PS2X_HWGS_DIRECT='0')
    if adapter:
        os.environ['PS2X_HWGS_ADAPTER'] = adapter
    os.chdir(dll.parent)
    gpu_exact = False
    with os.add_dll_directory(str(dll.parent)):
        gs = c.CDLL(str(dll))
        probe = gs.tf_gs_reflection_source_probe
        probe.argtypes = [c.c_void_p, c.c_void_p, c.c_void_p, c.c_uint32, c.c_void_p,
                          c.c_uint32, c.c_void_p, c.c_void_p, c.c_uint32]
        probe.restype = c.c_int
        error = c.create_string_buffer(4096)
        for index, (packet, lit, gif) in enumerate(packets):
            n = struct.unpack_from('<I', packet, 4)[0]
            output, stats = c.create_string_buffer(n * 368), (c.c_uint64 * 8)()
            passed = bool(probe(c.create_string_buffer(packet), c.create_string_buffer(lit),
                c.create_string_buffer(gif), len(gif), output, len(output), stats, error, len(error)))
            result = dict(index=index, vertices=n, passed=passed,
                          error=None if passed else error.value.decode(errors='replace'))
            result.update(zip(('projected_mismatches', 'lighting_mismatches', 'bounds_accepted',
                               'nodes', 'leaves', 'bounds_checks', 'rejection_checks', 'bounds_nanoseconds'), stats))
            records.append(result)
            expanded.extend(output.raw)
            if not passed:
                break
        if len(records) == len(packets) and all(r['passed'] for r in records):
            gs.tf_gs_open.argtypes = [c.c_int, c.c_int, c.c_char_p, c.c_void_p, c.c_uint32]
            gs.tf_gs_open.restype = c.c_int
            gs.tf_gs_close.argtypes, gs.tf_gs_close.restype = [], None
            gpu = gs.tf_gs_gpu_reflection_projection_probe
            gpu.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32]
            gpu.restype = c.c_int
            if not gs.tf_gs_open(1, 0, str(dll.parent / 'resources').encode(), error, len(error)):
                raise RuntimeError(error.value.decode(errors='replace'))
            try:
                output = c.create_string_buffer(len(expected))
                if not gpu(c.create_string_buffer(bytes(expanded)), count, output, len(output), error, len(error)):
                    raise RuntimeError(error.value.decode(errors='replace'))
                gpu_exact = output.raw == expected
            finally:
                gs.tf_gs_close()
    report = dict(passed=len(records) == len(packets) and all(r['passed'] for r in records) and gpu_exact,
        batches=len(packets), vertices=count, canonical_expansion_gpu_exact=gpu_exact,
        bounds_accepted=sum(r['bounds_accepted'] for r in records),
        total_bounds_nanoseconds=sum(r['bounds_nanoseconds'] for r in records), records=records,
        dll_sha256=digest(dll.read_bytes()), inputs_sha256=digest(original),
        compact_sha256=digest(b''.join(p[0] for p in packets)), expanded_sha256=digest(expanded),
        prepared_manifest_sha256=digest((prepared / 'manifest.json').read_bytes()),
        scope='Source-only fallback, conservative draw bounds and canonical GPU expansion. Captured accelerated geometry is comparison-only. No live integration or FPS claim.')
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps({k: v for k, v in report.items() if k != 'records'}, indent=2))
    for r in records:
        if not r['passed']:
            print(json.dumps(r))
    return 0 if report['passed'] else 1


def culling(prepared, fixtures, dll, report_path, adapter):
    prepared, fixtures, dll, report_path = map(private, (prepared, fixtures, dll, report_path))
    if report_path.exists():
        raise FileExistsError('Use a fresh private reflection culling report')
    packets, inputs, expected, count = compact(prepared, fixtures)
    cases, first, seen_counts = [], 0, set()
    for index, (packet, _, gif) in enumerate(packets):
        n = struct.unpack_from('<I', packet, 4)[0]
        source, native = inputs[first * 368:(first + n) * 368], expected[first * 128:(first + n) * 128]
        first += n
        scissors, offsets = [], []
        for section in range(4):
            start = section * (4 + 3 * n) * 16
            scissor, offset = struct.unpack_from('<Q', gif, start + 16)[0], struct.unpack_from('<Q', gif, start + 32)[0]
            x0, x1, y0, y1 = ((scissor >> shift) & 2047 for shift in (0, 16, 32, 48))
            scissors.append((x0 * 16 - 8, y0 * 16 - 8, x1 * 16 + 8, y1 * 16 + 8))
            offsets.append((offset & 65535, (offset >> 32) & 65535, 0, 0))
        output, rejected = bytearray(), 0
        for triangle in range(n - 2):
            skip = []
            for section in range(4):
                points = []
                for corner in range(3):
                    xy = struct.unpack_from('<I', native, ((triangle + corner) * 4 + section) * 32 + 16)[0]
                    points.append(((xy & 65535) - offsets[section][0], (xy >> 16) - offsets[section][1]))
                box = [min(p[0] for p in points), min(p[1] for p in points),
                       max(p[0] for p in points), max(p[1] for p in points)]
                box[2] -= box[2] % 16 == 0
                box[3] -= box[3] % 16 == 0
                s = scissors[section]
                culled = (box[0] >= box[2] or box[1] >= box[3] or box[2] + 1 <= s[0] or box[3] + 1 <= s[1]
                          or box[0] >= s[2] or box[1] >= s[3] or len(set(points)) < 3)
                skip.append(culled)
                rejected += culled
            for corner in range(3):
                for section in range(4):
                    offset = ((triangle + corner) * 4 + section) * 32
                    v = bytearray(native[offset:offset + 32])
                    if skip[section]:
                        start = (triangle * 4 + section) * 32
                        v[16:24] = native[start + 16:start + 24]
                    output.extend(v)
        mixed = 0 < rejected < (n - 2) * 4
        if (mixed and len(cases) < 12) or n not in seen_counts:
            raster = b''.join(struct.pack('<4i', *v) for v in scissors + offsets)
            cases.append((index, n, source + raster, bytes(output), rejected, mixed))
            seen_counts.add(n)
    os.environ.update(PS2X_HWGS_API='d3d12', PS2X_HWGS_DIRECT='0')
    if adapter:
        os.environ['PS2X_HWGS_ADAPTER'] = adapter
    os.chdir(dll.parent)
    results = []
    with os.add_dll_directory(str(dll.parent)):
        gs = c.CDLL(str(dll))
        gs.tf_gs_open.argtypes = [c.c_int, c.c_int, c.c_char_p, c.c_void_p, c.c_uint32]
        gs.tf_gs_open.restype = c.c_int
        gs.tf_gs_close.argtypes, gs.tf_gs_close.restype = [], None
        probe = gs.tf_gs_gpu_reflection_cull_probe
        probe.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32]
        probe.restype = c.c_int
        error = c.create_string_buffer(4096)
        if not gs.tf_gs_open(1, 0, str(dll.parent / 'resources').encode(), error, len(error)):
            raise RuntimeError(error.value.decode(errors='replace'))
        try:
            for index, n, source, reference, rejected, mixed in cases:
                actual = c.create_string_buffer(len(reference))
                if not probe(c.create_string_buffer(source), n, actual, len(actual), error, len(error)):
                    raise RuntimeError(error.value.decode(errors='replace'))
                results.append(dict(index=index, vertices=n, mixed=mixed, culled_triangles=rejected,
                    exact=actual.raw == reference, compared_words=len(reference) // 4,
                    actual_sha256=digest(actual.raw), expected_sha256=digest(reference)))
        finally:
            gs.tf_gs_close()
    report = dict(passed=bool(results) and all(r['exact'] for r in results) and any(r['mixed'] for r in results),
        cases=results, dll_sha256=digest(dll.read_bytes()), inputs_sha256=digest(inputs),
        adapter_requested=os.environ.get('PS2X_HWGS_ADAPTER'),
        scope='Diagnostic-only readback verifies GPU triangle expansion/culling against captured enhanced vertices and GS integer culling rules, including mixed surviving/culled strips. Production draws never read geometry back.')
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps(dict(passed=report['passed'], cases=len(results), mixed=sum(r['mixed'] for r in results),
        culled_triangles=sum(r['culled_triangles'] for r in results), compared_words=sum(r['compared_words'] for r in results)), indent=2))
    return 0 if report['passed'] else 1


def frontend(prepared, fixtures, dll, report_path, adapter, first=0, limit=None):
    prepared, fixtures, dll, report_path = map(private, (prepared, fixtures, dll, report_path))
    if report_path.exists():
        raise FileExistsError('Use a fresh private reflection frontend report')
    packets, _, _, count = compact(prepared, fixtures)
    packets = packets[first:first + limit] if limit is not None else packets[first:]
    if not packets:
        raise ValueError('Empty reflection packet selection')
    count = sum(struct.unpack_from('<I', p[0], 4)[0] for p in packets)
    sources = b''.join(p[0] for p in packets)
    captured = b''.join(p[2].ljust(4480, b'\0') for p in packets)
    os.environ.update(PS2X_HWGS_API='d3d12', PS2X_HWGS_DIRECT='0')
    if adapter:
        os.environ['PS2X_HWGS_ADAPTER'] = adapter
    os.chdir(dll.parent)
    modes = []
    with os.add_dll_directory(str(dll.parent)):
        gs = c.CDLL(str(dll))
        gs.tf_gs_open.argtypes = [c.c_int, c.c_int, c.c_char_p, c.c_void_p, c.c_uint32]
        gs.tf_gs_open.restype = c.c_int
        gs.tf_gs_close.argtypes, gs.tf_gs_close.restype = [], None
        probe = gs.tf_gs_reflection_frontend_probe
        probe.argtypes = [c.c_void_p, c.c_void_p, c.c_uint32, c.c_uint32, c.c_void_p, c.c_void_p, c.c_uint32]
        probe.restype = c.c_int
        error = c.create_string_buffer(4096)
        if not gs.tf_gs_open(4, 0, str(dll.parent / 'resources').encode(), error, len(error)):
            raise RuntimeError(error.value.decode(errors='replace'))
        try:
            for mode, name in enumerate(('nearest', 'linear_alpha_blend', 'mipmapped_blend', 'forced_cpu_fallback')):
                stats = (c.c_uint64 * 12)()
                passed = bool(probe(c.create_string_buffer(sources), c.create_string_buffer(captured),
                                    len(packets), mode, stats, error, len(error)))
                result = dict(mode=name, passed=passed, error=None if passed else error.value.decode(errors='replace'))
                result.update(zip(('packets', 'gpu_draws', 'cpu_materializations', 'declined_sections',
                    'differing_pixels', 'compared_pixels', 'latch_checks', 'painted_pixels', 'images',
                    'declined_packets', 'dispatches', 'gpu_vertices'), stats))
                modes.append(result)
                print(json.dumps(result), flush=True)
        finally:
            gs.tf_gs_close()
    report = dict(passed=len(modes) == 4 and all(m['passed'] for m in modes), batches=len(packets), vertices=count,
        modes=modes, dll_sha256=digest(dll.read_bytes()), compact_sha256=digest(sources), captured_sha256=digest(captured),
        first_batch=first, adapter_requested=os.environ.get('PS2X_HWGS_ADAPTER'),
        prepared_manifest_sha256=digest((prepared / 'manifest.json').read_bytes()),
        scope='Source-only native reflection through real GS frontend at 4x. Poisoned omitted geometry, ordered sections, texture/depth/filter/blend/fallback, image and latch comparison against accelerated captures. No live VU/FPS claim.')
    report_path.write_text(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('prepared', 'fixtures', 'dll', 'report'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--adapter')
    parser.add_argument('--frontend', action='store_true')
    parser.add_argument('--cull', action='store_true')
    parser.add_argument('--first', type=int, default=0)
    parser.add_argument('--limit', type=int)
    args = parser.parse_args()
    if args.cull:
        if args.frontend:
            parser.error('Choose either --cull or --frontend')
        raise SystemExit(culling(args.prepared, args.fixtures, args.dll, args.report, args.adapter))
    if args.frontend:
        raise SystemExit(frontend(args.prepared, args.fixtures, args.dll, args.report, args.adapter, args.first, args.limit))
    raise SystemExit(run(args.prepared, args.fixtures, args.dll, args.report, args.adapter))
