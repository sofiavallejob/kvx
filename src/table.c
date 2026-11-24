/*
 * [kvx/table] - reads CSV, TSV, NumPy (.npy) and JSON Lines files straight
 * into Pd, for sonification of long data series. Columns are kept as arrays
 * of numbers, so big files stay small. Rows can be stepped through, played on
 * a clock (at a fixed rate or following the real time stamps), scaled to a
 * range, copied into Pd arrays, thinned out, extended with computed columns,
 * recorded from Pd and written back to CSV.
 *
 * Copyright (c) 2026 Sofia Vallejo Budziszewski. BSD 3-Clause.
 */
#include "m_pd.h"
#include "csv_core.h"
#include "csv_ops.h"
#include "kv_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define KVX_VERSION "0.3.0"
#define MAXBURST 100000     /* most rows sent in one clock tick */

static t_class *table_class;

typedef struct _table {
    t_object x_obj;
    t_canvas *x_canvas;
    kv_csv *x_csv;
    long x_cursor;
    t_float x_nan;          /* value sent for empty cells */
    /* scaling of columns on the way out: out = a * v + b */
    int x_rngn;
    char *x_rngon;
    double *x_rnga, *x_rngb;
    /* playback */
    t_clock *x_clock;
    int x_playing;
    int x_loop;
    int x_bytime;           /* 1 = follow a time column */
    int x_tcol;
    double x_rate;          /* ms per row when not following time */
    double x_speed;
    double x_anchor_sys;    /* logical time when the anchor row was due */
    double x_anchor_data;   /* data time (seconds) of the anchor row */
    t_outlet *x_out, *x_info, *x_pos;
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
    if (!x->x_csv || !x->x_csv->ncols) { tbl_error(x, "no data loaded, send [read file.csv("); return 0; }
    return 1;
}

static int find_col(t_table *x, t_atom *a)
{
    kv_csv *c = x->x_csv;
    if (!c) return -1;
    if (a->a_type == A_FLOAT)
    {
        int i = (int)a->a_w.w_float;
        if (i >= 0 && i < c->ncols) return i;
        return -1;
    }
    if (a->a_type == A_SYMBOL) return kv_csv_find(c, a->a_w.w_symbol->s_name);
    return -1;
}

static int col_or_error(t_table *x, t_atom *a)
{
    int col = find_col(x, a);
    if (col < 0)
    {
        char buf[80], m[160];
        atom_string(a, buf, sizeof buf);
        snprintf(m, sizeof m, "no column %s", buf);
        tbl_error(x, m);
    }
    return col;
}

/* keep the scaling arrays as long as the number of columns */
static void sync_cols(t_table *x)
{
    int n = x->x_csv ? x->x_csv->ncols : 0, i;
    if (n == x->x_rngn) return;
    if (n > x->x_rngn)
    {
        x->x_rngon = (char *)resizebytes(x->x_rngon, x->x_rngn, n);
        x->x_rnga = (double *)resizebytes(x->x_rnga, x->x_rngn * sizeof(double), n * sizeof(double));
        x->x_rngb = (double *)resizebytes(x->x_rngb, x->x_rngn * sizeof(double), n * sizeof(double));
        for (i = x->x_rngn; i < n; i++) { x->x_rngon[i] = 0; x->x_rnga[i] = 1; x->x_rngb[i] = 0; }
    }
    else
    {
        x->x_rngon = (char *)resizebytes(x->x_rngon, x->x_rngn, n ? n : 1);
        x->x_rnga = (double *)resizebytes(x->x_rnga, x->x_rngn * sizeof(double), (n ? n : 1) * sizeof(double));
        x->x_rngb = (double *)resizebytes(x->x_rngb, x->x_rngn * sizeof(double), (n ? n : 1) * sizeof(double));
    }
    x->x_rngn = n;
}

static void ranges_off(t_table *x)
{
    int i;
    for (i = 0; i < x->x_rngn; i++) { x->x_rngon[i] = 0; x->x_rnga[i] = 1; x->x_rngb[i] = 0; }
}

static t_float out_value(t_table *x, int col, double v)
{
    if (kv_isnan(v)) return x->x_nan;
    if (col < x->x_rngn && x->x_rngon[col]) v = x->x_rnga[col] * v + x->x_rngb[col];
    return (t_float)v;
}

static void cell_atom(t_table *x, long r, int col, t_atom *a)
{
    kv_csv *c = x->x_csv;
    if (c->txt[col] && c->txt[col][r]) SETSYMBOL(a, gensym(c->txt[col][r]));
    else SETFLOAT(a, out_value(x, col, c->num[col][r]));
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

static void output_pos(t_table *x, long r)
{
    long n = x->x_csv ? x->x_csv->nrows : 0;
    outlet_float(x->x_pos, n > 1 ? (t_float)((double)r / (double)(n - 1)) : 0);
}

static void info2(t_table *x, const char *sel, t_float a, t_float b)
{
    t_atom at[2];
    SETFLOAT(&at[0], a);
    SETFLOAT(&at[1], b);
    outlet_anything(x->x_info, gensym(sel), 2, at);
}

static void table_stop(t_table *x)
{
    x->x_playing = 0;
    clock_unset(x->x_clock);
}

static void set_data(t_table *x, kv_csv *c)
{
    table_stop(x);
    kv_csv_free(x->x_csv);
    x->x_csv = c;
    x->x_cursor = 0;
    sync_cols(x);
    ranges_off(x);
}

static void report_loaded(t_table *x, const char *from, int reltime)
{
    kv_csv *c = x->x_csv;
    int i;
    if (from) post("kvx/table: %ld rows x %d columns from %s", c->nrows, c->ncols, from);
    info2(x, "loaded", (t_float)c->nrows, (t_float)c->ncols);
    for (i = 0; i < c->ncols; i++)
        if (c->is_time[i] && reltime)
        {
            char ts[64];
            t_atom ta[2];
            snprintf(ts, sizeof ts, "%.3f", c->t0[i]);
            SETSYMBOL(&ta[0], gensym(c->name[i]));
            SETSYMBOL(&ta[1], gensym(ts));
            outlet_anything(x->x_info, gensym("timeorigin"), 2, ta);
        }
}

static void full_path(t_table *x, const char *name, char *full)
{
    if (x->x_canvas) canvas_makefilename(x->x_canvas, name, full, MAXPDSTRING);
    else snprintf(full, MAXPDSTRING, "%s", name);
}

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    size_t i;
    if (a < b) return 0;
    for (i = 0; i < b; i++)
    {
        char c = s[a - b + i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != suf[i]) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- reading */

static const char *flag_name(t_atom *a)
{
    if (a->a_type == A_SYMBOL && a->a_w.w_symbol->s_name[0] == '-' && a->a_w.w_symbol->s_name[1]
        && !(a->a_w.w_symbol->s_name[1] >= '0' && a->a_w.w_symbol->s_name[1] <= '9'))
        return a->a_w.w_symbol->s_name + 1;
    return NULL;
}

static char *atom_dup(t_atom *a)
{
    char buf[256], *s;
    atom_string(a, buf, sizeof buf);
    s = (char *)malloc(strlen(buf) + 1);
    if (s) strcpy(s, buf);
    return s;
}

static void table_read(t_table *x, t_symbol *s, int argc, t_atom *argv)
{
    kv_csv_opts o;
    char full[MAXPDSTRING], err[1100], **sel = NULL;
    const char *fname;
    FILE *f;
    kv_csv *c;
    int i, nsel = 0;
    (void)s;
    kv_csv_opts_default(&o);
    if (argc < 1 || argv[0].a_type != A_SYMBOL)
    {
        tbl_error(x, "usage: read file [-stride n] [-cols a b c] [-maxrows n] [-skip n] [-delim comma|semicolon|tab] [-header 0|1] [-decimal point|comma] [-reltime 0|1]");
        return;
    }
    fname = argv[0].a_w.w_symbol->s_name;
    full_path(x, fname, full);
    sel = (char **)getbytes((argc + 1) * sizeof(char *));
    for (i = 1; i < argc; i++)
    {
        const char *fl = flag_name(&argv[i]);
        if (!fl) { tbl_error(x, "unexpected word in read, options start with a dash"); goto bad; }
        if (!strcmp(fl, "cols"))
        {
            int k = i + 1;
            while (k < argc && !flag_name(&argv[k])) sel[nsel++] = atom_dup(&argv[k++]);
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
                if (!strcmp(d->s_name, "comma")) o.delim = ',';
                else if (!strcmp(d->s_name, "semicolon")) o.delim = ';';
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
    if (ends_with(fname, ".npy")) c = kv_npy_read(f, &o, err, sizeof err);
    else if (ends_with(fname, ".jsonl") || ends_with(fname, ".ndjson")) c = kv_jsonl_read(f, &o, err, sizeof err);
    else c = kv_csv_read(f, &o, err, sizeof err);
    sys_fclose(f);
    if (!c) { tbl_error(x, err); goto bad; }
    set_data(x, c);
    report_loaded(x, fname, o.reltime);
bad:
    for (i = 0; i < nsel; i++) free(sel[i]);
    freebytes(sel, (argc + 1) * sizeof(char *));
}

/* write file.csv [columns...] - dates are written as dates again */
static void table_write(t_table *x, t_symbol *s, int argc, t_atom *argv)
{
    char full[MAXPDSTRING], m[MAXPDSTRING + 64];
    int *cols = NULL, n = 0, i, ok;
    FILE *f;
    (void)s;
    if (!need_data(x)) return;
    if (argc < 1 || argv[0].a_type != A_SYMBOL) { tbl_error(x, "usage: write file.csv [columns...]"); return; }
    if (argc > 1)
    {
        cols = (int *)getbytes((argc - 1) * sizeof(int));
        for (i = 1; i < argc; i++)
        {
            int k = col_or_error(x, &argv[i]);
            if (k < 0) { freebytes(cols, (argc - 1) * sizeof(int)); return; }
            cols[n++] = k;
        }
    }
    full_path(x, argv[0].a_w.w_symbol->s_name, full);
    f = sys_fopen(full, "wb");
    if (!f) { snprintf(m, sizeof m, "cannot write %s", full); tbl_error(x, m); }
    else
    {
        ok = kv_csv_write(x->x_csv, f, n, cols, 1);
        sys_fclose(f);
        if (!ok) { snprintf(m, sizeof m, "error writing %s", full); tbl_error(x, m); }
        else
        {
            t_atom a[2];
            SETSYMBOL(&a[0], argv[0].a_w.w_symbol);
            SETFLOAT(&a[1], (t_float)x->x_csv->nrows);
            outlet_anything(x->x_info, gensym("written"), 2, a);
        }
    }
    if (cols) freebytes(cols, (argc - 1) * sizeof(int));
}

/* append v1 v2 ... : add one row (makes a new table if there is none) */
static void table_append(t_table *x, t_symbol *s, int argc, t_atom *argv)
{
    kv_csv *c;
    double *v;
    int i;
    (void)s;
    if (argc < 1) { tbl_error(x, "append needs at least one value"); return; }
    if (!x->x_csv || !x->x_csv->ncols)
    {
        c = kv_csv_new(argc, NULL);
        if (!c) { tbl_error(x, "out of memory"); return; }
        set_data(x, c);
    }
    c = x->x_csv;
    if (argc != c->ncols)
    {
        char m[120];
        snprintf(m, sizeof m, "append needs %d values, one per column", c->ncols);
        tbl_error(x, m);
        return;
    }
    v = (double *)getbytes(argc * sizeof(double));
    for (i = 0; i < argc; i++)
        v[i] = argv[i].a_type == A_FLOAT ? kv_float_to_double(argv[i].a_w.w_float) : kv_nan();
    if (!kv_csv_append_row(c, argc, v)) tbl_error(x, "out of memory");
    else
        for (i = 0; i < argc; i++)
            if (argv[i].a_type == A_SYMBOL)
            {
                const char *t = argv[i].a_w.w_symbol->s_name;
                long r = c->nrows - 1;
                if (!c->txt[i]) c->txt[i] = (char **)calloc(c->cap, sizeof(char *));
                if (c->txt[i])
                {
                    c->txt[i][r] = (char *)malloc(strlen(t) + 1);
                    if (c->txt[i][r]) strcpy(c->txt[i][r], t);
                }
            }
    freebytes(v, argc * sizeof(double));
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
    if ((col = col_or_error(x, &argv[1])) < 0) return;
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
    double mn = 0, mx = 0, mean = 0, m2 = 0;
    t_atom a[6];
    kv_csv *c;
    (void)s;
    if (!need_data(x)) return;
    if (argc != 1) { tbl_error(x, "usage: stats column"); return; }
    if ((col = col_or_error(x, &argv[0])) < 0) return;
    c = x->x_csv;
    for (i = 0; i < c->nrows; i++)
    {
        double v = c->num[col][i], d;
        if (kv_isnan(v)) continue;
        if (!n || v < mn) mn = v;
        if (!n || v > mx) mx = v;
        n++;
        d = v - mean;
        mean += d / n;
        m2 += d * (v - mean);
    }
    if (!n) { tbl_error(x, "column has no numbers"); return; }
    SETSYMBOL(&a[0], gensym(c->name[col]));
    SETFLOAT(&a[1], (t_float)mn);
    SETFLOAT(&a[2], (t_float)mx);
    SETFLOAT(&a[3], (t_float)mean);
    SETFLOAT(&a[4], (t_float)sqrt(m2 / n));
    SETFLOAT(&a[5], (t_float)n);
    outlet_anything(x->x_info, gensym("stats"), 6, a);
}

static void table_nan(t_table *x, t_floatarg f) { x->x_nan = f; }

/* range column lo hi : the column comes out scaled from its min..max to lo..hi
 * range column off   : back to the raw values
 * range off          : all columns raw */
static void table_range(t_table *x, t_symbol *s, int argc, t_atom *argv)
{
    int col;
    double mn, mx;
    (void)s;
    if (argc == 1 && argv[0].a_type == A_SYMBOL && !strcmp(argv[0].a_w.w_symbol->s_name, "off")) { ranges_off(x); return; }
    if (!need_data(x)) return;
    if (argc < 2) { tbl_error(x, "usage: range column lo hi, or range column off"); return; }
    if ((col = col_or_error(x, &argv[0])) < 0) return;
    sync_cols(x);
    if (argv[1].a_type == A_SYMBOL && !strcmp(argv[1].a_w.w_symbol->s_name, "off"))
    {
        x->x_rngon[col] = 0;
        return;
    }
    if (argc < 3 || argv[1].a_type != A_FLOAT || argv[2].a_type != A_FLOAT) { tbl_error(x, "usage: range column lo hi"); return; }
    kv_csv_minmax(x->x_csv, col, &mn, &mx);
    {
        double lo = argv[1].a_w.w_float, hi = argv[2].a_w.w_float;
        if (mx > mn)
        {
            x->x_rnga[col] = (hi - lo) / (mx - mn);
            x->x_rngb[col] = lo - mn * x->x_rnga[col];
        }
        else { x->x_rnga[col] = 0; x->x_rngb[col] = (lo + hi) / 2; }
        x->x_rngon[col] = 1;
    }
}

/* ------------------------------------------------------- computed columns */

static void table_derive(t_table *x, t_symbol *s, int argc, t_atom *argv)
{
    int col;
    char name[MAXPDSTRING];
    (void)s;
    if (!need_data(x)) return;
    if (argc < 1) { tbl_error(x, "usage: derive column [newname]"); return; }
    if ((col = col_or_error(x, &argv[0])) < 0) return;
    if (argc > 1) atom_string(&argv[1], name, sizeof name);
    else snprintf(name, sizeof name, "%s_diff", x->x_csv->name[col]);
    if (kv_csv_derive(x->x_csv, col, name) < 0) tbl_error(x, "out of memory");
    sync_cols(x);
}

static void table_smooth(t_table *x, t_symbol *s, int argc, t_atom *argv)
{
    int col;
    long n;
    char name[MAXPDSTRING];
    (void)s;
    if (!need_data(x)) return;
    if (argc < 2 || argv[1].a_type != A_FLOAT) { tbl_error(x, "usage: smooth column rows [newname]"); return; }
    if ((col = col_or_error(x, &argv[0])) < 0) return;
    n = (long)argv[1].a_w.w_float;
    if (n < 1) n = 1;
    if (argc > 2) atom_string(&argv[2], name, sizeof name);
    else snprintf(name, sizeof name, "%s_smooth", x->x_csv->name[col]);
    if (kv_csv_smooth(x->x_csv, col, n, name) < 0) tbl_error(x, "out of memory");
    sync_cols(x);
}

/* decimate n [first|mean|minmax column] */
static void table_decimate(t_table *x, t_symbol *s, int argc, t_atom *argv)
{
    long n;
    int mode = KV_DEC_FIRST, col = -1;
    (void)s;
    if (!need_data(x)) return;
    if (argc < 1 || argv[0].a_type != A_FLOAT) { tbl_error(x, "usage: decimate n [first|mean|minmax column]"); return; }
    n = (long)argv[0].a_w.w_float;
    if (argc > 1)
    {
        const char *m = atom_getsymbol(&argv[1])->s_name;
        if (!strcmp(m, "mean")) mode = KV_DEC_MEAN;
        else if (!strcmp(m, "first")) mode = KV_DEC_FIRST;
        else if (!strcmp(m, "minmax"))
        {
            mode = KV_DEC_MINMAX;
            if (argc < 3) { tbl_error(x, "minmax needs a column: decimate 10 minmax ch1"); return; }
            if ((col = col_or_error(x, &argv[2])) < 0) return;
        }
        else { tbl_error(x, "decimate modes are first, mean and minmax"); return; }
    }
    table_stop(x);
    if (!kv_csv_decimate(x->x_csv, n, mode, col)) tbl_error(x, "out of memory");
    x->x_cursor = 0;
    info2(x, "loaded", (t_float)x->x_csv->nrows, (t_float)x->x_csv->ncols);
}

/* crop start count | crop -time t0 t1 */
static void table_crop(t_table *x, t_symbol *s, int argc, t_atom *argv)
{
    (void)s;
    if (!need_data(x)) return;
    table_stop(x);
    if (argc == 3 && flag_name(&argv[0]) && !strcmp(flag_name(&argv[0]), "time"))
    {
        int tc = kv_csv_timecol(x->x_csv);
        if (tc < 0) tc = kv_csv_find(x->x_csv, "time");
        if (tc < 0) { tbl_error(x, "crop -time needs a date or time column"); return; }
        kv_csv_crop_time(x->x_csv, tc, atom_getfloat(&argv[1]), atom_getfloat(&argv[2]));
    }
    else if (argc == 2 && argv[0].a_type == A_FLOAT && argv[1].a_type == A_FLOAT)
        kv_csv_crop(x->x_csv, (long)argv[0].a_w.w_float, (long)argv[1].a_w.w_float);
    else { tbl_error(x, "usage: crop start count, or crop -time from to"); return; }
    x->x_cursor = 0;
    info2(x, "loaded", (t_float)x->x_csv->nrows, (t_float)x->x_csv->ncols);
}

/* ------------------------------------------------------- Pd array transfer */

static t_garray *find_array(t_table *x, t_symbol *name)
{
    t_garray *g = (t_garray *)pd_findbyclass(name, garray_class);
    if (!g)
    {
        char m[MAXPDSTRING + 40];
        snprintf(m, sizeof m, "no array called %s", name->s_name);
        tbl_error(x, m);
    }
    return g;
}

static int copy_to_array(t_table *x, int col, t_symbol *arr, int norm)
{
    t_garray *g = find_array(x, arr);
    t_word *vec;
    int n;
    long i, nr;
    double mn = 0, mx = 0, scale = 1, mid = 0;
    kv_csv *c = x->x_csv;
    if (!g) return 0;
    nr = c->nrows;
    if (norm)
    {
        kv_csv_minmax(c, col, &mn, &mx);
        if (mx > mn) { mid = (mx + mn) / 2; scale = 2.0 / (mx - mn); }
        else { mid = mn; scale = 1; }
    }
    garray_resize_long(g, nr > 0 ? nr : 1);
    if (!garray_getfloatwords(g, &n, &vec) || n < nr) { tbl_error(x, "could not resize the array"); return 0; }
    for (i = 0; i < nr; i++)
    {
        double v = c->num[col][i];
        if (norm) vec[i].w_float = kv_isnan(v) ? 0 : (t_float)((v - mid) * scale);
        else vec[i].w_float = out_value(x, col, v);
    }
    if (!nr) vec[0].w_float = 0;
    garray_redraw(g);
    return 1;
}

/* toarray column arrayname [norm] */
static void table_toarray(t_table *x, t_symbol *s, int argc, t_atom *argv)
{
    int col, norm = 0;
    (void)s;
    if (!need_data(x)) return;
    if (argc < 2 || argv[1].a_type != A_SYMBOL) { tbl_error(x, "usage: toarray column arrayname [norm]"); return; }
    if (argc > 2 && argv[2].a_type == A_SYMBOL && !strcmp(argv[2].a_w.w_symbol->s_name, "norm")) norm = 1;
    if ((col = col_or_error(x, &argv[0])) < 0) return;
    if (copy_to_array(x, col, argv[1].a_w.w_symbol, norm))
    {
        t_atom a[2];
        SETSYMBOL(&a[0], argv[1].a_w.w_symbol);
        SETFLOAT(&a[1], (t_float)x->x_csv->nrows);
        outlet_anything(x->x_info, gensym("toarray"), 2, a);
    }
}

/* toarrays col1 array1 col2 array2 ... [norm] */
static void table_toarrays(t_table *x, t_symbol *s, int argc, t_atom *argv)
{
    int i, norm = 0, n = argc;
    (void)s;
    if (!need_data(x)) return;
    if (n > 0 && argv[n - 1].a_type == A_SYMBOL && !strcmp(argv[n - 1].a_w.w_symbol->s_name, "norm")) { norm = 1; n--; }
    if (n < 2 || n % 2) { tbl_error(x, "usage: toarrays col1 array1 col2 array2 ... [norm]"); return; }
    for (i = 0; i < n; i += 2)
    {
        int col = col_or_error(x, &argv[i]);
        if (col < 0) return;
        if (argv[i + 1].a_type != A_SYMBOL) { tbl_error(x, "array names must be words"); return; }
        if (!copy_to_array(x, col, argv[i + 1].a_w.w_symbol, norm)) return;
    }
    {
        t_atom a[2];
        SETFLOAT(&a[0], (t_float)(n / 2));
        SETFLOAT(&a[1], (t_float)x->x_csv->nrows);
        outlet_anything(x->x_info, gensym("toarrays"), 2, a);
    }
}

/* fromarray newcolumn arrayname : a Pd array becomes a column */
static void table_fromarray(t_table *x, t_symbol *col, t_symbol *arr)
{
    t_garray *g = find_array(x, arr);
    t_word *vec;
    int n, i;
    double *v;
    if (!g) return;
    if (!garray_getfloatwords(g, &n, &vec)) { tbl_error(x, "cannot read that array"); return; }
    if (!x->x_csv || !x->x_csv->ncols)
    {
        kv_csv *c = kv_csv_new(0, NULL);
        if (!c || !kv_csv_reserve(c, n)) { kv_csv_free(c); tbl_error(x, "out of memory"); return; }
        c->nrows = n;
        set_data(x, c);
    }
    if (n != x->x_csv->nrows)
    {
        char m[160];
        snprintf(m, sizeof m, "the array has %d points but the table has %ld rows", n, x->x_csv->nrows);
        tbl_error(x, m);
        return;
    }
    v = (double *)getbytes((n ? n : 1) * sizeof(double));
    for (i = 0; i < n; i++) v[i] = kv_float_to_double(vec[i].w_float);
    if (x->x_csv->cap < n || kv_csv_setcol(x->x_csv, col->s_name, v) < 0) tbl_error(x, "out of memory");
    freebytes(v, (n ? n : 1) * sizeof(double));
    sync_cols(x);
    info2(x, "loaded", (t_float)x->x_csv->nrows, (t_float)x->x_csv->ncols);
}

/* ---------------------------------------------------------------- playback */

/* the time of a row in seconds: from the time column, or from the row rate */
static double row_time(t_table *x, long r)
{
    if (x->x_bytime) return x->x_csv->num[x->x_tcol][r];
    return r * x->x_rate / 1000.0;
}

static void set_anchor(t_table *x)
{
    double t;
    x->x_anchor_sys = clock_getlogicaltime();
    if (x->x_cursor < x->x_csv->nrows && !kv_isnan(t = row_time(x, x->x_cursor))) x->x_anchor_data = t;
    else x->x_anchor_data = 0;
}

static void table_tick(t_table *x)
{
    kv_csv *c = x->x_csv;
    double elapsed, due = 0;
    long sent = 0, last = -1;
    if (!x->x_playing || !c || !c->nrows) { x->x_playing = 0; return; }
    elapsed = clock_gettimesince(x->x_anchor_sys);
    for (;;)
    {
        double t;
        if (x->x_cursor >= c->nrows)
        {
            if (x->x_loop)
            {
                x->x_cursor = 0;
                outlet_anything(x->x_info, gensym("loop"), 0, NULL);
                if (!x->x_playing) return;
                set_anchor(x);
                elapsed = 0;
                if (sent >= MAXBURST) break;
                continue;
            }
            x->x_playing = 0;
            if (last >= 0) output_pos(x, last);
            outlet_anything(x->x_info, gensym("end"), 0, NULL);
            return;
        }
        t = row_time(x, x->x_cursor);
        due = kv_isnan(t) ? elapsed : (t - x->x_anchor_data) * 1000.0 / x->x_speed;
        if (due > elapsed + 1e-6 || sent >= MAXBURST) break;
        last = x->x_cursor;
        output_row(x, x->x_cursor++);
        sent++;
        if (!x->x_playing) return;     /* stopped from inside the patch */
    }
    if (last >= 0) output_pos(x, last);
    if (due < elapsed) due = elapsed;
    clock_delay(x->x_clock, due - elapsed > 0 ? due - elapsed : 0);
}

/* play [ms per row] | play -time [speed] [column] */
static void table_play(t_table *x, t_symbol *s, int argc, t_atom *argv)
{
    (void)s;
    if (!need_data(x)) return;
    if (!x->x_csv->nrows) { tbl_error(x, "the table has no rows"); return; }
    if (argc > 0 && flag_name(&argv[0]) && !strcmp(flag_name(&argv[0]), "time"))
    {
        int tc = -1;
        if (argc > 2) { if ((tc = col_or_error(x, &argv[2])) < 0) return; }
        else
        {
            tc = kv_csv_timecol(x->x_csv);
            if (tc < 0) tc = kv_csv_find(x->x_csv, "time");
        }
        if (tc < 0) { tbl_error(x, "play -time needs a date or time column (or name one: play -time 10 seconds)"); return; }
        x->x_bytime = 1;
        x->x_tcol = tc;
        if (argc > 1 && argv[1].a_type == A_FLOAT && argv[1].a_w.w_float > 0) x->x_speed = argv[1].a_w.w_float;
    }
    else if (argc > 0 && argv[0].a_type == A_FLOAT)
    {
        x->x_bytime = 0;
        if (argv[0].a_w.w_float > 0) x->x_rate = argv[0].a_w.w_float;
    }
    if (x->x_cursor >= x->x_csv->nrows) x->x_cursor = 0;
    x->x_playing = 1;
    set_anchor(x);
    clock_delay(x->x_clock, 0);
}

static void table_speed(t_table *x, t_floatarg f)
{
    if (f <= 0) { tbl_error(x, "speed must be above 0"); return; }
    x->x_speed = f;
    if (x->x_playing)
    {
        set_anchor(x);
        clock_delay(x->x_clock, 0);
    }
}

static void table_loop(t_table *x, t_floatarg f) { x->x_loop = f != 0; }

static void move_to(t_table *x, long r)
{
    x->x_cursor = r;
    if (r < x->x_csv->nrows) output_pos(x, r);
    if (x->x_playing) { set_anchor(x); clock_delay(x->x_clock, 0); }
}

static void table_reset(t_table *x)
{
    if (x->x_csv) move_to(x, 0);
}

static void table_seek(t_table *x, t_floatarg f)
{
    long r = (long)f;
    if (!need_data(x)) return;
    if (r < 0 || r >= x->x_csv->nrows) { tbl_error(x, "row out of range"); return; }
    move_to(x, r);
}

/* scrub to a fraction 0..1 of the table */
static void table_pos(t_table *x, t_floatarg f)
{
    long n;
    if (!need_data(x) || !(n = x->x_csv->nrows)) return;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    move_to(x, (long)(f * (n - 1) + 0.5));
}

/* at seconds : jump to the first row at or after this time */
static void table_at(t_table *x, t_floatarg f)
{
    int tc;
    long r;
    if (!need_data(x)) return;
    tc = x->x_bytime ? x->x_tcol : kv_csv_timecol(x->x_csv);
    if (tc < 0) tc = kv_csv_find(x->x_csv, "time");
    if (tc < 0) { tbl_error(x, "at needs a date or time column"); return; }
    r = kv_csv_find_time(x->x_csv, tc, f);
    if (r >= x->x_csv->nrows) r = x->x_csv->nrows - 1;
    move_to(x, r);
}

/* bang: the row under the cursor, then move on */
static void table_bang(t_table *x)
{
    if (!need_data(x)) return;
    if (x->x_cursor >= x->x_csv->nrows)
    {
        if (x->x_loop && x->x_csv->nrows)
        {
            x->x_cursor = 0;
            outlet_anything(x->x_info, gensym("loop"), 0, NULL);
        }
        else
        {
            outlet_anything(x->x_info, gensym("end"), 0, NULL);
            return;
        }
    }
    output_pos(x, x->x_cursor);
    output_row(x, x->x_cursor++);
}

static void table_clear(t_table *x)
{
    set_data(x, NULL);
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
    x->x_rngn = 0;
    x->x_rngon = (char *)getbytes(1);
    x->x_rnga = (double *)getbytes(sizeof(double));
    x->x_rngb = (double *)getbytes(sizeof(double));
    x->x_rate = 10;
    x->x_speed = 1;
    x->x_loop = 0;
    x->x_bytime = 0;
    x->x_tcol = -1;
    x->x_playing = 0;
    x->x_clock = clock_new(x, (t_method)table_tick);
    x->x_out = outlet_new(&x->x_obj, 0);
    x->x_info = outlet_new(&x->x_obj, 0);
    x->x_pos = outlet_new(&x->x_obj, &s_float);
    if (!announced)
    {
        announced = 1;
        post("kvx " KVX_VERSION " - [kvx/table], data files for Pd");
    }
    return x;
}

static void table_destroy(t_table *x)
{
    clock_free(x->x_clock);
    kv_csv_free(x->x_csv);
    freebytes(x->x_rngon, x->x_rngn ? x->x_rngn : 1);
    freebytes(x->x_rnga, (x->x_rngn ? x->x_rngn : 1) * sizeof(double));
    freebytes(x->x_rngb, (x->x_rngn ? x->x_rngn : 1) * sizeof(double));
}

void table_setup(void)
{
    table_class = class_new(gensym("table"), (t_newmethod)table_new, (t_method)table_destroy,
        sizeof(t_table), CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(table_class, table_bang);
    class_addmethod(table_class, (t_method)table_read, gensym("read"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_write, gensym("write"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_append, gensym("append"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_columns, gensym("columns"), 0);
    class_addmethod(table_class, (t_method)table_rows, gensym("rows"), 0);
    class_addmethod(table_class, (t_method)table_get, gensym("get"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_row, gensym("row"), A_FLOAT, 0);
    class_addmethod(table_class, (t_method)table_stats, gensym("stats"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_range, gensym("range"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_nan, gensym("nan"), A_FLOAT, 0);
    class_addmethod(table_class, (t_method)table_derive, gensym("derive"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_smooth, gensym("smooth"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_decimate, gensym("decimate"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_crop, gensym("crop"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_toarray, gensym("toarray"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_toarrays, gensym("toarrays"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_fromarray, gensym("fromarray"), A_SYMBOL, A_SYMBOL, 0);
    class_addmethod(table_class, (t_method)table_reset, gensym("reset"), 0);
    class_addmethod(table_class, (t_method)table_seek, gensym("seek"), A_FLOAT, 0);
    class_addmethod(table_class, (t_method)table_pos, gensym("pos"), A_FLOAT, 0);
    class_addmethod(table_class, (t_method)table_at, gensym("at"), A_FLOAT, 0);
    class_addmethod(table_class, (t_method)table_play, gensym("play"), A_GIMME, 0);
    class_addmethod(table_class, (t_method)table_stop, gensym("stop"), 0);
    class_addmethod(table_class, (t_method)table_stop, gensym("pause"), 0);
    class_addmethod(table_class, (t_method)table_speed, gensym("speed"), A_FLOAT, 0);
    class_addmethod(table_class, (t_method)table_loop, gensym("loop"), A_FLOAT, 0);
    class_addmethod(table_class, (t_method)table_clear, gensym("clear"), 0);
}
