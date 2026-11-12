#include "settings.h"
#include "json.h"
#include "filters.h"
#include "durable.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

const led_color_t LED_COLORS[] = {
    { "led.theme", 0 }, { "led.white", 0xFFFFFF }, { "led.red", 0xFF0000 }, { "led.orange", 0xFF6000 },
    { "led.yellow", 0xFFD000 }, { "led.green", 0x00FF30 }, { "led.cyan", 0x00E0FF }, { "led.blue", 0x0030FF },
    { "led.purple", 0x9000FF }, { "led.pink", 0xFF2090 },
};
const int NLED_COLORS = sizeof LED_COLORS / sizeof *LED_COLORS;

#include "timezones.h"   /* tutti i fusi del database tz: toolchain/make-timezones.py */
const int NTIMEZONES = sizeof TIMEZONES / sizeof *TIMEZONES;

int settings_tz_index(const settings_t *s) {
    for (int i = 0; i < NTIMEZONES; i++)
        if (!strcmp(TIMEZONES[i].id, s->tz)) return i;
    for (int i = 0; i < NTIMEZONES; i++)   /* nome sconosciuto: Europe/Rome, come prima */
        if (!strcmp(TIMEZONES[i].id, "Europe/Rome")) return i;
    return 0;
}

void settings_apply_tz(const settings_t *s) {
    const char *tz = TIMEZONES[settings_tz_index(s)].posix;
    setenv("TZ", tz, 1);
    tzset();
    /* anche per gli script del sistema (wifi-ctl, wifi-event): i loro registri con la stessa ora
       del launcher, chiunque li avvii. Solo sulla console c'è /run */
    FILE *f = fopen("/run/arcade-tz", "w");
    if (f) { fprintf(f, "%s\n", tz); fclose(f); }
}

void settings_save_tz_for_boot(const settings_t *s, const char *root) {
    /* copia sulla SD per le righe scritte all'avvio, prima che parta il launcher */
    const char *tz = TIMEZONES[settings_tz_index(s)].posix;
    char p[1100], cur[64] = "";
    snprintf(p, sizeof p, "%s/System/fuso-orario.txt", root);
    FILE *f = fopen(p, "r");
    if (f) { if (!fgets(cur, sizeof cur, f)) cur[0] = 0; fclose(f); }
    cur[strcspn(cur, "\n")] = 0;
    if (!strcmp(cur, tz)) return;
    if ((f = fopen(p, "w"))) { fprintf(f, "%s\n", tz); fclose(f); }
}

void settings_defaults(settings_t *s) {
    memset(s, 0, sizeof *s);
    strcpy(s->theme, "tessera");
    strcpy(s->variant, "light");
    strcpy(s->device, "smartpro");
    strcpy(s->lang, "it");
    s->brightness = 8;
    s->volume = 12;
    s->cpu_profile = CPU_BALANCED;
    s->autosleep_min = 0;
    s->autosuspend_min = 5;
    s->update_notify = true;
    s->key_f1 = XK_SAVE;
    s->key_f2 = XK_LOAD;
    s->aspect = ASPECT_ORIGINAL;
    strcpy(s->filter, "nitido");
    s->bezel = false;
    s->dup_mode = 1;
    s->resume_last = false;
    s->autosave_exit = false;
    s->box_covers = true;
    s->speaker_rest = 0;
    s->wifi = true;
    s->music_vol = 5;
    strcpy(s->tz, "Europe/Rome");
    s->led_mode = 1;
    s->led_color = 0;
    s->led_bright = 5;
    s->home_size = 1;
}

static void copy(char *dst, size_t n, const char *src) {
    if (src) { strncpy(dst, src, n - 1); dst[n - 1] = 0; }
}

#define IN_RANGE(field, lo, hi, def) do { if (s->field < (lo) || s->field > (hi)) s->field = (def); } while (0)

bool settings_load(settings_t *s, const char *root) {
    char path[1024];
    settings_defaults(s);
    snprintf(path, sizeof path, "%s/System/settings.json", root);
    jval *j = json_load(path);
    if (!j) return false;
    copy(s->theme, sizeof s->theme, json_str(j, "theme", NULL));
    copy(s->variant, sizeof s->variant, json_str(j, "variant", NULL));
    copy(s->device, sizeof s->device, json_str(j, "device", NULL));
    copy(s->lang, sizeof s->lang, json_str(j, "language", NULL));
    s->brightness = json_int(j, "brightness", s->brightness);
    s->volume = json_int(j, "volume", s->volume);
    s->cpu_profile = json_int(j, "cpu_profile", s->cpu_profile);
    s->autosleep_min = json_int(j, "autosleep_min", s->autosleep_min);
    s->autosuspend_min = json_int(j, "autosuspend_min", s->autosuspend_min);
    s->update_channel = json_int(j, "update_channel", 0) ? 1 : 0;
    s->update_notify = json_bool(j, "update_notify", s->update_notify);
    /* le schede già in uso non hanno la voce: per loro il primo avvio è passato */
    s->setup_done = json_bool(j, "setup_done", true);
    s->key_f1 = json_int(j, "key_f1", s->key_f1);
    s->key_f2 = json_int(j, "key_f2", s->key_f2);
    s->key_home = json_int(j, "key_home", s->key_home);
    s->saver = json_int(j, "screensaver", s->saver) % 5;
    s->aspect = json_int(j, "aspect", s->aspect);
    /* il filtro era un numero (0 nitido, 1 morbido, 2 scanline): resta leggibile */
    jval *fv = json_get(j, "filter");
    if (fv && fv->t == J_NUM) copy(s->filter, sizeof s->filter, filter_legacy_id(json_to_int(fv->n, 0)));
    else copy(s->filter, sizeof s->filter, json_str(j, "filter", NULL));
    s->bezel = json_bool(j, "bezel", s->bezel);
    s->dup_mode = json_int(j, "duplicates", s->dup_mode);
    s->sort_mode = json_int(j, "sort", s->sort_mode) % 4;
    s->show_missing = json_bool(j, "show_missing", s->show_missing);
    s->resume_last = json_bool(j, "resume_last", s->resume_last);
    s->autosave_exit = json_bool(j, "autosave_exit", s->autosave_exit);
    s->box_covers = json_bool(j, "box_covers", s->box_covers);
    s->speaker_rest = json_int(j, "speaker_rest", s->speaker_rest);
    s->wifi = json_bool(j, "wifi", s->wifi);
    s->ext_p1 = json_bool(j, "external_p1", s->ext_p1);
    s->music_vol = json_int(j, "music_volume", s->music_vol);
    copy(s->tz, sizeof s->tz, json_str(j, "timezone", NULL));
    s->led_mode = json_int(j, "led_mode", s->led_mode) & 3;
    s->led_color = json_int(j, "led_color", s->led_color);
    if (s->led_color < 0 || s->led_color >= NLED_COLORS) s->led_color = 0;
    s->led_bright = json_int(j, "led_brightness", s->led_bright);
    s->grid_columns = json_int(j, "grid_columns", 0);
    if (s->grid_columns != 0 && (s->grid_columns < 3 || s->grid_columns > 7)) s->grid_columns = 0;
    s->home_size = json_int(j, "home_size", s->home_size);
    if (s->home_size < 0 || s->home_size > 2) s->home_size = 1;
    s->log_level = json_int(j, "debug_level", 0);
    if (s->log_level < 0 || s->log_level > 3) s->log_level = 0;
    if (s->led_bright < 1 || s->led_bright > 10) s->led_bright = 5;
    if (s->music_vol < 0 || s->music_vol > 10) s->music_vol = 5;
    /* file modificato a mano o rovinato: questi valori fanno da indici nei menu */
    IN_RANGE(brightness, 1, 10, 8);
    IN_RANGE(key_f1, 0, 3, XK_SAVE);
    IN_RANGE(key_f2, 0, 3, XK_LOAD);
    IN_RANGE(key_home, 0, 2, XH_LAUNCHER);
    IN_RANGE(volume, 0, 20, 12);
    IN_RANGE(cpu_profile, 0, 2, CPU_BALANCED);
    IN_RANGE(sort_mode, 0, 3, 0);
    IN_RANGE(saver, 0, 4, 0);
    IN_RANGE(aspect, 0, 2, ASPECT_ORIGINAL);
    IN_RANGE(dup_mode, 0, 32, 1);
    IN_RANGE(autosleep_min, 0, 1000, 0);
    IN_RANGE(autosuspend_min, 0, 1000, 5);
    IN_RANGE(speaker_rest, 0, 2, 0);
    json_free(j);
    return true;
}

bool settings_save(const settings_t *s, const char *root) {
    char path[1024], tmp[1040];
    snprintf(path, sizeof path, "%s/System/settings.json", root);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    int written = fprintf(f,
            "{\n"
            "  \"theme\": \"%s\",\n"
            "  \"variant\": \"%s\",\n"
            "  \"device\": \"%s\",\n"
            "  \"language\": \"%s\",\n"
            "  \"brightness\": %d,\n"
            "  \"volume\": %d,\n"
            "  \"cpu_profile\": %d,\n"
            "  \"autosleep_min\": %d,\n"
            "  \"autosuspend_min\": %d,\n"
            "  \"update_channel\": %d,\n"
            "  \"update_notify\": %s,\n"
            "  \"setup_done\": %s,\n"
            "  \"key_f1\": %d,\n"
            "  \"key_f2\": %d,\n"
            "  \"key_home\": %d,\n"
            "  \"screensaver\": %d,\n"
            "  \"aspect\": %d,\n"
            "  \"filter\": \"%s\",\n"
            "  \"bezel\": %s,\n"
            "  \"duplicates\": %d,\n"
            "  \"sort\": %d,\n"
            "  \"show_missing\": %s,\n"
            "  \"resume_last\": %s,\n"
            "  \"autosave_exit\": %s,\n"
            "  \"box_covers\": %s,\n"
            "  \"speaker_rest\": %d,\n"
            "  \"wifi\": %s,\n"
            "  \"external_p1\": %s,\n"
            "  \"music_volume\": %d,\n"
            "  \"timezone\": \"%s\",\n"
            "  \"led_mode\": %d,\n"
            "  \"led_color\": %d,\n"
            "  \"led_brightness\": %d,\n"
            "  \"grid_columns\": %d,\n"
            "  \"home_size\": %d,\n"
            "  \"debug_level\": %d\n"
            "}\n",
            s->theme, s->variant, s->device, s->lang, s->brightness, s->volume, s->cpu_profile,
            s->autosleep_min, s->autosuspend_min, s->update_channel, s->update_notify ? "true" : "false", s->setup_done ? "true" : "false", s->key_f1, s->key_f2, s->key_home, s->saver, s->aspect, s->filter, s->bezel ? "true" : "false", s->dup_mode, s->sort_mode, s->show_missing ? "true" : "false", s->resume_last ? "true" : "false",
            s->autosave_exit ? "true" : "false",
            s->box_covers ? "true" : "false", s->speaker_rest,
            s->wifi ? "true" : "false", s->ext_p1 ? "true" : "false", s->music_vol, s->tz, s->led_mode, s->led_color, s->led_bright, s->grid_columns, s->home_size, s->log_level);
    bool ok = written > 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    /* solo un file scritto per intero prende il posto del precedente */
    if (!ok) { remove(tmp); return false; }
    return durable_replace(tmp, path) == 0;
}

/* misure dello schermo per modello; "r36s" (640x480) è il prototipo del porting (docs/analisi-r36s.md) */
void device_size(const char *device, int *w, int *h) {
    if (!strcmp(device, "brickpro")) { *w = 1024; *h = 768; }
    else if (!strcmp(device, "r36s")) { *w = 640; *h = 480; }
    else { *w = 1280; *h = 720; }
}

const char *device_label(const char *device) {
    return !strcmp(device, "brickpro") ? "Brick Pro 1024×768" : !strcmp(device, "r36s") ? "R36S 640×480" : "Smart Pro 1280×720";
}
