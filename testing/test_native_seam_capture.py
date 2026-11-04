#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic parser boundaries; contains no captured game bytes."""
import struct
import tempfile
from pathlib import Path
import unittest

from inspect_native_seam_capture import EESnapshot, gif_packets, gif_summary, vif_commands
from import_grass_seam import import_list
from compare_native_seam_captures import canonical


class CaptureParserTests(unittest.TestCase):
    def snapshot(self):
        snapshot = EESnapshot.__new__(EESnapshot)
        snapshot.ram = bytearray(4096)
        snapshot.spr = bytearray(16384)
        snapshot.root = 0x20000100
        return snapshot

    def test_ref_then_nested_call_returns_to_outer_list(self):
        snapshot = self.snapshot()
        # REF data lives elsewhere; CALL's inline payload precedes its target.
        struct.pack_into("<4I", snapshot.ram, 0x100, 0x30000001, 0x300, 0, 0)
        struct.pack_into("<4I", snapshot.ram, 0x110, 0x50000001, 0x200, 0, 0)
        struct.pack_into("<4I", snapshot.ram, 0x130, 0x60000000, 0, 0, 0)
        struct.pack_into("<4I", snapshot.ram, 0x200, 0x60000000, 0, 0, 0)
        snapshot.ram[0x120:0x130] = b"i" * 16
        snapshot.ram[0x300:0x310] = b"r" * 16
        tags, stream, terminal = snapshot.dma_list()
        self.assertEqual([t["kind"] for t in tags], ["REF", "CALL", "RET", "RET"])
        self.assertEqual(terminal, "outer_RET")
        self.assertEqual(stream, bytes(8) + b"r" * 16 + bytes(8) + b"i" * 16 + bytes(16))

    def test_dma_cycle_and_invalid_reference_fail(self):
        snapshot = self.snapshot()
        struct.pack_into("<4I", snapshot.ram, 0x100, 0x20000000, 0x100, 0, 0)
        with self.assertRaisesRegex(ValueError, "Cyclic"):
            snapshot.dma_list()
        struct.pack_into("<4I", snapshot.ram, 0x100, 0x00000001, 0x10000000, 0, 0)
        with self.assertRaisesRegex(ValueError, "Out-of-bounds"):
            snapshot.dma_list()

    def test_external_call_counts_towards_dma_stack_depth(self):
        snapshot = self.snapshot()
        struct.pack_into("<4I", snapshot.ram, 0x100, 0x50000000, 0x200, 0, 0)
        struct.pack_into("<4I", snapshot.ram, 0x200, 0x50000000, 0x300, 0, 0)
        with self.assertRaisesRegex(ValueError, "hardware depth"):
            snapshot.dma_list()

    def test_vif_fill_cycle_consumes_only_input_vectors(self):
        stream = struct.pack("<2I", 0x01000402, 0x6c060035) + bytes(64) + struct.pack("<I", 0x17000000)
        commands = vif_commands(stream, 0x0404)
        self.assertEqual([r["name"] for r in commands], ["STCYCL", "UNPACK", "MSCNT"])
        self.assertEqual(commands[1]["bytes"], 64)
        self.assertEqual(commands[2]["offset"], 72)

    def test_vif_zero_num_is_256_and_truncation_fails(self):
        stream = struct.pack("<I", 0x6c000000) + bytes(4096)
        self.assertEqual(vif_commands(stream, 0x0404)[0]["bytes"], 4096)
        with self.assertRaisesRegex(ValueError, "Incomplete UNPACK"):
            vif_commands(stream[:-4], 0x0404)
        with self.assertRaisesRegex(ValueError, "Unsupported VIF"):
            vif_commands(struct.pack("<I", 0x12000000), 0x0404)

    def test_gif_packet_boundary_can_split_tag_payload(self):
        tag = struct.pack("<QQ", 1 | (1 << 15) | (1 << 60), 5)
        payload = bytes(16)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "synthetic.bin"
            path.write_bytes(struct.pack("<4I", 1, 16, 1, 0) + tag +
                             struct.pack("<4I", 1, 16, 1, 0) + payload)
            report = gif_summary(gif_packets(path))
            self.assertEqual(report["tags"], 1)
            self.assertEqual(report["register_writes"], {"5": 1})
            self.assertEqual(report["unparsed_bytes"], 0)
            path.write_bytes(path.read_bytes()[:-1])
            with self.assertRaisesRegex(ValueError, "Invalid GIF"):
                gif_packets(path)

    def test_grass_import_preserves_position_bits_and_rejects_relative_unpack(self):
        class SyntheticList:
            stream = struct.pack("<12I", 0x01000103, 0x040000b4, 0x680100b4, 2, 3, 0,
                                 0x680200b7, 1, 2, 3, 4, 5) + struct.pack("<2I", 6, 0x17000000)

            def dma_list(self):
                return [{}], self.stream, "outer_RET"
        source = SyntheticList()
        chunks, _ = import_list(source)
        self.assertEqual(chunks[0]["positions"], [(1, 2, 3), (4, 5, 6)])
        self.assertEqual(chunks[0]["count"], 2)
        stream = bytearray(source.stream)
        struct.pack_into("<I", stream, 24, 0x680280b7)
        source.stream = stream
        with self.assertRaisesRegex(ValueError, "absolute destinations"):
            import_list(source)

    def test_state_comparison_ignores_only_abi_padding(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "synthetic.bin"
            raw = bytearray(struct.pack("<4I", 0, 0, 0, 664) + bytes(32768 + 664))
            path.write_bytes(raw)
            baseline = canonical(path)
            for offset in (638, 639, 649, 650, 651, 660, 661, 662, 663):
                raw[32784 + offset] = 255
            path.write_bytes(raw)
            self.assertEqual(canonical(path), baseline)
            raw[32784 + 624] = 1  # Absolute VU cycle counter remains observable.
            path.write_bytes(raw)
            self.assertNotEqual(canonical(path), baseline)


if __name__ == "__main__":
    unittest.main()
