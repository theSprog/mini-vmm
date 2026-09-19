/* tests/unit/harness.h — 单元测试的最小断言框架
 *
 * 只做三件事：计数、失败时打印足够定位的现场、给 shell 层一个可断言的
 * 摘要行（"N checks, M failed"）。刻意不做 fixture / mock / 参数化，
 * 那些东西在这个规模上只会让失败输出变难读。
 *
 * 约定：
 *   T_CASE("名字")            开一个用例，后面的 CHECK 都算在它头上
 *   CHECK(cond, fmt, ...)     条件为假时记一次失败并打印上下文
 *   CHECK_U64(got, want, ...) 数值比较，失败时同时打十进制和十六进制
 *   CHECK_PTR(got, want, ...) 指针比较
 *   T_SUMMARY()               打印摘要，返回退出码（0 = 全过）
 *
 * 退出码：0 全过，1 有失败。摘要行格式固定为
 *   "<binary>: <N> checks in <C> cases, <M> failed"
 * shell 层断言 "0 failed"，和 boot_tables.sh 原有的约定保持一致。
 */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static const char *t_prog  = "test";
static const char *t_case_name = "(no case)";
static unsigned    t_checks, t_fails, t_cases, t_case_fails;

static inline void t_init(const char *prog)
{
    const char *slash = strrchr(prog, '/');
    t_prog = slash ? slash + 1 : prog;
}

static inline void t_case(const char *name)
{
    if (t_cases && !t_case_fails)
        printf("  ok   %s\n", t_case_name);
    t_case_name = name;
    t_case_fails = 0;
    t_cases++;
}

static inline int t_finish(void)
{
    if (t_cases && !t_case_fails)
        printf("  ok   %s\n", t_case_name);
    printf("%s: %u checks in %u cases, %u failed\n",
           t_prog, t_checks, t_cases, t_fails);
    return t_fails ? 1 : 0;
}

#define T_MAIN_BEGIN()  do { t_init(argv[0]); (void)argc; } while (0)
#define T_CASE(name)    t_case(name)
#define T_SUMMARY()     t_finish()

#define T_FAILED_(...)                                                  \
    do {                                                                \
        t_fails++; t_case_fails++;                                      \
        printf("  FAIL %s\n       %s:%d: ", t_case_name,                \
               __FILE__, __LINE__);                                     \
        printf(__VA_ARGS__);                                            \
        printf("\n");                                                   \
    } while (0)

#define CHECK(cond, ...)                                                \
    do {                                                                \
        t_checks++;                                                     \
        if (!(cond))                                                    \
            T_FAILED_(__VA_ARGS__);                                     \
    } while (0)

#define CHECK_U64(got, want, ...)                                       \
    do {                                                                \
        uint64_t g_ = (uint64_t)(got), w_ = (uint64_t)(want);           \
        t_checks++;                                                     \
        if (g_ != w_) {                                                 \
            t_fails++; t_case_fails++;                                  \
            printf("  FAIL %s\n       %s:%d: ", t_case_name,            \
                   __FILE__, __LINE__);                                 \
            printf(__VA_ARGS__);                                        \
            printf("\n       got  %llu (0x%llx)\n       want %llu (0x%llx)\n", \
                   (unsigned long long)g_, (unsigned long long)g_,      \
                   (unsigned long long)w_, (unsigned long long)w_);     \
        }                                                               \
    } while (0)

#define CHECK_PTR(got, want, ...)                                       \
    do {                                                                \
        const void *g_ = (const void *)(got);                           \
        const void *w_ = (const void *)(want);                          \
        t_checks++;                                                     \
        if (g_ != w_) {                                                 \
            t_fails++; t_case_fails++;                                  \
            printf("  FAIL %s\n       %s:%d: ", t_case_name,            \
                   __FILE__, __LINE__);                                 \
            printf(__VA_ARGS__);                                        \
            printf("\n       got  %p\n       want %p\n", g_, w_);       \
        }                                                               \
    } while (0)

#endif /* TEST_HARNESS_H */
