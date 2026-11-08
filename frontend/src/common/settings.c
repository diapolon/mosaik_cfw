#include "settings.h"
#include "json.h"
#include "filters.h"
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

const timezone_t TIMEZONES[] = {
    { "UTC", "UTC0" },
    { "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Moscow", "MSK-3" },
    { "America/New_York", "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Chicago", "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Denver", "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Sao_Paulo", "<-03>3" },
    { "Asia/Shanghai", "CST-8" },
    { "Asia/Tokyo", "JST-9" },
    { "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
};
const int NTIMEZONES = sizeof TIMEZONES / sizeof *TIMEZONES;

int settings_tz_index(const settings_t *s) {
    for (int i = 0; i < NTIMEZONES; i++)
        if (!strcmp(TIMEZONES[i].id, s->tz)) return i;
    return 3;   /* Europe/Rome */
}

void settings_apply_tz(const settings_t *s) {
    setenv("TZ", TIMEZONES[settings_tz_index(s)].posix, 1);
    tzset();
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
    s->aspect = ASPECT_ORIGINAL;
    strcpy(s->filter, "nitido");
    s->bezel = false;
    s->dup_mode = 1;
    s->resume_last = false;
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
    s->brightness = (int)json_num(j, "brightness", s->brightness);
    s->volume = (int)json_num(j, "volume", s->volume);
    s->cpu_profile = (int)json_num(j, "cpu_profile", s->cpu_profile);
    s->autosleep_min = (int)json_num(j, "autosleep_min", s->autosleep_min);
    s->autosuspend_min = (int)json_num(j, "autosuspend_min", s->autosuspend_min);
    s->saver = (int)json_num(j, "screensaver", s->saver) % 5;
    s->aspect = (int)json_num(j, "aspect", s->aspect);
    /* il filtro era un numero (0 nitido, 1 morbido, 2 scanline): resta leggibile */
    jval *fv = json_get(j, "filter");
    if (fv && fv->t == J_NUM) copy(s->filter, sizeof s->filter, filter_legacy_id((int)fv->n));
    else copy(s->filter, sizeof s->filter, json_str(j, "filter", NULL));
    s->bezel = json_bool(j, "bezel", s->bezel);
    s->dup_mode = (int)json_num(j, "duplicates", s->dup_mode);
    s->sort_mode = (int)json_num(j, "sort", s->sort_mode) % 4;
    s->show_missing = json_bool(j, "show_missing", s->show_missing);
    s->resume_last = json_bool(j, "resume_last", s->resume_last);
    s->wifi = json_bool(j, "wifi", s->wifi);
    s->ext_p1 = json_bool(j, "external_p1", s->ext_p1);
    s->music_vol = (int)json_num(j, "music_volume", s->music_vol);
    copy(s->tz, sizeof s->tz, json_str(j, "timezone", NULL));
    s->led_mode = (int)json_num(j, "led_mode", s->led_mode) & 3;
    s->led_color = (int)json_num(j, "led_color", s->led_color);
    if (s->led_color < 0 || s->led_color >= NLED_COLORS) s->led_color = 0;
    s->led_bright = (int)json_num(j, "led_brightness", s->led_bright);
    s->grid_columns = (int)json_num(j, "grid_columns", 0);
    if (s->grid_columns != 0 && (s->grid_columns < 3 || s->grid_columns > 7)) s->grid_columns = 0;
    s->home_size = (int)json_num(j, "home_size", s->home_size);
    if (s->home_size < 0 || s->home_size > 2) s->home_size = 1;
    s->log_level = (int)json_num(j, "debug_level", 0);
    if (s->log_level < 0 || s->log_level > 3) s->log_level = 0;
    if (s->led_bright < 1 || s->led_bright > 10) s->led_bright = 5;
    if (s->music_vol < 0 || s->music_vol > 10) s->music_vol = 5;
    json_free(j);
    return true;
}

bool settings_save(const settings_t *s, const char *root) {
    char path[1024], tmp[1040];
    snprintf(path, sizeof path, "%s/System/settings.json", root);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f,
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
            "  \"screensaver\": %d,\n"
            "  \"aspect\": %d,\n"
            "  \"filter\": \"%s\",\n"
            "  \"bezel\": %s,\n"
            "  \"duplicates\": %d,\n"
            "  \"sort\": %d,\n"
            "  \"show_missing\": %s,\n"
            "  \"resume_last\": %s,\n"
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
            s->autosleep_min, s->autosuspend_min, s->saver, s->aspect, s->filter, s->bezel ? "true" : "false", s->dup_mode, s->sort_mode, s->show_missing ? "true" : "false", s->resume_last ? "true" : "false",
            s->wifi ? "true" : "false", s->ext_p1 ? "true" : "false", s->music_vol, s->tz, s->led_mode, s->led_color, s->led_bright, s->grid_columns, s->home_size, s->log_level);
    fclose(f);
    /* scrittura atomica: su SD uno spegnimento a metà non lascia file corrotti */
    return rename(tmp, path) == 0;
}

void device_size(const char *device, int *w, int *h) {
    if (!strcmp(device, "brickpro")) { *w = 1024; *h = 768; }
    else { *w = 1280; *h = 720; }
}

const char *device_label(const char *device) {
    return !strcmp(device, "brickpro") ? "Brick Pro 1024×768" : "Smart Pro 1280×720";
}
