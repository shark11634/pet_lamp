/*
 * host_stubs/stub_headers/esp_log.h — 主机测试替身
 *
 * 默认静默；定义 HOST_TEST_VERBOSE_LOG 后把日志打到 stderr。
 */
#ifndef STUB_ESP_LOG_H
#define STUB_ESP_LOG_H

#include <stdio.h>

typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;

#ifdef HOST_TEST_VERBOSE_LOG
#define HOST_LOG(tag, fmt, ...) fprintf(stderr, "[%s] " fmt "\n", tag, ##__VA_ARGS__)
#else
/*
 * 静默模式必须**仍然引用**全部参数。
 *
 * 否则 -Wunused-parameter / -Wunused-variable 会把"只用于日志"的变量
 * 报成未使用 —— 那是替身的假警告，不是被测代码的问题。
 * 真实 ESP-IDF 的 ESP_LOGI 会把参数送进 vprintf，因此不会这样。
 *
 * 实现：`(void)(0, a, b)` 把每个参数求值一次（保证变量被"使用"）。
 *
 * 两个必需的抑制：
 *   __extension__        消除 -Wpedantic 对逗号表达式/空可变参数的报错
 *   -Wunused-value       消除"左操作数无副作用"（左操作数就是字面量 0）
 *
 * 这条 pragma 只作用于**本头文件被包含时**的词法范围，也就是只影响
 * 替身的日志宏，不会波及被测代码自身的任何警告。这是刻意选择：
 * 不要用"参数类型万能数组"那种花招 —— 日志参数混杂 int 与指针，
 * 而 GCC 8.1 的 C99 又不支持 __VA_OPT__，花招只会引入新问题。
 */
#pragma GCC diagnostic ignored "-Wunused-value"

#define HOST_LOG(tag, fmt, ...)                                       \
    do {                                                              \
        (void)(tag);                                                  \
        (void)(fmt);                                                  \
        __extension__ (void)(0, ##__VA_ARGS__);                       \
    } while (0)
#endif

#define ESP_LOGE(tag, fmt, ...) HOST_LOG(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) HOST_LOG(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) HOST_LOG(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) HOST_LOG(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) HOST_LOG(tag, fmt, ##__VA_ARGS__)

static inline void esp_log_level_set(const char *tag, esp_log_level_t level)
{
    (void)tag;
    (void)level;
}

#endif /* STUB_ESP_LOG_H */
