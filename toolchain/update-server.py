#!/usr/bin/env python3
"""Offre l'aggiornamento del sistema Arcade alle console sulla stessa rete Wi-Fi.

    python3 toolchain/update-server.py              offre l'aggiornamento in rete locale
    python3 toolchain/update-server.py --out CART   prepara i file da pubblicare online

Usa ciò che ha prodotto m2/build-m2.sh: dist/arcade-system.squashfs, dist/arcade-version.txt e
dist/SD/Arcade. Crea l'elenco dei file (update.txt), lo firma con la chiave privata
(~/.config/arcade-update/update-key.pem) e resta in ascolto finché non si preme Ctrl-C.
La console lo trova da sola con un messaggio broadcast (UDP 8766). Con --out scrive invece in
una cartella update.txt, update.sig e file-0, file-1...: file statici da caricare su un server
web o in una Release di GitHub (indirizzo da mettere nel file UPDATE_URL). Passano solo i file
del sistema: niente ROM, salvataggi o impostazioni.
"""
import argparse
import hashlib
import http.server
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading

ap = argparse.ArgumentParser()
top = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap.add_argument("--dist", default=os.path.join(top, "dist"))
ap.add_argument("--key", default=os.path.expanduser("~/.config/arcade-update/update-key.pem"))
ap.add_argument("--http-port", type=int, default=8765)
ap.add_argument("--udp-port", type=int, default=8766)
ap.add_argument("--out", help="cartella in cui preparare i file da pubblicare")
ap.add_argument("--reinstall", type=int, help="soglia dell'invito a riscrivere la SD (altrimenti RISCRIVERE_SOTTO di m2/area-avvio.conf)")
args = ap.parse_args()

DATA = os.path.join(args.dist, "SD", "Arcade")
SYSTEM = os.path.join(args.dist, "arcade-system.squashfs")
TREES = ["Lang", "Emulators", "Cores", "Themes", "Filters", "Bezels"]


def wanted():
    """(percorso sulla SD, percorso qui) di ogni file del sistema."""
    yield "System/arcade-system.squashfs", SYSTEM
    yield "System/cacert.pem", os.path.join(DATA, "System", "cacert.pem")
    for t in TREES:
        for d, _, names in sorted(os.walk(os.path.join(DATA, t))):
            for n in sorted(names):
                if n.startswith("."):
                    continue
                p = os.path.join(d, n)
                yield os.path.relpath(p, DATA).replace(os.sep, "/"), p
    # gli indici degli emulatori; Dats/arcadedb.tsv e Dats/history li scrive lo scraper sulla console
    dats = os.path.join(DATA, "Dats")
    for n in sorted(os.listdir(dats)):
        stem = n.rsplit(".", 1)[0]
        # solo gli indici arcade (con accanto <id>.roms): gli altri li scrive la console
        if n.endswith((".tsv", ".roms")) and os.path.exists(os.path.join(dats, stem + ".roms")):
            yield "Dats/" + n, os.path.join(dats, n)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


for need in (SYSTEM, DATA, os.path.join(args.dist, "arcade-version.txt")):
    if not os.path.exists(need):
        sys.exit(f"manca {need}: prima sh m2/build-m2.sh")
if not os.path.exists(args.key):
    sys.exit(f"manca la chiave privata {args.key}")

version = open(os.path.join(args.dist, "arcade-version.txt")).read().strip()
files = list(wanted())
lines = ["arcade-update 1", "version " + version]
# invito a riscrivere la SD per le schede con un'area di avvio più vecchia (m2/area-avvio.conf)
conf = open(os.path.join(top, "m2", "area-avvio.conf")).read()
reinstall = int(next((l.split("=", 1)[1] for l in conf.splitlines() if l.startswith("RISCRIVERE_SOTTO=")), "0"))
if args.reinstall is not None:
    reinstall = args.reinstall
if reinstall > 0:
    lines.append(f"reinstall {reinstall}")
# solo per le prove sul canale Sviluppo: MOSAIK_PROVA_PAUSA=30 ferma l'installazione a metà (dati
# nuovi, sistema vecchio) per spegnere a forza e vedere il ritorno ai file di prima (audit R03)
pausa = int(os.environ.get("MOSAIK_PROVA_PAUSA", "0") or 0)
if pausa > 0:
    lines.append(f"prova-pausa {min(pausa, 60)}")
for sd, path in files:
    lines.append(f"file {sha256(path)} {os.path.getsize(path)} {sd}")
manifest = ("\n".join(lines) + "\n").encode()
with tempfile.NamedTemporaryFile() as t:
    t.write(manifest)
    t.flush()
    signature = subprocess.run(["openssl", "dgst", "-sha256", "-sign", args.key, t.name],
                               check=True, capture_output=True).stdout


if args.out:
    os.makedirs(args.out, exist_ok=True)
    for name, data in (("update.txt", manifest), ("update.sig", signature)):
        with open(os.path.join(args.out, name), "wb") as f:
            f.write(data)
    for i, (sd, path) in enumerate(files):
        shutil.copyfile(path, os.path.join(args.out, f"file-{i}"))
    print(f"Aggiornamento {version}: {len(files) + 2} file pronti in {args.out}")
    sys.exit(0)


class Handler(http.server.BaseHTTPRequestHandler):
    def send_bytes(self, data):
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/update.txt":
            return self.send_bytes(manifest)
        if self.path == "/update.sig":
            return self.send_bytes(signature)
        if self.path.startswith("/file-") and self.path[6:].isdigit() and int(self.path[6:]) < len(files):
            sd, path = files[int(self.path[6:])]
            self.send_response(200)
            self.send_header("Content-Length", str(os.path.getsize(path)))
            self.end_headers()
            with open(path, "rb") as f:
                for b in iter(lambda: f.read(1 << 18), b""):
                    self.wfile.write(b)
            print("  inviato", sd, flush=True)
            return
        self.send_error(404)

    def log_message(self, *a):
        pass


def udp_responder():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", args.udp_port))
    while True:
        data, addr = s.recvfrom(64)
        if data == b"ARCADE-UPDATE?":
            s.sendto(f"ARCADE-UPDATE {args.http_port}".encode(), addr)
            print("console trovata:", addr[0], flush=True)


threading.Thread(target=udp_responder, daemon=True).start()
total = sum(os.path.getsize(p) for _, p in files)
print(f"Aggiornamento {version}: {len(files)} file, {total // (1 << 20)} MB.")
print("Sulla console: Impostazioni > Strumenti > Aggiorna sistema. Ctrl-C per finire.", flush=True)
try:
    http.server.ThreadingHTTPServer(("", args.http_port), Handler).serve_forever()
except KeyboardInterrupt:
    print()
