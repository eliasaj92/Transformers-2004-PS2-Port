#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Wrap the authenticated private reflection chain for accelerated capture."""
import argparse
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    for path in (args.package, args.output):
        if path.resolve() == root or root in path.resolve().parents:
            raise ValueError('Keep generated game code outside the source checkout')
    path = args.package / 'generated/chain_040.cpp'
    source = path.read_text()
    regions = re.findall(r'ps2Vu1StaticSpan\(v.m_state.pc,40u,(\d+)u,(\d+)u\)', source)
    if regions != [('0', '64'), ('64', '48'), ('112', '41'), ('153', '3')] or source.count('bool sc40(') != 1:
        raise ValueError('Private package lacks the qualified reflection boundary')
    # Runtime capture additionally authenticates every resident program byte.
    source = source.replace('bool sc40(', 'bool reflectionOriginal(')
    args.output.mkdir(parents=True, exist_ok=True)
    for name, text in (
        ('reflection_chain.cpp', source),
        ('reflection_chain.cmake', f'set(REFLECTION_ORIGINAL_CHAIN "{path.resolve().as_posix()}")\n'),
    ):
        target = args.output / name
        if not target.exists() or target.read_text() != text:
            target.write_text(text)


if __name__ == '__main__':
    main()
