#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Wrap the qualified private grass chain with the shared runtime offload hooks."""
import argparse
from pathlib import Path
import re


def prepare(package, output):
    root = Path(__file__).resolve().parents[2]
    for path in (package, output):
        if path.resolve() == root or root in path.resolve().parents:
            raise ValueError("Keep generated game code outside the source checkout")
    original = package / "generated/chain_038.cpp"
    source = original.read_text()
    entries = re.findall(r"ps2Vu1StaticSpan\(v.m_state.pc,(\d+)u,(\d+)u,(\d+)u\)\|\|!(sr\d+)\(", source)
    regions = {(int(rel), int(pairs)): name for _, rel, pairs, name in entries}
    if not all(key in regions for key in ((317, 59), (376, 22), (398, 5))):
        raise ValueError("Private package lacks the qualified grass loop/epilogue/kick")
    for key, hook in (((317, 59), "loop"), ((376, 22), "epilogue")):
        name = regions[key]
        needle = f"!{name}(v,data,size,end,ended)"
        if source.count(needle) != 1:
            raise ValueError("Ambiguous grass chain hook")
        source = source.replace(needle, f"!tf_gpu_grass::{hook}(v,data,size,end,ended,{name})")
    source = '#include "runtime/gs/gs_native_grass_vu.h"\n' + source
    output.mkdir(parents=True, exist_ok=True)
    for path, value in ((output / "grass_chain.cpp", source),
                        (output / "grass_chain.cmake", f'set(GRASS_ORIGINAL_CHAIN "{original.resolve().as_posix()}")\n')):
        if not path.exists() or path.read_text() != value:
            path.write_text(value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    prepare(args.package, args.output)


if __name__ == "__main__":
    main()
