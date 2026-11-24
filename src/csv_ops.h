/*
 * csv_ops - things to do with a kv_csv table once it is loaded: build one
 * row by row, add computed columns, thin it out, crop it, write it, and
 * read NumPy (.npy) and JSON Lines (.jsonl) files into the same shape.
 * No Pd code in here.
 *
 * Copyright (c) 2026 Sofia Vallejo Budziszewski. BSD 3-Clause.
 */
#ifndef KVX_CSV_OPS_H
#define KVX_CSV_OPS_H

#include "csv_core.h"

enum { KV_DEC_FIRST = 0, KV_DEC_MEAN = 1, KV_DEC_MINMAX = 2 };

kv_csv *kv_csv_new(int ncols, const char **names);         /* empty table */
int kv_csv_append_row(kv_csv *c, int n, const double *v);   /* 0 if n != ncols or no memory */
int kv_csv_setcol(kv_csv *c, const char *name, const double *v);   /* add, or replace if the name exists; v has nrows values. Returns the index or -1 */
int kv_csv_timecol(const kv_csv *c);                        /* first date/time column, or -1 */

int kv_csv_derive(kv_csv *c, int col, const char *name);    /* first difference; returns new index or -1 */
int kv_csv_smooth(kv_csv *c, int col, long n, const char *name);   /* centred moving average over n rows */
int kv_csv_decimate(kv_csv *c, long n, int mode, int col);  /* col is used by KV_DEC_MINMAX */
void kv_csv_crop(kv_csv *c, long start, long count);
int kv_csv_crop_time(kv_csv *c, int col, double t0, double t1);   /* keep rows with t0 <= time <= t1 */
long kv_csv_find_time(const kv_csv *c, int col, double t);  /* first row with time >= t (sorted column) */
void kv_csv_minmax(const kv_csv *c, int col, double *mn, double *mx);   /* NaN-free; 0,0 if empty */

/* writes a header and the rows; cols = NULL writes all columns. abstime writes time columns as dates (2024-03-05T10:20:30Z) */
int kv_csv_write(const kv_csv *c, FILE *f, int n, const int *cols, int abstime);

kv_csv *kv_npy_read(FILE *f, const kv_csv_opts *o, char *err, size_t errsize);
kv_csv *kv_jsonl_read(FILE *f, const kv_csv_opts *o, char *err, size_t errsize);

#endif
