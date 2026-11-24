#include "csv_ops.h"
#include "kv_core.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <locale.h>

/* ------------------------------------------------------------------ helpers */

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* one more (empty) column at the end; returns its index or -1 */
static int add_column(kv_csv *c, const char *name)
{
    int n = c->ncols + 1;
    char **nm = (char **)realloc(c->name, n * sizeof(char *));
    double **nu;
    char ***tx;
    char *it;
    double *t0;
    if (!nm) return -1;
    c->name = nm;
    nu = (double **)realloc(c->num, n * sizeof(double *));
    if (!nu) return -1;
    c->num = nu;
    tx = (char ***)realloc(c->txt, n * sizeof(char **));
    if (!tx) return -1;
    c->txt = tx;
    it = (char *)realloc(c->is_time, n);
    if (!it) return -1;
    c->is_time = it;
    t0 = (double *)realloc(c->t0, n * sizeof(double));
    if (!t0) return -1;
    c->t0 = t0;
    c->name[n - 1] = dupstr(name);
    if (!c->name[n - 1]) return -1;
    c->num[n - 1] = (double *)malloc((c->cap > 0 ? c->cap : 1) * sizeof(double));
    if (!c->num[n - 1]) { free(c->name[n - 1]); return -1; }
    c->txt[n - 1] = NULL;
    c->is_time[n - 1] = 0;
    c->t0[n - 1] = 0;
    c->ncols = n;
    return n - 1;
}

kv_csv *kv_csv_new(int ncols, const char **names)
{
    kv_csv *c = (kv_csv *)calloc(1, sizeof *c);
    int i;
    if (!c) return NULL;
    for (i = 0; i < ncols; i++)
    {
        char buf[32];
        const char *nm = names ? names[i] : NULL;
        if (!nm) { snprintf(buf, sizeof buf, "col%d", i + 1); nm = buf; }
        if (add_column(c, nm) < 0) { kv_csv_free(c); return NULL; }
    }
    return c;
}

int kv_csv_append_row(kv_csv *c, int n, const double *v)
{
    int i;
    if (n != c->ncols || n < 1) return 0;
    if (!kv_csv_reserve(c, c->nrows + 1)) return 0;
    for (i = 0; i < n; i++)
    {
        c->num[i][c->nrows] = v[i];
        if (c->txt[i]) c->txt[i][c->nrows] = NULL;
    }
    c->nrows++;
    return 1;
}

int kv_csv_setcol(kv_csv *c, const char *name, const double *v)
{
    int k = kv_csv_find(c, name);
    long j;
    if (k < 0) k = add_column(c, name);
    if (k < 0) return -1;
    if (c->txt[k])
    {
        for (j = 0; j < c->nrows; j++) free(c->txt[k][j]);
        free(c->txt[k]);
        c->txt[k] = NULL;
    }
    if (c->nrows) memcpy(c->num[k], v, c->nrows * sizeof(double));
    c->is_time[k] = 0;
    c->t0[k] = 0;
    return k;
}

int kv_csv_timecol(const kv_csv *c)
{
    int i;
    for (i = 0; i < c->ncols; i++) if (c->is_time[i]) return i;
    return -1;
}

void kv_csv_minmax(const kv_csv *c, int col, double *mn, double *mx)
{
    long i;
    int have = 0;
    *mn = *mx = 0;
    for (i = 0; i < c->nrows; i++)
    {
        double v = c->num[col][i];
        if (kv_isnan(v)) continue;
        if (!have || v < *mn) *mn = v;
        if (!have || v > *mx) *mx = v;
        have = 1;
    }
}

/* ------------------------------------------------------- computed columns */

int kv_csv_derive(kv_csv *c, int col, const char *name)
{
    double *d;
    long i;
    int k;
    if (col < 0 || col >= c->ncols) return -1;
    d = (double *)malloc((c->nrows ? c->nrows : 1) * sizeof(double));
    if (!d) return -1;
    for (i = 0; i < c->nrows; i++)
    {
        double a = c->num[col][i], b = i ? c->num[col][i - 1] : a;
        d[i] = (kv_isnan(a) || kv_isnan(b)) ? kv_nan() : a - b;
    }
    k = kv_csv_setcol(c, name, d);
    free(d);
    return k;
}

int kv_csv_smooth(kv_csv *c, int col, long n, const char *name)
{
    double *s, *out;
    long *cnt, i, half, N = c->nrows;
    int k;
    if (col < 0 || col >= c->ncols || n < 1) return -1;
    s = (double *)malloc((N + 1) * sizeof(double));
    cnt = (long *)malloc((N + 1) * sizeof(long));
    out = (double *)malloc((N ? N : 1) * sizeof(double));
    if (!s || !cnt || !out) { free(s); free(cnt); free(out); return -1; }
    s[0] = 0; cnt[0] = 0;
    for (i = 0; i < N; i++)
    {
        double v = c->num[col][i];
        int ok = !kv_isnan(v);
        s[i + 1] = s[i] + (ok ? v : 0);
        cnt[i + 1] = cnt[i] + ok;
    }
    half = n / 2;
    for (i = 0; i < N; i++)
    {
        long lo = i - half, hi = i + (n - 1 - half);
        if (lo < 0) lo = 0;
        if (hi > N - 1) hi = N - 1;
        out[i] = cnt[hi + 1] - cnt[lo] > 0 ? (s[hi + 1] - s[lo]) / (double)(cnt[hi + 1] - cnt[lo]) : kv_nan();
    }
    k = kv_csv_setcol(c, name, out);
    free(s); free(cnt); free(out);
    return k;
}

/* ------------------------------------------------------- thinning out rows */

/* keep the rows in idx (ascending, unique) */
static int select_rows(kv_csv *c, const long *idx, long m)
{
    long cap = m > 0 ? m : 1, j;
    int i;
    double **nn = (double **)calloc(c->ncols ? c->ncols : 1, sizeof(double *));
    char ***nt = (char ***)calloc(c->ncols ? c->ncols : 1, sizeof(char **));
    if (!nn || !nt) { free(nn); free(nt); return 0; }
    for (i = 0; i < c->ncols; i++)
    {
        nn[i] = (double *)malloc(cap * sizeof(double));
        if (!nn[i]) goto oom;
        if (c->txt[i])
        {
            nt[i] = (char **)calloc(cap, sizeof(char *));
            if (!nt[i]) goto oom;
        }
    }
    for (i = 0; i < c->ncols; i++)
    {
        for (j = 0; j < m; j++)
        {
            nn[i][j] = c->num[i][idx[j]];
            if (c->txt[i]) { nt[i][j] = c->txt[i][idx[j]]; c->txt[i][idx[j]] = NULL; }
        }
        free(c->num[i]);
        c->num[i] = nn[i];
        if (c->txt[i])
        {
            for (j = 0; j < c->nrows; j++) free(c->txt[i][j]);
            free(c->txt[i]);
            c->txt[i] = nt[i];
        }
    }
    c->nrows = m;
    c->cap = cap;
    free(nn); free(nt);
    return 1;
oom:
    for (i = 0; i < c->ncols; i++) { free(nn[i]); free(nt[i]); }
    free(nn); free(nt);
    return 0;
}

int kv_csv_decimate(kv_csv *c, long n, int mode, int col)
{
    long nb, b, m = 0, *idx;
    int ok;
    if (n <= 1 || c->nrows == 0) return 1;
    nb = (c->nrows + n - 1) / n;
    if (mode == KV_DEC_MEAN)
    {
        int i;
        long cap = nb;
        for (i = 0; i < c->ncols; i++)
        {
            double *out = (double *)malloc(cap * sizeof(double));
            char **tout = NULL;
            if (!out) return 0;
            if (c->txt[i] && !(tout = (char **)calloc(cap, sizeof(char *)))) { free(out); return 0; }
            for (b = 0; b < nb; b++)
            {
                long r, lo = b * n, hi = lo + n;
                double sum = 0;
                long k = 0;
                if (hi > c->nrows) hi = c->nrows;
                for (r = lo; r < hi; r++)
                    if (!kv_isnan(c->num[i][r])) { sum += c->num[i][r]; k++; }
                out[b] = k ? sum / k : kv_nan();
                if (tout) { tout[b] = c->txt[i][lo]; c->txt[i][lo] = NULL; }
            }
            if (c->txt[i])
            {
                long r;
                for (r = 0; r < c->nrows; r++) free(c->txt[i][r]);
                free(c->txt[i]);
                c->txt[i] = tout;
            }
            free(c->num[i]);
            c->num[i] = out;
        }
        c->nrows = nb;
        c->cap = cap;
        return 1;
    }
    idx = (long *)malloc(2 * nb * sizeof(long));
    if (!idx) return 0;
    for (b = 0; b < nb; b++)
    {
        long lo = b * n, hi = lo + n, r, imn = -1, imx = -1;
        if (hi > c->nrows) hi = c->nrows;
        if (mode != KV_DEC_MINMAX || col < 0 || col >= c->ncols) { idx[m++] = lo; continue; }
        for (r = lo; r < hi; r++)
        {
            double v = c->num[col][r];
            if (kv_isnan(v)) continue;
            if (imn < 0 || v < c->num[col][imn]) imn = r;
            if (imx < 0 || v > c->num[col][imx]) imx = r;
        }
        if (imn < 0) idx[m++] = lo;
        else if (imn == imx) idx[m++] = imn;
        else if (imn < imx) { idx[m++] = imn; idx[m++] = imx; }
        else { idx[m++] = imx; idx[m++] = imn; }
    }
    ok = select_rows(c, idx, m);
    free(idx);
    return ok;
}

void kv_csv_crop(kv_csv *c, long start, long count)
{
    int i;
    long j;
    if (start < 0) start = 0;
    if (start > c->nrows) start = c->nrows;
    if (count < 0 || count > c->nrows - start) count = c->nrows - start;
    for (i = 0; i < c->ncols; i++)
    {
        if (c->txt[i])
        {
            for (j = 0; j < start; j++) { free(c->txt[i][j]); c->txt[i][j] = NULL; }
            for (j = start + count; j < c->nrows; j++) { free(c->txt[i][j]); c->txt[i][j] = NULL; }
            if (start && count) memmove(c->txt[i], c->txt[i] + start, count * sizeof(char *));
            for (j = count; j < c->nrows; j++) c->txt[i][j] = NULL;
        }
        if (start && count) memmove(c->num[i], c->num[i] + start, count * sizeof(double));
    }
    c->nrows = count;
}

long kv_csv_find_time(const kv_csv *c, int col, double t)
{
    long lo = 0, hi = c->nrows;
    if (col < 0 || col >= c->ncols) return 0;
    while (lo < hi)
    {
        long mid = lo + (hi - lo) / 2;
        double v = c->num[col][mid];
        if (kv_isnan(v) || v < t) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

int kv_csv_crop_time(kv_csv *c, int col, double t0, double t1)
{
    long i, first = -1, last = -1;
    if (col < 0 || col >= c->ncols) return 0;
    for (i = 0; i < c->nrows; i++)
    {
        double v = c->num[col][i];
        if (kv_isnan(v) || v < t0 || v > t1) continue;
        if (first < 0) first = i;
        last = i;
    }
    if (first < 0) kv_csv_crop(c, 0, 0);
    else kv_csv_crop(c, first, last - first + 1);
    return 1;
}

/* ------------------------------------------------------------------ writing */

static void fmt_num(double v, char *buf, size_t n)
{
    char dp = localeconv()->decimal_point[0], *p;
    snprintf(buf, n, "%.15g", v);
    if (dp != '.') for (p = buf; *p; p++) if (*p == dp) *p = '.';
    {
        char tmp[64];
        double back;
        strncpy(tmp, buf, sizeof tmp - 1);
        tmp[sizeof tmp - 1] = 0;
        if (dp != '.') for (p = tmp; *p; p++) if (*p == '.') *p = dp;
        back = strtod(tmp, NULL);
        if (back != v)
        {
            snprintf(buf, n, "%.17g", v);
            if (dp != '.') for (p = buf; *p; p++) if (*p == dp) *p = '.';
        }
    }
}

static void civil_from_days(long z, long *y, int *m, int *d)
{
    long era, doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

/* seconds since 1970 as 2024-03-05T10:20:30.25Z */
static void fmt_time(double t, char *buf, size_t n)
{
    double day = t / 86400.0, fl = day >= 0 ? (double)(long)day : (double)(long)day - ((double)(long)day != day);
    long days = (long)fl, y;
    double sec = t - fl * 86400.0;
    int m, d, hh, mi;
    long ms;
    civil_from_days(days, &y, &m, &d);
    ms = (long)(sec * 1000.0 + 0.5);
    if (ms >= 86400000L) ms = 86399999L;
    hh = (int)(ms / 3600000L);
    mi = (int)(ms / 60000L % 60);
    if (ms % 1000)
        snprintf(buf, n, "%04ld-%02d-%02dT%02d:%02d:%02ld.%03ldZ", y, m, d, hh, mi, ms / 1000 % 60, ms % 1000);
    else
        snprintf(buf, n, "%04ld-%02d-%02dT%02d:%02d:%02ldZ", y, m, d, hh, mi, ms / 1000 % 60);
}

static int put_text(FILE *f, const char *s)
{
    const char *p;
    int q = 0;
    for (p = s; *p; p++) if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') { q = 1; break; }
    if (!*s) q = 0;
    if (!q) return fputs(s, f) >= 0;
    if (fputc('"', f) == EOF) return 0;
    for (p = s; *p; p++)
    {
        if (*p == '"' && fputc('"', f) == EOF) return 0;
        if (fputc(*p, f) == EOF) return 0;
    }
    return fputc('"', f) != EOF;
}

int kv_csv_write(const kv_csv *c, FILE *f, int n, const int *cols, int abstime)
{
    int i;
    long r;
    char buf[64];
    if (!cols) n = c->ncols;
    for (i = 0; i < n; i++)
    {
        int k = cols ? cols[i] : i;
        if (i && fputc(',', f) == EOF) return 0;
        if (!put_text(f, c->name[k])) return 0;
    }
    if (fputc('\n', f) == EOF) return 0;
    for (r = 0; r < c->nrows; r++)
    {
        for (i = 0; i < n; i++)
        {
            int k = cols ? cols[i] : i;
            if (i && fputc(',', f) == EOF) return 0;
            if (c->txt[k] && c->txt[k][r]) { if (!put_text(f, c->txt[k][r])) return 0; }
            else
            {
                double v = c->num[k][r];
                if (kv_isnan(v)) continue;
                if (abstime && c->is_time[k]) fmt_time(v + c->t0[k], buf, sizeof buf);
                else fmt_num(v, buf, sizeof buf);
                if (fputs(buf, f) < 0) return 0;
            }
        }
        if (fputc('\n', f) == EOF) return 0;
    }
    return 1;
}

/* ------------------------------------------------ choosing columns by name */

static int resolve_cols(char **names, int nn, const kv_csv_opts *o, int **sel_out, int *nsel, char *err, size_t es)
{
    int i, k, n = o->ncolsel > 0 ? o->ncolsel : nn;
    int *sel = (int *)malloc((n ? n : 1) * sizeof(int));
    if (!sel) { snprintf(err, es, "out of memory"); return 0; }
    for (i = 0; i < n; i++)
    {
        int found = -1;
        if (o->ncolsel <= 0) found = i;
        else
        {
            const char *w = o->colsel[i];
            for (k = 0; k < nn; k++) if (!strcmp(names[k], w)) { found = k; break; }
            if (found < 0)
            {
                char *e;
                long v = strtol(w, &e, 10);
                if (*w && !*e && v >= 0 && v < nn) found = (int)v;
            }
            if (found < 0) { snprintf(err, es, "no column called %s", w); free(sel); return 0; }
        }
        sel[i] = found;
    }
    *sel_out = sel;
    *nsel = n;
    return 1;
}

static kv_csv *new_selected(char **names, const int *sel, int nsel)
{
    const char **nm = (const char **)malloc((nsel ? nsel : 1) * sizeof(char *));
    kv_csv *c;
    int i;
    if (!nm) return NULL;
    for (i = 0; i < nsel; i++) nm[i] = names[sel[i]];
    c = kv_csv_new(nsel, nm);
    free(nm);
    return c;
}

static void apply_reltime(kv_csv *c)
{
    int i;
    long j;
    for (i = 0; i < c->ncols; i++)
    {
        if (!c->is_time[i]) continue;
        for (j = 0; j < c->nrows && kv_isnan(c->num[i][j]); j++) ;
        if (j == c->nrows) continue;
        c->t0[i] = c->num[i][j];
        for (j = 0; j < c->nrows; j++) c->num[i][j] -= c->t0[i];
    }
}

/* ------------------------------------------------------------------ NumPy */

static double npy_value(const unsigned char *p, char kind, int size)
{
    switch (kind)
    {
    case 'f':
        if (size == 8) { double d; memcpy(&d, p, 8); return d; }
        if (size == 4) { float f; memcpy(&f, p, 4); return kv_float_to_double(f); }
        break;
    case 'i':
        if (size == 1) return (double)(signed char)p[0];
        if (size == 2) { short v; memcpy(&v, p, 2); return v; }
        if (size == 4) { int v; memcpy(&v, p, 4); return v; }
        if (size == 8) { long long v; memcpy(&v, p, 8); return (double)v; }
        break;
    case 'u': case 'b':
        if (size == 1) return p[0];
        if (size == 2) { unsigned short v; memcpy(&v, p, 2); return v; }
        if (size == 4) { unsigned int v; memcpy(&v, p, 4); return v; }
        if (size == 8) { unsigned long long v; memcpy(&v, p, 8); return (double)v; }
        break;
    }
    return kv_nan();
}

kv_csv *kv_npy_read(FILE *f, const kv_csv_opts *o, char *err, size_t es)
{
    unsigned char mag[10];
    char *hdr = NULL, *p, kind = 0;
    unsigned long hlen;
    int size = 0, fortran = 0, ndim = 0, i, nsel = 0, *sel = NULL;
    long shape[2] = {0, 1}, r, kept = 0, stride = o->stride > 0 ? o->stride : 1;
    size_t rowbytes;
    unsigned char *row = NULL, *all = NULL;
    char **names = NULL;
    kv_csv *c = NULL;
    double *vals = NULL;

    if (fread(mag, 1, 8, f) != 8 || memcmp(mag, "\x93NUMPY", 6)) { snprintf(err, es, "not a .npy file"); return NULL; }
    if (mag[6] == 1)
    {
        unsigned char b[2];
        if (fread(b, 1, 2, f) != 2) goto trunc;
        hlen = b[0] | (b[1] << 8);
    }
    else
    {
        unsigned char b[4];
        if (fread(b, 1, 4, f) != 4) goto trunc;
        hlen = b[0] | (b[1] << 8) | ((unsigned long)b[2] << 16) | ((unsigned long)b[3] << 24);
    }
    if (hlen > 1000000) { snprintf(err, es, ".npy header too long"); return NULL; }
    hdr = (char *)malloc(hlen + 1);
    if (!hdr) { snprintf(err, es, "out of memory"); return NULL; }
    if (fread(hdr, 1, hlen, f) != hlen) goto trunc;
    hdr[hlen] = 0;

    p = strstr(hdr, "descr");
    if (!p || !(p = strchr(p + 5, ':')) || !(p = strchr(p, '\''))) goto badhdr;
    p++;
    if (*p == '>' && p[2] != '1') { snprintf(err, es, "big-endian .npy files are not supported"); goto fail; }
    if (*p == '<' || *p == '|' || *p == '=' || *p == '>') p++;
    kind = *p++;
    size = atoi(p);
    if (!strchr("fiub", kind) || !(size == 1 || size == 2 || size == 4 || size == 8) || (kind == 'f' && size < 4))
    { snprintf(err, es, "unsupported .npy type (use float, int or uint)"); goto fail; }
    p = strstr(hdr, "fortran_order");
    if (p && strstr(p, "True") && strstr(p, "True") < strchr(p, ',') + 1) fortran = 1;
    p = strstr(hdr, "shape");
    if (!p || !(p = strchr(p, '('))) goto badhdr;
    p++;
    while (*p && *p != ')')
    {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ')') break;
        if (!isdigit((unsigned char)*p)) goto badhdr;
        if (ndim >= 2) { snprintf(err, es, ".npy arrays with more than 2 dimensions are not supported"); goto fail; }
        shape[ndim++] = strtol(p, &p, 10);
    }
    if (ndim == 0) { snprintf(err, es, ".npy holds a single value, not an array"); goto fail; }
    if (ndim == 1) shape[1] = 1;
    if (shape[1] < 1 || shape[1] > 100000) { snprintf(err, es, "too many columns in .npy"); goto fail; }

    names = (char **)calloc(shape[1], sizeof(char *));
    if (!names) goto oom;
    for (i = 0; i < shape[1]; i++)
    {
        char b[32];
        snprintf(b, sizeof b, "col%d", i + 1);
        if (!(names[i] = dupstr(b))) goto oom;
    }
    if (!resolve_cols(names, (int)shape[1], o, &sel, &nsel, err, es)) goto fail;
    c = new_selected(names, sel, nsel);
    vals = (double *)malloc(nsel * sizeof(double));
    if (!c || !vals) goto oom;

    rowbytes = (size_t)shape[1] * size;
    if (fortran)
    {
        size_t total = rowbytes * (size_t)shape[0];
        if (total > (size_t)1 << 30) { snprintf(err, es, "Fortran-ordered .npy larger than 1 GB"); goto fail; }
        all = (unsigned char *)malloc(total ? total : 1);
        if (!all) goto oom;
        if (fread(all, 1, total, f) != total) goto trunc;
    }
    else if (!(row = (unsigned char *)malloc(rowbytes))) goto oom;

    for (r = 0; r < shape[0]; r++)
    {
        if (!fortran && fread(row, 1, rowbytes, f) != rowbytes) goto trunc;
        if (r < o->skip) continue;
        if ((r - o->skip) % stride) continue;
        for (i = 0; i < nsel; i++)
        {
            const unsigned char *q = fortran ? all + ((size_t)sel[i] * shape[0] + r) * size : row + (size_t)sel[i] * size;
            vals[i] = npy_value(q, kind, size);
        }
        if (!kv_csv_append_row(c, nsel, vals)) goto oom;
        kept++;
        if (o->maxrows > 0 && kept >= o->maxrows) break;
    }
    c->rows_seen = shape[0];
    goto done;
trunc:
    snprintf(err, es, ".npy file is shorter than its header says");
    goto fail;
badhdr:
    snprintf(err, es, "cannot read the .npy header");
    goto fail;
oom:
    snprintf(err, es, "out of memory");
fail:
    kv_csv_free(c);
    c = NULL;
done:
    if (names) for (i = 0; i < shape[1]; i++) free(names[i]);
    free(names); free(sel); free(hdr); free(row); free(all); free(vals);
    return c;
}

/* ------------------------------------------------------------ JSON Lines */

static char *read_line(FILE *f, char **buf, size_t *cap, size_t *len)
{
    size_t n = 0;
    int ch;
    if (!*buf) { *cap = 4096; *buf = (char *)malloc(*cap); if (!*buf) return NULL; }
    while ((ch = getc(f)) != EOF)
    {
        if (ch == '\n') break;
        if (n + 2 > *cap)
        {
            char *nb;
            if (*cap >= ((size_t)64 << 20)) return NULL;
            nb = (char *)realloc(*buf, *cap * 2);
            if (!nb) return NULL;
            *buf = nb; *cap *= 2;
        }
        (*buf)[n++] = (char)ch;
    }
    if (ch == EOF && n == 0) return NULL;
    if (n && (*buf)[n - 1] == '\r') n--;
    (*buf)[n] = 0;
    *len = n;
    return *buf;
}

kv_csv *kv_jsonl_read(FILE *f, const kv_csv_opts *o, char *err, size_t es)
{
    char *buf = NULL, **names = NULL, perr[200];
    size_t cap = 0, len;
    long lineno = 0, seen = 0, stride = o->stride > 0 ? o->stride : 1;
    int nn = 0, *sel = NULL, nsel = 0, i;
    kv_csv *c = NULL;
    double *vals = NULL;
    char **tvals = NULL;

    while (read_line(f, &buf, &cap, &len))
    {
        kv_node *n;
        const char *s = buf;
        lineno++;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) continue;
        if (lineno <= o->skip) continue;
        n = kv_parse(buf, len, perr, sizeof perr);
        if (!n) { snprintf(err, es, "line %ld: %s", lineno, perr); goto fail; }
        if (n->type != KV_OBJ) { kv_free(n); snprintf(err, es, "line %ld is not a JSON object", lineno); goto fail; }
        if (!c)
        {
            nn = (int)n->len;
            if (nn < 1) { kv_free(n); snprintf(err, es, "line %ld has no fields", lineno); goto fail; }
            names = (char **)calloc(nn, sizeof(char *));
            if (!names) { kv_free(n); goto oom; }
            for (i = 0; i < nn; i++) if (!(names[i] = dupstr(n->members[i].key))) { kv_free(n); goto oom; }
            if (!resolve_cols(names, nn, o, &sel, &nsel, err, es)) { kv_free(n); goto fail; }
            c = new_selected(names, sel, nsel);
            vals = (double *)malloc(nsel * sizeof(double));
            tvals = (char **)calloc(nsel, sizeof(char *));
            if (!c || !vals || !tvals) { kv_free(n); goto oom; }
        }
        if ((seen++) % stride) { kv_free(n); continue; }
        for (i = 0; i < nsel; i++)
        {
            kv_node *v = kv_obj_get(n, names[sel[i]]);
            tvals[i] = NULL;
            vals[i] = kv_nan();
            if (!v) continue;
            if (v->type == KV_NUM || v->type == KV_BOOL) vals[i] = v->num;
            else if (v->type == KV_STR)
            {
                double d;
                int kind = kv_csv_number_dc(v->str, &d, o->decimal_comma);
                if (kind == 1 || kind == 3) vals[i] = d;
                if (kind == 3) c->is_time[i] = 1;
                if (kind == 0) tvals[i] = v->str;
            }
        }
        if (!kv_csv_append_row(c, nsel, vals)) { kv_free(n); goto oom; }
        for (i = 0; i < nsel; i++)
            if (tvals[i])
            {
                long r = c->nrows - 1;
                if (!c->txt[i] && !(c->txt[i] = (char **)calloc(c->cap, sizeof(char *)))) { kv_free(n); goto oom; }
                if (!(c->txt[i][r] = dupstr(tvals[i]))) { kv_free(n); goto oom; }
            }
        kv_free(n);
        if (o->maxrows > 0 && c->nrows >= o->maxrows) break;
    }
    if (!c) { snprintf(err, es, "no data found"); goto fail; }
    c->rows_seen = seen;
    if (o->reltime) apply_reltime(c);
    goto done;
oom:
    snprintf(err, es, "out of memory");
fail:
    kv_csv_free(c);
    c = NULL;
done:
    if (names) for (i = 0; i < nn; i++) free(names[i]);
    free(names); free(sel); free(buf); free(vals); free(tvals);
    return c;
}
