#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify an offline grass-list importer against accelerated VU input captures.

Only the observed single-RET, absolute-address, 1/1 or 3/1 layout is admitted.
The importer produces constant word patches per geometry chunk, independently
of VU execution. It does not replace geometry, emulate the shader, or establish
that persistent state and surrounding GS state are safe to skip.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct

from inspect_native_seam_capture import EESnapshot, read_vu, vif_commands
from summarize_native_seam import fingerprint


GRASS_SHA256 = "1833fd99c4497bb04fa6f4a18651fb9bb5bbf744fbaf4e0e18e6c04a0620c55e"


def import_list(snapshot):
    tags, stream, terminal = snapshot.dma_list()
    if len(tags) != 1 or terminal != "outer_RET":
        raise ValueError("Importer requires one contiguous RET list")
    commands = vif_commands(stream, 0x0404)
    row = col = None
    mask, mode, itop, writes = None, 0, None, {}
    chunks = []
    for command in commands:
        name, immediate = command["name"], command["immediate"]
        payload = command["offset"] + 4
        if name in ("NOP", "FLUSH", "STCYCL"):
            continue
        if name == "STROW":
            row = struct.unpack_from("<4I", stream, payload)
        elif name == "STCOL":
            col = struct.unpack_from("<4I", stream, payload)
        elif name == "STMASK":
            mask, = struct.unpack_from("<I", stream, payload)
        elif name == "STMOD":
            if immediate not in (0, 1):
                raise ValueError("Importer does not support differential STMOD")
            mode = immediate
        elif name == "ITOP":
            if immediate > 1023:
                raise ValueError("ITOP outside VU memory")
            itop = immediate
        elif name == "UNPACK":
            opcode = int(command["opcode"], 16)
            if opcode not in (0x6c, 0x68, 0x62, 0x72, 0x75, 0x65, 0x6e):
                raise ValueError(f"Unqualified grass UNPACK format {opcode:02x}")
            if immediate & 0x8000 or command["cycle"] not in (0x0101, 0x0103):
                raise ValueError("Importer requires absolute destinations and 1/1 or 3/1 cycles")
            lanes, width = (opcode >> 2 & 3) + 1, 4 >> (opcode & 3)
            count, dest, stride = command["num"] or 256, immediate & 1023, command["cycle"] & 255
            unsigned, masked = bool(immediate & 0x4000), bool(opcode & 0x10)
            if (masked and mask is None) or (mode and row is None):
                raise ValueError("Missing preceding VIF mask or row")
            for index in range(count):
                start = payload + index * lanes * width
                values = [int.from_bytes(stream[start + n * width:start + (n + 1) * width],
                                         "little", signed=width < 4 and not unsigned) & 0xffffffff
                          for n in range(lanes)]
                if lanes == 1:
                    values *= 4
                elif lanes == 2:
                    values *= 2  # Qualified V2-16 repeats x/y into z/w.
                elif lanes == 3:
                    # Qualified hardware V3-32: w is lookahead or zero,
                    # depending on the true DMA quadword phase. This list has
                    # one TTE tag, whose eight bytes start at stream offset 0.
                    phase = ((payload + 8) // 4) & 3
                    next_word = int.from_bytes(stream[start + 12:start + 16], "little")
                    values.append(0 if (index & 1) != (phase & 1) else next_word)
                qword = dest + index * stride
                if qword > 1023:
                    raise ValueError("Importer does not admit wrapped VU writes")
                for lane, value in enumerate(values):
                    selection = mask >> (lane * 2) & 3 if masked else 0
                    if selection == 3:
                        continue
                    if selection == 1:
                        if row is None:
                            raise ValueError("Missing row for masked write")
                        value = row[lane]
                    elif selection == 2:
                        if col is None:
                            raise ValueError("Missing column for masked write")
                        value = col[0]  # Both admitted cycles have WL=1.
                    elif mode == 1:
                        value = (value + row[lane]) & 0xffffffff
                    writes[qword * 4 + lane] = value
        elif name == "MSCNT":
            if itop is None or itop * 4 not in writes or itop * 4 + 1 not in writes:
                raise ValueError("Missing chunk descriptor")
            count = writes[itop * 4] & 0x7fff
            if writes[itop * 4 + 1] != 3 or not 2 <= count <= 28:
                raise ValueError("Unqualified grass descriptor format or count")
            # The first two vertices seed a triangle strip; each subsequent
            # vertex completes a source triangle (subject to restart/cull bits).
            # Keep raw bits: an importer must not silently round asset floats.
            positions = [tuple(writes[(itop + 3 + n * 3) * 4 + lane] for lane in range(3))
                         for n in range(count)]
            chunks.append({"itop": itop, "count": count, "writes": writes,
                           "positions": positions, "stream_offset": command["offset"]})
            writes = {}
        else:
            raise ValueError(f"Unqualified grass command {name}")
    if writes or not chunks:
        raise ValueError("Incomplete grass list")
    return chunks, hashlib.sha256(stream).hexdigest()


def qualify(prefix, trace_path):
    snapshot = EESnapshot(Path(f"{prefix}-ee.bin"))
    chunks, stream_hash = import_list(snapshot)
    with trace_path.open(newline="", encoding="utf-8") as file:
        rows = list(csv.DictReader(file))
    begins = {int(r["capture"]): r for r in rows if r["event"] == "vu_begin"}
    ends = {int(r["capture"]): r for r in rows if r["event"] == "vu_end"}
    if not begins or begins.keys() != ends.keys():
        raise ValueError("Missing VU capture begin/end records")
    matches = [[] for _ in chunks]
    for ticket, begin in sorted(begins.items()):
        end = ends.get(ticket)
        if not end or int(end["cycle"]) or int(end["mode"]):
            continue
        head, code, data, state = read_vu(Path(f"{prefix}-vu-{ticket:06d}-pre.bin"))
        if not head[0] >> 31 or head[0] & 0x7fffffff not in (0x10, 0xf8):
            continue
        # Require the observed single grass stage at 0x1380, constants q53.
        if struct.unpack_from("<I", data, 64)[0] != 1 or struct.unpack_from("<2I", data, 80) != (624, 53):
            continue
        if hashlib.sha256(code[0x1380:0x1380 + 3872]).hexdigest() != GRASS_SHA256:
            continue
        if fingerprint(data) != begin["hash"] or fingerprint(code) != begin["code_hash"]:
            raise ValueError("Grass input fixture does not match its capture trace")
        words = struct.unpack("<4096I", data)
        for index, chunk in enumerate(chunks):
            if head[2] == chunk["itop"] and all(words[word] == value for word, value in chunk["writes"].items()):
                matches[index].append(ticket)
    report = {"root": f"{snapshot.root:08x}", "stream_sha256": stream_hash,
              "chunks": [{"index": index, "itop": chunk["itop"],
                          "descriptor_count": chunk["count"], "source_triangles": chunk["count"] - 2,
                          "compared_words": len(chunk["writes"]), "matching_vu_tickets": matches[index]}
                         for index, chunk in enumerate(chunks)],
              "all_chunks_have_exact_input_match": all(matches),
              "limitations": ["Only words written by this list are compared; masked preserved lanes remain dependencies.",
                              "Repeated meshes can match more than one invocation; this is not unique submission attribution.",
                              "No shader output, persistent VU state, GS state, or performance replacement is qualified."]}
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    args = parser.parse_args()
    result = qualify(args.prefix, args.trace)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["all_chunks_have_exact_input_match"] else 1)
