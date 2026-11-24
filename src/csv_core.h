/*
 * csv_core - a CSV/TSV reader that stores columns as arrays of doubles.
 * No Pd code in here, so it can be tested with a plain C compiler.
 *
 * Copyright (c) 2026 Sofia Vallejo Budziszewski. BSD 3-Clause.
 */
#ifndef KVX_CSV_CORE_H
#define KVX_CSV_CORE_H

#include <stdio.h>
#include <stddef.h>

typedef struct {
    int delim;              /* ',' ';' '\t' or 0 = detect */
    int header;             /* 1 = first row is a header, 0 = no header, -1 = detect */
    long skip;              /* lines to ignore at the start of the file */
    long stride;            /* keep every Nth data row (1 = all) */
    long maxrows;           /* stop after this many kept rows (0 = no limit) */
    int decimal_comma;      /* 1 = a comma is the decimal point (1,5 means 1.5) */
    int reltime;            /* 1 = time columns count seconds from their first row */
    int ncolsel;            /* number of entries in colsel (0 = all columns) */
    const char **colsel;    /* column names, or 1-based numbers */
} kv_csv_opts;

typedef struct {
    int ncols;
    long nrows;
    long cap;
    long rows_seen;         /* data rows in the file that were looked at */
    char **name;            /* [ncols] */
    double **num;           /* [ncols][nrows]; NaN for empty or text cells */
    char *is_time;          /* [ncols] 1 for columns that held dates or times */
    double *t0;             /* [ncols] epoch seconds of the first row, for time columns */
    char ***txt;            /* [ncols] is NULL for numeric columns, else [nrows] strings (NULL = number) */
} kv_csv;

void kv_csv_opts_default(kv_csv_opts *o);
kv_csv *kv_csv_read(FILE *f, const kv_csv_opts *o, char *err, size_t errsize);
void kv_csv_free(kv_csv *c);
int kv_csv_find(const kv_csv *c, const char *name);     /* column index or -1 */
int kv_csv_reserve(kv_csv *c, long rows);                /* room for this many rows; 0 = out of memory */

/* Returns 1 for a number, 3 for an ISO-8601 time (stored as seconds since 1970-01-01 UTC),
 * 2 for an empty cell or NaN/NA/null, 0 for text. */
int kv_csv_number(const char *s, double *out);
int kv_csv_number_dc(const char *s, double *out, int decimal_comma);

/* NaN test that survives -ffast-math (which pd-lib-builder turns on) */
#include <string.h>
static inline int kv_isnan(double v)
{
    unsigned long long u;
    memcpy(&u, &v, sizeof u);
    return ((u >> 52) & 0x7ff) == 0x7ff && (u & 0xfffffffffffffULL) != 0;
}
double kv_nan(void);

#endif
