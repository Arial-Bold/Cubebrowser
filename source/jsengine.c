/* jsengine.c - see jsengine.h. The speed test: a handful of small scripts,
 * each doing one kind of work web pages do (loops, function calls, text,
 * objects, JSON, regular expressions, maths), plus how long a big script
 * takes just to read in (sites send hundreds of KB of it). Each is timed and
 * its answer checked, so a wrong answer (a GameCube-only bug) shows too. */
#include "jsengine.h"
#include "quickjs.h"
#include "bigstack.h"
#include "memstat.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GEKKO
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
static double now_ms(void) { return ticks_to_microsecs(gettime()) / 1000.0; }
#else
#include <time.h>
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif

/* how deep scripts may go before QuickJS stops them (the big stack they run
 * on is 1 MB: this leaves room for the browser's own calls) */
#define JS_STACK_LIMIT (640 * 1024)

typedef struct { const char *name, *what, *code, *expect; } JsTest;

static const JsTest tests[] = {
    { "Counting loop", "200,000 additions",
      "let s = 0; for (let i = 0; i < 200000; i++) s += i; s", "19999900000" },
    { "Function calls", "fib(20): about 22,000 calls",
      "function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); } fib(20)", "6765" },
    { "Text", "5,000 pieces of text joined, searched and split",
      "let a = []; for (let i = 0; i < 5000; i++) a.push('item' + i);"
      "let s = a.join(','); s.indexOf('item4999') + ',' + s.split(',').length + ',' + s.toUpperCase().length",
      "43881,5000,43889" },
    { "Objects and sorting", "10,000 objects made and sorted",
      "let o = []; for (let i = 0; i < 10000; i++) o.push({ id: i, v: (i * 7919) % 10007 });"
      "o.sort((a, b) => a.v - b.v); o[0].id + ',' + o[9999].id",
      "0,1040" },
    { "JSON", "2,000 records turned into JSON and read back",
      "let d = { items: [] }; for (let i = 0; i < 2000; i++) d.items.push({ n: i, name: 'Thing ' + i, tags: ['a', 'b'], ok: i % 2 == 0 });"
      "let t = JSON.stringify(d); JSON.parse(t).items.length + ',' + t.length",
      "2000,114791" },
    { "Regular expressions", "links and addresses found and rewritten in 30 KB of text",
      "let t = ''; for (let i = 0; i < 500; i++) t += 'Visit https://example.com/page' + i + ' or mail bob' + i + '@mail.com. ';"
      "(t.match(/https?:\\/\\/[^\\s]+/g) || []).length + ',' + t.replace(/\\b(\\w+)@(\\w+)\\.com/g, '$2 at $1').length",
      "500,28780" },
    { "Maths", "50,000 sines and square roots",
      "let x = 0; for (let i = 1; i < 50000; i++) x += Math.sin(i) * Math.sqrt(i); Math.round(x)",
      NULL },   /* (its answer depends a little on the maths library: not checked) */
};
#define NTESTS (int)(sizeof(tests) / sizeof(tests[0]))

/* a big made-up script, like a site's: functions, objects, strings */
static char *big_script(size_t *len) {
    const int n = 1500;
    size_t cap = (size_t)n * 160 + 64;
    char *s = malloc(cap);
    if (!s) return NULL;
    size_t k = 0;
    for (int i = 0; i < n; i++)
        k += (size_t)snprintf(s + k, cap - k,
            "function f%d(a, b) { var c = a + b * %d; if (c > 10) { return { v: c, s: 'item %d' }; } "
            "return [a, b, c].map(function (x) { return x * 2; }).join(','); }\n", i, i, i);
    *len = k;
    return s;
}

typedef struct { char *out; size_t cap; } Job;

static void put(Job *j, size_t *k, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void put(Job *j, size_t *k, const char *fmt, ...) {
    if (*k >= j->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->out + *k, j->cap - *k, fmt, ap);
    va_end(ap);
    if (n > 0) *k += (size_t)n;
    if (*k >= j->cap) *k = j->cap - 1;
}

static void run(void *arg) {
    Job *j = arg;
    size_t k = 0;
    put(j, &k, "<title>JavaScript speed test</title><h2>JavaScript speed test</h2>"
               "<p>CubeBrowser's new JavaScript engine (QuickJS), timed on this console. "
               "Each test is a small script doing one kind of work web pages do.</p>");
    size_t free0 = cb_mem_free();
    double t0 = now_ms();
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = rt ? JS_NewContext(rt) : NULL;
    double t_start = now_ms() - t0;
    if (!ctx) {
        put(j, &k, "<p><b>The engine couldn't start: not enough memory.</b></p>");
        if (rt) JS_FreeRuntime(rt);
        return;
    }
    JS_SetMaxStackSize(rt, JS_STACK_LIMIT);
    size_t used_start = free0 > cb_mem_free() ? free0 - cb_mem_free() : 0;

    put(j, &k, "<table border=1 cellpadding=4><tr><th>Test</th><th>What it does</th><th>Time</th><th>Answer</th></tr>");
    double total = 0;
    int wrong = 0;
    for (int i = 0; i < NTESTS; i++) {
        const JsTest *t = &tests[i];
        double a = now_ms();
        JSValue v = JS_Eval(ctx, t->code, strlen(t->code), "<test>", JS_EVAL_TYPE_GLOBAL);
        double ms = now_ms() - a;
        total += ms;
        char ans[64] = "";
        bool ok;
        if (JS_IsException(v)) {
            JSValue e = JS_GetException(ctx);
            const char *es = JS_ToCString(ctx, e);
            snprintf(ans, sizeof(ans), "error: %.48s", es ? es : "?");
            if (es) JS_FreeCString(ctx, es);
            JS_FreeValue(ctx, e);
            ok = false;
        } else {
            const char *s = JS_ToCString(ctx, v);
            snprintf(ans, sizeof(ans), "%s", s ? s : "?");
            if (s) JS_FreeCString(ctx, s);
            ok = !t->expect || !strcmp(ans, t->expect);
        }
        JS_FreeValue(ctx, v);
        if (!ok) wrong++;
        put(j, &k, "<tr><td>%s</td><td>%s</td><td>%.0f ms</td><td>%s%s</td></tr>",
            t->name, t->what, ms, ok ? "" : "<b>WRONG</b> ", ans);
        /* each test in a fresh scope: let/const names would clash otherwise */
        JS_FreeContext(ctx);
        ctx = JS_NewContext(rt);
        if (!ctx) break;
    }
    /* reading in a big script (only reading: nothing in it runs) */
    size_t blen = 0;
    char *big = ctx ? big_script(&blen) : NULL;
    if (big) {
        double a = now_ms();
        JSValue fn = JS_Eval(ctx, big, blen, "<big>", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        double ms = now_ms() - a;
        bool ok = !JS_IsException(fn);
        JS_FreeValue(ctx, fn);
        free(big);
        put(j, &k, "<tr><td>Reading a big script</td><td>%u KB of code, read but not run</td><td>%.0f ms</td><td>%s%.0f KB a second</td></tr>",
            (unsigned)(blen / 1024), ms, ok ? "" : "<b>WRONG</b> ", ms > 0 ? blen / 1024.0 / (ms / 1000.0) : 0);
        if (!ok) wrong++;
    }
    size_t used_peak = free0 > cb_mem_free() ? free0 - cb_mem_free() : 0;
    put(j, &k, "</table>");
    put(j, &k, "<p><b>All the small tests:</b> %.0f ms. %s</p>", total,
        wrong ? "<b>Some answers were wrong: please report this.</b>" : "Every answer was right.");
    put(j, &k, "<p><b>Starting the engine:</b> %.0f ms, %u KB of memory. "
               "<b>With the tests' leftovers:</b> %u KB. "
               "<b>Memory still free:</b> %u KB.</p>",
        t_start, (unsigned)(used_start / 1024), (unsigned)(used_peak / 1024), (unsigned)(cb_mem_free() / 1024));
    if (ctx) JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    put(j, &k, "<p><a href=\"about:js\">Run it again</a> &nbsp; <a href=\"about:home\">Start page</a></p>");
}

void js_speed_test_html(char *out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    Job j = { out, cap };
    run_with_big_stack(run, &j);   /* (scripts can go deep: the 1 MB stack) */
}
