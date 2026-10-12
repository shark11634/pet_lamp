/*
 * pet_test.h — 主机测试公共骨架
 */
#ifndef PET_TEST_H
#define PET_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pet_test_fn)(void);

typedef struct {
    const char  *name;
    pet_test_fn  fn;
} pet_test_case_t;

/** 成立断言 */
void t_check(int cond, const char *what);

/** 区间断言（含端点） */
void t_range(long v, long lo, long hi, const char *what);

/** 相等断言（long） */
void t_eq(long got, long want, const char *what);

/** 打印一段标题 */
void t_section(const char *title);

/** 打印信息行 */
void t_info(const char *fmt, ...);

/** 累计失败数 */
int  t_fails(void);

/** 运行一组用例，返回失败数 */
int  t_run(const char *suite, const pet_test_case_t *cases, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* PET_TEST_H */
