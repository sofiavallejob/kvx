/* Drives dict.c through the mock Pd. Each step sends a message and compares
 * everything that came out of the outlets. o0 = values, o1 = info, o2 = replies. */
#include "m_pd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char mock_log[];
extern void dict_setup(void);
extern t_class *mock_last_class(void);
extern void *mock_class_newmethod(t_class *c);

static int fails = 0, checks = 0;

static t_atom *parse_atoms(const char *line, int *n)
{
    static t_atom a[256];
    static char buf[20000];
    char *tok;
    *n = 0;
    strncpy(buf, line, sizeof buf - 1);
    for (tok = strtok(buf, " "); tok; tok = strtok(NULL, " "))
    {
        char *end;
        double d = strtod(tok, &end);
        if (*end == 0 && end != tok) SETFLOAT(&a[*n], (t_float)d);
        else SETSYMBOL(&a[*n], gensym(tok));
        (*n)++;
    }
    return a;
}

/* send a message to an object: "get a.b" */
static void send(t_pd *x, const char *msg)
{
    int n;
    t_atom *a = parse_atoms(msg, &n);
    if (n == 0) return;
    if (a[0].a_type == A_SYMBOL) pd_typedmess(x, a[0].a_w.w_symbol, n - 1, a + 1);
    else pd_typedmess(x, &s_list, n, a);
}

static void expect(t_pd *x, const char *msg, const char *want)
{
    checks++;
    mock_log[0] = 0;
    send(x, msg);
    if (strcmp(mock_log, want))
    {
        fails++;
        printf("FAIL  %s\n  want: %s  got:  %s", msg, want, mock_log);
        if (!strchr(want, '\n')) printf("\n");
    }
}

/* same, but the message holds one big symbol (as a [netreceive] would deliver) */
static void expect_sym(t_pd *x, const char *sel, const char *sym, const char *want)
{
    t_atom a;
    checks++;
    mock_log[0] = 0;
    SETSYMBOL(&a, gensym(sym));
    pd_typedmess(x, gensym(sel), 1, &a);
    if (strcmp(mock_log, want)) { fails++; printf("FAIL  %s %s\n  want: %s  got:  %s", sel, sym, want, mock_log); }
}

int main(void)
{
    t_atom args[1];
    t_pd *d, *d2, *priv;

    mock_log[0] = 0;
    dict_setup();
    SETSYMBOL(&args[0], gensym("demo"));
    {
        t_class *c = mock_last_class();
        d = (t_pd *)((void *(*)(t_symbol *, int, t_atom *))mock_class_newmethod(c))(gensym("dict"), 1, args);
        d2 = (t_pd *)((void *(*)(t_symbol *, int, t_atom *))mock_class_newmethod(c))(gensym("dict"), 1, args);
        priv = (t_pd *)((void *(*)(t_symbol *, int, t_atom *))mock_class_newmethod(c))(gensym("dict"), 0, NULL);
    }

    /* values */
    expect(d, "set synth.freq 220", "");
    expect(d, "set synth.name saw", "");
    expect(d, "set notes 60 64 67", "");
    expect(d, "set ratio 0.1", "");
    expect(d, "get synth.freq", "o0: float 220\n");
    expect(d, "get synth.name", "o0: symbol saw\n");
    expect(d, "get notes", "o0: list 60 64 67\n");
    expect(d, "get ratio", "o0: float 0.1\n");
    expect(d, "get synth", "o0: json {\"freq\":220,\"name\":\"saw\"}\n");
    expect(d, "get", "o0: json {\"synth\":{\"freq\":220,\"name\":\"saw\"},\"notes\":[60,64,67],\"ratio\":0.1}\n");
    expect(d, "get nothing.here", "o1: missing nothing.here\n");

    /* shared by name, private when unnamed */
    expect(d2, "get synth.freq", "o0: float 220\n");
    expect(priv, "get synth.freq", "o1: missing synth.freq\n");

    /* queries */
    expect(d, "keys synth", "o1: keys synth freq name\n");
    expect(d, "keys", "o1: keys . synth notes ratio\n");
    expect(d, "keys notes", "error: kvx/dict: keys: not an object\no1: error keys: not an object\n");
    expect(d, "has synth.freq", "o1: has synth.freq 1\n");
    expect(d, "has synth.nope", "o1: has synth.nope 0\n");
    expect(d, "type synth", "o1: type synth object\n");
    expect(d, "type notes", "o1: type notes array\n");
    expect(d, "type synth.name", "o1: type synth.name string\n");
    expect(d, "type synth.freq", "o1: type synth.freq number\n");
    expect(d, "type zip", "o1: type zip missing\n");
    expect(d, "size notes", "o1: size notes 3\n");
    expect(d, "size synth", "o1: size synth 2\n");
    expect(d, "size synth.name", "o1: size synth.name 3\n");

    /* nested lists, push */
    expect(d, "push log 1 2", "");
    expect(d, "push log 3", "");
    expect(d, "get log", "o0: json [[1,2],3]\n");
    expect(d, "push synth.freq 1", "error: kvx/dict: push: that is not a list\no1: error push: that is not a list\n");
    expect(d, "set grid.1.0 5", "");
    expect(d, "get grid", "o0: json [null,[5]]\n");   /* a numeric segment creates a list, padded with null */
    expect(d, "set arr.0 5", "");
    expect(d, "get arr", "o0: float 5\n");            /* a list of one prints like a float, as in Pd */
    expect(d, "type arr", "o1: type arr array\n");

    /* errors */
    expect(d, "set a", "error: kvx/dict: set: need a path and a value\no1: error set: need a path and a value\n");
    expect(d, "set notes.name 1", "error: kvx/dict: 'name' is not an index, but a list is there\no1: error 'name' is not an index, but a list is there\n");

    /* null and bool */
    expect(d, "setnull nothing", "");
    expect(d, "get nothing", "o0: symbol null\n");
    expect(d, "setbool on 1", "");
    expect(d, "get on", "o0: float 1\n");
    expect(d, "type on", "o1: type on bool\n");

    /* delete and clear */
    expect(d, "del synth.name", "");
    expect(d, "get synth", "o0: json {\"freq\":220}\n");
    expect(d, "del synth.name", "o1: missing synth.name\n");
    expect(d, "clear", "");
    expect(d, "get", "o0: json {}\n");
    expect(d, "bang", "o0: json {}\n");

    /* json text in one symbol, like a network message */
    expect_sym(d, "json", "{\"a\":[1,2,{\"b\":null}],\"s\":\"x y\"}", "");
    expect(d, "get a.2.b", "o0: symbol null\n");
    expect(d, "get a", "o0: json [1,2,{\"b\":null}]\n");
    expect(d, "get s", "o0: symbol x y\n");
    expect_sym(d, "json", "{bad", "error: kvx/dict: json: expected a string key at byte 1\no1: error json: expected a string key at byte 1\n");
    expect(d, "get a.0", "o0: float 1\n");           /* unchanged after the failed parse */
    expect_sym(d, "setjson", "x", "error: kvx/dict: setjson: need a path and JSON text\no1: error setjson: need a path and JSON text\n");
    {
        t_atom sj[2];
        SETSYMBOL(&sj[0], gensym("cfg.list"));
        SETSYMBOL(&sj[1], gensym("[1,2,3]"));
        mock_log[0] = 0;
        pd_typedmess(d, gensym("setjson"), 2, sj);
        checks++;
        if (mock_log[0]) { fails++; printf("FAIL setjson: %s", mock_log); }
    }
    expect(d, "get cfg.list", "o0: list 1 2 3\n");
    expect(d, "tojson cfg", "o0: json {\"list\":[1,2,3]}\n");
    expect(d, "tojson pretty cfg", "o0: json {\n  \"list\": [\n    1,\n    2,\n    3\n  ]\n}\n");
    expect(d, "tojson cfg pretty", "o0: json {\n  \"list\": [\n    1,\n    2,\n    3\n  ]\n}\n");

    /* files */
    expect(d, "write /tmp/kvx_test.json", "o1: write /tmp/kvx_test.json\n");
    expect(d, "clear", "");
    expect(d, "read /tmp/kvx_test.json", "o1: read /tmp/kvx_test.json\n");
    expect(d, "get cfg.list", "o0: list 1 2 3\n");
    expect(d, "read /tmp/does_not_exist.json", "error: kvx/dict: cannot open /tmp/does_not_exist.json\no1: error cannot open /tmp/does_not_exist.json\n");
    expect(d, "write", "error: kvx/dict: write: need a file name\no1: error write: need a file name\n");

    /* requests: answers go to the third outlet, tagged with the id */
    expect(d, "req 7 get cfg.list", "o2: send reply 7 json [1,2,3]\n");
    expect(d, "req 8 set synth.name saw", "o2: send reply 8 ok\n");
    expect(d, "req 9 get synth.name", "o2: send reply 9 json \"saw\"\n");
    expect(d, "req 10 keys synth", "o2: send reply 10 keys synth name\n");
    expect(d, "req abc get nope", "o2: send reply abc missing nope\n");
    expect(d, "req 11 has synth.name", "o2: send reply 11 has synth.name 1\n");
    expect(d, "req 12 size cfg.list", "o2: send reply 12 size cfg.list 3\n");
    expect(d, "req 13 set cfg.list.name 1", "o2: send reply 13 error 'name' is not an index, but a list is there\n");
    expect(d, "req 14 bang", "o2: send reply 14 json {\"a\":[1,2,{\"b\":null}],\"s\":\"x y\",\"cfg\":{\"list\":[1,2,3]},\"synth\":{\"name\":\"saw\"}}\n");
    expect(d, "req 15 push cfg.list 4", "o2: send reply 15 ok\n");
    expect(d, "get cfg.list", "o0: list 1 2 3 4\n");   /* normal mode again after a request */
    expect(d, "req 16", "");
    expect(d, "req 17 req 18 get x", "o2: send reply 17 ok\n");   /* a nested req is ignored */

    /* long replies come out in short words that glue back together */
    {
        char big[1300], want[1400], *p;
        int i;
        t_atom a;
        strcpy(big, "{\"k\":\"");
        for (i = 0; i < 1000; i++) strcat(big, i % 7 ? "a" : "\xc3\xa9");
        strcat(big, "\"}");
        SETSYMBOL(&a, gensym(big));
        mock_log[0] = 0;
        pd_typedmess(d, gensym("json"), 1, &a);
        mock_log[0] = 0;
        send(d, "req 30 get");
        checks++;
        p = mock_log + strlen("o2: send reply 30 json ");
        want[0] = 0;
        for (i = 0; *p && *p != '\n'; p++) if (*p != ' ') { want[i++] = *p; want[i] = 0; }
        if (strncmp(mock_log, "o2: send reply 30 json ", 23) || strcmp(want, big)) { fails++; printf("FAIL long reply\n"); }
        checks++;
        if (strlen(mock_log) < 1000 || !strchr(mock_log + 30, ' ')) { fails++; printf("FAIL long reply was not split\n"); }
    }

    /* JSON in pieces */
    expect(d, "jsonbuf", "");
    expect_sym(d, "jsonbuf", "{\"big\":[1,2,", "");
    expect_sym(d, "jsonbuf", "3,4],\"s\":\"ab\"}", "");
    expect(d, "jsonapply big_target", "");
    expect(d, "get big_target.big", "o0: list 1 2 3 4\n");
    expect(d, "jsonapply", "error: kvx/dict: jsonapply: nothing in the buffer\no1: error jsonapply: nothing in the buffer\n");
    expect_sym(d, "jsonbuf", "[1,2", "");
    expect(d, "jsonapply", "error: kvx/dict: jsonapply: unterminated array at byte 4\no1: error jsonapply: unterminated array at byte 4\n");
    expect(d, "req 20 jsonbuf", "o2: send reply 20 ok\n");

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
