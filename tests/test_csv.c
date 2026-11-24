#include "csv_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static kv_csv *rd(const char *text, kv_csv_opts *o, char *err)
{
    FILE *f = tmpfile();
    kv_csv *c;
    fwrite(text, 1, strlen(text), f);
    rewind(f);
    c = kv_csv_read(f, o, err, 200);
    fclose(f);
    return c;
}

int main(void)
{
    char err[200] = "";
    kv_csv_opts o;
    kv_csv *c;
    double v;

    /* numbers and times */
    CHECK(kv_csv_number("12.5", &v) == 1 && v == 12.5);
    CHECK(kv_csv_number("  -3e2 ", &v) == 1 && v == -300);
    CHECK(kv_csv_number("", &v) == 2 && kv_isnan(v));
    CHECK(kv_csv_number("NaN", &v) == 2);
    CHECK(kv_csv_number("NA", &v) == 2);
    CHECK(kv_csv_number("abc", &v) == 0);
    CHECK(kv_csv_number("inf", &v) == 0);
    CHECK(kv_csv_number("0x10", &v) == 0);
    CHECK(kv_csv_number("1.2.3", &v) == 0);
    CHECK(kv_csv_number("1970-01-01", &v) == 3 && v == 0);
    CHECK(kv_csv_number("1970-01-02T00:00:00Z", &v) == 3 && v == 86400);
    CHECK(kv_csv_number("2000-03-01 00:00:00", &v) == 3 && v == 951868800);
    CHECK(kv_csv_number("2024-02-29T12:30:15.5Z", &v) == 3 && fabs(v - 1709209815.5) < 1e-6);
    CHECK(kv_csv_number("2024-01-01T02:00:00+02:00", &v) == 3 && v == 1704067200);
    CHECK(kv_csv_number("2024-13-01", &v) == 0);
    CHECK(kv_csv_number("2024-01-01T25:00", &v) == 0);

    /* simple file with header */
    kv_csv_opts_default(&o);
    c = rd("t,x,y\n0,1.5,2\n1,2.5,4\n2,3.5,6\n", &o, err);
    CHECK(c && c->ncols == 3 && c->nrows == 3);
    CHECK(c && !strcmp(c->name[1], "x") && c->num[1][2] == 3.5 && c->num[2][1] == 4);
    CHECK(c && kv_csv_find(c, "y") == 2 && kv_csv_find(c, "zz") == -1);
    kv_csv_free(c);

    /* windows line ends, semicolons, no trailing newline, comments, blank lines */
    kv_csv_opts_default(&o);
    c = rd("# comment\r\na;b\r\n\r\n1;2\r\n# mid\r\n3;4", &o, err);
    CHECK(c && c->ncols == 2 && c->nrows == 2 && c->num[0][1] == 3 && c->num[1][1] == 4);
    kv_csv_free(c);

    /* tabs */
    c = rd("a\tb\n1\t2\n", &o, err);
    CHECK(c && c->ncols == 2 && c->num[1][0] == 2);
    kv_csv_free(c);

    /* no header: detected by all-numeric first row */
    c = rd("1,2\n3,4\n", &o, err);
    CHECK(c && c->nrows == 2 && !strcmp(c->name[0], "col1") && c->num[0][0] == 1);
    kv_csv_free(c);

    /* forced header off / on */
    o.header = 0;
    c = rd("a,b\n1,2\n", &o, err);
    CHECK(c && c->nrows == 2 && c->txt[0] && !strcmp(c->txt[0][0], "a"));
    kv_csv_free(c);
    kv_csv_opts_default(&o);
    o.header = 1;
    c = rd("1,2\n3,4\n", &o, err);
    CHECK(c && c->nrows == 1 && !strcmp(c->name[0], "1"));
    kv_csv_free(c);
    kv_csv_opts_default(&o);

    /* quotes, embedded delimiter, embedded newline, doubled quote */
    c = rd("name,val\n\"Smith, J\",1\n\"line1\nline2\",2\n\"say \"\"hi\"\"\",3\n", &o, err);
    CHECK(c && c->nrows == 3);
    CHECK(c && c->txt[0] && !strcmp(c->txt[0][0], "Smith, J"));
    CHECK(c && !strcmp(c->txt[0][1], "line1\nline2"));
    CHECK(c && !strcmp(c->txt[0][2], "say \"hi\""));
    CHECK(c && c->num[1][2] == 3);
    kv_csv_free(c);

    /* a text column that starts numeric, empty cells, short rows */
    c = rd("a,b,c\n1,x,5\n2,,\n3,4\n", &o, err);
    CHECK(c && c->nrows == 3);
    CHECK(c && c->txt[1] && !strcmp(c->txt[1][0], "x") && c->txt[1][1] == NULL && kv_isnan(c->num[1][1]));
    CHECK(c && c->num[2][0] == 5 && kv_isnan(c->num[2][1]) && kv_isnan(c->num[2][2]));
    CHECK(c && c->txt[0] == NULL);
    kv_csv_free(c);

    /* decimal comma */
    CHECK(kv_csv_number_dc("1,5", &v, 1) == 1 && v == 1.5);
    CHECK(kv_csv_number_dc("1,5", &v, 0) == 0);
    kv_csv_opts_default(&o);
    o.decimal_comma = 1;
    c = rd("a;b\n1,5;2,25\n-0,5;3\n", &o, err);
    CHECK(c && c->ncols == 2 && c->num[0][0] == 1.5 && c->num[1][0] == 2.25 && c->num[0][1] == -0.5);
    kv_csv_free(c);
    c = rd("a\tb\n1,5\t2\n", &o, err);
    CHECK(c && c->ncols == 2 && c->num[0][0] == 1.5);
    kv_csv_free(c);
    kv_csv_opts_default(&o);

    /* time column: relative by default, absolute on request */
    c = rd("time,v\n2024-01-01T00:00:00Z,1\n2024-01-01T00:00:01.5Z,2\n", &o, err);
    CHECK(c && c->is_time[0] && c->num[0][0] == 0 && c->num[0][1] == 1.5 && c->t0[0] == 1704067200);
    kv_csv_free(c);
    o.reltime = 0;
    c = rd("time,v\n2024-01-01T00:00:00Z,1\n", &o, err);
    CHECK(c && c->num[0][0] == 1704067200);
    kv_csv_free(c);
    kv_csv_opts_default(&o);

    /* time column becomes numbers */
    c = rd("time,v\n2024-01-01T00:00:00Z,1\n2024-01-01T00:00:01Z,2\n", &o, err);
    CHECK(c && c->txt[0] == NULL && c->num[0][1] - c->num[0][0] == 1);
    kv_csv_free(c);

    /* stride, maxrows, skip */
    {
        char *big = (char *)malloc(200000), *p;
        int i;
        p = big;
        p += sprintf(p, "i,sq\n");
        for (i = 0; i < 10000; i++) p += sprintf(p, "%d,%d\n", i, i * i);
        kv_csv_opts_default(&o);
        o.stride = 10;
        c = rd(big, &o, err);
        CHECK(c && c->nrows == 1000 && c->num[0][999] == 9990 && c->rows_seen == 10000);
        kv_csv_free(c);
        o.stride = 1; o.maxrows = 50;
        c = rd(big, &o, err);
        CHECK(c && c->nrows == 50 && c->num[1][49] == 2401);
        kv_csv_free(c);
        o.maxrows = 0; o.skip = 3;
        c = rd(big, &o, err);
        CHECK(c && c->nrows == 9998 && c->ncols == 2 && c->num[0][0] == 2);   /* header skipped too */
        kv_csv_free(c);
        free(big);
    }

    /* column selection by name and by number */
    {
        const char *sel[2] = { "y", "0" };
        kv_csv_opts_default(&o);
        o.colsel = sel; o.ncolsel = 2;
        c = rd("t,x,y\n0,1,2\n", &o, err);
        CHECK(c && c->ncols == 2 && !strcmp(c->name[0], "y") && c->num[0][0] == 2 && c->num[1][0] == 0);
        kv_csv_free(c);
        sel[0] = "nope";
        c = rd("t,x,y\n0,1,2\n", &o, err);
        CHECK(!c && strstr(err, "nope"));
    }

    /* header guess: a data line that has words is not a header */
    kv_csv_opts_default(&o);
    c = rd("2024-01-01,1.5,ABC\n2024-01-02,2.5,ABC\n", &o, err);
    CHECK(c && c->nrows == 2 && !strcmp(c->name[2], "col3") && !strcmp(c->txt[2][0], "ABC"));
    kv_csv_free(c);
    c = rd("name,city\nAna,Graz\nLuis,Wien\n", &o, err);       /* all words: header */
    CHECK(c && c->nrows == 2 && !strcmp(c->name[1], "city"));
    kv_csv_free(c);
    c = rd("# comment\nt,v\n\n# another\n1,2\n", &o, err);   /* comments and blanks between */
    CHECK(c && c->nrows == 1 && !strcmp(c->name[1], "v") && c->num[1][0] == 2);
    kv_csv_free(c);
    c = rd("only,words\n", &o, err);
    CHECK(!c || (c->nrows == 0 && !strcmp(c->name[0], "only")));
    kv_csv_free(c);

    /* errors */
    kv_csv_opts_default(&o);
    c = rd("", &o, err);
    CHECK(!c && strstr(err, "no data"));
    c = rd("# only a comment\n", &o, err);
    CHECK(!c);

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
