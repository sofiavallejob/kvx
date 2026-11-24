/*
 * [kvx/table] - reads CSV/TSV files straight into Pd, for sonification of
 * long data series. Columns are kept as arrays of numbers, so big files
 * stay small, and can be stepped through, played back at a rate, or copied
 * into a Pd array in one go.
 *
 * Copyright (c) 2026 Sofia Vallejo Budziszewski. BSD 3-Clause.
 */
#include "m_pd.h"
#include "csv_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define KVX_VERSION "0.2.0"

static t_class *table_class;

typedef struct _table {
    t_object x_obj;
    t_canvas *x_canvas;
    kv_csv *x_csv;
    long x_cursor;
    t_float x_nan;          /* value sent for empty cells */
    t_clock *x_clock;
    t_float x_rate;         /* ms per row while playing */
    int x_playing;
    t_outlet *x_out, *x_info;
} t_table;

/* ---------------------------------------------------------------- helpers */

static void tbl_error(t_table *x, const char *msg)
{
    t_atom a;
    SETSYMBOL(&a, gensym(msg));
    pd_error(x, "kvx/table: %s", msg);
    outlet_anything(x->x_info, gensym("error"), 1, &a);
}

static int need_data(t_table *x)
{
    if (!x->x_csv) { tbl_error(x, "no data loaded, send [read file.csv("); return 0; }
    return 1;
}

static int find_col(t_table *x, t_atom *a)
{
    kv_csv *c = x->x_csv;
    if (a->a_type == A_FLOAT)
    {
        int i = (int)a->a_w.w_float;
        if (i >= 0 && i < c->ncols) return i;
        return -1;
    }
    if (a->a_type == A_SYMBOL) return kv_csv_find(c, a->a_w.w_symbol->s_name);
    return -1;
}

static void cell_atom(t_table *x, long r, int col, t_atom *a)
{
    kv_csv *c = x->x_csv;
    if (c->txt[col] && c->txt[col][r]) SETSYMBOL(a, gensym(c->txt[col][r]));
    else
    {
        double v = c->num[col][r];
        /* bit test, because -ffast-math may remove isnan() */
        SETFLOAT(a, kv_isnan(v) ? x->x_nan : (t_float)v);
    }
}

static void output_row(t_table *x, long r)
{
    kv_csv *c = x->x_csv;
    int i;
    t_atom *a = (t_atom *)getbytes(c->ncols * sizeof(t_atom));
    for (i = 0; i < c->ncols; i++) cell_atom(x, r, i, &a[i]);
    if (c->ncols == 1 && a[0].a_type == A_FLOAT) outlet_float(x->x_out, a[0].a_w.w_float);
    else outlet_list(x->x_out, &s_list, c->ncols, a);
    freebytes(a, c->ncols * sizeof(t_atom));
}

static void table_free_data(t_table *x)
{
    kv_csv_free(x->x_csv);
    x->x_csv = NULL;
    x->x_cursor = 0;
}

/* ---------------------------------------------------------------- reading */

static const char *flag_name(t_atom *a)
{
    if (a->a_type == A_SYMBOL && a->a_w.w_symbol->s_name[0] == '-' && a->a_w.w_symbol->s_name[1])
        return a->a_w.w_symbol->s_name + 1;
    return NULL;
}

static void table_read(t_table *x, t_symbol *s, int argc, t_atom *argv)
{
    kv_csv_opts o;
    char full[MAXPDSTRING], err[1100], **sel = NULL;
    FILE *f;
    kv_csv *c;
    int i, nsel = 0;
    t_atom a[2];
    (void)s;
    kv_csv_opts_default(&o);
    if (argc < 1 || argv[0].a_type != A_SYMBOL) { tbl_error(x, "usage: read file.csv [-stride n] [-cols a b c] [-maxrows n] [-skip n] [-delim comma|semicolon|tab] [-header 0|1] [-decimal point|comma] [-reltime 0|1]"); return; }
    if (x->x_canvas) canvas_makefilename(x->x_canvas, argv[0].a_w.w_symbol->s_name, full, MAXPDSTRING);
    else snprintf(full, MAXPDSTRING, "%s", argv[0].a_w.w_symbol->s_name);

    sel = (char **)getbytes((argc + 1) * sizeof(char *));
    for (i = 1; i < argc; i++)
    {
        const char *fl = flag_name(&argv[i]);
        if (!fl) { tbl_error(x, "unexpected word in read, options start with a dash"); goto bad; }
        if (!strcmp(fl, "cols"))
        {
            int k = i + 1;
            while (k < argc && !flag_name(&argv[k]))
            {
                char buf[256];
                atom_string(&argv[k], buf, sizeof buf);
                sel[nsel] = (char *)malloc(strlen(buf) + 1);
                strcpy(sel[nsel], buf);
                nsel++;
                k++;
            }
            if (!nsel) { tbl_error(x, "-cols needs at least one column"); goto bad; }
            i = k - 1;
        }
        else
        {
            if (i + 1 >= argc) { tbl_error(x, "option without a value"); goto bad; }
            if (!strcmp(fl, "stride")) o.stride = (long)atom_getfloat(&argv[++i]);
            else if (!strcmp(fl, "maxrows")) o.maxrows = (long)atom_getfloat(&argv[++i]);
            else if (!strcmp(fl, "skip")) o.skip = (long)atom_getfloat(&argv[++i]);
            else if (!strcmp(fl, "header")) o.header = atom_getfloat(&argv[++i]) != 0;
            else if (!strcmp(fl, "reltime")) o.reltime = atom_getfloat(&argv[++i]) != 0;
            else if (!strcmp(fl, "decimal"))
            {
                t_symbol *d = atom_getsymbol(&argv[++i]);
                if (!strcmp(d->s_name, "comma")) o.decimal_comma = 1;
                else if (!strcmp(d->s_name, "point")) o.decimal_comma = 0;
                else { tbl_error(x, "-decimal is point or comma"); goto bad; }
            }
            else if (!strcmp(fl, "delim"))
            {
                t_symbol *d = atom_getsymbol(&argv[++i]);
                if (!strcmp(d->s_name, "comma") || !strcmp(d->s_name, ",")) o.delim = ',';
                else if (!strcmp(d->s_name, "semicolon") || !strcmp(d->s_name, ";")) o.delim = ';';
                else if (!strcmp(d->s_name, "tab")) o.delim = '\t';
                else { tbl_error(x, "-delim is comma, semicolon or tab"); goto bad; }
            }
            else { tbl_error(x, "unknown option for read"); goto bad; }
        }
    }
    if (o.stride < 1) o.stride = 1;
    o.ncolsel = nsel;
    o.colsel = (const char **)sel;

    f = sys_fopen(full, "rb");
    if (!f) { snprintf(err, sizeof err, "cannot open %s", full); tbl_error(x, err); goto bad; }
    c = kv_csv_read(f, &o, err, sizeof err);
    sys_fclose(f);
    if (!c) { tbl_error(x, err); goto bad; }

    x->x_playing = 0;
    clock_unset(x->x_clock);
    table_free_data(x);
    x->x_csv = c;
    post("kvx/table: %ld rows x %d columns from %s%s", c->nrows, c->ncols, argv[0].a_w.w_symbol->s_name,
        o.stride > 1 ? " (every nth row kept)" : "");
    SETFLOAT(&a[0], (t_float)c->nrows);
    SETFLOAT(&a[1], (t_float)c->ncols);
    outlet_anything(x->x_info, gensym("loaded"), 2, a);
    for (i = 0; i < c->ncols; i++)
        if (c->is_time[i] && o.reltime)
        {
            char ts[64];
            t_atom ta[2];
            snprintf(ts, sizeof ts, "%.3f", c->t0[i]);
            SETSYMBOL(&ta[0], gensym(c->name[i]));
            SETSYMBOL(&ta[1], gensym(ts));
            outlet_anything(x->x_info, gensym("timeorigin"), 2, ta);
        }
bad:
    for (i = 0; i < nsel; i++) free(sel[i]);
    freebytes(sel, (argc + 1) * sizeof(char *));
}

/* ---------------------------------------------------------------- queries */

static void table_columns(t_table *x)
{
    int i;
    t_atom *a;
    if (!need_data(x)) return;
    a = (t_atom *)getbytes(x->x_csv->ncols * sizeof(t_atom));
    for (i = 0; i < x->x_csv->ncols; i++) SETSYMBOL(&a[i], gensym(x->x_csv->name[i]));
    outlet_anything(x->x_info, gensym("columns"), x->x_csv->ncols, a);
    freebytes(a, x->x_csv->ncols * sizeof(t_atom));
}

static void table_rows(t_table *x)
{
    t_atom a;
    if (!need_data(x)) return;
    SETFLOAT(&a, (t_float)x->x_csv->nrows);
    outlet_anything(x->x_info, gensym("rows"), 1, &a);
}

static void table_get(t_table *x, t_symbol *s, int argc, t_atom *argv)
{
    int col;
    long r;
    t_atom a;
    (void)s;
    if (!need_data(x)) return;
    if (argc != 2 || argv[0].a_type != A_FLOAT) { tbl_error(x, "usage: get row column"); return; }
    r = (long)argv[0].a_w.w_float;
    col = find_col(x, &argv[1]);
    if (col < 0) { tbl_error(x, "no such column"); return; }
    if (r < 0 || r >= x->x_csv->nrows) { tbl_error(x, "row out of range"); return; }
    cell_atom(x, r, col, &a);
    if (a.a_type == A_FLOAT) outlet_float(x->x_out, a.a_w.w_float);
    else outlet_symbol(x->x_out, a.a_w.w_symbol);
}

static void table_row(t_table *x, t_floatarg f)
{
    long r = (long)f;
    if (!need_data(x)) return;
    if (r < 0 || r >= x->x_csv->nrows) { tbl_error(x, "row out of range"); return; }
    output_row(x, r);
}

static void table_stats(t_table *x, t_symbol *s, int argc, t_atom *argv)
{
    int col;
    long i, n = 0;
    double mn = 0, mx = 0, sum = 0, sq = 0;
    t_atom a[6];
    kv_csv *c;
    (void)s;
    if (!need_data(x)) return;
    if (argc != 1) { tbl_error(x, "usage: stats column"); return; }
    col = find_col(x, &argv[0]);
    if (col < 0) { tbl_error(x, "no such column"); return; }
    c = x->x_csv;
    for (i = 0; i < c->nrows; i++)
    {
        double v = c->num[col][i];
        double d;
        if (kv_isnan(v)) continue;
        if (!n || v < mn) mn = v;
        if (!n || v > mx) mx = v;
        n++;
        d = v - sum;      /* running mean (Welford), sum holds the mean */
        sum += d / n;
        sq += d * (v - sum);
    }
    if (!n) { tbl_error(x, "column has no numbers"); return; }
    {
        double mean = sum, var = sq / n;
        SETSYMBOL(&a[0], gensym(c->name[col]));
        SETFLOAT(&a[1], (t_float)mn);
        SETFLOAT(&a[2], (t_float)mx);
        SETFLOAT(&a[3], (t_float)mean);
        SETFLOAT(&a[4], (t_float)sqrt(var > 0 ? var : 0));
        SETFLOAT(&a[5], (t_float)n);
    }
    outlet_anything(x->x_info, gensym("stats"), 6, a);
}

static void table_nan(t_table *x, t_floatarg f) { x->x_nan = f; }

/* ------------------------------------------------------- Pd array transfer */

/* toarray column arrayname [norm]   (norm scales the column to -1..1) */
static void table_toarray(t_table *x, t_symbol *s, int argc, t_atom *argv)
{
    int col, norm = 0, n;
    long i, nr;
    t_garray *g;
    t_word *vec;
    double mn = 0, mx = 0, scale = 1, mid = 0;
    int have = 0;
    kv_csv *c;
    (void)s;
    if (!need_data(x)) return;
    if (argc < 2 || argv[1].a_type != A_SYMBOL) { tbl_error(x, "usage: toarray column arrayname [norm]"); return; }
    if (argc > 2 && argv[2].a_type == A_SYMBOL && !strcmp(argv[2].a_w.w_symbol->s_name, "norm")) norm = 1;
    col = find_col(x, &argv[0]);
    if (col < 0) { tbl_error(x, "no such column"); return; }
    g = (t_garray *)pd_findbyclass(argv[1].a_w.w_symbol, garray_class);
    if (!g) { char m[300]; snprintf(m, sizeof m, "no array called %s", argv[1].a_w.w_symbol->s_name); tbl_error(x, m); return; }
    c = x->x_csv;
    nr = c->nrows;
    if (norm)
    {
        for (i = 0; i < nr; i++)
        {
            double v = c->num[col][i];
            if (kv_isnan(v)) continue;
            if (!have || v < mn) mn = v;
            if (!have || v > mx) mx = v;
            have = 1;
        }
        if (have && mx > mn) { mid = (mx + mn) / 2; scale = 2.0 / (mx - mn); }
        else { mid = have ? mn : 0; scale = 1; }
    }
    garray_resize_long(g, nr);
    if (!garray_getfloatwords(g, &n, &vec) || n != nr) { tbl_error(x, "could not resize the array"); return; }
    for (i = 0; i < nr; i++)
    {
        double v = c->num[col][i];
        if (kv_isnan(v)) v = norm ? mid : x->x_nan;
        vec[i].w_float = (t_float)(norm ? (v - mid) * scale : v);
    }
    garray_redraw(g);
    {
        t_atom a[2];
        SETSYMBOL(&a[0], argv[1].a_w.w_symbol);
        SETFLOAT(&a[1], (t_float)nr);
        outlet_anything(x->x_info, gensym("toarray"), 2, a);
    }
}

/* ---------------------------------------------------------------- stepping */

static void table_reset(t_table *x) { x->x_cursor = 0; }

static void table_seek(t_table *x, t_floatarg f)
{
    long r = (long)f;
    if (!need_data(x)) return;
    if (r < 0 || r >= x->x_csv->nrows) { tbl_error(x, "row out of range"); return; }
    x->x_cursor = r;
}

/* output the row under the cursor and move on; "end" comes out of the right outlet after the last row */
static int step(t_table *x)
{
    if (!x->x_csv) return 0;
    if (x->x_cursor >= x->x_csv->nrows)
    {
        outlet_anything(x->x_info, gensym("end"), 0, NULL);
        return 0;
    }
    output_row(x, x->x_cursor++);
    return 1;
}

static void table_bang(t_table *x)
{
    if (!need_data(x)) return;
    if (!step(x)) { /* already reported "end" */ }
}

static void table_tick(t_table *x)
{
    if (!x->x_playing) return;
    if (step(x) && x->x_cursor < x->x_csv->nrows) clock_delay(x->x_clock, x->x_rate);
    else
    {
        x->x_playing = 0;
        if (x->x_csv && x->x_cursor >= x->x_csv->nrows) outlet_anything(x->x_info, gensym("end"), 0, NULL);
    }
}

/* play [ms_per_row]: one row per tick of the clock, from the cursor on */
static void table_play(t_table *x, t_symbol *s, int argc, t_atom *argv)
{
    (void)s;
    if (!need_data(x)) return;
    if (argc > 0 && argv[0].a_type == A_FLOAT && argv[0].a_w.w_float >= 0.1f) x->x_rate = argv[0].a_w.w_float;
    if (x->x_cursor >= x->x_csv->nrows) x->x_cursor = 0;
    x->x_playing = 1;
    clock_delay(x->x_clock, 0);
}

static void table_stop(t_table *x)
{
    x->x_playing = 0;
    clock_unset(x->x_clock);
}

static void table_clear(t_table *x)
{
    table_stop(x);
    table_free_data(x);
}

/* ---------------------------------------------------------------- class */

static void *table_new(t_symbol *s, int argc, t_atom *argv)
{
    static int announced = 0;
    t_table *x = (t_table *)pd_new(table_class);
    (void)s; (void)argc; (void)argv;
    x->x_canvas = canvas_getcurrent();
    x->x_csv = NULL;
    x->x_cursor = 0;
    x->x_nan = 0;
    x->x_rate = 10;
    x->x_playing = 0;
    x->x_clock = clock_new(x, (t_method)table_tick);
    x->x_out = outlet_new(&x->x_obj, 0);
    x->x_info = outlet_new(&x->x_obj, 0);
    if (!announced)
    {
        announced = 1;
        post("kvx " KVX_VERSION " - [kvx/table], CSV reader");
    }
    return x;
}

static void table_destroy(t_table *x)
{
    clock_free(x->x_clock);
    table_free_data(x);
}

void table_setup(void)
{
    table_class = class_new(gensym("table"), (t_newmethod)table_new, (t_method)table_destroy,
        sizeof(t_table), CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(table_class, table_bang);
    class_addmethod(table_class, (t_method)table_read, gensym("read"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_columns, gensym("columns"), 0);
    class_addmethod(table_class, (t_method)table_rows, gensym("rows"), 0);
    class_addmethod(table_class, (t_method)table_get, gensym("get"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_row, gensym("row"), A_FLOAT, 0);
    class_addmethod(table_class, (t_method)table_stats, gensym("stats"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_toarray, gensym("toarray"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_nan, gensym("nan"), A_FLOAT, 0);
    class_addmethod(table_class, (t_method)table_reset, gensym("reset"), 0);
    class_addmethod(table_class, (t_method)table_seek, gensym("seek"), A_FLOAT, 0);
    class_addmethod(table_class, (t_method)table_play, gensym("play"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_stop, gensym("stop"), 0);
    class_addmethod(table_class, (t_method)table_clear, gensym("clear"), 0);
}
