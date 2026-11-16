#!/bin/sh
# Prove di urlget_core (app dello Store "Scarica file") con un server HTTP locale: indirizzi, nomi,
# metadati di archive.org, blocco degli indirizzi privati, download con controlli, annullamento,
# reindirizzamenti:
#   docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-urlget.sh
set -e
apk add -q build-base curl-dev openssl-dev python3 >/dev/null
T=$(mktemp -d)
trap 'kill $SRV 2>/dev/null; rm -rf "$T"' EXIT
mkdir -p "$T/fix/download/test-item" "$T/fix/metadata" "$T/dest"
python3 - "$T/fix" <<'PY'
import hashlib, json, os, sys
d = sys.argv[1]
data = os.urandom(3 * 1024 * 1024)
open(f"{d}/download/test-item/Game One.zip", "wb").write(data)
open(f"{d}/expected.txt", "w").write(f"sha1={hashlib.sha1(data).hexdigest()}\nmd5={hashlib.md5(data).hexdigest()}\nsize={len(data)}\n")
meta = {"files": [{"name": "Game One.zip", "source": "original", "size": str(len(data)), "md5": hashlib.md5(data).hexdigest(), "sha1": hashlib.sha1(data).hexdigest()},
                  {"name": "item_meta.xml", "source": "original", "format": "Metadata"}]}
json.dump(meta, open(f"{d}/metadata/test-item", "w"))
PY
cat > "$T/srv.py" <<'PY'
import http.server, sys
root = sys.argv[1]
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(s, *a, **k): super().__init__(*a, directory=root, **k)
    def do_GET(s):
        if s.path == "/redir":
            s.send_response(302); s.send_header("Location", "/download/test-item/Game%20One.zip"); s.end_headers(); return
        if s.path.startswith("/metadata/"):
            s.path = s.path
        super().do_GET()
    def log_message(s, *a): pass
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
PY
python3 "$T/srv.py" "$T/fix" > "$T/port" &
SRV=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s "$T/port" ] && break; sleep 0.3; done
PORT=$(cat "$T/port")
F=/trimui/frontend/src
cc -std=c11 -O2 -g -Wall -Wextra -D_DEFAULT_SOURCE -o "$T/urlget_test" $F/apps/urlget_test.c $F/apps/urlget_core.c $F/common/json.c -lcurl -lcrypto -lm
"$T/urlget_test" "http://127.0.0.1:$PORT" "$T/fix" "$T/dest"
