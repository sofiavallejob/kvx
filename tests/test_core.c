/* Tests for kv_core (no Pd needed). Build: see Makefile target "test". */
#include "../src/kv_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static char *rt(const char *json, int pretty)
{
    char err[128];
    kv_node *n = kv_parse(json, strlen(json), err, sizeof err);
    char *out;
    if (!n) { printf("parse error for %s: %s\n", json, err); { char *e = (char *)malloc(8); strcpy(e, "<error>"); return e; } }
    out = kv_dump(n, pretty, NULL);
    kv_free(n);
    return out;
}

static void expect_rt(const char *in, const char *want)
{
    char *got = rt(in, 0);
    checks++;
    if (strcmp(got, want)) { fails++; printf("FAIL roundtrip\n  in:   %s\n  want: %s\n  got:  %s\n", in, want, got); }
    free(got);
}

static void expect_err(const char *in)
{
    char err[128];
    kv_node *n = kv_parse(in, strlen(in), err, sizeof err);
    checks++;
    if (n) { fails++; printf("FAIL should not parse: %s\n", in); kv_free(n); }
    else if (!err[0]) { fails++; printf("FAIL no message for: %s\n", in); }
}

static kv_node *P(const char *json)
{
    char err[128];
    kv_node *n = kv_parse(json, strlen(json), err, sizeof err);
    if (!n) { printf("parse error: %s (%s)\n", err, json); exit(2); }
    return n;
}

int main(void)
{
    char err[128];
    kv_node *root, *n;
    char *s;
    int i;

    /* ---- round trips */
    expect_rt("{\"a\":1,\"b\":[1,2,3],\"c\":{\"d\":\"x\"}}", "{\"a\":1,\"b\":[1,2,3],\"c\":{\"d\":\"x\"}}");
    expect_rt(" [ 1 , 2.5 , -3e2 , true , false , null ] ", "[1,2.5,-300,true,false,null]");
    expect_rt("\"\\u00e9\\n\\t\\\"\"", "\"\xc3\xa9\\n\\t\\\"\"");
    expect_rt("\"\\ud83d\\ude00\"", "\"\xf0\x9f\x98\x80\"");           /* surrogate pair -> U+1F600 */
    expect_rt("\"\\ud83d\"", "\"\xef\xbf\xbd\"");                      /* lone surrogate */
    expect_rt("\"a\\u0000b\"", "\"a\xef\xbf\xbd" "b\"");              /* NUL replaced */
    expect_rt("{}", "{}");
    expect_rt("[]", "[]");
    expect_rt("0.1", "0.1");
    expect_rt("1e20", "1e+20");
    expect_rt("123456789012", "123456789012");
    expect_rt("{\"a\":1,\"a\":2}", "{\"a\":2}");                      /* duplicate key: last wins */
    expect_rt("\xEF\xBB\xBF[1]", "[1]");                               /* BOM */
    expect_rt("\"\x01\"", "\"\\u0001\"");

    /* ---- pretty */
    s = rt("{\"a\":[1,2],\"b\":{}}", 1);
    CHECK(!strcmp(s, "{\n  \"a\": [\n    1,\n    2\n  ],\n  \"b\": {}\n}"));
    free(s);

    /* ---- errors */
    expect_err("");
    expect_err("{");
    expect_err("[1,2");
    expect_err("[1 2]");
    expect_err("{\"a\" 1}");
    expect_err("{a:1}");
    expect_err("\"abc");
    expect_err("tru");
    expect_err("[1,]x");
    expect_err("1 2");
    expect_err("\"\\x\"");
    expect_err("\"\\u12\"");
    expect_err("1.2.3");

    /* depth bomb must fail cleanly, not crash */
    {
        char *bomb = (char *)malloc(2000);
        memset(bomb, '[', 1000);
        bomb[1000] = 0;
        expect_err(bomb);
        free(bomb);
    }

    /* ---- paths: get */
    root = P("{\"synth\":{\"freq\":220,\"name\":\"saw\",\"partials\":[1,2,3]},\"tags\":[{\"n\":\"a\"},{\"n\":\"b\"}]}");
    n = kv_path_get(root, "synth.freq");
    CHECK(n && n->type == KV_NUM && n->num == 220);
    n = kv_path_get(root, "synth.partials.1");
    CHECK(n && n->num == 2);
    n = kv_path_get(root, "synth.partials[2]");
    CHECK(n && n->num == 3);
    n = kv_path_get(root, "tags[1].n");
    CHECK(n && n->type == KV_STR && !strcmp(n->str, "b"));
    CHECK(kv_path_get(root, "nope") == NULL);
    CHECK(kv_path_get(root, "synth.partials.9") == NULL);
    CHECK(kv_path_get(root, "synth.freq.x") == NULL);
    CHECK(kv_path_get(root, "") == root);
    CHECK(kv_path_get(root, ".") == root);
    CHECK(kv_path_get(root, "a..b") == NULL);
    CHECK(kv_path_get(root, "a.") == NULL);
    CHECK(kv_path_get(root, "a[1") == NULL);

    /* ---- paths: set */
    CHECK(kv_path_set(&root, "synth.freq", kv_num(440), err, sizeof err) == 0);
    CHECK(kv_path_get(root, "synth.freq")->num == 440);
    CHECK(kv_path_set(&root, "new.deep.leaf", kv_str("x", 1), err, sizeof err) == 0);
    s = kv_dump(kv_path_get(root, "new"), 0, NULL);
    CHECK(!strcmp(s, "{\"deep\":{\"leaf\":\"x\"}}"));
    free(s);
    CHECK(kv_path_set(&root, "list.0", kv_num(1), err, sizeof err) == 0);     /* creates an array */
    CHECK(kv_path_set(&root, "list.2", kv_num(3), err, sizeof err) == 0);     /* null padded */
    s = kv_dump(kv_path_get(root, "list"), 0, NULL);
    CHECK(!strcmp(s, "[1,null,3]"));
    free(s);
    CHECK(kv_path_set(&root, "list.name", kv_num(1), err, sizeof err) == -1);  /* key on an array */
    CHECK(err[0] != 0);
    CHECK(kv_path_set(&root, "list.99999999", kv_num(1), err, sizeof err) == -1);
    CHECK(kv_path_set(&root, "synth.freq.sub", kv_num(5), err, sizeof err) == 0);  /* scalar replaced by object */
    CHECK(kv_path_get(root, "synth.freq.sub")->num == 5);
    CHECK(kv_path_set(&root, "tags[0].n", kv_str("z", 1), err, sizeof err) == 0);
    CHECK(!strcmp(kv_path_get(root, "tags.0.n")->str, "z"));
    CHECK(kv_path_set(&root, "bad..path", kv_num(1), err, sizeof err) == -1);

    /* ---- paths: delete */
    CHECK(kv_path_del(root, "synth.name") == 1);
    CHECK(kv_path_get(root, "synth.name") == NULL);
    CHECK(kv_path_del(root, "synth.name") == 0);
    CHECK(kv_path_del(root, "tags.0") == 1);
    CHECK(!strcmp(kv_path_get(root, "tags.0.n")->str, "b"));        /* shifted down */
    CHECK(kv_path_del(root, "nothing.here") == 0);
    CHECK(kv_path_del(root, "") == 0);
    kv_free(root);

    /* ---- set the root itself */
    root = kv_obj();
    CHECK(kv_path_set(&root, ".", kv_num(7), err, sizeof err) == 0);
    CHECK(root->type == KV_NUM);
    CHECK(kv_path_set(&root, "a.b", kv_num(1), err, sizeof err) == 0);   /* number root becomes an object */
    CHECK(root->type == KV_OBJ);
    kv_free(root);

    /* ---- many keys: hash index, update, delete */
    root = kv_obj();
    for (i = 0; i < 20000; i++)
    {
        char key[32];
        sprintf(key, "k%d", i);
        CHECK(kv_path_set(&root, key, kv_num(i), err, sizeof err) == 0);
    }
    CHECK(root->len == 20000);
    CHECK(kv_path_get(root, "k0")->num == 0);
    CHECK(kv_path_get(root, "k19999")->num == 19999);
    CHECK(kv_path_get(root, "k20000") == NULL);
    for (i = 0; i < 20000; i += 2)
    {
        char key[32];
        sprintf(key, "k%d", i);
        if (kv_path_del(root, key) != 1) { CHECK(0); break; }
    }
    CHECK(root->len == 10000);
    CHECK(kv_path_get(root, "k2") == NULL);
    CHECK(kv_path_get(root, "k3")->num == 3);
    CHECK(kv_path_get(root, "k19999")->num == 19999);
    CHECK(kv_path_set(&root, "k3", kv_num(-3), err, sizeof err) == 0);
    CHECK(root->len == 10000);
    CHECK(kv_path_get(root, "k3")->num == -3);
    s = kv_dump(root, 0, NULL);
    n = kv_parse(s, strlen(s), err, sizeof err);                      /* big round trip */
    CHECK(n && n->len == 10000);
    kv_free(n);
    free(s);
    kv_free(root);

    /* ---- float32 from Pd prints the way it was typed */
    {
        kv_node *f = kv_num(kv_float_to_double(0.1f));
        s = kv_dump(f, 0, NULL);
        CHECK(!strcmp(s, "0.1"));
        free(s);
        kv_free(f);
        f = kv_num(kv_float_to_double(220.0f));
        s = kv_dump(f, 0, NULL);
        CHECK(!strcmp(s, "220"));
        free(s);
        kv_free(f);
        f = kv_num(kv_float_to_double(-0.001f));
        s = kv_dump(f, 0, NULL);
        CHECK(!strcmp(s, "-0.001"));
        free(s);
        kv_free(f);
    }

    /* ---- non finite numbers become null */
    {
        kv_node *f = kv_num(1.0 / 0.0);
        s = kv_dump(f, 0, NULL);
        CHECK(!strcmp(s, "null"));
        free(s);
        kv_free(f);
    }

    /* ---- a comma-decimal locale must not change anything (skipped if not installed) */
    if (setlocale(LC_NUMERIC, "de_DE.UTF-8") || setlocale(LC_NUMERIC, "de_DE"))
    {
        expect_rt("[1.5,0.25]", "[1.5,0.25]");
        {
            kv_node *f = kv_num(kv_float_to_double(0.1f));
            s = kv_dump(f, 0, NULL);
            CHECK(!strcmp(s, "0.1"));
            free(s);
            kv_free(f);
        }
        setlocale(LC_NUMERIC, "C");
        printf("(locale test ran)\n");
    }
    else printf("(locale test skipped: de_DE not installed)\n");

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
