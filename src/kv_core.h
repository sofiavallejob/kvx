/*
 * kv_core - tiny JSON value tree with path access. No dependency on Pd.
 * Copyright (c) 2026 Sofia Vallejo Budziszewski. BSD 3-Clause.
 */
#ifndef KV_CORE_H
#define KV_CORE_H

#include <stddef.h>

typedef enum { KV_NULL, KV_BOOL, KV_NUM, KV_STR, KV_ARR, KV_OBJ } kv_type;

typedef struct kv_node kv_node;

struct kv_member { char *key; kv_node *val; };

struct kv_node {
    kv_type type;
    double num;                  /* KV_NUM value, KV_BOOL 0 or 1 */
    char *str;                   /* KV_STR text (NUL terminated) */
    size_t len;                  /* string bytes, array or object count */
    size_t cap;                  /* allocated slots for items/members */
    kv_node **items;             /* KV_ARR */
    struct kv_member *members;   /* KV_OBJ, insertion order */
    size_t *tab;                 /* KV_OBJ hash index (member index + 1), or NULL */
    size_t tabcap;
};

/* construction. Containers take ownership of the nodes you give them. */
kv_node *kv_null(void);
kv_node *kv_bool(int b);
kv_node *kv_num(double d);
kv_node *kv_str(const char *s, size_t len);
kv_node *kv_arr(void);
kv_node *kv_obj(void);
void kv_free(kv_node *n);

void kv_arr_push(kv_node *arr, kv_node *item);
kv_node *kv_obj_get(const kv_node *obj, const char *key);
void kv_obj_set(kv_node *obj, const char *key, kv_node *val);

const char *kv_type_name(const kv_node *n);

/* A float32 from Pd to a double that prints the way the user typed it (0.1, not 0.10000000149) */
double kv_float_to_double(float f);

/* JSON text <-> tree. kv_parse returns NULL and fills err on failure.
 * kv_dump returns a malloc'd string (free() it); len_out may be NULL. */
kv_node *kv_parse(const char *text, size_t len, char *err, size_t errsize);
char *kv_dump(const kv_node *n, int pretty, size_t *len_out);

/* Paths: "a.b.c", "list.0", "list[0].name". "" or "." is the root.
 * kv_path_get returns NULL if missing. kv_path_set always consumes val;
 * missing parents are created (an array if the next segment is a number).
 * kv_path_del returns 1 if something was removed. */
kv_node *kv_path_get(kv_node *root, const char *path);
int kv_path_set(kv_node **root, const char *path, kv_node *val, char *err, size_t errsize);
int kv_path_del(kv_node *root, const char *path);

#endif
