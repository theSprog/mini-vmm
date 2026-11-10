/* include/net/tap.h — 打开 TAP 设备，只被 net/netdev.c 使用 */
#ifndef NET_TAP_H
#define NET_TAP_H

/* 以 IFF_TAP | IFF_NO_PI 打开 ifname，fd 为 O_NONBLOCK | O_CLOEXEC。
 * id 只用于错误信息。失败时已打印原因和处理建议。 */
int tap_open(const char *id, const char *ifname, int *fd_out);

#endif /* NET_TAP_H */
