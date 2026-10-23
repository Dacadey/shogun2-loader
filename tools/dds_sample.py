r"""Picks a sample of the game's .dds textures, a few of every pixel format, for engine\test\tex_test.exe.

  python -I tools/dds_sample.py <game data folder> <out folder> [--per-kind 12]

Reads the index of every .pack in the folder (PFH2/PFH3), classifies each .dds by its header
(DX10 header format, FourCC, or 32-bit mask layout, plus cube and mip count) and copies up to --per-kind files of each
kind, largest first and then every n-th, to <out>\<kind>\<n>.dds. Prints the kinds and how many files each has.
"""
import collections
import os
import struct
import sys


def read_index(path):
    with open(path, "rb") as f:
        head = f.read(0x20)
        magic, flags, pf_count, pf_size, count, idx_size = struct.unpack_from("<4sIIIII", head)
        if magic not in (b"PFH3", b"PFH2"):
            return []
        f.seek(0x20 + pf_size)
        idx = f.read(idx_size)
    has_ts = bool(flags & 0x40)
    entries, pos, data_off = [], 0, 0x20 + pf_size + idx_size
    for _ in range(count):
        size = struct.unpack_from("<I", idx, pos)[0]
        pos += 4 + (8 if has_ts else 0)
        end = idx.index(b"\0", pos)
        entries.append((idx[pos:end].decode("latin-1"), data_off, size))
        pos = end + 1
        data_off += size
    return entries


def kind_of(h):
    if len(h) < 128 or h[:4] != b"DDS ":
        return None
    flags, height, width = struct.unpack_from("<III", h, 8)
    mips = struct.unpack_from("<I", h, 28)[0] if flags & 0x20000 else 1
    pf_flags, fourcc, bits, r, g, b, a = struct.unpack_from("<IIIIIII", h, 80)
    caps2 = struct.unpack_from("<I", h, 112)[0]
    if pf_flags & 4:
        cc = struct.pack("<I", fourcc)
        if cc == b"DX10" and len(h) >= 148:
            fmt = "DX10_%d" % struct.unpack_from("<I", h, 128)[0]
        elif fourcc < 256:
            fmt = "D3DFMT_%d" % fourcc
        else:
            fmt = cc.decode("latin-1")
    else:
        fmt = "%dbit_%08X_%08X" % (bits, r, a)
    full = max(width, height).bit_length()
    return "%s%s%s" % (fmt, "_cube" if caps2 & 0x200 else "", "_fullmips" if mips >= full else "_m%d" % mips)


def main():
    data, out = sys.argv[1], sys.argv[2]
    per = int(sys.argv[sys.argv.index("--per-kind") + 1]) if "--per-kind" in sys.argv else 12
    found = collections.defaultdict(list)
    for name in sorted(os.listdir(data)):
        if not name.endswith(".pack"):
            continue
        path = os.path.join(data, name)
        with open(path, "rb") as f:
            for entry, off, size in read_index(path):
                if not entry.lower().endswith(".dds"):
                    continue
                f.seek(off)
                k = kind_of(f.read(min(size, 148)))
                if k:
                    found[k].append((size, path, off, entry))
    for k in sorted(found, key=lambda k: -len(found[k])):
        files = sorted(found[k], reverse=True)
        pick = files[:per // 2] + files[per // 2::max(1, len(files) // max(1, per - per // 2))][:per - per // 2]
        os.makedirs(os.path.join(out, k), exist_ok=True)
        for i, (size, path, off, entry) in enumerate(pick):
            with open(path, "rb") as f:
                f.seek(off)
                blob = f.read(size)
            with open(os.path.join(out, k, "%02d.dds" % i), "wb") as o:
                o.write(blob)
        print("%-40s %6d files, %4d copied (largest %.1f MB)" % (k, len(files), len(pick), files[0][0] / 1048576))


if __name__ == "__main__":
    main()
