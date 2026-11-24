/*
 * kv_core - tiny JSON value tree with path access. No dependency on Pd.
 * Copyright (c) 2026 Sofia Vallejo Budziszewski. BSD 3-Clause.
 */
#include "kv_core.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <locale.h>
#include <stdint.h>

#define KV_MAX_DEPTH 128     /* JSON nesting limit */
#define KV_MAX_PATH 64       /* segments in a path */
#define KV_MAX_FILL 1000000  /* null padding when setting an index past the end */
#define KV_LINEAR_MAX 8      /* objects up to this size are searched linearly */

/* Pd builds with -ffast-math, which may fold isfinite() away, so test the bits */
static int finite_d(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return ((u >> 52) & 0x7FF) != 0x7FF;
}

static int finite_f(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return ((u >> 23) & 0xFF) != 0xFF;
}

/* ------------------------------------------------------------ memory */

static void *xalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("kvx: out of memory\n", stderr); abort(); }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fputs("kvx: out of memory\n", stderr); abort(); }
    return q;
}

static char *xdup(const char *s, size_t n)
{
    char *p = (char *)xalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ------------------------------------------------------------ nodes */

static kv_node *node_new(kv_type t)
{
    kv_node *n = (kv_node *)xalloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->type = t;
    return n;
}

kv_node *kv_null(void) { return node_new(KV_NULL); }
kv_node *kv_bool(int b) { kv_node *n = node_new(KV_BOOL); n->num = b ? 1 : 0; return n; }
kv_node *kv_num(double d) { kv_node *n = node_new(KV_NUM); n->num = d; return n; }
kv_node *kv_arr(void) { return node_new(KV_ARR); }
kv_node *kv_obj(void) { return node_new(KV_OBJ); }

kv_node *kv_str(const char *s, size_t len)
{
    kv_node *n = node_new(KV_STR);
    n->str = xdup(s, len);
    n->len = len;
    return n;
}

void kv_free(kv_node *n)
{
    size_t i;
    if (!n) return;
    switch (n->type)
    {
    case KV_STR:
        free(n->str);
        break;
    case KV_ARR:
        for (i = 0; i < n->len; i++) kv_free(n->items[i]);
        free(n->items);
        break;
    case KV_OBJ:
        for (i = 0; i < n->len; i++)
        {
            free(n->members[i].key);
            kv_free(n->members[i].val);
        }
        free(n->members);
        free(n->tab);
        break;
    default:
        break;
    }
    free(n);
}

const char *kv_type_name(const kv_node *n)
{
    if (!n) return "missing";
    switch (n->type)
    {
    case KV_NULL: return "null";
    case KV_BOOL: return "bool";
    case KV_NUM: return "number";
    case KV_STR: return "string";
    case KV_ARR: return "array";
    case KV_OBJ: return "object";
    }
    return "missing";
}

/* ------------------------------------------------------------ arrays */

void kv_arr_push(kv_node *arr, kv_node *item)
{
    if (arr->len == arr->cap)
    {
        arr->cap = arr->cap ? arr->cap * 2 : 4;
        arr->items = (kv_node **)xrealloc(arr->items, arr->cap * sizeof *arr->items);
    }
    arr->items[arr->len++] = item;
}

/* ------------------------------------------------------------ objects */

static size_t hash_key(const char *s)
{
    unsigned long h = 2166136261UL;
    while (*s)
    {
        h ^= (unsigned char)*s++;
        h = (h * 16777619UL) & 0xffffffffUL;
    }
    return (size_t)h;
}

static void tab_rebuild(kv_node *o)
{
    size_t cap = 16, i, j;
    free(o->tab);
    o->tab = NULL;
    o->tabcap = 0;
    if (o->len <= KV_LINEAR_MAX) return;
    while (cap < o->len * 2 + 2) cap <<= 1;
    o->tab = (size_t *)xalloc(cap * sizeof *o->tab);
    memset(o->tab, 0, cap * sizeof *o->tab);
    o->tabcap = cap;
    for (i = 0; i < o->len; i++)
    {
        j = hash_key(o->members[i].key) & (cap - 1);
        while (o->tab[j]) j = (j + 1) & (cap - 1);
        o->tab[j] = i + 1;
    }
}

/* index of key, or -1 */
static long obj_find(const kv_node *o, const char *key)
{
    size_t i;
    if (!o->tab)
    {
        for (i = 0; i < o->len; i++)
            if (!strcmp(o->members[i].key, key)) return (long)i;
        return -1;
    }
    i = hash_key(key) & (o->tabcap - 1);
    while (o->tab[i])
    {
        size_t m = o->tab[i] - 1;
        if (!strcmp(o->members[m].key, key)) return (long)m;
        i = (i + 1) & (o->tabcap - 1);
    }
    return -1;
}

/* append a member that is known not to exist yet; returns its slot */
static kv_node **obj_append(kv_node *o, const char *key, kv_node *val)
{
    size_t idx;
    if (o->len == o->cap)
    {
        o->cap = o->cap ? o->cap * 2 : 4;
        o->members = (struct kv_member *)xrealloc(o->members, o->cap * sizeof *o->members);
    }
    idx = o->len++;
    o->members[idx].key = xdup(key, strlen(key));
    o->members[idx].val = val;
    if (o->len > KV_LINEAR_MAX && (!o->tab || o->len * 2 > o->tabcap))
        tab_rebuild(o);
    else if (o->tab)
    {
        size_t j = hash_key(key) & (o->tabcap - 1);
        while (o->tab[j]) j = (j + 1) & (o->tabcap - 1);
        o->tab[j] = idx + 1;
    }
    return &o->members[idx].val;
}

static kv_node **obj_slot(kv_node *o, const char *key, int create)
{
    long i = obj_find(o, key);
    if (i >= 0) return &o->members[i].val;
    return create ? obj_append(o, key, kv_null()) : NULL;
}

kv_node *kv_obj_get(const kv_node *o, const char *key)
{
    long i;
    if (!o || o->type != KV_OBJ) return NULL;
    i = obj_find(o, key);
    return i >= 0 ? o->members[i].val : NULL;
}

void kv_obj_set(kv_node *o, const char *key, kv_node *val)
{
    long i = obj_find(o, key);
    if (i >= 0)
    {
        kv_free(o->members[i].val);
        o->members[i].val = val;
    }
    else obj_append(o, key, val);
}

static void obj_remove(kv_node *o, size_t idx)
{
    free(o->members[idx].key);
    kv_free(o->members[idx].val);
    memmove(&o->members[idx], &o->members[idx + 1], (o->len - idx - 1) * sizeof *o->members);
    o->len--;
    tab_rebuild(o);
}

/* ------------------------------------------------------------ numbers */

double kv_float_to_double(float f)
{
    char buf[40];
    int p;
    if (!finite_f(f)) return (double)f;
    for (p = 1; p <= 9; p++)
    {
        snprintf(buf, sizeof buf, "%.*g", p, (double)f);
        if (strtof(buf, NULL) == f) break;
    }
    return strtod(buf, NULL);
}

/* ------------------------------------------------------------ parser */

typedef struct {
    const char *start, *cur, *end;
    char *err;
    size_t errsize;
    int failed;
    int depth;
} kv_parser;

typedef struct { char *p; size_t len, cap; } sbuf;

static void sb_put(sbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap)
    {
        size_t c = b->cap ? b->cap : 64;
        while (c < b->len + n + 1) c *= 2;
        b->p = (char *)xrealloc(b->p, c);
        b->cap = c;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_ch(sbuf *b, char c) { sb_put(b, &c, 1); }

static void perr(kv_parser *p, const char *msg)
{
    if (p->failed) return;
    p->failed = 1;
    if (p->err && p->errsize)
        snprintf(p->err, p->errsize, "%s at byte %lu", msg, (unsigned long)(p->cur - p->start));
}

static void skip_ws(kv_parser *p)
{
    while (p->cur < p->end &&
        (*p->cur == ' ' || *p->cur == '\t' || *p->cur == '\n' || *p->cur == '\r'))
        p->cur++;
}

static void put_utf8(sbuf *b, unsigned long cp)
{
    char c[4];
    if (cp < 0x80) { c[0] = (char)cp; sb_put(b, c, 1); }
    else if (cp < 0x800)
    {
        c[0] = (char)(0xC0 | (cp >> 6)); c[1] = (char)(0x80 | (cp & 0x3F));
        sb_put(b, c, 2);
    }
    else if (cp < 0x10000)
    {
        c[0] = (char)(0xE0 | (cp >> 12)); c[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        c[2] = (char)(0x80 | (cp & 0x3F));
        sb_put(b, c, 3);
    }
    else
    {
        c[0] = (char)(0xF0 | (cp >> 18)); c[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        c[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); c[3] = (char)(0x80 | (cp & 0x3F));
        sb_put(b, c, 4);
    }
}

static int hex4(const char *s, unsigned long *out)
{
    int i;
    unsigned long v = 0;
    for (i = 0; i < 4; i++)
    {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned long)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* on success *out is a malloc'd string, *outlen its byte length */
static int parse_string(kv_parser *p, char **out, size_t *outlen)
{
    sbuf b = {0, 0, 0};
    p->cur++; /* opening quote */
    sb_put(&b, "", 0);
    for (;;)
    {
        char c;
        if (p->cur >= p->end) { perr(p, "unterminated string"); free(b.p); return 0; }
        c = *p->cur++;
        if (c == '"') break;
        if (c != '\\') { sb_ch(&b, c); continue; }
        if (p->cur >= p->end) { perr(p, "unterminated string"); free(b.p); return 0; }
        c = *p->cur++;
        switch (c)
        {
        case '"': sb_ch(&b, '"'); break;
        case '\\': sb_ch(&b, '\\'); break;
        case '/': sb_ch(&b, '/'); break;
        case 'b': sb_ch(&b, '\b'); break;
        case 'f': sb_ch(&b, '\f'); break;
        case 'n': sb_ch(&b, '\n'); break;
        case 'r': sb_ch(&b, '\r'); break;
        case 't': sb_ch(&b, '\t'); break;
        case 'u':
        {
            unsigned long cp, lo;
            if (p->end - p->cur < 4 || !hex4(p->cur, &cp))
            { perr(p, "bad \\u escape"); free(b.p); return 0; }
            p->cur += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF)
            {
                if (p->end - p->cur >= 6 && p->cur[0] == '\\' && p->cur[1] == 'u' &&
                    hex4(p->cur + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p->cur += 6;
                }
                else cp = 0xFFFD;
            }
            else if (cp >= 0xDC00 && cp <= 0xDFFF) cp = 0xFFFD;
            if (cp == 0) cp = 0xFFFD; /* a C string cannot hold NUL */
            put_utf8(&b, cp);
            break;
        }
        default:
            perr(p, "bad escape");
            free(b.p);
            return 0;
        }
    }
    *out = b.p;
    *outlen = b.len;
    return 1;
}

static kv_node *parse_number(kv_parser *p)
{
    char buf[64], *end;
    const char *dp;
    size_t n = 0;
    double d;
    while (p->cur + n < p->end && strchr("+-0123456789.eE", p->cur[n]) && p->cur[n]) n++;
    if (n == 0) { perr(p, "unexpected character"); return NULL; }
    if (n >= sizeof buf) { perr(p, "number too long"); return NULL; }
    memcpy(buf, p->cur, n);
    buf[n] = 0;
    dp = localeconv()->decimal_point;
    if (dp[0] != '.' && dp[1] == 0)
    {
        char *dot = strchr(buf, '.');
        if (dot) *dot = dp[0];
    }
    d = strtod(buf, &end);
    if (end != buf + n) { perr(p, "bad number"); return NULL; }
    p->cur += n;
    return kv_num(d);
}

static kv_node *parse_value(kv_parser *p);

static kv_node *parse_literal(kv_parser *p, const char *word, kv_node *(*make)(void))
{
    size_t n = strlen(word);
    if ((size_t)(p->end - p->cur) >= n && !memcmp(p->cur, word, n))
    {
        p->cur += n;
        return make();
    }
    perr(p, "unexpected character");
    return NULL;
}

static kv_node *make_true(void) { return kv_bool(1); }
static kv_node *make_false(void) { return kv_bool(0); }

static kv_node *parse_array(kv_parser *p)
{
    kv_node *arr;
    if (++p->depth > KV_MAX_DEPTH) { perr(p, "nested too deep"); return NULL; }
    p->cur++; /* [ */
    arr = kv_arr();
    skip_ws(p);
    if (p->cur < p->end && *p->cur == ']') { p->cur++; p->depth--; return arr; }
    for (;;)
    {
        kv_node *v = parse_value(p);
        if (!v) { kv_free(arr); return NULL; }
        kv_arr_push(arr, v);
        skip_ws(p);
        if (p->cur >= p->end) { perr(p, "unterminated array"); kv_free(arr); return NULL; }
        if (*p->cur == ',') { p->cur++; continue; }
        if (*p->cur == ']') { p->cur++; break; }
        perr(p, "expected , or ]");
        kv_free(arr);
        return NULL;
    }
    p->depth--;
    return arr;
}

static kv_node *parse_object(kv_parser *p)
{
    kv_node *obj;
    if (++p->depth > KV_MAX_DEPTH) { perr(p, "nested too deep"); return NULL; }
    p->cur++; /* { */
    obj = kv_obj();
    skip_ws(p);
    if (p->cur < p->end && *p->cur == '}') { p->cur++; p->depth--; return obj; }
    for (;;)
    {
        char *key;
        size_t klen;
        kv_node *v;
        skip_ws(p);
        if (p->cur >= p->end || *p->cur != '"') { perr(p, "expected a string key"); kv_free(obj); return NULL; }
        if (!parse_string(p, &key, &klen)) { kv_free(obj); return NULL; }
        skip_ws(p);
        if (p->cur >= p->end || *p->cur != ':') { perr(p, "expected :"); free(key); kv_free(obj); return NULL; }
        p->cur++;
        v = parse_value(p);
        if (!v) { free(key); kv_free(obj); return NULL; }
        kv_obj_set(obj, key, v);
        free(key);
        skip_ws(p);
        if (p->cur >= p->end) { perr(p, "unterminated object"); kv_free(obj); return NULL; }
        if (*p->cur == ',') { p->cur++; continue; }
        if (*p->cur == '}') { p->cur++; break; }
        perr(p, "expected , or }");
        kv_free(obj);
        return NULL;
    }
    p->depth--;
    return obj;
}

static kv_node *parse_value(kv_parser *p)
{
    skip_ws(p);
    if (p->cur >= p->end) { perr(p, "unexpected end"); return NULL; }
    switch (*p->cur)
    {
    case '{': return parse_object(p);
    case '[': return parse_array(p);
    case '"':
    {
        char *s;
        size_t n;
        kv_node *v;
        if (!parse_string(p, &s, &n)) return NULL;
        v = node_new(KV_STR);
        v->str = s;
        v->len = n;
        return v;
    }
    case 't': return parse_literal(p, "true", make_true);
    case 'f': return parse_literal(p, "false", make_false);
    case 'n': return parse_literal(p, "null", kv_null);
    default: return parse_number(p);
    }
}

kv_node *kv_parse(const char *text, size_t len, char *err, size_t errsize)
{
    kv_parser p;
    kv_node *v;
    p.start = p.cur = text;
    p.end = text + len;
    p.err = err;
    p.errsize = errsize;
    p.failed = 0;
    p.depth = 0;
    if (err && errsize) err[0] = 0;
    if (len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) p.cur += 3;
    v = parse_value(&p);
    if (!v) return NULL;
    skip_ws(&p);
    if (p.cur != p.end)
    {
        perr(&p, "unexpected text after the value");
        kv_free(v);
        return NULL;
    }
    return v;
}

/* ------------------------------------------------------------ writer */

static void dump_string(sbuf *b, const char *s, size_t n)
{
    size_t i;
    sb_ch(b, '"');
    for (i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)s[i];
        switch (c)
        {
        case '"': sb_put(b, "\\\"", 2); break;
        case '\\': sb_put(b, "\\\\", 2); break;
        case '\n': sb_put(b, "\\n", 2); break;
        case '\r': sb_put(b, "\\r", 2); break;
        case '\t': sb_put(b, "\\t", 2); break;
        case '\b': sb_put(b, "\\b", 2); break;
        case '\f': sb_put(b, "\\f", 2); break;
        default:
            if (c < 0x20)
            {
                char u[8];
                snprintf(u, sizeof u, "\\u%04x", (unsigned)c);
                sb_put(b, u, 6);
            }
            else sb_ch(b, (char)c);
        }
    }
    sb_ch(b, '"');
}

static void dump_number(sbuf *b, double d)
{
    char buf[64];
    const char *dp;
    size_t i;
    if (!finite_d(d)) { sb_put(b, "null", 4); return; }
    snprintf(buf, sizeof buf, "%.15g", d);
    if (strtod(buf, NULL) != d) snprintf(buf, sizeof buf, "%.17g", d);
    dp = localeconv()->decimal_point;
    if (dp[0] != '.' && dp[1] == 0)
        for (i = 0; buf[i]; i++)
            if (buf[i] == dp[0]) buf[i] = '.';
    sb_put(b, buf, strlen(buf));
}

static void indent(sbuf *b, int level)
{
    int i;
    for (i = 0; i < level; i++) sb_put(b, "  ", 2);
}

static void dump_node(sbuf *b, const kv_node *n, int pretty, int level)
{
    size_t i;
    switch (n->type)
    {
    case KV_NULL: sb_put(b, "null", 4); break;
    case KV_BOOL: if (n->num != 0) sb_put(b, "true", 4); else sb_put(b, "false", 5); break;
    case KV_NUM: dump_number(b, n->num); break;
    case KV_STR: dump_string(b, n->str, n->len); break;
    case KV_ARR:
        if (!n->len) { sb_put(b, "[]", 2); break; }
        sb_ch(b, '[');
        for (i = 0; i < n->len; i++)
        {
            if (i) sb_ch(b, ',');
            if (pretty) { sb_ch(b, '\n'); indent(b, level + 1); }
            dump_node(b, n->items[i], pretty, level + 1);
        }
        if (pretty) { sb_ch(b, '\n'); indent(b, level); }
        sb_ch(b, ']');
        break;
    case KV_OBJ:
        if (!n->len) { sb_put(b, "{}", 2); break; }
        sb_ch(b, '{');
        for (i = 0; i < n->len; i++)
        {
            if (i) sb_ch(b, ',');
            if (pretty) { sb_ch(b, '\n'); indent(b, level + 1); }
            dump_string(b, n->members[i].key, strlen(n->members[i].key));
            sb_ch(b, ':');
            if (pretty) sb_ch(b, ' ');
            dump_node(b, n->members[i].val, pretty, level + 1);
        }
        if (pretty) { sb_ch(b, '\n'); indent(b, level); }
        sb_ch(b, '}');
        break;
    }
}

char *kv_dump(const kv_node *n, int pretty, size_t *len_out)
{
    sbuf b = {0, 0, 0};
    sb_put(&b, "", 0);
    dump_node(&b, n, pretty, 0);
    if (len_out) *len_out = b.len;
    return b.p;
}

/* ------------------------------------------------------------ paths */

typedef struct { char *key; int numeric; unsigned long idx; } kv_seg;

static void segs_free(kv_seg *s, int n)
{
    int i;
    for (i = 0; i < n; i++) free(s[i].key);
}

/* returns 1 ok (n may be 0 for the root), 0 on error */
static int path_split(const char *path, kv_seg *segs, int *n, char *err, size_t errsize)
{
    const char *p = path, *q;
    *n = 0;
    if (err && errsize) err[0] = 0;
    if (!*p || (p[0] == '.' && !p[1])) return 1;
    if (*p == '.') p++;
    for (;;)
    {
        char *key;
        size_t klen, i;
        int all_digits;
        if (*n >= KV_MAX_PATH)
        {
            if (err) snprintf(err, errsize, "path is too deep");
            segs_free(segs, *n);
            return 0;
        }
        if (*p == '[')
        {
            p++;
            q = p;
            while (*p && *p != ']') p++;
            if (*p != ']')
            {
                if (err) snprintf(err, errsize, "missing ] in path");
                segs_free(segs, *n);
                return 0;
            }
            klen = (size_t)(p - q);
            p++;
        }
        else
        {
            q = p;
            while (*p && *p != '.' && *p != '[') p++;
            klen = (size_t)(p - q);
        }
        if (klen == 0)
        {
            if (err) snprintf(err, errsize, "empty name in path");
            segs_free(segs, *n);
            return 0;
        }
        key = xdup(q, klen);
        all_digits = klen <= 9;
        for (i = 0; i < klen && all_digits; i++)
            if (key[i] < '0' || key[i] > '9') all_digits = 0;
        segs[*n].key = key;
        segs[*n].numeric = all_digits;
        segs[*n].idx = all_digits ? strtoul(key, NULL, 10) : 0;
        (*n)++;
        if (*p == '.')
        {
            p++;
            if (!*p)
            {
                if (err) snprintf(err, errsize, "path ends with a dot");
                segs_free(segs, *n);
                return 0;
            }
            continue;
        }
        if (*p == '[') continue;
        if (!*p) break;
        if (err) snprintf(err, errsize, "bad path");
        segs_free(segs, *n);
        return 0;
    }
    return 1;
}

kv_node *kv_path_get(kv_node *root, const char *path)
{
    kv_seg segs[KV_MAX_PATH];
    int n, i;
    kv_node *cur = root;
    if (!path_split(path, segs, &n, NULL, 0)) return NULL;
    for (i = 0; i < n && cur; i++)
    {
        if (cur->type == KV_ARR)
            cur = (segs[i].numeric && segs[i].idx < cur->len) ? cur->items[segs[i].idx] : NULL;
        else if (cur->type == KV_OBJ)
            cur = kv_obj_get(cur, segs[i].key);
        else
            cur = NULL;
    }
    segs_free(segs, n);
    return cur;
}

int kv_path_set(kv_node **root, const char *path, kv_node *val, char *err, size_t errsize)
{
    kv_seg segs[KV_MAX_PATH];
    int n, i;
    kv_node **slot = root, *cont;
    if (!path_split(path, segs, &n, err, errsize)) { kv_free(val); return -1; }
    for (i = 0; i < n; i++)
    {
        if (!*slot || ((*slot)->type != KV_ARR && (*slot)->type != KV_OBJ))
        {
            kv_free(*slot);
            *slot = segs[i].numeric ? kv_arr() : kv_obj();
        }
        cont = *slot;
        if (cont->type == KV_ARR)
        {
            if (!segs[i].numeric)
            {
                if (err) snprintf(err, errsize, "'%s' is not an index, but a list is there", segs[i].key);
                segs_free(segs, n);
                kv_free(val);
                return -1;
            }
            if (segs[i].idx >= cont->len && segs[i].idx - cont->len > KV_MAX_FILL)
            {
                if (err) snprintf(err, errsize, "index %lu is too far past the end", segs[i].idx);
                segs_free(segs, n);
                kv_free(val);
                return -1;
            }
            while (cont->len <= segs[i].idx) kv_arr_push(cont, kv_null());
            slot = &cont->items[segs[i].idx];
        }
        else
            slot = obj_slot(cont, segs[i].key, 1);
    }
    segs_free(segs, n);
    kv_free(*slot);
    *slot = val;
    return 0;
}

int kv_path_del(kv_node *root, const char *path)
{
    kv_seg segs[KV_MAX_PATH];
    int n, i, removed = 0;
    kv_node *cur = root;
    if (!path_split(path, segs, &n, NULL, 0)) return 0;
    if (n == 0) { segs_free(segs, n); return 0; }
    for (i = 0; i < n - 1 && cur; i++)
    {
        if (cur->type == KV_ARR)
            cur = (segs[i].numeric && segs[i].idx < cur->len) ? cur->items[segs[i].idx] : NULL;
        else if (cur->type == KV_OBJ)
            cur = kv_obj_get(cur, segs[i].key);
        else
            cur = NULL;
    }
    if (cur && cur->type == KV_ARR && segs[n - 1].numeric && segs[n - 1].idx < cur->len)
    {
        size_t idx = segs[n - 1].idx;
        kv_free(cur->items[idx]);
        memmove(&cur->items[idx], &cur->items[idx + 1], (cur->len - idx - 1) * sizeof *cur->items);
        cur->len--;
        removed = 1;
    }
    else if (cur && cur->type == KV_OBJ)
    {
        long idx = obj_find(cur, segs[n - 1].key);
        if (idx >= 0) { obj_remove(cur, (size_t)idx); removed = 1; }
    }
    segs_free(segs, n);
    return removed;
}
