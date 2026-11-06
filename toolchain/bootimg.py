#!/usr/bin/env python3
"""Immagine di avvio Android (header v0), il formato di boot.fex sui firmware TrimUI.
  bootimg.py unpack boot.fex DIR       -> DIR/kernel, DIR/ramdisk, DIR/info.txt
  bootimg.py pack DIR NUOVO.img [ramdisk] -> stessa intestazione, ramdisk sostituito"""
import hashlib, struct, sys, os

HDR = "<8s10I16s512s32s1024s"   # magic, kernel_size/addr, ramdisk_size/addr, second_size/addr, tags, page, ver, os_ver, name, cmdline, id, extra

def unpack(img, out):
    d = open(img, "rb").read()
    f = struct.unpack_from(HDR, d)
    assert f[0] == b"ANDROID!"
    ks, ka, rs, ra, ss, sa, tags, page = f[1:9]
    os.makedirs(out, exist_ok=True)
    def blk(n): return (n + page - 1) // page * page
    k0 = page; r0 = k0 + blk(ks)
    open(f"{out}/kernel", "wb").write(d[k0:k0 + ks])
    open(f"{out}/ramdisk", "wb").write(d[r0:r0 + rs])
    info = dict(kernel_addr=hex(ka), ramdisk_addr=hex(ra), second_addr=hex(sa), tags_addr=hex(tags), page=page,
                name=f[11].rstrip(b"\0").decode(), cmdline=(f[12].rstrip(b"\0") + f[14].rstrip(b"\0")).decode())
    with open(f"{out}/info.txt", "w") as o:
        for k, v in info.items(): o.write(f"{k}={v}\n")
    print(open(f"{out}/info.txt").read(), end="")

def pack(src_img, out, ramdisk):
    d = open(src_img, "rb").read()
    f = list(struct.unpack_from(HDR, d))
    page = f[8]
    ks = f[1]
    kernel = d[page:page + ks]
    rd = open(ramdisk, "rb").read()
    f[3] = len(rd); f[5] = 0
    sha = hashlib.sha1(); sha.update(kernel); sha.update(struct.pack("<I", len(kernel)))
    sha.update(rd); sha.update(struct.pack("<I", len(rd))); sha.update(struct.pack("<I", 0))
    f[13] = sha.digest().ljust(32, b"\0")
    hdr = struct.pack(HDR, *f).ljust(page, b"\0")
    def pad(b): return b + b"\0" * ((-len(b)) % page)
    open(out, "wb").write(hdr + pad(kernel) + pad(rd))

if __name__ == "__main__":
    if sys.argv[1] == "unpack": unpack(sys.argv[2], sys.argv[3])
    else: pack(sys.argv[2], sys.argv[3], sys.argv[4])
