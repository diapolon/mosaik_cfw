#!/usr/bin/env python3
"""Quali suite di m2/test-*.sh riguardano i file cambiati, e quali file nessuna prova copre.

  python3 toolchain/prove-toccate.py              # modifiche non ancora committate (e file nuovi)
  python3 toolchain/prove-toccate.py --cached     # solo il commit preparato (lo usa il pre-commit)
  python3 toolchain/prove-toccate.py --da v0.9.88 # tutto il cambiato da un tag: prima di una stabile
  ... --esegui                                    # le esegue in Docker, con il comando scritto in
                                                  # testa a ogni suite, e si ferma al primo errore

La mappa si ricava dalle suite stesse (i file che nominano), più EXTRA qui sotto: una suite nuova
che nomina i suoi sorgenti entra da sola. I file che nessuna suite nomina si elencano: sono quelli
da provare a mano, o per cui scrivere una prova.
"""
import os
import re
import subprocess
import sys

# collegamenti che le suite non scrivono per nome
EXTRA = {
    "frontend/src/launcher/fileserver.html": ["test-fileserver"],
    "frontend/src/launcher/fileserver_page.h": ["test-fileserver"],
    "toolchain/embed-page.py": ["test-fileserver"],
    "frontend/src/launcher/scrape_client.c": ["test-scraper"],
    "frontend/src/common/durable.h": ["test-files", "test-store", "test-savesplit"],
}
# file che non sono codice: non serve una prova automatica
SENZA_PROVA = re.compile(r"\.(md|txt|png|bmp|jpg|svg|ttf|otf|yml|LESSER)$|^docs/|^licenses/|^frontend/lang/|^\.github/"
                         r"|^frontend/package/immagini/|^frontend/package/.*/(icon|cover)\.png$|^CHANGELOG|^VERSION$|^TODO")


def git(*args):
    r = subprocess.run(["git", *args], capture_output=True, text=True, errors="replace")
    if r.returncode:
        sys.exit(f"prove-toccate: git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout


def cambiati(argv):
    if "--cached" in argv:
        files = git("diff", "--cached", "--name-only", "--diff-filter=ACMRD").split("\n")
    elif "--da" in argv:
        rif = argv[argv.index("--da") + 1]
        files = git("diff", "--name-only", rif).split("\n") + git("ls-files", "-o", "--exclude-standard").split("\n")
    else:
        files = git("diff", "--name-only", "HEAD").split("\n") + git("ls-files", "-o", "--exclude-standard").split("\n")
    return sorted({f for f in files if f})


def mappa(tracciati):
    """suite -> insieme dei file del repository che nomina"""
    base = {}
    for f in tracciati:
        base.setdefault(os.path.basename(f), []).append(f)
    suite = {}
    for t in sorted(f for f in tracciati if re.match(r"m2/test-[\w-]+\.sh$", f)):
        nome = os.path.basename(t)[:-3]
        testo = open(t, encoding="utf-8", errors="replace").read()
        coperti = {t}
        # percorsi assoluti nel container: /trimui/<percorso del repository>
        for p in re.findall(r"/trimui/([\w./-]+)", testo):
            p = p.rstrip("/.")
            if p in ("frontend/src", "frontend") or p.startswith("frontend/src/") and "." not in os.path.basename(p):
                continue   # cartelle dei sorgenti: troppo larghe, contano i file nominati
            coperti |= {f for f in tracciati if f == p or f.startswith(p + "/")}
        # file nominati con una variabile davanti ($S/launcher/x.c, F/apps/y.c) o solo per nome
        for tok in re.findall(r"[\w./$-]+\.(?:c|h|py|html)\b", testo):
            tok = re.sub(r"^.*?/trimui/", "", tok)
            parti = [x for x in tok.split("/") if x and not re.fullmatch(r"\$?[A-Z]\w*", x)]
            if not parti:
                continue
            coda = "/".join(parti)
            if "/" in coda:
                coperti |= {f for f in tracciati if ("/" + f).endswith("/" + coda)}
            elif len(base.get(coda, [])) == 1:
                coperti.add(base[coda][0])
        suite[nome] = coperti
    for f, nomi in EXTRA.items():
        for n in nomi:
            suite.setdefault(n, set()).add(f)
    return suite


def comando(nome, radice):
    with open(os.path.join(radice, "m2", nome + ".sh"), encoding="utf-8") as f:
        for riga in f.readlines()[:15]:
            if "docker run" in riga:
                c = riga.split("docker run", 1)[1].strip()
                return "docker run " + c.replace('"$PWD"', '"' + radice + '"')
    return None


def main(argv):
    radice = git("rev-parse", "--show-toplevel").strip()
    os.chdir(radice)
    tracciati = [f for f in git("ls-files").split("\n") if f]
    files = cambiati(argv)
    if not files:
        print("prove-toccate: nessun file cambiato")
        return 0
    suite = mappa(tracciati + [f for f in files if f not in tracciati])
    scelte = {n: sorted(set(files) & coperti) for n, coperti in suite.items() if set(files) & coperti}
    senza = [f for f in files if f not in set().union(*scelte.values()) and not SENZA_PROVA.search(f)
             and os.path.exists(f)]
    if scelte:
        print("Suite da eseguire per i file cambiati:")
        for n in sorted(scelte):
            print(f"  {n:16} ({', '.join(scelte[n][:4])}{' ...' if len(scelte[n]) > 4 else ''})")
    if senza:
        print("Nessuna prova automatica per (provali a mano, o scrivi una prova):")
        for f in senza:
            print("  " + f)
    if "--esegui" not in argv:
        if scelte:
            print("Per eseguirle: python3 toolchain/prove-toccate.py --esegui" +
                  (" --cached" if "--cached" in argv else "") +
                  (f" --da {argv[argv.index('--da') + 1]}" if "--da" in argv else ""))
        return 0
    if subprocess.run(["docker", "info"], capture_output=True).returncode:
        print("prove-toccate: Docker non risponde (open -a Docker)", file=sys.stderr)
        return 2
    for n in sorted(scelte):
        c = comando(n, radice)
        if not c:
            print(f"prove-toccate: {n}: nessun comando docker in testa alla suite", file=sys.stderr)
            return 2
        print(f"== {n}", flush=True)
        r = subprocess.run(c, shell=True, capture_output=True, text=True, errors="replace")
        ultime = (r.stdout + r.stderr).strip().splitlines()[-3:]
        print("\n".join("   " + u for u in ultime))
        if r.returncode:
            print(f"prove-toccate: {n} FALLITA (uscita {r.returncode})", file=sys.stderr)
            return 1
    print(f"prove-toccate: {len(scelte)} suite passate")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
