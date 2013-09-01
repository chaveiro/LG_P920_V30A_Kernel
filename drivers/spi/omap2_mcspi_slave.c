/* 
 * OMAP2 McSPI controller driver
 *
 * Copyright (C) 2005, 2006 Nokia Corporation
 * Author:	Samuel Ortiz <samuel.ortiz@nokia.com> and
 *		Juha Yrj??<juha.yrjola@nokia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>

#include <linux/spi/spi.h>

#include <plat/dma.h>
#include <plat/clock.h>
#include <plat/mcspi.h>

//20110327 ws.yang@lge.com .. to max of vdd2_opp when dma is tx/rx /* 20110720 dongyu.gwak@lge.com L3 200Mhz from Justin */
#include <plat/omap-pm.h>
#define OMAP2_MCSPI_MAX_FREQ		48000000
#define OMAP2_MCSPI_MAX_FIFODEPTH       64

/* OMAP2 has 3 SPI controllers, while OMAP3/4 has 4 */
#define OMAP2_MCSPI_MAX_CTRL 		4

/* per-register bitmasks: */

#define OMAP2_MCSPI_MODULCTRL_SINGLE	BIT(0)
#define OMAP2_MCSPI_MODULCTRL_MS	BIT(2)
#define OMAP2_MCSPI_MODULCTRL_STEST	BIT(3)

#define OMAP2_MCSPI_CHCONF_PHA		BIT(0)
#define OMAP2_MCSPI_CHCONF_POL		BIT(1)
#define OMAP2_MCSPI_CHCONF_CLKD_MASK	(0x0f << 2)
#define OMAP2_MCSPI_CHCONF_EPOL		BIT(6)
#define OMAP2_MCSPI_CHCONF_WL_MASK	(0x1f << 7)
#define OMAP2_MCSPI_CHCONF_TRM_RX_ONLY	BIT(12)
#define OMAP2_MCSPI_CHCONF_TRM_TX_ONLY	BIT(13)
#define OMAP2_MCSPI_CHCONF_TRM_MASK	(0x03 << 12)
#define OMAP2_MCSPI_CHCONF_DMAW		BIT(14)
#define OMAP2_MCSPI_CHCONF_DMAR		BIT(15)
#define OMAP2_MCSPI_CHCONF_DPE0		BIT(16)
#define OMAP2_MCSPI_CHCONF_DPE1		BIT(17)
#define OMAP2_MCSPI_CHCONF_IS		BIT(18)
#define OMAP2_MCSPI_CHCONF_TURBO	BIT(19)
#define OMAP2_MCSPI_CHCONF_FORCE	BIT(20)
#define OMAP2_MCSPI_CHCONF_FFET	BIT(27)
#define OMAP2_MCSPI_CHCONF_FFER	BIT(28)

#define OMAP2_MCSPI_CHSTAT_RXS		BIT(0)
#define OMAP2_MCSPI_CHSTAT_TXS		BIT(1)
#define OMAP2_MCSPI_CHSTAT_EOT		BIT(2)

#define OMAP2_MCSPI_IRQ_EOW		BIT(17)

#define OMAP2_MCSPI_CHCTRL_EN		BIT(0)

#define OMAP2_MCSPI_WAKEUPENABLE_WKEN	BIT(0)

#define SDMA_BASE_ADDRESS_OMAP4430	(0x4A056000)

#define DMA4_IRQSTATUS_OFFSET		(0x08)	/* RW 0x4A056008 + (0x4 * j)	The interrupt status register */
#define DMA4_IRQENABLE_OFFSET		(0x18)	/* RW 0x4A056018 + (0x4 * j)	The interrupt enable register */
#define DMA4_SYSSTATUS_OFFSET		(0x28)	/* R 	The status information about the module excluding IRQSTATUS */
#define DMA4_OCP_SYSCONF_OFFSET		(0x2C)	/* RW	*/
#define DMA4_CAPS_0_OFFSET			(0x64)	/* RW	*/
#define DMA4_CAPS_2_OFFSET			(0x6C)	/* R	*/
#define DMA4_CAPS_3_OFFSET			(0x70)	/* R	*/
#define DMA4_CAPS_4_OFFSET			(0x74)	/* RW	*/
#define DMA4_GCR_OFFSET				(0x78)	/* RW	*/
#define DMA4_CCR_OFFSET				(0x80)	/* RW 0x4A056080 + (0x60 * i)	*/
#define DMA4_CLNK_CTRL_OFFSET		(0x84)	/* RW 0x4A056084 + (0x60 * i)	*/
#define DMA4_CICR_OFFSET			(0x88)	/* RW 0x4A056088 + (0x60 * i)	*/
#define DMA4_CSR_OFFSET				(0x8C)	/* RW 0x4A05608C + (0x60 * i)	*/
#define DMA4_CSDP_OFFSET			(0x90)	/* RW 0x4A056090 + (0x60 * i)	*/
#define DMA4_CEN_OFFSET				(0x94)	/* RW 0x4A056094 + (0x60 * i)	*/
#define DMA4_CFN_OFFSET				(0x98)	/* RW 0x4A056098 + (0x60 * i)	*/
#define DMA4_CSSA_OFFSET			(0x9C)	/* RW 0x4A05609C + (0x60 * i)	*/
#define DMA4_CDSA_OFFSET			(0xA0)	/* RW 0x4A0560A0 + (0x60 * i)	*/
#define DMA4_CSEI_OFFSET			(0xA4)	/* RW 0x4A0560A4 + (0x60 * i)	*/
#define DMA4_CSFI_OFFSET			(0xA8)	/* RW 0x4A0560A8 + (0x60 * i)	*/
#define DMA4_CDEI_OFFSET			(0xAC)	/* RW 0x4A0560AC + (0x60 * i)	*/
#define DMA4_CDFI_OFFSET			(0xB0)	/* RW 0x4A0560B0 + (0x60 * i)	*/
#define DMA4_CSAC_OFFSET			(0xB4) 	/* R  0x4A0560B4 + (0x60 * i) 	*/
#define DMA4_CDAC_OFFSET			(0xB8)	/* RW 0x4A0560B8 + (0x60 * i)	*/

#define DMA4_CDAC_OFFSET_OMAP4430	(0xB8)	
#define DMA4_CCEN_OFFSET_OMAP4430	(0xBC)
#define DMA4_CCFN_OFFSET_OMAP4430	(0xC0)
#define DMA4_COLOR_OFFSET			(0xC4)	/* RW 0x4A0560C4 + (0x60 * i)	*/


#define	DMA4_IRQSTATUS_ADDR		( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_IRQSTATUS_OFFSET )
#define	DMA4_IRQENABLE_ADDR		( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_IRQENABLE_OFFSET)
#define	DMA4_SYSSTATUS_ADDR		( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_SYSSTATUS_OFFSET)
#define	DMA4_OCP_SYSCONF_ADDR	( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_OCP_SYSCONF_OFFSET)
#define	DMA4_CAPS_0_ADDR		( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CAPS_0_OFFSET)
#define	DMA4_CAPS_2_ADDR		( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CAPS_2_OFFSET)
#define	DMA4_CAPS_3_ADDR		( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CAPS_3_OFFSET)
#define	DMA4_CAPS_4_ADDR		( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CAPS_4_OFFSET)
#define	DMA4_GCR_ADDR			( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_GCR_OFFSET)
#define	DMA4_CCR_ADDR			( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CCR_OFFSET)
#define	DMA4_CLNK_CTRL_ADDR		( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CLNK_CTRL_OFFSET)
#define	DMA4_CICR_ADDR			( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CICR_OFFSET)
#define	DMA4_CSR_ADDR			( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CSR_OFFSET)
#define	DMA4_CSDP_ADDR			( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CSDP_OFFSET)
#define	DMA4_CEN_ADDR			( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CEN_OFFSET)
#define	DMA4_CFN_ADDR			( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CFN_OFFSET)
#define	DMA4_CSSA_ADDR			( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CSSA_OFFSET)
#define	DMA4_CDSA_ADDR			( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CDSA_OFFSET)
#define	DMA4_CSEI_ADDR			( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CSEI_OFFSET)
#define	DMA4_CSFI_ADDR			( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CSFI_OFFSET)
#define	DMA4_CDEI_ADDR			( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CDEI_OFFSET)
#define	DMA4_CDFI_ADDR			( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CDFI_OFFSET)
#define DMA4_CSAC_ADDR			( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CSAC_OFFSET)
#define	DMA4_CDAC_ADDR			( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CDAC_OFFSET)

#define DMA4_CDAC_ADDRESS	( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CDAC_OFFSET_OMAP4430 )
#define DMA4_CCEN_ADDRESS	( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CCEN_OFFSET_OMAP4430 )
#define DMA4_CCFN_ADDRESS	( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_CCFN_OFFSET_OMAP4430 )
#define DMA4_COLOR_ADDR			( SDMA_BASE_ADDRESS_OMAP4430 + DMA4_COLOR_OFFSET)

/* We have 2 DMA channels per CS, one for RX and one for TX */
struct omap2_mcspi_dma {
	int dma_tx_channel;
	int dma_rx_channel;

	int dma_tx_sync_dev;
	int dma_rx_sync_dev;

	struct completion dma_tx_completion;
	struct completion dma_rx_completion;
};

/* use PIO for small transfers, avoiding DMA setup/teardown overhead and
 * cache operations; better heuristics consider wordsize and bitrate.
 */
#define DMA_MIN_BYTES			160
#define DMA_TIMEOUT_LIMIT		((3)*(HZ)) //100->300 for delay	in first booting [hyunh0.cho@lge.com]
// LGE_UPDATE_S eungbo.shim@lge.com [EBS] For DMA FIFO 
#define LGE_RIL_SPI
// LGE_UPDATE_E eungbo.shim@lge.com [EBS]
#define SPI_RETRY_ENABLE

struct omap2_mcspi {
	struct work_struct	work;
	/* lock protects queue and registers */
	spinlock_t		lock;
	struct list_head	msg_queue;
	struct spi_master	*master;
	/* Virtual base address of the controller */
	void __iomem		*base;
	u16			*regs;
	unsigned long		phys;
	/* SPI1 has 4 channels, while SPI2 has 2 */
	struct omap2_mcspi_dma	*dma_channels;
	u8			mcspi_mode;
	u8			dma_mode;
	u8			force_cs_mode;
	u16			fifo_depth;
	struct  device          *dev;
// LGE_UPDATE_S eungbo.shim@lge.com [EBS]
#ifdef CONFIG_LGE_SPI
	unsigned char		name[20];
	struct workqueue_struct *wq;
#endif
// LGE_UPDATE_E eungbo.shim@lge.com [EBS]
};

struct omap2_mcspi_cs {
	void __iomem		*base;
	unsigned long		phys;
	int			word_len;
	struct list_head	node;
	/* Context save and restore shadow register */
	u32			chconf0;
};

/* used for context save and restore, structure members to be updated whenever
 * corresponding registers are modified.
 */
struct omap2_mcspi_regs {
	u32 modulctrl;
	u32 wakeupenable;
	struct list_head cs;
};

static struct omap2_mcspi_regs omap2_mcspi_ctx[OMAP2_MCSPI_MAX_CTRL];
static struct pm_qos_request_list pm_qos_handle_for_spi;
#ifdef CONFIG_SPI_DEBUG
struct reg_type {
	char name[40];
	int offset;
};

static struct reg_type reg_map[] = {
	{"MCSPI_REV", 0x0},
	{"MCSPI_SYSCONFIG", 0x10},
	{"MCSPI_SYSSTATUS", 0x14},
	{"MCSPI_IRQSTATUS", 0x18},
	{"MCSPI_IRQENABLE", 0x1C},
	{"MCSPI_WAKEUPENABLE", 0x20},
	{"MCSPI_SYST", 0x24},
	{"MCSPI_MODULCTRL", 0x28},
	{"MCSPI_XFERLEVEL", 0x7c},
	{"CH0", 0x2C},
	{"CH1", 0x40},
	{"CH2", 0x54},
	{"CH3", 0x68}
};

static struct reg_type ch_reg_type[] = {
	{"CONF", 0x00},
	{"STAT", 0x04},
	{"CTRL", 0x08},
	{"TX", 0x0C},
	{"RX", 0x10},
};
#endif

#ifdef CONFIG_LGE_SPI
#else
static struct workqueue_struct *omap2_mcspi_wq;
#endif

#define MOD_REG_BIT(val, mask, set) do { \
	if (set) \
		val |= mask; \
	else \
		val &= ~mask; \
} while (0)

static inline void mcspi_write_reg(struct spi_master *master,
		int idx, u32 val)
{
	struct omap2_mcspi *mcspi = spi_master_get_devdata(master);

	__raw_writel(val, mcspi->base + mcspi->regs[idx]);
}

static inline u32 mcspi_read_reg(struct spi_master *master, int idx)
{
	struct omap2_mcspi *mcspi = spi_master_get_devdata(master);

	return __raw_readl(mcspi->base + mcspi->regs[idx]);
}

static inline void mcspi_write_cs_reg(const struct spi_device *spi,
		int idx, u32 val)
{
	struct omap2_mcspi_cs	*cs = spi->controller_state;
	struct spi_master *master = spi->master;
	struct omap2_mcspi *mcspi = spi_master_get_devdata(master);

	__raw_writel(val, cs->base + mcspi->regs[idx]);
}

static inline u32 mcspi_read_cs_reg(const struct spi_device *spi, int idx)
{
	struct omap2_mcspi_cs	*cs = spi->controller_state;
	struct spi_master *master = spi->master;
	struct omap2_mcspi *mcspi = spi_master_get_devdata(master);

	return __raw_readl(cs->base + mcspi->regs[idx]);
}

static inline u32 mcspi_cached_chconf0(const struct spi_device *spi)
{
	struct omap2_mcspi_cs *cs = spi->controller_state;

	return cs->chconf0;
}

static inline void mcspi_write_chconf0(const struct spi_device *spi, u32 val)
{
	struct omap2_mcspi_cs *cs = spi->controller_state;

	cs->chconf0 = val;
	mcspi_write_cs_reg(spi, OMAP2_MCSPI_CHCONF0, val);
	mcspi_read_cs_reg(spi, OMAP2_MCSPI_CHCONF0);
}

static void omap2_mcspi_set_dma_req(const struct spi_device *spi,
		int is_read, int enable)
{
	u32 l, rw;

	l = mcspi_cached_chconf0(spi);

	if (is_read) /* 1 is read, 0 write */
		rw = OMAP2_MCSPI_CHCONF_DMAR;
	else
		rw = OMAP2_MCSPI_CHCONF_DMAW;

	MOD_REG_BIT(l, rw, enable);
	mcspi_write_chconf0(spi, l);
}

#ifdef CONFIG_SPI_DEBUG
static int
omap2_mcspi_dump_regs(struct spi_master *master)
{
	u32 spi_base;
	u32 reg;
	u32 channel;
	struct omap2_mcspi *mcspi = spi_master_get_devdata(master);

	spi_base = (u32)mcspi->base;

	for (reg = 0; (reg < ARRAY_SIZE(reg_map)); reg++) {
		struct reg_type *reg_d = &reg_map[reg];
		u32 base1 = spi_base + reg_d->offset;
		if (reg_d->name[0] == 'C') {
			for (channel = 0; (channel < (ARRAY_SIZE(ch_reg_type)));
			    channel++) {
				struct reg_type *reg_c = &ch_reg_type[channel];
				u32 base2 = base1 + reg_c->offset;
				pr_debug("MCSPI_%s%s [0x%08X] = 0x%08X\n",
				       reg_d->name, reg_c->name, base2,
				       __raw_readl(base2));
			}
		} else {
			pr_debug("%s : [0x%08X] = 0x%08X\n",
				reg_d->name, base1, __raw_readl(base1));
		}

	}
	return 0;
}
#endif

static void omap2_mcspi_set_enable(const struct spi_device *spi, int enable)
{
	u32 l;

	l = enable ? OMAP2_MCSPI_CHCTRL_EN : 0;
	mcspi_write_cs_reg(spi, OMAP2_MCSPI_CHCTRL0, l);
	/* Flash post-writes */
	mcspi_read_cs_reg(spi, OMAP2_MCSPI_CHCTRL0);
}

static void omap2_mcspi_set_dma_req_both(const struct spi_device *spi,
		int is_read, int enable)
{
	u32 l, rw;
	l = mcspi_cached_chconf0(spi);
	rw = OMAP2_MCSPI_CHCONF_DMAR | OMAP2_MCSPI_CHCONF_DMAW;
	MOD_REG_BIT(l, rw, enable);
	mcspi_write_chconf0(spi, l);
}

static void omap2_mcspi_force_cs(struct spi_device *spi, int cs_active)
{
	u32 l;

	l = mcspi_cached_chconf0(spi);
	MOD_REG_BIT(l, OMAP2_MCSPI_CHCONF_FORCE, cs_active);
	mcspi_write_chconf0(spi, l);
}

static int omap2_mcspi_set_txfifo(const struct spi_device *spi, int buf_size,
					int enable, int bytes_per_wl)
{

	u32 l, rw, s, xfer_afl;

	unsigned short revert = 0;
	struct spi_master *master = spi->master;
	struct omap2_mcspi *mcspi = spi_master_get_devdata(master);

	u32 wcnt = buf_size/bytes_per_wl;

//	l = mcspi_read_cs_reg(spi, OMAP2_MCSPI_CHCONF0);
	l =  mcspi_cached_chconf0(spi);
	s = mcspi_read_cs_reg(spi, OMAP2_MCSPI_CHCTRL0);

	/* Read settings for TX FIFO */    
	xfer_afl = mcspi_read_reg(master, OMAP2_MCSPI_XFERLEVEL) & 0xff00;

	if (enable == 1) {

		/* FIFO cannot be enabled for both TX and RX
		 * simultaneously
		 */
//		if (l & OMAP2_MCSPI_CHCONF_FFER)
//			return -EPERM;

		/* Channel needs to be disabled and enabled
		 * for FIFO setting to take affect
		 */
		if (s & OMAP2_MCSPI_CHCTRL_EN) {
			omap2_mcspi_set_enable(spi, 0);
			revert = 1;
		}

		if (buf_size < mcspi->fifo_depth)
			mcspi_write_reg(master, OMAP2_MCSPI_XFERLEVEL,

						((wcnt << 16) |
						(xfer_afl) | 

						(buf_size - 1) << 0));
		else
			mcspi_write_reg(master, OMAP2_MCSPI_XFERLEVEL,

						((wcnt << 16) |
						(xfer_afl) |

						//(mcspi->fifo_depth - 1) << 0));
						((mcspi->fifo_depth - 1) << 0) | ((mcspi->fifo_depth - 1) << 8) ));
	}

	rw = OMAP2_MCSPI_CHCONF_FFET;
	MOD_REG_BIT(l, rw, enable);
//	mcspi_write_cs_reg(spi, OMAP2_MCSPI_CHCONF0, l);
	mcspi_write_chconf0(spi,l);

	if (revert)
		omap2_mcspi_set_enable(spi, 1);

	return 0;

}

static int omap2_mcspi_set_rxfifo(const struct spi_device *spi, int buf_size,
					int enable, int bytes_per_wl )
{

	u32 l, rw, s, xfer_ael;

	unsigned short revert = 0;
	struct spi_master *master = spi->master;
	struct omap2_mcspi *mcspi = spi_master_get_devdata(master);

	u32 wcnt = buf_size/bytes_per_wl;


//	l = mcspi_read_cs_reg(spi, OMAP2_MCSPI_CHCONF0);
	l =  mcspi_cached_chconf0(spi);

	s = mcspi_read_cs_reg(spi, OMAP2_MCSPI_CHCTRL0);

	/* Read settings for RX FIFO */    
	xfer_ael = mcspi_read_reg(master, OMAP2_MCSPI_XFERLEVEL) & 0xff00;

	if (enable == 1) {

		/* FIFO cannot be enabled for both TX and RX
		 * simultaneously
		 */
//		if (l & OMAP2_MCSPI_CHCONF_FFET)
//			return -EPERM;

		/* Channel needs to be disabled and enabled
		 * for FIFO setting to take affect
		 */
		if (s & OMAP2_MCSPI_CHCTRL_EN) {
			omap2_mcspi_set_enable(spi, 0);
			revert = 1;
		}

		if (buf_size < mcspi->fifo_depth)
			mcspi_write_reg(master, OMAP2_MCSPI_XFERLEVEL,

						((wcnt << 16) |
						(xfer_ael) | 

						(buf_size - 1) << 8));
		else
			mcspi_write_reg(master, OMAP2_MCSPI_XFERLEVEL,

						((wcnt << 16) |
						(xfer_ael) |

						//(mcspi->fifo_depth - 1) << 8));
						((mcspi->fifo_depth - 1) << 8) | ((mcspi->fifo_depth - 1) << 0) ));
	}

	rw = OMAP2_MCSPI_CHCONF_FFER;
	MOD_REG_BIT(l, rw, enable);
//	mcspi_write_cs_reg(spi, OMAP2_MCSPI_CHCONF0, l);
	mcspi_write_chconf0(spi,l);

	if (revert)
		omap2_mcspi_set_enable(spi, 1);

	return 0;

}

static void omap2_mcspi_set_master_mode(struct spi_master *master)
{
	u32 l;
	struct omap2_mcspi *mcspi = spi_master_get_devdata(master);

	/* setup when switching from (reset default) slave mode
	 * to single-channel master mode based on config value
	 */
	l = mcspi_read_reg(master, OMAP2_MCSPI_MODULCTRL);
	MOD_REG_BIT(l, OMAP2_MCSPI_MODULCTRL_STEST, 0);
	MOD_REG_BIT(l, OMAP2_MCSPI_MODULCTRL_MS, 0);

	if (mcspi->force_cs_mode)
		MOD_REG_BIT(l, OMAP2_MCSPI_MODULCTRL_SINGLE, 1);

	mcspi_write_reg(master, OMAP2_MCSPI_MODULCTRL, l);

	omap2_mcspi_ctx[master->bus_num - 1].modulctrl = l;
}

static void omap2_mcspi_set_slave_mode(struct spi_master *master)
{
	u32 l;

	l = mcspi_read_reg(master, OMAP2_MCSPI_MODULCTRL);
	MOD_REG_BIT(l, OMAP2_MCSPI_MODULCTRL_STEST, 0);
	MOD_REG_BIT(l, OMAP2_MCSPI_MODULCTRL_MS, 1);
	mcspi_write_reg(master, OMAP2_MCSPI_MODULCTRL, l);

	omap2_mcspi_ctx[master->bus_num - 1].modulctrl = l;
}

static int mcspi_wait_for_reg_bit(void __iomem *reg, unsigned long bit)
{
	unsigned long timeout;

	timeout = jiffies + msecs_to_jiffies(1000);
	while (!(__raw_readl(reg) & bit)) {
		if (time_after(jiffies, timeout))
			return -1;
		cpu_relax();
	}
	return 0;
}

static void omap2_mcspi_restore_ctx(struct omap2_mcspi *mcspi)
{
	struct spi_master *spi_cntrl;
	struct omap2_mcspi_cs *cs;
	spi_cntrl = mcspi->master;

	/* McSPI: context restore */
	mcspi_write_reg(spi_cntrl, OMAP2_MCSPI_MODULCTRL,
			omap2_mcspi_ctx[spi_cntrl->bus_num - 1].modulctrl);

	mcspi_write_reg(spi_cntrl, OMAP2_MCSPI_WAKEUPENABLE,
			omap2_mcspi_ctx[spi_cntrl->bus_num - 1].wakeupenable);

	list_for_each_entry(cs, &omap2_mcspi_ctx[spi_cntrl->bus_num - 1].cs,
			node)
		__raw_writel(cs->chconf0, cs->base +
				mcspi->regs[OMAP2_MCSPI_CHCONF0]);
}

static inline void omap2_mcspi_disable_clocks(struct omap2_mcspi *mcspi)
{
	pm_runtime_put_sync(mcspi->dev);
}

static inline int omap2_mcspi_enable_clocks(struct omap2_mcspi *mcspi)
{
	return pm_runtime_get_sync(mcspi->dev);
}

// LGE_UPDATE_S eungbo.shim@lge.com [EBS] For DMA FIFO 
static int                                                                                     
omap2_mcspi_dump_regs(struct spi_device *spi, int channel)                                                  
{   
	struct omap2_mcspi* mcspi = spi_master_get_devdata(spi->master);
	struct omap2_mcspi_dma* mcspi_dma = &mcspi->dma_channels[spi->chip_select];

#if 0
	printk("OMAP2_MCSPI_REVISION 0x%08X\n",mcspi_read_reg(master, OMAP2_MCSPI_REVISION));         
	printk("OMAP2_MCSPI_SYSCONFIG  0x%08X\n",mcspi_read_reg(master, OMAP2_MCSPI_SYSCONFIG));      
	printk("OMAP2_MCSPI_SYSSTATUS  0x%08X\n",mcspi_read_reg(master, OMAP2_MCSPI_SYSSTATUS));      
	printk("OMAP2_MCSPI_IRQSTATUS  0x%08X\n",mcspi_read_reg(master, OMAP2_MCSPI_IRQSTATUS));      
	printk("OMAP2_MCSPI_IRQENABLE  0x%08X\n",mcspi_read_reg(master, OMAP2_MCSPI_IRQENABLE));      
	printk("OMAP2_MCSPI_WAKEUPENABLE  0x%08X\n",mcspi_read_reg(master, OMAP2_MCSPI_WAKEUPENABLE));
	printk("OMAP2_MCSPI_SYST  0x%08X\n",mcspi_read_reg(master, OMAP2_MCSPI_SYST));                
	printk("OMAP2_MCSPI_MODULCTRL  0x%08X\n",mcspi_read_reg(master, OMAP2_MCSPI_MODULCTRL));      
	printk("OMAP2_MCSPI_XFERLEVEL  0x%08X\n",mcspi_read_reg(master, OMAP2_MCSPI_XFERLEVEL));   
	printk("OMAP2_MCSPI_CHCONF0  0x%08X\n",mcspi_read_cs_reg(spi, OMAP2_MCSPI_CHCONF0));          
	printk("OMAP2_MCSPI_CHSTAT0  0x%08X\n",mcspi_read_cs_reg(spi, OMAP2_MCSPI_CHSTAT0));          
	printk("OMAP2_MCSPI_CHCTRL0  0x%08X\n",mcspi_read_cs_reg(spi, OMAP2_MCSPI_CHCTRL0));          
	printk("OMAP2_MCSPI_TX0  0x%08X\n",mcspi_read_cs_reg(spi, OMAP2_MCSPI_TX0));                  
	printk("OMAP2_MCSPI_RX0  0x%08X\n",mcspi_read_cs_reg(spi, OMAP2_MCSPI_RX0));                  
#endif
	// read OMAP4430 registers directly
	printk("OMAP2_MCSPI_REVISION 0x%08X\n", omap_readl(0x480BA100));         
	printk("OMAP2_MCSPI_SYSCONFIG  0x%08X\n",omap_readl(0x480BA110));      
    printk("#####OMAP2_MCSPI_HL_SYSCONFIG  0x%08X#####\n",omap_readl(0x480BA010));    
	printk("OMAP2_MCSPI_SYSSTATUS  0x%08X\n",omap_readl(0x480BA114));      
	printk("OMAP2_MCSPI_IRQSTATUS  0x%08X\n",omap_readl(0x480BA118));      
	printk("OMAP2_MCSPI_IRQENABLE  0x%08X\n",omap_readl(0x480BA11C));      
	printk("OMAP2_MCSPI_WAKEUPENABLE  0x%08X\n",omap_readl(0x480BA120));
	printk("OMAP2_MCSPI_SYST  0x%08X\n",omap_readl(0x480BA124));                
	printk("OMAP2_MCSPI_MODULCTRL  0x%08X\n",omap_readl(0x480BA128));      
	printk("OMAP2_MCSPI_XFERLEVEL  0x%08X\n",omap_readl(0x480BA17C));   
	printk("OMAP2_MCSPI_CHCONF0  0x%08X\n",omap_readl(0x480BA12C));          
	printk("OMAP2_MCSPI_CHSTAT0  0x%08X\n",omap_readl(0x480BA130));          
	printk("OMAP2_MCSPI_CHCTRL0  0x%08X\n",omap_readl(0x480BA134));          
	printk("OMAP2_MCSPI_TX0  0x%08X\n",omap_readl(0x480BA138));                  
	printk("OMAP2_MCSPI_RX0  0x%08X\n",omap_readl(0x480BA13C));                  
	
	printk("DMA4 REG DUMPS FOR %s Channel\n", ( channel == mcspi_dma->dma_tx_channel )?"TX":"RX");
	channel = channel * 0x60; 
    
	printk("DMA4_IRQSTATUS 0x%08X\n", omap_readl( DMA4_IRQSTATUS_ADDR + ( 0x4 * 2)));   //hyunh0.cho 3-> 2
    printk("DMA4_IRQENABLE 0x%08X\n", omap_readl( DMA4_IRQENABLE_ADDR + ( 0x4 * 2)));   //hyunh0.cho 3-> 2
    
    printk("DMA4_SYSSTATUS 0x%08X\n", omap_readl( DMA4_SYSSTATUS_ADDR ));
    printk("DMA4_OCP_SYSCONF_ADDR 0x%08X\n", omap_readl( DMA4_OCP_SYSCONF_ADDR ));
    printk("DMA4_CAPS_0_ADDR 0x%08X\n", omap_readl( DMA4_CAPS_0_ADDR ));
    printk("DMA4_CAPS_2_ADDR 0x%08X\n", omap_readl( DMA4_CAPS_2_ADDR ));
    printk("DMA4_CAPS_3_ADDR 0x%08X\n", omap_readl( DMA4_CAPS_3_ADDR ));
    printk("DMA4_CAPS_4_ADDR 0x%08X\n", omap_readl( DMA4_CAPS_4_ADDR ));
    printk("DMA4_GCR_ADDR 0x%08X\n", omap_readl( DMA4_GCR_ADDR ));

    printk("DMA4_CCR_ADDR 0x%08X\n", omap_readl( DMA4_CCR_ADDR + channel ));
    printk("DMA4_CLNK_CTRL_ADDR 0x%08X\n", omap_readl( DMA4_CLNK_CTRL_ADDR + channel ));
    printk("DMA4_CICR_ADDR 0x%08X\n", omap_readl( DMA4_CICR_ADDR + channel ));
    printk("DMA4_CSR_ADDR 0x%08X\n", omap_readl( DMA4_CSR_ADDR + channel ));
    printk("DMA4_CSDP_ADDR 0x%08X\n", omap_readl( DMA4_CSDP_ADDR + channel ));
    printk("DMA4_CEN_ADDR 0x%08X\n", omap_readl( DMA4_CEN_ADDR + channel ));
    printk("DMA4_CFN_ADDR 0x%08X\n", omap_readl( DMA4_CFN_ADDR + channel ));
    printk("DMA4_CSSA_ADDR 0x%08X\n", omap_readl( DMA4_CSSA_ADDR + channel ));
    printk("DMA4_CDSA_ADDR 0x%08X\n", omap_readl( DMA4_CDSA_ADDR + channel));
    printk("DMA4_CSEI_ADDR 0x%08X\n", omap_readl( DMA4_CSEI_ADDR + channel));
    printk("DMA4_CSFI_ADDR 0x%08X\n", omap_readl( DMA4_CSFI_ADDR + channel));
    printk("DMA4_CDEI_ADDR 0x%08X\n", omap_readl( DMA4_CDEI_ADDR + channel));
    printk("DMA4_CDFI_ADDR 0x%08X\n", omap_readl( DMA4_CDFI_ADDR + channel));
    printk("DMA4_CSAC_ADDR 0x%08X\n", omap_readl( DMA4_CSAC_ADDR + channel));
	printk("DMA4_CDAC_ADDR 0x%08X\n", omap_readl( DMA4_CDAC_ADDR + channel));
    printk("DMA4_CCEN_ADDRESS 0x%08X\n", omap_readl( DMA4_CCEN_ADDRESS + channel));
    printk("DMA4_CCFN_ADDRESS 0x%08X\n", omap_readl( DMA4_CCFN_ADDRESS + channel));
	printk("DMA4_COLOR_ADDR 0x%08X\n", omap_readl( DMA4_COLOR_ADDR + channel));

	return 0;                                                                                  
}          
// LGE_UPDATE_E eungbo.shim@lge.com [EBS]

static unsigned
omap2_mcspi_txrx_dma(struct spi_device *spi, struct spi_transfer *xfer)
{
	struct omap2_mcspi	*mcspi;
	struct omap2_mcspi_cs	*cs = spi->controller_state;
	struct omap2_mcspi_dma  *mcspi_dma;
	unsigned int		count, c, bytes_per_transfer;
	unsigned long		base, tx_reg, rx_reg;
	int			word_len, data_type, element_count;
	int			elements = 0, frame_count, sync_type;
	
#ifdef SPI_RETRY_ENABLE
	volatile unsigned int before_tx_ccen = 0;
	volatile unsigned int before_tx_ccfn = 0;
	volatile unsigned int before_rx_ccen = 0;
	volatile unsigned int before_rx_ccfn = 0;
	// temp ti request start by et.jo 20111110
	volatile int ti_temp_1 = 0;
	volatile int ti_temp_2 = 0;
	volatile int ti_temp_3 = 0;
	volatile int ti_temp_4 = 0;
	volatile int ti_temp_5 = 0;
	volatile int ti_temp_6 = 0;
	// temp ti end
#endif /* SPI_RETRY_ENABLE */
// LGE_UPDATE_S eungbo.shim@lge.com [EBS] For BURST Mode Setting 
#ifdef LGE_RIL_SPI
#ifdef CONFIG_LGE_SPI
	int chk_set = 0;
#endif
#endif
// LGE_UPDATE_E eungbo.shim@lge.com [EBS]
	u32			l;
	u8			* rx;
	const u8		* tx;
	void __iomem            *irqstat_reg;

	mcspi = spi_master_get_devdata(spi->master);
	mcspi_dma = &mcspi->dma_channels[spi->chip_select];
	irqstat_reg = mcspi->base + mcspi->regs[OMAP2_MCSPI_IRQSTATUS];
	l = mcspi_cached_chconf0(spi);

	count = xfer->len;
	c = count;
	word_len = cs->word_len;

	base = cs->phys;
	tx_reg = base + mcspi->regs[OMAP2_MCSPI_TX0];
	rx_reg = base + mcspi->regs[OMAP2_MCSPI_RX0];
	rx = xfer->rx_buf;
	tx = xfer->tx_buf;

	if (word_len <= 8) {
		data_type = OMAP_DMA_DATA_TYPE_S8;
		element_count = count;
		bytes_per_transfer = 1;
	} else if (word_len <= 16) {
		data_type = OMAP_DMA_DATA_TYPE_S16;
		element_count = count >> 1;
		bytes_per_transfer = 2;
	} else /* word_len <= 32 */ {
		data_type = OMAP_DMA_DATA_TYPE_S32;
		element_count = count >> 2;
		bytes_per_transfer = 4;
	}

	if ((mcspi->fifo_depth != 0) && (count > mcspi->fifo_depth)) {
		sync_type = OMAP_DMA_SYNC_FRAME;
		element_count = mcspi->fifo_depth/bytes_per_transfer;
		frame_count = count/mcspi->fifo_depth;
	} else if ((mcspi->fifo_depth != 0) && (count <=  mcspi->fifo_depth)) {
		sync_type = OMAP_DMA_SYNC_FRAME;
		frame_count = 1;
	} else {
		sync_type = OMAP_DMA_SYNC_ELEMENT;
		frame_count = 1;
	}
// LGE_UPDATE_S eungbo.shim@lge.com [EBS] For BURST Mode Setting 
#ifdef LGE_RIL_SPI
#ifdef CONFIG_LGE_SPI
	if( mcspi->master->bus_num == 4 )
	{
			chk_set = 1;
			omap_dma_set_global_params(DMA_DEFAULT_ARB_RATE, 0x20, 1); // SPI_IPC
	  		omap_dma_set_prio_lch(mcspi_dma->dma_tx_channel, DMA_CH_PRIO_HIGH, DMA_CH_PRIO_HIGH);
	  		omap_dma_set_prio_lch(mcspi_dma->dma_rx_channel, DMA_CH_PRIO_HIGH, 0);
			omap_set_dma_dest_burst_mode(mcspi_dma->dma_tx_channel,OMAP_DMA_DATA_BURST_4);	
			omap_set_dma_src_burst_mode(mcspi_dma->dma_tx_channel,OMAP_DMA_DATA_BURST_4);
			omap_set_dma_dest_burst_mode(mcspi_dma->dma_rx_channel,OMAP_DMA_DATA_BURST_4);	
			omap_set_dma_src_burst_mode(mcspi_dma->dma_rx_channel,OMAP_DMA_DATA_BURST_4);

   
           
			omap_writel((omap_readl(0x480BA110) & 0xffffffe7) | 0x00000010, 0x480BA110);   //test //hyunh0.cho



	 		// omap_set_dma_prefetch(mcspi_dma->dma_tx_channel, 1);
	 		//omap_set_dma_write_mode(mcspi_dma->dma_tx_channel, OMAP_DMA_WRITE_POSTED); // Performance improvement - TEDCHO
			//omap_set_dma_write_mode(mcspi_dma->dma_rx_channel, OMAP_DMA_WRITE_POSTED); // Performance improvement - TEDCHO
			pm_qos_update_request(&pm_qos_handle_for_spi, 10);
			omap_pm_set_min_bus_tput(&spi->dev,OCP_INITIATOR_AGENT, 800000);	//omap-pm.c
	}
#endif
#endif
// LGE_UPDATE_E eungbo.shim@lge.com [EBS]
	if (tx != NULL) {
		omap_set_dma_transfer_params(mcspi_dma->dma_tx_channel,
				data_type, element_count, frame_count,
				sync_type, mcspi_dma->dma_tx_sync_dev, 0);
		omap_set_dma_dest_params(mcspi_dma->dma_tx_channel, 0,
				OMAP_DMA_AMODE_CONSTANT,
				tx_reg, 0, 0);
		omap_set_dma_src_params(mcspi_dma->dma_tx_channel, 0,
				OMAP_DMA_AMODE_POST_INC,
				xfer->tx_dma, 0, 0);

		if (mcspi->fifo_depth != 0)
				omap2_mcspi_set_txfifo(spi, count, 1, bytes_per_transfer);
	}

	if (rx != NULL) {
		if (mcspi->fifo_depth == 0) {
		elements = element_count - 1;
		if (l & OMAP2_MCSPI_CHCONF_TURBO)
			elements--;
		} else {
			elements = element_count;
		}

		omap_set_dma_transfer_params(mcspi_dma->dma_rx_channel,
				data_type, elements, frame_count,
				sync_type,
				mcspi_dma->dma_rx_sync_dev, 1);

		omap_set_dma_src_params(mcspi_dma->dma_rx_channel, 0,
				OMAP_DMA_AMODE_CONSTANT,
				rx_reg, 0, 0);


		omap_set_dma_dest_params(mcspi_dma->dma_rx_channel, 0,
				OMAP_DMA_AMODE_POST_INC,
				xfer->rx_dma, 0, 0);

		if (mcspi->fifo_depth != 0) {
			omap2_mcspi_set_rxfifo(spi, count, 1, bytes_per_transfer);
		//	printk(" [MSPI] fifo_depth / count = %d\n",count);
			/* Dummy write required for RX only mode */
			if (tx == NULL)
				mcspi_write_cs_reg(spi, OMAP2_MCSPI_TX0, 0);

		}
	}

	//hyunh0.cho
	mcspi_write_reg(mcspi->master, OMAP2_MCSPI_IRQSTATUS,
			0x00010001);

// LGE_UPDATE_S eungbo.shim@lge.com [EBS] For DMA Debugging 
#if 0 //def LGE_RIL_SPI	
#if 1 // TEDCHO DMA Monitor - TRM 11.5.4 Synchronized Transfer Monitoring Using CDAC
 omap_dma_set_global_params(DMA_DEFAULT_ARB_RATE, 0x20, 1); // SPI_IPC
 omap_dma_set_prio_lch(mcspi_dma->dma_tx_channel, 0, DMA_CH_PRIO_HIGH);  
 omap_dma_set_prio_lch(mcspi_dma->dma_rx_channel, DMA_CH_PRIO_HIGH, 0); 
 // omap_set_dma_prefetch(mcspi_dma->dma_tx_channel, 1); 
 // omap_set_dma_write_mode(mcspi_dma->dma_tx_channel, OMAP_DMA_WRITE_POSTED);
#endif // TEDCHO
#endif 
// LGE_UPDATE_E eungbo.shim@lge.com [EBS]

#if 1 //EBS 
		omap_writel(0, ((DMA4_CDAC_ADDRESS) + (mcspi_dma->dma_tx_channel * 0x60))); // OMAP_DMA4_CDACi
		omap_writel(0, ((DMA4_CDAC_ADDRESS) + (mcspi_dma->dma_rx_channel * 0x60))); // OMAP_DMA4_CDACi

#endif
#if 0
    if ((tx != NULL) && ( rx != NULL )) 
	{
        omap_start_dma(mcspi_dma->dma_tx_channel);
		omap_start_dma(mcspi_dma->dma_rx_channel);
		//omap2_mcspi_set_dma_req(spi, 0, 1);
		omap2_mcspi_set_dma_req_both(spi, 1, 1);
	}
	else if (rx != NULL) 
	{
		omap_start_dma(mcspi_dma->dma_rx_channel);
		omap2_mcspi_set_dma_req(spi, 1, 1);
    }
	else if( tx != NULL )
	{
		omap_start_dma(mcspi_dma->dma_tx_channel);
		omap2_mcspi_set_dma_req(spi, 0, 1);
	}
#else

#ifdef SPI_RETRY_ENABLE
	// et.jo test ti request
	ti_temp_1 = word_len;
	ti_temp_2 = mcspi->fifo_depth;
	ti_temp_3 = count;
	ti_temp_4 = element_count;
	ti_temp_5 = frame_count;
	ti_temp_6 = sync_type;
#endif /* SPI_RETRY_ENABLE */
	// et.jo test end
	
#if 0	//location change for test  [hyunh0.cho@lge.com]
	#ifdef CONFIG_LGE_SPI
            if (mcspi->mcspi_mode == OMAP2_MCSPI_SLAVE) 
            {
                        spi->slave_ready(spi, 1);
                        //udelay(100);
                        spi->slave_ready(spi, 0);
            }
	#endif
#endif


	
	if (tx != NULL) 
	{
		omap_start_dma(mcspi_dma->dma_tx_channel);
		omap2_mcspi_set_dma_req(spi, 0, 1);
	}

	if (rx != NULL)
	{
		omap_start_dma(mcspi_dma->dma_rx_channel);
		omap2_mcspi_set_dma_req(spi, 1, 1);
	}

#endif 

#ifdef SPI_RETRY_ENABLE
	if( tx != NULL )
	{
		before_tx_ccen = omap_readl((DMA4_CCEN_ADDRESS) + (mcspi_dma->dma_tx_channel * 0x60)); 
		before_tx_ccfn = omap_readl((DMA4_CCFN_ADDRESS) + (mcspi_dma->dma_tx_channel * 0x60)); 
	}
	if( rx != NULL )
	{
		before_rx_ccen = omap_readl((DMA4_CCEN_ADDRESS) + (mcspi_dma->dma_rx_channel * 0x60)); 
		before_rx_ccfn = omap_readl((DMA4_CCFN_ADDRESS) + (mcspi_dma->dma_rx_channel * 0x60)); 
	}
#endif /* SPI_RETRY_ENABLE */

#ifdef CONFIG_LGE_SPI
	if (mcspi->mcspi_mode == OMAP2_MCSPI_SLAVE) 
	{
		spi->slave_ready(spi, 1);
		udelay(100);
		spi->slave_ready(spi, 0);
	}
#endif

	if (tx != NULL) 
	{
		// LGE_UPDATE_S eungbo.shim@lge.com [EBS] SPI Retry Routine 
#ifdef LGE_RIL_SPI// KNK_TEST
#ifdef CONFIG_LGE_SPI
		if (mcspi->mcspi_mode == OMAP2_MCSPI_SLAVE) //For RIL SPI 
		{

#ifndef SPI_RETRY_ENABLE
			if( (wait_for_completion_timeout(&mcspi_dma->dma_tx_completion, DMA_TIMEOUT_LIMIT)) == 0 )
			{
				printk("omap2_mcspi_txrx_dma : TX DMA Timeout !!\n");
				return -1;
			}
#else
			int wait_ret ,rt_cnt=0;
			do 
			{
				wait_ret = wait_for_completion_timeout(&mcspi_dma->dma_tx_completion, DMA_TIMEOUT_LIMIT) ;  /* 20110304 dongyu.gwak@lge.com Retry Scheme */
				if( wait_ret == 0 )
				{
					volatile unsigned long tmp_dma_read, tmp_dma_read1;
					tmp_dma_read = omap_readl( (DMA4_CDAC_ADDRESS) + (mcspi_dma->dma_tx_channel * 0x60) );	// OMAP_DMA4_CDACi
					if(tmp_dma_read  == 0)
					{
						// transfer didn't start
						printk(KERN_ERR "<< SPI v.S14 Timeout occurred, Transfer not started\n" );
					}
					else
					{
						// transfer started.
						tmp_dma_read = omap_readl( DMA4_CCEN_ADDRESS + (mcspi_dma->dma_tx_channel * 0x60) ); // CCENi
						tmp_dma_read1 = omap_readl( DMA4_CCFN_ADDRESS + (mcspi_dma->dma_tx_channel * 0x60) ); // CCFNi
						printk(KERN_ERR "<< SPI v.S14 Timeout occurred, Transfer started CCEN 0x%x CCFN 0x%x\n", tmp_dma_read, tmp_dma_read1 );



                        
					}
					tmp_dma_read = omap_readl( DMA4_CCEN_ADDRESS + (mcspi_dma->dma_tx_channel * 0x60) ); // CCENi
					tmp_dma_read1 = omap_readl( DMA4_CCFN_ADDRESS + (mcspi_dma->dma_tx_channel * 0x60) ); // CCFNi
					printk(KERN_ERR "Transfer timeout CCEN 0x%08X -> 0x%08X CCFN 0x%08X -> 0x%08X\n", before_tx_ccen, tmp_dma_read, before_tx_ccfn, tmp_dma_read1 );
					printk("[LGE_IPC] LAST TX : %c%c%c%c%c%c%c%c%c\n", *(((char*)tx)+8), *(((char*)tx)+9),*(((char*)tx)+10),*(((char*)tx)+11),*(((char*)tx)+12),*(((char*)tx)+13), *(((char*)tx)+14),*(((char*)tx)+15),*(((char*)tx)+16));
					printk("[LGE_IPC] current pm_qos target value : %d\n", pm_qos_request(PM_QOS_CPU_DMA_LATENCY));
					printk("[LGE_IPC] version : board_rev rev_c \n");
					omap2_mcspi_dump_regs(spi, mcspi_dma->dma_tx_channel);



                    
					//return -ETIMEDOUT;
					printk(KERN_ERR "[LGE-SPI] SPI Host Req. Retry -----------------[%d] \n",rt_cnt);  /* 20110311 dongyu.gwak@lge.com Don't Retry*/
					
					// temp code for debugging for TD 34233		
					printk(KERN_ERR "<< omap2_mcspi_txrx_dma %d %d %d %d %d %d\n", 
									ti_temp_1, ti_temp_2, ti_temp_3, ti_temp_4, ti_temp_5, ti_temp_6);
// [LGE_IPC] LGE_UPDATE_S DMA RESET TEST et.jo
#if 1 
								if( rx != NULL )
								{
									omap2_mcspi_set_dma_req(spi, 1, 0);
									omap_stop_dma(mcspi_dma->dma_rx_channel);
									omap_clear_dma(mcspi_dma->dma_rx_channel);
									printk("[LGE-SPI] omap_clear_dma : dma_rx\n");
								}
								omap2_mcspi_set_dma_req(spi, 0, 0);
								omap_stop_dma(mcspi_dma->dma_tx_channel);
								omap_clear_dma(mcspi_dma->dma_tx_channel);
								printk("[LGE-SPI] omap_clear_dma : dma_tx\n");
                                
								if( mcspi->master->bus_num == 4 )
								{
									pm_qos_update_request(&pm_qos_handle_for_spi, PM_QOS_DEFAULT_VALUE);
									omap_pm_set_min_bus_tput(&spi->dev,OCP_INITIATOR_AGENT, -1);    //omap-pm.c
								}
								// [LGE_IPC] LGE_UPDATE_S DMA RESET TEST et.jo
													
#endif /* DMA_RESET_TEST by et.jo */
					return -1; // for no retry in dma timeout
				}
				else 
					break ;
			} while((++rt_cnt <= 2)) ;  /* 20110304 dongyu.gwak@lge.com Don't Retry */
#endif	/* SPI_RETRY_ENABLE */
		}
		else
#endif	/* CONFIG_LGE_SPI 	*/
#endif	/* LGE_RIL_SPI		*/

			wait_for_completion(&mcspi_dma->dma_tx_completion); //For dmB mode 

		if (mcspi->fifo_depth != 0) {
			if (mcspi_wait_for_reg_bit(irqstat_reg,
						OMAP2_MCSPI_IRQ_EOW) < 0)
				dev_err(&spi->dev, "TXS timed out\n");

			mcspi_write_reg(mcspi->master, OMAP2_MCSPI_IRQSTATUS,
					OMAP2_MCSPI_IRQ_EOW);

			omap2_mcspi_set_txfifo(spi, count, 0, bytes_per_transfer);
		}

		dma_unmap_single(NULL, xfer->tx_dma, count, DMA_TO_DEVICE);
		//20110329 ws.yang@lge.com .. Reset Through-put requirement /* 20110719 dongyu.gwak@lge.com L3 200Mhz from Justin */ 
	}

	if (rx != NULL) 
	{

// LGE_UPDATE_S eungbo.shim@lge.com [EBS] -- For SPI Retry Routine 
#ifdef LGE_RIL_SPI // KNK_TEST
#ifdef CONFIG_LGE_SPI
					if (mcspi->mcspi_mode == OMAP2_MCSPI_SLAVE)
					{
#ifndef SPI_RETRY_ENABLE
						if( (wait_for_completion_timeout(&mcspi_dma->dma_rx_completion, DMA_TIMEOUT_LIMIT)) == 0 )
						{
							printk("omap2_mcspi_txrx_dma : RX DMA Timeout !!\n");
							return -1;
						}
#else
						int wait_ret_rx ,rt_cnt_rx=0;
						do 
						{
							//wait_ret_rx = wait_for_completion_timeout(&mcspi_dma->dma_rx_completion,1*HZ) ;
							wait_ret_rx = wait_for_completion_timeout(&mcspi_dma->dma_rx_completion, DMA_TIMEOUT_LIMIT); // 1tick == 8ms, 3000 * 8ms = 24,000ms
							if( wait_ret_rx == 0 )
							{
								volatile unsigned long tmp_dma_read_rx, tmp_dma_read1_rx;
								tmp_dma_read_rx = omap_readl( DMA4_CDAC_ADDRESS + (mcspi_dma->dma_rx_channel * 0x60) );	// OMAP_DMA4_CDACi
								if(tmp_dma_read_rx	== 0)
								{
									// transfer didn't start
									printk(KERN_ERR "<< SPI v.S14 Timeout occurred, Receiving not started\n" );
								}
								else
								{
									// transfer started.
									tmp_dma_read_rx = omap_readl( DMA4_CCEN_ADDRESS + (mcspi_dma->dma_rx_channel * 0x60) ); // CCENi
									tmp_dma_read1_rx = omap_readl( DMA4_CCFN_ADDRESS + (mcspi_dma->dma_rx_channel * 0x60) ); // CCFNi
									printk(KERN_ERR "<< SPI v.S14 Timeout occurred, Receiving started CCEN 0x%x CCFN 0x%x\n", tmp_dma_read_rx, tmp_dma_read1_rx );
								}
								tmp_dma_read_rx = omap_readl( DMA4_CCEN_ADDRESS + (mcspi_dma->dma_rx_channel * 0x60) ); // CCENi
								tmp_dma_read1_rx = omap_readl( DMA4_CCFN_ADDRESS + (mcspi_dma->dma_rx_channel * 0x60) ); // CCFNi
								printk(KERN_ERR "Receiving timeout CCEN 0x%08X -> 0x%08X CCFN 0x%08X -> 0x%08X\n", before_rx_ccen, tmp_dma_read_rx, before_rx_ccfn, tmp_dma_read1_rx );
								if( tx != NULL )
									printk("[LGE_IPC] LAST TX : %c%c%c%c%c%c%c%c%c\n", *(((char*)tx)+8), *(((char*)tx)+9),*(((char*)tx)+10),*(((char*)tx)+11),*(((char*)tx)+12),*(((char*)tx)+13), *(((char*)tx)+14),*(((char*)tx)+15),*(((char*)tx)+16));	
								printk("[LGE_IPC] current pm_qos target value : %d\n", pm_qos_request(PM_QOS_CPU_DMA_LATENCY));
								omap2_mcspi_dump_regs(spi, mcspi_dma->dma_rx_channel);
						//return -ETIMEDOUT;
								printk(KERN_ERR "[LGE-SPI] SPI Host Req. Retry -----------------[%d] \n",rt_cnt_rx);
								
								// temp code for debugging TD 34233
								printk(KERN_ERR "<< omap2_mcspi_txrx_dma %d %d %d %d %d %d\n", 
											ti_temp_1, ti_temp_2, ti_temp_3, ti_temp_4, ti_temp_5, ti_temp_6);

								// [LGE_IPC] LGE_UPDATE_S DMA RESET TEST et.jo
#if 1 
								omap2_mcspi_set_dma_req(spi, 1, 0);
								omap_stop_dma(mcspi_dma->dma_rx_channel);
								omap_clear_dma(mcspi_dma->dma_rx_channel);
								printk("[LGE-SPI] omap_clear_dma : dma_rx\n");
								if( tx != NULL )
								{
									omap_stop_dma(mcspi_dma->dma_tx_channel);
									omap_clear_dma(mcspi_dma->dma_tx_channel);
									printk("[LGE-SPI] omap_clear_dma : dma_tx\n");
								}
								if( mcspi->master->bus_num == 4 )
								{
									pm_qos_update_request(&pm_qos_handle_for_spi, PM_QOS_DEFAULT_VALUE);
									omap_pm_set_min_bus_tput(&spi->dev,OCP_INITIATOR_AGENT, -1);    //omap-pm.c
								}
#endif /* DMA_RESET_TEST by et.jo */
								// [LGE_IPC] LGE_UPDATE_S DMA RESET TEST et.jo

								return -1; // for no retry in dma timeout
								// temp code end
							//	spi->slave_ready(spi, 1);
							//	spi->slave_ready(spi, 0);
							}
							else 
								break ;
						} while((++rt_cnt_rx <= 2));
#endif /* SPI_RETRY_ENABLE */
					}
					else
#endif	/* CONFIG_LGE_SPI 	*/
#endif	/* LGE_RIL_SPI 		*/
//LGE_UPDATE_E eungbo.shim@lge.com [EBS]
	
		wait_for_completion(&mcspi_dma->dma_rx_completion);

		if (mcspi->fifo_depth != 0) {
			omap2_mcspi_set_rxfifo(spi, count, 0, bytes_per_transfer);

		mcspi_write_reg(mcspi->master, OMAP2_MCSPI_IRQSTATUS,
				OMAP2_MCSPI_IRQ_EOW);

		}

		dma_unmap_single(NULL, xfer->rx_dma, count, DMA_FROM_DEVICE);
		omap2_mcspi_set_enable(spi, 0);

		if (l & OMAP2_MCSPI_CHCONF_TURBO) {

			if (likely(mcspi_read_cs_reg(spi, OMAP2_MCSPI_CHSTAT0)
				   & OMAP2_MCSPI_CHSTAT_RXS)) {
				u32 w;

				w = mcspi_read_cs_reg(spi, OMAP2_MCSPI_RX0);
				if (word_len <= 8)
					((u8 *)xfer->rx_buf)[elements++] = w;
				else if (word_len <= 16)
					((u16 *)xfer->rx_buf)[elements++] = w;
				else /* word_len <= 32 */
					((u32 *)xfer->rx_buf)[elements++] = w;
			} else {
				dev_err(&spi->dev,
					"DMA RX penultimate word empty");
				count -= (word_len <= 8)  ? 2 :
					(word_len <= 16) ? 4 :
					/* word_len <= 32 */ 8;
				omap2_mcspi_set_enable(spi, 1);
				return count;
			}
		}

		if (mcspi->fifo_depth == 0) { // check later  - TEDCHO
		if (likely(mcspi_read_cs_reg(spi, OMAP2_MCSPI_CHSTAT0)
				& OMAP2_MCSPI_CHSTAT_RXS)) {
			u32 w;

			w = mcspi_read_cs_reg(spi, OMAP2_MCSPI_RX0);
			if (word_len <= 8)
				((u8 *)xfer->rx_buf)[elements] = w;
			else if (word_len <= 16)
				((u16 *)xfer->rx_buf)[elements] = w;
			else /* word_len <= 32 */
				((u32 *)xfer->rx_buf)[elements] = w;
		} else {
			dev_err(&spi->dev, "DMA RX last word empty");
			count -= (word_len <= 8)  ? 1 :
				 (word_len <= 16) ? 2 :
			       /* word_len <= 32 */ 4;
		}
		} 
		// if (mcspi->fifo_depth == 0)  // check later
		omap2_mcspi_set_enable(spi, 1);
// LGE_UPDATES_S eungbo.shim@lge.com [EBS] Move to slave Rdy Code  
#if 0 // KNK_TEST
#if 1
	if (mcspi->mcspi_mode == OMAP2_MCSPI_SLAVE)
		spi->slave_ready(spi, 0);
#endif
#endif
// LGE_UPDATES_E eungbo.shim@lge.com [EBS] Move to slave Rdy Code  
	}
	
	// [LGE-IPC] LGE_UPDATE_S et.jo@lge.com 20111126
	if( mcspi->master->bus_num == 4 )
	{
		pm_qos_update_request(&pm_qos_handle_for_spi, PM_QOS_DEFAULT_VALUE);
		omap_pm_set_min_bus_tput(&spi->dev, OCP_INITIATOR_AGENT, -1); 		
	}
	// [LGE-IPC] LGE_UPDATE_E et.jo@lge.com
	return count;
}

static unsigned
omap2_mcspi_txrx_pio(struct spi_device *spi, struct spi_transfer *xfer)
{
	struct omap2_mcspi	*mcspi;
	struct omap2_mcspi_cs	*cs = spi->controller_state;
	unsigned int		count, c;
	u32			l;
	void __iomem		*base = cs->base;
	void __iomem		*tx_reg;
	void __iomem		*rx_reg;
	void __iomem		*chstat_reg;
	int			word_len;

	mcspi = spi_master_get_devdata(spi->master);
	count = xfer->len;
	c = count;
	word_len = cs->word_len;

	l = mcspi_cached_chconf0(spi);

	/* We store the pre-calculated register addresses on stack to speed
	 * up the transfer loop. */
	tx_reg		= base + mcspi->regs[OMAP2_MCSPI_TX0];
	rx_reg		= base + mcspi->regs[OMAP2_MCSPI_RX0];
	chstat_reg	= base + mcspi->regs[OMAP2_MCSPI_CHSTAT0];

	if (word_len <= 8) {
		u8		*rx;
		const u8	*tx;

		rx = xfer->rx_buf;
		tx = xfer->tx_buf;

		do {
			c -= 1;
			if (tx != NULL) {
				if (mcspi_wait_for_reg_bit(chstat_reg,
						OMAP2_MCSPI_CHSTAT_TXS) < 0) {
					dev_err(&spi->dev, "TXS timed out\n");
					goto out;
				}
#ifdef VERBOSE
				dev_dbg(&spi->dev, "write-%d %02x\n",
						word_len, *tx);
#endif
				__raw_writel(*tx++, tx_reg);
			}
			if (rx != NULL) {
				if (mcspi_wait_for_reg_bit(chstat_reg,
						OMAP2_MCSPI_CHSTAT_RXS) < 0) {
					dev_err(&spi->dev, "RXS timed out\n");
					goto out;
				}

				if (c == 1 && tx == NULL &&
				    (l & OMAP2_MCSPI_CHCONF_TURBO)) {
					omap2_mcspi_set_enable(spi, 0);
					*rx++ = __raw_readl(rx_reg);
#ifdef VERBOSE
					dev_dbg(&spi->dev, "read-%d %02x\n",
						    word_len, *(rx - 1));
#endif
					if (mcspi_wait_for_reg_bit(chstat_reg,
						OMAP2_MCSPI_CHSTAT_RXS) < 0) {
						dev_err(&spi->dev,
							"RXS timed out\n");
						goto out;
					}
					c = 0;
				} else if (c == 0 && tx == NULL) {
					omap2_mcspi_set_enable(spi, 0);
				}

				*rx++ = __raw_readl(rx_reg);
#ifdef VERBOSE
				dev_dbg(&spi->dev, "read-%d %02x\n",
						word_len, *(rx - 1));
#endif
			}
		} while (c);
	} else if (word_len <= 16) {
		u16		*rx;
		const u16	*tx;

		rx = xfer->rx_buf;
		tx = xfer->tx_buf;
		do {
			c -= 2;
			if (tx != NULL) {
				if (mcspi_wait_for_reg_bit(chstat_reg,
						OMAP2_MCSPI_CHSTAT_TXS) < 0) {
					dev_err(&spi->dev, "TXS timed out\n");
					goto out;
				}
#ifdef VERBOSE
				dev_dbg(&spi->dev, "write-%d %04x\n",
						word_len, *tx);
#endif
				__raw_writel(*tx++, tx_reg);
			}
			if (rx != NULL) {
				if (mcspi_wait_for_reg_bit(chstat_reg,
						OMAP2_MCSPI_CHSTAT_RXS) < 0) {
					dev_err(&spi->dev, "RXS timed out\n");
					goto out;
				}

				if (c == 2 && tx == NULL &&
				    (l & OMAP2_MCSPI_CHCONF_TURBO)) {
					omap2_mcspi_set_enable(spi, 0);
					*rx++ = __raw_readl(rx_reg);
#ifdef VERBOSE
					dev_dbg(&spi->dev, "read-%d %04x\n",
						    word_len, *(rx - 1));
#endif
					if (mcspi_wait_for_reg_bit(chstat_reg,
						OMAP2_MCSPI_CHSTAT_RXS) < 0) {
						dev_err(&spi->dev,
							"RXS timed out\n");
						goto out;
					}
					c = 0;
				} else if (c == 0 && tx == NULL) {
					omap2_mcspi_set_enable(spi, 0);
				}

				*rx++ = __raw_readl(rx_reg);
#ifdef VERBOSE
				dev_dbg(&spi->dev, "read-%d %04x\n",
						word_len, *(rx - 1));
#endif
			}
		} while (c);
	} else if (word_len <= 32) {
		u32		*rx;
		const u32	*tx;

		rx = xfer->rx_buf;
		tx = xfer->tx_buf;
		do {
			c -= 4;
			if (tx != NULL) {
				if (mcspi_wait_for_reg_bit(chstat_reg,
						OMAP2_MCSPI_CHSTAT_TXS) < 0) {
					dev_err(&spi->dev, "TXS timed out\n");
					goto out;
				}
#ifdef VERBOSE
				dev_dbg(&spi->dev, "write-%d %08x\n",
						word_len, *tx);
#endif
				__raw_writel(*tx++, tx_reg);
			}
			if (rx != NULL) {
				if (mcspi_wait_for_reg_bit(chstat_reg,
						OMAP2_MCSPI_CHSTAT_RXS) < 0) {
					dev_err(&spi->dev, "RXS timed out\n");
					goto out;
				}

				if (c == 4 && tx == NULL &&
				    (l & OMAP2_MCSPI_CHCONF_TURBO)) {
					omap2_mcspi_set_enable(spi, 0);
					*rx++ = __raw_readl(rx_reg);
#ifdef VERBOSE
					dev_dbg(&spi->dev, "read-%d %08x\n",
						    word_len, *(rx - 1));
#endif
					if (mcspi_wait_for_reg_bit(chstat_reg,
						OMAP2_MCSPI_CHSTAT_RXS) < 0) {
						dev_err(&spi->dev,
							"RXS timed out\n");
						goto out;
					}
					c = 0;
				} else if (c == 0 && tx == NULL) {
					omap2_mcspi_set_enable(spi, 0);
				}

				*rx++ = __raw_readl(rx_reg);
#ifdef VERBOSE
				dev_dbg(&spi->dev, "read-%d %08x\n",
						word_len, *(rx - 1));
#endif
			}
		} while (c);
	}

	/* for TX_ONLY mode, be sure all words have shifted out */
	if (xfer->rx_buf == NULL) {
		if (mcspi_wait_for_reg_bit(chstat_reg,
				OMAP2_MCSPI_CHSTAT_TXS) < 0) {
			dev_err(&spi->dev, "TXS timed out\n");
		} else if (mcspi_wait_for_reg_bit(chstat_reg,
				OMAP2_MCSPI_CHSTAT_EOT) < 0)
			dev_err(&spi->dev, "EOT timed out\n");
	}
out:
	omap2_mcspi_set_enable(spi, 1);
	return count - c;
}

/* called only when no transfer is active to this device */
static int omap2_mcspi_setup_transfer(struct spi_device *spi,
		struct spi_transfer *t)
{
	struct omap2_mcspi_cs *cs = spi->controller_state;
	struct omap2_mcspi *mcspi;
	struct spi_master *spi_cntrl;
	u32 l = 0, div = 0;
	u8 word_len = spi->bits_per_word;
	u32 speed_hz = spi->max_speed_hz;

	mcspi = spi_master_get_devdata(spi->master);
	spi_cntrl = mcspi->master;

	if (t != NULL && t->bits_per_word)
		word_len = t->bits_per_word;

	cs->word_len = word_len;

	if (t && t->speed_hz)
		speed_hz = t->speed_hz;

	if (speed_hz) {
		while (div <= 15 && (OMAP2_MCSPI_MAX_FREQ / (1 << div))
					> speed_hz)
			div++;
	} else
		div = 15;

	l = mcspi_cached_chconf0(spi);

	/* standard 4-wire master mode:  SCK, MOSI/out, MISO/in, nCS
	 * REVISIT: this controller could support SPI_3WIRE mode.
	 */
	if (mcspi->mcspi_mode == OMAP2_MCSPI_MASTER) {
		l &= ~(OMAP2_MCSPI_CHCONF_IS|OMAP2_MCSPI_CHCONF_DPE1);
		l |= OMAP2_MCSPI_CHCONF_DPE0;
	} else {
		l |= OMAP2_MCSPI_CHCONF_IS;
		l |= OMAP2_MCSPI_CHCONF_DPE1;
		l &= ~OMAP2_MCSPI_CHCONF_DPE0;
	}

	/* wordlength */
	l &= ~OMAP2_MCSPI_CHCONF_WL_MASK;
	l |= (word_len - 1) << 7;

	/* set chipselect polarity; manage with FORCE */
	if (!(spi->mode & SPI_CS_HIGH))
		l |= OMAP2_MCSPI_CHCONF_EPOL;	/* active-low; normal */
	else
		l &= ~OMAP2_MCSPI_CHCONF_EPOL;

	if (mcspi->mcspi_mode == OMAP2_MCSPI_MASTER) {
		/* set clock divisor */
		l &= ~OMAP2_MCSPI_CHCONF_CLKD_MASK;
		l |= div << 2;
	}

	/* set SPI mode 0..3 */
	if (spi->mode & SPI_CPOL)
		l |= OMAP2_MCSPI_CHCONF_POL;
	else
		l &= ~OMAP2_MCSPI_CHCONF_POL;
	if (spi->mode & SPI_CPHA)
		l |= OMAP2_MCSPI_CHCONF_PHA;
	else
		l &= ~OMAP2_MCSPI_CHCONF_PHA;

	mcspi_write_chconf0(spi, l);

	dev_dbg(&spi->dev, "setup: speed %d, sample %s edge, clk %s\n",
			OMAP2_MCSPI_MAX_FREQ / (1 << div),
			(spi->mode & SPI_CPHA) ? "trailing" : "leading",
			(spi->mode & SPI_CPOL) ? "inverted" : "normal");

	return 0;
}

static void omap2_mcspi_dma_rx_callback(int lch, u16 ch_status, void *data)
{
	struct spi_device	*spi = data;
	struct omap2_mcspi	*mcspi;
	struct omap2_mcspi_dma	*mcspi_dma;

	mcspi = spi_master_get_devdata(spi->master);
	mcspi_dma = &(mcspi->dma_channels[spi->chip_select]);

	complete(&mcspi_dma->dma_rx_completion);

	/* We must disable the DMA RX request */
	omap2_mcspi_set_dma_req(spi, 1, 0);
}

static void omap2_mcspi_dma_tx_callback(int lch, u16 ch_status, void *data)
{
	struct spi_device	*spi = data;
	struct omap2_mcspi	*mcspi;
	struct omap2_mcspi_dma	*mcspi_dma;

	mcspi = spi_master_get_devdata(spi->master);
	mcspi_dma = &(mcspi->dma_channels[spi->chip_select]);

	complete(&mcspi_dma->dma_tx_completion);

	/* We must disable the DMA TX request */
	omap2_mcspi_set_dma_req(spi, 0, 0);
}

static int omap2_mcspi_request_dma(struct spi_device *spi)
{
	struct spi_master	*master = spi->master;
	struct omap2_mcspi	*mcspi;
	struct omap2_mcspi_dma	*mcspi_dma;

	mcspi = spi_master_get_devdata(master);
	mcspi_dma = mcspi->dma_channels + spi->chip_select;

	if (omap_request_dma(mcspi_dma->dma_rx_sync_dev, "McSPI RX",
			omap2_mcspi_dma_rx_callback, spi,
			&mcspi_dma->dma_rx_channel)) {
		dev_err(&spi->dev, "no RX DMA channel for McSPI\n");
		return -EAGAIN;
	}

	if (omap_request_dma(mcspi_dma->dma_tx_sync_dev, "McSPI TX",
			omap2_mcspi_dma_tx_callback, spi,
			&mcspi_dma->dma_tx_channel)) {
		omap_free_dma(mcspi_dma->dma_rx_channel);
		mcspi_dma->dma_rx_channel = -1;
		dev_err(&spi->dev, "no TX DMA channel for McSPI\n");
		return -EAGAIN;
	}

	init_completion(&mcspi_dma->dma_rx_completion);
	init_completion(&mcspi_dma->dma_tx_completion);

	return 0;
}

static int omap2_mcspi_setup(struct spi_device *spi)
{
	int			ret;
	struct omap2_mcspi	*mcspi;
	struct omap2_mcspi_dma	*mcspi_dma;
	struct omap2_mcspi_cs	*cs = spi->controller_state;

	if (spi->bits_per_word < 4 || spi->bits_per_word > 32) {
		dev_dbg(&spi->dev, "setup: unsupported %d bit words\n",
			spi->bits_per_word);
		return -EINVAL;
	}

	mcspi = spi_master_get_devdata(spi->master);
	mcspi_dma = &mcspi->dma_channels[spi->chip_select];

	if (!cs) {
		cs = kzalloc(sizeof *cs, GFP_KERNEL);
		if (!cs)
			return -ENOMEM;
		cs->base = mcspi->base + spi->chip_select * 0x14;
		cs->phys = mcspi->phys + spi->chip_select * 0x14;
		cs->chconf0 = 0;
		spi->controller_state = cs;
		/* Link this to context save list */
		list_add_tail(&cs->node,
			&omap2_mcspi_ctx[mcspi->master->bus_num - 1].cs);
	}

	if (mcspi_dma->dma_rx_channel == -1
			|| mcspi_dma->dma_tx_channel == -1) {
		ret = omap2_mcspi_request_dma(spi);
		if (ret < 0)
			return ret;
	}

	if (omap2_mcspi_enable_clocks(mcspi) < 0)
		return -ENODEV;

	ret = omap2_mcspi_setup_transfer(spi, NULL);
	omap2_mcspi_disable_clocks(mcspi);

	return ret;
}

static void omap2_mcspi_cleanup(struct spi_device *spi)
{
	struct omap2_mcspi	*mcspi;
	struct omap2_mcspi_dma	*mcspi_dma;
	struct omap2_mcspi_cs	*cs;

	mcspi = spi_master_get_devdata(spi->master);

	if (spi->controller_state) {
		/* Unlink controller state from context save list */
		cs = spi->controller_state;
		list_del(&cs->node);

		kfree(spi->controller_state);
	}

	if (spi->chip_select < spi->master->num_chipselect) {
		mcspi_dma = &mcspi->dma_channels[spi->chip_select];

		if (mcspi_dma->dma_rx_channel != -1) {
			omap_free_dma(mcspi_dma->dma_rx_channel);
			mcspi_dma->dma_rx_channel = -1;
		}
		if (mcspi_dma->dma_tx_channel != -1) {
			omap_free_dma(mcspi_dma->dma_tx_channel);
			mcspi_dma->dma_tx_channel = -1;
		}
	}
}

static void omap2_mcspi_work(struct work_struct *work)
{
	struct omap2_mcspi	*mcspi;

	mcspi = container_of(work, struct omap2_mcspi, work);

	if (omap2_mcspi_enable_clocks(mcspi) < 0)
		return;

	spin_lock_irq(&mcspi->lock);

	/* We only enable one channel at a time -- the one whose message is
	 * at the head of the queue -- although this controller would gladly
	 * arbitrate among multiple channels.  This corresponds to "single
	 * channel" master mode.  As a side effect, we need to manage the
	 * chipselect with the FORCE bit ... CS != channel enable.
	 */
	while (!list_empty(&mcspi->msg_queue)) {
		struct spi_message		*m;
		struct spi_device		*spi;
		struct spi_transfer		*t = NULL;
		int				cs_active = 0;
		struct omap2_mcspi_cs		*cs;
		struct omap2_mcspi_device_config *cd;
		int				par_override = 0;
		int				status = 0;
		u32				chconf;

		m = container_of(mcspi->msg_queue.next, struct spi_message,
				 queue);

		list_del_init(&m->queue);
		spin_unlock_irq(&mcspi->lock);

		spi = m->spi;
		cs = spi->controller_state;
		cd = spi->controller_data;

		omap2_mcspi_set_enable(spi, 1);
		list_for_each_entry(t, &m->transfers, transfer_list) {
			if (t->tx_buf == NULL && t->rx_buf == NULL && t->len) {
				status = -EINVAL;
				break;
			}
			if (par_override || t->speed_hz || t->bits_per_word) {
				par_override = 1;
				status = omap2_mcspi_setup_transfer(spi, t);
				if (status < 0)
					break;
				if (!t->speed_hz && !t->bits_per_word)
					par_override = 0;
			}

			if ((!cs_active) && (mcspi->force_cs_mode) &&
				(mcspi->mcspi_mode ==
				OMAP2_MCSPI_MASTER)) {

				omap2_mcspi_force_cs(spi, 1);
				cs_active = 1;
			}

			chconf = mcspi_cached_chconf0(spi);
			chconf &= ~OMAP2_MCSPI_CHCONF_TRM_MASK;
			chconf &= ~OMAP2_MCSPI_CHCONF_TURBO;

			if (t->tx_buf == NULL)
				chconf |= OMAP2_MCSPI_CHCONF_TRM_RX_ONLY;
			else if (t->rx_buf == NULL)
				chconf |= OMAP2_MCSPI_CHCONF_TRM_TX_ONLY;

			if (cd && cd->turbo_mode && t->tx_buf == NULL) {
				/* Turbo mode is for more than one word */
				if (t->len > ((cs->word_len + 7) >> 3))
					chconf |= OMAP2_MCSPI_CHCONF_TURBO;
			}

			mcspi_write_chconf0(spi, chconf);

			if (t->len) {
				unsigned	count;
#if 0
// LGE_CHANGE_S jisil.park@lge.com
//LGE_CHANGE_S [david.seo, kibum] 2011-02-05, common : after wakeup,  dma not ready, temp
                                //TI JANGHANLEE 20110205 OFF_MODE_DMA_WA [START]
//                              extern int offmode_enter;               // LGE_CHANGE jisil.park@lge.com
                                int offmode_enter;              // LGE_CHANGE jisil.park@lge.com
                                int dma_sysconfig_cnt;
                                unsigned long dma_sysconfig_value;
                                //TI JANGHANLEE 20110205 OFF_MODE_DMA_WA [END]
//LGE_CHANGE_E [david.seo, kibum] 2011-02-05, common : after wakeup,  dma not ready
#endif /* blocked by et.jo 20111108 */
				/* RX_ONLY mode needs dummy data in TX reg */
				if (t->tx_buf == NULL)
					__raw_writel(0, cs->base
						+ mcspi->regs[OMAP2_MCSPI_TX0]);
#if 0
                                dma_sysconfig_value = omap_readl(0x4a05602c);                           
                                dma_sysconfig_cnt =0;   
                                

                                while((offmode_enter)&&(!dma_sysconfig_value)&&(dma_sysconfig_cnt<20)){ 
                                        mdelay(10);                                     
                                        dma_sysconfig_value = omap_readl(0x4a05602c);
                                        dma_sysconfig_cnt++;
                                        
                                };

                                offmode_enter=0;
#endif /* blocked by et.jo 20111108 */
					if (m->is_dma_mapped ||
					t->len >= DMA_MIN_BYTES ||
					mcspi->dma_mode)

					count = omap2_mcspi_txrx_dma(spi, t);
				else
					count = omap2_mcspi_txrx_pio(spi, t);

				m->actual_length += count;

				if (count != t->len) {
					status = -EIO;
// TODO:[EBS] LGE_UPDATE_S eungbo.shim@lge.com 20110713 For Ril recovery Debugging printk 					
					printk("[%s] [omap2_mcspi status = %d, count = %d , m->length = %d\n", __FUNCTION__, status, count, m->actual_length);
// TODO:[EBS] LGE_UPDATE_E eungbo.shim@lge.com 20110713 For Ril recovery 
					break;
				}
			}

			if (t->delay_usecs)
				udelay(t->delay_usecs);

			/* ignore the "leave it on after last xfer" hint */
			if ((t->cs_change) && (mcspi->force_cs_mode) &&
				(mcspi->mcspi_mode ==
				OMAP2_MCSPI_MASTER)) {

				omap2_mcspi_force_cs(spi, 0);
				cs_active = 0;
			}
		}

		/* Restore defaults if they were overriden */
		if (par_override) {
			par_override = 0;
			status = omap2_mcspi_setup_transfer(spi, NULL);
		}

		if ((cs_active) && (mcspi->force_cs_mode) &&
			(mcspi->mcspi_mode == OMAP2_MCSPI_MASTER))
				omap2_mcspi_force_cs(spi, 0);

		omap2_mcspi_set_enable(spi, 0);

		m->status = status;
		m->complete(m->context);

		spin_lock_irq(&mcspi->lock);
	}

	spin_unlock_irq(&mcspi->lock);

	omap2_mcspi_disable_clocks(mcspi);
}

static int omap2_mcspi_transfer(struct spi_device *spi, struct spi_message *m)
{
	struct omap2_mcspi	*mcspi;
	unsigned long		flags;
	struct spi_transfer	*t;

	m->actual_length = 0;
	m->status = 0;

	mcspi = spi_master_get_devdata(spi->master);

	/* reject invalid messages and transfers */
	if (list_empty(&m->transfers) || !m->complete)
		return -EINVAL;
	list_for_each_entry(t, &m->transfers, transfer_list) {
		const void	*tx_buf = t->tx_buf;
		void		*rx_buf = t->rx_buf;
		unsigned	len = t->len;

		if (t->speed_hz > OMAP2_MCSPI_MAX_FREQ
				|| (len && !(rx_buf || tx_buf))
				|| (t->bits_per_word &&
					(  t->bits_per_word < 4
					|| t->bits_per_word > 32))) {
			dev_dbg(&spi->dev, "transfer: %d Hz, %d %s%s, %d bpw\n",
					t->speed_hz,
					len,
					tx_buf ? "tx" : "",
					rx_buf ? "rx" : "",
					t->bits_per_word);
			return -EINVAL;
		}
		if (t->speed_hz && t->speed_hz < OMAP2_MCSPI_MAX_FREQ/(1<<16)) {
			dev_dbg(&spi->dev, "%d Hz max exceeds %d\n",
					t->speed_hz,
					OMAP2_MCSPI_MAX_FREQ/(1<<16));
			return -EINVAL;
		}

		if (mcspi->fifo_depth != 0) {
			if ((len % mcspi->fifo_depth) != 0)
				return -EINVAL;
		}

		/* Ignore DMA_MIN_BYTES check if dma only mode is set */
		if (m->is_dma_mapped || ((len < DMA_MIN_BYTES) &&
						(!mcspi->dma_mode)))
			continue;

		/* Do DMA mapping "early" for better error reporting and
		 * dcache use.  Note that if dma_unmap_single() ever starts
		 * to do real work on ARM, we'd need to clean up mappings
		 * for previous transfers on *ALL* exits of this loop...
		 */
		if (tx_buf != NULL) {
			t->tx_dma = dma_map_single(&spi->dev, (void *) tx_buf,
					len, DMA_TO_DEVICE);
			if (dma_mapping_error(&spi->dev, t->tx_dma)) {
				dev_dbg(&spi->dev, "dma %cX %d bytes error\n",
						'T', len);
				return -EINVAL;
			}
		}
		if (rx_buf != NULL) {
			t->rx_dma = dma_map_single(&spi->dev, rx_buf, t->len,
					DMA_FROM_DEVICE);
			if (dma_mapping_error(&spi->dev, t->rx_dma)) {
				dev_dbg(&spi->dev, "dma %cX %d bytes error\n",
						'R', len);
				if (tx_buf != NULL)
					dma_unmap_single(NULL, t->tx_dma,
							len, DMA_TO_DEVICE);
				return -EINVAL;
			}
		}
	}

	spin_lock_irqsave(&mcspi->lock, flags);
	list_add_tail(&m->queue, &mcspi->msg_queue);
#ifdef CONFIG_LGE_SPI
	queue_work(mcspi->wq, &mcspi->work);
#else
	queue_work(omap2_mcspi_wq, &mcspi->work);
#endif
	spin_unlock_irqrestore(&mcspi->lock, flags);

	return 0;
}

static int __init omap2_mcspi_reset(struct omap2_mcspi *mcspi)
{
	struct spi_master	*master = mcspi->master;
	u32			tmp;

	if (omap2_mcspi_enable_clocks(mcspi) < 0)
		return -1;

	tmp = OMAP2_MCSPI_WAKEUPENABLE_WKEN;
	mcspi_write_reg(master, OMAP2_MCSPI_WAKEUPENABLE, tmp);
	omap2_mcspi_ctx[master->bus_num - 1].wakeupenable = tmp;

	if (mcspi->mcspi_mode == OMAP2_MCSPI_MASTER)
		omap2_mcspi_set_master_mode(master);
	else
		omap2_mcspi_set_slave_mode(master);

	omap2_mcspi_disable_clocks(mcspi);
	return 0;
}

static int omap_mcspi_runtime_suspend(struct device *dev)
{
	//printk("[EBS] %s\n", __FUNCTION__);	
	return 0;
}

static int omap_mcspi_runtime_resume(struct device *dev)
{
	struct omap2_mcspi	*mcspi;
	struct spi_master	*master;

	master = dev_get_drvdata(dev);
	mcspi = spi_master_get_devdata(master);
	omap2_mcspi_restore_ctx(mcspi);
	//printk("[EBS] %s\n", __FUNCTION__);
	return 0;
}

static int __init omap2_mcspi_probe(struct platform_device *pdev)
{
	struct spi_master	*master;
	struct omap2_mcspi_platform_config *pdata =
		(struct omap2_mcspi_platform_config *)pdev->dev.platform_data;
	struct omap2_mcspi	*mcspi;
	struct resource		*r;
	int			status = 0, i;


//	printk("!!!!!!!!!!!!!!!!!!!omap2_mcspi_probe [Start]\n");
	
	
	master = spi_alloc_master(&pdev->dev, sizeof *mcspi);
	if (master == NULL) {
		dev_dbg(&pdev->dev, "master allocation failed\n");
		return -ENOMEM;
	}

	/* the spi->mode bits understood by this driver: */
	master->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH;

	if (pdev->id != -1)
		master->bus_num = pdev->id;

	master->setup = omap2_mcspi_setup;
	master->transfer = omap2_mcspi_transfer;
	master->cleanup = omap2_mcspi_cleanup;
	master->num_chipselect = pdata->num_cs;

	dev_set_drvdata(&pdev->dev, master);

	mcspi = spi_master_get_devdata(master);
	mcspi->master = master;

	
	mcspi->mcspi_mode = pdata->mode; //hyunh0.cho
	
	mcspi->dma_mode = pdata->dma_mode;
	mcspi->force_cs_mode = pdata->force_cs_mode;
	mcspi->regs = pdata->regs_data;

	if (pdata->fifo_depth <= OMAP2_MCSPI_MAX_FIFODEPTH)
		mcspi->fifo_depth = pdata->fifo_depth;
	else {
		mcspi->fifo_depth = 0;
		dev_dbg(&pdev->dev, "Invalid fifo depth specified\n");
	}

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (r == NULL) {
		status = -ENODEV;
		goto err1;
	}
	if (!request_mem_region(r->start, (r->end - r->start) + 1,
			dev_name(&pdev->dev))) {
		status = -EBUSY;
		goto err1;
	}

	mcspi->phys = r->start;
	mcspi->base = ioremap(r->start, r->end - r->start + 1);
	if (!mcspi->base) {
		dev_dbg(&pdev->dev, "can't ioremap MCSPI\n");
		status = -ENOMEM;
		goto err2;
	}

	mcspi->dev = &pdev->dev;
	INIT_WORK(&mcspi->work, omap2_mcspi_work);

// LGE_UPDATE_S eungbo.shim@lge.com [EBS] for make multiple SPI thread 	
#ifdef CONFIG_LGE_SPI
	/* make multiple workqueue for multiple spi */
	sprintf(mcspi->name, "%s_wq%d", pdev->name, pdev->id);
	mcspi->wq = create_workqueue(mcspi->name);//create_singlethread_workqueue create_rt_workqueue->create_workqueue
#endif
// LGE_UPDATE_E eungbo.shim@lge.com [EBS] for make multiple SPI thread	

	spin_lock_init(&mcspi->lock);
	INIT_LIST_HEAD(&mcspi->msg_queue);
	INIT_LIST_HEAD(&omap2_mcspi_ctx[master->bus_num - 1].cs);

	mcspi->dma_channels = kcalloc(master->num_chipselect,
			sizeof(struct omap2_mcspi_dma),
			GFP_KERNEL);

	if (mcspi->dma_channels == NULL)
		goto err2;

	for (i = 0; i < pdata->num_cs; i++) {
		char dma_ch_name[14];
		struct resource *dma_res;

		sprintf(dma_ch_name, "rx%d", i);
		dma_res = platform_get_resource_byname(pdev, IORESOURCE_DMA,
							dma_ch_name);
		if (!dma_res) {
			dev_dbg(&pdev->dev, "cannot get DMA RX channel\n");
			status = -ENODEV;
			break;
		}
		mcspi->dma_channels[i].dma_rx_channel = -1;
		mcspi->dma_channels[i].dma_rx_sync_dev = dma_res->start;

		sprintf(dma_ch_name, "tx%d", i);
		dma_res = platform_get_resource_byname(pdev, IORESOURCE_DMA,
							dma_ch_name);
		if (!dma_res) {
			dev_dbg(&pdev->dev, "cannot get DMA TX channel\n");
			status = -ENODEV;
			break;
		}
		mcspi->dma_channels[i].dma_tx_channel = -1;
		mcspi->dma_channels[i].dma_tx_sync_dev = dma_res->start;
	}
//	printk("!!!!!!!!!!!!!!!!!!!omap2_mcspi_probe [END]\n");

	pm_runtime_enable(&pdev->dev);
	if (status || omap2_mcspi_reset(mcspi) < 0)
		goto err3;

	status = spi_register_master(master);
	if (status < 0)
		goto err4;

	return status;
err4:
	spi_master_put(master);
err3:
	kfree(mcspi->dma_channels);
err2:
	release_mem_region(r->start, (r->end - r->start) + 1);
	iounmap(mcspi->base);
err1:
	return status;
}

static int __exit omap2_mcspi_remove(struct platform_device *pdev)
{
	struct spi_master	*master;
	struct omap2_mcspi	*mcspi;
	struct omap2_mcspi_dma	*dma_channels;
	struct resource		*r;
	void __iomem *base;

	master = dev_get_drvdata(&pdev->dev);
	mcspi = spi_master_get_devdata(master);
	dma_channels = mcspi->dma_channels;

	omap2_mcspi_disable_clocks(mcspi);

#ifdef CONFIG_LGE_SPI
	destroy_workqueue(mcspi->wq);
#endif
	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	release_mem_region(r->start, (r->end - r->start) + 1);

	base = mcspi->base;
	spi_unregister_master(master);
	iounmap(base);
	kfree(dma_channels);

	return 0;
}

/* work with hotplug and coldplug */
MODULE_ALIAS("platform:omap2_mcspi");

static const struct dev_pm_ops omap_mcspi_dev_pm_ops = {
	.runtime_suspend = omap_mcspi_runtime_suspend,
	.runtime_resume	= omap_mcspi_runtime_resume,
};

static struct platform_driver omap2_mcspi_driver = {
	.driver = {
		.name	= "omap2_mcspi_slave",
		.owner	= THIS_MODULE,
		.pm	= &omap_mcspi_dev_pm_ops,
	},
	.remove =	__exit_p(omap2_mcspi_remove),
};

static int __init omap2_mcspi_init(void)
{
#ifdef CONFIG_LGE_SPI
#else
	omap2_mcspi_wq = create_singlethread_workqueue(
				omap2_mcspi_driver.driver.name);

	if (omap2_mcspi_wq == NULL)
		return -1;
#endif

	/* Request Power Management Quality Service for McSPI4 */
	pm_qos_add_request(&pm_qos_handle_for_spi, PM_QOS_CPU_DMA_LATENCY, PM_QOS_DEFAULT_VALUE);
	
	return platform_driver_probe(&omap2_mcspi_driver, omap2_mcspi_probe);
}
subsys_initcall(omap2_mcspi_init);

static void __exit omap2_mcspi_exit(void)
{
	pm_qos_remove_request(&pm_qos_handle_for_spi);
	platform_driver_unregister(&omap2_mcspi_driver);

#ifdef CONFIG_LGE_SPI
#else
	destroy_workqueue(omap2_mcspi_wq);
#endif
}
module_exit(omap2_mcspi_exit);

MODULE_LICENSE("GPL");
