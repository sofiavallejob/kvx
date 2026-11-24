#include "csv_ops.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static FILE *mem(const void *data, size_t n)
{
    FILE *f = tmpfile();
    fwrite(data, 1, n, f);
    rewind(f);
    return f;
}

static kv_csv *csv(const char *t)
{
    kv_csv_opts o;
    char err[200];
    FILE *f = mem(t, strlen(t));
    kv_csv *c;
    kv_csv_opts_default(&o);
    c = kv_csv_read(f, &o, err, sizeof err);
    fclose(f);
    return c;
}

static char *write_str(kv_csv *c, int n, const int *cols, int abs)
{
    FILE *f = tmpfile();
    long len;
    char *s;
    kv_csv_write(c, f, n, cols, abs);
    len = ftell(f);
    rewind(f);
    s = (char *)calloc(len + 1, 1);
    if (fread(s, 1, len, f) != (size_t)len) s[0] = 0;
    fclose(f);
    return s;
}

/* a .npy file in memory */
static size_t make_npy(unsigned char *out, const char *descr, const char *shape, int fortran, const void *data, size_t dlen)
{
    char hdr[256];
    size_t hl, pad, n = 0;
    snprintf(hdr, sizeof hdr, "{'descr': '%s', 'fortran_order': %s, 'shape': %s, }", descr, fortran ? "True" : "False", shape);
    hl = strlen(hdr);
    pad = 64 - ((10 + hl + 1) % 64);
    if (pad == 64) pad = 0;
    memcpy(out, "\x93NUMPY\x01\x00", 8); n = 8;
    out[n++] = (unsigned char)((hl + pad + 1) & 0xff);
    out[n++] = (unsigned char)((hl + pad + 1) >> 8);
    memcpy(out + n, hdr, hl); n += hl;
    memset(out + n, ' ', pad); n += pad;
    out[n++] = '\n';
    memcpy(out + n, data, dlen); n += dlen;
    return n;
}

int main(void)
{
    kv_csv *c;
    char err[200], *s;
    kv_csv_opts o;
    double mn, mx;

    /* building row by row, writing */
    {
        const char *nm[2] = { "x", "y" };
        double r1[2] = { 1, 2.5 }, r2[2] = { 3, 0.1 }, bad[1] = { 0 };
        long i;
        c = kv_csv_new(2, nm);
        CHECK(c && c->ncols == 2 && c->nrows == 0);
        CHECK(kv_csv_append_row(c, 2, r1));
        CHECK(kv_csv_append_row(c, 2, r2));
        CHECK(!kv_csv_append_row(c, 1, bad));
        s = write_str(c, 0, NULL, 0);
        CHECK(!strcmp(s, "x,y\n1,2.5\n3,0.1\n"));
        free(s);
        for (i = 0; i < 5000; i++) { double r[2] = { (double)i, -i }; kv_csv_append_row(c, 2, r); }
        CHECK(c->nrows == 5002 && c->num[1][5001] == -4999);
        kv_csv_free(c);
    }

    /* write keeps text, quotes, empty cells, column choice, absolute time */
    c = csv("t,name,v\n2024-01-01T00:00:00Z,\"a, b\",1\n2024-01-01T00:00:02Z,\"say \"\"x\"\"\",\n");
    CHECK(c != NULL);
    s = write_str(c, 0, NULL, 0);
    CHECK(!strcmp(s, "t,name,v\n0,\"a, b\",1\n2,\"say \"\"x\"\"\",\n"));
    free(s);
    {
        int cols[2] = { 2, 0 };
        s = write_str(c, 2, cols, 1);
        CHECK(!strcmp(s, "v,t\n1,2024-01-01T00:00:00Z\n,2024-01-01T00:00:02Z\n"));
        free(s);
    }
    CHECK(kv_csv_timecol(c) == 0);
    /* written file reads back the same */
    {
        FILE *f = tmpfile();
        kv_csv *b;
        kv_csv_write(c, f, 0, NULL, 1);
        rewind(f);
        kv_csv_opts_default(&o);
        b = kv_csv_read(f, &o, err, sizeof err);
        fclose(f);
        CHECK(b && b->nrows == 2 && !strcmp(b->txt[1][1], "say \"x\"") && b->num[0][1] == 2 && b->t0[0] == 1704067200);
        kv_csv_free(b);
    }
    kv_csv_free(c);

    /* derive, smooth, setcol replaces */
    c = csv("v\n1\n4\n9\nNA\n25\n");
    CHECK(kv_csv_derive(c, 0, "d") == 1);
    CHECK(c->num[1][0] == 0 && c->num[1][1] == 3 && c->num[1][2] == 5 && kv_isnan(c->num[1][3]) && kv_isnan(c->num[1][4]));
    CHECK(kv_csv_smooth(c, 0, 3, "s") == 2);
    CHECK(c->num[2][0] == 2.5 && c->num[2][1] == 14.0 / 3 && c->num[2][2] == 6.5 && c->num[2][3] == 17 && c->num[2][4] == 25);
    CHECK(kv_csv_smooth(c, 0, 1, "s") == 2 && c->num[2][1] == 4 && c->ncols == 3);
    kv_csv_minmax(c, 0, &mn, &mx);
    CHECK(mn == 1 && mx == 25);
    CHECK(kv_csv_derive(c, 9, "z") == -1);
    kv_csv_free(c);

    /* decimate */
    {
        char *big = (char *)malloc(100000), *p = big;
        long i;
        p += sprintf(p, "i,v,lab\n");
        for (i = 0; i < 1000; i++) p += sprintf(p, "%ld,%d,L%ld\n", i, (i == 437) ? 100 : (i == 440 ? -100 : 0), i);
        c = csv(big);
        CHECK(kv_csv_decimate(c, 10, KV_DEC_FIRST, -1) && c->nrows == 100 && c->num[0][43] == 430 && !strcmp(c->txt[2][43], "L430"));
        kv_csv_free(c);
        c = csv(big);
        CHECK(kv_csv_decimate(c, 10, KV_DEC_MINMAX, 1));
        mn = 0; mx = 0;
        kv_csv_minmax(c, 1, &mn, &mx);
        CHECK(mn == -100 && mx == 100);     /* the spikes survive */
        CHECK(c->nrows < 200 && c->nrows >= 100);
        kv_csv_free(c);
        c = csv(big);
        CHECK(kv_csv_decimate(c, 10, KV_DEC_MEAN, -1) && c->nrows == 100 && c->num[0][0] == 4.5 && c->num[1][0] == 0 && c->num[1][43] == 10 && !strcmp(c->txt[2][1], "L10"));
        kv_csv_free(c);
        c = csv(big);
        CHECK(kv_csv_decimate(c, 7, KV_DEC_MEAN, -1) && c->nrows == 143 && c->num[0][142] == 996.5);   /* last block is short */
        kv_csv_free(c);
        free(big);
    }

    /* crop and time */
    c = csv("t,v,s\n2024-01-01T00:00:00Z,0,a\n2024-01-01T00:00:01Z,1,b\n2024-01-01T00:00:02Z,2,c\n2024-01-01T00:00:03Z,3,d\n");
    CHECK(kv_csv_find_time(c, 0, 1.5) == 2 && kv_csv_find_time(c, 0, -5) == 0 && kv_csv_find_time(c, 0, 99) == 4);
    kv_csv_crop(c, 1, 2);
    CHECK(c->nrows == 2 && c->num[1][0] == 1 && !strcmp(c->txt[2][1], "c"));
    kv_csv_free(c);
    c = csv("t,v,s\n2024-01-01T00:00:00Z,0,a\n2024-01-01T00:00:01Z,1,b\n2024-01-01T00:00:02Z,2,c\n2024-01-01T00:00:03Z,3,d\n");
    CHECK(kv_csv_crop_time(c, 0, 0.5, 2.5) && c->nrows == 2 && c->num[1][0] == 1 && c->num[1][1] == 2);
    kv_csv_crop(c, 5, 5);
    CHECK(c->nrows == 0);
    kv_csv_free(c);

    /* dates written back */
    c = csv("t\n2024-02-29T23:59:59.5Z\n1969-12-31T00:00:00Z\n2000-01-01\n");
    s = write_str(c, 0, NULL, 1);
    CHECK(!strcmp(s, "t\n2024-02-29T23:59:59.500Z\n1969-12-31T00:00:00Z\n2000-01-01T00:00:00Z\n"));
    free(s);
    kv_csv_free(c);

    /* npy: 2D float64, 1D float32, int16, Fortran order, options */
    {
        unsigned char buf[4096];
        size_t n;
        FILE *f;
        double d[6] = { 1, 2, 3, 4, 5, 6 };
        float fl[3] = { 0.1f, 0.5f, -2 };
        short sh[4] = { -3, 7, 32000, 0 };
        const char *sel[1] = { "col2" };
        n = make_npy(buf, "<f8", "(3, 2)", 0, d, sizeof d);
        f = mem(buf, n);
        kv_csv_opts_default(&o);
        c = kv_npy_read(f, &o, err, sizeof err);
        fclose(f);
        CHECK(c && c->ncols == 2 && c->nrows == 3 && c->num[0][2] == 5 && c->num[1][2] == 6 && !strcmp(c->name[1], "col2"));
        kv_csv_free(c);
        o.colsel = sel; o.ncolsel = 1; o.stride = 2;
        f = mem(buf, n);
        c = kv_npy_read(f, &o, err, sizeof err);
        fclose(f);
        CHECK(c && c->ncols == 1 && c->nrows == 2 && c->num[0][0] == 2 && c->num[0][1] == 6);
        kv_csv_free(c);
        kv_csv_opts_default(&o);
        n = make_npy(buf, "<f8", "(3, 2)", 1, d, sizeof d);    /* column-major: col1 = 1 2 3 */
        f = mem(buf, n);
        c = kv_npy_read(f, &o, err, sizeof err);
        fclose(f);
        CHECK(c && c->num[0][1] == 2 && c->num[1][0] == 4);
        kv_csv_free(c);
        n = make_npy(buf, "<f4", "(3,)", 0, fl, sizeof fl);
        f = mem(buf, n);
        c = kv_npy_read(f, &o, err, sizeof err);
        fclose(f);
        CHECK(c && c->ncols == 1 && c->nrows == 3 && c->num[0][0] == 0.1 && c->num[0][2] == -2);
        kv_csv_free(c);
        n = make_npy(buf, "<i2", "(2, 2)", 0, sh, sizeof sh);
        f = mem(buf, n);
        o.maxrows = 1;
        c = kv_npy_read(f, &o, err, sizeof err);
        fclose(f);
        CHECK(c && c->nrows == 1 && c->num[0][0] == -3 && c->num[1][0] == 7);
        kv_csv_free(c);
        kv_csv_opts_default(&o);
        n = make_npy(buf, ">f8", "(3, 2)", 0, d, sizeof d);
        f = mem(buf, n);
        c = kv_npy_read(f, &o, err, sizeof err);
        fclose(f);
        CHECK(!c && strstr(err, "big-endian"));
        n = make_npy(buf, "<f8", "(2, 2, 2)", 0, d, 8 * 8 > sizeof d ? sizeof d : 64);
        f = mem(buf, n);
        c = kv_npy_read(f, &o, err, sizeof err);
        fclose(f);
        CHECK(!c && strstr(err, "dimensions"));
        n = make_npy(buf, "<f8", "(30, 2)", 0, d, sizeof d);   /* data missing */
        f = mem(buf, n);
        c = kv_npy_read(f, &o, err, sizeof err);
        fclose(f);
        CHECK(!c && strstr(err, "shorter"));
        f = mem("hello world", 11);
        c = kv_npy_read(f, &o, err, sizeof err);
        fclose(f);
        CHECK(!c && strstr(err, "not a .npy"));
    }

    /* JSON Lines */
    {
        const char *t =
            "{\"time\": \"2024-01-01T00:00:00Z\", \"temp\": 20.5, \"ok\": true, \"room\": \"lab\"}\n"
            "\n"
            "{\"time\": \"2024-01-01T00:01:00Z\", \"temp\": 21, \"room\": \"hall\", \"extra\": 5}\n"
            "{\"temp\": null, \"time\": \"2024-01-01T00:02:00Z\", \"ok\": false, \"room\": \"1,5\"}\r\n";
        FILE *f = mem(t, strlen(t));
        const char *sel[2] = { "temp", "room" };
        kv_csv_opts_default(&o);
        c = kv_jsonl_read(f, &o, err, sizeof err);
        fclose(f);
        CHECK(c && c->ncols == 4 && c->nrows == 3);
        CHECK(c && c->is_time[0] && c->num[0][1] == 60 && c->t0[0] == 1704067200);
        CHECK(c && c->num[1][0] == 20.5 && kv_isnan(c->num[1][2]));
        CHECK(c && c->num[2][0] == 1 && kv_isnan(c->num[2][1]) && c->num[2][2] == 0);
        CHECK(c && c->txt[3] && !strcmp(c->txt[3][1], "hall") && !strcmp(c->txt[3][2], "1,5"));
        kv_csv_free(c);
        o.colsel = sel; o.ncolsel = 2; o.maxrows = 2;
        f = mem(t, strlen(t));
        c = kv_jsonl_read(f, &o, err, sizeof err);
        fclose(f);
        CHECK(c && c->ncols == 2 && c->nrows == 2 && !strcmp(c->name[1], "room"));
        kv_csv_free(c);
        kv_csv_opts_default(&o);
        f = mem("{\"a\":1}\n[1,2]\n", 14);
        c = kv_jsonl_read(f, &o, err, sizeof err);
        fclose(f);
        CHECK(!c && strstr(err, "line 2"));
        f = mem("{\"a\":1}\n{\"a\":\n", 14);
        c = kv_jsonl_read(f, &o, err, sizeof err);
        fclose(f);
        CHECK(!c && strstr(err, "line 2"));
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
