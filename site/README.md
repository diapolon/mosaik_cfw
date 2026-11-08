# Sito di Arcade

`index.html` e `assets/` sono il modello. Il sito vero si genera, dopo `sh m2/build-m2.sh`, con:

    python3 toolchain/make-site.py

e finisce in `dist/site`: solo file statici, da caricare nella radice di
**arcade.koghisoft.it** (per ora; l'indirizzo della cartella `update/` è nel file `UPDATE_URL`
ed è compilato nel launcher). Serve HTTPS con un certificato valido.

Per pubblicare una versione: aumentare `VERSION`, `sh m2/build-m2.sh`, `make-site.py`, caricare
`dist/site` sostituendo il contenuto precedente. Conviene caricare `update/update.txt` e
`update/update.sig` per ultimi, così una console non legge un elenco di file non ancora arrivati.

La firma usa la chiave privata `~/.config/arcade-update/update-key.pem`, che non va mai caricata
né messa nel repository.
