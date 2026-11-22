/* include/devices/rtc.h — CMOS RTC，端口 0x70/0x71 */
#ifndef DEVICES_RTC_H
#define DEVICES_RTC_H

struct vmm_vm;

/* 存世纪数的 CMOS 寄存器。MC146818 本身只有两位年份，世纪放在哪个寄存器
 * 没有统一规定，由 FADT 的 CENTURY 字段告诉 OS，所以 ACPI 表也要用它 */
#define RTC_CENTURY 0x32

int rtc_attach(struct vmm_vm *vm);

#endif /* DEVICES_RTC_H */
