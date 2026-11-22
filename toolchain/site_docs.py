"""Sito di documentazione di www.mosaikcfw.org, organizzato come i siti di MkDocs (Knulli): colonna
a sinistra con le sezioni, indice della pagina a destra, pagina precedente e successiva, ricerca,
tema chiaro e scuro, inglese e italiano.

Le guide vengono dalla wiki di GitHub (un solo manuale: la wiki resta la fonte, il sito la mostra),
la pagina Sistemi dai JSON degli emulatori e dal catalogo dello Store, le pagine dei dispositivi da
site/<lingua>/<pagina>.html. I link alle immagini da scaricare sono quelli dell'ultima versione
STABILE su GitHub, non della build locale: il sito si carica solo per le stabili (22 novembre: era
rimasto online un sito con i link alla v0.9.95, che non esiste).

Chiamato da toolchain/make-site.py (build_docs). Serve il modulo Python markdown.
"""
import html
import json
import os
import re
import subprocess
import sys
import urllib.request

import markdown

REPO = "diapolon/mosaik_cfw"


def stable_release(top, fallback):
    """Ultima stabile su GitHub: versione, file delle immagini con dimensioni e SHA-256 (da build-info.txt)."""
    try:
        out = subprocess.run(["gh", "release", "view", "-R", REPO, "--json", "tagName,assets,publishedAt"],
                             capture_output=True, text=True, check=True).stdout
        j = json.loads(out)
        ver = j["tagName"].lstrip("v")
        info = {"VERSION": ver, "DATE": f"{j['publishedAt'][8:10]}/{j['publishedAt'][5:7]}/{j['publishedAt'][:4]}"}
        sizes = {a["name"]: a["size"] for a in j["assets"]}
        bi = subprocess.run(["gh", "release", "download", "v" + ver, "-R", REPO, "-p", "build-info.txt", "-O", "-"],
                            capture_output=True, text=True, check=True).stdout
        sha = {}
        for line in bi.splitlines():
            m = re.match(r"([0-9a-f]{64})\s+\S*?(mosaik-[^ ]+\.img\.gz)", line.strip())
            if m: sha[m.group(2)] = m.group(1)
        for key, model in (("SMART", "smartpro"), ("BRICK", "brickpro")):
            name = f"mosaik-{ver}-{model}.img.gz"
            if name not in sizes: raise RuntimeError(f"{name} non è nella Release v{ver}")
            n = sizes[name]
            info |= {f"{key}_FILE": name, f"{key}_SIZE": f"{n // (1 << 20)} MB", f"{key}_SHA": sha.get(name, "?"),
                     f"{key}_URL": f"https://github.com/{REPO}/releases/download/v{ver}/{name}"}
        return info
    except Exception as e:   # senza rete o senza gh: la build locale, detto chiaramente
        print(f"ATTENZIONE: ultima stabile non letta da GitHub ({e}); il sito usa la build locale {fallback['VERSION']}: non caricarlo così")
        return fallback


def wiki_dir(top):
    """Clone della wiki in dist/wiki, aggiornato a ogni costruzione."""
    d = os.path.join(top, "dist", "wiki")
    if os.path.isdir(os.path.join(d, ".git")):
        subprocess.run(["git", "-C", d, "pull", "-q", "--ff-only"], check=False)
    else:
        subprocess.run(["git", "clone", "-q", "--depth", "1", f"https://github.com/{REPO}.wiki.git", d], check=True)
    return d


def slugify(value, separator="-"):
    """Ancore come quelle di GitHub: minuscole, senza punteggiatura, spazi in trattini."""
    value = re.sub(r"[^\w\- ]", "", value.strip().lower(), flags=re.UNICODE)
    return re.sub(r" ", separator, value)


def text_of(h):
    return html.unescape(re.sub(r"<[^>]+>", " ", h))


def build_docs(top, src, web, conf, layout, values, fill):
    """Pagine di documentazione per ogni lingua di site.json ("docs"), più la ricerca."""
    wiki = wiki_dir(top)
    # pagine della wiki di ogni lingua -> pagina del sito (stesso nome nelle due lingue)
    page_of = {}
    for lang, c in conf.items():
        for sec in c["docs"]:
            for p in sec["pages"]:
                if p.get("wiki"): page_of[p["wiki"]] = (lang, p["slug"])
        for w in c.get("wiki_home", []): page_of[w] = (lang, "index")
    shots_base = f"https://raw.githubusercontent.com/{REPO}/main/docs/images/"
    for lang, c in conf.items():
        up = "../" if c["prefix"] else ""
        oc = conf[c["other"]]
        flat = [p for sec in c["docs"] for p in sec["pages"]]
        search = []

        def link(target, anchor):
            if target not in page_of: return None
            tl, slug = page_of[target]
            href = ("" if tl == lang else (up + conf[tl]["prefix"] if conf[tl]["prefix"] else up)) + ("index" if slug == "index" else slug) + ".html"
            return href + (anchor or "")

        for i, p in enumerate(flat):
            if p.get("wiki"):
                md_text = open(os.path.join(wiki, p["wiki"] + ".md"), encoding="utf-8").read()
                md_text = md_text.replace(shots_base + "it/", up + "assets/shots/it/").replace(shots_base, up + "assets/shots/en/")
                md_text = re.sub(r"\]\(([A-Za-z0-9\-]+)(#[^)\s]*)?\)",
                                 lambda m: "](" + (link(m.group(1), m.group(2)) or (m.group(1) + (m.group(2) or ""))) + ")", md_text)
                # come su GitHub: un avviso (> ...) finisce alla prima riga che non comincia con >, anche
                # senza riga vuota (Python-Markdown la terrebbe nell'avviso, il passo dopo ci finiva dentro)
                lines, fixed = md_text.split("\n"), []
                for k, ln in enumerate(lines):
                    fixed.append(ln)
                    nxt = lines[k + 1] if k + 1 < len(lines) else ""
                    if ln.lstrip().startswith(">") and nxt.strip() and not nxt.lstrip().startswith(">"):
                        fixed.append("")
                md_text = "\n".join(fixed)
                md = markdown.Markdown(extensions=["tables", "fenced_code", "sane_lists", "toc"],
                                       extension_configs={"toc": {"slugify": slugify, "toc_depth": "2"}})
                body = md.convert(md_text)
                toc = [(t["id"], t["name"]) for t in md.toc_tokens]
            else:
                body = fill(open(os.path.join(src, lang, p["slug"] + ".html"), encoding="utf-8").read(), {"SHOTS": up + f"assets/shots/{lang}/"})
                if p["slug"] == "systems": body = body.replace("{{SYSTEMS}}", systems_table(top, lang, c))
                toc = [(m.group(1), text_of(m.group(2))) for m in re.finditer(r'<h2 id="([^"]+)">(.*?)</h2>', body)]
            body = re.sub(r"<table>", '<div class="table"><table>', body).replace("</table>", "</table></div>")
            # colonna a sinistra: sezioni con le loro pagine
            side = f'<div class="search"><input type="search" placeholder="{c["search"]}" aria-label="{c["search"]}" data-lang="{lang}"><ol class="results"></ol></div>'
            # sul telefono le sezioni stanno dietro un pulsante (site.js), sennò spingono la pagina in fondo
            side += f'<button type="button" class="menu-btn" aria-expanded="false">{c["docs_label"]}: {p["title"]}</button><div class="menu">'
            for sec in c["docs"]:
                side += f'<p class="sec">{sec["title"]}</p><ul>'
                for q in sec["pages"]:
                    side += f'<li><a href="{q["slug"]}.html"' + (' aria-current="page"' if q is p else "") + f'>{q["title"]}</a></li>'
                side += "</ul>"
            side += "</div>"
            pager = '<nav class="pager">'
            if i > 0: pager += f'<a class="prev" href="{flat[i - 1]["slug"]}.html"><small>{c["prev"]}</small>{flat[i - 1]["title"]}</a>'
            if i + 1 < len(flat): pager += f'<a class="next" href="{flat[i + 1]["slug"]}.html"><small>{c["next"]}</small>{flat[i + 1]["title"]}</a>'
            pager += "</nav>"
            toc_html = ""
            if len(toc) > 1:
                toc_html = f'<aside class="toc"><p class="sec">{c["on_page"]}</p><ul>' + "".join(f'<li><a href="#{a}">{html.escape(n)}</a></li>' for a, n in toc) + "</ul></aside>"
            edit = ""
            if p.get("wiki"):
                edit = f'<p class="small">{c["from_wiki"]} <a href="https://github.com/{REPO}/wiki/{p["wiki"]}">{p["wiki"].replace("-", " ")}</a></p>'
            page_body = (f'<div class="doc"><nav class="side" aria-label="{c["docs_label"]}">{side}</nav>'
                         f'<article class="article"><h1>{p["title"]}</h1>{body}{edit}{pager}</article>{toc_html}</div>')
            search.append({"t": p["title"], "u": p["slug"] + ".html", "x": re.sub(r"\s+", " ", text_of(body))[:4000]})
            write_page(web, c, oc, lang, p["slug"], page_body, p["title"], layout, values, fill, conf)
        with open(os.path.join(web, "assets", f"search-{lang}.js"), "w", encoding="utf-8") as f:
            f.write("window.MOSAIK_SEARCH=" + json.dumps(search, ensure_ascii=False) + ";\n")


def write_page(web, c, oc, lang, slug, body, title, layout, values, fill, conf):
    up = "../" if c["prefix"] else ""
    links = ""
    for k, v in c["nav"].items():
        href = k[1:] if k.startswith("@") else k + ".html"
        cur = k == slug or (k == c["docs_start"] and slug not in ("index", "download"))
        links += f'<a href="{href}"' + (' aria-current="page"' if cur else "") + f">{v}</a>"
    other_href = ("../" if c["prefix"] else oc["prefix"]) + slug + ".html"
    links += f'<a class="lang" href="{other_href}" lang="{c["other"]}" hreflang="{c["other"]}">{c["other_label"]}</a>'
    page_path = c["prefix"] + ("" if slug == "index" else slug + ".html")
    other_path = oc["prefix"] + ("" if slug == "index" else slug + ".html")
    extra = {"LANG": lang, "OTHER_LANG": c["other"], "ASSETS": up + "assets/", "SHOTS": up + f"assets/shots/{lang}/",
             "HOME": "index.html", "SELF": page_path, "OTHER": other_path, "DESCRIPTION": c["description"],
             "PAGE_TITLE": c["site"] if slug == "index" else f"{title} · Mosaik"}
    page = layout.replace("{{BODY}}", body).replace("{{FOOTER}}", c["footer"]).replace("{{TOPLINKS}}", links)
    page = fill(page, extra)
    assert "{{" not in page, f"segnaposto non sostituito in {lang}/{slug}"
    path = os.path.join(web, c["prefix"], slug + ".html")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w", encoding="utf-8").write(page)


def systems_table(top, lang, c):
    """Tabella dei sistemi dai JSON degli emulatori: cartella, formati, BIOS (dalla guida), pacchetto dello Store."""
    emus = {}
    for d in (os.path.join(top, "frontend", "emulators"),):
        for n in sorted(os.listdir(d)):
            if n.endswith(".json"): emus[n[:-5]] = json.load(open(os.path.join(d, n), encoding="utf-8"))
    fly = json.load(open(os.path.join(top, "frontend", "package", "flycast", "dc.json"), encoding="utf-8"))
    cat_path = os.path.join(top, "dist", "site", "store", "store.json")
    pkg_of = {}
    if os.path.exists(cat_path):
        cat = json.load(open(cat_path, encoding="utf-8"))
        for k in ("packages", "packages2"):
            for p in cat.get(k, []):
                for e in p.get("emulators", []):
                    pkg_of.setdefault(e, []).append(p)
    computers = {"zx", "msx", "c64", "dos", "amiga", "scummvm"}
    groups = {"arcade": [], "console": [], "computer": []}
    rows = list(emus.items()) + [("dc-flycast", fly)]
    for eid, e in rows:
        if e.get("planned"): continue
        g = "computer" if eid in computers else "console" if e.get("kind") == "console" else "arcade"
        groups[g].append((eid, e))

    def guide_line(e, tag):
        gl = e.get("guide", {}).get(lang) or e.get("guide", {}).get("en") or []
        for line in gl:
            if line.startswith(tag + " "): return line[len(tag) + 1:]
        return ""

    out = ""
    for g in ("arcade", "console", "computer"):
        out += f'<h2 id="{g}">{c["sys_groups"][g]}</h2><div class="table"><table><thead><tr>'
        out += "".join(f"<th>{h}</th>" for h in c["sys_cols"]) + "</tr></thead><tbody>"
        for eid, e in sorted(groups[g], key=lambda x: (x[1].get("order", 100), x[1].get("name", ""))):
            base = "dc" if eid == "dc-flycast" else eid
            pk = [p for p in pkg_of.get(base, []) if (p["id"] == "flycast") == (eid == "dc-flycast")]
            pkg = ""
            if pk:
                p = pk[0]
                pkg = html.escape(p["name"])
                if p.get("status") == "experimental": pkg += f' <span class="badge">{c["experimental"]}</span>'
                if p.get("models"): pkg += f' <span class="badge">{" ".join(m.replace("smartpro", "Smart Pro").replace("brickpro", "Brick Pro") for m in p["models"])}</span>'
            folder = guide_line(e, "@folder") or f'{e.get("roms", "")} ({" ".join("." + x for x in e.get("extensions", []))})'
            bios = guide_line(e, "@chip") or "–"
            out += (f'<tr><td>{html.escape(e.get("name", eid))}</td><td>{pkg}</td>'
                    f'<td><code>{html.escape(folder.split(" (")[0])}</code> {html.escape(folder[len(folder.split(" (")[0]):].strip())}</td>'
                    f'<td>{html.escape(bios)}</td></tr>')
        out += "</tbody></table></div>"
    return out
