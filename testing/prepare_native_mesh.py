# SPDX-License-Identifier: GPL-3.0-or-later
"""Import one complete, authored non-grass mesh and its named texture, once.

Private USA disc input only. The deliberately narrow, hash-qualified fixture is
ROCK_J3 (all vertices/elements/strips), not a guessed selection of VU packets.
The native renderer itself knows nothing about VBIN, ITF, guest RAM or the GS.
Format research: https://github.com/bbatts15/TF2004-File-Editor (Mesh.cpp,
VBINtoSTL.cpp, BinChanger.cpp). This importer is independently implemented and
also handles the raw RGB24 ITF used by this asset.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib

ASSET_DIR = "LEVELS/EPISODES/01_AMAZON_1/ECOSYSTEM/ROCKS/"
QUALIFIED = {
    "ROCK_J3.VBIN": "b0a3f3dad0adacb21a0af2f3134d6a78ba4ea109fc3236534f807a96b3323e61",
    "JUNGLE_CLIFF.ITF": "62198b5fbdcb62213871df70dd4b8a67aa4de66fc929f5e77752d48b82448fdb",
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def private_output(path):
    path = Path(path).resolve()
    source = Path(__file__).resolve().parents[1]
    if path.is_relative_to(source) or path.exists():
        raise ValueError("Use a fresh output directory outside the source checkout")
    return path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def extract(iso_path, map_path):
    extent = None
    for line in Path(map_path).read_text().splitlines():
        sector, count, name = line.split("\t")
        if name == "/TFA.ZIP":
            require(extent is None, "Ambiguous TFA.ZIP extent")
            extent = (int(sector) * 2048, int(count) * 2048)
    require(extent is not None and 0 < extent[1] < 256 * 1024 * 1024, "Invalid TFA.ZIP extent")
    with Path(iso_path).open("rb") as iso:
        iso.seek(extent[0])
        archive = iso.read(extent[1])
    require(len(archive) == extent[1], "Truncated disc extent")
    # This game's ZIP places the central directory before the local entries.
    # Offsets are archive-relative; no filesystem paths from it are extracted.
    records, at = {}, 0
    while archive[at:at + 4] == b"PK\x01\x02":
        require(at + 46 <= len(archive), "Truncated ZIP directory")
        flags, method = struct.unpack_from("<HH", archive, at + 8)
        crc, compressed, size = struct.unpack_from("<III", archive, at + 16)
        n, extra, comment = struct.unpack_from("<HHH", archive, at + 28)
        local = struct.unpack_from("<I", archive, at + 42)[0]
        end = at + 46 + n + extra + comment
        require(end <= len(archive), "ZIP directory entry outside extent")
        name = archive[at + 46:at + 46 + n].decode("ascii")
        require(name not in records, "Duplicate ZIP entry")
        records[name] = (flags, method, crc, compressed, size, local)
        at = end
    require(archive[at:at + 4] == b"PK\x05\x06", "Unexpected ZIP directory termination")
    result = {}
    for basename, expected_hash in QUALIFIED.items():
        name = ASSET_DIR + basename
        flags, method, crc, compressed, size, local = records[name]
        require(not (flags & 1) and method == 8 and size < 1024 * 1024, "Unsupported asset compression")
        require(0 <= local <= len(archive) - 30 and archive[local:local + 4] == b"PK\x03\x04", "Invalid ZIP local entry")
        n, extra = struct.unpack_from("<HH", archive, local + 26)
        require(archive[local + 30:local + 30 + n].decode("ascii") == name, "ZIP filename mismatch")
        start = local + 30 + n + extra
        require(start + compressed <= len(archive), "ZIP payload outside extent")
        decoder = zlib.decompressobj(-15)
        data = decoder.decompress(archive[start:start + compressed], size + 1)
        require(decoder.eof and not decoder.unused_data and len(data) == size and zlib.crc32(data) == crc,
                "Corrupt compressed asset")
        require(digest(data) == expected_hash, "Asset differs from the qualified USA fixture")
        result[basename] = data
    return result


def section(data, name, start=0, end=None):
    end = len(data) if end is None else end
    marker = name.encode("ascii") + b"\0\0"
    at = data.find(marker, start, end)
    require(at >= start and data.find(marker, at + 1, end) == -1, "Missing or duplicate " + name)
    length_at = at + len(marker)
    require(length_at + 4 <= end, "Truncated " + name)
    size = struct.unpack_from("<I", data, length_at)[0]
    require(size >= 4 and length_at + size <= end, "Section outside parent: " + name)
    return length_at + 4, length_at + size


def import_mesh(data):
    require(digest(data) == QUALIFIED["ROCK_J3.VBIN"], "Only the qualified rigid mesh is supported")
    require(data.startswith(b"FISH~SceneNode\0\1"), "Unknown VBIN header")
    marker = b"~Mesh\0\1"
    require(data.count(marker) == 1, "Expected a complete single-mesh asset")
    at = data.index(marker) + len(marker)
    name_length = struct.unpack_from("<I", data, at)[0]
    name = data[at + 4:at + 4 + name_length].decode("ascii").rstrip("\0")
    size_at = at + 4 + name_length
    size, elements = struct.unpack_from("<II", data, size_at)
    require(size_at + size == len(data) and elements == 1, "Unsupported mesh container or element count")
    va, ve = section(data, "~VertexSet", size_at, len(data))
    vertices, retained_property, channels = struct.unpack_from("<III", data, va)

    def values(label, components):
        begin, end = section(data, label, va, ve)
        require(end - begin == vertices * components * 4, label + " vertex count mismatch")
        result = list(struct.iter_unpack("<" + "f" * components, data[begin:end]))
        require(all(math.isfinite(x) for v in result for x in v), "Nonfinite vertex attribute")
        return result

    positions = values("~PositionArray", 3)
    colors = values("~ColorArray", 4)
    uv = values("~TextureCoords", 2)
    require(all(v[3] == 1 for v in colors), "Prototype only supports this opaque material")
    ea, ee = section(data, "~ElementArray", ve, len(data))
    element_begin, element_end = section(data, "~Element0", ea, ee)
    require(element_end == ee, "Unconsumed mesh elements")
    ia, ie = section(data, "~IndexArray", element_begin, element_end)
    strips, indices, degenerate = [], [], 0
    while ia < ie:
        require(ia + 2 <= ie, "Truncated strip length")
        length = struct.unpack_from("<H", data, ia)[0]
        ia += 2
        require(length >= 3 and ia + length * 2 <= ie, "Invalid strip")
        strip = struct.unpack_from("<" + "H" * length, data, ia)
        require(max(strip) < vertices, "Index outside mesh")
        ia += length * 2
        strips.append(list(strip))
        for i in range(2, length):
            triangle = (strip[i - 2], strip[i - 1], strip[i])
            if i & 1:
                triangle = (triangle[1], triangle[0], triangle[2])
            if len(set(triangle)) != 3:
                degenerate += 1
            else:
                indices.extend(triangle)
    sa, se = section(data, "~SurfaceProperties", element_begin, element_end)
    flags, n = struct.unpack_from("<II", data, sa)
    binding = data[sa + 8:sa + 8 + n].decode("ascii").rstrip("\0")
    require(flags == 3 and binding == "Jungle_Cliff", "Unsupported material binding")
    ma, me = section(data, "~Material", sa, se)
    require(me - ma == 52, "Unsupported material block")
    constants = struct.unpack_from("<13f", data, ma)
    require(constants[:7] == (1.0,) * 7, "Unsupported material color")
    # The qualified file's only node/mesh transforms are identity. Do not apply
    # heuristic scene-node transformations to arbitrary VBINs here.
    require(vertices == 87 and channels == 1 and indices, "Unexpected qualified mesh layout")
    native_vertices = b"".join(struct.pack("<9f", *p, *t, *c) for p, t, c in zip(positions, uv, colors))
    native_indices = struct.pack("<" + "I" * len(indices), *indices)
    return native_vertices, native_indices, {
        "mesh_name": name, "vertices": vertices, "elements": elements,
        "triangle_strips": strips, "triangles": len(indices) // 3,
        "discarded_repeated_index_triangles": degenerate,
        "binding": binding, "material_constants": constants,
        "retained_vertex_property": retained_property,
        "bounds": {"min": list(map(min, zip(*positions))), "max": list(map(max, zip(*positions)))},
        "scope": "Complete authored rigid mesh; opaque vertex color times albedo; no live game context/lighting claim",
    }


def import_texture(data):
    require(digest(data) == QUALIFIED["JUNGLE_CLIFF.ITF"], "Only the qualified raw RGB24 texture is supported")
    require(data[:4] == b"FORM" and data[8:15] == b"ITF0HDR" and data[20:23] == b"PS2", "Unknown ITF header")
    require(struct.unpack_from("<I", data, 4)[0] == len(data) - 8 and data[23] == 0x0c,
            "Unsupported ITF storage")
    width, height = struct.unpack_from("<II", data, 28)
    require(data[52:56] == b"TXTR" and struct.unpack_from("<I", data, 56)[0] == width * height * 3
            and len(data) == 60 + width * height * 3, "Raw RGB24 texture size mismatch")
    # Already linear RGB24 on disc; no palette, GS VRAM or swizzle reconstruction.
    pixels = b"".join(data[i:i + 3] + b"\xff" for i in range(60, len(data), 3))
    return width, height, pixels


def prepare(iso, isomap, output):
    output = private_output(output)
    assets = extract(iso, isomap)
    vertices, indices, meta = import_mesh(assets["ROCK_J3.VBIN"])
    width, height, pixels = import_texture(assets["JUNGLE_CLIFF.ITF"])
    payload = struct.pack("<8s6I", b"TFNMESH1", len(vertices) // 36, len(indices) // 4, width, height, 0, 0) + vertices + indices + pixels
    meta.update(version=1, source_archive="/TFA.ZIP", source_directory=ASSET_DIR,
                source_sha256=QUALIFIED, texture_size=[width, height], asset_sha256=digest(payload))
    output.mkdir(parents=True)
    (output / "asset.bin").write_bytes(payload)
    (output / "manifest.json").write_text(json.dumps(meta, indent=2) + "\n")
    print(json.dumps({"vertices": meta["vertices"], "triangles": meta["triangles"],
                      "strips": len(meta["triangle_strips"]), "texture": meta["binding"], "output": str(output)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", required=True, type=Path)
    parser.add_argument("--isomap", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    prepare(args.iso, args.isomap, args.output)
