#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualification failure gates, using synthetic packets only."""
from pathlib import Path
import struct
import tempfile
import unittest

from qualify_grass_geometry import compare, geometry_from_gif


def packet():
    data = bytearray(1216)
    for offset, loops, registers, count in ((0, 2, 0xe, 1), (3, 1, 2, 1), (5, 8, 0x4124, 4),
                                           (38, 2, 0xe, 1), (41, 1, 2, 1), (43, 8, 0x4124, 4)):
        struct.pack_into("<2Q", data, offset*16, loops | count << 60, registers)
    return data


def record(data, source=1):
    return struct.pack("<4I", source, len(data), 1, 0) + data


class GeometryQualificationTests(unittest.TestCase):
    def test_layout_path_and_field_color_fail_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "synthetic-gif.bin"
            valid = packet()
            path.write_bytes(record(valid))
            self.assertEqual(geometry_from_gif(path), (bytes(640), 0))
            path.write_bytes(record(valid, source=2))
            with self.assertRaisesRegex(ValueError, "path"):
                geometry_from_gif(path)
            changed = bytearray(valid)
            changed[5*16] = 7  # Wrong vertex count in one field.
            path.write_bytes(record(changed))
            with self.assertRaisesRegex(ValueError, "tag"):
                geometry_from_gif(path)
            changed = bytearray(valid)
            changed[46*16] = 1  # A color differs between the two fields.
            path.write_bytes(record(changed))
            with self.assertRaisesRegex(ValueError, "field-dependent"):
                geometry_from_gif(path)

    def test_mutated_attribute_and_missing_group_fail_comparison(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            pre = root / "synthetic-pre.bin"
            (root / "synthetic-gif.bin").write_bytes(record(packet()))
            native_gif = root / "synthetic-pre-native-gif.bin"
            native_gif.write_bytes(record(packet()))
            native = root / "synthetic-pre-native.bin"
            header = struct.pack("<8I", 0x53415247, 1, 1, 1, 0, 0, 0, 640)
            native.write_bytes(header + bytes(640))
            self.assertTrue(compare([pre], root)["pass"])
            # Geometry still matches, but a GS state write or transport flag differs.
            altered = packet()
            altered[16] = 1
            native_gif.write_bytes(record(altered))
            self.assertFalse(compare([pre], root)["pass"])
            native_gif.write_bytes(record(packet(), source=2))
            self.assertFalse(compare([pre], root)["pass"])
            native_gif.write_bytes(record(packet()))
            changed = bytearray(640)
            changed[511] = 1  # Last clip word of the second field.
            native.write_bytes(header + changed)
            result = compare([pre], root)
            self.assertFalse(result["pass"])
            self.assertEqual(result["results"][0]["first_mismatched_word"], 127)
            native.write_bytes(struct.pack("<8I", 0x53415247, 1, 0, 1, 0, 0, 0, 640))
            self.assertFalse(compare([pre], root)["pass"])
            native.write_bytes(header + bytes(639))
            with self.assertRaisesRegex(ValueError, "output"):
                compare([pre], root)

    def test_empty_packet_does_not_count_as_geometry(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "synthetic-gif.bin"
            path.write_bytes(record(bytes(16)) + record(packet()))
            self.assertEqual(geometry_from_gif(path), (bytes(640), 1))
            path.write_bytes(record(bytes(32)))
            with self.assertRaisesRegex(ValueError, "size"):
                geometry_from_gif(path)


if __name__ == "__main__":
    unittest.main()
