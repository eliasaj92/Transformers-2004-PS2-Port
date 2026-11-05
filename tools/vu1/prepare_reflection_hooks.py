#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Wrap authenticated private reflection chains with shared runtime offload hooks."""
import argparse
from pathlib import Path
import re


def prepare(package, output):
    root = Path(__file__).resolve().parents[2]
    for path in (package, output):
        if path.resolve() == root or root in path.resolve().parents:
            raise ValueError('Keep generated game code outside the source checkout')
    shapes = {
        40: [('0', '64'), ('64', '48'), ('112', '41'), ('153', '3')],
        46: [('0', '58'), ('58', '3'), ('59', '2'), ('61', '23'), ('84', '26'),
             ('110', '9'), ('119', '4'), ('124', '4'), ('129', '36'), ('165', '36'),
             ('201', '10'), ('211', '4')],
    }
    output.mkdir(parents=True, exist_ok=True)
    originals = []
    for chain, hook in ((40, 'lighting'), (46, 'projection')):
        path = package / f'generated/chain_{chain:03}.cpp'
        originals.append(path.resolve().as_posix())
        source = path.read_text()
        regions = re.findall(rf'ps2Vu1StaticSpan\(v.m_state.pc,{chain}u,(\d+)u,(\d+)u\)', source)
        if regions != shapes[chain] or source.count(f'bool sc{chain}(') != 1:
            raise ValueError('Private package lacks the qualified reflection boundary')
        body = f'reflection_{hook}_body'
        source = source.replace(f'bool sc{chain}(', f'bool {body}(')
        if chain == 46:
            for original, replacement in (('sr798', 'projectionFirst'), ('sr801', 'projectionEnd')):
                needle = f'!{original}(v,data,size,end,ended)'
                if source.count(needle) != 1:
                    raise ValueError('Ambiguous reflection projection boundary')
                source = source.replace(needle, f'!tf_gpu_reflection::{replacement}(v,data,size,end,ended,{original})')
        source = '#include "runtime/gs/gs_native_reflection_vu.h"\n' + source
        source += (f'\nbool sc{chain}(StaticVU& v,uint8_t* data,uint32_t size,uint64_t end,bool& ended,unsigned relative){{\n'
                   f' return tf_gpu_reflection::{hook}(v,data,size,end,ended,relative,{body});\n}}\n')
        target = output / f'reflection_{hook}.cpp'
        if not target.exists() or target.read_text() != source:
            target.write_text(source)
    cmake = 'set(REFLECTION_ORIGINAL_CHAINS ' + ' '.join(f'"{p}"' for p in originals) + ')\n'
    target = output / 'reflection_chains.cmake'
    if not target.exists() or target.read_text() != cmake:
        target.write_text(cmake)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    prepare(args.package, args.output)
