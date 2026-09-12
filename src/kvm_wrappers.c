/* src/kvm_wrappers.c — KVM ioctl 薄封装 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "vmm.h"
#include "kvm_wrappers.h"

/* 统一的 ioctl 包装：出错时打出 ioctl 名字。
 * 没有这层的话，调试期看到的全是 "ioctl failed: Invalid argument"，
 * 完全无法定位是哪一步。 */
#define KVM_IOCTL(fd, req, arg) ({                                    \
    int _r = ioctl((fd), (req), (arg));                           \
    if (_r < 0)                                                   \
        vmm_err("ioctl(%s) failed: %s", #req, strerror(errno));\
    _r;                                                           \
})

int kvm_open(void)
{
    int fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        vmm_err("open /dev/kvm: %s", strerror(errno));
        if (errno == EACCES)
            vmm_err("  hint: is the current user in the kvm group? (id -nG | grep kvm)");
        if (errno == ENOENT)
            vmm_err("  hint: is the kvm_amd module loaded? (lsmod | grep kvm)");
        return VMM_ERR_SYS;
    }
    return fd;
}

int kvm_get_api_version(int kvm_fd)
{
    return KVM_IOCTL(kvm_fd, KVM_GET_API_VERSION, 0);
}

/* 注意：必须用 KVM_CHECK_EXTENSION 做运行时探测，而不是看头文件里
 * 有没有定义某个 KVM_CAP_*。发行版的 linux-libc-dev 常常比运行中的
 * 内核新，头文件里有的宏，跑起来的内核不一定认。 */
int kvm_check_extension(int kvm_fd, int cap)
{
    int r = ioctl(kvm_fd, KVM_CHECK_EXTENSION, cap);
    return r < 0 ? 0 : r;
}

int kvm_get_vcpu_mmap_size(int kvm_fd)
{
    return KVM_IOCTL(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
}

int kvm_create_vm(int kvm_fd)
{
    /* 第三个参数是 machine type，x86 上必须为 0 */
    return KVM_IOCTL(kvm_fd, KVM_CREATE_VM, (unsigned long)0);
}

int kvm_create_vcpu(int vm_fd, int vcpu_id)
{
    return KVM_IOCTL(vm_fd, KVM_CREATE_VCPU, (unsigned long)vcpu_id);
}

int kvm_set_user_memory_region(int vm_fd, uint32_t slot, uint32_t flags,
                               uint64_t gpa, uint64_t size, uint64_t hva)
{
    struct kvm_userspace_memory_region r;

    memset(&r, 0, sizeof(r));
    r.slot            = slot;
    r.flags           = flags;
    r.guest_phys_addr = gpa;
    r.memory_size     = size;
    r.userspace_addr  = hva;

    return KVM_IOCTL(vm_fd, KVM_SET_USER_MEMORY_REGION, &r) < 0
           ? VMM_ERR_SYS : VMM_OK;
}

/* KVM_SET_TSS_ADDR 是 _IO(KVMIO, 0x47)，参数按值传，不是指针 */
int kvm_set_tss_addr(int vm_fd, uint64_t addr)
{
    return KVM_IOCTL(vm_fd, KVM_SET_TSS_ADDR, (unsigned long)addr) < 0
           ? VMM_ERR_SYS : VMM_OK;
}

/* KVM_SET_IDENTITY_MAP_ADDR 是 _IOW(KVMIO, 0x48, __u64)，参数传指针 */
int kvm_set_identity_map_addr(int vm_fd, uint64_t addr)
{
    return KVM_IOCTL(vm_fd, KVM_SET_IDENTITY_MAP_ADDR, &addr) < 0
           ? VMM_ERR_SYS : VMM_OK;
}

struct kvm_run *kvm_mmap_run(int vcpu_fd, size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   vcpu_fd, 0);
    if (p == MAP_FAILED) {
        vmm_err("mmap kvm_run: %s", strerror(errno));
        return NULL;
    }
    return (struct kvm_run *)p;
}

int kvm_get_regs(int vcpu_fd, struct kvm_regs *regs)
{
    return KVM_IOCTL(vcpu_fd, KVM_GET_REGS, regs) < 0 ? VMM_ERR_SYS : VMM_OK;
}

int kvm_set_regs(int vcpu_fd, const struct kvm_regs *regs)
{
    return KVM_IOCTL(vcpu_fd, KVM_SET_REGS, regs) < 0 ? VMM_ERR_SYS : VMM_OK;
}

int kvm_get_sregs(int vcpu_fd, struct kvm_sregs *sregs)
{
    return KVM_IOCTL(vcpu_fd, KVM_GET_SREGS, sregs) < 0 ? VMM_ERR_SYS : VMM_OK;
}

int kvm_set_sregs(int vcpu_fd, const struct kvm_sregs *sregs)
{
    return KVM_IOCTL(vcpu_fd, KVM_SET_SREGS, sregs) < 0 ? VMM_ERR_SYS : VMM_OK;
}

int kvm_run(int vcpu_fd)
{
    int r;

    /* 两种“没有 exit 可处理、重进 KVM_RUN 即可”的返回：
     *   EINTR   vm_request_stop() 用 immediate_exit + SIGUSR1 把 vCPU
     *           踢出来（Ctrl-A x、另一个 vCPU 触发了停机）
     *   EAGAIN  AP 处于 KVM_MP_STATE_UNINITIALIZED 时 KVM_RUN 阻塞到
     *           收到 INIT/SIPI，醒来后返回 -EAGAIN 而不是进 guest，
     *           见 host arch/x86/kvm/x86.c kvm_arch_vcpu_ioctl_run()
     * 两者都不写（或不保证写）exit_reason，单独返回 VMM_ERR_INTR，
     * 让 run loop 回去检查 should_stop 后重进。 */
    r = ioctl(vcpu_fd, KVM_RUN, 0);
    if (r < 0 && (errno == EINTR || errno == EAGAIN))
        return VMM_ERR_INTR;

    if (r < 0) {
        vmm_err("KVM_RUN: %s", strerror(errno));
        return VMM_ERR_SYS;
    }
    return VMM_OK;
}

int kvm_get_supported_cpuid(int kvm_fd, struct kvm_cpuid2 **out)
{
    struct kvm_cpuid2 *c;
    int nent = 128;

    for (;;) {
        /* 多分配 KVM_CPUID_SPARE 条但只向 KVM 声明 nent 条，
         * 于是返回后 entries[] 末尾必然还有这么多空位可供追加 */
        c = calloc(1, sizeof(*c) +
                      (nent + KVM_CPUID_SPARE) * sizeof(struct kvm_cpuid_entry2));
        if (!c)
            return VMM_ERR_NOMEM;
        c->nent = nent;

        if (ioctl(kvm_fd, KVM_GET_SUPPORTED_CPUID, c) == 0)
            break;

        free(c);
        if (errno != E2BIG) {
            vmm_err("KVM_GET_SUPPORTED_CPUID: %s", strerror(errno));
            return VMM_ERR_SYS;
        }
        nent *= 2;
        if (nent > 4096)
            return VMM_ERR_NOMEM;
    }

    *out = c;
    return VMM_OK;
}

int kvm_set_cpuid2(int vcpu_fd, struct kvm_cpuid2 *cpuid)
{
    return KVM_IOCTL(vcpu_fd, KVM_SET_CPUID2, cpuid) < 0
           ? VMM_ERR_SYS : VMM_OK;
}

int kvm_get_mp_state(int vcpu_fd, uint32_t *state)
{
    struct kvm_mp_state st;

    memset(&st, 0, sizeof(st));
    if (KVM_IOCTL(vcpu_fd, KVM_GET_MP_STATE, &st) < 0)
        return VMM_ERR_SYS;

    *state = st.mp_state;
    return VMM_OK;
}

const char *kvm_mp_state_str(uint32_t state)
{
    switch (state) {
    case KVM_MP_STATE_RUNNABLE:      return "RUNNABLE";
    case KVM_MP_STATE_UNINITIALIZED: return "UNINITIALIZED";
    case KVM_MP_STATE_INIT_RECEIVED: return "INIT_RECEIVED";
    case KVM_MP_STATE_HALTED:        return "HALTED";
    case KVM_MP_STATE_SIPI_RECEIVED: return "SIPI_RECEIVED";
    default:                         return "?";
    }
}

int kvm_get_msr(int vcpu_fd, uint32_t index, uint64_t *value)
{
    struct {
        struct kvm_msrs hdr;
        struct kvm_msr_entry ent[1];
    } buf;

    memset(&buf, 0, sizeof(buf));
    buf.hdr.nmsrs   = 1;
    buf.ent[0].index = index;

    if (KVM_IOCTL(vcpu_fd, KVM_GET_MSRS, &buf.hdr) < 0)
        return VMM_ERR_SYS;

    *value = buf.ent[0].data;
    return VMM_OK;
}

int kvm_set_msr(int vcpu_fd, uint32_t index, uint64_t value)
{
    struct {
        struct kvm_msrs hdr;
        struct kvm_msr_entry ent[1];
    } buf;

    memset(&buf, 0, sizeof(buf));
    buf.hdr.nmsrs    = 1;
    buf.ent[0].index = index;
    buf.ent[0].data  = value;

    return KVM_IOCTL(vcpu_fd, KVM_SET_MSRS, &buf.hdr) < 0
           ? VMM_ERR_SYS : VMM_OK;
}