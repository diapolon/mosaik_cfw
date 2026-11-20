/* Prove di urlget_core con un piccolo server locale: vedi m2/test-urlget.sh.
   uso: urlget_test BASE_URL CARTELLA_FILE_DI_PROVA CARTELLA_DI_ARRIVO */
#include "urlget_core.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static int fails, total;
#define CHECK(c, ...) do { total++; if (!(c)) { fails++; printf("  NO: " __VA_ARGS__); printf("\n"); } } while (0)

static bool exists(const char *dir, const char *name) {
    char p[1600];
    struct stat sb;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return stat(p, &sb) == 0;
}

static int leftovers(const char *dir) {   /* file ".part" rimasti */
    int n = 0;
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d));) if (strstr(e->d_name, ".part")) n++;
    if (d) closedir(d);
    return n;
}

static bool cancel_at_half(void *ud, long long done, long long total) {
    (void)ud;
    return total > 0 && done > total / 4;
}

static void hex_of(const char *file, const char *key, char *out, size_t n) {
    /* legge "chiave=valore" da expected.txt */
    FILE *f = fopen(file, "r");
    char line[256];
    out[0] = 0;
    while (f && fgets(line, sizeof line, f)) {
        size_t kl = strlen(key);
        if (!strncmp(line, key, kl) && line[kl] == '=') { snprintf(out, n, "%s", line + kl + 1); out[strcspn(out, "\r\n")] = 0; }
    }
    if (f) fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    const char *base = argv[1], *fix = argv[2], *dest = argv[3];
    setenv("URLGET_TEST", "1", 1);
    char url[800], exp[900], sha[64], md5[64], sz[32];
    snprintf(exp, sizeof exp, "%s/expected.txt", fix);
    hex_of(exp, "sha1", sha, sizeof sha);
    hex_of(exp, "md5", md5, sizeof md5);
    hex_of(exp, "size", sz, sizeof sz);
    long long size = atoll(sz);

    puts("indirizzi");
    ug_link_t l;
    CHECK(ug_classify("https://archive.org/download/some-item/", &l) == UG_ITEM && !strcmp(l.item, "some-item") &&
          !strcmp(l.meta, "https://archive.org/metadata/some-item"), "download/<id>/ e' un item");
    CHECK(ug_classify("https://archive.org/details/some-item", &l) == UG_ITEM, "details");
    CHECK(ug_classify("https://www.archive.org/metadata/x_1.y", &l) == UG_ITEM && !strcmp(l.origin, "https://archive.org"), "www e metadata");
    CHECK(ug_classify("https://archive.org/download/some-item/a%20b.zip", &l) == UG_FILE, "file dentro un item e' un file");
    CHECK(ug_classify("https://example.com/x/y.zip", &l) == UG_FILE, "file diretto");
    CHECK(ug_classify("https://archive.org/download/../etc/", &l) == UG_BAD, "id con ..");
    CHECK(ug_classify("ftp://example.com/a.zip", &l) == UG_BAD, "ftp");
    CHECK(ug_classify("https://user:pw@example.com/a.zip", &l) == UG_BAD, "credenziali nell'indirizzo");
    CHECK(ug_classify("https://exa mple.com/a", &l) == UG_BAD, "spazio");
    CHECK(ug_classify("file:///etc/passwd", &l) == UG_BAD, "file://");
    CHECK(ug_classify("", &l) == UG_BAD, "vuoto");
    char b[400];
    ug_item_url(&(ug_link_t){ .kind = UG_ITEM, .origin = "https://archive.org", .item = "it" }, "dir/Game (1) #2.zip", b, sizeof b);
    CHECK(!strcmp(b, "https://archive.org/download/it/dir/Game%20%281%29%20%232.zip"), "codifica: %s", b);
    CHECK(ug_url_name("https://x.org/a/Game%20One.zip?x=1#f", b, sizeof b) && !strcmp(b, "Game One.zip"), "nome dall'indirizzo: %s", b);

    puts("nomi");
    CHECK(ug_safe_name("../../etc/passwd", b, sizeof b) && !strcmp(b, "passwd"), "percorso: %s", b);
    CHECK(!ug_safe_name(".hidden", b, sizeof b), "punto iniziale");
    CHECK(!ug_safe_name("..", b, sizeof b) && !ug_safe_name("a/", b, sizeof b) && !ug_safe_name("", b, sizeof b), "vuoti");
    CHECK(ug_safe_name("a:b?.zip", b, sizeof b) && !strcmp(b, "a_b_.zip"), "caratteri FAT: %s", b);
    CHECK(ug_safe_name("x. . ", b, sizeof b) && !strcmp(b, "x"), "spazi e punti finali: %s", b);
    char lng[700];
    memset(lng, 'a', 400);
    strcpy(lng + 400, ".gba");
    CHECK(ug_safe_name(lng, b, sizeof b) && strlen(b) == UG_NAME_MAX && !strcmp(b + UG_NAME_MAX - 4, ".gba"), "nome lungo: %zu", strlen(b));
    char u8[700];
    for (int i = 0; i < 120; i++) memcpy(u8 + 2 * i, "\xc3\xa8", 2);   /* "è" x120 = 240 byte */
    strcpy(u8 + 240, ".zip");
    CHECK(ug_safe_name(u8, b, sizeof b), "utf8 lungo");
    bool okutf = true;
    for (size_t i = 0; b[i]; i++) if (((unsigned char)b[i] & 0xC0) == 0x80 && (i == 0 || ((unsigned char)b[i - 1] < 0x80))) okutf = false;
    CHECK(okutf && strlen(b) <= UG_NAME_MAX, "utf8 non tagliato a meta'");

    puts("metadati");
    ug_file_t *fl = NULL;
    const char *js = "{\"files\":["
        "{\"name\":\"Game One.zip\",\"source\":\"original\",\"size\":\"1234\",\"md5\":\"d41d8cd98f00b204e9800998ecf8427e\",\"sha1\":\"da39a3ee5e6b4b0d3255bfef95601890afd80709\",\"format\":\"ZIP\"},"
        "{\"name\":\"sub/Game Two.bin\",\"source\":\"original\",\"size\":99,\"md5\":\"zz\"},"
        "{\"name\":\"Game One.zip_thumb.jpg\",\"source\":\"derivative\",\"size\":\"5\"},"
        "{\"name\":\"item_meta.xml\",\"source\":\"original\",\"format\":\"Metadata\"},"
        "{\"name\":\"item_files.xml\",\"source\":\"original\"},"
        "{\"name\":\"../evil\",\"source\":\"original\"},"
        "{\"name\":\".hidden\",\"source\":\"original\"},"
        "{\"name\":\"priv.zip\",\"source\":\"original\",\"private\":\"true\"}]}";
    int n = ug_parse_meta(js, &fl);
    CHECK(n == 2, "attesi 2 file, trovati %d", n);
    if (n == 2) {
        CHECK(!strcmp(fl[0].name, "Game One.zip") && fl[0].size == 1234 && strlen(fl[0].sha1) == 40 && strlen(fl[0].md5) == 32, "primo file");
        CHECK(!strcmp(fl[1].name, "Game Two.bin") && !strcmp(fl[1].path, "sub/Game Two.bin") && fl[1].size == 99 && !fl[1].md5[0], "secondo file: md5 non valido scartato");
    }
    free(fl);
    CHECK(ug_parse_meta("non json", &fl) == 0 && ug_parse_meta("{}", &fl) == 0 && ug_parse_meta(NULL, &fl) == 0, "json non valido");

    puts("indirizzi bloccati");
    struct sockaddr_in a4 = { .sin_family = AF_INET };
    const char *blocked4[] = { "127.0.0.1", "10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.1.1", "169.254.169.254", "100.64.0.1", "0.0.0.0", "224.0.0.1" };
    const char *open4[] = { "8.8.8.8", "172.32.0.1", "100.128.0.1", "93.184.216.34" };
    setenv("URLGET_TEST", "0", 1);
    for (size_t i = 0; i < sizeof blocked4 / sizeof *blocked4; i++) {
        inet_pton(AF_INET, blocked4[i], &a4.sin_addr);
        CHECK(ug_addr_blocked((struct sockaddr *)&a4), "%s deve essere bloccato", blocked4[i]);
    }
    for (size_t i = 0; i < sizeof open4 / sizeof *open4; i++) {
        inet_pton(AF_INET, open4[i], &a4.sin_addr);
        CHECK(!ug_addr_blocked((struct sockaddr *)&a4), "%s deve passare", open4[i]);
    }
    struct sockaddr_in6 a6 = { .sin6_family = AF_INET6 };
    const char *blocked6[] = { "::1", "::", "fe80::1", "fc00::1", "fd12::1", "::ffff:127.0.0.1", "::ffff:192.168.0.1", "ff02::1" };
    for (size_t i = 0; i < sizeof blocked6 / sizeof *blocked6; i++) {
        inet_pton(AF_INET6, blocked6[i], &a6.sin6_addr);
        CHECK(ug_addr_blocked((struct sockaddr *)&a6), "%s deve essere bloccato", blocked6[i]);
    }
    inet_pton(AF_INET6, "2606:4700:4700::1111", &a6.sin6_addr);
    CHECK(!ug_addr_blocked((struct sockaddr *)&a6), "IPv6 pubblico deve passare");
    CHECK(!ug_url_ok("http://example.com/a") && !ug_url_ok("http://127.0.0.1/a"), "http rifiutato fuori dalle prove");
    snprintf(url, sizeof url, "https://127.0.0.1:%s/", strrchr(base, ':') + 1);
    char *mem = NULL;
    CHECK(ug_fetch(url, NULL, 1 << 20, &mem, NULL, NULL) == UG_E_NET && !mem, "https verso loopback rifiutato dalla rete");
    setenv("URLGET_TEST", "1", 1);

    puts("rete");
    snprintf(url, sizeof url, "%s/metadata/test-item", base);
    int r = ug_fetch(url, NULL, UG_META_MAX, &mem, NULL, NULL);
    CHECK(r == UG_OK && mem, "metadati: %d", r);
    n = mem ? ug_parse_meta(mem, &fl) : 0;
    CHECK(n >= 1, "metadati dal server: %d file", n);
    free(mem);
    free(fl);
    mem = NULL;
    CHECK(ug_fetch(url, NULL, 50, &mem, NULL, NULL) == UG_E_BIG, "limite del JSON");
    free(mem);
    snprintf(url, sizeof url, "%s/missing", base);
    CHECK(ug_fetch(url, NULL, 1 << 20, &mem, NULL, NULL) == UG_E_NET, "404");

    snprintf(url, sizeof url, "%s/download/test-item/Game%%20One.zip", base);
    r = ug_download(url, dest, "Game One.zip", size, sha, md5, NULL, NULL, NULL);
    CHECK(r == UG_OK && exists(dest, "Game One.zip") && !leftovers(dest), "download riuscito: %d", r);
    struct stat sb;
    char path[1600];
    snprintf(path, sizeof path, "%s/Game One.zip", dest);
    CHECK(stat(path, &sb) == 0 && sb.st_size == size, "dimensione scritta");
    r = ug_download(url, dest, "Game One.zip", size, sha, md5, NULL, NULL, NULL);
    CHECK(r == UG_E_EXISTS, "non sovrascrive: %d", r);
    r = ug_download(url, dest, "two.zip", size, "0000000000000000000000000000000000000000", "", NULL, NULL, NULL);
    CHECK(r == UG_E_HASH && !exists(dest, "two.zip") && !leftovers(dest), "sha1 sbagliato: %d", r);
    r = ug_download(url, dest, "three.zip", size, "", "00000000000000000000000000000000", NULL, NULL, NULL);
    CHECK(r == UG_E_HASH && !exists(dest, "three.zip") && !leftovers(dest), "md5 sbagliato: %d", r);
    r = ug_download(url, dest, "four.zip", size + 1, sha, md5, NULL, NULL, NULL);
    CHECK(r == UG_E_SIZE && !exists(dest, "four.zip") && !leftovers(dest), "dimensione sbagliata: %d", r);
    r = ug_download(url, dest, "five.zip", 0, "", "", NULL, NULL, NULL);
    CHECK(r == UG_OK && exists(dest, "five.zip"), "senza controlli dichiarati: %d", r);
    r = ug_download(url, dest, "six.zip", size, sha, md5, NULL, cancel_at_half, NULL);
    CHECK(r == UG_E_CANCEL && !exists(dest, "six.zip") && !leftovers(dest), "annullato: %d", r);
    r = ug_download(url, dest, "../escape.zip", 0, "", "", NULL, NULL, NULL);
    CHECK(r != UG_OK && !exists(dest, "../escape.zip"), "nome con percorso rifiutato");
    r = ug_download(url, dest, ".nascosto", 0, "", "", NULL, NULL, NULL);
    CHECK(r != UG_OK, "nome con punto rifiutato");
    r = ug_download(url, dest, "huge.zip", UG_FILE_MAX + 1, "", "", NULL, NULL, NULL);
    CHECK(r == UG_E_BIG, "oltre il limite del FAT");
    r = ug_download(url, dest, "space.zip", 1LL << 50, "", "", NULL, NULL, NULL);
    CHECK(r == UG_E_BIG || r == UG_E_SPACE, "troppo grande");
    snprintf(url, sizeof url, "%s/redir", base);
    r = ug_download(url, dest, "viaredirect.zip", size, sha, md5, NULL, NULL, NULL);
    CHECK(r == UG_OK && exists(dest, "viaredirect.zip"), "reindirizzamento seguito: %d", r);
    snprintf(url, sizeof url, "%s/missing", base);
    r = ug_download(url, dest, "gone.zip", 0, "", "", NULL, NULL, NULL);
    CHECK(r == UG_E_NET && !exists(dest, "gone.zip") && !leftovers(dest), "404 nel download: %d", r);

    /* audit Codex 0.9.100, A100-05: con la misura ignota il controllo iniziale vedeva 0 byte e il
       download poteva consumare anche i 16 MB di riserva della scheda. UG_SMALL_DIR: una scheda
       piccola (tmpfs, test-urlget.sh con --privileged) con poco più della riserva libera */
    const char *small = getenv("UG_SMALL_DIR");
    if (small) {
        snprintf(url, sizeof url, "%s/download/test-item/Game%%20One.zip", base);
        r = ug_download(url, small, "riserva.zip", 0, "", "", NULL, NULL, NULL);
        struct statvfs vs;
        long long freeb = statvfs(small, &vs) == 0 ? (long long)vs.f_bavail * (long long)vs.f_frsize : -1;
        CHECK(r == UG_E_SPACE && !exists(small, "riserva.zip") && !leftovers(small) && freeb >= (16LL << 20) - (64 << 10),
              "misura ignota: la riserva di 16 MB resta libera (%d, %lld byte liberi)", r, freeb);
        r = ug_download(url, "/nessuna/cartella", "x.zip", 0, "", "", NULL, NULL, NULL);
        CHECK(r == UG_E_SPACE || r == UG_E_IO, "spazio non misurabile: rifiutato (%d)", r);
    } else puts("  (riserva della scheda non provata: serve una scheda piccola, UG_SMALL_DIR)");

    printf("%d prove, %d non riuscite\n", total, fails);
    return fails ? 1 : 0;
}
