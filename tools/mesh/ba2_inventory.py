"""Read-only BA2 GNRL / NIF block inventory, with bounded header decompression.

FO4 GNRL versions 1 and 8 share the validated 24-byte header/36-byte entries.
Only an explicitly named JSON output is created; archives are opened 'rb'.
No asset is extracted, converted, deployed or loaded into a renderer.
"""
from __future__ import annotations
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import time
import zlib


class Ba2:
    def __init__(self, path: Path):
        self.path, self.file = path, path.open("rb")
        magic, self.version, kind, count, names = struct.unpack("<4sI4sIQ", self.file.read(24))
        if magic != b"BTDX" or kind != b"GNRL" or self.version not in (1, 8) or count > 1000000:
            raise ValueError("unsupported BA2 header")
        self.size = path.stat().st_size
        entries = [struct.unpack("<I4sIIQIII", self.file.read(36)) for _ in range(count)]
        self.file.seek(names)
        self.entries = []
        for row in entries:
            length = struct.unpack("<H", self.file.read(2))[0]
            name = self.file.read(length).decode("utf-8", errors="replace")
            if row[4] + (row[5] or row[6]) > self.size:
                raise ValueError("BA2 entry exceeds archive")
            self.entries.append((name, row))

    def prefix(self, row, limit=65536):
        _, _, _, _, offset, packed, unpacked, _ = row
        self.file.seek(offset)
        if not packed:
            return self.file.read(min(limit, unpacked))
        decoder = zlib.decompressobj()
        output = bytearray()
        remaining = packed
        while remaining and len(output) < limit:
            chunk = self.file.read(min(4096, remaining))
            remaining -= len(chunk)
            if not chunk:
                raise ValueError("truncated BA2 payload")
            output.extend(decoder.decompress(chunk, limit - len(output)))
        return bytes(output)


def block_types(data):
    position = data.find(b"\n", 0, 100) + 1
    if not position:
        raise ValueError("missing NIF signature")
    def take(fmt):
        nonlocal position
        result = struct.unpack_from("<" + fmt, data, position)
        position += struct.calcsize("<" + fmt)
        return result[0] if len(result) == 1 else result
    version, endian, user, count, bethesda = take("IBIII")
    if (version, endian, user) != (0x14020007, 1, 12) or bethesda not in (83, 100, 130):
        raise ValueError(f"unsupported NIF version {version:X}/{user}/{bethesda}")
    for _ in range(4 if bethesda == 130 else 3):
        length = take("B")
        position += length
    types = []
    for _ in range(take("H")):
        length = take("I")
        if length > 512 or position + length > len(data):
            raise ValueError("invalid or truncated block type string")
        types.append(data[position:position+length].decode("ascii"))
        position += length
    ids = [take("H") for _ in range(count)]
    if any(i >= len(types) for i in ids):
        raise ValueError("invalid block type index")
    counts = Counter(types[i] for i in ids)
    return dict(bethesda_version=bethesda, blocks=count, types=dict(counts),
                mesh_blocks=[i for i,t in enumerate(ids) if types[t] == "BSMeshLODTriShape"],
                legacy_blocks=[i for i,t in enumerate(ids) if types[t] == "BSLODTriShape"])


def inventory(path):
    started = time.monotonic()
    archive = Ba2(path)
    total, matches, errors, types = 0, [], [], Counter()
    for name, row in archive.entries:
        if not name.lower().endswith(".nif"):
            continue
        total += 1
        try:
            info = block_types(archive.prefix(row, 65536))
            types.update(info["types"])
            if info["mesh_blocks"] or info["legacy_blocks"]:
                matches.append(dict(path=name, uncompressed_bytes=row[6], **info))
        except (ValueError, struct.error, zlib.error, UnicodeError) as error:
            errors.append(dict(path=name, error=str(error)))
    archive.file.close()
    return dict(archive=str(path.resolve()), ba2_version=archive.version, bytes=archive.size,
                nif_files=total, parsed_nif_files=total-len(errors), block_counts=dict(types),
                lod_files=matches, errors=errors, seconds=round(time.monotonic()-started,2),
                decompressed_prefix_limit_bytes=65536)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archives", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit("output must be a new report")
    result=[]
    for path in args.archives:
        row=inventory(path);result.append(row)
        print(json.dumps({k:row[k] for k in ("archive","nif_files","parsed_nif_files","seconds")}|{
            "mesh_blocks":row["block_counts"].get("BSMeshLODTriShape",0),
            "legacy_blocks":row["block_counts"].get("BSLODTriShape",0),"lod_files":len(row["lod_files"])}), flush=True)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open("x") as out:
        json.dump(result,out,indent=2)


if __name__ == "__main__":
    main()
