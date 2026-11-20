// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

#include "vector.h"

/* ESP32-P4 rev1.3 TRM ch.33 JPEG, ch.6 2D-DMA, and ch.10 clocks. */
#define JPEG_BASE       0x50086000
#define DMA2D_BASE      0x50088000
#define CLKRST_BASE     0x500e6000
#define SOC_CLK_CTRL1   0x18
#define HP_RST_EN0      0xc0
#define LAB_CLK_MASK    (BIT(6) | BIT(8))
#define LAB_RESET_MASK  (BIT(30) | BIT(31))
#define JPEG_CONFIG     0x00
#define JPEG_DQT_INFO   0x04
#define JPEG_PIC_SIZE   0x08
#define JPEG_QNR0       0x10
#define JPEG_DEC_CONF   0x20
#define JPEG_C0         0x24
#define JPEG_DHT_INFO   0x34
#define JPEG_IRQ_RAW    0x38
#define JPEG_IRQ_ENA    0x3c
#define JPEG_IRQ_ST     0x40
#define JPEG_IRQ_CLR    0x44
#define JPEG_DONE       BIT(0)
#define JPEG_FRAME_EOF_ERR BIT(18)
#define JPEG_SOFT_RST   BIT(24)
#define JPEG_DECODE     BIT(31)
#define JPEG_START      BIT(1)
#define JPEG_QNR_FIFO   BIT(8)
#define JPEG_HUFF_FIFO  BIT(15)

#define DMA2D_RX        0x500
#define D2D_RAW         0x04
#define D2D_ENA         0x08
#define D2D_ST          0x0c
#define D2D_CLR         0x10
#define D2D_LINK        0x1c
#define D2D_ADDR        0x20
#define D2D_STATE       0x24
#define D2D_TX_PERI     0x38
#define D2D_RX_PERI     0x3c
#define D2D_RX_EOF      BIT(1)
#define D2D_RX_ERR      (BIT(2) | BIT(3) | BIT(10))
#define D2D_RESET       BIT(24)
#define D2D_TX_START    BIT(21)
#define D2D_RX_START    BIT(22)
#define D2D_OWNER       BIT(31)
#define D2D_2D          BIT(29)
#define D2D_MULTI       BIT(28)

static unsigned int pair;
module_param(pair, uint, 0444);

struct jpeg_event {
	void __iomem *jpeg;
	struct completion done;
	struct completion dma_done;
	void __iomem *rx;
	u32 seen;
	u32 dma_seen;
};

static irqreturn_t jpeg_irq(int irq, void *arg)
{
	struct jpeg_event *event = arg;
	u32 status = readl(event->jpeg + JPEG_IRQ_ST);

	if (!status)
		return IRQ_NONE;

	event->seen |= status;
	writel(status, event->jpeg + JPEG_IRQ_CLR);
	complete(&event->done);
	return IRQ_HANDLED;
}

static irqreturn_t dma_irq(int irq, void *arg)
{
	struct jpeg_event *event = arg;
	u32 status = readl(event->rx + D2D_ST);

	if (!status)
		return IRQ_NONE;

	event->dma_seen |= status;
	writel(status, event->rx + D2D_CLR);
	complete(&event->dma_done);
	return IRQ_HANDLED;
}

static void jpeg_huff(void __iomem *jpeg, bool ac, const u8 *table, unsigned int size)
{
	u32 count_reg = ac ? 0x60 : 0x58;
	u32 value_reg = count_reg + 4;
	u32 min_reg = ac ? 0x7c : 0x78;
	unsigned int code = 0, total = 0;
	unsigned int i;

	for (i = 0; i < 16; i++) {
		/* TRM 33.9 TOTLEN FIFO, cumulative per jpeg_ll_dht_dc0_write_codeword(). */
		total += table[i];
		writel(total, jpeg + count_reg);
		writel(table[i] ? code << (15 - i) : 0xffff, jpeg + min_reg);
		code = (code + table[i]) << 1;
	}
	for (i = 16; i < size; i++)
		writel(table[i], jpeg + value_reg);
}

static int jpeg_frame(struct jpeg_event *event, void __iomem *dma,
		      void *page, dma_addr_t bus, unsigned int frame, bool bad)
{
	const unsigned int width = frame ? 32 : 16;
	const unsigned int height = frame ? 24 : 16;
	const u8 *scan = frame ? gray_scan1 : gray_scan0;
	const size_t bytes = frame ? sizeof(gray_scan1) : sizeof(gray_scan0);
	void __iomem *jpeg = event->jpeg;
	void __iomem *rx = event->rx;
	__le32 *txd = page, *rxd = page + 32;
	u8 *out = page + 1024;
	u32 config, rx_raw = 0;
	int ret;
	unsigned int i;

	reinit_completion(&event->done);
	reinit_completion(&event->dma_done);
	event->seen = 0;
	event->dma_seen = 0;
	config = readl(jpeg + JPEG_CONFIG);
	writel(config | JPEG_SOFT_RST, jpeg + JPEG_CONFIG);
	config = (config & ~JPEG_SOFT_RST) | JPEG_DECODE |
		 JPEG_QNR_FIFO | JPEG_HUFF_FIFO;
	writel(config, jpeg + JPEG_CONFIG);
	writel(0, jpeg + JPEG_DQT_INFO);
	for (i = 0; i < 64; i++)
		writel(1, jpeg + JPEG_QNR0);
	writel((width + (bad ? 8 : 0)) << 16 | height, jpeg + JPEG_PIC_SIZE);
	writel(1 << 16, jpeg + JPEG_DEC_CONF);
	writel(0x11100, jpeg + JPEG_C0);
	writel(0x1010, jpeg + JPEG_DHT_INFO);
	jpeg_huff(jpeg, false, gray_dc0, sizeof(gray_dc0));
	jpeg_huff(jpeg, true, gray_ac0, sizeof(gray_ac0));

	memcpy(page + 256, scan, bytes);
	memset(out - 32, 0xa5, width * height + 64);
	txd[0] = cpu_to_le32(D2D_OWNER | (bytes << 14) | bytes);
	txd[1] = cpu_to_le32(1 << 28);
	txd[2] = 0;
	txd[3] = cpu_to_le32(bus + 256);
	txd[4] = 0;
	rxd[0] = cpu_to_le32(D2D_OWNER | D2D_2D | (8 << 14) | 8);
	rxd[1] = cpu_to_le32((1 << 28) | (width << 14) | height);
	rxd[2] = cpu_to_le32(D2D_MULTI);
	rxd[3] = cpu_to_le32(bus + 1024);
	rxd[4] = 0;
	dma_wmb();

	writel(D2D_RESET, dma);
	writel(D2D_RESET, rx);
	writel(0, dma);
	writel(0, rx);
	writel(0, dma + D2D_TX_PERI);
	writel(0, rx + D2D_RX_PERI);
	writel(bus, dma + D2D_ADDR);
	writel(bus + 32, rx + D2D_ADDR);
	writel(U32_MAX, dma + D2D_CLR);
	writel(U32_MAX, rx + D2D_CLR);
	writel(U32_MAX, jpeg + JPEG_IRQ_CLR);
	writel(JPEG_DONE | JPEG_FRAME_EOF_ERR, jpeg + JPEG_IRQ_ENA);
	writel(D2D_RX_EOF | D2D_RX_ERR, rx + D2D_ENA);
	writel(D2D_TX_START, dma + D2D_LINK);
	writel(D2D_RX_START, rx + D2D_LINK);
	writel(config | JPEG_START, jpeg + JPEG_CONFIG);

	ret = wait_for_completion_timeout(&event->done, msecs_to_jiffies(50)) ?
		0 : -ETIMEDOUT;
	if (!ret)
		ret = wait_for_completion_timeout(&event->dma_done,
						  msecs_to_jiffies(50)) ?
			0 : -ETIMEDOUT;
	rx_raw = readl(rx + D2D_RAW) | event->dma_seen;
	writel(0, jpeg + JPEG_IRQ_ENA);
	writel(0, rx + D2D_ENA);
	pr_info("p4-jpeg-lab: pair=%u %ux%u bad=%u jpeg_irq=%#x dma_irq=%#x TX=%#x RX=%#x desc=%#x ret=%d\n",
		pair, width, height, bad, event->seen, event->dma_seen,
		readl(dma + D2D_RAW), rx_raw, le32_to_cpu(rxd[0]), ret);
	if (ret)
		return ret;
	if (bad)
		return (event->seen & JPEG_FRAME_EOF_ERR) &&
			(event->dma_seen & D2D_RX_ERR) ? 0 : -EIO;
	if (!(event->seen & JPEG_DONE) || event->seen & JPEG_FRAME_EOF_ERR ||
	    !(event->dma_seen & D2D_RX_EOF) || rx_raw & D2D_RX_ERR)
		return -EIO;

	dma_rmb();
	for (i = 0; i < width * height; i++) {
		u8 sample = 17 + 47 * ((i % width) / 8) +
			    71 * ((i / width) / 8) + 83 * frame;
		if (out[i] != sample) {
			pr_err("p4-jpeg-lab: mismatch at %u, got %u expected %u\n",
			       i, out[i], sample);
			return -EIO;
		}
	}
	for (i = 0; i < 32; i++)
		if ((out - 32)[i] != 0xa5 || out[width * height + i] != 0xa5)
			return -EOVERFLOW;
	pr_info("p4-jpeg-lab: verified %u pixels and 64 guard bytes\n",
		width * height);
	return 0;
}

static void lab_update_bits(void __iomem *reg, u32 mask, u32 value)
{
	writel((readl(reg) & ~mask) | (value & mask), reg);
}

static int __init jpeg_lab_init(void)
{
	struct of_phandle_args args = { .args_count = 2, .args = { 95, 4 } };
	struct jpeg_event event;
	struct device *dev;
	void __iomem *dma, *clock;
	struct device_node *dma_node;
	unsigned int irq = 0;
	int rx_irq;
	void *page = NULL;
	dma_addr_t bus;
	u32 old_clock;
	int ret = -ENOMEM;

	if (!of_machine_is_compatible("waveshare,esp32p4-wifi6-dev-kit-touch-display-2"))
		return -ENODEV;
	if (pair > 1)
		return -EINVAL;
	if (!request_mem_region(DMA2D_BASE, 0x1000, KBUILD_MODNAME))
		return -EBUSY;
	if (!request_mem_region(JPEG_BASE, 0x1000, KBUILD_MODNAME)) {
		ret = -EBUSY;
		goto release_dma;
	}
	dev = bus_find_device_by_name(&platform_bus_type, NULL, "5008a000.dma-controller");
	if (!dev) {
		ret = -ENODEV;
		goto release_jpeg;
	}
	event.jpeg = ioremap(JPEG_BASE, 0x1000);
	dma = ioremap(DMA2D_BASE, 0x1000);
	clock = ioremap(CLKRST_BASE, 0x100);
	page = dma_alloc_coherent(dev, PAGE_SIZE, &bus, GFP_KERNEL);
	if (!event.jpeg || !dma || !clock || !page)
		goto unmap;

	args.np = of_find_node_by_path("/soc/interrupt-controller@20800000");
	if (!args.np) {
		ret = -ENODEV;
		goto unmap;
	}
	irq = irq_create_of_mapping(&args);
	of_node_put(args.np);
	if (!irq) {
		ret = -ENXIO;
		goto unmap;
	}
	init_completion(&event.done);
	init_completion(&event.dma_done);
	event.rx = dma + DMA2D_RX + pair * 0x100;
	ret = request_irq(irq, jpeg_irq, 0, KBUILD_MODNAME, &event);
	if (ret)
		goto dispose;

	dma_node = of_find_node_by_path("/soc/dma-controller@50088000");
	if (!dma_node) {
		ret = -ENODEV;
		goto free_jpeg_irq;
	}
	rx_irq = of_irq_get_byname(dma_node, pair ? "rx1" : "rx0");
	of_node_put(dma_node);
	if (rx_irq <= 0) {
		ret = rx_irq ?: -ENXIO;
		goto free_jpeg_irq;
	}
	ret = request_irq(rx_irq, dma_irq, 0, KBUILD_MODNAME, &event);
	if (ret)
		goto free_jpeg_irq;

	old_clock = readl(clock + SOC_CLK_CTRL1);
	lab_update_bits(clock + SOC_CLK_CTRL1, LAB_CLK_MASK, LAB_CLK_MASK);
	lab_update_bits(clock + HP_RST_EN0, LAB_RESET_MASK, LAB_RESET_MASK);
	lab_update_bits(clock + HP_RST_EN0, LAB_RESET_MASK, 0);
	writel(BIT(2), dma + 0xa04);
	writel(0x4fc00000, dma + 0xa08);
	writel(0x4ffc0000, dma + 0xa0c);
	writel(0x48000000, dma + 0xa10);
	writel(0x4c000000, dma + 0xa14);

	ret = jpeg_frame(&event, dma + pair * 0x100, page, bus, 0, false);
	if (!ret)
		ret = jpeg_frame(&event, dma + pair * 0x100, page, bus, 0, true);
	if (!ret)
		ret = jpeg_frame(&event, dma + pair * 0x100, page, bus, 1, false);

	writel(0, event.jpeg + JPEG_IRQ_ENA);
	writel(0, event.rx + D2D_ENA);
	synchronize_irq(irq);
	synchronize_irq(rx_irq);
	lab_update_bits(clock + HP_RST_EN0, LAB_RESET_MASK, LAB_RESET_MASK);
	lab_update_bits(clock + HP_RST_EN0, LAB_RESET_MASK, 0);
	lab_update_bits(clock + SOC_CLK_CTRL1, LAB_CLK_MASK, old_clock);
	free_irq(rx_irq, &event);
free_jpeg_irq:
	free_irq(irq, &event);
dispose:
	irq_dispose_mapping(irq);
unmap:
	if (page)
		dma_free_coherent(dev, PAGE_SIZE, page, bus);
	if (clock)
		iounmap(clock);
	if (dma)
		iounmap(dma);
	if (event.jpeg)
		iounmap(event.jpeg);
	put_device(dev);
release_jpeg:
	release_mem_region(JPEG_BASE, 0x1000);
release_dma:
	release_mem_region(DMA2D_BASE, 0x1000);
	return ret;
}

static void __exit jpeg_lab_exit(void) {}
module_init(jpeg_lab_init);
module_exit(jpeg_lab_exit);
MODULE_DESCRIPTION("ESP32-P4 JPEG and 2D-DMA board verification");
MODULE_LICENSE("GPL");
