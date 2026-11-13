/* Lettore JSON minimale: abbastanza per temi e impostazioni. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jval {
    jtype t;
    double n;            /* J_NUM, J_BOOL (0/1) */
    char *s;             /* J_STR */
    char *key;           /* chiave se membro di un oggetto */
    struct jval *child;  /* primo figlio di J_ARR / J_OBJ */
    struct jval *next;   /* fratello successivo */
} jval;

jval *json_parse(const char *text);
jval *json_load(const char *path);
void json_free(jval *v);

/* Percorso con punti: "variants.dark.accent". NULL se assente. */
jval *json_get(jval *obj, const char *path);
double json_num(jval *obj, const char *path, double def);
/* numero come int: def se è fuori da un intervallo ragionevole o non valido */
int json_int(jval *obj, const char *path, int def);
int json_to_int(double d, int def);
const char *json_str(jval *obj, const char *path, const char *def);
bool json_bool(jval *obj, const char *path, bool def);

/* s pronta da mettere fra virgolette in un file JSON (\" \\ e caratteri di controllo); buf, n
   almeno 6 volte la lunghezza più uno per non tagliare. Restituisce buf */
const char *json_esc(const char *s, char *buf, size_t n);
