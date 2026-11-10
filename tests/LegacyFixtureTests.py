"""CPU validation against a separately extracted vanilla fixture; no game I/O."""
import copy
import importlib.util
from pathlib import Path
import struct
import sys

module_path = Path(__file__).parents[1] / "tools/mesh/legacy_fixture.py"
spec = importlib.util.spec_from_file_location("legacy_fixture", module_path)
fixture = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = fixture
spec.loader.exec_module(fixture)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def rejects(call, message):
    try:
        call()
    except (ValueError, struct.error):
        return
    raise AssertionError(message)


source = Path(sys.argv[1]).read_bytes()
original = fixture.Nif.parse(source)
check(original.serialize() == source, "header, block references and footer exact roundtrip")
converted = copy.deepcopy(original)
changes = fixture.convert(converted)
check(changes, "fixture contains real legacy LOD blocks")
converted = fixture.Nif.parse(converted.serialize())
changed = {row["block"] for row in changes}
for i, block in enumerate(original.blocks):
    if i not in changed:
        check(converted.blocks[i] == block and converted.ids[i] == original.ids[i], "unrelated blocks unchanged")

for row in changes:
    index = row["block"]
    check(converted.types[converted.ids[index]] == fixture.PRIVATE_BLOCK, "distinct private loader name")
    old_data = fixture.legacy_data(original.blocks[row["source_data"]])
    reader = fixture.Reader(converted.blocks[index])
    old_reader = fixture.Reader(original.blocks[index])
    check(fixture.av_prefix(reader) == fixture.av_prefix(old_reader), "name, transform and flags preserved")
    check(reader.take(16) == old_data["bound"], "full source bound preserved")
    skin, shader, alpha, desc, tris, verts, payload = reader.unpack("iiiQHHI")
    check(skin == -1 and tris == old_data["triangles"] and verts == len(old_data["vertices"]), "native dimensions preserved")
    stride = (desc & 15) * 4
    vertices = reader.take(stride * verts)
    indices = reader.take(tris * 6)
    check(indices == old_data["indices"], "all original vertex IDs, winding and segment order preserved")
    check(payload == len(vertices) + len(indices), "native upload payload exact")
    check(reader.unpack("I") == 0, "no fabricated particle payload")
    check(list(reader.unpack("III")) == row["segments"], "native LOD segments preserved")
    reader.done()
    check(sum(row["segments"]) == tris, "segments cover full triangle list")
    for vertex in range(verts):
        at = vertex * stride
        check(struct.unpack_from("<3f", vertices, at) == old_data["vertices"][vertex], "positions remain full precision")
        if old_data["uv"]:
            uv_at = (desc >> 6) & 0x3c
            decoded = struct.unpack_from("<2e", vertices, at + uv_at)
            expected = struct.unpack("<2e", struct.pack("<2e", *old_data["uv"][vertex]))
            check(decoded == expected, "UV descriptor offset resolves packed half values")
        if old_data["normals"]:
            normal_at = (desc >> 14) & 0x3c
            decoded = [x / 127.5 - 1 for x in vertices[at + normal_at:at + normal_at + 3]]
            check(all(abs(a-b) <= 1/255 + 0.00001 for a,b in zip(decoded, old_data["normals"][vertex])),
                  "normal quantization stays within half native step")
    bad_counts = copy.deepcopy(original)
    bad = bytearray(bad_counts.blocks[index])
    struct.pack_into("<I", bad, len(bad)-12, row["segments"][0]+1)
    bad_counts.blocks[index] = bytes(bad)
    rejects(lambda: fixture.convert(bad_counts), "bad LOD segments must reject before output")

rejects(lambda: fixture.Nif.parse(source[:-1]), "truncated input rejects")
bad_version = bytearray(source)
struct.pack_into("<I", bad_version, source.index(b"\n")+1, 0x14020008)
rejects(lambda: fixture.Nif.parse(bytes(bad_version)), "unsupported stream version rejects")
rejects(lambda: fixture.byte_normal(1.1), "invalid normals reject")
rebased = copy.deepcopy(original)
rebased_changes = fixture.convert(rebased, rebase_private_lod0=True)
for row in rebased_changes:
    index = row["block"]
    check(rebased.blocks[index][:-12] == converted.blocks[index][:-12],
          "private visibility rebase changes only final triangle-count metadata")
    nonempty = [count for count in row["original_segments"] if count]
    check(row["segments"] == nonempty + [0] * (3-len(nonempty)), "nonempty segment order preserved")
    check(row["segments"][0] > 0 and sum(row["segments"]) == row["triangles"], "LOD0 segment is nonempty and covers same full geometry")
for i in range(len(original.blocks)):
    if i not in changed:
        check(rebased.blocks[i] == converted.blocks[i], "rebase keeps textures, shader properties and all other blocks exact")
active = copy.deepcopy(rebased)
switch_changes = fixture.activate_private_switch_child(active)
for row in switch_changes:
    before, after = rebased.blocks[row["block"]], active.blocks[row["block"]]
    offset = row["child_reference_offset"]
    end = offset + len(row["activated_children"]) * 4
    check(before[:offset] == after[:offset] and before[end:] == after[end:], "activation changes only child refs")
    check(sorted(row["original_children"]) == sorted(row["activated_children"]), "no added/lost scene child")
    check(row["activated_children"][row["active_index"]] != row["original_children"][row["active_index"]], "converted branch activated")
for i in range(len(original.blocks)):
    if i not in {row["block"] for row in switch_changes}:
        check(active.blocks[i] == rebased.blocks[i], "activation keeps every geometry/material block exact")
print(f"Legacy fixture checks passed: {len(changes)} converted blocks, exact positions/indices/LOD ranges and bounded packed attributes")
