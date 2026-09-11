/* include/devices/rtc.h — CMOS RTC，端口 0x70/0x71 */
#ifndef DEVICES_RTC_H
#define DEVICES_RTC_H

struct vmm_vm;

int rtc_attach(struct vmm_vm *vm);

#endif /* DEVICES_RTC_H */
