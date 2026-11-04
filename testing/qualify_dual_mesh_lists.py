# SPDX-License-Identifier: GPL-3.0-or-later
"""Import complete mesh lists and bind their chunks to consecutive enhanced VU captures.

All binary outputs remain private. This establishes decoded input and submission
grouping for the captured window, not a live engine hook or a state-liveness proof.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import struct

from import_grass_seam import import_list
from inspect_native_seam_capture import EESnapshot, read_vu
from test_gpu_dual_transform import private, digest


def qualify(snapshot_path, prepared_path, fixtures_path, output_path):
    snapshot_path, prepared_path, fixtures_path, output_path = (
        private(path) for path in (snapshot_path, prepared_path, fixtures_path, output_path))
    if output_path.exists():
        raise FileExistsError("Use a fresh private grouped-mesh directory")
    meta = json.loads((prepared_path / "manifest.json").read_text())
    if meta["version"] != 2:
        raise ValueError("Expected the qualified packed-XYZF2 fixture format")
    source = {name: (prepared_path / name).read_bytes() for name in ("inputs.bin", "batches.bin", "expected.bin", "packed.bin")}
    if any(digest(value) != meta["sha256"][name] for name, value in source.items()):
        raise ValueError("Prepared fixture changed")
    if (len(source["inputs.bin"]) != meta["vertices"] * 32 or
            len(source["batches.bin"]) != meta["batches"] * 144 or
            len(source["expected.bin"]) != meta["vertices"] * 64 or
            len(source["packed.bin"]) != meta["vertices"] * 96 or len(meta["selected"]) != meta["batches"]):
        raise ValueError("Prepared payload sizes disagree with their manifest")
    paths = {}
    for line in fixtures_path.read_text().splitlines():
        if not line.strip():
            continue
        path = private(Path(line))
        if path.name in paths:
            raise ValueError("Fixture names must be unique")
        paths[path.name] = path
    inputs_by_key = defaultdict(list)
    selected = {}
    for index, item in enumerate(meta["selected"]):
        path = paths[item["fixture"]]
        if digest(path.read_bytes()) != item["pre_sha256"]:
            raise ValueError("Captured VU input changed since qualification")
        head, _, data, _ = read_vu(path)
        count = item["vertices"]
        if not 3 <= count <= 28 or not 0 <= head[2] <= 1024 - 1 - count * 3:
            raise ValueError("Selected capture has an invalid dual source range")
        positions = tuple(struct.unpack_from("<3I", data, (head[2] + 3 + i * 3) * 16) for i in range(count))
        ticket = int(path.name.rsplit("-vu-", 1)[1][:6])
        if ticket in selected:
            raise ValueError("Mixed capture sequences are not supported")
        words = struct.unpack("<4096I", data)
        selected[ticket] = (index, words, item)
        inputs_by_key[(head[2], positions)].append(ticket)

    snapshot = EESnapshot(snapshot_path)
    ram = bytes(snapshot.ram)
    roots = {snapshot.root & 0x1fffffff}
    offset = 0
    while True:
        offset = ram.find(b"\x00\x00\x00\x50", offset)
        if offset < 0:
            break
        if offset % 16 == 0 and offset + 16 <= len(ram):
            _, address, launch, flush = struct.unpack_from("<4I", ram, offset)
            if launch == 0x14000000 and flush == 0x11000000 and address % 16 == 0 and 0x100000 <= (address & 0x1fffffff) < len(ram):
                roots.add(address & 0x1fffffff)
        offset += 4
    candidates = defaultdict(list)
    declined = Counter()
    imported = 0
    for address in sorted(roots):
        snapshot.root = address
        try:
            chunks, stream_hash = import_list(snapshot)
        except (ValueError, KeyError, IndexError) as error:
            declined[str(error)] += 1
            continue
        imported += 1
        matches = []
        for chunk in chunks:
            matches.append({ticket for ticket in inputs_by_key[(chunk["itop"], tuple(chunk["positions"]))]
                            if all(selected[ticket][1][word] == value for word, value in chunk["writes"].items())})
        for first in matches[0]:
            if not all(first + i in matches[i] for i in range(len(chunks))):
                continue
            # An engine group shares its two transforms and STQ bias. Per-chunk
            # restart masks and descriptors are decoded independently below.
            constants = [source["batches.bin"][selected[first + i][0] * 144:selected[first + i][0] * 144 + 132]
                         for i in range(len(chunks))]
            if any(value != constants[0] for value in constants):
                declined["matrix_or_bias_changed_inside_list"] += 1
                continue
            candidates[(first, len(chunks), stream_hash)].append((address, chunks))

    # Keep only non-overlapping sequences with one physical candidate root.
    # Identical assets at different addresses remain ambiguous ownership leads.
    groups, used = [], set()
    payload = {name: bytearray() for name in source}
    grouped_selected = []
    for (first, count, stream_hash), owners in sorted(candidates.items()):
        if len(owners) != 1:
            declined["ambiguous_physical_root"] += 1
            continue
        if any(first + i in used for i in range(count)):
            declined["overlapping_capture_group"] += 1
            continue
        address, chunks = owners[0]
        group_start = len(grouped_selected)
        for i, chunk in enumerate(chunks):
            index, _, item = selected[first + i]
            constants = bytearray(source["batches.bin"][index * 144:(index + 1) * 144])
            old_first = struct.unpack_from("<I", constants, 140)[0]
            new_first = len(payload["inputs.bin"]) // 32
            batch_index = len(grouped_selected)
            struct.pack_into("<I", constants, 140, new_first)
            itop, n, writes = chunk["itop"], chunk["count"], chunk["writes"]
            # Require every source word consumed by the GPU, including masks
            # and bias, to come from the decoded engine list itself.
            bias = writes[itop * 4 + 2]
            masks = tuple(writes[(itop + n * 3 + 4) * 4 + lane] for lane in range(2))
            if (bias, *masks) != struct.unpack_from("<3I", constants, 128):
                raise ValueError("Imported descriptor does not match qualified GPU constants")
            for vertex in range(n):
                base = (itop + 1 + vertex * 3) * 4
                stq = tuple(writes[base + lane] for lane in range(3))
                position = tuple(writes[base + 8 + lane] for lane in range(3))
                color = tuple(writes[base + 4 + lane] for lane in range(4))
                if any(value > 255 for value in color):
                    raise ValueError("Imported color is outside the qualified format")
                rgba = sum(value << (lane * 8) for lane, value in enumerate(color))
                record = struct.pack("<8I", *position, index, *stq, rgba)
                original = source["inputs.bin"][(old_first + vertex) * 32:(old_first + vertex + 1) * 32]
                if record != original:
                    raise ValueError("List-decoded geometry differs from the captured GPU input")
                payload["inputs.bin"].extend(struct.pack("<8I", *position, batch_index, *stq, rgba))
            payload["batches.bin"].extend(constants)
            payload["expected.bin"].extend(source["expected.bin"][old_first * 64:(old_first + n) * 64])
            payload["packed.bin"].extend(source["packed.bin"][old_first * 96:(old_first + n) * 96])
            grouped_selected.append(item)
            used.add(first + i)
        groups.append({"first_batch": group_start, "end_batch": len(grouped_selected), "root": f"{address:08x}",
                       "first_ticket": first, "chunks": count, "stream_sha256": stream_hash})
    if not groups:
        raise ValueError("No unambiguous complete list sequences matched")
    result = {"version": 2, "batches": len(grouped_selected), "vertices": len(payload["inputs.bin"]) // 32,
              "sha256": {name: digest(value) for name, value in payload.items()}, "selected": grouped_selected,
              "groups": groups, "candidate_roots": len(roots), "imported_roots": imported, "declined": dict(declined),
              "snapshot_sha256": digest(snapshot_path.read_bytes()),
              "source_manifest_sha256": digest((prepared_path / "manifest.json").read_bytes()),
              "scope": "List-decoded geometry with captured transforms; consecutive capture groups, not live ownership"}
    output_path.mkdir(parents=True)
    for name, value in payload.items():
        (output_path / name).write_bytes(value)
    (output_path / "manifest.json").write_text(json.dumps(result, indent=2))
    print(json.dumps({key: value for key, value in result.items() if key not in ("selected", "groups", "sha256")}, indent=2))
    print(f"Grouped {result['batches']} VU chunks into {len(groups)} complete mesh submissions.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--prepared", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    qualify(args.snapshot, args.prepared, args.fixtures, args.output)
