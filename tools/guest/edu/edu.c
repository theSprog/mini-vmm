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
 * 藏起来。
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
MODULE_PARM_DESC(irq_mode, "interrupt type to request: msi (default) or intx");

static DEFINE_IDA(edu_ida);

struct edu_dev {
	struct pci_dev    *pdev;
	void __iomem      *regs;
	void              *dma_buf;      /* 一致性 DMA 缓冲区，EDU_BUF_SIZE 字节 */
	dma_addr_t         dma_handle;
	int                irq;
	bool               msi;
	int                id;
	char               name[16];
	struct miscdevice  misc;

	struct mutex       lock;         /* 串行 DMA 和 RAISE */
	struct completion  dma_done;
	wait_queue_head_t  soft_wq;
	u32                soft_seen;    /* 中断处理函数看到过的软件中断位 */
	spinlock_t         soft_lock;
	atomic64_t         irq_count;
};

static irqreturn_t edu_irq(int irq, void *data)
{
	struct edu_dev *e = data;
	u32 status = ioread32(e->regs + EDU_REG_INT_STATUS);

	/* INTx 可能和别的设备共享一根线，不是自己的中断要说 IRQ_NONE */
	if (!status)
		return IRQ_NONE;
	/* 只清读到的位：处理过程中新到的原因留在 INT_STATUS 里，INTx 下线保持
	 * 有效、MSI 下设备已经为它单独发过消息，都不会丢 */
	iowrite32(status, e->regs + EDU_REG_INT_ACK);
	/* 读回一次，把上面的写推到设备：真实总线上 MMIO 写可以被缓冲（posted），
	 * 不推过去的话 INTx 线在 handler 返回后还高着，会多进一次中断 */
	ioread32(e->regs + EDU_REG_INT_STATUS);

	atomic64_inc(&e->irq_count);
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

static long edu_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct edu_dev *e = file_edu(f);
	void __user *uarg = (void __user *)arg;
	struct edu_info info;
	struct edu_dma req;
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
		info.irq_mode   = e->msi;
		info.irq_count  = atomic64_read(&e->irq_count);
		info.dma_done   = ioread32(e->regs + EDU_REG_DMA_DONE_COUNT);
		info.int_status = ioread32(e->regs + EDU_REG_INT_STATUS);
		return copy_to_user(uarg, &info, sizeof(info)) ? -EFAULT : 0;
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

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	unsigned int flags;
	struct edu_dev *e;
	int r;

	if (!strcmp(irq_mode, "msi"))
		flags = PCI_IRQ_MSI;
	else if (!strcmp(irq_mode, "intx"))
		flags = PCI_IRQ_INTX;
	else
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "irq_mode must be msi or intx, got '%s'\n", irq_mode);

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;
	e->pdev = pdev;
	e->msi = flags == PCI_IRQ_MSI;
	mutex_init(&e->lock);
	init_completion(&e->dma_done);
	init_waitqueue_head(&e->soft_wq);
	spin_lock_init(&e->soft_lock);
	atomic64_set(&e->irq_count, 0);
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

	r = pci_alloc_irq_vectors(pdev, 1, 1, flags);
	if (r < 0)
		goto err_dma;
	e->irq = pci_irq_vector(pdev, 0);
	r = request_irq(e->irq, edu_irq, e->msi ? 0 : IRQF_SHARED, "edu", e);
	if (r)
		goto err_vectors;

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

	dev_info(&pdev->dev, "/dev/%s, irq %d (%s), BAR0 %pR\n", e->name, e->irq,
		 e->msi ? "MSI" : "INTx", &pdev->resource[0]);
	return 0;

err_ida:
	ida_free(&edu_ida, e->id);
err_irq:
	free_irq(e->irq, e);
err_vectors:
	pci_free_irq_vectors(pdev);
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
	free_irq(e->irq, e);
	pci_free_irq_vectors(pdev);
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
