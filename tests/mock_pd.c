/* A tiny stand-in for the parts of Pd that dict.c uses, so the object's
 * logic can be tested without a running Pd. Needs the real m_pd.h for the
 * types (pass PDINCLUDEDIR). Not part of the shipped library. */
#include "m_pd.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXM 64
struct _class {
    char name[32];
    t_newmethod newm;
    t_method freem;
    size_t size;
    struct { char sel[32]; t_method fn; int types[6]; int n; } m[MAXM];
    int nm;
};

struct _outlet { void *owner; int index; };

static t_class *last_class;
char mock_log[200000];
static void logf_(const char *fmt, ...)
{
    va_list ap;
    size_t l = strlen(mock_log);
    va_start(ap, fmt);
    vsnprintf(mock_log + l, sizeof mock_log - l, fmt, ap);
    va_end(ap);
}

t_symbol s_list = {"list", 0, 0};
t_symbol s_float = {"float", 0, 0};
t_symbol s_symbol = {"symbol", 0, 0};
t_symbol s_bang = {"bang", 0, 0};

static t_symbol *syms[4096];
static int nsyms;
t_symbol *gensym(const char *s)
{
    int i;
    for (i = 0; i < nsyms; i++) if (!strcmp(syms[i]->s_name, s)) return syms[i];
    syms[nsyms] = (t_symbol *)calloc(1, sizeof(t_symbol));
    syms[nsyms]->s_name = strdup(s);
    return syms[nsyms++];
}

t_class *class_new(t_symbol *name, t_newmethod newm, t_method freem, size_t size, int flags, t_atomtype a1, ...)
{
    t_class *c = (t_class *)calloc(1, sizeof *c);
    (void)flags; (void)a1;
    strncpy(c->name, name->s_name, sizeof c->name - 1);
    c->newm = newm; c->freem = freem; c->size = size;
    last_class = c;
    return c;
}

void class_addmethod(t_class *c, t_method fn, t_symbol *s, t_atomtype a1, ...)
{
    va_list ap;
    int t = (int)a1, n = 0;
    strncpy(c->m[c->nm].sel, s->s_name, 31);
    c->m[c->nm].fn = fn;
    va_start(ap, a1);
    while (t && n < 6) { c->m[c->nm].types[n++] = t; t = va_arg(ap, int); }
    va_end(ap);
    c->m[c->nm].n = n;
    c->nm++;
}

#undef class_addbang
void class_addbang(t_class *c, t_method fn)
{
    strcpy(c->m[c->nm].sel, "bang");
    c->m[c->nm].fn = fn;
    c->m[c->nm].n = 0;
    c->nm++;
}

t_pd *pd_new(t_class *c)
{
    t_object *x = (t_object *)calloc(1, c->size);
    *(t_class **)x = c;
    return (t_pd *)x;
}

t_outlet *outlet_new(t_object *owner, t_symbol *s)
{
    static t_outlet pool[256];
    static int used;
    t_outlet *o = &pool[used++];
    int i;
    (void)s;
    o->owner = owner;
    o->index = 0;
    for (i = 0; i < used - 1; i++) if (pool[i].owner == owner) o->index++;
    return o;
}

static void put_atom(const t_atom *a)
{
    if (a->a_type == A_FLOAT) logf_(" %g", (double)a->a_w.w_float);
    else if (a->a_type == A_SYMBOL) logf_(" %s", a->a_w.w_symbol->s_name);
    else logf_(" ?");
}

void outlet_float(t_outlet *o, t_float f) { logf_("o%d: float %g\n", o->index, (double)f); }
void outlet_symbol(t_outlet *o, t_symbol *s) { logf_("o%d: symbol %s\n", o->index, s->s_name); }
void outlet_list(t_outlet *o, t_symbol *s, int argc, t_atom *argv)
{
    int i;
    (void)s;
    logf_("o%d: list", o->index);
    for (i = 0; i < argc; i++) put_atom(&argv[i]);
    logf_("\n");
}
void outlet_anything(t_outlet *o, t_symbol *s, int argc, t_atom *argv)
{
    int i;
    logf_("o%d: %s", o->index, s->s_name);
    for (i = 0; i < argc; i++) put_atom(&argv[i]);
    logf_("\n");
}

void *getbytes(size_t n) { return calloc(1, n ? n : 1); }
void freebytes(void *p, size_t n) { (void)n; free(p); }
void post(const char *fmt, ...)
{
    va_list ap;
    char buf[1000];
    va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    logf_("post: %s\n", buf);
}
void pd_error(const void *obj, const char *fmt, ...)
{
    va_list ap;
    char buf[1000];
    (void)obj;
    va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    logf_("error: %s\n", buf);
}
void atom_string(const t_atom *a, char *buf, unsigned int size)
{
    if (a->a_type == A_FLOAT) snprintf(buf, size, "%g", (double)a->a_w.w_float);
    else snprintf(buf, size, "%s", a->a_w.w_symbol->s_name);
}
t_canvas *canvas_getcurrent(void) { return NULL; }
void canvas_makefilename(const t_glist *c, const char *file, char *result, int size)
{ (void)c; snprintf(result, (size_t)size, "%s", file); }
FILE *sys_fopen(const char *f, const char *m) { return fopen(f, m); }
int sys_fclose(FILE *f) { return fclose(f); }

void pd_typedmess(t_pd *x, t_symbol *s, int argc, t_atom *argv)
{
    t_class *c = *x;
    int i;
    for (i = 0; i < c->nm; i++)
    {
        if (strcmp(c->m[i].sel, s->s_name)) continue;
        if (c->m[i].n == 0 && !strcmp(s->s_name, "bang")) { ((void (*)(void *))c->m[i].fn)(x); return; }
        if (c->m[i].n == 0) { ((void (*)(void *))c->m[i].fn)(x); return; }
        if (c->m[i].types[0] == A_GIMME) { ((void (*)(void *, t_symbol *, int, t_atom *))c->m[i].fn)(x, s, argc, argv); return; }
        if (c->m[i].n == 1 && c->m[i].types[0] == A_SYMBOL)
        { ((void (*)(void *, t_symbol *))c->m[i].fn)(x, argv[0].a_w.w_symbol); return; }
        if (c->m[i].n == 2 && c->m[i].types[0] == A_SYMBOL && c->m[i].types[1] == A_FLOAT)
        { ((void (*)(void *, t_symbol *, t_float))c->m[i].fn)(x, argv[0].a_w.w_symbol, argv[1].a_w.w_float); return; }
    }
    logf_("error: no method for %s\n", s->s_name);
}


t_class *mock_last_class(void) { return last_class; }
void *mock_class_newmethod(t_class *c) { return (void *)c->newm; }
