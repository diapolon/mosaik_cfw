#!/usr/bin/env python3
"""Mette la pagina del trasferimento file (frontend/src/launcher/fileserver.html) in un header C.

Da rilanciare dopo ogni modifica alla pagina:  python3 toolchain/embed-page.py
L'header generato si committa: la compilazione per console non ha bisogno di Python."""
import os

top = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(top, "frontend/src/launcher/fileserver.html")
dst = os.path.join(top, "frontend/src/launcher/fileserver_page.h")
data = open(src, encoding="utf-8").read()
out = ["/* Generato da toolchain/embed-page.py da fileserver.html: non modificare a mano. */",
       "#pragma once", "static const char FS_PAGE[] ="]
for line in data.splitlines(keepends=True):
    esc = line.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")
    esc = esc.replace("??", "?\\?")   # niente trigrafi
    out.append('    "%s"' % esc)
out[-1] += ";"
open(dst, "w", encoding="utf-8").write("\n".join(out) + "\n")
print("scritto", os.path.relpath(dst, top), len(data), "byte")
