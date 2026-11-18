"""Print the esp_app_desc of app images (name, compile date and time, ELF SHA) and compare them.
Usage: python appdesc.py label=path [label=path ...]   (the first one is the reference)
"""
import struct
import sys


def desc(path):
    with open(path, "rb") as f:
        b = f.read(0x200)
    off = 0x20  # 24-byte image header + 8-byte segment header.
    magic = struct.unpack_from("<I", b, off)[0]
    if magic != 0xABCD5432:
        return None
    ver = b[off + 16:off + 48].split(b"\x00")[0].decode(errors="replace")
    name = b[off + 48:off + 80].split(b"\x00")[0].decode(errors="replace")
    tm = b[off + 80:off + 96].split(b"\x00")[0].decode(errors="replace")
    dt = b[off + 96:off + 112].split(b"\x00")[0].decode(errors="replace")
    idf = b[off + 112:off + 144].split(b"\x00")[0].decode(errors="replace")
    sha = b[off + 144:off + 176].hex()
    return dict(name=name, ver=ver, date=dt, time=tm, idf=idf, sha=sha)


ref = None
for arg in sys.argv[1:]:
    label, path = arg.split("=", 1)
    try:
        d = desc(path)
    except OSError as e:
        print(f"{label}: cannot read ({e})")
        continue
    if d is None:
        print(f"{label}: no app header found")
        continue
    if ref is None:
        ref = d
        same = "(reference)"
    else:
        same = "SAME BUILD" if d["sha"] == ref["sha"] else "DIFFERENT BUILD"
    print(f"{label}: {d['name']} ver={d['ver']} built {d['date']} {d['time']} idf={d['idf']} sha={d['sha'][:16]} {same}")
