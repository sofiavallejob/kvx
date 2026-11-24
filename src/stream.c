/*
 * [kvx/stream] - a ring buffer for live data. Rows come in from Pd or from
 * another program (Python, a sensor bridge) and the newest ones are kept.
 * Ask for the last row, statistics over the last n rows, or copy the last
 * n values of a column into a Pd array to draw or play it.
 *
 * Copyright (c) 2026 Sofia Vallejo Budziszewski. BSD 3-Clause.
 */
#include "m_pd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static t_class *stream_class;

typedef struct _stream {
    t_object x_obj;
    int x_ncols;
    long x_size;            /* rows kept */
    long x_head;            /* where the next row goes */
    long x_count;           /* rows held, up to x_size */
    double x_total;         /* rows received since the last clear */
    t_float *x_buf;         /* x_size * x_ncols */
    t_symbol **x_names;
    int x_thru;
    t_outlet *x_out, *x_info;
} t_stream;

static void st_error(t_stream *x, const char *msg)
{
    t_atom a;
    SETSYMBOL(&a, gensym(msg));
    pd_error(x, "kvx/stream: %s", msg);
    outlet_anything(x->x_info, gensym("error"), 1, &a);
}

static void st_free_buf(t_stream *x)
{
    if (x->x_buf) freebytes(x->x_buf, x->x_size * x->x_ncols * sizeof(t_float));
    if (x->x_names) freebytes(x->x_names, x->x_ncols * sizeof(t_symbol *));
    x->x_buf = NULL;
    x->x_names = NULL;
}

static void st_alloc(t_stream *x, int ncols, long size)
{
    int i;
    st_free_buf(x);
    x->x_ncols = ncols;
    x->x_size = size;
    x->x_head = x->x_count = 0;
    x->x_total = 0;
    if (ncols < 1) return;
    x->x_buf = (t_float *)getbytes(size * ncols * sizeof(t_float));
    x->x_names = (t_symbol **)getbytes(ncols * sizeof(t_symbol *));
    for (i = 0; i < ncols; i++)
    {
        char b[32];
        snprintf(b, sizeof b, "col%d", i + 1);
        x->x_names[i] = gensym(b);
    }
}

/* row k counted back from the newest (0 = newest) */
static t_float *row_ptr(t_stream *x, long k)
{
    long r = x->x_head - 1 - k;
    while (r < 0) r += x->x_size;
    return x->x_buf + r * x->x_ncols;
}

static int find_col(t_stream *x, t_atom *a)
{
    int i;
    if (a->a_type == A_FLOAT)
    {
        i = (int)a->a_w.w_float;
        return (i >= 0 && i < x->x_ncols) ? i : -1;
    }
    if (a->a_type == A_SYMBOL)
        for (i = 0; i < x->x_ncols; i++) if (x->x_names[i] == a->a_w.w_symbol) return i;
    return -1;
}

static void out_row(t_stream *x, t_outlet *o, long k)
{
    t_atom *a = (t_atom *)getbytes(x->x_ncols * sizeof(t_atom));
    t_float *p = row_ptr(x, k);
    int i;
    for (i = 0; i < x->x_ncols; i++) SETFLOAT(&a[i], p[i]);
    if (x->x_ncols == 1) outlet_float(o, p[0]);
    else outlet_list(o, &s_list, x->x_ncols, a);
    freebytes(a, x->x_ncols * sizeof(t_atom));
}

/* push v1 v2 ... : one row, or several rows one after the other */
static void stream_push(t_stream *x, t_symbol *s, int argc, t_atom *argv)
{
    int i, j, nrows;
    (void)s;
    if (argc < 1) return;
    if (x->x_ncols < 1) st_alloc(x, argc, x->x_size);
    if (argc % x->x_ncols)
    {
        char m[120];
        snprintf(m, sizeof m, "push needs a multiple of %d values", x->x_ncols);
        st_error(x, m);
        return;
    }
    nrows = argc / x->x_ncols;
    for (j = 0; j < nrows; j++)
    {
        t_float *p = x->x_buf + x->x_head * x->x_ncols;
        for (i = 0; i < x->x_ncols; i++) p[i] = atom_getfloat(&argv[j * x->x_ncols + i]);
        x->x_head = (x->x_head + 1) % x->x_size;
        if (x->x_count < x->x_size) x->x_count++;
        x->x_total++;
        if (x->x_thru) out_row(x, x->x_out, 0);
    }
}

static void stream_float(t_stream *x, t_floatarg f)
{
    t_atom a;
    SETFLOAT(&a, f);
    stream_push(x, &s_list, 1, &a);
}

static void stream_bang(t_stream *x)
{
    if (!x->x_count) return;      /* nothing yet: stay quiet, live data may not have started */
    out_row(x, x->x_out, 0);
}

/* get k : the row k steps back (0 = newest) */
static void stream_get(t_stream *x, t_floatarg f)
{
    long k = (long)f;
    if (k < 0 || k >= x->x_count) { st_error(x, "not that many rows yet"); return; }
    out_row(x, x->x_out, k);
}

static void stream_columns(t_stream *x, t_symbol *s, int argc, t_atom *argv)
{
    int i;
    (void)s;
    if (!argc)
    {
        t_atom *a = (t_atom *)getbytes((x->x_ncols ? x->x_ncols : 1) * sizeof(t_atom));
        for (i = 0; i < x->x_ncols; i++) SETSYMBOL(&a[i], x->x_names[i]);
        outlet_anything(x->x_info, gensym("columns"), x->x_ncols, a);
        freebytes(a, (x->x_ncols ? x->x_ncols : 1) * sizeof(t_atom));
        return;
    }
    if (argc != x->x_ncols) st_alloc(x, argc, x->x_size);
    for (i = 0; i < argc; i++)
    {
        char b[MAXPDSTRING];
        atom_string(&argv[i], b, sizeof b);
        x->x_names[i] = gensym(b);
    }
}

static void stream_rows(t_stream *x)
{
    t_atom a[2];
    SETFLOAT(&a[0], (t_float)x->x_count);
    SETFLOAT(&a[1], (t_float)x->x_total);
    outlet_anything(x->x_info, gensym("rows"), 2, a);
}

static long last_n(t_stream *x, int argc, t_atom *argv, int at)
{
    long n = x->x_count;
    if (argc > at && argv[at].a_type == A_FLOAT && argv[at].a_w.w_float >= 1) n = (long)argv[at].a_w.w_float;
    if (n > x->x_count) n = x->x_count;
    return n;
}

/* stats column [n] -> stats column min max mean std count */
static void stream_stats(t_stream *x, t_symbol *s, int argc, t_atom *argv)
{
    int col;
    long n, k;
    double mn = 0, mx = 0, mean = 0, m2 = 0;
    t_atom a[6];
    (void)s;
    if (argc < 1 || (col = find_col(x, &argv[0])) < 0) { st_error(x, "usage: stats column [rows]"); return; }
    if (!(n = last_n(x, argc, argv, 1))) return;
    for (k = 0; k < n; k++)
    {
        double v = row_ptr(x, k)[col], d;
        if (!k || v < mn) mn = v;
        if (!k || v > mx) mx = v;
        d = v - mean;
        mean += d / (k + 1);
        m2 += d * (v - mean);
    }
    SETSYMBOL(&a[0], x->x_names[col]);
    SETFLOAT(&a[1], (t_float)mn);
    SETFLOAT(&a[2], (t_float)mx);
    SETFLOAT(&a[3], (t_float)mean);
    SETFLOAT(&a[4], (t_float)sqrt(m2 / n));
    SETFLOAT(&a[5], (t_float)n);
    outlet_anything(x->x_info, gensym("stats"), 6, a);
}

/* toarray column arrayname [n] [norm] : oldest to newest */
static void stream_toarray(t_stream *x, t_symbol *s, int argc, t_atom *argv)
{
    int col, norm = 0, size;
    long n, k;
    t_garray *g;
    t_word *vec;
    double mn = 0, mx = 0;
    (void)s;
    if (argc < 2 || (col = find_col(x, &argv[0])) < 0 || argv[1].a_type != A_SYMBOL)
    { st_error(x, "usage: toarray column arrayname [rows] [norm]"); return; }
    if (argc > 2 && argv[argc - 1].a_type == A_SYMBOL && !strcmp(argv[argc - 1].a_w.w_symbol->s_name, "norm")) norm = 1;
    n = last_n(x, argc, argv, 2);
    if (!n) return;
    if (!(g = (t_garray *)pd_findbyclass(argv[1].a_w.w_symbol, garray_class)))
    {
        char m[MAXPDSTRING + 32];
        snprintf(m, sizeof m, "no array called %s", argv[1].a_w.w_symbol->s_name);
        st_error(x, m);
        return;
    }
    if (norm)
        for (k = 0; k < n; k++)
        {
            double v = row_ptr(x, k)[col];
            if (!k || v < mn) mn = v;
            if (!k || v > mx) mx = v;
        }
    garray_resize_long(g, n > 0 ? n : 1);
    if (!garray_getfloatwords(g, &size, &vec) || size < n) { st_error(x, "could not resize the array"); return; }
    for (k = 0; k < n; k++)
    {
        double v = row_ptr(x, n - 1 - k)[col];
        if (norm) v = mx > mn ? (v - (mx + mn) / 2) * 2 / (mx - mn) : 0;
        vec[k].w_float = (t_float)v;
    }
    if (!n) vec[0].w_float = 0;
    garray_redraw(g);
}

static void stream_thru(t_stream *x, t_floatarg f) { x->x_thru = f != 0; }

static void stream_clear(t_stream *x)
{
    x->x_head = x->x_count = 0;
    x->x_total = 0;
}

static void stream_size(t_stream *x, t_floatarg f)
{
    long n = (long)f;
    if (n < 1) n = 1;
    if (n > 50000000) n = 50000000;
    {
        t_symbol **old = NULL;
        int nc = x->x_ncols, i;
        if (nc > 0)
        {
            old = (t_symbol **)getbytes(nc * sizeof(t_symbol *));
            memcpy(old, x->x_names, nc * sizeof(t_symbol *));
        }
        st_alloc(x, nc, n);
        if (old)
        {
            for (i = 0; i < nc; i++) x->x_names[i] = old[i];
            freebytes(old, nc * sizeof(t_symbol *));
        }
    }
}

/* [kvx/stream columns size], e.g. [kvx/stream 3 10000]. Both optional. */
static void *stream_new(t_floatarg ncols, t_floatarg size)
{
    t_stream *x = (t_stream *)pd_new(stream_class);
    x->x_buf = NULL;
    x->x_names = NULL;
    x->x_thru = 0;
    x->x_ncols = 0;
    x->x_size = size >= 1 ? (long)size : 10000;
    if (x->x_size > 50000000) x->x_size = 50000000;
    st_alloc(x, ncols >= 1 ? (int)ncols : 0, x->x_size);
    x->x_out = outlet_new(&x->x_obj, 0);
    x->x_info = outlet_new(&x->x_obj, 0);
    return x;
}

static void stream_free(t_stream *x)
{
    st_free_buf(x);
}

void stream_setup(void)
{
    stream_class = class_new(gensym("stream"), (t_newmethod)stream_new, (t_method)stream_free,
        sizeof(t_stream), CLASS_DEFAULT, A_DEFFLOAT, A_DEFFLOAT, 0);
    class_addbang(stream_class, stream_bang);
    class_addfloat(stream_class, stream_float);
    class_addlist(stream_class, (t_method)stream_push);
    class_addmethod(stream_class, (t_method)stream_push, gensym("push"), A_GIMME, 0);
    class_addmethod(stream_class, (t_method)stream_get, gensym("get"), A_FLOAT, 0);
    class_addmethod(stream_class, (t_method)stream_columns, gensym("columns"), A_GIMME, 0);
    class_addmethod(stream_class, (t_method)stream_rows, gensym("rows"), 0);
    class_addmethod(stream_class, (t_method)stream_stats, gensym("stats"), A_GIMME, 0);
    class_addmethod(stream_class, (t_method)stream_toarray, gensym("toarray"), A_GIMME, 0);
    class_addmethod(stream_class, (t_method)stream_thru, gensym("thru"), A_FLOAT, 0);
    class_addmethod(stream_class, (t_method)stream_clear, gensym("clear"), 0);
    class_addmethod(stream_class, (t_method)stream_size, gensym("size"), A_FLOAT, 0);
}
