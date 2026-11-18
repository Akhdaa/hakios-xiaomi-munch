/*
 * The touch screen of the POCO F4 (munch): a FocalTech FT3658U on the SPI bus of serial engine 4
 * of the first QUP (0x990000), as a source of touches for /dev/hakios-touch (sys/hakios.input.h).
 * Only with boot-arg touchscreen=1, and only in a munch kernel (devices/munch/configs: CONFIG_HAKIOS_MUNCH_TOUCH):
 * every address below belongs to this phone. The way of doing it is drivers/input/hakios.goodix.c's FocalTech path; the numbers are
 * this phone's:
 *   power    vcc_i2c is LDO1 of the PM8150A ("ldoc1", 1.8 V), vdd its LDO10 ("ldoc10", 3.2 V, as
 *            the vendor driver sets it), both through RPMh; nothing else uses either
 *   clocks   QUP0's core, AHB and serial engine 4 clocks, by vote in the global clock controller
 *   pins     GPIO 8-11 to the serial engine (function 1, "qup4", the TLMM's north tile), reset on
 *            GPIO 33 and the interrupt line on GPIO 39 (south tile)
 *   SPI      the serial engine ("GENI") in FIFO mode, polled; or the same four pins driven by hand
 *   chip     its firmware is in its own flash (not an in-cell part: nothing is downloaded, nothing
 *            is written to it); after a reset its ID reads 0x56 0x52, and the touch report is read
 *            at register 0x01 with FTS 5.x SPI framing (0xa0 = read with CRC-16/Kermit)
 * Android's driver (the e404 kernel's focaltech_3658u) reads the chip on the line's falling edge, with the bus at 6 MHz (dt
 * spi-max-frequency). No interrupt is taken here, but the line is looked at every millisecond (touch_poll_us=N), which is
 * as good for a chip that reports at most 240 times a second.
 *
 * Numbers: the phone's own device tree (spi@990000/focaltech@0, qupv3_se4_spi_active, ts_active,
 * rpmh-regulator-ldoc1/ldoc10), and its Android kernel's drivers/clk/qcom/gcc-kona.c,
 * drivers/pinctrl/qcom/pinctrl-kona.c and drivers/input/touchscreen/focaltech_3658u/.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

static vm_offset_t tlmm_north, tlmm_south, gcc, se, rsc, cmd_db;
static uint32_t flip;

static struct {
	uint32_t polls, reports, bad, rolled, touches, last_x, last_y;
} seen;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- pins (TLMM: three tiles; a pin's registers are at its tile + 0x1000 * its number) ---------- */

#define TLMM_NORTH_PHYS         0x0f900000UL
#define TLMM_NORTH_SIZE         0x10000UL       /* GPIO 0-15 */
#define TLMM_SOUTH_PHYS         0x0f500000UL
#define TLMM_SOUTH_SIZE         0x30000UL       /* up to GPIO 47 */
#define PULL_NONE               0
#define PULL_UP                 3

#define PIN_MISO                8
#define PIN_MOSI                9
#define PIN_CLK                 10
#define PIN_CS                  11
#define PIN_RESET               33      /* low = in reset */
#define PIN_INT                 39      /* low = the chip has a report */
#define PIN_FUNCTION_SPI        1       /* "qup4" */

static vm_offset_t
gpio_cfg(unsigned pin)
{
	return (pin < 16 ? tlmm_north : tlmm_south) + 0x1000UL * pin;     /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
}

static vm_offset_t
gpio_io(unsigned pin)
{
	return gpio_cfg(pin) + 4;                                           /* bit 0 in, bit 1 out */
}

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(gpio_cfg(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(gpio_io(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(gpio_io(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
}

/* ---- the supplies (RPMh; the same command slots and database as on redwood) --------------------- */

#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x18220000UL    /* the application processor's "DRV 2" */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))           /* command slot set 0, an "active" one */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("munch-touch: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1],
		    db->magic[2], db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value, const char *what)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	printf("munch-touch: RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
	return false;
}

/* Send up to three register writes to RPMh and wait until they are done. */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	bool done;

	/* the slot set must be idle, and switched off the way the hardware wants it: trigger, then enable */
	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			printf("munch-touch: RPMh: the command slots stay busy\n");
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);        /* the status bit follows the enable; the interrupt itself stays masked */
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1, "the request");
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

/*
 * A regulator voted for through RPMh, by its name in the command database: on at `millivolts`
 * (with mode `mode`, 0: the mode left alone) or off. Our vote is one of several -- RPMh keeps a
 * supply on as long as anybody wants it -- so "off" only takes back what we asked for. Also for
 * the Wi-Fi's start (hakios_munch_pcie.c), which shares these command slots with us: one at a time.
 */
extern lck_mtx_t hakios_rsc_lock;              /* (drivers/core/hakios.platform.c; the audio DSP's rails come through this TCS too) */

bool
hakios_munch_vreg(const char *name, uint32_t millivolts, bool on, uint32_t mode)
{
	uint32_t base, address[3], data[3];
	unsigned count = 0;
	bool ok;

	lck_mtx_lock(&hakios_rsc_lock);
	if (rsc == 0) {
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	}
	base = rpmh_address(name);
	/* addresses of regulators ("VRM") have 4 in bits 19:16; anything else is not what we are looking for */
	if (base == 0 || (base >> 16 & 0xf) != 4) {
		printf("munch: RPMh: no regulator \"%s\" (address %#x)\n", name, base);
		lck_mtx_unlock(&hakios_rsc_lock);
		return false;
	}
	if (on) {
		address[count] = base + VRM_VOLTAGE;
		data[count++] = millivolts;
	}
	address[count] = base + VRM_ENABLE;
	data[count++] = on;
	if (on && mode != 0) {
		address[count] = base + VRM_MODE;
		data[count++] = mode;
	}
	ok = rpmh_write(address, data, count);
	lck_mtx_unlock(&hakios_rsc_lock);
	if (ok) {
		printf("munch: %s %s, RPMh %#x\n", name, on ? "on" : "off (our vote taken back)", base);
	}
	return ok;
}

/* One LDO of the PM8150A on at `millivolts`, in high-power mode. */
static bool
ldo_on(const char *name, uint32_t millivolts)
{
	return hakios_munch_vreg(name, millivolts, true, VRM_MODE_LDO_HPM);
}

/* ---- clocks (GCC; offsets of gcc-kona.c) ------------------------------------------------------------ */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
#define GCC_VOTE                (gcc + 0x52008)         /* the application processor's votes */
#define VOTE_QUP0_M_AHB         (1u << 6)
#define VOTE_QUP0_S_AHB         (1u << 7)
#define VOTE_QUP0_CORE          (1u << 8)
#define VOTE_QUP0_CORE_2X       (1u << 9)
#define VOTE_QUP0_SE4           (1u << 14)
#define GCC_QUP0_M_AHB_CBCR     (gcc + 0x17004)         /* bit 31: the clock is off */
#define GCC_QUP0_S_AHB_CBCR     (gcc + 0x17008)
#define GCC_QUP0_SE4_CBCR       (gcc + 0x174cc)
#define GCC_QUP0_SE4_CMD        (gcc + 0x174d0)         /* bit 0: take the new configuration */
#define GCC_QUP0_SE4_CFG        (gcc + 0x174d4)         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define GCC_QUP0_SE4_DFSR       (gcc + 0x174e4)         /* bit 0: the engine chooses among preset rates itself */
#define GCC_QUP0_SE4_PERF_CFG(l) (gcc + 0x174ec + 4 * (l))
#define GCC_QUP0_SE4_PERF_M(l)  (gcc + 0x1752c + 4 * (l))
#define GCC_QUP0_SE4_PERF_N(l)  (gcc + 0x1756c + 4 * (l))
#define CLOCK_OFF               (1u << 31)
#define CLOCK_HW_CTL            (1u << 1)       /* the hardware gates it by itself: off while idle */
#define XO_HZ                   19200000

/*
 * A branch clock is on, or gated by the hardware between transfers: QUP0's slave AHB clock reads
 * 0x80000002 here (off and hardware-gated) with the serial engine working.
 */
static bool
clock_running(uint32_t cbcr)
{
	return !(cbcr & CLOCK_OFF) || (cbcr & CLOCK_HW_CTL);
}

static uint32_t se_source_hz, se_clock_select;

/* parent_map_0 of gcc-kona.c: 0 the crystal, 1 GPLL0 (600 MHz), 6 GPLL0's even output (300 MHz) */
static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static bool
clocks_on(void)
{
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP0_CORE_2X | VOTE_QUP0_CORE | VOTE_QUP0_M_AHB | VOTE_QUP0_S_AHB);
	if (rd(GCC_QUP0_SE4_DFSR) & 1) {
		/* preset rates: the lowest that is at least 4 MHz */
		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_QUP0_SE4_PERF_CFG(level)), rd(GCC_QUP0_SE4_PERF_M(level)),
			    rd(GCC_QUP0_SE4_PERF_N(level)));

			if (hz >= 4000000 && (se_source_hz == 0 || hz < se_source_hz)) {
				se_source_hz = hz;
				se_clock_select = level;
			}
		}
		if (se_source_hz == 0) {
			printf("munch-touch: no usable preset clock rate\n");
			return false;
		}
	} else {
		/* the crystal, undivided: nothing else has to be running */
		wr(GCC_QUP0_SE4_CFG, (rd(GCC_QUP0_SE4_CFG) & ~0x371fu) | 1);
		wr(GCC_QUP0_SE4_CMD, rd(GCC_QUP0_SE4_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_QUP0_SE4_CMD) & 1); i++) {
			sleep_us(10);
		}
		if (rd(GCC_QUP0_SE4_CMD) & 1) {
			printf("munch-touch: the serial engine's clock does not take its configuration (%#x)\n",
			    rd(GCC_QUP0_SE4_CMD));
			return false;
		}
		se_source_hz = XO_HZ;
		se_clock_select = 0;
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP0_SE4);
	for (int i = 0; i < 1000; i++) {
		if (clock_running(rd(GCC_QUP0_M_AHB_CBCR)) && clock_running(rd(GCC_QUP0_S_AHB_CBCR)) &&
		    clock_running(rd(GCC_QUP0_SE4_CBCR))) {
			return true;
		}
		sleep_us(10);
	}
	printf("munch-touch: clocks stay off: %#x %#x %#x\n", rd(GCC_QUP0_M_AHB_CBCR), rd(GCC_QUP0_S_AHB_CBCR),
	    rd(GCC_QUP0_SE4_CBCR));
	return false;
}

/* ---- SPI through the serial engine (GENI; the same block as redwood's, at another address) ------- */

#define SE_PHYS                 0x00990000UL
#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit 0 enable, [15:4] divider */
#define GENI_IF_DISABLE_RO      (se + 0x064)    /* bit 0: no FIFO interface */
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 1 = SPI */
#define GENI_CLK_SEL            (se + 0x07c)
#define SPI_CPHA                (se + 0x224)
#define SPI_LOOPBACK            (se + 0x22c)
#define SPI_CPOL                (se + 0x230)
#define SPI_DEMUX_OUTPUT_INV    (se + 0x24c)
#define SPI_DEMUX_SEL           (se + 0x250)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define SPI_TRANS_CFG           (se + 0x25c)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define SPI_WORD_LEN            (se + 0x268)
#define SPI_TX_TRANS_LEN        (se + 0x26c)
#define SPI_RX_TRANS_LEN        (se + 0x270)
#define SPI_DELAY_COUNTERS      (se + 0x278)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)    /* [31:27] operation */
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)    /* [27:0] words */
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* bit 31 last, [30:28] bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] FIFO depth in words */
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define SPI_FULL_DUPLEX         (3u << 27)

static uint32_t fifo_depth;

/* 0 = ready; otherwise why the engine cannot be used */
static const char *
geni_spi_init(uint32_t hz)
{
	uint32_t divider;

	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 1) {
		return "the serial engine has no SPI firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	/* the status bits we poll follow these enables; the interrupt itself stays masked at the GIC */
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);

	/* mode 0, chip select 0, 8 bits, most significant first, four bytes in a FIFO word */
	wr(SPI_LOOPBACK, 0);
	wr(SPI_DEMUX_SEL, 0);
	wr(SPI_CPHA, 0);
	wr(SPI_CPOL, 0);
	wr(SPI_DEMUX_OUTPUT_INV, 0);
	wr(SPI_TRANS_CFG, rd(SPI_TRANS_CFG) & ~2u);
	wr(SPI_DELAY_COUNTERS, 0);
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_BYTE_GRAN, 0);
	wr(SPI_WORD_LEN, 8 - 4);

	divider = (se_source_hz + hz - 1) / hz;
	wr(GENI_CLK_SEL, se_clock_select & 7);
	wr(GENI_SER_M_CLK_CFG, (divider ? divider : 1) << 4 | 1);
	return NULL;
}

static void
geni_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, bytes = words * 4;

	if (status & 0x80000000u) {
		uint32_t valid = status >> 28 & 7;

		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(GENI_RX_FIFO);

		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				if (rx) {
					rx[*received] = (uint8_t)(word >> (8 * j));
				}
				(*received)++;
			}
		}
	}
}

static void
geni_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(100);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(100);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* One transfer with the chip selected throughout: `length` bytes out (zeros without tx) and as many in. */
static bool
geni_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	static unsigned complaints;
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline;

	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			geni_recover();
			break;
		}
		sleep_us(100);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SPI_TX_TRANS_LEN, length);
	wr(SPI_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, SPI_FULL_DUPLEX);
	clock_interval_to_deadline(200, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		while (sent < length && (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < fifo_depth) {
			uint32_t word = 0;

			for (int j = 0; j < 4 && sent < length; j++, sent++) {
				word |= (uint32_t)(tx ? tx[sent] : 0) << (8 * j);
			}
			wr(GENI_TX_FIFO, word);
		}
		status = rd(GENI_M_IRQ_STATUS);
		geni_drain(rx, length, &received);
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			geni_drain(rx, length, &received);
			wr(GENI_M_IRQ_CLEAR, status);
			if (received == length) {
				return true;
			}
			break;
		}
	}
	if (complaints++ < 8) {
		printf("munch-touch: SPI transfer of %u bytes failed: status %#x, sent %u, received %u\n", length, status,
		    sent, received);
	}
	geni_recover();
	return false;
}

/* ---- SPI by hand, on the same pins ----------------------------------------------------------------- */

static unsigned bitbang_in = PIN_MISO, bitbang_out = PIN_MOSI;

static void
bitbang_init(void)
{
	gpio_output(PIN_CS, true, PULL_NONE, 6);
	gpio_output(PIN_CLK, false, PULL_NONE, 6);
	gpio_output(bitbang_out, false, PULL_NONE, 6);
	gpio_configure(bitbang_in, 0, PULL_NONE, 2, false);
}

static bool
bitbang_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	gpio_set(PIN_CS, false);
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx ? tx[i] : 0, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(bitbang_out, out >> bit & 1);
			gpio_set(PIN_CLK, true);
			in = (uint8_t)(in << 1 | gpio_get(bitbang_in));
			gpio_set(PIN_CLK, false);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	gpio_set(PIN_CS, true);
	return true;
}

static bool (*spi_transfer)(const uint8_t *tx, uint8_t *rx, uint32_t length);

/* ---- the FT3658U --------------------------------------------------------------------------------------- */

/*
 * FTS 5.x SPI framing, as focaltech_3658u/focaltech_spi.c (the same as redwood's FT3680):
 *   TX[0] register, TX[1] 0xa0 (read, with CRC) or 0x00 (write), TX[2..3] data length big-endian,
 *   TX[4..6] three dummy bytes, then the data; a read ends in the data's CRC-16/Kermit, little-endian.
 *   RX[3] is the chip's status: bit 7 busy, bit 5 a CRC error.
 */
#define FOCALTECH_READ_CTRL         0xa0
#define FOCALTECH_WRITE_CTRL        0x00
#define FOCALTECH_CHIP_ID_H         0xa3        /* 0x56 ... */
#define FOCALTECH_CHIP_ID_L         0x9f        /* ... 0x52: the FT3658U running its firmware */
#define FOCALTECH_TOUCH_DATA        0x01
#define FOCALTECH_MAX_TOUCHES       10
#define FOCALTECH_TOUCH_REPORT_LEN  (2 + 6 * FOCALTECH_MAX_TOUCHES)
#define FOCALTECH_MAX_TRANSFER      (7 + FOCALTECH_TOUCH_REPORT_LEN + 2)

static uint8_t focaltech_tx[FOCALTECH_MAX_TRANSFER], focaltech_rx[FOCALTECH_MAX_TRANSFER];

/* CRC-16/Kermit (polynomial 0x8408 reflected, initial 0xffff): the vendor's crckermit(). */
static uint16_t
focaltech_crc(const uint8_t *data, uint32_t length)
{
	uint16_t crc = 0xffff;

	for (uint32_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
		}
	}
	return crc;
}

static bool
focaltech_read(uint8_t address, uint8_t *data, uint32_t length)
{
	const uint32_t data_offset = 7;
	uint32_t transfer_length = data_offset + length + 2;

	if (length == 0 || transfer_length > sizeof(focaltech_tx)) {
		return false;
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		memset(focaltech_tx, 0, transfer_length);
		memset(focaltech_rx, 0, transfer_length);
		focaltech_tx[0] = address;
		focaltech_tx[1] = FOCALTECH_READ_CTRL;
		focaltech_tx[2] = (uint8_t)(length >> 8);
		focaltech_tx[3] = (uint8_t)length;
		if (spi_transfer(focaltech_tx, focaltech_rx, transfer_length) && !(focaltech_rx[3] & 0xa0)) {
			uint16_t expected = focaltech_crc(focaltech_rx + data_offset, length);
			uint16_t received = (uint16_t)(focaltech_rx[data_offset + length] |
			    focaltech_rx[data_offset + length + 1] << 8);

			if (expected == received) {
				memcpy(data, focaltech_rx + data_offset, length);
				sleep_us(150);          /* the vendor driver's least time with chip select high */
				return true;
			}
		}
		sleep_us(150);
	}
	return false;
}

static void
touch_reset(void)
{
	gpio_set(PIN_RESET, false);
	sleep_ms(1);
	gpio_set(PIN_RESET, true);
	sleep_ms(200);                  /* fts_reset_proc(200) */
}

/* Is the chip there and running its firmware? (vendor fts_wait_tp_to_valid: 1 s at most) */
static bool
focaltech_probe(bool verbose)
{
	uint8_t high = 0, low = 0;

	touch_reset();
	for (int attempt = 0; attempt < 5; attempt++) {
		if (focaltech_read(FOCALTECH_CHIP_ID_H, &high, 1) && focaltech_read(FOCALTECH_CHIP_ID_L, &low, 1) &&
		    high == 0x56 && low == 0x52) {
			printf("munch-touch: FT3658U answers (ID %02x %02x) after %d attempt(s)\n", high, low, attempt + 1);
			return true;
		}
		sleep_ms(200);
	}
	if (verbose) {
		printf("munch-touch: no FT3658U: ID reads %02x %02x\n", high, low);
	}
	return false;
}

/* ---- its touch reports ----------------------------------------------------------------------------- */

/*
 * The report at register 0x01 (focaltech_3658u/focaltech_core.h, FTS_TOUCH_*_POS less the register byte):
 *   report[1]           bits 3..0: how many fingers
 *   report[2 + 6*i + 0] bits 7..6: event (0 down, 1 up, 2 contact); bits 3..0: x bits 15..12
 *   report[2 + 6*i + 1] x bits 11..4
 *   report[2 + 6*i + 2] bits 7..4: finger id (0xf: no more); bits 3..0: y bits 15..12
 *   report[2 + 6*i + 3] y bits 11..4
 *   report[2 + 6*i + 4] bits 7..4: x bits 3..0; bits 3..0: y bits 3..0
 *   report[2 + 6*i + 5] bits 7..4: the touch's area
 * The 16-bit values are 1.6 times the panel's 10800 x 24000 (the vendor driver takes 10/16 of them).
 */
#define FOCALTECH_EVENT_DOWN        0
#define FOCALTECH_EVENT_UP          1
#define FOCALTECH_EVENT_CONTACT     2

static const uint32_t focaltech_max_x = 10800, focaltech_max_y = 24000;

static struct {
	bool down, seen;
	uint32_t x, y;
} contact[FOCALTECH_MAX_TOUCHES];

static void
contact_update(uint32_t id, uint32_t event, uint32_t x, uint32_t y)
{
	static unsigned shown;

	if (event == FOCALTECH_EVENT_UP) {
		contact[id].seen = true;
		if (contact[id].down) {
			contact[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
		return;
	}
	if (x >= focaltech_max_x) {
		x = focaltech_max_x - 1;
	}
	if (y >= focaltech_max_y) {
		y = focaltech_max_y - 1;
	}
	seen.touches++;
	switch (flip) {
	case 1: x = focaltech_max_x - 1 - x; break;
	case 2: y = focaltech_max_y - 1 - y; break;
	case 3: x = focaltech_max_x - 1 - x; y = focaltech_max_y - 1 - y; break;
	}
	x = (uint32_t)(((uint64_t)x * 65535) / (focaltech_max_x - 1));
	y = (uint32_t)(((uint64_t)y * 65535) / (focaltech_max_y - 1));
	seen.last_x = x;
	seen.last_y = y;
	contact[id].seen = true;
	/*
	 * Thumbs typing in turn: between two looks one finger has left and the next has come down under the same id, and
	 * the chip says "down", or goes on saying "contact" with the place jumped. That is a key let go and another one
	 * pressed, not a finger moved: as a move the first key stayed held and the second was never pressed. (A jump of an
	 * eighth of the screen's width and height between two looks is no finger.)
	 */
	if (contact[id].down) {
		uint32_t far_x = x > contact[id].x ? x - contact[id].x : contact[id].x - x;
		uint32_t far_y = y > contact[id].y ? y - contact[id].y : contact[id].y - y;

		/*
		 * "Down" again where the finger already is, is the same report read again: with a finger down every look
		 * reads one now (every millisecond), faster than the chip makes them, and as a new finger it lifted the old one
		 * a millisecond after it landed, every time -- no swipe was a swipe (6 November). A "down" a 64th of the screen
		 * away is another key.
		 */
		if ((event == FOCALTECH_EVENT_DOWN && (far_x > 65535 / 64 || far_y > 65535 / 64)) || far_x > 65535 / 8 ||
		    far_y > 65535 / 8) {
			hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
			contact[id].down = false;
			seen.rolled++;
		}
	}
	if (!contact[id].down) {
		contact[id].down = true;
		hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
	} else if (contact[id].x != x || contact[id].y != y) {
		hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
	}
	contact[id].x = x;
	contact[id].y = y;
	if (shown++ < 8) {
		printf("munch-touch: finger %u %s at %u, %u\n", id, event == FOCALTECH_EVENT_DOWN ? "down" : "move", x, y);
	}
}

static void
focaltech_report(const uint8_t *report)
{
	uint32_t count = report[1] & 0x0f;

	/* all 0xff: the chip has lost its state (the vendor driver resets it) */
	if (count == 0x0f && report[1] == 0xff && report[2] == 0xff && report[3] == 0xff) {
		return;
	}
	for (uint32_t i = 0; i < FOCALTECH_MAX_TOUCHES; i++) {
		contact[i].seen = false;
	}
	for (uint32_t i = 0; i < FOCALTECH_MAX_TOUCHES; i++) {
		const uint8_t *touch = report + 2 + 6 * i;
		uint32_t id = touch[2] >> 4, event, x, y;

		if (id >= FOCALTECH_MAX_TOUCHES) {
			break;
		}
		event = touch[0] >> 6;
		x = ((uint32_t)(touch[0] & 0x0f) << 12 | (uint32_t)touch[1] << 4 | touch[4] >> 4) * 10 / 16;
		y = ((uint32_t)(touch[2] & 0x0f) << 12 | (uint32_t)touch[3] << 4 | (touch[4] & 0x0f)) * 10 / 16;
		contact_update(id, event, x, y);
	}
	/* fingers not in this report are up */
	for (uint32_t i = 0; i < FOCALTECH_MAX_TOUCHES; i++) {
		if (!contact[i].seen && contact[i].down) {
			contact[i].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, i, contact[i].x, contact[i].y);
		}
	}
}

/* Is a finger on the screen, as far as the reports have said? */
static bool
touching(void)
{
	for (uint32_t i = 0; i < FOCALTECH_MAX_TOUCHES; i++) {
		if (contact[i].down) {
			return true;
		}
	}
	return false;
}

static void
touch_poll(void)
{
	static uint8_t report[FOCALTECH_TOUCH_REPORT_LEN];
	static unsigned bad;

	seen.polls++;
	if (!focaltech_read(FOCALTECH_TOUCH_DATA, report, sizeof(report))) {
		seen.bad++;
		if (bad++ < 8) {
			printf("munch-touch: the touch report could not be read\n");
		}
		return;
	}
	seen.reports++;
	focaltech_report(report);
}

/* ---- all of it together ---------------------------------------------------------------------------- */

static void
show_state(const char *when)
{
	printf("munch-touch: %s: votes %#x, clocks %#x %#x %#x, rate cfg %#x dfs %#x; pins miso/mosi/clk/cs cfg %#x %#x %#x %#x,"
	    " reset %#x/%u, int %#x/%u\n", when, rd(GCC_VOTE), rd(GCC_QUP0_M_AHB_CBCR), rd(GCC_QUP0_S_AHB_CBCR),
	    rd(GCC_QUP0_SE4_CBCR), rd(GCC_QUP0_SE4_CFG), rd(GCC_QUP0_SE4_DFSR), rd(gpio_cfg(PIN_MISO)), rd(gpio_cfg(PIN_MOSI)),
	    rd(gpio_cfg(PIN_CLK)), rd(gpio_cfg(PIN_CS)), rd(gpio_cfg(PIN_RESET)), rd(gpio_io(PIN_RESET)), rd(gpio_cfg(PIN_INT)),
	    rd(gpio_io(PIN_INT)));
}

/* Power the chip up, in the vendor driver's order (fts_power_source_ctrl): reset low, vcc_i2c, vdd. */
static void
power_on(void)
{
	gpio_output(PIN_RESET, false, PULL_UP, 8);
	gpio_configure(PIN_INT, 0, PULL_UP, 8, false);
	sleep_ms(1);
	ldo_on("ldoc1", 1800);
	sleep_ms(1);
	ldo_on("ldoc10", 3200);
	sleep_ms(10);
}

static void
use_engine_pins(void)
{
	gpio_configure(PIN_MISO, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_MOSI, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_CLK, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_CS, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
}

static bool
bring_up(void)
{
	const char *problem = "its clocks cannot be turned on";

	show_state("as found");
	power_on();
	if (clocks_on()) {
		uint32_t spi_hz = 6000000;

		printf("munch-touch: serial engine: firmware %#x, interface %#x, parameters %#x, source clock %u Hz (select %u)\n",
		    rd(GENI_FW_REVISION_RO), rd(GENI_IF_DISABLE_RO), rd(SE_HW_PARAM_0), se_source_hz, se_clock_select);
		/* boot-arg touch_spi_hz=N: the bus at N Hz (Android's device tree gives the chip 6 MHz) */
		PE_parse_boot_argn("touch_spi_hz", &spi_hz, sizeof(spi_hz));
		if (spi_hz < 100000 || spi_hz > 6000000) {
			spi_hz = 6000000;
		}
		printf("munch-touch: the bus at %u Hz\n", spi_hz);
		problem = geni_spi_init(spi_hz);
	}
	if (problem == NULL) {
		use_engine_pins();
		spi_transfer = geni_transfer;
		show_state("powered, on the serial engine");
		if (focaltech_probe(true)) {
			return true;
		}
		printf("munch-touch: no answer through the serial engine; trying the pins by hand\n");
	} else {
		printf("munch-touch: %s; driving the pins by hand\n", problem);
	}

	/* by hand, and both ways round for the two data pins */
	spi_transfer = bitbang_transfer;
	for (int attempt = 0; attempt < 2; attempt++) {
		bitbang_in = attempt == 0 ? PIN_MISO : PIN_MOSI;
		bitbang_out = attempt == 0 ? PIN_MOSI : PIN_MISO;
		bitbang_init();
		if (focaltech_probe(true)) {
			printf("munch-touch: the chip answers on pins driven by hand (data in on gpio%u)\n", bitbang_in);
			return true;
		}
	}
	show_state("no answer");
	return false;
}

static void
touch_thread(__unused void *parameter, __unused wait_result_t result)
{
	unsigned idle = 0, idle_looks;
	uint32_t poll_us = 1000;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 1000;
	}
	idle_looks = 16000 / poll_us;   /* a report read every 16 ms even with the line high, in case it is missed */

	tlmm_north = ml_io_map(TLMM_NORTH_PHYS, TLMM_NORTH_SIZE);
	tlmm_south = ml_io_map(TLMM_SOUTH_PHYS, TLMM_SOUTH_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	lck_mtx_lock(&hakios_rsc_lock);
	if (rsc == 0) {
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);      /* memory, not registers: no alignment rules */
	}
	lck_mtx_unlock(&hakios_rsc_lock);

	if (!bring_up()) {
		printf("munch-touch: no touch screen\n");
		thread_terminate(current_thread());
		return;
	}
	printf("munch-touch: touch screen ready; the interrupt line is looked at every %u us\n", poll_us);
	for (unsigned tick = 1;; tick++) {
		/*
		 * The line is low only for a moment on this FocalTech family: on redwood's, looked at every 4 ms, reports were missed
		 * and a finger's lift came 88 to 169 ms after its last move, by the look of every 80 ms (hakios-xnu-kernel 81b5dd2,
		 * 5 November). So with a finger down every look reads, and with none a report is read every 16 ms anyway.
		 */
		if (!gpio_get(PIN_INT) || touching()) {
			idle = 0;
			touch_poll();
		} else if (++idle >= idle_looks) {
			idle = 0;
			touch_poll();
		}
		if (tick % (60 * (1000000 / poll_us)) == 0) {
			printf("munch-touch: so far %u reports in %u looks (%u bad), %u fingers followed by another under the same id, %u finger positions, the last at %u, %u; "
			    "interrupt line %s\n", seen.reports, seen.polls, seen.bad, seen.rolled, seen.touches, seen.last_x, seen.last_y,
			    gpio_get(PIN_INT) ? "high" : "low");
		}
		sleep_us(poll_us);
	}
}

static void
hakios_munch_touch_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	if (kernel_thread_start(touch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_munch_touch_init);
