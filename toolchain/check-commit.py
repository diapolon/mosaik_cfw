#!/usr/bin/env python3
"""Controllo prima di ogni commit: niente dati privati, file locali o ROM su GitHub.

Lo chiama .git/hooks/pre-commit. Guarda solo le righe aggiunte nel commit e i file aggiunti:
  - file che restano sul Mac (CLAUDE.md, docs/AUDIT-*, site/, ...), chiavi, ROM e salvataggi;
  - indirizzi MAC veri (quelli finti vanno scritti con il bit "locale": 02:00:00:...);
  - indirizzi IP (quelli finti: 192.0.2.x, 198.51.100.x, 203.0.113.x);
  - percorsi personali (/Users/..., /Volumes/<nome del NAS>), chiavi private, email;
  - le parole di .git/info/parole-private.txt (SSID, nomi, indirizzi: una per riga, mai su GitHub);
  - autore e committer diversi dall'indirizzo noreply.

  python3 toolchain/check-commit.py           # il commit preparato (git add)
  python3 toolchain/check-commit.py --tutto   # tutti i file tracciati, per un controllo completo

Un dato finto voluto si segna sulla sua riga con il commento `check-commit: ok`; un falso allarme
si scavalca con `git commit --no-verify`, solo dopo averlo controllato a occhio.
"""
import os
import re
import subprocess
import sys

NOREPLY = "diapolon@users.noreply.github.com"

# percorsi che restano sul Mac (CLAUDE.md, "Vincoli da rispettare") o che non vanno mai su GitHub
VIETATI = [
    r"CLAUDE\.md$", r"docs/AUDIT-", r"docs/audit/", r"docs/archivio/", r"docs/PASSAGGIO\.md$",
    r"docs/rilascio-", r"docs/testing-notes\.md$", r"docs/ROADMAP\.md$", r"docs/STUDIO-",
    r"docs/analisi-", r"docs/mockup/", r"site/", r"rom-inventario/", r"frontend/package/linux-diapolon/",
    r"\.claude/", r"dist/", r"deps/", r"frontend/sdcard/",
]
# chiavi, ROM, BIOS, dischi e salvataggi ("md" no: è anche Markdown)
EST_VIETATE = {
    "pem", "key", "p12", "zip", "7z", "rar", "chd", "cue", "iso", "pbp", "bin", "img", "rom", "sfc", "smc",
    "gba", "gb", "gbc", "nes", "fds", "gen", "smd", "sms", "gg", "n64", "z64", "v64", "ngp", "ngc", "ws",
    "wsc", "pce", "lnx", "a26", "a78", "col", "vb", "min", "srm", "sav", "rtc", "state", "squashfs", "gz",
}

MAC = re.compile(r"(?<![0-9A-Fa-f:])((?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2})(?![0-9A-Fa-f:])")
MAC_FINTI = {"00:11:22:33:44:55", "11:22:33:44:55:66", "AA:BB:CC:DD:EE:FF", "12:34:56:78:9A:BC",
             "00:00:00:00:00:00", "FF:FF:FF:FF:FF:FF"}
IP = re.compile(r"(?<![\w.])((?:\d{1,3}\.){3}\d{1,3})(?![\w.])")
# file che per mestiere elencano indirizzi privati e pubblici da rifiutare (prove di Scarica file)
IP_ESENTI = {"frontend/src/apps/urlget_test.c", "m2/test-urlget.sh"}
EMAIL = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}")
PERCORSI = re.compile(r"/Users/(?!\$|<|\.\.\.)[A-Za-z]|/Volumes/diapolon\b|/home/(?!\$|<)[a-z]")  # check-commit: ok
CHIAVE = re.compile(r"BEGIN [A-Z ]*PRIVATE KEY")


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True, errors="replace").stdout


def mac_ok(m):
    m = m.upper()
    if m in MAC_FINTI or len(set(m.split(":"))) == 1:
        return True
    return int(m[:2], 16) & 0x02 != 0   # indirizzo "locale": non appartiene a nessun produttore


def ip_ok(ip, resto):
    p = [int(x) for x in ip.split(".")]
    if any(x > 255 for x in p):
        return True   # non è un indirizzo (versioni, numeri)
    if re.match(r"/\d{1,2}\b", resto):
        return True   # rete in notazione CIDR (regole del codice, non un apparecchio)
    if p[0] in (0, 127, 255) or p[:3] in ([192, 0, 2], [198, 51, 100], [203, 0, 113]):
        return True
    if p[0] == 224 or p[0] == 239 or (p[0] == 169 and p[1] == 254 and p[2] == 0):
        return True   # multicast (mDNS) e rete locale di servizio
    if p[3] == 0 and (p[0] == 10 or p[:2] == [192, 168] or (p[0] == 172 and 16 <= p[1] <= 31)):
        return True   # inizio di una rete privata: limiti nel codice
    return False


def email_ok(e, path):
    e = e.lower()
    if e.endswith("@users.noreply.github.com") or e.startswith("noreply@"):
        return True
    if re.search(r"@(example|esempio)\.(com|org|net|it)$", e) or e.endswith(".invalid") or e.endswith(".test"):
        return True
    # testi delle licenze e patch dei progetti originali: indirizzi dei loro autori
    return path.startswith("licenses/") or path.endswith(".patch")


def parole_private():
    p = os.path.join(git("rev-parse", "--git-dir").strip(), "info", "parole-private.txt")
    try:
        with open(p, encoding="utf-8") as f:
            return [l.strip() for l in f if l.strip() and not l.lstrip().startswith("#")]
    except OSError:
        return []


def controlla_riga(path, n, riga, parole, errori):
    if "check-commit: ok" in riga:
        return   # dato finto voluto (per esempio indirizzi privati che una prova deve rifiutare)
    for m in MAC.finditer(riga):
        if not mac_ok(m.group(1)):
            errori.append(f"{path}:{n}: indirizzo MAC che sembra vero ({m.group(1)}): usa 02:00:00:xx:xx:xx")
    for m in IP.finditer(riga):
        if path not in IP_ESENTI and not ip_ok(m.group(1), riga[m.end():]):
            errori.append(f"{path}:{n}: indirizzo IP ({m.group(1)}): usa 192.0.2.x o 198.51.100.x")
    for m in EMAIL.finditer(riga):
        if not email_ok(m.group(0), path):
            errori.append(f"{path}:{n}: indirizzo email ({m.group(0)})")
    if PERCORSI.search(riga):
        errori.append(f"{path}:{n}: percorso personale (/Users, /home o il NAS)")
    if CHIAVE.search(riga):
        errori.append(f"{path}:{n}: chiave privata")
    low = riga.lower()
    for i, w in enumerate(parole, 1):
        if w.lower() in low:   # la parola non si stampa: anche il terminale può finire in un registro
            errori.append(f"{path}:{n}: parola privata n. {i} di .git/info/parole-private.txt")


def controlla_percorso(path, errori):
    if any(re.match(v, path) for v in VIETATI):
        errori.append(f"{path}: file che resta sul Mac (CLAUDE.md, «Vincoli da rispettare»)")
    est = path.rsplit(".", 1)[-1].lower() if "." in os.path.basename(path) else ""
    if est in EST_VIETATE:
        errori.append(f"{path}: tipo di file che non va su GitHub (.{est}: ROM, BIOS, chiavi, archivi, salvataggi)")


def commit_preparato(parole, errori):
    for path in git("diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z").split("\0"):
        if path:
            controlla_percorso(path, errori)
    # Nomi con lettere accentate: senza core.quotePath=false git li scrive tra virgolette ("+++ "b/…")
    # e il file non si controllava; l'intestazione "+++ " vale solo dopo "diff --git", se no una riga
    # aggiunta che comincia con "++ " fermava i controlli del resto del file (revisione della 0.9.91)
    path, n, testa = None, 0, False
    for riga in git("-c", "core.quotePath=false", "diff", "--cached", "-U0", "--no-color", "--no-ext-diff",
                    "--diff-filter=ACMR").splitlines():
        if riga.startswith("diff --git "):
            path, testa = None, True
        elif testa and riga.startswith("+++ "):
            p = riga[4:]
            if p.startswith('"') and p.endswith('"'):
                p = p[1:-1]
            path = p[2:] if p.startswith("b/") else None
        elif riga.startswith("@@"):
            testa = False
            m = re.search(r"\+(\d+)", riga)
            n = int(m.group(1)) if m else 0
        elif not testa and riga.startswith("+") and path:
            controlla_riga(path, n, riga[1:], parole, errori)
            n += 1
    for chi in ("GIT_AUTHOR_IDENT", "GIT_COMMITTER_IDENT"):
        ident = git("var", chi)
        if NOREPLY not in ident:
            errori.append(f"{chi.split('_')[1].lower()}: l'indirizzo deve essere {NOREPLY}")


def tutto(parole, errori):
    for path in git("ls-files", "-z").split("\0"):
        if not path:
            continue
        controlla_percorso(path, errori)
        try:
            with open(path, "rb") as f:
                dati = f.read()
        except OSError:
            continue
        if b"\0" in dati[:8192]:
            continue   # binario (immagini)
        for n, riga in enumerate(dati.decode("utf-8", "replace").splitlines(), 1):
            controlla_riga(path, n, riga, parole, errori)


def main():
    os.chdir(git("rev-parse", "--show-toplevel").strip() or ".")
    parole, errori = parole_private(), []
    (tutto if "--tutto" in sys.argv[1:] else commit_preparato)(parole, errori)
    if errori:
        print("check-commit: commit fermato, da controllare:", file=sys.stderr)
        for e in errori[:60]:
            print("  " + e, file=sys.stderr)
        if len(errori) > 60:
            print(f"  ... e altri {len(errori) - 60}", file=sys.stderr)
        print("Correggi e riprova. Solo un falso allarme verificato a occhio: git commit --no-verify", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
