#include "csv_core.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>

#define MAXREC (16L * 1024L * 1024L)

void kv_csv_opts_default(kv_csv_opts *o)
{
    memset(o, 0, sizeof *o);
    o->reltime = 1;
    o->header = -1;
    o->stride = 1;
}

/* ------------------------------------------------------------------ numbers */

static long days_from_civil(long y, int m, int d)
{
    long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int dig(const char *p, int n)
{
    int i;
    for (i = 0; i < n; i++) if (!isdigit((unsigned char)p[i])) return 0;
    return 1;
}

static int num2(const char *p) { return (p[0] - '0') * 10 + (p[1] - '0'); }

/* 2024-03-05, 2024-03-05T10:20, 2024-03-05 10:20:30.25Z, ...+02:00 */
static int parse_time(const char *s, double *out)
{
    int mo, d, hh = 0, mi = 0, off = 0;
    long y;
    double sec = 0;
    if (!(dig(s, 4) && s[4] == '-' && dig(s + 5, 2) && s[7] == '-' && dig(s + 8, 2))) return 0;
    y = (s[0]-'0')*1000L + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    mo = num2(s + 5);
    d = num2(s + 8);
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    s += 10;
    if (*s == 'T' || *s == ' ')
    {
        s++;
        if (!(dig(s, 2) && s[2] == ':' && dig(s + 3, 2))) return 0;
        hh = num2(s); mi = num2(s + 3);
        if (hh > 23 || mi > 59) return 0;
        s += 5;
        if (*s == ':')
        {
            double frac = 0, scale = 0.1;
            if (!dig(s + 1, 2)) return 0;
            sec = num2(s + 1);
            if (sec > 60) return 0;
            s += 3;
            if (*s == '.' || *s == ',')
            {
                s++;
                if (!isdigit((unsigned char)*s)) return 0;
                while (isdigit((unsigned char)*s)) { frac += (*s - '0') * scale; scale /= 10; s++; }
                sec += frac;
            }
        }
        if (*s == 'Z') s++;
        else if (*s == '+' || *s == '-')
        {
            int sign = *s == '-' ? -1 : 1, oh, om = 0;
            if (!dig(s + 1, 2)) return 0;
            oh = num2(s + 1);
            s += 3;
            if (*s == ':') s++;
            if (dig(s, 2)) { om = num2(s); s += 2; }
            off = sign * (oh * 3600 + om * 60);
        }
    }
    if (*s) return 0;
    *out = (double)days_from_civil(y, mo, d) * 86400.0 + hh * 3600.0 + mi * 60.0 + sec - off;
    return 1;
}

static int strcasecmp_(const char *b)
{
    static const char *w[] = { "nan", "na", "n/a", "null", "none", NULL };
    int i;
    for (i = 0; w[i]; i++)
    {
        const char *a = w[i], *c = b;
        while (*a && tolower((unsigned char)*c) == *a) { a++; c++; }
        if (!*a && !*c) return 1;
    }
    return 0;
}

double kv_nan(void)
{
    unsigned long long u = 0x7ff8000000000000ULL;
    double v;
    memcpy(&v, &u, sizeof v);
    return v;
}

int kv_csv_number(const char *s, double *out) { return kv_csv_number_dc(s, out, 0); }

int kv_csv_number_dc(const char *s, double *out, int dc)
{
    char buf[64], *end;
    size_t n;
    const char *p, *q;
    while (*s == ' ' || *s == '\t') s++;
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (!n) { *out = kv_nan(); return 2; }
    if (n < sizeof buf)
    {
        memcpy(buf, s, n); buf[n] = 0;
        if (dc) { char *k; for (k = buf; *k; k++) if (*k == ',') *k = '.'; }
        if (strcasecmp_(buf)) { *out = kv_nan(); return 2; }
    }
    else return 0;
    p = buf;
    q = p + ((*p == '+' || *p == '-') ? 1 : 0);
    if (!(isdigit((unsigned char)*q) || (*q == '.' && isdigit((unsigned char)q[1])))) return 0;
    if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) return 0;
    if (parse_time(buf, out)) return 3;
    {
        /* strtod follows the locale; make the decimal point match */
        char dp = localeconv()->decimal_point[0];
        char *c;
        if (dp != '.') for (c = buf; *c; c++) if (*c == '.') *c = dp;
    }
    *out = strtod(buf, &end);
    if (*end || end == buf) return 0;
    if (kv_isnan(*out) || *out > 1.7976931348623157e308 || *out < -1.7976931348623157e308) return 0;
    return 1;
}

/* ------------------------------------------------------------------ reading */

typedef struct {
    char *buf;
    size_t cap;
    char **f;       /* field starts */
    int nf, fcap;
} rec_t;

/* Read one record (a line, or several when a quoted field holds a newline).
 * Returns 1 for a record, 0 at the end of the file, -1 when it is too long. */
static int read_record(FILE *fp, rec_t *r, size_t *len_out)
{
    size_t n = 0;
    int ch, inq = 0, any = 0;
    while ((ch = getc(fp)) != EOF)
    {
        any = 1;
        if (ch == '"') inq = !inq;
        if (ch == '\n' && !inq) break;
        if (n + 2 > r->cap)
        {
            size_t nc = r->cap ? r->cap * 2 : 4096;
            char *nb;
            if (nc > (size_t)MAXREC) return -1;
            nb = (char *)realloc(r->buf, nc);
            if (!nb) return -1;
            r->buf = nb; r->cap = nc;
        }
        r->buf[n++] = (char)ch;
    }
    if (!any) return 0;
    if (!r->buf) { r->buf = (char *)malloc(16); r->cap = 16; if (!r->buf) return -1; }
    if (n && r->buf[n - 1] == '\r') n--;
    r->buf[n] = 0;
    *len_out = n;
    return 1;
}

static int add_field(rec_t *r, char *p)
{
    if (r->nf == r->fcap)
    {
        int nc = r->fcap ? r->fcap * 2 : 32;
        char **nf = (char **)realloc(r->f, nc * sizeof(char *));
        if (!nf) return 0;
        r->f = nf; r->fcap = nc;
    }
    r->f[r->nf++] = p;
    return 1;
}

/* split in place, removing quotes; fields end with a zero byte */
static int split_record(rec_t *r, int delim)
{
    char *rd = r->buf, *wr = r->buf;
    int inq = 0;
    r->nf = 0;
    if (!add_field(r, wr)) return 0;
    for (;; rd++)
    {
        char c = *rd;
        if (!c) { *wr = 0; break; }
        if (inq)
        {
            if (c == '"')
            {
                if (rd[1] == '"') { *wr++ = '"'; rd++; }
                else inq = 0;
            }
            else *wr++ = c;
        }
        else if (c == '"') inq = 1;
        else if (c == delim)
        {
            *wr++ = 0;
            if (!add_field(r, wr)) return 0;
        }
        else *wr++ = c;
    }
    return 1;
}

static int detect_delim(const char *s, int dc)
{
    int cnt[3] = {0, 0, 0}, inq = 0, best = 0, i;
    static const char d[3] = { ',', ';', '\t' };
    for (; *s; s++)
    {
        if (*s == '"') inq = !inq;
        else if (!inq)
            for (i = 0; i < 3; i++) if (*s == d[i]) cnt[i]++;
    }
    if (dc) cnt[0] = -1;      /* commas are decimals */
    for (i = 1; i < 3; i++) if (cnt[i] > cnt[best]) best = i;
    return cnt[best] > 0 ? d[best] : (dc ? ';' : ',');
}

static int is_blank(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return !*s;
}

void kv_csv_free(kv_csv *c)
{
    int i;
    long j;
    if (!c) return;
    for (i = 0; i < c->ncols; i++)
    {
        if (c->name) free(c->name[i]);
        if (c->num) free(c->num[i]);
        if (c->txt && c->txt[i])
        {
            for (j = 0; j < c->nrows; j++) free(c->txt[i][j]);
            free(c->txt[i]);
        }
    }
    free(c->name); free(c->num); free(c->txt); free(c->is_time); free(c->t0);
    free(c);
}

int kv_csv_find(const kv_csv *c, const char *name)
{
    int i;
    for (i = 0; i < c->ncols; i++) if (!strcmp(c->name[i], name)) return i;
    return -1;
}

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char *trimdup(const char *s)
{
    size_t n;
    char *p;
    while (*s == ' ' || *s == '\t') s++;
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    p = (char *)malloc(n + 1);
    if (p) { memcpy(p, s, n); p[n] = 0; }
    return p;
}

static int grow(kv_csv *c)
{
    long nc = c->cap ? c->cap * 2 : 1024, j;
    int i;
    for (i = 0; i < c->ncols; i++)
    {
        double *nn = (double *)realloc(c->num[i], nc * sizeof(double));
        if (!nn) return 0;
        c->num[i] = nn;
        if (c->txt[i])
        {
            char **nt = (char **)realloc(c->txt[i], nc * sizeof(char *));
            if (!nt) return 0;
            for (j = c->cap; j < nc; j++) nt[j] = NULL;
            c->txt[i] = nt;
        }
    }
    c->cap = nc;
    return 1;
}

kv_csv *kv_csv_read(FILE *fp, const kv_csv_opts *o, char *err, size_t es)
{
    kv_csv *c = NULL;
    rec_t r = {0};
    size_t len;
    long lineno = 0, seen = 0, stride = o->stride > 0 ? o->stride : 1;
    int delim = o->delim, first = 1, ncf = 0, i, rc;
    int *sel = NULL;          /* file column index for each stored column */
    int nsel = 0;

    c = (kv_csv *)calloc(1, sizeof *c);
    if (!c) { snprintf(err, es, "out of memory"); return NULL; }

    while ((rc = read_record(fp, &r, &len)) == 1)
    {
        lineno++;
        if (lineno <= o->skip) continue;
        if (is_blank(r.buf) || (r.buf[0] == '#' && first == 0) || r.buf[0] == '#') continue;
        if (first)
        {
            int hdr;
            if (!delim) delim = detect_delim(r.buf, o->decimal_comma);
            if (!split_record(&r, delim)) { snprintf(err, es, "out of memory"); goto fail; }
            ncf = r.nf;
            if (o->header >= 0) hdr = o->header;
            else
            {
                double v;
                hdr = 0;
                for (i = 0; i < ncf; i++)
                    if (kv_csv_number_dc(r.f[i], &v, o->decimal_comma) == 0) { hdr = 1; break; }
            }
            /* which columns to keep */
            if (o->ncolsel > 0)
            {
                sel = (int *)malloc(o->ncolsel * sizeof(int));
                if (!sel) { snprintf(err, es, "out of memory"); goto fail; }
                for (i = 0; i < o->ncolsel; i++)
                {
                    int k, found = -1;
                    const char *w = o->colsel[i];
                    if (hdr)
                        for (k = 0; k < ncf; k++)
                        {
                            char *t = trimdup(r.f[k]);
                            int same = t && !strcmp(t, w);
                            free(t);
                            if (same) { found = k; break; }
                        }
                    if (found < 0)
                    {
                        char *e;
                        long v = strtol(w, &e, 10);
                        if (*w && !*e && v >= 0 && v < ncf) found = (int)v;
                    }
                    if (found < 0) { snprintf(err, es, "no column called %s", w); goto fail; }
                    sel[i] = found;
                }
                nsel = o->ncolsel;
            }
            else
            {
                sel = (int *)malloc(ncf * sizeof(int));
                if (!sel) { snprintf(err, es, "out of memory"); goto fail; }
                for (i = 0; i < ncf; i++) sel[i] = i;
                nsel = ncf;
            }
            c->ncols = nsel;
            c->name = (char **)calloc(nsel, sizeof(char *));
            c->num = (double **)calloc(nsel, sizeof(double *));
            c->txt = (char ***)calloc(nsel, sizeof(char **));
            c->is_time = (char *)calloc(nsel, 1);
            c->t0 = (double *)calloc(nsel, sizeof(double));
            if (!c->name || !c->num || !c->txt || !c->is_time || !c->t0) { snprintf(err, es, "out of memory"); goto fail; }
            for (i = 0; i < nsel; i++)
            {
                char nm[32];
                if (hdr)
                {
                    c->name[i] = trimdup(r.f[sel[i]]);
                    if (c->name[i] && !c->name[i][0])
                    {
                        free(c->name[i]);
                        snprintf(nm, sizeof nm, "col%d", sel[i] + 1);
                        c->name[i] = dupstr(nm);
                    }
                }
                else
                {
                    snprintf(nm, sizeof nm, "col%d", sel[i] + 1);
                    c->name[i] = dupstr(nm);
                }
                if (!c->name[i]) { snprintf(err, es, "out of memory"); goto fail; }
            }
            first = 0;
            if (hdr) continue;     /* header consumed; otherwise this row is data (already split) */
        }
        else if (!split_record(&r, delim)) { snprintf(err, es, "out of memory"); goto fail; }

        /* data row */
        if ((seen++) % stride) continue;
        if (c->nrows == c->cap && !grow(c)) { snprintf(err, es, "out of memory"); goto fail; }
        for (i = 0; i < nsel; i++)
        {
            const char *cell = sel[i] < r.nf ? r.f[sel[i]] : "";
            double v;
            int kind = kv_csv_number_dc(cell, &v, o->decimal_comma);
            if (kind == 0)
            {
                char *t = trimdup(cell);
                if (!t) { snprintf(err, es, "out of memory"); goto fail; }
                if (!c->txt[i])
                {
                    c->txt[i] = (char **)calloc(c->cap, sizeof(char *));
                    if (!c->txt[i]) { free(t); snprintf(err, es, "out of memory"); goto fail; }
                }
                c->txt[i][c->nrows] = t;
                v = kv_nan();
            }
            else if (kind == 3) c->is_time[i] = 1;
            c->num[i][c->nrows] = v;
        }
        c->nrows++;
        if (o->maxrows > 0 && c->nrows >= o->maxrows) break;
    }
    if (rc < 0) { snprintf(err, es, "a row is longer than %ld MB or memory ran out", MAXREC >> 20); goto fail; }
    if (first) { snprintf(err, es, "no data found"); goto fail; }
    c->rows_seen = seen;
    if (o->reltime)
        for (i = 0; i < c->ncols; i++)
        {
            long j;
            if (!c->is_time[i]) continue;
            for (j = 0; j < c->nrows && kv_isnan(c->num[i][j]); j++) ;
            if (j == c->nrows) continue;
            c->t0[i] = c->num[i][j];
            for (j = 0; j < c->nrows; j++) c->num[i][j] -= c->t0[i];   /* NaN stays NaN */
        }
    free(r.buf); free(r.f); free(sel);
    return c;
fail:
    free(r.buf); free(r.f); free(sel);
    kv_csv_free(c);
    return NULL;
}
