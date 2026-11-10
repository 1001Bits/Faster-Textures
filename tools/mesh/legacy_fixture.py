"""Strict, private SE mesh fixture converter; never edits an archive or input.

Source schema: NifSkope nif.xml (2018-02-22), NiAVObject/NiGeometry,
NiTriShapeData, BSTriShape and BSMeshLODTriShape; CommonLib VertexDesc.h.
Only unskinned, uncontrolled SE 20.2.0.7 / user12 / Bethesda100 input is
accepted. Other blocks and block references are preserved byte-for-byte.
The output requires an independently verified BSMeshLODTriShape loader.
No engine registration, deployment or game launch is performed here.
"""
from __future__ import annotations
import argparse
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import struct

PRIVATE_BLOCK = b"DTLPrivateMeshLODTriShape"

class Reader:
    def __init__(self, data: bytes):
        self.data, self.pos = data, 0

    def take(self, count: int) -> bytes:
        if count < 0 or count > len(self.data) - self.pos:
            raise ValueError("truncated or oversized NIF field")
        result = self.data[self.pos:self.pos + count]
        self.pos += count
        return result

    def unpack(self, fmt: str):
        result = struct.unpack("<" + fmt, self.take(struct.calcsize("<" + fmt)))
        return result[0] if len(result) == 1 else result

    def string(self) -> bytes:
        return self.take(self.unpack("I"))

    def done(self):
        if self.pos != len(self.data):
            raise ValueError("unsupported trailing block data")


@dataclass
class Nif:
    prefix: bytes
    types: list[bytes]
    ids: list[int]
    middle: bytes
    blocks: list[bytes]
    footer: bytes

    @classmethod
    def parse(cls, data: bytes) -> Nif:
        r = Reader(data)
        line_end = data.find(b"\n", 0, 100)
        if line_end < 0:
            raise ValueError("missing NIF signature")
        r.take(line_end + 1)
        version, endian, user, count, bethesda = r.unpack("IBIII")
        if (version, endian, user, bethesda) != (0x14020007, 1, 12, 100):
            raise ValueError("only little-endian Skyrim SE Bethesda100 is supported")
        if count > 100000:
            raise ValueError("too many blocks")
        for _ in range(3):
            r.take(r.unpack("B"))
        prefix = data[:r.pos]
        types = [r.string() for _ in range(r.unpack("H"))]
        ids = [r.unpack("H") for _ in range(count)]
        if any(i >= len(types) for i in ids):
            raise ValueError("invalid block type index")
        sizes = [r.unpack("I") for _ in range(count)]
        middle_start = r.pos
        strings, _maximum = r.unpack("II")
        for _ in range(strings):
            r.string()
        groups = r.unpack("I")
        r.take(groups * 4)
        middle = data[middle_start:r.pos]
        blocks = [r.take(n) for n in sizes]
        footer = data[r.pos:]
        footer_reader = Reader(footer)
        roots = footer_reader.unpack("I")
        for _ in range(roots):
            root = footer_reader.unpack("i")
            if root < -1 or root >= count:
                raise ValueError("invalid root reference")
        footer_reader.done()
        return cls(prefix, types, ids, middle, blocks, footer)

    def serialize(self) -> bytes:
        parts = [self.prefix, struct.pack("<H", len(self.types))]
        for name in self.types:
            parts += [struct.pack("<I", len(name)), name]
        parts += [struct.pack("<" + "H" * len(self.ids), *self.ids),
                  struct.pack("<" + "I" * len(self.blocks), *(len(b) for b in self.blocks)),
                  self.middle, *self.blocks, self.footer]
        return b"".join(parts)


def av_prefix(r: Reader) -> bytes:
    r.unpack("i")  # string table name
    extras = r.unpack("I")
    if extras:
        raise ValueError("fixture conversion excludes extra-data-dependent geometry")
    if r.unpack("i") != -1:
        raise ValueError("fixture conversion excludes controlled geometry")
    r.unpack("I")  # flags; copied unchanged
    transform = r.unpack("13f")
    if not all(math.isfinite(x) for x in transform):
        raise ValueError("non-finite object transform")
    if r.unpack("i") != -1:
        raise ValueError("fixture conversion excludes shape-owned collision")
    return r.data[:r.pos]


def finite_vectors(r: Reader, n: int, width: int) -> list[tuple[float, ...]]:
    result = [r.unpack("f" * width) for _ in range(n)]
    if not all(math.isfinite(x) for row in result for x in row):
        raise ValueError("non-finite vertex attribute")
    return result


def legacy_data(data: bytes) -> dict:
    r = Reader(data)
    group, count, keep, compress, has_vertices = r.unpack("iHBBB")
    if group != 0 or keep or compress or has_vertices != 1 or count == 0:
        raise ValueError("unsupported legacy vertex storage flags")
    vertices = finite_vectors(r, count, 3)
    vector_flags, material_crc, has_normals = r.unpack("HIB")
    if vector_flags & ~0x1001 or has_normals not in (0, 1):
        raise ValueError("unsupported legacy vector flags")
    normals = finite_vectors(r, count, 3) if has_normals else None
    tangents = finite_vectors(r, count, 3) if has_normals and vector_flags & 0x1000 else None
    bitangents = finite_vectors(r, count, 3) if tangents else None
    if vector_flags & 0x1000 and not has_normals:
        raise ValueError("tangents without normals")
    bound = r.take(16)
    if not all(math.isfinite(x) for x in struct.unpack("<4f", bound)) or struct.unpack("<4f", bound)[3] < 0:
        raise ValueError("invalid bound")
    has_colors = r.unpack("B")
    if has_colors not in (0, 1):
        raise ValueError("unsupported legacy color encoding")
    colors = finite_vectors(r, count, 4) if has_colors else None
    uv = finite_vectors(r, count, 2) if vector_flags & 1 else None
    consistency, additional = r.unpack("Hi")
    if additional != -1:
        raise ValueError("additional geometry data is unsupported")
    triangles, points, has_triangles = r.unpack("HIB")
    if has_triangles != 1 or triangles == 0 or points != triangles * 3:
        raise ValueError("invalid triangle list")
    indices = r.take(points * 2)
    if max(struct.unpack("<" + "H" * points, indices)) >= count:
        raise ValueError("index exceeds vertex count")
    if r.unpack("H") != 0:
        raise ValueError("normal match groups are unsupported")
    r.done()
    return dict(vertices=vertices, normals=normals, tangents=tangents, bitangents=bitangents,
                colors=colors, uv=uv, bound=bound, triangles=triangles, indices=indices,
                material_crc=material_crc, consistency=consistency)


def byte_normal(v: float) -> int:
    if not -1.00001 <= v <= 1.00001:
        raise ValueError("normal or tangent component outside normalized range")
    return max(0, min(255, int(math.floor((v + 1.0) * 127.5 + 0.5))))


def packed_vertices(source: dict) -> tuple[int, bytes]:
    flags = 1 | 0x400  # vertex + full precision
    offsets = {}
    stride = 16
    for key, attribute, flag in (("uv", 1, 2), ("normals", 3, 8), ("tangents", 4, 16), ("colors", 5, 32)):
        if source[key] is not None:
            flags |= flag
            offsets[attribute] = stride
            stride += 4
    descriptor = (flags << 44) | (stride // 4)
    for attribute, offset in offsets.items():
        descriptor |= offset << (4 * attribute + 2)
    packed = bytearray()
    for i, position in enumerate(source["vertices"]):
        bitangent = source["bitangents"][i] if source["bitangents"] else (0.0, 0.0, 0.0)
        packed.extend(struct.pack("<4f", *position, bitangent[0]))
        if source["uv"]:
            packed.extend(struct.pack("<2e", *source["uv"][i]))
        if source["normals"]:
            packed.extend(bytes(byte_normal(x) for x in (*source["normals"][i], bitangent[1])))
        if source["tangents"]:
            packed.extend(bytes(byte_normal(x) for x in (*source["tangents"][i], bitangent[2])))
        if source["colors"]:
            color = source["colors"][i]
            if any(x < 0 or x > 1 for x in color):
                raise ValueError("color outside normalized range")
            packed.extend(bytes(int(math.floor(x * 255 + 0.5)) for x in color))
    assert len(packed) == stride * len(source["vertices"])
    return descriptor, bytes(packed)


def convert(nif: Nif, rebase_private_lod0: bool = False) -> list[dict]:
    if PRIVATE_BLOCK not in nif.types:
        nif.types.append(PRIVATE_BLOCK)
    target = nif.types.index(PRIVATE_BLOCK)
    changes = []
    for i, block in enumerate(nif.blocks):
        if nif.types[nif.ids[i]] != b"BSLODTriShape":
            continue
        r = Reader(block)
        prefix = av_prefix(r)
        reference, skin, materials = r.unpack("iiI")
        if skin != -1 or materials != 0:
            raise ValueError("skinned or material-array legacy geometry is unsupported")
        r.unpack("iB")  # active material / needs-update; no named material entries
        shader, alpha = r.unpack("ii")
        lod = r.unpack("III")
        original_lod = lod
        r.done()
        if reference < 0 or reference >= len(nif.blocks) or nif.types[nif.ids[reference]] != b"NiTriShapeData":
            raise ValueError("legacy mesh has no supported data block")
        source = legacy_data(nif.blocks[reference])
        if sum(lod) != source["triangles"]:
            raise ValueError("LOD segment counts do not cover triangle list")
        if rebase_private_lod0:
            # Private activation fixture only. Native BSMesh OnVisible tests
            # prefix(n) and segment(n); at fade LOD0 a leading empty segment
            # makes both empty. Shift nonempty ordered ranges toward slot0.
            # All vertex/index bytes and the full primitive order stay equal.
            nonempty = [count for count in lod if count]
            lod = tuple(nonempty + [0] * (3 - len(nonempty)))
        descriptor, vertices = packed_vertices(source)
        body = (prefix + source["bound"] + struct.pack("<iiiQHHI", -1, shader, alpha, descriptor,
                source["triangles"], len(source["vertices"]), len(vertices) + len(source["indices"])) +
                vertices + source["indices"] + struct.pack("<IIII", 0, *lod))
        nif.blocks[i], nif.ids[i] = body, target
        changes.append(dict(block=i, source_data=reference, vertices=len(source["vertices"]),
                            triangles=source["triangles"], original_segments=list(original_lod), segments=list(lod), descriptor=f"0x{descriptor:016x}",
                            source_index_sha256=hashlib.sha256(source["indices"]).hexdigest(),
                            packed_vertex_sha256=hashlib.sha256(vertices).hexdigest(), shader_reference=shader, alpha_reference=alpha,
                            source_block_bytes=len(block), converted_block_bytes=len(body)))
    if not changes:
        raise ValueError("no BSLODTriShape blocks found")
    return changes


def activate_private_switch_child(nif: Nif) -> list[dict]:
    """Private fixture only: choose the converted variant at existing active index.

    NifSkope NiNode children are u32 count+i32 refs; NiSwitchNode then adds
    u16 switch flags+u32 active index. No scene node, mesh, or shader is added.
    """
    nodes = {}
    for index, block in enumerate(nif.blocks):
        kind = nif.types[nif.ids[index]]
        if kind not in (b"NiNode", b"BSFadeNode", b"NiSwitchNode"):
            continue
        r = Reader(block)
        r.unpack("i")
        extras = r.unpack("I")
        r.take(extras * 4)
        r.unpack("iI13fi")  # controller, AV flags, transform, collision
        count = r.unpack("I")
        offset = r.pos
        children = [r.unpack("i") for _ in range(count)]
        effects = r.unpack("I")
        r.take(effects * 4)
        active = None
        if kind == b"NiSwitchNode":
            _flags, active = r.unpack("HI")
        r.done()
        nodes[index] = (children, offset, active)

    def contains_private(index, ancestors=()):
        if index < 0:
            return False
        if index >= len(nif.blocks) or index in ancestors:
            raise ValueError("invalid/cyclic fixture node tree")
        if nif.types[nif.ids[index]] == PRIVATE_BLOCK:
            return True
        if index not in nodes:
            return False
        return any(contains_private(child, ancestors + (index,)) for child in nodes[index][0])

    changes = []
    for index, (children, offset, active) in nodes.items():
        if active is None:
            continue
        candidates = [i for i, child in enumerate(children) if contains_private(child)]
        if not candidates:
            continue
        if len(candidates) != 1 or active >= len(children):
            raise ValueError("private switch activation requires one converted child and a valid active index")
        chosen = candidates[0]
        if chosen == active:
            continue
        updated = children.copy()
        updated[active], updated[chosen] = updated[chosen], updated[active]
        block = bytearray(nif.blocks[index])
        struct.pack_into("<" + "i" * len(updated), block, offset, *updated)
        nif.blocks[index] = bytes(block)
        changes.append(dict(block=index, active_index=active, original_children=children,
                            activated_children=updated, child_reference_offset=offset,
                            reason="converted variant was under an inactive switch child; private activation only"))
    if not changes:
        raise ValueError("no inactive converted switch branch found")
    return changes


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--rebase-private-lod0", action="store_true",
                    help="private activation fixture: put nonempty ordered LOD ranges first; changes native visibility semantics")
    ap.add_argument("--activate-private-switch-child", action="store_true",
                    help="private activation fixture: select converted child at the existing switch active index")
    args = ap.parse_args()
    if args.output.exists() or args.output.resolve() == args.input.resolve():
        raise SystemExit("output must be a new private file, distinct from source")
    data = args.input.read_bytes()
    nif = Nif.parse(data)
    if nif.serialize() != data:
        raise ValueError("input did not round-trip byte-for-byte")
    changes = convert(nif, args.rebase_private_lod0)
    switch_changes = activate_private_switch_child(nif) if args.activate_private_switch_child else []
    converted = nif.serialize()
    if Nif.parse(converted).serialize() != converted:
        raise ValueError("converted file did not round-trip")
    manifest = dict(source=str(args.input.resolve()), source_sha256=hashlib.sha256(data).hexdigest(),
                    output=str(args.output.resolve()), output_sha256=hashlib.sha256(converted).hexdigest(),
                    loader_required="DTLPrivateMeshLODTriShape registered to verified native BSMeshLODTriShape factory",
                    runtime_validated=False, private_lod0_rebased=args.rebase_private_lod0, changes=changes, switch_changes=switch_changes,
                    limitations=["normal/tangent/color quantization to native packed format; UV half precision",
                                 "unchanged legacy data blocks remain unreferenced; runtime lifetime needs validation"])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("xb") as f:
        f.write(converted)
    with args.output.with_suffix(args.output.suffix + ".json").open("x") as f:
        json.dump(manifest, f, indent=2)
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
