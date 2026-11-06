"""Elenca ed estrae i file di un'immagine Allwinner IMAGEWTY v3 (non cifrata)."""
import struct, sys
path, out = sys.argv[1], sys.argv[2]
want = set(sys.argv[3:])
f = open(path, "rb")
hdr = f.read(1024)
assert hdr[:8] == b"IMAGEWTY"
nfiles = struct.unpack_from("<I", hdr, 0x3C)[0]
for i in range(nfiles):
    f.seek(1024 + i * 1024)
    h = f.read(1024)
    main = h[8:16].rstrip(b"\0 ").decode(errors="replace")
    sub = h[16:32].rstrip(b"\0 ").decode(errors="replace")
    name = h[36:36 + 256].split(b"\0")[0].decode(errors="replace")
    stored, _, orig, _, off = struct.unpack_from("<IIIII", h, 36 + 256)
    print(f"{sub:16} {name:40} {orig:>11} @ {off:#x}")
    base = name.rsplit("/", 1)[-1] or sub
    if base in want or sub in want:
        f.seek(off)
        with open(f"{out}/{base}", "wb") as o:
            left = orig
            while left:
                chunk = f.read(min(left, 1 << 20)); o.write(chunk); left -= len(chunk)
