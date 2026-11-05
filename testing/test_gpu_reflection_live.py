#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare private enhanced reflection fixtures and qualify live memory/hooks."""
import argparse
import csv
import json
import re
from pathlib import Path
import subprocess

from test_gpu_dual_transform import digest, private
from test_gpu_reflection_source import compact
from test_gpu_reflection import replay_environment


def prepare(prepared, fixtures, output):
    prepared, fixtures, output = map(private, (prepared, fixtures, output))
    if output.exists():
        raise FileExistsError('Use a fresh private reflection fixture directory')
    packets, _, _, count = compact(prepared, fixtures)
    data = {'sources.bin': b''.join(p[0] for p in packets),
            'packets.bin': b''.join(p[2].ljust(4480, b'\0') for p in packets)}
    output.mkdir(parents=True)
    for name, value in data.items():
        (output / name).write_bytes(value)
    manifest = dict(version=1, batches=len(packets), vertices=count,
        prepared_manifest_sha256=digest((prepared / 'manifest.json').read_bytes()),
        fixtures_sha256=digest(fixtures.read_bytes()), sha256={k: digest(v) for k, v in data.items()})
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    print(json.dumps(manifest, indent=2))


def memory(prepared, runner, report):
    prepared, runner, report = map(private, (prepared, runner, report))
    if report.exists():
        raise FileExistsError('Use a fresh private reflection memory report')
    manifest = json.loads((prepared / 'manifest.json').read_text())
    if manifest['version'] != 1 or set(manifest['sha256']) != {'sources.bin', 'packets.bin'}:
        raise ValueError('Invalid reflection fixture manifest')
    for name, expected in manifest['sha256'].items():
        if digest((prepared / name).read_bytes()) != expected:
            raise ValueError('Reflection fixture changed: ' + name)
    result = subprocess.run([str(runner), str(prepared / 'sources.bin'), str(prepared / 'packets.bin')],
                            text=True, capture_output=True)
    passed = result.returncode == 0 and f"REFLECTION_MEMORY packets={manifest['batches']} " in result.stdout and 'dirty=0 PASS' in result.stdout
    value = dict(passed=passed, exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr,
        runner_sha256=digest(runner.read_bytes()), manifest_sha256=digest((prepared / 'manifest.json').read_bytes()))
    report.write_text(json.dumps(value, indent=2))
    print(json.dumps(value, indent=2))
    return 0 if passed else 1


def replay(prepared, fixtures, runner, report):
    prepared, fixtures, runner, report = map(private, (prepared, fixtures, runner, report))
    log_path, csv_path, source_path = (report.with_suffix(s) for s in ('.log', '.csv', '.sources.bin'))
    if any(p.exists() for p in (report, log_path, csv_path, source_path)):
        raise FileExistsError('Use fresh private reflection replay paths')
    packets, _, _, vertices = compact(prepared, fixtures)
    source_path.write_bytes(b''.join(p[0] for p in packets))
    env = dict(replay_environment(), TF_REFLECTION_LIVE_SOURCES=str(source_path))
    with log_path.open('w') as stream:
        result = subprocess.run([str(runner), str(fixtures), str(csv_path)], env=env,
                                stdout=stream, stderr=subprocess.STDOUT)
    rows = list(csv.DictReader(csv_path.open())) if csv_path.exists() else []
    log = log_path.read_text()
    manifest = json.loads((prepared / 'manifest.json').read_text())
    summary = re.search(r'REFLECTION_LIVE packets=(\d+) lighting_skipped=(\d+) projection_skipped=(\d+) poison=1 dirty=0 PASS', log)
    carry = re.search(r'REFLECTION_CARRY calls=(\d+) external_qwords=(\d+) overwritten=(\d+) preserved_w=(\d+)', log)
    passed = (result.returncode == 0 and len(rows) == manifest['replay_fixtures'] and summary is not None and
        all(row[k] == '1' for row in rows for k in ('data', 'state', 'code', 'packets', 'control')) and
        'fallback=0 pending=0' in log and int(summary[1]) > 0 and carry is not None and int(carry[1]) > 0)
    value = dict(passed=passed, exit_code=result.returncode, replay_fixtures=len(rows), captured_vertices=vertices,
        live=dict(zip(('packets', 'lighting_skipped', 'projection_skipped'), map(int, summary.groups()))) if summary else {},
        carry=dict(zip(('calls', 'external_qwords', 'overwritten', 'preserved_w'), map(int, carry.groups()))) if carry else {},
        runner_sha256=digest(runner.read_bytes()), manifest_sha256=digest((prepared / 'manifest.json').read_bytes()),
        sources_sha256=digest(source_path.read_bytes()), fixtures_sha256=digest(fixtures.read_bytes()),
        log_sha256=digest(log_path.read_bytes()), csv_sha256=digest(csv_path.read_bytes()) if csv_path.exists() else None,
        scope='Shared live hooks carrying deferred memory across consecutive calls and observed external writes; poisoned omitted lighting, exact GPU-source companions, complete VU state/memory/cycles and ordered GIF. Accelerated captures only; no live FPS claim.')
    report.write_text(json.dumps(value, indent=2))
    print(json.dumps(value, indent=2))
    if not passed:
        print(log[-5000:])
    return 0 if passed else 1


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    commands = p.add_subparsers(dest='command', required=True)
    c = commands.add_parser('prepare')
    for name in ('prepared', 'fixtures', 'output'):
        c.add_argument('--' + name, type=Path, required=True)
    c = commands.add_parser('memory')
    for name in ('prepared', 'runner', 'report'):
        c.add_argument('--' + name, type=Path, required=True)
    c = commands.add_parser('replay')
    for name in ('prepared', 'fixtures', 'runner', 'report'):
        c.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args()
    if a.command == 'prepare':
        prepare(a.prepared, a.fixtures, a.output)
    elif a.command == 'memory':
        raise SystemExit(memory(a.prepared, a.runner, a.report))
    else:
        raise SystemExit(replay(a.prepared, a.fixtures, a.runner, a.report))
