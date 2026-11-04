#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inspect private whole-list and accelerated VU fixtures without executing code.

Emits metadata and fingerprints only. Input bytes, including captured RAM,
meshes, microcode and GIF packets, must stay outside the source checkout.
This is an observation report, not a renderer or a replacement qualification.
"""
import argparse
from collections import Counter
import csv
import hashlib
import json
from pathlib import Path
import struct

from summarize_native_seam import fingerprint


DMA_NAMES = ("REFE", "CNT", "NEXT", "REF", "REFS", "CALL", "RET", "END")
VIF_NAMES = {0: "NOP", 1: "STCYCL", 2: "OFFSET", 3: "BASE", 4: "ITOP",
             5: "STMOD", 6: "MSKPATH3", 7: "MARK", 0x10: "FLUSHE",
             0x11: "FLUSH", 0x13: "FLUSHA", 0x14: "MSCAL", 0x15: "MSCALF",
             0x17: "MSCNT", 0x20: "STMASK", 0x30: "STROW", 0x31: "STCOL",
             0x4a: "MPG", 0x50: "DIRECT", 0x51: "DIRECTHL"}


class EESnapshot:
    def __init__(self, path):
        raw = path.read_bytes()
        if len(raw) < 160:
            raise ValueError("Truncated EE snapshot header")
        magic, version, ram_size, spr_size, self.caller, self.root, self.tag, self.pc = struct.unpack_from("<8I", raw)
        if (magic, version, ram_size, spr_size) != (0x3145534e, 1, 32 << 20, 16 << 10):
            raise ValueError("Unsupported EE snapshot format")
        if len(raw) != 160 + ram_size + spr_size:
            raise ValueError("Truncated or oversized EE snapshot")
        self.registers = struct.unpack_from("<32I", raw, 32)
        self.ram = memoryview(raw)[160:160 + ram_size]
        self.spr = memoryview(raw)[160 + ram_size:]

    def read(self, address, size, scratch=False):
        if scratch or 0x70000000 <= address < 0x70004000:
            offset, region = address & 0x3fff, self.spr
        else:
            if not (address < 0x40000000 or 0x80000000 <= address < 0xc0000000):
                raise ValueError(f"Not a RAM address: {address:08x}")
            offset, region = address & 0x1fffffff, self.ram
        if size < 0 or offset + size > len(region):
            raise ValueError(f"Out-of-bounds snapshot span: {address:08x}+{size}")
        return bytes(region[offset:offset + size])

    def dma_list(self):
        """Follow this called list through its outermost RET, retaining TTE.

        The caller's MSCAL/FLUSH tag is reported separately. Bytes after the
        caller's return address may not have been built at snapshot time.
        """
        address, stack, seen = self.root, [], set()
        fragments, tags, size = [], [], 0
        for _ in range(16384):
            identity = (address, tuple(stack))
            if identity in seen:
                raise ValueError("Cyclic DMA list")
            seen.add(identity)
            if address & 15:
                raise ValueError("Unaligned DMA tag")
            words = struct.unpack("<4I", self.read(address, 16))
            qwords, kind = words[0] & 0xffff, words[0] >> 28 & 7
            target = words[1] & 0x7fffffff
            if words[1] >> 31:
                target = 0x70000000 | (target & 0x3fff)
            payload = target if kind in (0, 3, 4) else address + 16
            amount = qwords * 16
            size += amount + 8
            if size > 16 << 20:
                raise ValueError("DMA list exceeds 16 MiB inspection bound")
            fragments.extend((self.read(address + 8, 8), self.read(payload, amount)))
            tags.append({"address": f"{address:08x}", "kind": DMA_NAMES[kind],
                         "qwords": qwords, "payload": f"{payload:08x}"})
            sequential = address + 16 + amount
            if kind in (0, 7):
                return tags, b"".join(fragments), DMA_NAMES[kind]
            if kind == 5:
                # The captured caller's CALL already occupies one ASR slot.
                if len(stack) == 1:
                    raise ValueError("DMA CALL stack exceeds the hardware depth")
                stack.append(sequential)
                address = target
            elif kind == 6:
                if not stack:
                    return tags, b"".join(fragments), "outer_RET"
                address = stack.pop()
            elif kind == 2:
                address = target
            elif kind in (3, 4):
                address += 16
            else:
                address = sequential
        raise ValueError("DMA tag limit exceeded")


def vif_commands(stream, initial_cycle):
    """Parse command boundaries only, with an explicit preceding STCYCL.

    This does not emulate unpack lane rules, masking, TOP/DBF or VU execution.
    """
    offset, cycle = 0, initial_cycle
    commands = []
    while offset < len(stream):
        if offset + 4 > len(stream):
            raise ValueError("Incomplete VIF command")
        word, = struct.unpack_from("<I", stream, offset)
        command, num, immediate = word >> 24 & 0x7f, word >> 16 & 255, word & 0xffff
        count, payload_bytes = num or 256, 0
        name = VIF_NAMES.get(command)
        if command & 0x60 == 0x60:
            name = "UNPACK"
            cl, wl = cycle & 255, cycle >> 8 & 255
            if not cl or not wl:
                raise ValueError("Zero STCYCL fields are outside this inspector's qualified subset")
            vectors = count if wl <= cl else count // wl * cl + min(count % wl, cl)
            lanes, width = (command >> 2 & 3) + 1, command & 3
            if width == 3:
                if lanes != 4:
                    raise ValueError("Invalid VIF V4-5 unpack opcode")
                payload_bytes = vectors * 2
            else:
                payload_bytes = vectors * lanes * (4 >> width)
            payload_bytes = (payload_bytes + 3) & ~3
        elif command == 1:
            cycle = immediate
        elif command == 0x20:
            payload_bytes = 4
        elif command in (0x30, 0x31):
            payload_bytes = 16
        elif command == 0x4a:
            payload_bytes = count * 8
        elif command in (0x50, 0x51):
            payload_bytes = (immediate or 65536) * 16
        elif name is None:
            raise ValueError(f"Unsupported VIF opcode {command:02x} at {offset}")
        end = offset + 4 + payload_bytes
        if end > len(stream):
            raise ValueError(f"Incomplete {name} payload at {offset}")
        commands.append({"offset": offset, "name": name, "opcode": f"{command:02x}",
                         "num": num, "immediate": immediate, "cycle": cycle,
                         "bytes": payload_bytes,
                         "payload_hash": fingerprint(stream[offset + 4:end]) if payload_bytes else None})
        offset = end
    return commands


def read_vu(path):
    raw = path.read_bytes()
    if len(raw) < 16:
        raise ValueError("Incomplete VU fixture")
    header = struct.unpack_from("<4I", raw)
    if header[3] != 664 or len(raw) != 16 + 32768 + header[3]:
        raise ValueError("Unsupported or incomplete VU fixture")
    return header, raw[16:16400], raw[16400:32784], raw[32784:]


def gif_packets(path):
    raw, offset, packets = path.read_bytes(), 0, []
    while offset < len(raw):
        if offset + 16 > len(raw):
            raise ValueError("Incomplete GIF packet header")
        source, size, drain, direct = struct.unpack_from("<4I", raw, offset)
        offset += 16
        if size == 0 or size & 15 or offset + size > len(raw) or source not in (1, 2, 3) or drain > 1 or direct > 1:
            raise ValueError("Invalid GIF packet capture")
        packets.append((source, raw[offset:offset + size]))
        offset += size
    return packets


def gif_summary(packets):
    if any(source != 1 for source, _ in packets):
        raise ValueError("Only ordered VU PATH1 output is qualified by this inspector")
    registers, primitive_types, formats = Counter(), Counter(), Counter()
    stream = b"".join(data for _, data in packets)
    offset = tags = 0
    while offset < len(stream):
        if offset + 16 > len(stream):
            break
        low, high = struct.unpack_from("<QQ", stream, offset)
        loops, form, nreg = low & 0x7fff, low >> 58 & 3, low >> 60 or 16
        size = loops * (nreg * (16 if form == 0 else 8) if form < 2 else 16)
        size = (size + 15) & ~15
        if offset + 16 + size > len(stream):
            break
        formats[form] += 1
        if low >> 46 & 1:
            primitive_types[low >> 47 & 7] += 1
        if form < 2:
            for index in range(nreg):
                registers[f"{high >> (index * 4) & 15:x}"] += loops
        tags += 1
        offset += 16 + size
    return {"packets": len(packets), "bytes": len(stream), "tags": tags,
            "formats": dict(formats), "primitive_types": dict(primitive_types),
            "register_writes": dict(registers), "unparsed_bytes": len(stream) - offset}


def inspect(prefix, trace_path, manifest=None):
    with trace_path.open(newline="", encoding="utf-8") as file:
        rows = list(csv.DictReader(file))
    if [int(r["sequence"]) for r in rows] != list(range(1, len(rows) + 1)):
        raise ValueError("Trace sequence gaps or duplicate rows")
    begins = {int(r["capture"]): r for r in rows if r["event"] == "vu_begin"}
    ends = {int(r["capture"]): r for r in rows if r["event"] == "vu_end"}
    if not begins or begins.keys() != ends.keys():
        raise ValueError("Missing VU capture begin/end records")
    programs = []
    if manifest:
        for item in json.loads(manifest.read_text())["programs"]:
            data = (manifest.parent / item["file"]).read_bytes()
            if hashlib.sha256(data).hexdigest() != item["sha256"]:
                raise ValueError("Disc program manifest mismatch")
            programs.append((item["name"], data))
    code_families, fixtures = {}, []
    for ticket, begin in sorted(begins.items()):
        base = f"{prefix}-vu-{ticket:06d}-"
        head, code, data, state = read_vu(Path(base + "pre.bin"))
        post_head, post_code, post_data, post_state = read_vu(Path(base + "post.bin"))
        end = ends[ticket]
        if head != post_head or code != post_code or int(begin["pc"], 16) != head[0] & 0x7fffffff:
            raise ValueError(f"Inconsistent VU fixture {ticket}")
        for row, payload in ((begin, data), (end, post_data)):
            if fingerprint(payload) != row["hash"] or fingerprint(code) != row["code_hash"]:
                raise ValueError(f"Capture/trace fingerprint mismatch {ticket}")
        code_sha = hashlib.sha256(code).hexdigest()
        if code_sha not in code_families:
            code_families[code_sha] = [{"name": name, "offset": code.find(program)}
                                      for name, program in programs if program and program in code]
        stages = []
        stage_count, = struct.unpack_from("<I", data, 4 * 16)
        if 0 < stage_count <= 48:
            for index in range(stage_count):
                target, constants = struct.unpack_from("<2I", data, (5 + index) * 16)
                stages.append({"pc": (target & 0xffff) * 8, "constants": constants & 0xffff})
        packets = gif_packets(Path(base + "gif.bin"))
        if len(packets) != int(end["destination"]):
            raise ValueError(f"GIF packet count mismatch {ticket}")
        cycles = struct.unpack_from("<Q", post_state, 624)[0] - struct.unpack_from("<Q", state, 624)[0]
        fixtures.append({"ticket": ticket, "present_vs": int(begin["present_vs"]),
                         "pc": head[0] & 0x7fffffff, "continuing": bool(head[0] >> 31),
                         "top": head[1], "itop": head[2], "code_sha256": code_sha,
                         "candidate_manager_stages": stages,
                         "qword53_context_hash": fingerprint(data[53 * 16:109 * 16]),
                         "cycles": cycles, "needs_transfer": bool(int(end["cycle"])),
                         "fell_back": bool(int(end["mode"])),
                         "changed_data_qwords": sum(data[i:i + 16] != post_data[i:i + 16] for i in range(0, 16384, 16)),
                         "gif": gif_summary(packets)})
    snapshot = EESnapshot(Path(f"{prefix}-ee.bin"))
    mesh_rows = [r for r in rows if r["event"] == "mesh_call" and r["capture"] == "1"]
    if len(mesh_rows) != 1 or int(mesh_rows[0]["address"], 16) != snapshot.root:
        raise ValueError("Missing or mismatched EE snapshot trace record")
    mesh = mesh_rows[0]
    if fingerprint(snapshot.read(snapshot.root, 64)) != mesh["hash"]:
        raise ValueError("Mesh list snapshot/trace mismatch")
    tags, stream, terminal = snapshot.dma_list()
    # The captured grass writer emitted STCYCL(4,4) immediately before CALL.
    # Verify that trace before using its cycle as this list's initial state.
    constants = [r for r in rows if r["event"] == "constants" and r["hash"] == mesh["context_hash"]
                 and r["ee_vs"] == mesh["ee_vs"] and int(r["sequence"]) < int(mesh["sequence"])]
    if not constants:
        raise ValueError("No preceding captured grass context for this list")
    constant = constants[-1]
    block = int(constant["address"], 16)
    if fingerprint(snapshot.read(block, 56 * 16)) != mesh["context_hash"]:
        raise ValueError("Grass constants snapshot/trace mismatch")
    tag = struct.unpack("<4I", snapshot.read(block - 16, 16))
    expected_unpack = 0x6c380000 | int(constant["destination"])
    if tag[0] != 0x10000038 or tag[2:] != (0x01000404, expected_unpack):
        raise ValueError("Preceding grass upload is not the expected CNT/STCYCL/UNPACK")
    caller_tag = struct.unpack("<4I", snapshot.read(snapshot.tag, 16))
    if caller_tag != (0x50000000, snapshot.root, 0x14000000, 0x11000000):
        raise ValueError("Mesh call tag is not CALL/MSCAL(0)/FLUSH")
    commands = vif_commands(stream, tag[2] & 0xffff)
    return {"mesh": {"root": f"{snapshot.root:08x}", "caller": f"{snapshot.caller:08x}",
                     "context_hash": mesh["context_hash"], "dma_tags": tags,
                     "terminal": terminal, "vif_bytes": len(stream),
                     "stream_sha256": hashlib.sha256(stream).hexdigest(),
                     "commands": commands},
            "code_families": code_families, "fixtures": fixtures,
            "limitations": ["EE snapshot is not globally atomic across all devices.",
                            "Presenter labels and repeated payloads do not establish unique batch ownership.",
                            "VIF parsing identifies boundaries; it does not emulate data layout or persistent state.",
                            "Captured GIF is VU output only; surrounding GS state and other paths are not replayed.",
                            "Transfer/fallback fixtures are not complete replacement oracles."]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--program-manifest", type=Path)
    args = parser.parse_args()
    print(json.dumps(inspect(args.prefix, args.trace, args.program_manifest), indent=2))
