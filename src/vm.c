/* src/vm.c — VM 生命周期：打开 KVM、检查能力、创建 VM、内存、中断控制器和 vCPU */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>

#include "vmm.h"
#include "memory.h"
#include "kvm_wrappers.h"
#include "arch/arch.h"
#include "devices/irqchip.h"

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

int vm_create_vcpu(struct vmm_vm *vm, int id)
{
    struct vmm_vcpu *v;
    int r;

    if (id < 0 || id >= VMM_MAX_VCPUS)
        return VMM_ERR_INVAL;

    v = &vm->vcpus[id];
    memset(v, 0, sizeof(*v));
    v->vm = vm;
    v->id = id;

    v->stats_fd = -1;
    pthread_mutex_init(&v->work_lock, NULL);
    v->work_tail = &v->work_head;

    v->fd = kvm_create_vcpu(vm->vm_fd, id);
    if (v->fd < 0)
        return VMM_ERR_SYS;

    /* 统计 fd 只有 --exit-stats 和 monitor 用。必须在 vCPU 线程启动前打开，
     * 理由见 struct vmm_vcpu.stats_fd；没人用时不打开，host 内核不支持时
     * 也就不会在每次启动都报一条 ioctl 错误 */
    if (vm->cfg.exit_stats_path || vm->cfg.monitor_path)
        v->stats_fd = kvm_get_stats_fd(v->fd);

    v->run_size = vm->vcpu_mmap_size;
    v->run = kvm_mmap_run(v->fd, v->run_size);
    if (!v->run) {
        close(v->fd);
        v->fd = -1;
        return VMM_ERR_SYS;
    }

    if (vm->arch->init_vcpu) {
        r = vm->arch->init_vcpu(v);
        if (r != VMM_OK)
            return r;
    }

    if (id + 1 > vm->nr_vcpus)
        vm->nr_vcpus = id + 1;

    vmm_dbg("vCPU %d created (fd=%d, kvm_run=%p, %zu bytes)",
            id, v->fd, (void *)v->run, v->run_size);
    return VMM_OK;
}

/* KVM 的 API 版本，以及这个 VMM 离不开的几项能力 */
static int check_kvm(struct vmm_vm *vm, const struct vmm_config *cfg)
{
    int max_vcpus;

    /* API 版本必须精确等于 12。KVM 从 2007 年定下这个数字后再没
     * 改过——新功能全部通过 KVM_CHECK_EXTENSION 增量协商，
     * 而不是升版本号。所以不等于 12 就说明环境根本不对。 */
    vm->api_version = kvm_get_api_version(vm->kvm_fd);
    if (vm->api_version != KVM_API_VERSION) {
        vmm_err("KVM API version %d, expected %d",
                vm->api_version, KVM_API_VERSION);
        return VMM_ERR_SYS;
    }

    if (!kvm_check_extension(vm->kvm_fd, KVM_CAP_USER_MEMORY)) {
        vmm_err("kernel does not support KVM_CAP_USER_MEMORY");
        return VMM_ERR_SYS;
    }

    vm->vcpu_mmap_size = (size_t)kvm_get_vcpu_mmap_size(vm->kvm_fd);
    if ((ssize_t)vm->vcpu_mmap_size < (ssize_t)sizeof(struct kvm_run)) {
        vmm_err("KVM_GET_VCPU_MMAP_SIZE=%zu is smaller than "
                "sizeof(kvm_run)=%zu; the linux/kvm.h used at build "
                "time is probably newer than the running kernel",
                vm->vcpu_mmap_size, sizeof(struct kvm_run));
        return VMM_ERR_SYS;
    }

    max_vcpus = kvm_check_extension(vm->kvm_fd, KVM_CAP_MAX_VCPUS);
    vmm_info("KVM API v%d, kvm_run size %zu bytes, max vCPUs %d",
             vm->api_version, vm->vcpu_mmap_size, max_vcpus);
    if (max_vcpus > 0 && cfg->nr_vcpus > max_vcpus) {
        vmm_err("%d vCPUs requested, KVM allows at most %d",
                cfg->nr_vcpus, max_vcpus);
        return VMM_ERR_SYS;
    }

    /* 停机协调依赖 kvm_run->immediate_exit，见 smp.c vm_request_stop() */
    if (!kvm_check_extension(vm->kvm_fd, KVM_CAP_IMMEDIATE_EXIT)) {
        vmm_err("kernel does not support KVM_CAP_IMMEDIATE_EXIT (needs 4.11+)");
        return VMM_ERR_SYS;
    }
    return VMM_OK;
}

/* 探测 CPU 厂商并校验虚拟化能力 */
static int probe_arch(struct vmm_vm *vm)
{
    vm->arch = arch_probe();
    if (!vm->arch)
        return VMM_ERR_SYS;
    if (vm->arch->check_features)
        return vm->arch->check_features(vm);
    return VMM_OK;
}

/* 创建 VM，再做厂商相关的 VM 级准备（Intel 的 TSS/identity map 在这里） */
static int create_vm_fd(struct vmm_vm *vm, const struct vmm_config *cfg)
{
    vm->vm_fd = kvm_create_vm(vm->kvm_fd);
    if (vm->vm_fd < 0)
        return VMM_ERR_SYS;
    if (cfg->exit_stats_path || cfg->monitor_path)
        vm->stats_fd = kvm_get_stats_fd(vm->vm_fd);
    if (vm->arch->prepare_vm)
        return vm->arch->prepare_vm(vm);
    return VMM_OK;
}

/* Linux 需要中断控制器和 PIT，且必须在创建 vCPU 之前建好：KVM 在
 * KVM_CREATE_VCPU 时才决定是否给 vCPU 挂内核态 LAPIC。裸 payload 模式
 * 不创建任何中断控制器。 */
static int create_irqchip(struct vmm_vm *vm, const struct vmm_config *cfg)
{
    const struct irqchip_ops *chip;
    int r;

    if (cfg->mode != VMM_BOOT_LINUX)
        return VMM_OK;
    chip = cfg->irqchip == VMM_IRQCHIP_SPLIT ? &irqchip_split_ops
                                             : &irqchip_kernel_ops;
    r = chip->create(vm);
    if (r != VMM_OK)
        return r;
    vm->irqchip = chip;
    return VMM_OK;
}

int vm_create(struct vmm_vm *vm, const struct vmm_config *cfg)
{
    int i;

    memset(vm, 0, sizeof(*vm));
    vm->kvm_fd = vm->vm_fd = vm->stats_fd = -1;
    vm->cfg = *cfg;
    pthread_mutex_init(&vm->io_lock, NULL);
    io_trace_open(vm);

    vm->kvm_fd = kvm_open();
    if (vm->kvm_fd < 0)
        return VMM_ERR_SYS;

    /* 顺序有依赖：VM 要在确认能力和厂商之后创建，内存和中断控制器
     * 挂在 VM 上，vCPU 最后建（理由见 create_irqchip） */
    if (check_kvm(vm, cfg) != VMM_OK ||
        probe_arch(vm) != VMM_OK ||
        create_vm_fd(vm, cfg) != VMM_OK ||
        mem_init(vm, cfg->ram_size) != VMM_OK ||
        create_irqchip(vm, cfg) != VMM_OK)
        goto fail;
    for (i = 0; i < cfg->nr_vcpus; i++) {
        if (vm_create_vcpu(vm, i) != VMM_OK)
            goto fail;
    }

    vm->running = true;
    return VMM_OK;

fail:
    vm_destroy(vm);
    return VMM_ERR_SYS;
}

void vm_destroy(struct vmm_vm *vm)
{
    int i;

    for (i = 0; i < VMM_MAX_VCPUS; i++) {
        struct vmm_vcpu *v = &vm->vcpus[i];
        if (v->run)
            munmap(v->run, v->run_size);
        if (v->fd > 0)
            close(v->fd);
        if (v->stats_fd > 0)
            close(v->stats_fd);
        if (i < vm->nr_vcpus)
            pthread_mutex_destroy(&v->work_lock);
        v->run = NULL;
        v->fd = v->stats_fd = -1;
    }
    if (vm->stats_fd > 0)
        close(vm->stats_fd);
    vm->stats_fd = -1;

    vm_power_on_free(vm);
    if (vm->irqchip && vm->irqchip->destroy)
        vm->irqchip->destroy(vm);
    vm->irqchip = NULL;
    mem_destroy(vm);

    if (vm->vm_fd >= 0)
        close(vm->vm_fd);
    if (vm->kvm_fd >= 0)
        close(vm->kvm_fd);
    vm->vm_fd = vm->kvm_fd = -1;
    vm->running = false;
    io_trace_close();
}
