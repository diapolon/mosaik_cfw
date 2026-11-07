#!/usr/bin/env python3
"""FAT32 per l'immagine SD, pronto a crescere fino a tutta la scheda (vedi m2/sdgrow.c).

    fatprep.py FILE MB MAXGB

FILE è un FAT32 appena creato da mkfs.vfat su un file sparso di MAXGB: la sua tabella dei
cluster basta già per una scheda di quella misura. Qui il file system si riduce a MB (solo il
numero di settori nel settore di avvio e lo spazio libero) e il file si tronca: all'avvio sulla
console sdgrow riporta il numero di settori alla misura della scheda, senza spostare dati.
"""
import struct
import sys

path, mb = sys.argv[1], int(sys.argv[2])
SEC = 512
with open(path, "r+b") as f:
    bs = bytearray(f.read(SEC))
    assert bs[82:90] == b"FAT32   " and bs[510:512] == b"\x55\xaa", "non è FAT32"
    bps, spc, rsvd, nfats = struct.unpack_from("<HBHB", bs, 11)
    assert bps == SEC
    fatsz, = struct.unpack_from("<I", bs, 36)
    fsinfo, bk = struct.unpack_from("<HH", bs, 48)
    tot = mb * 1024 * 1024 // SEC
    data = rsvd + nfats * fatsz
    clusters = (tot - data) // spc
    # sotto 65525 cluster non è più un FAT32 valido
    assert clusters >= 65525, f"troppo piccolo per FAT32 con cluster da {spc // 2} KB: {clusters} cluster"
    for s in (0, bk):
        f.seek(s * SEC)
        b = bytearray(f.read(SEC))
        struct.pack_into("<I", b, 32, tot)
        f.seek(s * SEC)
        f.write(b)
    for s in (fsinfo, bk + fsinfo):
        f.seek(s * SEC)
        fi = bytearray(f.read(SEC))
        assert struct.unpack_from("<I", fi, 0)[0] == 0x41615252
        # libero: tutti i cluster tranne quello della cartella principale (il 2)
        struct.pack_into("<II", fi, 488, clusters - 1, 3)
        f.seek(s * SEC)
        f.write(fi)
    f.truncate(tot * SEC)
cap = fatsz * SEC // 4 - 2
print(f"FAT32: {mb} MB, cluster da {spc // 2} KB, {clusters} cluster; tabella per {cap * spc // 2 // 1024 // 1024} GB")
