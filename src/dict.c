/*
 * [kvx/dict] - a named, shared dictionary for Pd. Holds nested JSON-like data
 * (objects, lists, numbers, strings, booleans, null), reads and writes JSON
 * files, and answers requests from other programs (see the "req" message).
 *
 * Copyright (c) 2026 Sofia Vallejo Budziszewski. BSD 3-Clause.
 */
#include "m_pd.h"
#include "kv_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KVX_VERSION "0.3.0"
#define PATHSZ 4096
#define KVX_MAXFILE (512UL * 1024UL * 1024UL)
#define RES 3   /* atoms reserved in front of every output buffer, for replies */

static t_class *dict_class;

/* ------------------------------------------------------------ shared stores */

typedef struct _store {
    t_symbol *name;
    kv_node *root;
    int refs;
    struct _store *next;
} t_store;

static t_store *store_list = NULL;

static t_store *store_acquire(t_symbol *name)
{
    t_store *s;
    if (name)
        for (s = store_list; s; s = s->next)
            if (s->name == name) { s->refs++; return s; }
    s = (t_store *)getbytes(sizeof(*s));
    s->name = name;
    s->root = kv_obj();
    s->refs = 1;
    s->next = NULL;
    if (name)
    {
        s->next = store_list;
        store_list = s;
    }
    return s;
}

static void store_release(t_store *s)
{
    if (--s->refs > 0) return;
    if (s->name)
    {
        t_store **pp = &store_list;
        while (*pp && *pp != s) pp = &(*pp)->next;
        if (*pp) *pp = s->next;
    }
    kv_free(s->root);
    freebytes(s, sizeof(*s));
}

/* ------------------------------------------------------------ the object */

typedef struct _dict {
    t_object x_obj;
    t_outlet *x_out;     /* values and json text */
    t_outlet *x_info;    /* keys, size, type, has, missing, error ... */
    t_outlet *x_reply;   /* answers to "req" messages: [send reply <id> ...( for [netsend] */
    t_canvas *x_canvas;
    t_store *x_store;
    int x_inreq;
    int x_replied;
    t_atom x_replyid;
    char *x_buf;         /* JSON text arriving in pieces (see jsonbuf) */
    size_t x_buflen;
} t_dict;

#define ROOT(x) ((x)->x_store->root)

/* ------------------------------------------------------------ small helpers */

static double to_double(t_float f)
{
#if PD_FLOATSIZE == 32
    return kv_float_to_double(f);
#else
    return (double)f;
#endif
}

static t_atom *abuf_new(int n)
{
    return (t_atom *)getbytes((size_t)(n + RES) * sizeof(t_atom)) + RES;
}

static void abuf_free(t_atom *a, int n)
{
    freebytes(a - RES, (size_t)(n + RES) * sizeof(t_atom));
}

/* text of an atom without Pd's escaping for symbols */
static void atom_text(const t_atom *a, char *buf, int size)
{
    if (a->a_type == A_SYMBOL)
    {
        strncpy(buf, a->a_w.w_symbol->s_name, (size_t)size - 1);
        buf[size - 1] = 0;
    }
    else atom_string(a, buf, (unsigned int)size);
}

/* all atoms joined with sep; caller frees */
static char *atoms_join_with(int argc, t_atom *argv, const char *sep, size_t *outlen)
{
    size_t cap = 256, len = 0, seplen = strlen(sep);
    char *p = (char *)malloc(cap);
    int i;
    if (!p) return NULL;
    for (i = 0; i < argc; i++)
    {
        char tmp[MAXPDSTRING];
        const char *s;
        size_t l;
        if (argv[i].a_type == A_SYMBOL) s = argv[i].a_w.w_symbol->s_name;
        else { atom_string(&argv[i], tmp, MAXPDSTRING); s = tmp; }
        l = strlen(s);
        if (len + l + seplen + 1 > cap)
        {
            char *q;
            while (len + l + seplen + 1 > cap) cap *= 2;
            q = (char *)realloc(p, cap);
            if (!q) { free(p); return NULL; }
            p = q;
        }
        if (i && seplen) { memcpy(p + len, sep, seplen); len += seplen; }
        memcpy(p + len, s, l);
        len += l;
    }
    p[len] = 0;
    *outlen = len;
    return p;
}

static char *atoms_join(int argc, t_atom *argv, size_t *outlen)
{
    return atoms_join_with(argc, argv, " ", outlen);
}

static void node_to_atom(const kv_node *n, t_atom *a)
{
    switch (n->type)
    {
    case KV_NUM: SETFLOAT(a, (t_float)n->num); break;
    case KV_BOOL: SETFLOAT(a, n->num != 0 ? 1 : 0); break;
    case KV_STR: SETSYMBOL(a, gensym(n->str)); break;
    default: SETSYMBOL(a, gensym("null")); break;
    }
}

static kv_node *atom_to_node(const t_atom *a)
{
    if (a->a_type == A_FLOAT) return kv_num(to_double(a->a_w.w_float));
    if (a->a_type == A_SYMBOL)
        return kv_str(a->a_w.w_symbol->s_name, strlen(a->a_w.w_symbol->s_name));
    return kv_null();
}

/* one atom is a scalar, several atoms are a list */
static kv_node *atoms_to_node(int argc, t_atom *argv)
{
    kv_node *arr;
    int i;
    if (argc == 1) return atom_to_node(argv);
    arr = kv_arr();
    for (i = 0; i < argc; i++) kv_arr_push(arr, atom_to_node(&argv[i]));
    return arr;
}

static int is_scalar(const kv_node *n)
{
    return n->type != KV_ARR && n->type != KV_OBJ;
}

static int is_flat_list(const kv_node *n)
{
    size_t i;
    if (n->type != KV_ARR) return 0;
    for (i = 0; i < n->len; i++)
        if (!is_scalar(n->items[i])) return 0;
    return 1;
}

/* ------------------------------------------------------------ output */

/* Answers leave the right outlet as [send reply <id> <kind> ...(, which is what
 * [netsend] wants. */
static void send_reply(t_dict *x, t_symbol *kind, t_atom *args, int n)
{
    t_atom *h = args - RES;
    SETSYMBOL(&h[0], gensym("reply"));
    h[1] = x->x_replyid;
    SETSYMBOL(&h[2], kind);
    outlet_anything(x->x_reply, gensym("send"), n + RES, h);
    x->x_replied = 1;
}

static void emit_info(t_dict *x, const char *sel, t_atom *args, int n)
{
    if (x->x_inreq) send_reply(x, gensym(sel), args, n);
    else outlet_anything(x->x_info, gensym(sel), n, args);
}

static void emit_error(t_dict *x, const char *msg)
{
    t_atom *a = abuf_new(1);
    SETSYMBOL(a, gensym(msg));
    if (!x->x_inreq) pd_error(x, "kvx/dict: %s", msg);
    emit_info(x, "error", a, 1);
    abuf_free(a, 1);
}

static void emit_missing(t_dict *x, const char *path)
{
    t_atom *a = abuf_new(1);
    SETSYMBOL(a, gensym(path));
    emit_info(x, "missing", a, 1);
    abuf_free(a, 1);
}

/* Pd cuts a word at 1000 characters when it sends text over the network, so a
 * JSON text in a reply goes out as several short words that the receiver glues
 * together again. */
#define JSON_ATOM 400

static void emit_json_text(t_dict *x, const char *text)
{
    if (x->x_inreq)
    {
        size_t len = strlen(text), pos = 0;
        int n = 0, cap = (int)(len / (JSON_ATOM - 3)) + 2;
        t_atom *a = abuf_new(cap);
        char piece[JSON_ATOM + 1];
        while (pos < len)
        {
            size_t l = len - pos;
            if (l > JSON_ATOM)
            {
                l = JSON_ATOM;   /* do not cut a UTF-8 character in half */
                while (l > 1 && ((unsigned char)text[pos + l] & 0xC0) == 0x80) l--;
            }
            memcpy(piece, text + pos, l);
            piece[l] = 0;
            SETSYMBOL(&a[n], gensym(piece));
            n++;
            pos += l;
        }
        send_reply(x, gensym("json"), a, n);
        abuf_free(a, cap);
        return;
    }
    {
        t_atom *a = abuf_new(1);
        SETSYMBOL(a, gensym(text));
        outlet_anything(x->x_out, gensym("json"), 1, a);
        abuf_free(a, 1);
    }
}

static void emit_json_node(t_dict *x, const kv_node *n, int pretty)
{
    char *text = kv_dump(n, pretty, NULL);
    emit_json_text(x, text);
    free(text);
}

static void emit_values(t_dict *x, t_atom *a, int n)
{
    if (n == 1 && a[0].a_type == A_FLOAT) outlet_float(x->x_out, a[0].a_w.w_float);
    else if (n == 1 && a[0].a_type == A_SYMBOL) outlet_symbol(x->x_out, a[0].a_w.w_symbol);
    else outlet_list(x->x_out, &s_list, n, a);
}

/* numbers, strings and flat lists come out as plain Pd messages, anything
 * nested comes out as [json <text>(. Requests always get JSON back, so a
 * string "123" and a number 123 stay distinguishable. */
static void emit_node(t_dict *x, const kv_node *n)
{
    if (x->x_inreq || !(is_scalar(n) || is_flat_list(n)))
    {
        emit_json_node(x, n, 0);
        return;
    }
    if (is_scalar(n))
    {
        t_atom *a = abuf_new(1);
        node_to_atom(n, a);
        emit_values(x, a, 1);
        abuf_free(a, 1);
    }
    else
    {
        int count = (int)n->len, i;
        t_atom *a = abuf_new(count);
        for (i = 0; i < count; i++) node_to_atom(n->items[i], &a[i]);
        emit_values(x, a, count);
        abuf_free(a, count);
    }
}

/* ------------------------------------------------------------ messages */

static const char *path_arg(int argc, t_atom *argv, char *buf)
{
    if (argc < 1) return ".";
    atom_text(&argv[0], buf, PATHSZ);
    return buf[0] ? buf : ".";
}

static void dict_set(t_dict *x, t_symbol *s, int argc, t_atom *argv)
{
    char path[PATHSZ], err[256];
    (void)s;
    if (argc < 2) { emit_error(x, "set: need a path and a value"); return; }
    atom_text(&argv[0], path, PATHSZ);
    if (kv_path_set(&ROOT(x), path, atoms_to_node(argc - 1, argv + 1), err, sizeof err))
        emit_error(x, err);
}

static void dict_setnull(t_dict *x, t_symbol *path)
{
    char err[256];
    if (kv_path_set(&ROOT(x), path->s_name, kv_null(), err, sizeof err)) emit_error(x, err);
}

static void dict_setbool(t_dict *x, t_symbol *path, t_floatarg f)
{
    char err[256];
    if (kv_path_set(&ROOT(x), path->s_name, kv_bool(f != 0), err, sizeof err)) emit_error(x, err);
}

static void dict_setjson(t_dict *x, t_symbol *s, int argc, t_atom *argv)
{
    char path[PATHSZ], err[256], msg[320];
    size_t len;
    char *txt;
    kv_node *n;
    (void)s;
    if (argc < 2) { emit_error(x, "setjson: need a path and JSON text"); return; }
    atom_text(&argv[0], path, PATHSZ);
    txt = atoms_join(argc - 1, argv + 1, &len);
    if (!txt) { emit_error(x, "setjson: out of memory"); return; }
    n = kv_parse(txt, len, err, sizeof err);
    free(txt);
    if (!n)
    {
        snprintf(msg, sizeof msg, "setjson: %s", err);
        emit_error(x, msg);
        return;
    }
    if (kv_path_set(&ROOT(x), path, n, err, sizeof err)) emit_error(x, err);
}

/* [json <text>( replaces the whole dictionary */
static void dict_json(t_dict *x, t_symbol *s, int argc, t_atom *argv)
{
    char err[256], msg[320];
    size_t len;
    char *txt;
    kv_node *n;
    (void)s;
    if (argc < 1) { emit_error(x, "json: need JSON text"); return; }
    txt = atoms_join(argc, argv, &len);
    if (!txt) { emit_error(x, "json: out of memory"); return; }
    n = kv_parse(txt, len, err, sizeof err);
    free(txt);
    if (!n)
    {
        snprintf(msg, sizeof msg, "json: %s", err);
        emit_error(x, msg);
        return;
    }
    kv_free(ROOT(x));
    ROOT(x) = n;
}

static void dict_get(t_dict *x, t_symbol *s, int argc, t_atom *argv)
{
    char buf[PATHSZ];
    const char *path = path_arg(argc, argv, buf);
    kv_node *n = kv_path_get(ROOT(x), path);
    (void)s;
    if (!n) emit_missing(x, path);
    else emit_node(x, n);
}

static void dict_tojson(t_dict *x, t_symbol *s, int argc, t_atom *argv)
{
    char buf[PATHSZ];
    const char *path = NULL;
    int pretty = 0, i, got_path = 0;
    kv_node *n;
    (void)s;
    for (i = 0; i < argc; i++)
    {
        if (argv[i].a_type == A_SYMBOL && !strcmp(argv[i].a_w.w_symbol->s_name, "pretty")) pretty = 1;
        else if (!got_path)
        {
            atom_text(&argv[i], buf, PATHSZ);
            path = buf;
            got_path = 1;
        }
    }
    if (!path) path = ".";
    n = kv_path_get(ROOT(x), path);
    if (!n) emit_missing(x, path);
    else emit_json_node(x, n, pretty);
}

static void dict_bang(t_dict *x)
{
    emit_json_node(x, ROOT(x), 0);
}

static void dict_has(t_dict *x, t_symbol *s, int argc, t_atom *argv)
{
    char buf[PATHSZ];
    const char *path = path_arg(argc, argv, buf);
    t_atom *a = abuf_new(2);
    (void)s;
    SETSYMBOL(&a[0], gensym(path));
    SETFLOAT(&a[1], kv_path_get(ROOT(x), path) ? 1 : 0);
    emit_info(x, "has", a, 2);
    abuf_free(a, 2);
}

static void dict_type(t_dict *x, t_symbol *s, int argc, t_atom *argv)
{
    char buf[PATHSZ];
    const char *path = path_arg(argc, argv, buf);
    t_atom *a = abuf_new(2);
    (void)s;
    SETSYMBOL(&a[0], gensym(path));
    SETSYMBOL(&a[1], gensym(kv_type_name(kv_path_get(ROOT(x), path))));
    emit_info(x, "type", a, 2);
    abuf_free(a, 2);
}

static void dict_keys(t_dict *x, t_symbol *s, int argc, t_atom *argv)
{
    char buf[PATHSZ];
    const char *path = path_arg(argc, argv, buf);
    kv_node *n = kv_path_get(ROOT(x), path);
    t_atom *a;
    int count, i;
    (void)s;
    if (!n) { emit_missing(x, path); return; }
    if (n->type != KV_OBJ) { emit_error(x, "keys: not an object"); return; }
    count = (int)n->len;
    a = abuf_new(count + 1);
    SETSYMBOL(&a[0], gensym(path));
    for (i = 0; i < count; i++) SETSYMBOL(&a[i + 1], gensym(n->members[i].key));
    emit_info(x, "keys", a, count + 1);
    abuf_free(a, count + 1);
}

static void dict_size(t_dict *x, t_symbol *s, int argc, t_atom *argv)
{
    char buf[PATHSZ];
    const char *path = path_arg(argc, argv, buf);
    kv_node *n = kv_path_get(ROOT(x), path);
    t_atom *a;
    (void)s;
    if (!n) { emit_missing(x, path); return; }
    a = abuf_new(2);
    SETSYMBOL(&a[0], gensym(path));
    SETFLOAT(&a[1], (n->type == KV_ARR || n->type == KV_OBJ || n->type == KV_STR) ? (t_float)n->len : 1);
    emit_info(x, "size", a, 2);
    abuf_free(a, 2);
}

static void dict_del(t_dict *x, t_symbol *s, int argc, t_atom *argv)
{
    char buf[PATHSZ];
    const char *path = path_arg(argc, argv, buf);
    (void)s;
    if (!strcmp(path, "."))
    {
        kv_free(ROOT(x));
        ROOT(x) = kv_obj();
        return;
    }
    if (!kv_path_del(ROOT(x), path)) emit_missing(x, path);
}

static void dict_clear(t_dict *x)
{
    kv_free(ROOT(x));
    ROOT(x) = kv_obj();
}

/* append to a list, creating it if needed. Several atoms are added as one nested list. */
static void dict_push(t_dict *x, t_symbol *s, int argc, t_atom *argv)
{
    char path[PATHSZ], err[256];
    kv_node *n, *item;
    (void)s;
    if (argc < 2) { emit_error(x, "push: need a path and a value"); return; }
    atom_text(&argv[0], path, PATHSZ);
    item = atoms_to_node(argc - 1, argv + 1);
    n = kv_path_get(ROOT(x), path);
    if (!n)
    {
        kv_node *arr = kv_arr();
        kv_arr_push(arr, item);
        if (kv_path_set(&ROOT(x), path, arr, err, sizeof err)) emit_error(x, err);
    }
    else if (n->type == KV_ARR) kv_arr_push(n, item);
    else
    {
        kv_free(item);
        emit_error(x, "push: that is not a list");
    }
}

/* ------------------------------------------------------------ files */

static void full_path(t_dict *x, const char *name, char *full)
{
    if (x->x_canvas) canvas_makefilename(x->x_canvas, name, full, MAXPDSTRING);
    else
    {
        strncpy(full, name, MAXPDSTRING - 1);
        full[MAXPDSTRING - 1] = 0;
    }
}

static char *read_all(const char *path, size_t *len, char *err, size_t errsize)
{
    FILE *f = sys_fopen(path, "rb");
    size_t cap = 65536, n = 0, got;
    char *buf;
    if (!f) { snprintf(err, errsize, "cannot open %s", path); return NULL; }
    buf = (char *)malloc(cap);
    if (!buf) { sys_fclose(f); snprintf(err, errsize, "out of memory"); return NULL; }
    for (;;)
    {
        got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (n < cap) break;
        if (cap >= KVX_MAXFILE)
        {
            snprintf(err, errsize, "%s is too big", path);
            free(buf);
            sys_fclose(f);
            return NULL;
        }
        cap *= 2;
        {
            char *q = (char *)realloc(buf, cap);
            if (!q) { free(buf); sys_fclose(f); snprintf(err, errsize, "out of memory"); return NULL; }
            buf = q;
        }
    }
    if (ferror(f))
    {
        snprintf(err, errsize, "error reading %s", path);
        free(buf);
        sys_fclose(f);
        return NULL;
    }
    sys_fclose(f);
    *len = n;
    return buf;
}

static void emit_done(t_dict *x, const char *what, t_symbol *file)
{
    t_atom *a = abuf_new(1);
    SETSYMBOL(a, file);
    emit_info(x, what, a, 1);
    abuf_free(a, 1);
}

static void dict_read(t_dict *x, t_symbol *file)
{
    char full[MAXPDSTRING], err[1100], msg[1400];
    size_t len = 0;
    char *buf;
    kv_node *n;
    full_path(x, file->s_name, full);
    buf = read_all(full, &len, err, sizeof err);
    if (!buf) { emit_error(x, err); return; }
    n = kv_parse(buf, len, err, sizeof err);
    free(buf);
    if (!n)
    {
        snprintf(msg, sizeof msg, "read %s: %s", file->s_name, err);
        emit_error(x, msg);
        return;
    }
    kv_free(ROOT(x));
    ROOT(x) = n;
    emit_done(x, "read", file);
}

static void dict_write(t_dict *x, t_symbol *s, int argc, t_atom *argv)
{
    char full[MAXPDSTRING], msg[1400];
    FILE *f;
    char *text;
    size_t len;
    int pretty = 0, i;
    t_symbol *file = NULL;
    (void)s;
    for (i = 0; i < argc; i++)
    {
        if (argv[i].a_type != A_SYMBOL) continue;
        if (!strcmp(argv[i].a_w.w_symbol->s_name, "pretty")) pretty = 1;
        else if (!file) file = argv[i].a_w.w_symbol;
    }
    if (!file) { emit_error(x, "write: need a file name"); return; }
    full_path(x, file->s_name, full);
    text = kv_dump(ROOT(x), pretty, &len);
    f = sys_fopen(full, "wb");
    if (!f)
    {
        free(text);
        snprintf(msg, sizeof msg, "cannot write %s", full);
        emit_error(x, msg);
        return;
    }
    if (fwrite(text, 1, len, f) != len || fputc('\n', f) == EOF)
    {
        sys_fclose(f);
        free(text);
        snprintf(msg, sizeof msg, "error writing %s", full);
        emit_error(x, msg);
        return;
    }
    sys_fclose(f);
    free(text);
    emit_done(x, "write", file);
}

/* print the (pretty) JSON of a path in the Pd window */
static void dict_print(t_dict *x, t_symbol *s, int argc, t_atom *argv)
{
    char buf[PATHSZ];
    const char *path = path_arg(argc, argv, buf);
    kv_node *n = kv_path_get(ROOT(x), path);
    char *text, *line, *nl;
    (void)s;
    if (!n) { emit_missing(x, path); return; }
    text = kv_dump(n, 1, NULL);
    for (line = text; line; line = nl ? nl + 1 : NULL)
    {
        nl = strchr(line, '\n');
        if (nl) *nl = 0;
        post("%.900s", line);
    }
    free(text);
}

/* Pd's network input holds about 4 kB per message and 1000 characters per word, so
 * a big JSON text arrives in pieces: [jsonbuf <piece> <piece>( for each message ([jsonbuf( alone clears), then
 * [jsonapply [path](. pdkv does this by itself for large values. */
static void dict_jsonbuf(t_dict *x, t_symbol *s, int argc, t_atom *argv)
{
    size_t len;
    char *txt, *nb;
    (void)s;
    if (argc < 1)
    {
        free(x->x_buf);
        x->x_buf = NULL;
        x->x_buflen = 0;
        return;
    }
    txt = atoms_join_with(argc, argv, "", &len);   /* pieces are glued together as they are */
    if (!txt) { emit_error(x, "jsonbuf: out of memory"); return; }
    if (x->x_buflen + len > KVX_MAXFILE)
    {
        free(txt);
        emit_error(x, "jsonbuf: too much text");
        return;
    }
    nb = (char *)realloc(x->x_buf, x->x_buflen + len + 1);
    if (!nb) { free(txt); emit_error(x, "jsonbuf: out of memory"); return; }
    memcpy(nb + x->x_buflen, txt, len + 1);
    x->x_buf = nb;
    x->x_buflen += len;
    free(txt);
}

static void dict_jsonapply(t_dict *x, t_symbol *s, int argc, t_atom *argv)
{
    char buf[PATHSZ], err[256], msg[320];
    const char *path = path_arg(argc, argv, buf);
    kv_node *n;
    (void)s;
    if (!x->x_buf) { emit_error(x, "jsonapply: nothing in the buffer"); return; }
    n = kv_parse(x->x_buf, x->x_buflen, err, sizeof err);
    free(x->x_buf);
    x->x_buf = NULL;
    x->x_buflen = 0;
    if (!n)
    {
        snprintf(msg, sizeof msg, "jsonapply: %s", err);
        emit_error(x, msg);
        return;
    }
    if (!strcmp(path, "."))
    {
        kv_free(ROOT(x));
        ROOT(x) = n;
    }
    else if (kv_path_set(&ROOT(x), path, n, err, sizeof err)) emit_error(x, err);
}

/* ------------------------------------------------------------ requests */

/* [req <id> <message...>( runs the message and sends the answer, tagged with the
 * id, out of the third outlet as [send reply <id> <kind> ...( so it can go straight into
 * [netsend]. Meant for a bridge such as Python over [netreceive] and [netsend]. */
static void dict_req(t_dict *x, t_symbol *s, int argc, t_atom *argv)
{
    (void)s;
    if (argc < 2 || x->x_inreq) return;
    x->x_replyid = argv[0];
    x->x_inreq = 1;
    x->x_replied = 0;
    if (argv[1].a_type == A_SYMBOL)
        pd_typedmess((t_pd *)x, argv[1].a_w.w_symbol, argc - 2, argv + 2);
    else
        pd_typedmess((t_pd *)x, &s_list, argc - 1, argv + 1);
    if (!x->x_replied)
    {
        t_atom *a = abuf_new(0);
        send_reply(x, gensym("ok"), a, 0);
        abuf_free(a, 0);
    }
    x->x_inreq = 0;
}

/* ------------------------------------------------------------ class */

static void *dict_new(t_symbol *s, int argc, t_atom *argv)
{
    static int announced = 0;
    t_dict *x = (t_dict *)pd_new(dict_class);
    t_symbol *name = NULL;
    (void)s;
    if (argc > 0)
    {
        if (argv[0].a_type == A_SYMBOL) name = argv[0].a_w.w_symbol;
        else
        {
            char buf[64];
            atom_string(&argv[0], buf, sizeof buf);   /* e.g. $0 */
            name = gensym(buf);
        }
    }
    x->x_canvas = canvas_getcurrent();
    x->x_store = store_acquire(name);
    x->x_inreq = 0;
    x->x_replied = 0;
    x->x_buf = NULL;
    x->x_buflen = 0;
    SETFLOAT(&x->x_replyid, 0);
    x->x_out = outlet_new(&x->x_obj, 0);
    x->x_info = outlet_new(&x->x_obj, 0);
    x->x_reply = outlet_new(&x->x_obj, 0);
    if (!announced)
    {
        announced = 1;
        post("kvx " KVX_VERSION " - [kvx/dict], a JSON dictionary");
    }
    return x;
}

static void dict_free(t_dict *x)
{
    free(x->x_buf);
    store_release(x->x_store);
}

void dict_setup(void)
{
    dict_class = class_new(gensym("dict"), (t_newmethod)dict_new, (t_method)dict_free,
        sizeof(t_dict), CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(dict_class, dict_bang);
    class_addmethod(dict_class, (t_method)dict_set, gensym("set"), A_GIMME, 0);
    class_addmethod(dict_class, (t_method)dict_setnull, gensym("setnull"), A_SYMBOL, 0);
    class_addmethod(dict_class, (t_method)dict_setbool, gensym("setbool"), A_SYMBOL, A_FLOAT, 0);
    class_addmethod(dict_class, (t_method)dict_setjson, gensym("setjson"), A_GIMME, 0);
    class_addmethod(dict_class, (t_method)dict_json, gensym("json"), A_GIMME, 0);
    class_addmethod(dict_class, (t_method)dict_get, gensym("get"), A_GIMME, 0);
    class_addmethod(dict_class, (t_method)dict_tojson, gensym("tojson"), A_GIMME, 0);
    class_addmethod(dict_class, (t_method)dict_has, gensym("has"), A_GIMME, 0);
    class_addmethod(dict_class, (t_method)dict_type, gensym("type"), A_GIMME, 0);
    class_addmethod(dict_class, (t_method)dict_keys, gensym("keys"), A_GIMME, 0);
    class_addmethod(dict_class, (t_method)dict_size, gensym("size"), A_GIMME, 0);
    class_addmethod(dict_class, (t_method)dict_del, gensym("del"), A_GIMME, 0);
    class_addmethod(dict_class, (t_method)dict_clear, gensym("clear"), 0);
    class_addmethod(dict_class, (t_method)dict_push, gensym("push"), A_GIMME, 0);
    class_addmethod(dict_class, (t_method)dict_read, gensym("read"), A_SYMBOL, 0);
    class_addmethod(dict_class, (t_method)dict_write, gensym("write"), A_GIMME, 0);
    class_addmethod(dict_class, (t_method)dict_print, gensym("print"), A_GIMME, 0);
    class_addmethod(dict_class, (t_method)dict_jsonbuf, gensym("jsonbuf"), A_GIMME, 0);
    class_addmethod(dict_class, (t_method)dict_jsonapply, gensym("jsonapply"), A_GIMME, 0);
    class_addmethod(dict_class, (t_method)dict_req, gensym("req"), A_GIMME, 0);
}
