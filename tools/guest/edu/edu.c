// SPDX-License-Identifier: GPL-2.0
/* tools/guest/edu/edu.c — edu 教学 PCI 设备的 Linux 驱动
 *
 * 设备的寄存器、DMA 和中断语义由 edu_regs.h 规定，字符设备接口由
 * edu_ioctl.h 规定。驱动做的是一个 PCI 驱动的完整生命周期：probe 里依次
 * 使能设备、占用 BAR、映射寄存器、设 DMA 掩码、打开 Bus Master、分配一致性
 * DMA 缓冲区、申请中断向量、注册字符设备；remove 里按相反的顺序全部归还。
 * 加载卸载反复做也不该留下任何东西，每一步都有对应的释放。
 *
 * 中断方式由模块参数 irq_mode 选，只申请选中的那一种、失败就让 probe 失败，
 * 而不退回另一种：验收要确认的正是“这一种中断能走通”，悄悄退回会把失败
 * 藏起来。INTx 和 MSI 只有一个向量，负责 INT_STATUS 的全部位；MSI-X 申请
 * 全部 EDU_MSIX_VECTORS 个向量，第 n 个只读、只清 EDU_INT_VEC_MASK(n) 那个
 * 字节，所以每个向量的计数各自独立，可以和设备一侧精确对账。
 *
 * MSI-X 的向量设了 IRQ_DISABLE_UNLAZY：disable_irq 默认是“懒”的，只做记号，
 * 等中断真来了才去屏蔽硬件，那条中断由内核自己记下、enable_irq 时重放，
 * 设备的 Mask 位和 PBA 根本没参与。屏蔽测试要看的正是设备一侧的行为，所以
 * 要 disable_irq 立即写表项的 Mask 位。
 *
 * 一次 DMA：把用户数据拷进一致性缓冲区（或者反过来），填 DMA_ADDR/OFF/LEN，
 * 写 DMA_CMD 启动，然后睡在 completion 上，直到中断处理函数看到
 * INT_STATUS 的 DMA 位并 complete。设备是异步完成的，写完 DMA_CMD 时数据
 * 还没搬，所以必须等。等待有 EDU_WAIT_MS 的上限，只用来兜住设备不再完成
 * 命令的情况，正常路径上醒来靠的是中断。
 *
 * 同一时刻只有一条 DMA 在执行（设备只有一个引擎），用 mutex 串行。
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "edu_regs.h"
#include "edu_ioctl.h"

#define EDU_WAIT_MS  5000

static char *irq_mode = "msi";
module_param(irq_mode, charp, 0444);
MODULE_PARM_DESC(irq_mode, "interrupt type to request: msi (default), msix or intx");

static DEFINE_IDA(edu_ida);

struct edu_dev;

/* 一个中断向量和它负责的 INT_STATUS 位 */
struct edu_vec {
	struct edu_dev    *e;
	int                irq;
	u32                mask;
	bool               requested;
	atomic64_t         calls;        /* 处理函数被调用的次数，含 IRQ_NONE */
	atomic64_t         handled;      /* 其中认领了的次数 */
};

struct edu_dev {
	struct pci_dev    *pdev;
	void __iomem      *regs;
	void __iomem      *pba;          /* MSI-X PBA 的第一个 qword，只在 MSI-X 下映射 */
	void              *dma_buf;      /* 一致性 DMA 缓冲区，EDU_BUF_SIZE 字节 */
	dma_addr_t         dma_handle;
	unsigned int       mode;         /* EDU_MODE_* */
	unsigned int       nvec;
	struct edu_vec     vec[EDU_MSIX_VECTORS];
	int                id;
	char               name[16];
	struct miscdevice  misc;

	struct mutex       lock;         /* 串行 DMA 和 RAISE */
	struct completion  dma_done;
	wait_queue_head_t  soft_wq;
	u32                soft_seen;    /* 中断处理函数看到过的软件中断位 */
	spinlock_t         soft_lock;
};

static irqreturn_t edu_irq(int irq, void *data)
{
	struct edu_vec *v = data;
	struct edu_dev *e = v->e;
	u32 status;

	atomic64_inc(&v->calls);
	status = ioread32(e->regs + EDU_REG_INT_STATUS) & v->mask;
	/* INTx 可能和别的设备共享一根线，不是自己的中断要说 IRQ_NONE；MSI-X 下
	 * 自己那个字节是 0 说明这条消息没有原因 */
	if (!status)
		return IRQ_NONE;
	/* 只清读到的位：处理过程中新到的原因留在 INT_STATUS 里，INTx 下线保持
	 * 有效、MSI 下设备已经为它单独发过消息，都不会丢 */
	iowrite32(status, e->regs + EDU_REG_INT_ACK);
	/* 读回一次，把上面的写推到设备：真实总线上 MMIO 写可以被缓冲（posted），
	 * 不推过去的话 INTx 线在 handler 返回后还高着，会多进一次中断 */
	ioread32(e->regs + EDU_REG_INT_STATUS);

	atomic64_inc(&v->handled);
	if (status & EDU_INT_DMA)
		complete(&e->dma_done);
	if (status & EDU_INT_SOFT_MASK) {
		spin_lock(&e->soft_lock);
		e->soft_seen |= status & EDU_INT_SOFT_MASK;
		spin_unlock(&e->soft_lock);
		wake_up(&e->soft_wq);
	}
	return IRQ_HANDLED;
}

static struct edu_dev *file_edu(struct file *f)
{
	return container_of(f->private_data, struct edu_dev, misc);
}

/* read/write：CPU 经 BAR0 直接访问设备缓冲区，不经过 DMA 引擎 */
static ssize_t edu_read(struct file *f, char __user *ubuf, size_t len, loff_t *ppos)
{
	struct edu_dev *e = file_edu(f);
	void *tmp;
	ssize_t r;

	if (*ppos >= EDU_BUF_SIZE)
		return 0;
	len = min_t(size_t, len, EDU_BUF_SIZE - *ppos);
	tmp = kmalloc(len, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	memcpy_fromio(tmp, e->regs + EDU_BUF_OFFSET + *ppos, len);
	r = copy_to_user(ubuf, tmp, len) ? -EFAULT : (ssize_t)len;
	kfree(tmp);
	if (r > 0)
		*ppos += r;
	return r;
}

static ssize_t edu_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct edu_dev *e = file_edu(f);
	void *tmp;

	if (*ppos >= EDU_BUF_SIZE)
		return len ? -ENOSPC : 0;
	len = min_t(size_t, len, EDU_BUF_SIZE - *ppos);
	tmp = memdup_user(ubuf, len);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);
	memcpy_toio(e->regs + EDU_BUF_OFFSET + *ppos, tmp, len);
	kfree(tmp);
	*ppos += len;
	return len;
}

static loff_t edu_llseek(struct file *f, loff_t off, int whence)
{
	return fixed_size_llseek(f, off, whence, EDU_BUF_SIZE);
}

static long edu_do_dma(struct edu_dev *e, const struct edu_dma *req)
{
	bool to_ram = req->dir == EDU_DMA_DIR_FROM_DEVICE;
	void __user *ubuf = u64_to_user_ptr(req->user_addr);
	u32 cmd = EDU_DMA_CMD_START | EDU_DMA_CMD_IRQ;
	long r;

	if (req->pad || req->dir > EDU_DMA_DIR_FROM_DEVICE)
		return -EINVAL;
	/* 一致性缓冲区只有 EDU_BUF_SIZE；off 不在这里检查，交给设备判定 */
	if (req->len == 0 || req->len > EDU_BUF_SIZE)
		return -EINVAL;
	if (!to_ram && copy_from_user(e->dma_buf, ubuf, req->len))
		return -EFAULT;

	reinit_completion(&e->dma_done);
	iowrite32(lower_32_bits(e->dma_handle), e->regs + EDU_REG_DMA_ADDR_LO);
	iowrite32(upper_32_bits(e->dma_handle), e->regs + EDU_REG_DMA_ADDR_HI);
	iowrite32(req->off, e->regs + EDU_REG_DMA_OFF);
	iowrite32(req->len, e->regs + EDU_REG_DMA_LEN);
	if (to_ram)
		cmd |= EDU_DMA_CMD_TO_RAM;
	iowrite32(cmd, e->regs + EDU_REG_DMA_CMD);

	if (!wait_for_completion_timeout(&e->dma_done, msecs_to_jiffies(EDU_WAIT_MS))) {
		dev_err(&e->pdev->dev, "DMA did not complete: cmd 0x%x status 0x%x\n",
			ioread32(e->regs + EDU_REG_DMA_CMD),
			ioread32(e->regs + EDU_REG_INT_STATUS));
		return -ETIMEDOUT;
	}
	switch (ioread32(e->regs + EDU_REG_DMA_RESULT)) {
	case EDU_DMA_RES_OK:
		break;
	case EDU_DMA_RES_BAD_RANGE:
		return -EINVAL;
	default:
		return -EIO;
	}
	r = 0;
	if (to_ram && copy_to_user(ubuf, e->dma_buf, req->len))
		r = -EFAULT;
	return r;
}

static long edu_do_raise(struct edu_dev *e, u32 bits)
{
	long left;

	if (!bits || (bits & ~EDU_INT_SOFT_MASK))
		return -EINVAL;
	spin_lock_irq(&e->soft_lock);
	e->soft_seen &= ~bits;
	spin_unlock_irq(&e->soft_lock);
	iowrite32(bits, e->regs + EDU_REG_INT_RAISE);
	left = wait_event_timeout(e->soft_wq,
				  (READ_ONCE(e->soft_seen) & bits) == bits,
				  msecs_to_jiffies(EDU_WAIT_MS));
	return left ? 0 : -ETIMEDOUT;
}

static void snap_calls(struct edu_dev *e, __u64 *out)
{
	unsigned int i;

	for (i = 0; i < EDU_MSIX_VECTORS; i++)
		out[i] = i < e->nvec ? atomic64_read(&e->vec[i].calls) : 0;
}

static void sub_calls(__u64 *now, const __u64 *base)
{
	unsigned int i;

	for (i = 0; i < EDU_MSIX_VECTORS; i++)
		now[i] -= base[i];
}

/* 置软件中断位但不等，屏蔽期间用 */
static void raise_nowait(struct edu_dev *e, u32 bits)
{
	spin_lock_irq(&e->soft_lock);
	e->soft_seen &= ~bits;
	spin_unlock_irq(&e->soft_lock);
	iowrite32(bits, e->regs + EDU_REG_INT_RAISE);
}

static long wait_seen(struct edu_dev *e, u32 bits)
{
	return wait_event_timeout(e->soft_wq, (READ_ONCE(e->soft_seen) & bits) == bits,
				  msecs_to_jiffies(EDU_WAIT_MS)) ? 0 : -ETIMEDOUT;
}

static void set_function_mask(struct edu_dev *e, bool on)
{
	int pos = e->pdev->msix_cap;
	u16 ctl;

	pci_read_config_word(e->pdev, pos + PCI_MSIX_FLAGS, &ctl);
	ctl = on ? ctl | PCI_MSIX_FLAGS_MASKALL : ctl & ~PCI_MSIX_FLAGS_MASKALL;
	pci_write_config_word(e->pdev, pos + PCI_MSIX_FLAGS, ctl);
}

/* 见 edu_ioctl.h 的 EDU_MASKTEST_*。中断数用处理函数被调用的次数（含
 * IRQ_NONE）：屏蔽失效、多发或少发的消息都会体现在里面。 */
static long edu_do_masktest(struct edu_dev *e, struct edu_masktest *t)
{
	__u64 base[EDU_MSIX_VECTORS];
	struct edu_vec *v = NULL;
	u32 bit = 0;
	long r = 0;

	if (e->mode != EDU_MODE_MSIX)
		return -EOPNOTSUPP;
	if (t->kind == EDU_MASKTEST_VECTOR || t->kind == EDU_MASKTEST_ACKED) {
		if (t->vector < 1 || t->vector >= e->nvec)
			return -EINVAL;
		v = &e->vec[t->vector];
		bit = 1u << (8 * t->vector);
	} else if (t->kind == EDU_MASKTEST_FUNCTION) {
		bit = 0x01010100;
	} else {
		return -EINVAL;
	}
	t->pba_acked = 0;
	snap_calls(e, base);

	if (v)
		disable_irq(v->irq);
	else
		set_function_mask(e, true);
	raise_nowait(e, bit);
	t->pba_masked = readq(e->pba);
	snap_calls(e, t->calls_masked);
	sub_calls(t->calls_masked, base);

	if (t->kind == EDU_MASKTEST_ACKED) {
		iowrite32(bit, e->regs + EDU_REG_INT_ACK);
		t->pba_acked = readq(e->pba);
		enable_irq(v->irq);
		/* 解除屏蔽后经正常路径再来一次，作为“前面的都已送到”的分界 */
		raise_nowait(e, bit);
	} else if (v) {
		enable_irq(v->irq);
	} else {
		set_function_mask(e, false);
	}
	r = wait_seen(e, bit);
	t->pba_after = readq(e->pba);
	snap_calls(e, t->calls_after);
	sub_calls(t->calls_after, base);
	return r;
}

static long edu_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct edu_dev *e = file_edu(f);
	void __user *uarg = (void __user *)arg;
	struct edu_masktest mt;
	struct edu_info info;
	struct edu_dma req;
	unsigned int i;
	u32 bits;
	long r;

	switch (cmd) {
	case EDU_IOC_DMA:
		if (copy_from_user(&req, uarg, sizeof(req)))
			return -EFAULT;
		mutex_lock(&e->lock);
		r = edu_do_dma(e, &req);
		mutex_unlock(&e->lock);
		return r;
	case EDU_IOC_RAISE:
		if (get_user(bits, (u32 __user *)uarg))
			return -EFAULT;
		mutex_lock(&e->lock);
		r = edu_do_raise(e, bits);
		mutex_unlock(&e->lock);
		return r;
	case EDU_IOC_INFO:
		memset(&info, 0, sizeof(info));
		info.irq_mode   = e->mode;
		info.nvec       = e->nvec;
		for (i = 0; i < e->nvec; i++) {
			info.vec_handled[i] = atomic64_read(&e->vec[i].handled);
			info.vec_calls[i]   = atomic64_read(&e->vec[i].calls);
			info.irq_count     += info.vec_handled[i];
		}
		info.dma_done   = ioread32(e->regs + EDU_REG_DMA_DONE_COUNT);
		info.int_status = ioread32(e->regs + EDU_REG_INT_STATUS);
		return copy_to_user(uarg, &info, sizeof(info)) ? -EFAULT : 0;
	case EDU_IOC_MASKTEST:
		if (copy_from_user(&mt, uarg, sizeof(mt)))
			return -EFAULT;
		mutex_lock(&e->lock);
		r = edu_do_masktest(e, &mt);
		mutex_unlock(&e->lock);
		if (r && r != -ETIMEDOUT)
			return r;
		/* 超时也把读到的事实交回去，用户态据此报告卡在哪一步 */
		if (copy_to_user(uarg, &mt, sizeof(mt)))
			return -EFAULT;
		return r;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations edu_fops = {
	.owner          = THIS_MODULE,
	.read           = edu_read,
	.write          = edu_write,
	.llseek         = edu_llseek,
	.unlocked_ioctl = edu_ioctl,
};

static const char *const mode_name[] = {
	[EDU_MODE_INTX] = "INTx", [EDU_MODE_MSI] = "MSI", [EDU_MODE_MSIX] = "MSI-X",
};

static void edu_free_irqs(struct edu_dev *e)
{
	unsigned int i;

	for (i = 0; i < e->nvec; i++) {
		struct edu_vec *v = &e->vec[i];

		if (!v->requested)
			continue;
		free_irq(v->irq, v);
		if (e->mode == EDU_MODE_MSIX)
			irq_clear_status_flags(v->irq, IRQ_DISABLE_UNLAZY);
		v->requested = false;
	}
}

/* 申请向量并为每个向量注册处理函数。MSI-X 下还映射 PBA：它的位置从
 * capability 的 PBA Offset/BIR 读，不假设设备把它放在哪 */
static int edu_setup_irqs(struct edu_dev *e)
{
	struct pci_dev *pdev = e->pdev;
	unsigned int flags, i;
	u32 pba;
	int r;

	flags = e->mode == EDU_MODE_MSIX ? PCI_IRQ_MSIX :
		e->mode == EDU_MODE_MSI ? PCI_IRQ_MSI : PCI_IRQ_INTX;
	e->nvec = e->mode == EDU_MODE_MSIX ? EDU_MSIX_VECTORS : 1;
	r = pci_alloc_irq_vectors(pdev, e->nvec, e->nvec, flags);
	if (r < 0)
		return r;

	if (e->mode == EDU_MODE_MSIX) {
		pci_read_config_dword(pdev, pdev->msix_cap + PCI_MSIX_PBA, &pba);
		e->pba = pci_iomap_range(pdev, pba & PCI_MSIX_PBA_BIR,
					 pba & PCI_MSIX_PBA_OFFSET, sizeof(u64));
		if (!e->pba) {
			r = -ENOMEM;
			goto err;
		}
	}
	for (i = 0; i < e->nvec; i++) {
		struct edu_vec *v = &e->vec[i];

		v->e    = e;
		v->irq  = pci_irq_vector(pdev, i);
		v->mask = e->nvec == 1 ? ~0u : EDU_INT_VEC_MASK(i);
		atomic64_set(&v->calls, 0);
		atomic64_set(&v->handled, 0);
		if (e->mode == EDU_MODE_MSIX)
			irq_set_status_flags(v->irq, IRQ_DISABLE_UNLAZY);
		r = request_irq(v->irq, edu_irq, e->mode == EDU_MODE_INTX ? IRQF_SHARED : 0,
				"edu", v);
		if (r) {
			if (e->mode == EDU_MODE_MSIX)
				irq_clear_status_flags(v->irq, IRQ_DISABLE_UNLAZY);
			goto err;
		}
		v->requested = true;
	}
	return 0;

err:
	edu_free_irqs(e);
	if (e->pba)
		pci_iounmap(pdev, e->pba);
	e->pba = NULL;
	pci_free_irq_vectors(pdev);
	return r;
}

static void edu_teardown_irqs(struct edu_dev *e)
{
	edu_free_irqs(e);
	if (e->pba)
		pci_iounmap(e->pdev, e->pba);
	e->pba = NULL;
	pci_free_irq_vectors(e->pdev);
}

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	unsigned int mode;
	struct edu_dev *e;
	int r;

	if (!strcmp(irq_mode, "msi"))
		mode = EDU_MODE_MSI;
	else if (!strcmp(irq_mode, "msix"))
		mode = EDU_MODE_MSIX;
	else if (!strcmp(irq_mode, "intx"))
		mode = EDU_MODE_INTX;
	else
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "irq_mode must be msi, msix or intx, got '%s'\n", irq_mode);

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;
	e->pdev = pdev;
	e->mode = mode;
	mutex_init(&e->lock);
	init_completion(&e->dma_done);
	init_waitqueue_head(&e->soft_wq);
	spin_lock_init(&e->soft_lock);
	pci_set_drvdata(pdev, e);

	r = pci_enable_device(pdev);
	if (r)
		goto err_free;
	r = pci_request_regions(pdev, "edu");
	if (r)
		goto err_disable;
	e->regs = pci_iomap(pdev, 0, EDU_BAR0_SIZE);
	if (!e->regs) {
		r = -ENOMEM;
		goto err_regions;
	}
	if (ioread32(e->regs + EDU_REG_ID) != EDU_ID_VALUE) {
		dev_err(&pdev->dev, "unexpected ID register 0x%08x\n",
			ioread32(e->regs + EDU_REG_ID));
		r = -ENODEV;
		goto err_unmap;
	}
	/* DMA_ADDR 有高低两半，设备能访问整个 64 位总线地址空间 */
	r = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (r)
		goto err_unmap;
	pci_set_master(pdev);
	e->dma_buf = dma_alloc_coherent(&pdev->dev, EDU_BUF_SIZE, &e->dma_handle, GFP_KERNEL);
	if (!e->dma_buf) {
		r = -ENOMEM;
		goto err_master;
	}

	r = edu_setup_irqs(e);
	if (r)
		goto err_dma;

	e->id = ida_alloc(&edu_ida, GFP_KERNEL);
	if (e->id < 0) {
		r = e->id;
		goto err_irq;
	}
	snprintf(e->name, sizeof(e->name), "edu%d", e->id);
	e->misc.minor  = MISC_DYNAMIC_MINOR;
	e->misc.name   = e->name;
	e->misc.fops   = &edu_fops;
	e->misc.parent = &pdev->dev;
	r = misc_register(&e->misc);
	if (r)
		goto err_ida;

	if (e->nvec == 1)
		dev_info(&pdev->dev, "/dev/%s, irq %d (%s), BAR0 %pR\n", e->name,
			 e->vec[0].irq, mode_name[e->mode], &pdev->resource[0]);
	else
		dev_info(&pdev->dev, "/dev/%s, irqs %d-%d (%s, %u vectors), BAR0 %pR\n",
			 e->name, e->vec[0].irq, e->vec[e->nvec - 1].irq,
			 mode_name[e->mode], e->nvec, &pdev->resource[0]);
	return 0;

err_ida:
	ida_free(&edu_ida, e->id);
err_irq:
	edu_teardown_irqs(e);
err_dma:
	dma_free_coherent(&pdev->dev, EDU_BUF_SIZE, e->dma_buf, e->dma_handle);
err_master:
	pci_clear_master(pdev);
err_unmap:
	pci_iounmap(pdev, e->regs);
err_regions:
	pci_release_regions(pdev);
err_disable:
	pci_disable_device(pdev);
err_free:
	kfree(e);
	return r;
}

static void edu_remove(struct pci_dev *pdev)
{
	struct edu_dev *e = pci_get_drvdata(pdev);

	misc_deregister(&e->misc);
	ida_free(&edu_ida, e->id);
	edu_teardown_irqs(e);
	dma_free_coherent(&pdev->dev, EDU_BUF_SIZE, e->dma_buf, e->dma_handle);
	pci_clear_master(pdev);
	pci_iounmap(pdev, e->regs);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	kfree(e);
}

static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

static struct pci_driver edu_driver = {
	.name     = "edu",
	.id_table = edu_ids,
	.probe    = edu_probe,
	.remove   = edu_remove,
};
module_pci_driver(edu_driver);

MODULE_DESCRIPTION("mini-vmm edu teaching PCI device");
MODULE_LICENSE("GPL");
