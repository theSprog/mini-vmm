/* src/devices/i8042.c — i8042 键盘控制器 + 一个只会应答命令的 AT 键盘
 *
 * 为什么不能只做个占位：Linux 的 i8042 驱动探测时要读 CTR（命令 0x20），
 * 等 OBF 置位等不到就忙等 0.5 秒再报 "Can't read CTR"——i8042_wait_read()
 * 转 I8042_CTL_TIMEOUT（10000）圈，每圈 udelay(50)。探测路径上有十几条
 * 命令，任何一条没应答都要白等这半秒，所以控制器命令都得有应答。
 *
 * 端口：
 *   0x60  R: 输出缓冲（控制器应答 / 键盘数据）  W: 参数或发给键盘的字节
 *   0x64  R: 状态寄存器                          W: 控制器命令
 *
 * 设计取舍：
 *   - KBD 口挂一个键盘：只应答命令（ACK/ID/自检），没有 host 按键输入。
 *     这样 atkbd 驱动能正常注册，不会因为探测超时拖慢启动。
 *   - AUX 口（PS/2 鼠标）报告不存在。驱动先发 AUX_LOOP(0xD3)，我们回
 *     数据但不置 AUXDATA 位，驱动当场判定回环失败（i8042.c
 *     __i8042_command 里的 AUXDATA 检查），接着 AUX_TEST(0xA9) 回 0x01
 *     "时钟线卡死"，驱动就放弃 AUX 口。整个过程没有一次超时。
 *   - 键盘数据经 IRQ 1 送出：PS/2 设备的应答走中断路径（serio -> libps2），
 *     不打中断的话 atkbd 会等到超时。控制器自身的应答不打中断，
 *     驱动是关着中断轮询读的。
 *   - reset：命令 0xFE（脉冲 reset 线）或写输出端口 bit0=0，都让 VMM 退出。
 */
#include <string.h>
#include <stdint.h>

#include "vmm.h"
#include "devices/i8042.h"
#include "devices/irqchip.h"

#define I8042_DATA_PORT 0x60
#define I8042_CMD_PORT  0x64
#define I8042_KBD_IRQ   1

/* 状态寄存器 */
#define STR_OBF      0x01
#define STR_SYSFLAG  0x04   /* 自检通过后为 1 */
#define STR_CMDDAT   0x08   /* 最后一次写的是命令口 */
#define STR_KEYLOCK  0x10   /* 1 = 键盘未被锁 */
#define STR_AUXDATA  0x20

/* CTR（控制器命令字节） */
#define CTR_KBDINT   0x01
#define CTR_AUXINT   0x02
#define CTR_SYSFLAG  0x04
#define CTR_KBDDIS   0x10
#define CTR_AUXDIS   0x20
#define CTR_XLATE    0x40

/* 控制器命令 */
#define CMD_READ_CTR    0x20
#define CMD_WRITE_CTR   0x60
#define CMD_AUX_DISABLE 0xa7
#define CMD_AUX_ENABLE  0xa8
#define CMD_AUX_TEST    0xa9
#define CMD_SELF_TEST   0xaa
#define CMD_KBD_TEST    0xab
#define CMD_KBD_DISABLE 0xad
#define CMD_KBD_ENABLE  0xae
#define CMD_READ_OUTP   0xd0
#define CMD_WRITE_OUTP  0xd1
#define CMD_WRITE_KBD_OBUF 0xd2
#define CMD_WRITE_AUX_OBUF 0xd3
#define CMD_WRITE_AUX   0xd4
#define CMD_PULSE_BASE  0xf0   /* 0xf0-0xff：低 4 位为 0 的线被脉冲拉低 */

#define SELF_TEST_OK    0x55
#define AUX_TEST_CLK_STUCK 0x01
#define OUTP_RESET_N    0x01   /* 输出端口 bit0：CPU reset 线，低有效 */
#define OUTP_A20        0x02

/* 键盘命令与应答 */
#define KBD_CMD_SETLEDS 0xed
#define KBD_CMD_ECHO    0xee
#define KBD_CMD_SSCANSET 0xf0
#define KBD_CMD_GETID   0xf2
#define KBD_CMD_SETREP  0xf3
#define KBD_CMD_RESET   0xff
#define KBD_ACK         0xfa
#define KBD_BAT_OK      0xaa
#define KBD_ID0         0xab
#define KBD_ID1         0x83

#define OBUF_SIZE 16

struct obuf_ent {
    uint8_t val;
    bool    aux;       /* 置 STR_AUXDATA */
    bool    from_kbd;  /* 键盘设备发出的字节，需要打 IRQ 1 */
};

static struct {
    struct vmm_vm *vm;
    uint8_t ctr;
    uint8_t outp;
    uint8_t status_cmd;     /* STR_CMDDAT */

    /* 写 0x64 的某些命令后，下一次写 0x60 是它的参数 */
    uint8_t pending_cmd;
    /* 发给键盘的某些命令带一个参数字节 */
    uint8_t kbd_pending;

    struct obuf_ent obuf[OBUF_SIZE];
    int head, count;
    int irq_level;
} g_kbc;

static void set_irq(int level)
{
    if (level != g_kbc.irq_level) {
        g_kbc.irq_level = level;
        irqchip_set_irq(g_kbc.vm, I8042_KBD_IRQ, level);
    }
}

/* IOAPIC 上 ISA IRQ 是边沿触发，每个字节都要一个新的上升沿 */
static void update_irq(void)
{
    const struct obuf_ent *e = &g_kbc.obuf[g_kbc.head];

    set_irq(g_kbc.count && e->from_kbd && (g_kbc.ctr & CTR_KBDINT) &&
            !(g_kbc.ctr & CTR_KBDDIS));
}

static void obuf_push(uint8_t val, bool aux, bool from_kbd)
{
    struct obuf_ent *e;

    if (g_kbc.count == OBUF_SIZE)
        return;
    e = &g_kbc.obuf[(g_kbc.head + g_kbc.count) % OBUF_SIZE];
    e->val = val;
    e->aux = aux;
    e->from_kbd = from_kbd;
    g_kbc.count++;
    update_irq();
}

static uint8_t obuf_pop(void)
{
    uint8_t v;

    if (!g_kbc.count)
        return 0;
    v = g_kbc.obuf[g_kbc.head].val;
    g_kbc.head = (g_kbc.head + 1) % OBUF_SIZE;
    g_kbc.count--;
    set_irq(0);
    update_irq();
    return v;
}

static void kbd_reply(uint8_t v)
{
    obuf_push(v, false, true);
}

/* 主机经 0x60 发给键盘设备的字节 */
static void kbd_write(uint8_t v)
{
    if (g_kbc.kbd_pending) {
        uint8_t cmd = g_kbc.kbd_pending;

        g_kbc.kbd_pending = 0;
        kbd_reply(KBD_ACK);
        /* "F0 00" 是查询当前扫描码集，报告 set 2 */
        if (cmd == KBD_CMD_SSCANSET && v == 0)
            kbd_reply(0x02);
        return;
    }

    switch (v) {
    case KBD_CMD_RESET:
        kbd_reply(KBD_ACK);
        kbd_reply(KBD_BAT_OK);
        break;
    case KBD_CMD_GETID:
        kbd_reply(KBD_ACK);
        kbd_reply(KBD_ID0);
        kbd_reply(KBD_ID1);
        break;
    case KBD_CMD_ECHO:
        kbd_reply(KBD_CMD_ECHO);
        break;
    case KBD_CMD_SETLEDS:
    case KBD_CMD_SETREP:
    case KBD_CMD_SSCANSET:
        g_kbc.kbd_pending = v;
        kbd_reply(KBD_ACK);
        break;
    default:
        /* F4 enable / F5 disable / F6 default 等，一律 ACK */
        kbd_reply(KBD_ACK);
        break;
    }
}

static int request_reset(void)
{
    vmm_info("guest requested reset via i8042");
    g_kbc.vm->exit_code = 0;
    return VMM_ERR_EXIT;
}

static int kbc_command(uint8_t cmd)
{
    /* 新命令会冲掉还没被读走的旧应答。否则一次失败的 AUX_LOOP 残留
     * 的字节会被下一条 AUX_TEST 当成自己的应答读走。 */
    g_kbc.count = 0;
    set_irq(0);

    switch (cmd) {
    case CMD_READ_CTR:
        obuf_push(g_kbc.ctr, false, false);
        break;
    case CMD_WRITE_CTR:
    case CMD_WRITE_OUTP:
    case CMD_WRITE_KBD_OBUF:
    case CMD_WRITE_AUX_OBUF:
    case CMD_WRITE_AUX:
        g_kbc.pending_cmd = cmd;
        break;
    case CMD_AUX_DISABLE:
        g_kbc.ctr |= CTR_AUXDIS;
        break;
    case CMD_AUX_ENABLE:
        g_kbc.ctr &= (uint8_t)~CTR_AUXDIS;
        break;
    case CMD_AUX_TEST:
        obuf_push(AUX_TEST_CLK_STUCK, false, false);
        break;
    case CMD_SELF_TEST:
        obuf_push(SELF_TEST_OK, false, false);
        break;
    case CMD_KBD_TEST:
        obuf_push(0x00, false, false);
        break;
    case CMD_KBD_DISABLE:
        g_kbc.ctr |= CTR_KBDDIS;
        break;
    case CMD_KBD_ENABLE:
        g_kbc.ctr &= (uint8_t)~CTR_KBDDIS;
        update_irq();
        break;
    case CMD_READ_OUTP:
        obuf_push(g_kbc.outp, false, false);
        break;
    default:
        if (cmd >= CMD_PULSE_BASE && !(cmd & OUTP_RESET_N))
            return request_reset();
        /* 其余命令（0xc0 读输入端口等）Linux 不会用到，忽略 */
        break;
    }
    return VMM_OK;
}

static int kbc_data_write(uint8_t v)
{
    uint8_t cmd = g_kbc.pending_cmd;

    g_kbc.pending_cmd = 0;
    switch (cmd) {
    case CMD_WRITE_CTR:
        g_kbc.ctr = v;
        update_irq();
        break;
    case CMD_WRITE_OUTP:
        g_kbc.outp = v;
        if (!(v & OUTP_RESET_N))
            return request_reset();
        break;
    case CMD_WRITE_KBD_OBUF:
        obuf_push(v, false, true);
        break;
    case CMD_WRITE_AUX_OBUF:
        /* 故意不置 AUXDATA：让驱动判定 AUX 回环失败，见文件头 */
        obuf_push(v, false, false);
        break;
    case CMD_WRITE_AUX:
        /* 没有 AUX 设备，字节丢弃 */
        break;
    default:
        kbd_write(v);
        break;
    }
    return VMM_OK;
}

/* opaque 里放的是端口号本身，用来区分 0x60 和 0x64 */
#define PORT_OF(opaque) ((uint16_t)(uintptr_t)(opaque))

static int i8042_read(void *opaque, uint64_t off, uint32_t size, void *data)
{
    uint8_t v;

    (void)off;
    if (PORT_OF(opaque) == I8042_DATA_PORT) {
        v = obuf_pop();
    } else {
        v = STR_SYSFLAG | STR_KEYLOCK | g_kbc.status_cmd;
        if (g_kbc.count) {
            v |= STR_OBF;
            if (g_kbc.obuf[g_kbc.head].aux)
                v |= STR_AUXDATA;
        }
    }
    memset(data, 0, size);
    *(uint8_t *)data = v;
    return VMM_OK;
}

static int i8042_write(void *opaque, uint64_t off, uint32_t size,
                       const void *data)
{
    uint8_t v = *(const uint8_t *)data;

    (void)off; (void)size;
    if (PORT_OF(opaque) == I8042_DATA_PORT) {
        g_kbc.status_cmd = 0;
        return kbc_data_write(v);
    }
    g_kbc.status_cmd = STR_CMDDAT;
    return kbc_command(v);
}

int i8042_attach(struct vmm_vm *vm)
{
    struct vmm_io_dev dev;
    int r;

    memset(&g_kbc, 0, sizeof(g_kbc));
    g_kbc.vm   = vm;
    /* BIOS 交接时的典型值：键盘中断开、扫描码翻译开、AUX 关 */
    g_kbc.ctr  = CTR_KBDINT | CTR_SYSFLAG | CTR_AUXDIS | CTR_XLATE;
    g_kbc.outp = OUTP_RESET_N | OUTP_A20;

    /* 0x60 和 0x64 中间隔着 0x61（PIT gate，由 KVM 内核处理），
     * 所以分两个单字节设备注册，不能占 [0x60, 0x65) 整段。 */
    memset(&dev, 0, sizeof(dev));
    dev.name   = "i8042-data";
    dev.base   = I8042_DATA_PORT;
    dev.len    = 1;
    dev.opaque = (void *)(uintptr_t)I8042_DATA_PORT;
    dev.read   = i8042_read;
    dev.write  = i8042_write;
    r = vmm_register_pio(vm, &dev);
    if (r != VMM_OK)
        return r;

    dev.name   = "i8042-cmd";
    dev.base   = I8042_CMD_PORT;
    dev.opaque = (void *)(uintptr_t)I8042_CMD_PORT;
    r = vmm_register_pio(vm, &dev);
    return r;
}
