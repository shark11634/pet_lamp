/*
 * pet_test.c — 主机测试公共骨架实现
 */
#include "pet_test.h"

#include <stdarg.h>
#include <stdio.h>

static int g_fails;

void t_check(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        ++g_fails;
    }
}

void t_range(long v, long lo, long hi, const char *what)
{
    const int ok = (v >= lo && v <= hi);
    printf("  [%s] %s (值=%ld 期望 %ld..%ld)\n", ok ? "PASS" : "FAIL", what, v, lo, hi);
    if (!ok) {
        ++g_fails;
    }
}

void t_eq(long got, long want, const char *what)
{
    const int ok = (got == want);
    printf("  [%s] %s (得到 %ld 期望 %ld)\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) {
        ++g_fails;
    }
}

void t_section(const char *title)
{
    printf("\n--- %s ---\n", title);
}

void t_info(const char *fmt, ...)
{
    va_list ap;
    printf("  [INFO] ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

int t_fails(void)
{
    return g_fails;
}

int t_run(const char *suite, const pet_test_case_t *cases, size_t count)
{
    printf("\n=== %s ===\n", suite);
    for (size_t i = 0; i < count; ++i) {
        printf("\n[%u/%u] %s\n", (unsigned)(i + 1u), (unsigned)count, cases[i].name);
        if (cases[i].fn != NULL) {
            cases[i].fn();
        }
    }
    return g_fails;
}
