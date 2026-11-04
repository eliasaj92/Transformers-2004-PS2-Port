#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture accelerated reflection boundaries, then qualify GPU vertex arithmetic.

All game data stays in private files. Expected output is never uploaded to the
GPU. Readback is diagnostic only; this does not enable a live VU replacement.
"""
import argparse
from collections import Counter
import csv
import ctypes as c
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess

from test_gpu_dual_transform import digest, private


def prepare(raw):
    stride = 32 + 2 * (664 + 16384)
    if not raw or len(raw) % stride or len(raw) // stride > 10000:
        raise ValueError('Invalid reflection capture size')
    inputs, expected = bytearray(), bytearray()
    for index, offset in enumerate(range(0, len(raw), stride)):
        record = raw[offset:offset + stride]
        if struct.unpack_from('<4I', record) != (0x5245464c, 1, index, 16384):
            raise ValueError('Invalid reflection capture header/order')
        first_cycle, last_cycle = struct.unpack_from('<2Q', record, 16)
        if last_cycle <= first_cycle:
            raise ValueError('Invalid reflection cycle interval')
        vi = struct.unpack_from('<16i', record, 32 + 512)
        before = record[32 + 664:32 + 664 + 16384]
        after = record[32 + 664 + 16384 + 664:]
        base, vertices, constants = vi[1] & 1023, vi[10], vi[11] & 1023
        if not (1 <= vertices <= 28 and base + 3 * vertices + 14 <= 1024 and constants + 11 <= 1024):
            raise ValueError('Unqualified reflection source range')
        allowed = set()
        for vertex in range(vertices):
            first = (base + vertex * 3) * 16
            source = before[constants * 16:(constants + 11) * 16] + before[first:first + 48]
            if len(source) != 224 or not all(math.isfinite(x[0]) for x in struct.iter_unpack('<f', source)):
                raise ValueError('Non-finite or truncated reflection input')
            inputs.extend(source)
            expected.extend(after[first:first + 48])
            allowed.update(range(first, first + 32))
            allowed.update(range(first + 44, first + 48))
        # The pipelined CPU chain also stores the following vertex's color.
        # That tail and retained registers are deliberately outside this kernel.
        allowed.update(range((base + vertices * 3 + 1) * 16, (base + vertices * 3 + 2) * 16))
        if any(x != y and i not in allowed for i, (x, y) in enumerate(zip(before, after))):
            raise ValueError('Reflection wrote outside its observed geometry footprint')
    if len(inputs) // 224 > 65536:
        raise ValueError('Reflection vertex limit exceeded; use a smaller capture')
    return bytes(inputs), bytes(expected), len(raw) // stride


def capture(fixtures, runner, output):
    fixtures, runner, output = (private(p) for p in (fixtures, runner, output))
    if output.exists():
        raise FileExistsError('Use a fresh private reflection directory')
    output.mkdir(parents=True)
    env = dict(os.environ, TF_REFLECTION_CAPTURE=str(output / 'raw.bin'))
    env.pop('TF_REFLECTION_GPU_CAPTURE', None)
    env.pop('TF_REFLECTION_GPU_RAW', None)
    with (output / 'replay.log').open('w') as log:
        subprocess.run([str(runner), str(fixtures), str(output / 'replay.csv')], env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    with (output / 'replay.csv').open() as stream:
        rows = list(csv.DictReader(stream))
    if not rows or any(row[key] != '1' for row in rows for key in ('data', 'state', 'code', 'packets', 'control')):
        raise ValueError('Accelerated replay did not preserve complete captured output')
    log = (output / 'replay.log').read_text()
    if 'REPLAY final mode=exact' not in log or 'fallback=0 pending=0' not in log:
        raise ValueError('Capture lacks exact accelerated replay completion')
    raw = (output / 'raw.bin').read_bytes()
    inputs, expected, count = prepare(raw)
    for name, data in (('inputs.bin', inputs), ('expected.bin', expected)):
        (output / name).write_bytes(data)
    names = ('raw.bin', 'inputs.bin', 'expected.bin', 'replay.csv', 'replay.log')
    manifest = dict(version=1, captures=count, vertices=len(inputs) // 224, replay_fixtures=len(rows),
        runner_sha256=digest(runner.read_bytes()), fixtures_list_sha256=digest(fixtures.read_bytes()),
        sha256={name: digest((output / name).read_bytes()) for name in names},
        scope='Authenticated reflection chain in an exact accelerated VU replay; no interpreter or game launch')
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    print(json.dumps(manifest, indent=2))


def gpu(prepared, dll, report_path, adapter, capture_path=None):
    prepared, dll, report_path = (private(p) for p in (prepared, dll, report_path))
    if report_path.exists():
        raise FileExistsError('Use a fresh private GPU report')
    if capture_path:
        capture_path = private(capture_path)
        if capture_path.exists() or capture_path == report_path:
            raise FileExistsError('Use distinct fresh private GPU capture and report paths')
    manifest = json.loads((prepared / 'manifest.json').read_text())
    if manifest['version'] != 1:
        raise ValueError('Unsupported reflection fixture version')
    files = {}
    for name in ('raw.bin', 'inputs.bin', 'expected.bin', 'replay.csv', 'replay.log'):
        files[name] = (prepared / name).read_bytes()
        if digest(files[name]) != manifest['sha256'][name]:
            raise ValueError('Reflection fixture changed: ' + name)
    inputs, expected, count = prepare(files['raw.bin'])
    if (inputs != files['inputs.bin'] or expected != files['expected.bin'] or
            count != manifest['captures'] or len(inputs) // 224 != manifest['vertices']):
        raise ValueError('Prepared reflection data differs from the accelerated boundary capture')
    vertices = manifest['vertices']
    os.environ.update(PS2X_HWGS_API='d3d12', PS2X_HWGS_DIRECT='0')
    if adapter:
        os.environ['PS2X_HWGS_ADAPTER'] = adapter
    os.chdir(dll.parent)
    with os.add_dll_directory(str(dll.parent)):
        gs = c.CDLL(str(dll))
        gs.tf_gs_open.argtypes = [c.c_int, c.c_int, c.c_char_p, c.c_void_p, c.c_uint32]
        gs.tf_gs_open.restype = c.c_int
        gs.tf_gs_close.argtypes = []
        gs.tf_gs_close.restype = None
        probe = gs.tf_gs_gpu_reflection_probe
        probe.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32, c.c_void_p, c.c_uint32]
        probe.restype = c.c_int
        error, source = c.create_string_buffer(4096), c.create_string_buffer(inputs)
        output = c.create_string_buffer(len(expected))
        if not gs.tf_gs_open(1, 0, str(dll.parent / 'resources').encode(), error, len(error)):
            raise RuntimeError(error.value.decode(errors='replace'))
        try:
            if not probe(source, vertices, output, len(output), error, len(error)):
                raise RuntimeError(error.value.decode(errors='replace'))
        finally:
            gs.tf_gs_close()
    actual, bad, first = output.raw, Counter(), []
    for index, (x, y) in enumerate(zip(struct.iter_unpack('<I', actual), struct.iter_unpack('<I', expected))):
        if x != y:
            bad[index % 12] += 1
            if len(first) < 12:
                first.append(dict(vertex=index // 12, word=index % 12, actual=x[0], expected=y[0]))
    report = dict(exact=actual == expected, captures=count, vertices=vertices, compared_words=len(expected) // 4,
        mismatches=dict(bad), first_mismatches=first, adapter_requested=os.environ.get('PS2X_HWGS_ADAPTER'),
        dll_sha256=digest(dll.read_bytes()), prepared_manifest_sha256=digest((prepared / 'manifest.json').read_bytes()),
        inputs_sha256=digest(inputs), expected_sha256=digest(expected), actual_sha256=digest(actual),
        scope='Offline optimized DXC GPU lighting/reflection; retained VU state and live resident integration remain unfinished')
    if report['exact'] and capture_path:
        header = struct.pack('<4I32s', 0x52464750, 1, vertices, 48, hashlib.sha256(inputs).digest())
        with capture_path.open('xb') as stream:
            stream.write(header + actual)
        report['gpu_capture_sha256'] = digest(header + actual)
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return 0 if report['exact'] else 1


def tail(prepared, gpu_report, gpu_capture, fixtures, runner, report_path):
    prepared, gpu_report, gpu_capture, fixtures, runner, report_path = (
        private(p) for p in (prepared, gpu_report, gpu_capture, fixtures, runner, report_path))
    log_path, csv_path = report_path.with_suffix('.log'), report_path.with_suffix('.csv')
    if len({report_path, log_path, csv_path}) != 3 or any(p.exists() for p in (report_path, log_path, csv_path)):
        raise FileExistsError('Use a fresh private .json tail report and associated .log/.csv paths')
    manifest = json.loads((prepared / 'manifest.json').read_text())
    result = json.loads(gpu_report.read_text())
    captured = gpu_capture.read_bytes()
    raw = (prepared / 'raw.bin').read_bytes()
    inputs, _, count = prepare(raw)
    vertices = len(inputs) // 224
    if (manifest['version'] != 1 or not result['exact'] or count != manifest['captures'] or
            vertices != manifest['vertices'] or result['vertices'] != vertices or
            digest(raw) != manifest['sha256']['raw.bin'] or
            digest(fixtures.read_bytes()) != manifest['fixtures_list_sha256'] or
            digest((prepared / 'manifest.json').read_bytes()) != result['prepared_manifest_sha256'] or
            digest(captured) != result['gpu_capture_sha256'] or len(captured) != 48 + vertices * 48 or
            struct.unpack_from('<4I32s', captured) != (0x52464750, 1, vertices, 48, hashlib.sha256(inputs).digest())):
        raise ValueError('Reflection tail input/GPU provenance mismatch')
    env = dict(os.environ, TF_REFLECTION_GPU_CAPTURE=str(gpu_capture), TF_REFLECTION_GPU_RAW=str(prepared / 'raw.bin'))
    env.pop('TF_REFLECTION_CAPTURE', None)
    with log_path.open('w') as stream:
        process = subprocess.run([str(runner), str(fixtures), str(csv_path)], env=env,
                                 stdout=stream, stderr=subprocess.STDOUT)
    with csv_path.open() as stream:
        rows = list(csv.DictReader(stream))
    log = log_path.read_text()
    summary = re.search(r'REFLECTION tail calls=(\d+) replaced=(\d+) overlapping=(\d+) skipped_vertices=(\d+)', log)
    passed = (process.returncode == 0 and len(rows) == manifest['replay_fixtures'] and
              all(row[key] == '1' for row in rows for key in ('data', 'state', 'code', 'packets', 'control')) and
              summary is not None and int(summary[1]) == count and int(summary[2]) > 0 and
              'fallback=0 pending=0' in log)
    report = dict(passed=passed, replay_fixtures=len(rows), captures=count, gpu_vertices=vertices,
        tail_counts=dict(zip(('calls', 'replaced', 'overlapping', 'skipped_vertices'), map(int, summary.groups()))) if summary else {},
        runner_sha256=digest(runner.read_bytes()), gpu_report_sha256=digest(gpu_report.read_bytes()),
        gpu_capture_sha256=digest(captured), log_sha256=digest(log_path.read_bytes()), csv_sha256=digest(csv_path.read_bytes()),
        scope='Accelerated CPU-tail experiment using actual GPU results; complete state/memory/timing/ordered-GIF comparison. Offline readback, not a live offload or FPS claim.')
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    command = commands.add_parser('capture')
    for name in ('fixtures', 'runner', 'output'):
        command.add_argument('--' + name, type=Path, required=True)
    command = commands.add_parser('gpu')
    for name in ('prepared', 'dll', 'report'):
        command.add_argument('--' + name, type=Path, required=True)
    command.add_argument('--adapter', help='Exact D3D adapter name; defaults to the existing environment/renderer choice')
    command.add_argument('--capture', type=Path, help='Write exact GPU output for the accelerated tail experiment')
    command = commands.add_parser('tail')
    for name in ('prepared', 'gpu-report', 'gpu-capture', 'fixtures', 'runner', 'report'):
        command.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'capture':
        capture(args.fixtures, args.runner, args.output)
        return 0
    if args.command == 'tail':
        return tail(args.prepared, args.gpu_report, args.gpu_capture, args.fixtures, args.runner, args.report)
    return gpu(args.prepared, args.dll, args.report, args.adapter, args.capture)


if __name__ == '__main__':
    raise SystemExit(main())
