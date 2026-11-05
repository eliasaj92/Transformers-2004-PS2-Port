#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Instrument private accelerated region calls without publishing game code."""
import argparse
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    for path in (args.package, args.output):
        if path.resolve() == root or root in path.resolve().parents:
            raise ValueError("Keep generated game code outside the source checkout")
    found = []
    # This experiment targets the supported package's ordinary grass program.
    # Another shader has the same control-flow shape. The runtime probe also
    # authenticates all 3872 bytes of the resident program, at its actual base.
    for path in (args.package / "generated").glob("chain_038.cpp"):
        source = path.read_text()
        regions = re.findall(r"ps2Vu1StaticSpan\(v.m_state.pc,(\d+)u,(\d+)u,(\d+)u\)\|\|!(sr\d+)\(", source)
        mapping = {(int(rel), int(pairs)): name for _, rel, pairs, name in regions}
        if all(key in mapping for key in ((262, 55), (317, 59), (376, 22), (398, 5))):
            found.append((path, source, mapping[(262, 55)], mapping[(317, 59)], mapping[(376, 22)]))
    if len(found) != 1:
        raise ValueError("Private package lacks the qualified grass emission boundaries")
    path, source, prologue, loop, epilogue = found[0]
    for original, replacement in ((prologue, "grassPrologueProbe"), (loop, "grassTailProbe"),
                                  (epilogue, "grassEpilogueProbe")):
        needle = f"!{original}(v,data,size,end,ended)"
        if source.count(needle) != 1:
            raise ValueError("Ambiguous grass chain")
        source = source.replace(needle, f"!{replacement}(v,data,size,end,ended)")
        source = f"bool {replacement}(class StaticVU&,unsigned char*,unsigned int,unsigned long long,bool&);\n" + source
    needle = f"if(v.m_state.pc==((base+2536u)&0x3fffu))goto r{loop[2:]};"
    if source.count(needle) != 2:
        raise ValueError("Grass prologue continuation changed")
    source = source.replace(needle, needle + f"\nif(v.m_state.pc==((base+3008u)&0x3fffu))goto r{epilogue[2:]};", 1)
    args.output.mkdir(parents=True, exist_ok=True)
    def write_changed(path, text):
        if not path.exists() or path.read_text() != text:
            path.write_text(text)

    write_changed(args.output / "grass_chain.cpp", source)
    write_changed(args.output / "grass_chain.cmake",
        f'set(GRASS_ORIGINAL_CHAIN "{path.resolve().as_posix()}")\n'
        f'set(GRASS_PROLOGUE "{prologue}")\nset(GRASS_LOOP "{loop}")\nset(GRASS_EPILOGUE "{epilogue}")\n')


if __name__ == "__main__":
    main()
