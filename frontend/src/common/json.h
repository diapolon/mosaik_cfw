/* Lettore JSON minimale: abbastanza per temi e impostazioni. */
#pragma once
#include <stdbool.h>

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
const char *json_str(jval *obj, const char *path, const char *def);
bool json_bool(jval *obj, const char *path, bool def);
