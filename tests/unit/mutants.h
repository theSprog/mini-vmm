/* tests/unit/mutants.h — negative control：故意写错的实现
 *
 * 解决的问题：一套断言全绿，只说明它在当前实现上没报警，不说明它有能力
 * 报警。要证明后者，必须看见它红过。
 *
 * 为什么不用“改一行源码再撤销”那种做法：那是一次性的。行号会漂，
 * 重构一次就失效，没法留在仓库里。这里把故障注入在**接口层**——
 * 每个 mutant 是一份完整的、故意写错的实现，只依赖函数签名。
 * `src/mem.c` 和 `src/vm.c` 内部怎么重构都不影响它们，能一直用下去。
 *
 * 怎么用：`mem_test --mutant N` / `io_bus_test --mutant N` 把被测函数
 * 换成第 N 个 mutant 再跑同一套用例，**要求进程非零退出**。
 * 由 tests/offline/negctl.sh 逐个驱动。
 *
 * 它证明什么、不证明什么：
 *   证明   —— 这套输入和 oracle 有能力区分正确实现与这几类错误实现；
 *   不证明 —— 测试确实连在真实代码上（那由链接关系保证），
 *             也不证明没有别的错误类型能溜过去。
 *
 * 加新契约时配一个对应的 mutant，这份清单就一直有意义。
 */
#ifndef TEST_MUTANTS_H
#define TEST_MUTANTS_H

#include <stdbool.h>
#include <stdint.h>

#include "vmm.h"
#include "memory.h"

typedef void *(*gpa_to_hva_fn)(struct vmm_vm *vm, uint64_t gpa, uint64_t len);

typedef int (*pio_dispatch_fn)(struct vmm_vcpu *vcpu, uint16_t port,
                               bool is_write, uint32_t size, uint32_t count,
                               uint8_t *data);

typedef int (*mmio_dispatch_fn)(struct vmm_vcpu *vcpu, uint64_t addr,
                                bool is_write, uint32_t len, uint8_t *data);

struct mem_mutant {
    const char   *name;
    const char   *breaks;      /* 这个 mutant 违反的是哪一条契约 */
    gpa_to_hva_fn fn;
};

struct io_mutant {
    const char      *name;
    const char      *breaks;
    pio_dispatch_fn  pio;      /* NULL 表示这一路用真实实现 */
    mmio_dispatch_fn mmio;
};

extern const struct mem_mutant mem_mutants[];
extern const int               mem_mutants_count;

extern const struct io_mutant  io_mutants[];
extern const int               io_mutants_count;

#endif /* TEST_MUTANTS_H */
