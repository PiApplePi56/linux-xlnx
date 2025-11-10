// SPDX-License-Identifier: GPL-2.0+
/*
 * FB driver for the ILI9341 LCD display controller
 *
 * This display uses 9-bit SPI: Data/Command bit + 8 data bits
 * For platforms that doesn't support 9-bit, the driver is capable
 * of emulating this using 8-bit transfer.
 * This is done by transferring eight 9-bit words in 9 bytes.
 *
 * Modified to support RGB666 (18-bit) mode with 24-bit RGB888 framebuffer
 * Device Tree property 'bgr' controls hardware BGR mode:
 *   - Set 'bgr' property to enable RGB->BGR conversion in hardware
 *   - Omit 'bgr' property for native RGB mode (no conversion)
 *
 *
 * Copyright (C) 2013 Christian Vogelgsang
 * Based on adafruit22fb.c by Noralf Tronnes
 */

#include <linux/types.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/stddef.h>
#include <linux/wait.h>
#include <video/mipi_display.h>

#include "fbtft.h"

#define DRVNAME		"fb_ili9341"
#define WIDTH		320
#define HEIGHT		240
#define TXBUFLEN	(4 * PAGE_SIZE)
#define ILI9341_DMA_BUF_COUNT	3
#define ILI9341_DMA_BUF_SIZE	(16 * 1024)
#define ILI9341_DMA_TIMEOUT_MS	1000
#define ILI9341_DMA_FLUSH_TIMEOUT_MS	5000
#define DEFAULT_GAMMA	"1F 1A 18 0A 0F 06 45 87 32 0A 07 02 07 05 00\n" \
			"00 25 27 05 10 09 3A 78 4D 05 18 0D 38 3A 1F"

struct ili9341_dma;

struct ili9341_dma_buf {
	struct ili9341_dma *parent;
	void *cpu_addr;
	size_t capacity;
	struct spi_transfer transfer;
	struct spi_message message;
	struct completion completion;
	unsigned int busy;
};

struct ili9341_dma {
	struct fbtft_par *par;
	struct device *dev;
	struct ili9341_dma_buf bufs[ILI9341_DMA_BUF_COUNT];
	unsigned int next;
	size_t chunk_size;
	int pending;
	spinlock_t pending_lock;
	wait_queue_head_t wait;
	struct mutex lock;
};

static void ili9341_spi_complete(void *context)
{
	struct ili9341_dma_buf *buf = context;
	struct ili9341_dma *dma = buf->parent;
	int status = buf->message.status;
	unsigned long flags;

	if (status)
		dev_err(dma->dev, "spi_async transfer failed: %d\n", status);

	buf->busy = 0;
	complete(&buf->completion);

	spin_lock_irqsave(&dma->pending_lock, flags);
	if (dma->pending > 0)
		dma->pending--;
	if (!dma->pending)
		wake_up(&dma->wait);
	spin_unlock_irqrestore(&dma->pending_lock, flags);
}

static struct ili9341_dma_buf *ili9341_dma_acquire(struct ili9341_dma *dma)
{
	for (;;) {
		struct ili9341_dma_buf *buf = &dma->bufs[dma->next];
		unsigned long timeout;

		dma->next = (dma->next + 1) % ARRAY_SIZE(dma->bufs);

		if (!buf->busy)
			return buf;

		timeout = wait_for_completion_timeout(&buf->completion,
				 msecs_to_jiffies(ILI9341_DMA_TIMEOUT_MS));
		if (!timeout) {
			dev_err(dma->dev, "timeout waiting for DMA buffer\n");
			return NULL;
		}
	}
}

static int ili9341_dma_queue(struct ili9341_dma *dma,
			      struct ili9341_dma_buf *buf,
			      const u8 *src, size_t len)
{
	int ret;
	unsigned long flags;

	if (len > buf->capacity)
		return -EINVAL;

	memcpy(buf->cpu_addr, src, len);

	memset(&buf->transfer, 0, sizeof(buf->transfer));
	buf->transfer.tx_buf = buf->cpu_addr;
	buf->transfer.len = len;

	spi_message_init(&buf->message);
	buf->message.complete = ili9341_spi_complete;
	buf->message.context = buf;
	spi_message_add_tail(&buf->transfer, &buf->message);

	reinit_completion(&buf->completion);
	buf->busy = 1;
	spin_lock_irqsave(&dma->pending_lock, flags);
	dma->pending++;
	spin_unlock_irqrestore(&dma->pending_lock, flags);

	ret = spi_async(dma->par->spi, &buf->message);
	if (ret) {
		buf->busy = 0;
		spin_lock_irqsave(&dma->pending_lock, flags);
		if (dma->pending > 0)
			dma->pending--;
		spin_unlock_irqrestore(&dma->pending_lock, flags);
		complete(&buf->completion);
	}

	return ret;
}

static bool ili9341_dma_has_pending(struct ili9341_dma *dma)
{
	unsigned long flags;
	bool pending;

	spin_lock_irqsave(&dma->pending_lock, flags);
	pending = dma->pending != 0;
	spin_unlock_irqrestore(&dma->pending_lock, flags);

	return pending;
}

static int ili9341_dma_wait_idle(struct ili9341_dma *dma)
{
	if (!ili9341_dma_has_pending(dma))
		return 0;

	if (!wait_event_timeout(dma->wait,
			!ili9341_dma_has_pending(dma),
			msecs_to_jiffies(ILI9341_DMA_FLUSH_TIMEOUT_MS))) {
		dev_err(dma->dev, "timeout waiting for SPI queue flush\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static int ili9341_dma_write(struct ili9341_dma *dma, const u8 *vmem,
				 size_t len)
{
	size_t remain = len;
	bool queued = false;
	int ret = 0;

	mutex_lock(&dma->lock);

	while (remain) {
		struct ili9341_dma_buf *buf;
		size_t chunk = min_t(size_t, dma->chunk_size, remain);

		buf = ili9341_dma_acquire(dma);
		if (!buf) {
			ret = -ETIMEDOUT;
			goto out_unlock;
		}

		ret = ili9341_dma_queue(dma, buf, vmem, chunk);
		if (ret)
			goto out_unlock;

		vmem += chunk;
		remain -= chunk;
		queued = true;
	}

out_unlock:
	if (queued) {
		int flush = ili9341_dma_wait_idle(dma);

		if (!ret && flush)
			ret = flush;
	}

	mutex_unlock(&dma->lock);
	return ret;
}

static int ili9341_cpu_write(struct fbtft_par *par, size_t offset,
			       const u8 *vmem, size_t len)
{
	u8 *vmem8;
	u8 *txbuf = par->txbuf.buf;
	size_t remain;
	size_t to_copy;
	size_t tx_array_size;
	int ret = 0;

	fbtft_par_dbg(DEBUG_WRITE_VMEM, par, "%s(offset=%zu, len=%zu)\n",
		      __func__, offset, len);

	remain = len;
	vmem8 = (u8 *)vmem;

	/* non buffered write */
	if (!par->txbuf.buf)
		return par->fbtftops.write(par, (void *)vmem8, len);

	/* buffered write */
	tx_array_size = par->txbuf.len;

	while (remain) {
		to_copy = min(tx_array_size, remain);
		dev_dbg(par->info->device, "to_copy=%zu, remain=%zu\n",
			to_copy, remain - to_copy);

		memcpy(txbuf, vmem8, to_copy);

		vmem8 += to_copy;
		ret = par->fbtftops.write(par, txbuf, to_copy);
		if (ret < 0)
			return ret;
		remain -= to_copy;
	}

	return ret;
}

static int ili9341_dma_setup(struct fbtft_par *par)
{
	struct device *dev = par->info->device;
	struct ili9341_dma *dma;
	size_t base_chunk;
	int i;

	if (!par->spi)
		return -ENODEV;

	if (par->extra)
		return 0;

	dma = devm_kzalloc(dev, sizeof(*dma), GFP_KERNEL);
	if (!dma)
		return -ENOMEM;

	dma->par = par;
	dma->dev = dev;
	dma->next = 0;
	mutex_init(&dma->lock);
	init_waitqueue_head(&dma->wait);
	spin_lock_init(&dma->pending_lock);
	dma->pending = 0;

	base_chunk = par->txbuf.len ? par->txbuf.len : ILI9341_DMA_BUF_SIZE;
	dma->chunk_size = max_t(size_t, base_chunk, ILI9341_DMA_BUF_SIZE);

	for (i = 0; i < ARRAY_SIZE(dma->bufs); i++) {
		struct ili9341_dma_buf *buf = &dma->bufs[i];

		buf->parent = dma;
		buf->capacity = dma->chunk_size;
		buf->cpu_addr = devm_kmalloc(dev, buf->capacity,
					     GFP_KERNEL | GFP_DMA);
		if (!buf->cpu_addr)
			return -ENOMEM;

		init_completion(&buf->completion);
		complete(&buf->completion);
		buf->busy = 0;
	}

	par->extra = dma;
	return 0;
}

static int write_vmem24_bus8(struct fbtft_par *par, size_t offset, size_t len)
{
	struct ili9341_dma *dma = par->extra;
	const u8 *vmem8;
	int ret;

	if (!len)
		return 0;

	vmem8 = (u8 *)(par->info->screen_buffer + offset);

	if (par->gpio.dc)
		gpiod_set_value(par->gpio.dc, 1);

	if (dma && par->spi) {
		ret = ili9341_dma_write(dma, vmem8, len);
		if (!ret)
			return 0;

		if (ret == -ETIMEDOUT)
			return ret;

		dev_warn(par->info->device,
			 "DMA transfer failed (%d), falling back to CPU path\n",
			 ret);
	}

	return ili9341_cpu_write(par, offset, vmem8, len);
}

static int init_display(struct fbtft_par *par)
{
	int ret;

	ret = ili9341_dma_setup(par);
	if (ret)
		dev_warn(par->info->device,
			 "DMA triple-buffering unavailable, falling back: %d\n",
			 ret);

	par->fbtftops.reset(par);

	/* startup sequence for MI0283QT-9A */
	write_reg(par, MIPI_DCS_SOFT_RESET);
	mdelay(5);
	write_reg(par, MIPI_DCS_SET_DISPLAY_OFF);
	/* --------------------------------------------------------- */
	write_reg(par, 0xCF, 0x00, 0x83, 0x30);
	write_reg(par, 0xED, 0x64, 0x03, 0x12, 0x81);
	write_reg(par, 0xE8, 0x85, 0x01, 0x79);
	write_reg(par, 0xCB, 0x39, 0X2C, 0x00, 0x34, 0x02);
	write_reg(par, 0xF7, 0x20);
	write_reg(par, 0xEA, 0x00, 0x00);
	/* ------------power control-------------------------------- */
	write_reg(par, 0xC0, 0x26);
	write_reg(par, 0xC1, 0x11);
	/* ------------VCOM --------- */
	write_reg(par, 0xC5, 0x35, 0x3E);
	write_reg(par, 0xC7, 0xBE);
	/* ------------memory access control------------------------ */
	write_reg(par, MIPI_DCS_SET_PIXEL_FORMAT, 0x66); /* DPI=DBI=6h */
	/* ------------frame rate----------------------------------- */
	write_reg(par, 0xB1, 0x00, 0x1B);
	/* ------------Gamma---------------------------------------- */
	/* write_reg(par, 0xF2, 0x08); */ /* Gamma Function Disable */
	write_reg(par, MIPI_DCS_SET_GAMMA_CURVE, 0x01);
	/* ------------display-------------------------------------- */
	write_reg(par, 0xB7, 0x07); /* entry mode set */
	write_reg(par, 0xB6, 0x0A, 0x82, 0x27, 0x00);
	write_reg(par, MIPI_DCS_EXIT_SLEEP_MODE);
	mdelay(100);
	write_reg(par, MIPI_DCS_SET_DISPLAY_ON);
	mdelay(20);

	return 0;
}

static void set_addr_win(struct fbtft_par *par, int xs, int ys, int xe, int ye)
{
	write_reg(par, MIPI_DCS_SET_COLUMN_ADDRESS,
		  (xs >> 8) & 0xFF, xs & 0xFF, (xe >> 8) & 0xFF, xe & 0xFF);

	write_reg(par, MIPI_DCS_SET_PAGE_ADDRESS,
		  (ys >> 8) & 0xFF, ys & 0xFF, (ye >> 8) & 0xFF, ye & 0xFF);

	write_reg(par, MIPI_DCS_WRITE_MEMORY_START);
}

#define MEM_Y   BIT(7) /* MY row address order */
#define MEM_X   BIT(6) /* MX column address order */
#define MEM_V   BIT(5) /* MV row / column exchange */
#define MEM_L   BIT(4) /* ML vertical refresh order */
#define MEM_BGR BIT(3) /* RGB-BGR Order */
#define MEM_H   BIT(2) /* MH horizontal refresh order */
static int set_var(struct fbtft_par *par)
{
	switch (par->info->var.rotate) {
	case 0:
		write_reg(par, MIPI_DCS_SET_ADDRESS_MODE, MEM_BGR);
		break;
	case 270:
		write_reg(par, MIPI_DCS_SET_ADDRESS_MODE,
			  MEM_V | MEM_X | MEM_L | MEM_BGR);
		break;
	case 180:
		write_reg(par, MIPI_DCS_SET_ADDRESS_MODE,
			  MEM_Y | MEM_X | MEM_BGR);
		break;
	case 90:
		write_reg(par, MIPI_DCS_SET_ADDRESS_MODE,
			  MEM_Y | MEM_V | MEM_BGR);
		break;
	}

	return 0;
}

/*
 * Gamma string format:
 *  Positive: Par1 Par2 [...] Par15
 *  Negative: Par1 Par2 [...] Par15
 */
#define CURVE(num, idx)  curves[(num) * par->gamma.num_values + (idx)]
static int set_gamma(struct fbtft_par *par, u32 *curves)
{
	int i;

	for (i = 0; i < par->gamma.num_curves; i++)
		write_reg(par, 0xE0 + i,
			  CURVE(i, 0), CURVE(i, 1), CURVE(i, 2),
			  CURVE(i, 3), CURVE(i, 4), CURVE(i, 5),
			  CURVE(i, 6), CURVE(i, 7), CURVE(i, 8),
			  CURVE(i, 9), CURVE(i, 10), CURVE(i, 11),
			  CURVE(i, 12), CURVE(i, 13), CURVE(i, 14));

	return 0;
}

#undef CURVE

static struct fbtft_display display = {
	.regwidth = 8,
	.width = WIDTH,
	.height = HEIGHT,
	.txbuflen = TXBUFLEN,
	.gamma_num = 2,
	.gamma_len = 15,
	.gamma = DEFAULT_GAMMA,
	.bpp = 24, /* 24 bpp for RGB888 native support */
	.fbtftops = {
		.init_display = init_display,
		.set_addr_win = set_addr_win,
		.set_var = set_var,
		.set_gamma = set_gamma,
		.write_vmem = write_vmem24_bus8,
	},
};

FBTFT_REGISTER_SPI_DRIVER(DRVNAME, "ilitek", "ili9341", &display);

MODULE_ALIAS("spi:" DRVNAME);
MODULE_ALIAS("platform:" DRVNAME);
MODULE_ALIAS("spi:ili9341");
MODULE_ALIAS("platform:ili9341");

MODULE_DESCRIPTION("FB driver for the ILI9341 LCD display controller");
MODULE_AUTHOR("Christian Vogelgsang");
MODULE_LICENSE("GPL");
