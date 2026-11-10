/* src/net/tap.c — 打开 TAP 设备
 *
 * 只负责拿到 fd 和给出可操作的错误提示，收发在 netdev.c。TUNSETIFF 的
 * 语义见 linux drivers/net/tun.c tun_set_iff：
 *   名字不存在   新建一个 TAP，要求调用者在所属 netns 有 CAP_NET_ADMIN；
 *               这样建出来的接口不是 persistent，进程退出就消失
 *   名字已存在   接到这个接口上；预先用 "ip tuntap add ... user <名字>"
 *               建好的接口，属主可以不带特权打开（tun_not_capable）
 *   EBUSY       单队列 TAP 已经被另一个 fd 接着（tun_attach）
 *   EINVAL      同名接口存在但不是 TAP（例如 lo、或者 TUN 模式），
 *               或者多队列属性不一致
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if_tun.h>

#include "vmm.h"
#include "net/tap.h"

/* 接口没 up 时往 TAP 写会得到 EIO（tun_get_user 检查 IFF_UP），guest 发出
 * 的帧全部丢失。这不妨碍启动，接口可以在 VM 运行中再 up，所以只警告。 */
static void warn_if_down(const char *id, const char *ifname)
{
    struct ifreq ifr;
    int s;

    s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        return;
    memset(&ifr, 0, sizeof(ifr));
    memcpy(ifr.ifr_name, ifname, strlen(ifname) + 1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0 && !(ifr.ifr_flags & IFF_UP))
        vmm_warn("netdev %s: TAP %s is down, frames from the guest are "
                 "dropped until 'ip link set %s up'", id, ifname, ifname);
    close(s);
}

int tap_open(const char *id, const char *ifname, int *fd_out)
{
    struct ifreq ifr;
    int fd;

    fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        vmm_err("netdev %s: open /dev/net/tun: %s (is the tun module loaded?)",
                id, strerror(errno));
        return VMM_ERR_SYS;
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    /* 长度在解析阶段已经限制在 IFNAMSIZ - 1 以内 */
    memcpy(ifr.ifr_name, ifname, strlen(ifname) + 1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        int e = errno;

        if (e == EPERM)
            vmm_err("netdev %s: TAP %s: %s; create it beforehand with "
                    "'ip tuntap add dev %s mode tap user $(id -un)'",
                    id, ifname, strerror(e), ifname);
        else if (e == EBUSY)
            vmm_err("netdev %s: TAP %s: %s; another process already has it open",
                    id, ifname, strerror(e));
        else if (e == EINVAL)
            vmm_err("netdev %s: TAP %s: %s; an interface with this name exists "
                    "but is not a single-queue TAP, or the name is invalid",
                    id, ifname, strerror(e));
        else
            vmm_err("netdev %s: TAP %s: TUNSETIFF failed: %s",
                    id, ifname, strerror(e));
        close(fd);
        return VMM_ERR_SYS;
    }

    vmm_info("netdev %s: TAP %s attached", id, ifr.ifr_name);
    warn_if_down(id, ifr.ifr_name);
    *fd_out = fd;
    return VMM_OK;
}
