/*
 * The POCO F4's (munch's) cameras: step 1, a look. The way is drivers/camera/hakios.camera.c's (the POCO X5 Pro's, whose
 * Camera.app works): the sensors' power from a PM8008 on an I2C bus done by hand, the sensors themselves on their CCI pins
 * done by hand too, their MCLK from the camera clock controller. Every number here is the POCO F4's, from its Android's
 * device tree (munch-sm8250-camera-sensor-mtp.dtsi, munch-sm8250-camera-board.dtsi, pm8008.dtsi, kona.dtsi) and its
 * kernel's drivers (camcc-kona.c, gcc-kona.c, pinctrl-kona.c), read as data:
 *
 *   the PM8008     on QUP 2's serial engine 2 ("se16") I2C: GPIO 48 data, 49 clock (south tile), addresses 0x08 (the
 *                  chip) and 0x09 (its regulators, L1P-L7P), 16-bit registers; its reset pin TLMM 77 (high: running,
 *                  "pm8008_active"), its interrupt TLMM 84
 *   the macro      GC02M1 (cell 3), the first to be done: CCI 0 master 1 (GPIO 103 data, 104 clock), MCLK1 on GPIO 95
 *                  (camcc RCG 0x501c, branch 0x5034, 19.2 MHz), reset GPIO 92, VIO PM8150 L7 1.8 V, VANA the PM8008's
 *                  L4P 2.8 V, CSIPHY 0
 *   the others     wide (cell 0: MCLK0 GPIO 94, reset 93, CCI 0 master 0 on 101/102, VANA switched by GPIO 13 and GPIO 22),
 *                  front (cell 1: MCLK 97, reset 109, CCI 1 master 0 on 105/106), ultra-wide (cell 2: MCLK 96, reset 78)
 *   the clocks     camcc at 0xad00000 behind GCC's camera_ahb (0xb008); its top power domain titan_top_gdsc (0xad0c144)
 *
 *   munch_cam=1    a look, nothing written to the hardware but the two pins of the PM8008's bus while it is read: the
 *                  camera's bus clock and, only if it runs, camcc's power domain and the clocks named above; the pins;
 *                  and, only if the PM8008 is out of reset, its revision, its chip enable and its seven LDOs
 *   munch_cam=2    ... and the macro powered up as its module file says (com.qti.sensormodule.munch_*_gc02m1_macro.bin,
 *                  CamX's "Parameter Parser V2.0.0": slave 0x6e, chip ID 0x02e0 at 0xf0; power-up RESET low 1 ms, VIO on
 *                  1 ms, VANA on 1 ms, MCLK 19.2 MHz 5 ms, RESET high 12 ms; power-down RESET low, MCLK off, VANA off,
 *                  VIO off), its chip ID read over CCI 0 master 1 by hand, and powered down again. Written: GCC's
 *                  camera_ahb, camcc's gdsc clock, titan_top_gdsc, MCLK1's RCG and branch, pins 95, 92, 77, RPMh's
 *                  PM8150 L7 (VIO, 1.8 V), the PM8008's chip enable and its L4P (VANA, 2.8 V; set point, mode, enable),
 *                  every PM8008 write read back. What was found on is left on; the rest is switched off at the end,
 *                  but for the vote for PM8150 L7, a 1.8 V rail other parts of the phone may share, which stays.
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.version.h>
#include <sys/hakios.input.h>
#include <sys/hakios.camera.h>
#include <sys/systm.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern bool hakios_munch_vreg(const char *name, uint32_t millivolts, bool on, uint32_t mode);      /* hakios.munch_touch.c */

static int say(const char *format, ...) __printflike(1, 2);
static bool say_quiet;           /* the run's way out: the loader's log only */

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (!say_quiet && ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return (int)strlen(line);
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile ("dsb sy");
}

/* ---- the pins: two tiles of the pin controller (pinctrl-kona.c) ------------------------------------------ */

#define TLMM_NORTH_PHYS         0x0f900000UL
#define TLMM_SOUTH_PHYS         0x0f500000UL
#define TLMM_TILE_SIZE          0x80000UL
#define CFG_PULL_UP             3u
#define CFG_OUTPUT              (1u << 9)

static vm_offset_t tlmm_north, tlmm_south;

static vm_offset_t
pin_base(unsigned pin)
{
	/* (of the pins used here, 48 and 49 are the south tile's, all the others the north's) */
	return (pin == 48 || pin == 49 ? tlmm_south : tlmm_north) + 0x1000UL * pin;
}

#define GPIO_CFG(n)             (pin_base(n))
#define GPIO_IO(n)              (pin_base(n) + 4)

static void
show_pin(unsigned pin, const char *what)
{
	uint32_t cfg = rd(GPIO_CFG(pin)), io = rd(GPIO_IO(pin));

	say("munch-cam: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pin, what, (cfg >> 2) & 15, cfg & 3,
	    (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
}

/* ---- I2C by hand (hakios.camera.c's): the PM8008's bus ------------------------------------------------- */

#define PIN_PMIC_SDA            48
#define PIN_PMIC_SCL            49
#define PIN_PMIC_RESET          77

struct bus {
	const char *    name;
	unsigned        sda, scl;
	uint32_t        cfg[2], io[2];
};
static struct bus pmic_bus = { .name = "the PM8008's I2C", .sda = PIN_PMIC_SDA, .scl = PIN_PMIC_SCL };

static void
half_bit(void)
{
	delay_for_interval(5, NSEC_PER_USEC);   /* 100 kHz at most */
}

static void
line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static bool
level_of(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
i2c_start(struct bus *b)
{
	line(b->sda, true);
	line(b->scl, true);
	half_bit();
	line(b->sda, false);
	half_bit();
	line(b->scl, false);
	half_bit();
}

static void
i2c_stop(struct bus *b)
{
	line(b->sda, false);
	half_bit();
	line(b->scl, true);
	half_bit();
	line(b->sda, true);
	half_bit();
}

static bool
i2c_bit(struct bus *b, bool out)
{
	bool in;

	line(b->sda, out);
	half_bit();
	line(b->scl, true);
	for (unsigned stretched = 0; !level_of(b->scl) && stretched < 100; stretched++) {
		half_bit();
	}
	half_bit();
	in = level_of(b->sda);
	line(b->scl, false);
	return in;
}

static bool
i2c_write(struct bus *b, uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit(b, (byte >> bit) & 1);
	}
	return !i2c_bit(b, true);
}

static uint8_t
i2c_read(struct bus *b, bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(b, true));
	}
	i2c_bit(b, !more);
	return byte;
}

static void
bus_give_back(struct bus *b)
{
	wr(GPIO_IO(b->sda), b->io[0] & 2);
	wr(GPIO_IO(b->scl), b->io[1] & 2);
	wr(GPIO_CFG(b->sda), b->cfg[0]);
	wr(GPIO_CFG(b->scl), b->cfg[1]);
}

static bool
bus_take(struct bus *b)
{
	b->cfg[0] = rd(GPIO_CFG(b->sda));
	b->cfg[1] = rd(GPIO_CFG(b->scl));
	b->io[0] = rd(GPIO_IO(b->sda));
	b->io[1] = rd(GPIO_IO(b->scl));
	line(b->sda, true);
	line(b->scl, true);
	half_bit();
	if (level_of(b->sda) && level_of(b->scl)) {
		return true;
	}
	{
		bool sda = level_of(b->sda), scl = level_of(b->scl);

		bus_give_back(b);
		say("munch-cam: %s is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", b->name, b->sda, sda, b->scl, scl);
	}
	return false;
}

/* A read: the register's two address bytes written, then `count' bytes read after a repeated start. */
static bool
i2c_get(struct bus *b, uint8_t address, uint16_t reg, uint8_t *in, unsigned count)
{
	uint8_t where[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
	bool answered;

	for (unsigned go = 0; go < 4; go++) {
		if (!bus_take(b)) {
			return false;
		}
		i2c_start(b);
		answered = i2c_write(b, (uint8_t)(address << 1)) && i2c_write(b, where[0]) && i2c_write(b, where[1]);
		if (answered) {
			i2c_start(b);
			answered = i2c_write(b, (uint8_t)(address << 1 | 1));
			for (unsigned i = 0; answered && i < count; i++) {
				in[i] = i2c_read(b, i + 1 < count);
			}
		}
		i2c_stop(b);
		bus_give_back(b);
		if (answered) {
			return true;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	return false;
}

/* ---- the PM8008 (pm8008.dtsi; registers as hakios.camera.c has them) ------------------------------------- */

#define PM8008_CHIP             0x08
#define PM8008_REGULATORS       0x09
#define REVID_BASE              0x0100
#define MISC_CHIP_ENABLE        0x0950
#define LDO_BASE(n)             (0x4000 + 0x100 * ((n) - 1))
#define LDO_STATUS1             0x08    /* [7] ready, [5] over-current, [1:0] mode */
#define LDO_VSET_LB             0x40
#define LDO_VSET_UB             0x41
#define LDO_ENABLE              0x46    /* [7] */

static void
look_pm8008(void)
{
	uint8_t rev[4] = { 0 }, enable = 0;

	show_pin(PIN_PMIC_RESET, "the PM8008's reset");
	show_pin(84, "the PM8008's interrupt");
	show_pin(PIN_PMIC_SDA, "the PM8008's I2C data");
	show_pin(PIN_PMIC_SCL, "the PM8008's I2C clock");
	if (!(rd(GPIO_IO(PIN_PMIC_RESET)) & 1)) {
		say("munch-cam: the PM8008 is held in reset: not read\n");
		return;
	}
	if (!i2c_get(&pmic_bus, PM8008_CHIP, REVID_BASE, rev, 4)) {
		say("munch-cam: the PM8008 does not answer at %#x\n", PM8008_CHIP);
		return;
	}
	(void)i2c_get(&pmic_bus, PM8008_CHIP, MISC_CHIP_ENABLE, &enable, 1);
	say("munch-cam: THE PM8008 ANSWERS: revision %02x %02x %02x %02x, chip enable %#x\n", rev[0], rev[1], rev[2], rev[3], enable);
	for (unsigned n = 1; n <= 7; n++) {
		uint8_t status = 0, lb = 0, ub = 0, en = 0;

		if (i2c_get(&pmic_bus, PM8008_REGULATORS, LDO_BASE(n) + LDO_STATUS1, &status, 1) &&
		    i2c_get(&pmic_bus, PM8008_REGULATORS, LDO_BASE(n) + LDO_VSET_LB, &lb, 1) &&
		    i2c_get(&pmic_bus, PM8008_REGULATORS, LDO_BASE(n) + LDO_VSET_UB, &ub, 1) &&
		    i2c_get(&pmic_bus, PM8008_REGULATORS, LDO_BASE(n) + LDO_ENABLE, &en, 1)) {
			say("munch-cam: L%uP: %s, set to %u mV, status %#x%s\n", n, (en & 0x80) ? "ON" : "off", ub << 8 | lb, status,
			    (status & 0x20) ? " (OVER-CURRENT)" : "");
		} else {
			say("munch-cam: L%uP: does not answer\n", n);
		}
	}
}

/* ---- the camera's clocks (gcc-kona.c, camcc-kona.c) -------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_CAMERA_AHB_CBCR     0xb008
#define GCC_CAMERA_HF_AXI_CBCR  0xb02c
#define GCC_CAMERA_XO_CBCR      0xb040
#define CAMCC_PHYS              0x0ad00000UL
#define CAMCC_SIZE              0x10000UL
#define CAMCC_TITAN_TOP_GDSCR   0xc144
#define CAMCC_MCLK1_CMD         0x501c
#define CAMCC_MCLK1_CFG         0x5020
#define CAMCC_MCLK1_CBCR        0x5034
#define CAMCC_CCI0_CBCR         0xc0d4
#define CAMCC_CCI1_CBCR         0xc0f0
#define CBCR_OFF                (1u << 31)

static void
look_clocks(void)
{
	vm_offset_t gcc = ml_io_map(GCC_PHYS, 0x10000), camcc;
	uint32_t ahb = rd(gcc + GCC_CAMERA_AHB_CBCR);

	say("munch-cam: gcc camera_ahb %#x (%s), camera_hf_axi %#x, camera_xo %#x\n", ahb, (ahb & CBCR_OFF) ? "off" : "running",
	    rd(gcc + GCC_CAMERA_HF_AXI_CBCR), rd(gcc + GCC_CAMERA_XO_CBCR));
	if (ahb & CBCR_OFF) {
		say("munch-cam: the camera clock controller's bus clock is off: it is not read\n");
		return;
	}
	camcc = ml_io_map(CAMCC_PHYS, CAMCC_SIZE);
	say("munch-cam: camcc titan_top_gdsc %#x (%s); mclk1: rcg %#x, cfg %#x, branch %#x; cci0 %#x, cci1 %#x\n",
	    rd(camcc + CAMCC_TITAN_TOP_GDSCR), (rd(camcc + CAMCC_TITAN_TOP_GDSCR) & (1u << 31)) ? "on" : "off",
	    rd(camcc + CAMCC_MCLK1_CMD), rd(camcc + CAMCC_MCLK1_CFG), rd(camcc + CAMCC_MCLK1_CBCR), rd(camcc + CAMCC_CCI0_CBCR),
	    rd(camcc + CAMCC_CCI1_CBCR));
}

/* ---- munch_cam=2: the macro powered up, its chip ID read, powered down ------------------------------------------------ */

#define PIN_MACRO_MCLK          95      /* function 1: cam_mclk */
#define PIN_MACRO_RESET         92
#define PIN_CCI0_M1_SDA         103
#define PIN_CCI0_M1_SCL         104
#define MACRO_SLAVE             0x37    /* the module file's 0x6e, as 7 bits */
#define MACRO_ID_REG            0xf0
#define MACRO_ID                0x02e0
#define MACRO_VANA_LDO          4       /* the PM8008's L4P */
#define MACRO_VANA_MV           2800
#define CAMCC_GDSC_CBCR         0xc16c
#define LDO_MODE_CTL1           0x45
#define GDSC_COLLAPSE           1u
#define CFG_FUNCTION(n)         ((n) << 2)
#define CFG_MA(n)               ((((n) / 2) - 1) << 6)

static struct bus cci_bus = { .name = "CCI 0 master 1 (the macro's)", .sda = PIN_CCI0_M1_SDA, .scl = PIN_CCI0_M1_SCL };
static vm_offset_t gcc_map, camcc_map;
static uint32_t found_77_cfg, found_77_io, found_95_cfg, found_92_cfg, found_92_io, found_ahb, found_gdsc_clk, found_gdsc;
static uint32_t found_mclk_cfg, found_mclk_cbcr;
static bool pm8008_released, vio_on, vana_on, mclk_on, gdsc_switched, chip_enabled, broke;

static void
wait_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* a register byte of the PM8008: written, read back */
static bool
pm8008_put(uint8_t address, uint16_t reg, uint8_t value)
{
	uint8_t back = 0xff;
	uint16_t base = reg & 0xff00, offset = reg & 0xff;
	bool allowed = (address == PM8008_CHIP && reg == MISC_CHIP_ENABLE) || (address == PM8008_REGULATORS &&
	    base >= LDO_BASE(1) && base <= LDO_BASE(7) && (offset == LDO_VSET_LB || offset == LDO_VSET_UB || offset == LDO_MODE_CTL1 ||
	    offset == LDO_ENABLE));
	uint8_t out[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, value };

	if (!allowed || broke) {
		say("munch-cam: a PM8008 write that is not to be made (%#x %#x): refused\n", address, reg);
		broke = true;
		return false;
	}
	for (unsigned go = 0; go < 4; go++) {
		bool answered;

		if (!bus_take(&pmic_bus)) {
			break;
		}
		i2c_start(&pmic_bus);
		answered = i2c_write(&pmic_bus, (uint8_t)(address << 1)) && i2c_write(&pmic_bus, out[0]) &&
		    i2c_write(&pmic_bus, out[1]) && i2c_write(&pmic_bus, out[2]);
		i2c_stop(&pmic_bus);
		bus_give_back(&pmic_bus);
		if (answered && i2c_get(&pmic_bus, address, reg, &back, 1) && back == value) {
			return true;
		}
		wait_ms(1);
	}
	say("munch-cam: the PM8008's %#x at %#x <- %#x did not read back (%#x): nothing more is written\n", address, reg, value, back);
	broke = true;
	return false;
}

static bool
clocks_up(void)
{
	gcc_map = ml_io_map(GCC_PHYS, 0x10000);
	found_ahb = rd(gcc_map + GCC_CAMERA_AHB_CBCR);
	wr(gcc_map + GCC_CAMERA_AHB_CBCR, found_ahb | 1);
	wait_ms(1);
	if (rd(gcc_map + GCC_CAMERA_AHB_CBCR) & CBCR_OFF) {
		say("munch-cam: camera_ahb does not come on (%#x)\n", rd(gcc_map + GCC_CAMERA_AHB_CBCR));
		return false;
	}
	camcc_map = ml_io_map(CAMCC_PHYS, CAMCC_SIZE);
	found_gdsc_clk = rd(camcc_map + CAMCC_GDSC_CBCR);
	wr(camcc_map + CAMCC_GDSC_CBCR, found_gdsc_clk | 1);
	found_gdsc = rd(camcc_map + CAMCC_TITAN_TOP_GDSCR);
	if (!(found_gdsc & (1u << 31))) {
		wr(camcc_map + CAMCC_TITAN_TOP_GDSCR, found_gdsc & ~GDSC_COLLAPSE);
		gdsc_switched = true;
		for (unsigned i = 0; i < 100 && !(rd(camcc_map + CAMCC_TITAN_TOP_GDSCR) & (1u << 31)); i++) {
			delay_for_interval(10, NSEC_PER_USEC);
		}
	}
	say("munch-cam: camera_ahb %#x, camcc gdsc clock %#x, titan_top_gdsc %#x\n", rd(gcc_map + GCC_CAMERA_AHB_CBCR),
	    rd(camcc_map + CAMCC_GDSC_CBCR), rd(camcc_map + CAMCC_TITAN_TOP_GDSCR));
	return (rd(camcc_map + CAMCC_TITAN_TOP_GDSCR) & (1u << 31)) != 0;
}

static void
clocks_down(void)
{
	if (gdsc_switched) {
		wr(camcc_map + CAMCC_TITAN_TOP_GDSCR, rd(camcc_map + CAMCC_TITAN_TOP_GDSCR) | GDSC_COLLAPSE);
		gdsc_switched = false;
	}
	if (camcc_map != 0) {
		wr(camcc_map + CAMCC_GDSC_CBCR, found_gdsc_clk);
	}
	if (gcc_map != 0) {
		wr(gcc_map + GCC_CAMERA_AHB_CBCR, found_ahb);
	}
}

static bool
pm8008_release(void)
{
	uint8_t rev[4] = { 0 }, enable = 0;

	found_77_cfg = rd(GPIO_CFG(PIN_PMIC_RESET));
	found_77_io = rd(GPIO_IO(PIN_PMIC_RESET));
	wr(GPIO_IO(PIN_PMIC_RESET), 2);
	wr(GPIO_CFG(PIN_PMIC_RESET), CFG_OUTPUT | CFG_PULL_UP);              /* "pm8008_active": output high, pull-up, 2 mA */
	pm8008_released = true;
	wait_ms(10);
	if (!i2c_get(&pmic_bus, PM8008_CHIP, REVID_BASE, rev, 4)) {
		say("munch-cam: the PM8008 does not answer after its reset\n");
		return false;
	}
	(void)i2c_get(&pmic_bus, PM8008_CHIP, MISC_CHIP_ENABLE, &enable, 1);
	say("munch-cam: THE PM8008 ANSWERS: revision %02x %02x %02x %02x, chip enable %#x\n", rev[0], rev[1], rev[2], rev[3], enable);
	if (!(enable & 1)) {
		if (!pm8008_put(PM8008_CHIP, MISC_CHIP_ENABLE, enable | 1)) {
			return false;
		}
		chip_enabled = true;
	}
	return true;
}

static bool
vana(bool on)
{
	uint16_t base = LDO_BASE(MACRO_VANA_LDO);
	uint8_t status = 0;

	if (!on) {
		if (vana_on) {
			(void)pm8008_put(PM8008_REGULATORS, base + LDO_ENABLE, 0);
			vana_on = false;
		}
		return true;
	}
	if (!pm8008_put(PM8008_REGULATORS, base + LDO_VSET_LB, MACRO_VANA_MV & 0xff) ||
	    !pm8008_put(PM8008_REGULATORS, base + LDO_VSET_UB, MACRO_VANA_MV >> 8) ||
	    !pm8008_put(PM8008_REGULATORS, base + LDO_MODE_CTL1, 7) || !pm8008_put(PM8008_REGULATORS, base + LDO_ENABLE, 0x80)) {
		return false;
	}
	vana_on = true;
	for (unsigned i = 0; i < 20; i++) {
		if (i2c_get(&pmic_bus, PM8008_REGULATORS, base + LDO_STATUS1, &status, 1) && (status & 0x80)) {
			break;
		}
		wait_ms(1);
	}
	say("munch-cam: VANA (L%uP) at %u mV: status %#x%s\n", MACRO_VANA_LDO, MACRO_VANA_MV, status,
	    (status & 0x20) ? " OVER-CURRENT" : (status & 0x80) ? ", ready" : ", NOT READY");
	return (status & 0x80) && !(status & 0x20);
}

static bool
mclk(bool on)
{
	if (!on) {
		if (mclk_on) {
			wr(camcc_map + CAMCC_MCLK1_CBCR, found_mclk_cbcr & ~1u);
			wr(camcc_map + CAMCC_MCLK1_CFG, found_mclk_cfg);
			wr(camcc_map + CAMCC_MCLK1_CMD, rd(camcc_map + CAMCC_MCLK1_CMD) | 1);
			wr(GPIO_CFG(PIN_MACRO_MCLK), found_95_cfg);
			mclk_on = false;
		}
		return true;
	}
	found_mclk_cfg = rd(camcc_map + CAMCC_MCLK1_CFG);
	found_mclk_cbcr = rd(camcc_map + CAMCC_MCLK1_CBCR);
	found_95_cfg = rd(GPIO_CFG(PIN_MACRO_MCLK));
	wr(camcc_map + CAMCC_MCLK1_CFG, 0 << 8 | 1);                          /* 19.2 MHz: the crystal, divided by 1 */
	wr(camcc_map + CAMCC_MCLK1_CMD, rd(camcc_map + CAMCC_MCLK1_CMD) | 1);
	for (unsigned i = 0; i < 100 && (rd(camcc_map + CAMCC_MCLK1_CMD) & 1); i++) {
		delay_for_interval(10, NSEC_PER_USEC);
	}
	wr(camcc_map + CAMCC_MCLK1_CBCR, found_mclk_cbcr | 1);
	wr(GPIO_CFG(PIN_MACRO_MCLK), CFG_FUNCTION(1) | CFG_MA(4));           /* cam_mclk, no pull, 4 mA */
	mclk_on = true;
	wait_ms(1);
	say("munch-cam: MCLK1 19.2 MHz: rcg %#x cfg %#x, branch %#x (%s)\n", rd(camcc_map + CAMCC_MCLK1_CMD),
	    rd(camcc_map + CAMCC_MCLK1_CFG), rd(camcc_map + CAMCC_MCLK1_CBCR),
	    (rd(camcc_map + CAMCC_MCLK1_CBCR) & CBCR_OFF) ? "OFF" : "running");
	return !(rd(camcc_map + CAMCC_MCLK1_CBCR) & CBCR_OFF);
}

static void
macro_reset(bool high)
{
	wr(GPIO_IO(PIN_MACRO_RESET), high ? 2 : 0);
	wr(GPIO_CFG(PIN_MACRO_RESET), CFG_OUTPUT | CFG_MA(2));              /* GPIO, no pull, 2 mA */
}

static void
macro_down(void)
{
	macro_reset(false);
	wait_ms(1);
	(void)mclk(false);
	wait_ms(1);
	(void)vana(false);
	wait_ms(1);
	/* (VIO, PM8150 L7, is a 1.8 V rail other parts may share: our vote is left as it is rather than taken away) */
	wr(GPIO_IO(PIN_MACRO_RESET), found_92_io & 2);
	wr(GPIO_CFG(PIN_MACRO_RESET), found_92_cfg);
	if (chip_enabled) {
		broke = false;
		(void)pm8008_put(PM8008_CHIP, MISC_CHIP_ENABLE, 0);
		chip_enabled = false;
	}
	if (pm8008_released) {
		wr(GPIO_IO(PIN_PMIC_RESET), found_77_io & 2);
		wr(GPIO_CFG(PIN_PMIC_RESET), found_77_cfg);
		pm8008_released = false;
	}
	clocks_down();
	say("munch-cam: the macro and what it took switched off again\n");
}

static void
macro_chip_id(void)
{
	uint8_t id[2] = { 0, 0 };
	bool ok;

	say("munch-cam: THE MACRO IS POWERED UP AS ITS MODULE FILE SAYS (munch_cam=2)\n");
	found_92_cfg = rd(GPIO_CFG(PIN_MACRO_RESET));
	found_92_io = rd(GPIO_IO(PIN_MACRO_RESET));
	if (!clocks_up() || !pm8008_release()) {
		macro_down();
		return;
	}
	macro_reset(false);
	wait_ms(1);
	if (!hakios_munch_vreg("ldoa7", 1800, true, 7)) {
		say("munch-cam: VIO (PM8150 L7) not given\n");
		macro_down();
		return;
	}
	vio_on = true;
	wait_ms(1);
	if (!vana(true)) {
		macro_down();
		return;
	}
	wait_ms(1);
	if (!mclk(true)) {
		macro_down();
		return;
	}
	wait_ms(5);
	macro_reset(true);
	wait_ms(12);
	/* the chip ID: an 8-bit register address, two bytes read (0xf0, 0xf1) */
	ok = false;
	for (unsigned go = 0; go < 3 && !ok; go++) {
		if (bus_take(&cci_bus)) {
			i2c_start(&cci_bus);
			ok = i2c_write(&cci_bus, MACRO_SLAVE << 1) && i2c_write(&cci_bus, MACRO_ID_REG);
			if (ok) {
				i2c_start(&cci_bus);
				ok = i2c_write(&cci_bus, MACRO_SLAVE << 1 | 1);
				if (ok) {
					id[0] = i2c_read(&cci_bus, true);
					id[1] = i2c_read(&cci_bus, false);
				}
			}
			i2c_stop(&cci_bus);
			bus_give_back(&cci_bus);
		}
		if (!ok) {
			wait_ms(2);
		}
	}
	if (!ok) {
		say("munch-cam: the macro does not answer at %#x on CCI 0 master 1\n", MACRO_SLAVE);
	} else {
		say("munch-cam: THE MACRO ANSWERS: chip ID %#04x%s\n", id[0] << 8 | id[1],
		    (id[0] << 8 | id[1]) == MACRO_ID ? " = 0x02e0: THE GC02M1" : " (not the GC02M1's 0x02e0)");
	}
	macro_down();
}

/*
 * munch_cam=4: read IDs from the three Camera.app sensors. GPIO, CCI, MCLK,
 * and supplies are from e404's munch-sm8250-camera-sensor-mtp.dtsi; register
 * addresses and power delays are from the corresponding CamX modules.
 */
struct probe_pin {
	unsigned pin;
	uint32_t cfg, io;
};

static void
probe_pin_drive(struct probe_pin *pin, unsigned number, bool high)
{
	pin->pin = number;
	pin->cfg = rd(GPIO_CFG(number));
	pin->io = rd(GPIO_IO(number));
	wr(GPIO_IO(number), high ? 2 : 0);
	wr(GPIO_CFG(number), CFG_OUTPUT);
}

static void
probe_pin_restore(const struct probe_pin *pin)
{
	if (pin->pin != 0) {
		wr(GPIO_IO(pin->pin), pin->io & 2);
		wr(GPIO_CFG(pin->pin), pin->cfg);
	}
}

static bool
probe_ldo(unsigned ldo, unsigned mv, bool *enabled)
{
	uint16_t base = LDO_BASE(ldo);
	uint8_t lb, ub, en, status = 0;

	if (!i2c_get(&pmic_bus, PM8008_REGULATORS, base + LDO_ENABLE, &en, 1)) {
		return false;
	}
	if (en & 0x80) {
		if (!i2c_get(&pmic_bus, PM8008_REGULATORS, base + LDO_VSET_LB, &lb, 1) ||
		    !i2c_get(&pmic_bus, PM8008_REGULATORS, base + LDO_VSET_UB, &ub, 1) ||
		    ((unsigned)ub << 8 | lb) != mv) {
			say("munch-cam: L%uP was on at a different voltage\n", ldo);
			return false;
		}
	} else {
		if (!pm8008_put(PM8008_REGULATORS, base + LDO_VSET_LB, mv & 0xff) ||
		    !pm8008_put(PM8008_REGULATORS, base + LDO_VSET_UB, mv >> 8) ||
		    !pm8008_put(PM8008_REGULATORS, base + LDO_MODE_CTL1, 7)) {
			return false;
		}
		*enabled = true;
		if (!pm8008_put(PM8008_REGULATORS, base + LDO_ENABLE, 0x80)) {
			return false;
		}
	}
	for (unsigned ms = 0; ms < 20; ms++) {
		if (i2c_get(&pmic_bus, PM8008_REGULATORS, base + LDO_STATUS1, &status, 1) && (status & 0xa0)) {
			break;
		}
		wait_ms(1);
	}
	say("munch-cam: L%uP at %u mV: status %#x\n", ldo, mv, status);
	return (status & 0xa0) == 0x80;
}

static bool
probe_id(struct bus *bus, unsigned address, uint16_t reg, uint16_t expected, const char *model)
{
	uint8_t id[2] = { 0 };
	bool read = i2c_get(bus, address, reg, id, sizeof(id));
	uint16_t actual = (uint16_t)(id[0] << 8 | id[1]);

	say("munch-cam: %s at %#x register %#x: %s, ID %#06x (expected %#06x)%s\n",
	    model, address << 1, reg, read ? "answered" : "no reply", actual, expected,
	    read && actual == expected ? " MATCH" : "");
	return read && actual == expected;
}

struct probe_sensor {
	const char *name;
	unsigned reset, mclk, sda, scl;
	unsigned vana, vana_mv, vdig;
};

static const struct probe_sensor wide_probe = { "wide", 93, 0, 101, 102, 0, 0, 2 };
static const struct probe_sensor ultra_probe = { "ultrawide", 78, 2, 105, 106, 3, 2800, 0 };
static const struct probe_sensor front_probe = { "front", 109, 3, 107, 108, 5, 2900, 1 };

static void capture_wide(struct bus *bus);
static bool sensor_stream_start(struct bus *bus, bool front, uint32_t stride, uint32_t height, uint32_t address);
static void wide_stream_stop(struct bus *bus);

struct sensor_power {
	struct probe_pin reset, vana, vana1;
	struct bus bus;
	bool avdd, dvdd, iovdd, clock, ultra_vote;
	uint32_t mclk_cfg, mclk_branch, mclk_pin;
};

static void sensor_power_down(const struct probe_sensor *sensor, struct sensor_power *power);

static bool
sensor_power_up(const struct probe_sensor *sensor, struct sensor_power *power)
{
	unsigned mclk_gpio = 94 + sensor->mclk;
	unsigned cmd = 0x5000 + 0x1c * sensor->mclk;

	power->bus = (struct bus){ .name = sensor->name, .sda = sensor->sda, .scl = sensor->scl };
	say("munch-cam: probing %s\n", sensor->name);
	probe_pin_drive(&power->reset, sensor->reset, false);
	wait_ms(1);
	if (sensor == &wide_probe) {
		probe_pin_drive(&power->vana, 13, true);
		probe_pin_drive(&power->vana1, 22, true);
		wait_ms(1);
	} else if (!probe_ldo(sensor->vana, sensor->vana_mv, &power->avdd)) {
		goto down;
	}
	if (sensor == &wide_probe && !probe_ldo(sensor->vdig, 1100, &power->dvdd)) {
		goto down;
	}
	if (!probe_ldo(6, 1800, &power->iovdd)) {
		goto down;
	}
	if (sensor == &front_probe && !probe_ldo(sensor->vdig, 1100, &power->dvdd)) {
		goto down;
	}
	if (sensor == &ultra_probe) {
		power->ultra_vote = true;
		if (!hakios_munch_vreg("ldoc3", 1210, true, 7)) {
			goto down;
		}
	}
	wait_ms(1);
	power->mclk_cfg = rd(camcc_map + cmd + 4);
	power->mclk_branch = rd(camcc_map + cmd + 0x18);
	power->mclk_pin = rd(GPIO_CFG(mclk_gpio));
	power->clock = true;
	wr(camcc_map + cmd + 4, 1);
	wr(camcc_map + cmd, rd(camcc_map + cmd) | 1);
	for (unsigned i = 0; i < 100 && (rd(camcc_map + cmd) & 1); i++) {
		delay_for_interval(10, NSEC_PER_USEC);
	}
	if (rd(camcc_map + cmd) & 1) {
		goto down;
	}
	wr(camcc_map + cmd + 0x18, power->mclk_branch | 1);
	wr(GPIO_CFG(mclk_gpio), CFG_FUNCTION(1) | CFG_MA(4));
	wait_ms(1);
	if (rd(camcc_map + cmd + 0x18) & CBCR_OFF) {
		goto down;
	}
	wr(GPIO_IO(sensor->reset), 2);
	wait_ms(2);
	return true;

down:
	sensor_power_down(sensor, power);
	return false;
}

static void
sensor_power_down(const struct probe_sensor *sensor, struct sensor_power *power)
{
	unsigned mclk_gpio = 94 + sensor->mclk;
	unsigned cmd = 0x5000 + 0x1c * sensor->mclk;

	wr(GPIO_IO(sensor->reset), 0);
	wait_ms(2);
	if (power->clock) {
		wr(GPIO_CFG(mclk_gpio), power->mclk_pin);
		wr(camcc_map + cmd + 0x18, power->mclk_branch);
		wr(camcc_map + cmd + 4, power->mclk_cfg);
		wr(camcc_map + cmd, rd(camcc_map + cmd) | 1);
	}
	/* Attempt each rollback independently, even if a readback failed. */
	if (power->ultra_vote) {
		(void)hakios_munch_vreg("ldoc3", 0, false, 0);
	}
	if (power->dvdd) {
		broke = false;
		(void)pm8008_put(PM8008_REGULATORS, LDO_BASE(sensor->vdig) + LDO_ENABLE, 0);
	}
	if (power->iovdd) {
		broke = false;
		(void)pm8008_put(PM8008_REGULATORS, LDO_BASE(6) + LDO_ENABLE, 0);
	}
	if (power->avdd) {
		broke = false;
		(void)pm8008_put(PM8008_REGULATORS, LDO_BASE(sensor->vana) + LDO_ENABLE, 0);
	}
	probe_pin_restore(&power->vana1);
	probe_pin_restore(&power->vana);
	probe_pin_restore(&power->reset);
}

static void
probe_sensor(const struct probe_sensor *sensor, bool capture)
{
	struct sensor_power power = { 0 };

	if (!sensor_power_up(sensor, &power)) {
		return;
	}
	if (sensor == &wide_probe) {
		if (!probe_id(&power.bus, 0x10, 0x0016, 0x0582, "IMX582")) {
			if (probe_id(&power.bus, 0x10, 0x300a, 0x5664, "OV64B40") && capture) {
				capture_wide(&power.bus);
			}
		}
	} else if (sensor == &front_probe) {
		(void)probe_id(&power.bus, 0x10, 0x0016, 0x0596, "IMX596");
	} else {
		(void)probe_id(&power.bus, 0x36, 0x300b, 0x885a, "OV8856");
	}
	sensor_power_down(sensor, &power);
}

static void
sensor_supply_down(void)
{
	if (chip_enabled) {
		broke = false;
		(void)pm8008_put(PM8008_CHIP, MISC_CHIP_ENABLE, 0);
		chip_enabled = false;
	}
	if (pm8008_released) {
		wr(GPIO_IO(PIN_PMIC_RESET), found_77_io & 2);
		wr(GPIO_CFG(PIN_PMIC_RESET), found_77_cfg);
		pm8008_released = false;
	}
	clocks_down();
}

static void
probe_cameras(bool capture)
{
	if (clocks_up() && pm8008_release()) {
		probe_sensor(&wide_probe, capture);
		if (!capture && !broke) {
			probe_sensor(&ultra_probe, false);
		}
		if (!capture && !broke) {
			probe_sensor(&front_probe, false);
		}
	}
	sensor_supply_down();
	say("munch-cam: %s finished\n", capture ? "wide capture test" : "camera ID probe");
}

/* ---- munch_cam=3: Titan 480 without a sensor, the CSID's test pattern into memory --------------------------------- */

/*
 * The camera subsystem's own blocks, every number from the POCO F4's Android (kona-camera.dtsi, camcc-kona.c,
 * gcc-kona.c, cam_ife_csid_lite480.h, cam_vfe_lite48x.h, cam_vfe_bus_ver3.c, cam_vfe_top_ver3.c,
 * cam_vfe_camif_lite_ver3.c, cam_ife_csid_core.c), the order drivers/camera/hakios.camera.c's capture_tpg() has:
 *
 *   clocks       GCC camera_hf_axi (0xb02c) and camera_sf_axi (0xb030); camcc's PLL0 (Lucid, 1200 MHz: even 600, odd
 *                400), slow_ahb 80 MHz (0x7058), camnoc_axi 300 (0xc0f8), cphy_rx 400 (0xa068), ife_lite 400 (0xc000),
 *                ife_lite_csid 400 (0xc01c), and the branches core_ahb, cpas_ahb, camnoc_axi, camnoc_dcd_xo, ife_lite_ahb,
 *                ife_lite_axi, ife_lite_csid, ife_lite_cphy_rx, ife_lite. Every one is at or under the device tree's
 *                lowest level for it ("lowsvs"), so nobody's vote for CX is needed here.
 *   the SMMU     the apps SMMU (0x15000000): four free match registers for the IFEs' streams, each as dt
 *                msm_cam_smmu_ife has it (0x800, 0x801, 0x840, 0x841, mask 0x400), and a free context with page
 *                tables of ours for the frames at FRAME_ADDRESSES (inside dt's pool 0x7400000 +0xd8c00000)
 *   CSID-lite 0  0xacd9200: the registers are the POCO X5 Pro's CSID-lite's, offset for offset (RDI 0, the TPG)
 *   IFE-lite 0   0xacd9000 (0x2200; the CSID sits in its hole): top ver3 (reset 0xc, irq 0x24-0x48), CAMIF-lite of RDI 0
 *                (0x1200), bus ver3 (0x1a00; write master 0, RDI 0's, at 0x1c00) in frame mode
 *
 * Two frames of 1600x1200 RAW10 colour bars; the loader hands them to the host (xl camera). Whatever was switched on is
 * switched off again in the reverse order, here and on the run's way out (hakios_munch_camera_quiesce()).
 */

extern uint64_t kvtophys(vm_offset_t va);
extern uint64_t loader_recording[3];
extern lck_mtx_t hakios_smmu_lock;

static bool leaving;

#define PLL_MODE                0x00            /* [0] outputs, [2] out of reset, [23] update bypass, [31] locked */
#define PLL_L_VAL               0x04
#define PLL_CAL_L_VAL           0x08
#define PLL_USER_CTL            0x0c
#define PLL_USER_CTL_U          0x10
#define PLL_USER_CTL_U1         0x14
#define PLL_CONFIG_CTL          0x18
#define PLL_CONFIG_CTL_U        0x1c
#define PLL_CONFIG_CTL_U1       0x20
#define PLL_OPMODE              0x38
#define PLL_ALPHA_VAL           0x40
#define CMD_UPDATE              1u
#define GCC_CAMERA_SF_AXI_CBCR  0xb030

static bool pll_on;

static bool
camcc_put(uint32_t offset, uint32_t value, uint32_t check)
{
	wr(camcc_map + offset, value);
	if ((rd(camcc_map + offset) & check) != (value & check)) {
		say("munch-cam: camcc %#x <- %#x, but it reads %#x\n", offset, value, rd(camcc_map + offset));
		broke = true;
		return false;
	}
	return true;
}

static bool
camcc_wait(uint32_t offset, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t waited = 0; waited <= us; waited += 10) {
		if ((rd(camcc_map + offset) & mask) == want) {
			return true;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	return false;
}

static bool
pll0_up(void)
{
	static const struct {
		uint32_t offset, value;
	} config[] = {                  /* camcc-kona.c: cam_cc_pll0_config; the outputs switched on below */
		{ PLL_L_VAL, 0x3e }, { PLL_CAL_L_VAL, 0x44 }, { PLL_ALPHA_VAL, 0x8000 }, { PLL_CONFIG_CTL, 0x20485699 },
		{ PLL_CONFIG_CTL_U, 0x00002261 }, { PLL_CONFIG_CTL_U1, 0x329a699c }, { PLL_USER_CTL, 0x00003100 },
		{ PLL_USER_CTL_U, 0x00000805 }, { PLL_USER_CTL_U1, 0x00000000 },
	};

	if (rd(camcc_map + PLL_MODE) & 1) {
		say("munch-cam: camcc's PLL0 is running already (somebody else's): left alone\n");
		return false;
	}
	for (unsigned i = 0; i < sizeof(config) / sizeof(config[0]); i++) {
		if (!camcc_put(config[i].offset, config[i].value, ~0u)) {
			return false;
		}
	}
	pll_on = true;
	if (!camcc_put(PLL_MODE, rd(camcc_map + PLL_MODE) | 1u << 23, 1u << 23) || !camcc_put(PLL_MODE, rd(camcc_map + PLL_MODE) & ~1u, 1) ||
	    !camcc_put(PLL_OPMODE, 0, 1) || !camcc_put(PLL_MODE, rd(camcc_map + PLL_MODE) | 1u << 2, 1u << 2) ||
	    !camcc_put(PLL_OPMODE, 1, 1)) {
		return false;
	}
	if (!camcc_wait(PLL_MODE, 1u << 31, 1u << 31, 1000)) {
		say("munch-cam: camcc's PLL0 does not lock (mode %#x)\n", rd(camcc_map + PLL_MODE));
		return false;
	}
	if (!camcc_put(PLL_USER_CTL, rd(camcc_map + PLL_USER_CTL) | 7, 7) || !camcc_put(PLL_MODE, rd(camcc_map + PLL_MODE) | 1, 1)) {
		return false;
	}
	say("munch-cam: camcc's PLL0 locked at 1200 MHz (mode %#x, user_ctl %#x)\n", rd(camcc_map + PLL_MODE),
	    rd(camcc_map + PLL_USER_CTL));
	return true;
}

static void
pll0_down(void)
{
	if (!pll_on) {
		return;
	}
	wr(camcc_map + PLL_MODE, rd(camcc_map + PLL_MODE) & ~1u);
	wr(camcc_map + PLL_USER_CTL, rd(camcc_map + PLL_USER_CTL) & ~7u);
	wr(camcc_map + PLL_OPMODE, 0);
	wr(camcc_map + PLL_MODE, rd(camcc_map + PLL_MODE) & ~(1u << 2));
	pll_on = false;
	say("munch-cam: camcc's PLL0 off (mode %#x)\n", rd(camcc_map + PLL_MODE));
}

struct rcg {
	const char *    name;
	uint32_t        cmd, source, divider;   /* source: 2 PLL0 even (600), 3 PLL0 odd (400); divider field 2n - 1 */
	uint32_t        found_cfg;
	bool            set;
};
static struct rcg rcgs[] = {
	{ "slow_ahb 80 MHz", 0x7058, 2, 14 },
	{ "camnoc_axi 300 MHz", 0xc0f8, 2, 3 },
	{ "cphy_rx 400 MHz", 0xa068, 3, 1 },
	{ "ife_lite 400 MHz", 0xc000, 3, 1 },
	{ "ife_lite_csid 400 MHz", 0xc01c, 3, 1 },
};
struct branch {
	const char *    name;
	bool            gcc;
	uint32_t        cbcr;
	bool            set;
};
static struct branch branches[] = {
	{ "gcc camera_hf_axi", true, GCC_CAMERA_HF_AXI_CBCR }, { "gcc camera_sf_axi", true, GCC_CAMERA_SF_AXI_CBCR },
	{ "core_ahb", false, 0xc150 }, { "cpas_ahb", false, 0xc0f4 }, { "camnoc_axi", false, 0xc114 },
	{ "camnoc_dcd_xo", false, 0xc11c }, { "ife_lite_ahb", false, 0xc040 }, { "ife_lite_axi", false, 0xc044 },
	{ "ife_lite_csid", false, 0xc034 }, { "ife_lite_cphy_rx", false, 0xc03c }, { "ife_lite", false, 0xc018 },
};

static bool
titan_clocks_up(void)
{
	for (unsigned i = 0; i < sizeof(rcgs) / sizeof(rcgs[0]); i++) {
		struct rcg *r = &rcgs[i];
		uint32_t cfg = r->cmd + 4;

		r->found_cfg = rd(camcc_map + cfg);
		if (!camcc_put(cfg, (r->found_cfg & ~0x371fu) | r->source << 8 | r->divider, 0x371f)) {
			return false;
		}
		r->set = true;
		wr(camcc_map + r->cmd, rd(camcc_map + r->cmd) | CMD_UPDATE);
		if (!camcc_wait(r->cmd, CMD_UPDATE, 0, 500)) {
			say("munch-cam: the RCG %s does not take its configuration (cmd %#x)\n", r->name, rd(camcc_map + r->cmd));
			return false;
		}
	}
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		struct branch *b = &branches[i];
		vm_offset_t base = b->gcc ? gcc_map : camcc_map;

		if (rd(base + b->cbcr) & 1) {
			continue;               /* (on already: left on at the end too) */
		}
		wr(base + b->cbcr, rd(base + b->cbcr) | 1);
		b->set = true;
		for (unsigned waited = 0; rd(base + b->cbcr) & CBCR_OFF; waited += 10) {
			if (waited > 500) {
				say("munch-cam: the branch %s does not come on (%#x)\n", b->name, rd(base + b->cbcr));
				return false;
			}
			delay_for_interval(10, NSEC_PER_USEC);
		}
	}
	say("munch-cam: Titan's clocks on (%u RCGs, %u branches)\n", (unsigned)(sizeof(rcgs) / sizeof(rcgs[0])),
	    (unsigned)(sizeof(branches) / sizeof(branches[0])));
	return true;
}

static void
titan_clocks_down(void)
{
	for (int i = (int)(sizeof(branches) / sizeof(branches[0])) - 1; i >= 0; i--) {
		struct branch *b = &branches[i];

		if (b->set) {
			vm_offset_t base = b->gcc ? gcc_map : camcc_map;

			wr(base + b->cbcr, rd(base + b->cbcr) & ~1u);
			b->set = false;
		}
	}
	for (int i = (int)(sizeof(rcgs) / sizeof(rcgs[0])) - 1; i >= 0; i--) {
		struct rcg *r = &rcgs[i];

		if (r->set) {
			wr(camcc_map + r->cmd + 4, r->found_cfg);
			wr(camcc_map + r->cmd, rd(camcc_map + r->cmd) | CMD_UPDATE);
			(void)camcc_wait(r->cmd, CMD_UPDATE, 0, 500);
			r->set = false;
		}
	}
}

/* ---- the frames' memory and the SMMU (hakios.camera.c's, with the POCO F4's streams) ---- */

#define FRAME_ADDRESSES         0x40000000ull
#define CAM_HEADER_BYTES        4096
#define CAM_FRAMES              2
#define TPG_WIDTH               1600
#define TPG_HEIGHT              1200
#define TPG_STRIDE              2000            /* RAW10: 1600 * 10 / 8, a multiple of 16 already */
#define SMMU_PHYS               0x15000000UL
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define CAM_PTE                 (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
/*
 * dt msm_cam_smmu_ife: eight streams as four pairs, each with mask 0x400 (0x800 and 0xc00, 0x801 and 0xc01, 0x840 and
 * 0xc40, 0x841 and 0xc41). The hypervisor takes a match register only as the device tree has it: one for 0x800 with mask
 * 0x441 read back 0 (9 November, run 185425). So four, each of the device tree's, all to our context.
 */
#define IFE_STREAM_MASK         0x400
#define IFE_MATCHES             4
static const uint16_t ife_streams[IFE_MATCHES] = { 0x800, 0x801, 0x840, 0x841 };

struct cam_header {                     /* what tools/camraw.py reads */
	char            magic[8];
	uint32_t        width, height, stride, format, bayer, frames, frame_bytes, source;
	char            sensor[16];
	uint32_t        last;
};

static vm_offset_t frames, smmu, smmu_second, smmu_context, page_tables;
static uint64_t frames_bytes;
static uint32_t smmu_match[IFE_MATCHES] = { ~0u, ~0u, ~0u, ~0u }, smmu_old_smr[IFE_MATCHES], smmu_old_s2cr[IFE_MATCHES];
static uint32_t smmu_matches, smmu_set, smmu_context_n, smmu_pages_size, smmu_pages, page_table_pages;
static bool smmu_on;

/* Taken at the thread's start, while the memory is still in large pieces. */
static bool
frames_alloc(uint32_t stride, uint32_t height, unsigned count)
{
	frames_bytes = round_page(CAM_HEADER_BYTES + (uint64_t)count * stride * height);
	if (kmem_alloc_contig(kernel_map, &frames, frames_bytes, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("munch-cam: no memory in one piece for %u frames (%llu KB)\n", count, frames_bytes >> 10);
		frames = 0;
		return false;
	}
	return true;
}

static void
frames_header(uint32_t width, uint32_t height, uint32_t stride, uint32_t bayer, const char *name)
{
	struct cam_header *h = (struct cam_header *)frames;

	memcpy(h->magic, "RWCAM001", 8);
	h->width = width;
	h->height = height;
	h->stride = stride;
	h->format = 1;
	h->bayer = bayer;
	h->frame_bytes = stride * height;
	h->source = 0;
	strlcpy(h->sensor, name, sizeof(h->sensor));
	for (vm_offset_t line = frames; line < frames + frames_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	loader_recording[0] = kvtophys(frames);
	loader_recording[1] = frames_bytes;
	loader_recording[2] = (uint64_t)frames;
	say("munch-cam: %llu KB of frame memory at %#llx, handed to the loader for the host\n", frames_bytes >> 10, loader_recording[0]);
}

static bool
smmu_find(void)
{
	bool used[256] = { false };
	uint32_t idr0, idr1;

	if (smmu == 0) {
		smmu = ml_io_map(SMMU_PHYS, 0x1000);
	}
	idr0 = rd(smmu + SMMU_IDR0);
	idr1 = rd(smmu + SMMU_IDR1);
	smmu_pages_size = idr1 >> 31 ? 0x10000 : 0x1000;
	smmu_pages = 2u << (idr1 >> 28 & 7);
	if (2 * smmu_pages * smmu_pages_size != 0x100000) {
		say("munch-cam: the SMMU's layout is not the one known here (idr1 %#x)\n", idr1);
		return false;
	}
	smmu_matches = 0;
	for (uint32_t n = 0; n < (idr0 & 0xff) && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			for (unsigned i = 0; i < IFE_MATCHES; i++) {
				if ((((smr & 0x7fff) ^ ife_streams[i]) & ~((smr >> 16 & 0x7fff) | IFE_STREAM_MASK)) == 0) {
					say("munch-cam: SMMU match register %u (%#x) is somebody's for the IFEs' streams: left alone\n", n, smr);
					return false;
				}
			}
		} else if (smmu_matches < IFE_MATCHES) {
			smmu_match[smmu_matches] = n;
			smmu_old_smr[smmu_matches] = smr;
			smmu_old_s2cr[smmu_matches] = s2cr;
			smmu_matches++;
		}
	}
	for (smmu_context_n = 0; smmu_context_n < (idr1 & 0xff) && used[smmu_context_n]; smmu_context_n++) {
	}
	if (smmu_matches < IFE_MATCHES || smmu_context_n >= (idr1 & 0xff)) {
		say("munch-cam: the SMMU has not %u match registers and a context left\n", IFE_MATCHES);
		smmu_matches = 0;
		return false;
	}
	smmu_second = ml_io_map(SMMU_PHYS + smmu_pages_size, 0x1000);
	smmu_context = ml_io_map(SMMU_PHYS + (uint64_t)(smmu_pages + smmu_context_n) * smmu_pages_size, 0x1000);
	if (rd(smmu_context + SMMU_CB_SCTLR) & 1) {
		say("munch-cam: SMMU context %u is switched on although no stream uses it: somebody's\n", smmu_context_n);
		smmu_matches = 0;
		return false;
	}
	return true;
}

static bool
smmu_put(vm_offset_t base, uint32_t offset, uint32_t value, uint32_t check)
{
	wr(base + offset, value);
	if ((rd(base + offset) & check) != (value & check)) {
		say("munch-cam: SMMU %#x <- %#x, but it reads %#x\n", offset, value, rd(base + offset));
		broke = true;
		return false;
	}
	return true;
}

static bool
smmu_up(void)
{
	uint64_t *root, *second, table_root;
	uint32_t thirds = (uint32_t)((frames_bytes + 0x1fffff) >> 21), pages = (uint32_t)(frames_bytes >> 12);
	bool ok;

	lck_mtx_lock(&hakios_smmu_lock);
	if (!smmu_find()) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	page_table_pages = 2 + thirds;
	if (kmem_alloc(kernel_map, &page_tables, page_table_pages * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		lck_mtx_unlock(&hakios_smmu_lock);
		smmu_matches = 0;
		page_tables = 0;
		say("munch-cam: no memory for page tables\n");
		return false;
	}
	root = (uint64_t *)page_tables;
	second = (uint64_t *)(page_tables + 4096);
	root[FRAME_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	for (uint32_t t = 0; t < thirds; t++) {
		uint64_t *third = (uint64_t *)(page_tables + (2 + t) * 4096);

		second[(FRAME_ADDRESSES >> 21 & 0x1ff) + t] = kvtophys((vm_offset_t)third) | 3;
		for (uint32_t p = 0; p < 512 && t * 512 + p < pages; p++) {
			third[p] = kvtophys(frames + 4096ull * (t * 512 + p)) | CAM_PTE;
		}
	}
	for (vm_offset_t line = page_tables; line < page_tables + page_table_pages * 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	table_root = kvtophys(page_tables) | (uint64_t)smmu_context_n << 48;
	say("munch-cam: SMMU: context %u gets our tables for %llu KB at %#llx; match registers %u %u %u %u send the device tree's "
	    "four (%#x %#x %#x %#x, mask %#x) through it\n", smmu_context_n, frames_bytes >> 10, FRAME_ADDRESSES, smmu_match[0],
	    smmu_match[1], smmu_match[2], smmu_match[3], ife_streams[0], ife_streams[1], ife_streams[2], ife_streams[3], IFE_STREAM_MASK);
	ok = smmu_put(smmu_context, SMMU_CB_SCTLR, 0, ~0u) && smmu_put(smmu_second, SMMU_CBA2R(smmu_context_n), 1, 1) &&
	    smmu_put(smmu_second, SMMU_CBAR(smmu_context_n), SMMU_CBAR_STAGE_1, 0x30000) &&
	    smmu_put(smmu_context, SMMU_CB_TCR2, SMMU_TCR2, 0) && smmu_put(smmu_context, SMMU_CB_TCR, SMMU_TCR_39_BITS, ~0u) &&
	    smmu_put(smmu_context, SMMU_CB_TTBR0, (uint32_t)table_root, ~0u) &&
	    smmu_put(smmu_context, SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32), ~0u) &&
	    smmu_put(smmu_context, SMMU_CB_TTBR1, 0, ~0u) && smmu_put(smmu_context, SMMU_CB_TTBR1 + 4, 0, ~0u) &&
	    smmu_put(smmu_context, SMMU_CB_MAIR0, SMMU_MAIR0, ~0u) && smmu_put(smmu_context, SMMU_CB_MAIR1, 0, ~0u);
	if (ok) {
		wr(smmu_context + SMMU_CB_FSR, SMMU_FSR_ALL);
		wr(smmu_context + SMMU_CB_TLBIASID, smmu_context_n);
		wr(smmu_context + SMMU_CB_TLBSYNC, 1);
		for (unsigned waited = 0; ok && rd(smmu_context + SMMU_CB_TLBSTATUS) & 1; waited++) {
			ok = waited < 1000;
			delay_for_interval(10, NSEC_PER_USEC);
		}
	}
	if (ok && smmu_put(smmu_context, SMMU_CB_SCTLR, SMMU_SCTLR_ON, 1)) {
		smmu_on = true;
		/*
		 * The first is needed; the others are taken where the hypervisor takes them (run 191839: 0x800 was taken, 0x801
		 * read back 0). A stream that reaches none of them shows in the SMMU's global fault status (smmu_faults()).
		 */
		for (unsigned i = 0; ok && i < IFE_MATCHES; i++) {
			uint32_t smr = 1u << 31 | (uint32_t)IFE_STREAM_MASK << 16 | ife_streams[i];

			smmu_set = i + 1;       /* (put back, even half done) */
			wr(smmu + SMMU_S2CR(smmu_match[i]), smmu_context_n);
			wr(smmu + SMMU_SMR(smmu_match[i]), smr);
			if (rd(smmu + SMMU_SMR(smmu_match[i])) != smr || (rd(smmu + SMMU_S2CR(smmu_match[i])) & 0x300ff) != smmu_context_n) {
				say("munch-cam: SMMU match register %u for stream %#x: not taken (reads %#x %#x)%s\n", smmu_match[i],
				    ife_streams[i], rd(smmu + SMMU_SMR(smmu_match[i])), rd(smmu + SMMU_S2CR(smmu_match[i])),
				    i == 0 ? ": no capture" : "; the others go on");
				wr(smmu + SMMU_SMR(smmu_match[i]), smmu_old_smr[i]);
				wr(smmu + SMMU_S2CR(smmu_match[i]), smmu_old_s2cr[i]);
				ok = i != 0;
			}
		}
	} else {
		ok = false;
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	return ok;
}

static void
smmu_faults(void)
{
	uint32_t status, global;

	if (!smmu_on) {
		return;
	}
	status = rd(smmu_context + SMMU_CB_FSR);
	global = rd(smmu + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		say("munch-cam: the SMMU has refused the IFE nothing (context %u)\n", smmu_context_n);
		return;
	}
	say("munch-cam: THE SMMU HAS REFUSED A REQUEST: context %u status %#x, address %#llx, syndrome %#x; global status %#x, "
	    "stream %#x\n", smmu_context_n, status, (uint64_t)rd(smmu_context + SMMU_CB_FAR + 4) << 32 | rd(smmu_context + SMMU_CB_FAR),
	    rd(smmu_context + SMMU_CB_FSYNR0), global, rd(smmu + SMMU_SGFSYNR1));
}

static void
smmu_down(void)
{
	if (!smmu_on && smmu_set == 0) {
		return;
	}
	smmu_faults();
	if (!leaving) {
		lck_mtx_lock(&hakios_smmu_lock);
	}
	for (unsigned i = 0; i < smmu_set; i++) {
		wr(smmu + SMMU_SMR(smmu_match[i]), smmu_old_smr[i]);
		wr(smmu + SMMU_S2CR(smmu_match[i]), smmu_old_s2cr[i]);
	}
	if (smmu_on) {
		wr(smmu_context + SMMU_CB_SCTLR, 0);
		wr(smmu_context + SMMU_CB_TLBIASID, smmu_context_n);
		wr(smmu_context + SMMU_CB_TLBSYNC, 1);
	}
	if (!leaving) {
		lck_mtx_unlock(&hakios_smmu_lock);
	}
	smmu_on = false;
	smmu_set = 0;
	smmu_matches = 0;
	say("munch-cam: the SMMU's match registers and context put back as they were found\n");
}

/* ---- CSID-lite 0 (the POCO X5 Pro's offsets) and IFE-lite 0 (ver3) ---- */

#define CSID_PHYS               0x0acd9200UL
#define IFE_PHYS                0x0acd9000UL
#define CSID_HW_VERSION         0x000
#define CSID_RST_STROBES        0x010
#define CSID_CSI2_RX_IRQ_STATUS 0x020
#define CSID_CSI2_RX_IRQ_MASK   0x024
#define CSID_CSI2_RX_IRQ_CLEAR  0x028
#define CSID_RDI0_IRQ_STATUS    0x030
#define CSID_RDI0_IRQ_MASK      0x034
#define CSID_RDI0_IRQ_CLEAR     0x038
#define CSID_TOP_IRQ_STATUS     0x070
#define CSID_TOP_IRQ_MASK       0x074
#define CSID_TOP_IRQ_CLEAR      0x078
#define CSID_IRQ_CMD            0x080
#define CSID_CSI2_RX_CFG0       0x100
#define CSID_CSI2_RX_CFG1       0x104
#define CSID_CSI2_RX_PKTS       0x160
#define CSID_RDI0_CFG0          0x200
#define CSID_RDI0_CFG1          0x204
#define CSID_RDI0_CTRL          0x208
#define CSID_RDI0_FRM_DROP_PATTERN 0x20c
#define CSID_RDI0_FRM_DROP_PERIOD  0x210
#define CSID_RDI0_IRQ_SUB_PATTERN  0x214
#define CSID_RDI0_IRQ_SUB_PERIOD   0x218
#define CSID_RDI0_PIX_DROP_PATTERN 0x224
#define CSID_RDI0_PIX_DROP_PERIOD  0x228
#define CSID_RDI0_LINE_DROP_PATTERN 0x22c
#define CSID_RDI0_LINE_DROP_PERIOD  0x230
#define CSID_RDI0_STATUS        0x250
#define CSID_RDI0_BYTES_PING    0x2e0
#define CSID_RDI0_BYTES_PONG    0x2e4
#define CSID_TPG_CTRL           0x600
#define CSID_TPG_VC_CFG0        0x604
#define CSID_TPG_VC_CFG1        0x608
#define CSID_TPG_LFSR_SEED      0x60c
#define CSID_TPG_DT0_CFG0       0x610
#define CSID_TPG_DT0_CFG1       0x614
#define CSID_TPG_DT0_CFG2       0x618
#define CSID_TPG_COLOR_BARS_CFG 0x640
#define CSID_TPG_COMMON_GEN_CFG 0x648
#define TPG_VC                  0xa
#define RAW10_DT                0x2b

#define IFE_HW_VERSION          0x0000
#define IFE_GLOBAL_RESET        0x000c          /* 2: hardware and registers (an IFE-lite's) */
#define IFE_REG_UPDATE          0x0020          /* [0] RDI 0 */
#define IFE_IRQ_GLOBAL_CLEAR    0x0024
#define IFE_IRQ_MASK0           0x0028          /* [17] reset done */
#define IFE_IRQ_MASK1           0x002c          /* RDI 0: [0] start of frame, [1] end */
#define IFE_IRQ_CLEAR0          0x0034
#define IFE_IRQ_CLEAR1          0x0038
#define IFE_IRQ_CLEAR2          0x003c
#define IFE_IRQ_STATUS0         0x0040
#define IFE_IRQ_STATUS1         0x0044
#define IFE_IRQ_STATUS2         0x0048
#define IFE_TOP_DEBUG_CFG       0x0074
#define IFE_RDI0_EPOCH_IRQ      0x1270
#define BUS_HW_VERSION          0x1a00
#define BUS_CGC_OVD             0x1a08
#define BUS_IRQ_MASK0           0x1a18
#define BUS_IRQ_CLEAR0          0x1a20
#define BUS_IRQ_CLEAR1          0x1a24
#define BUS_IRQ_STATUS0         0x1a28          /* [0..3] register update done, [4 + n] comp group n (RDI n) done */
#define BUS_IRQ_STATUS1         0x1a2c
#define BUS_IRQ_GLOBAL_CLEAR    0x1a30
#define BUS_CCIF_VIOLATION      0x1a64
#define BUS_OVERFLOW_STATUS     0x1a68
#define BUS_IMAGE_SIZE_VIOLATION 0x1a70
#define BUS_TEST_CTRL           0x1adc
#define WM0_CFG                 0x1c00          /* [16] frame mode, [0] on */
#define WM0_IMAGE_ADDR          0x1c04
#define WM0_FRAME_INC           0x1c08
#define WM0_IMAGE_CFG_0         0x1c0c          /* [31:16] height, [15:0] width */
#define WM0_IMAGE_CFG_1         0x1c10
#define WM0_IMAGE_CFG_2         0x1c14          /* stride */
#define WM0_PACKER_CFG          0x1c18
#define WM0_IRQ_SUB_PERIOD      0x1c30
#define WM0_IRQ_SUB_PATTERN     0x1c34
#define WM0_FRAMEDROP_PERIOD    0x1c38
#define WM0_FRAMEDROP_PATTERN   0x1c3c
#define WM0_BURST_LIMIT         0x1c64          /* (2 bits here: 0xf reads back 0x3; Android writes 0xf all the same) */
#define WM0_ADDR_STATUS0        0x1c68
#define WM0_ADDR_STATUS1        0x1c6c
#define WM0_DEBUG_STATUS0       0x1c7c

static vm_offset_t csid, ife;
static bool csid_running, ife_running, tpg_running;

static bool
blk_put(vm_offset_t base, const char *what, uint32_t offset, uint32_t value, bool check)
{
	wr(base + offset, value);
	if (check && rd(base + offset) != value) {
		say("munch-cam: %s %#x <- %#x, but it reads %#x\n", what, offset, value, rd(base + offset));
		broke = true;
		return false;
	}
	return true;
}

static bool
blk_wait(vm_offset_t base, uint32_t offset, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t waited = 0; waited <= us; waited += 10) {
		if ((rd(base + offset) & mask) == want) {
			return true;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	return false;
}

static bool
csid_reset(void)
{
	blk_put(csid, "CSID", CSID_CSI2_RX_IRQ_MASK, 0, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_IRQ_MASK + 0x10 * r, 0, false);
	}
	blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
	blk_put(csid, "CSID", CSID_CSI2_RX_IRQ_CLEAR, 0xfffffff, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_IRQ_CLEAR + 0x10 * r, 0x7fff, false);
	}
	blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
	blk_put(csid, "CSID", CSID_CSI2_RX_CFG1, 0x80, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_CFG0 + 0x100 * r, 2, false);
	}
	for (unsigned hw = 0; hw < 2; hw++) {
		blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
		blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		if (hw) {
			blk_put(csid, "CSID", CSID_TOP_IRQ_MASK, 1, false);
			blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		}
		blk_put(csid, "CSID", CSID_RST_STROBES, hw ? 0x1e : 0x1, false);
		if (!hw) {
			blk_put(csid, "CSID", CSID_TOP_IRQ_MASK, 1, false);
			blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		}
		if (!blk_wait(csid, CSID_TOP_IRQ_STATUS, 1, 1, 100000)) {
			say("munch-cam: the CSID's %s reset does not finish (top irq %#x)\n", hw ? "hardware" : "registers'",
			    rd(csid + CSID_TOP_IRQ_STATUS));
			return false;
		}
		blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
		blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
	}
	say("munch-cam: CSID-lite 0 reset (version %#x)\n", rd(csid + CSID_HW_VERSION));
	return true;
}

/* The RX from the test pattern generator, and RDI 0 for its RAW10, written as it comes (packed). */
static bool
csid_config(bool tpg, uint32_t rx)
{
	uint32_t cfg0 = (tpg ? TPG_VC : 0) << 22 | RAW10_DT << 16 | 0xfu << 12 | 1u << 2 | 3u;

	if (!blk_put(csid, "CSID", CSID_CSI2_RX_CFG0, rx, true) ||
	    !blk_put(csid, "CSID", CSID_CSI2_RX_CFG1, 1u << 6 | 1u, true)) {
		return false;
	}
	if (tpg && (!blk_put(csid, "CSID", CSID_TPG_VC_CFG0, 0u << 16 | 1u << 10 | TPG_VC, true) ||
	    !blk_put(csid, "CSID", CSID_TPG_VC_CFG1, 0x3ffu << 12 | 0x740, true) ||
	    !blk_put(csid, "CSID", CSID_TPG_LFSR_SEED, 0x12345678, true) ||
	    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG0, TPG_WIDTH << 16 | TPG_HEIGHT, true) ||
	    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG1, RAW10_DT, true) ||
	    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG2, (3u - 1) << 16 | 0x8, true) ||
	    !blk_put(csid, "CSID", CSID_TPG_COLOR_BARS_CFG, 1u << 5, true) ||
	    !blk_put(csid, "CSID", CSID_TPG_COMMON_GEN_CFG, 0, true))) {
		return false;
	}
	if (!blk_put(csid, "CSID", CSID_RDI0_CFG0, cfg0, true) || !blk_put(csid, "CSID", CSID_RDI0_CFG1, 2, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_FRM_DROP_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_FRM_DROP_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_IRQ_SUB_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_IRQ_SUB_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_PIX_DROP_PATTERN, 0, true) || !blk_put(csid, "CSID", CSID_RDI0_PIX_DROP_PERIOD, 1, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_LINE_DROP_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_LINE_DROP_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_CTRL, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_CFG0, cfg0 | 1u << 31, true)) {
		return false;
	}
	csid_running = true;
	return blk_put(csid, "CSID", CSID_RDI0_CTRL, 1, true) &&
	       blk_put(csid, "CSID", CSID_RDI0_IRQ_MASK, 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 6, true);
}

/* cam_vfe_top_ver3_reset(), cam_vfe_bus_ver3_init_hw(), start_wm() and update_wm() for RDI 0 in frame mode, the CAMIF-lite's start. */
static bool
ife_config(uint32_t stride, uint32_t height, uint32_t address)
{
	uint32_t frame_bytes = stride * height;

	blk_put(ife, "IFE", IFE_IRQ_MASK0, 1u << 17, false);
	blk_put(ife, "IFE", IFE_GLOBAL_RESET, 2, false);
	if (!blk_wait(ife, IFE_IRQ_STATUS0, 1u << 17, 1u << 17, 100000)) {
		say("munch-cam: the IFE does not reset (irq %#x)\n", rd(ife + IFE_IRQ_STATUS0));
		return false;
	}
	blk_put(ife, "IFE", IFE_IRQ_CLEAR0, 0xffffffff, false);
	blk_put(ife, "IFE", IFE_IRQ_CLEAR1, 0xffffffff, false);
	blk_put(ife, "IFE", IFE_IRQ_CLEAR2, 0xffffffff, false);
	blk_put(ife, "IFE", IFE_IRQ_GLOBAL_CLEAR, 1, false);
	say("munch-cam: IFE-lite 0 reset (version %#x, bus %#x)\n", rd(ife + IFE_HW_VERSION), rd(ife + BUS_HW_VERSION));
	if (!blk_put(ife, "IFE", BUS_CGC_OVD, 0x3ffffff, false) || !blk_put(ife, "IFE", BUS_TEST_CTRL, 0, false) ||
	    !blk_put(ife, "IFE", BUS_IRQ_MASK0, 0x1f, false) || !blk_put(ife, "IFE", IFE_IRQ_MASK1, 3, false) ||
	    !blk_put(ife, "IFE", WM0_BURST_LIMIT, 0xf, false) || !blk_put(ife, "IFE", WM0_IMAGE_CFG_0, 0xffff, true) ||
	    !blk_put(ife, "IFE", WM0_PACKER_CFG, 0, true) || !blk_put(ife, "IFE", WM0_IMAGE_CFG_2, stride, true) ||
	    !blk_put(ife, "IFE", WM0_IMAGE_CFG_1, 0, true) || !blk_put(ife, "IFE", WM0_FRAMEDROP_PERIOD, 0, true) ||
	    !blk_put(ife, "IFE", WM0_FRAMEDROP_PATTERN, 1, true) || !blk_put(ife, "IFE", WM0_IRQ_SUB_PERIOD, 0, true) ||
	    !blk_put(ife, "IFE", WM0_IRQ_SUB_PATTERN, 1, true) ||
	    !blk_put(ife, "IFE", WM0_IMAGE_ADDR, address, false) ||
	    !blk_put(ife, "IFE", WM0_FRAME_INC, frame_bytes, true) || !blk_put(ife, "IFE", WM0_CFG, 1u << 16 | 1u, true)) {
		return false;
	}
	ife_running = true;
	blk_put(ife, "IFE", IFE_RDI0_EPOCH_IRQ, 0, false);
	blk_put(ife, "IFE", IFE_TOP_DEBUG_CFG, 1, false);
	return blk_put(ife, "IFE", IFE_REG_UPDATE, 1, false);
}

static void
show_paths(const char *when)
{
	say("munch-cam: %s: CSID rx irq %#x, packets %u; RDI0 irq %#x status %#x, bytes %u/%u; IFE irq %#x %#x %#x; bus irq %#x %#x, "
	    "overflow %#x, ccif %#x, size violation %#x; WM0 addresses %#x %#x, debug %#x\n", when, rd(csid + CSID_CSI2_RX_IRQ_STATUS),
	    rd(csid + CSID_CSI2_RX_PKTS), rd(csid + CSID_RDI0_IRQ_STATUS), rd(csid + CSID_RDI0_STATUS), rd(csid + CSID_RDI0_BYTES_PING),
	    rd(csid + CSID_RDI0_BYTES_PONG), rd(ife + IFE_IRQ_STATUS0), rd(ife + IFE_IRQ_STATUS1), rd(ife + IFE_IRQ_STATUS2),
	    rd(ife + BUS_IRQ_STATUS0), rd(ife + BUS_IRQ_STATUS1), rd(ife + BUS_OVERFLOW_STATUS), rd(ife + BUS_CCIF_VIOLATION),
	    rd(ife + BUS_IMAGE_SIZE_VIOLATION), rd(ife + WM0_ADDR_STATUS0), rd(ife + WM0_ADDR_STATUS1), rd(ife + WM0_DEBUG_STATUS0));
}

static void
paths_down(void)
{
	if (tpg_running) {
		wr(csid + CSID_TPG_CTRL, 0);
		tpg_running = false;
	}
	if (csid_running) {
		wr(csid + CSID_RDI0_IRQ_MASK, 0);
		wr(csid + CSID_RDI0_CTRL, (rd(csid + CSID_RDI0_CTRL) & ~3u) | 2);
		wr(csid + CSID_RDI0_CFG0, rd(csid + CSID_RDI0_CFG0) & ~(1u << 31));
		wr(csid + CSID_CSI2_RX_CFG0, 0);
		csid_running = false;
	}
	if (ife_running) {
		wr(ife + WM0_CFG, 0);
		wr(ife + IFE_REG_UPDATE, 1);
		delay_for_interval(1, NSEC_PER_MSEC);
		ife_running = false;
	}
}

/* OV64B40 wide: the sensor module names mode 2 as 4624x2608 RAW10, 30 fps. */
#define WIDE_MODE               2
#define WIDE_WIDTH              4624
#define WIDE_HEIGHT             2608
#define WIDE_STRIDE             (((WIDE_WIDTH * 5 / 4) + 15) & ~15u)
#define WIDE_PHY                1u

struct wide_mode {
	uint32_t width, height, x, y, lanes, cphy, settle, pixel_clock, line_length, frame_length, fps_milli, data_type;
};

struct sensor_setting {
	uint16_t address, data;
	uint8_t address_bytes, data_bytes, operation, reserved;
	uint32_t delay_us;
};

static bool
wide_write(struct bus *bus, uint16_t address, uint8_t value)
{
	if (!bus_take(bus)) {
		return false;
	}
	i2c_start(bus);
	bool ok = i2c_write(bus, 0x20) && i2c_write(bus, address >> 8) &&
	    i2c_write(bus, address & 0xff) && i2c_write(bus, value);
	i2c_stop(bus);
	bus_give_back(bus);
	return ok;
}

static bool
wide_write_table(struct bus *bus, const uint8_t *at, const uint8_t *end, const char *name)
{
	uint32_t tables;
	if ((size_t)(end - at) < sizeof(tables)) {
		return false;
	}
	memcpy(&tables, at, sizeof(tables));
	at += sizeof(tables);
	while (tables-- && (size_t)(end - at) >= 12) {
		uint32_t count;
		memcpy(&count, at + 8, sizeof(count));
		if (count > (size_t)(end - at - 12) / sizeof(struct sensor_setting)) {
			return false;
		}
		const struct sensor_setting *settings = (const void *)(at + 12);
		if (strncmp((const char *)at, name, 8) == 0) {
			for (uint32_t i = 0; i < count; i++) {
				const struct sensor_setting *r = &settings[i];
				if (r->operation != 0 || r->address_bytes != 2 || r->data_bytes != 1) {
					return false;
				}
				if (!wide_write(bus, r->address, r->data)) {
					say("munch-cam: wide %s write %u at %#x was not acknowledged\n", name, i, r->address);
					return false;
				}
				if (r->delay_us != 0) {
					delay_for_interval(MIN(r->delay_us, 100000u), NSEC_PER_USEC);
				}
			}
			say("munch-cam: OV64B40 table %s: %u writes\n", name, count);
			return true;
		}
		at += 12 + count * sizeof(struct sensor_setting);
	}
	return false;
}

static vm_offset_t csiphy;

static bool
csiphy_cphy(unsigned settle, uint64_t bits_per_trio)
{
	static const uint32_t trios[3][2] = { { 0x100, 0x980 }, { 0x300, 0xa80 }, { 0x500, 0xb80 } };
	static const struct {
		uint64_t        bandwidth;
		uint8_t         r144, r164, r16c, rb4;  /* (rb4: 0 for none) */
	} rates[3] = {
		{ 5700000000ull, 0x32, 0x0b, 0x25, 0 }, { 7980000000ull, 0xb2, 0x33, 0x1d, 0x03 }, { 10260000000ull, 0xb2, 0x33, 0x25, 0x02 },
	};
	unsigned r = 0;

	for (unsigned t = 0; t < 3; t++) {
		uint32_t b = trios[t][0], m = trios[t][1];

		if (!blk_put(csiphy, "CSIPHY", m + 0x10, 0x00, true) || !blk_put(csiphy, "CSIPHY", m + 0x14, 0x00, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x18, 0x00, true) || !blk_put(csiphy, "CSIPHY", m + 0x10, 0x08, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x14, 0x08, true) || !blk_put(csiphy, "CSIPHY", m + 0x18, 0x1a, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x0c, 0xaf, true)) {
			return false;
		}
		delay_for_interval(1, NSEC_PER_USEC);
		if (!blk_put(csiphy, "CSIPHY", b + 0x5c, 0x46, true) || !blk_put(csiphy, "CSIPHY", b + 0x68, 0xa0, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x04, 0x06, true) || !blk_put(csiphy, "CSIPHY", b + 0x0c, settle & 0xff, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x08, (settle >> 8) & 0xff, true) || !blk_put(csiphy, "CSIPHY", b + 0x14, 0x20, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x50, 0x00, true) || !blk_put(csiphy, "CSIPHY", b + 0x88, 0x00, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x8c, 0x7f, true) || !blk_put(csiphy, "CSIPHY", b + 0x90, 0x7f, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x18, 0x3e, true) || !blk_put(csiphy, "CSIPHY", b + 0x1c, 0x41, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x20, 0x41, true) || !blk_put(csiphy, "CSIPHY", b + 0x24, 0x7f, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x28, 0x00, true) || !blk_put(csiphy, "CSIPHY", b + 0x2c, 0x00, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x60, 0x02, true) || !blk_put(csiphy, "CSIPHY", b + 0xcc, 0x41, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0xdc, 0x50, true) || !blk_put(csiphy, "CSIPHY", m + 0x04, 0x20, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x08, 0x05, true) || !blk_put(csiphy, "CSIPHY", m + 0x00, 0x60, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x2c, 0x35, true) || !blk_put(csiphy, "CSIPHY", m + 0x30, 0x01, true) ||
		    !blk_put(csiphy, "CSIPHY", 0x800, 0x0e, true) || !blk_put(csiphy, "CSIPHY", 0x884, 0x01, true)) {
			return false;
		}
	}
	while (r < 2 && bits_per_trio > rates[r].bandwidth) {
		r++;
	}
	for (unsigned t = 0; t < 3; t++) {
		uint32_t b = trios[t][0], m = trios[t][1];

		if (!blk_put(csiphy, "CSIPHY", b + 0x44, rates[r].r144, true) || !blk_put(csiphy, "CSIPHY", b + 0x64, rates[r].r164, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x6c, rates[r].r16c, true) ||
		    (rates[r].rb4 != 0 && !blk_put(csiphy, "CSIPHY", m + 0x34, rates[r].rb4, true))) {
			return false;
		}
		if (rates[r].rb4 != 0) {
			delay_for_interval(1, NSEC_PER_USEC);
		}
	}
	say("munch-cam: CSIPHY %u: C-PHY rate row %u (up to %llu bit/s a trio) for %llu\n", 1u, r, rates[r].bandwidth, bits_per_trio);
	return true;
}

static bool
csiphy_dphy(unsigned settle)
{
	static const uint32_t lanes[5][2] = {           /* each lane position's two blocks: data lane 0, the clock lane, data lanes 1, 2, 3 */
		{ 0x000, 0x900 }, { 0x700, 0xc80 }, { 0x200, 0xa00 }, { 0x400, 0xb00 }, { 0x600, 0xc00 },
	};

	for (unsigned l = 0; l < 5; l++) {
		uint32_t b = lanes[l][0], m = lanes[l][1];
		bool clock = l == 1;

		if (!blk_put(csiphy, "CSIPHY", b + 0x30, 0x00, true) || !blk_put(csiphy, "CSIPHY", m + 0x0, 0x05, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x8, 0x10, true) || !blk_put(csiphy, "CSIPHY", m + 0x4, 0x00, true) ||
		    !blk_put(csiphy, "CSIPHY", m + 0x4, 0x07, true) || !blk_put(csiphy, "CSIPHY", b + 0x04, 0x0c, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x2c, 0x01, true) || !blk_put(csiphy, "CSIPHY", b + 0x34, 0x07, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x10, 0x02, true) || !blk_put(csiphy, "CSIPHY", b + 0x1c, 0x08, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x3c, 0xb8, true) || !blk_put(csiphy, "CSIPHY", b + 0x08, settle & 0xff, true) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x00, clock ? 0x80 : 0x8d, true) ||
		    (clock && !blk_put(csiphy, "CSIPHY", b + 0x0c, 0xa5, true)) ||
		    !blk_put(csiphy, "CSIPHY", b + 0x38, clock ? 0x1f : 0xfe, true) || !blk_put(csiphy, "CSIPHY", b + 0x14, 0x60, true) ||
		    (clock && !blk_put(csiphy, "CSIPHY", b + 0x28, 0x04, true)) || !blk_put(csiphy, "CSIPHY", 0x800, 0x02, true) ||
		    !blk_put(csiphy, "CSIPHY", 0x884, 0x01, true)) {
			return false;
		}
	}
	return true;
}

static bool
sensor_phy_up(unsigned phy, bool cphy, unsigned settle, uint64_t bits_per_lane)
{
	static const uint8_t irq_masks[11] = { 0xff, 0xff, 0xfb, 0xff, 0x7f, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff };
	csiphy = ml_io_map(0x0ac6a000UL + 0x2000 * phy, 0x2000);
	if (csiphy == 0) {
		return false;
	}
	wr(csiphy + 0x814, 0);
	delay_for_interval(5, NSEC_PER_USEC);
	wr(csiphy + 0x818, 0);
	wr(csiphy + 0x81c, 0);
	wr(csiphy + 0x800, 1);
	delay_for_interval(1, NSEC_PER_USEC);
	wr(csiphy + 0x800, 0);
	if (!blk_put(csiphy, "CSIPHY", 0x814, cphy ? 0x2a : 0xd5, true) ||
	    !blk_put(csiphy, "CSIPHY", 0x818, 1, true) ||
	    !blk_put(csiphy, "CSIPHY", 0x81c, cphy ? 0x52 : 2, true)) {
		return false;
	}
	wr(csiphy + 0x800, 3);
	delay_for_interval(1, NSEC_PER_USEC);
	wr(csiphy + 0x800, cphy ? 0x0e : 2);
	if (!(cphy ? csiphy_cphy(settle, bits_per_lane) : csiphy_dphy(settle))) {
		return false;
	}
	for (unsigned i = 0; i < 11; i++) {
		wr(csiphy + 0x82c + i * 4, irq_masks[i]);
		wr(csiphy + 0x858 + i * 4, 0xff);
	}
	wr(csiphy + 0x828, 1);
	wr(csiphy + 0x828, 0);
	say("munch-cam: CSIPHY %u configured, settle %u\n", phy, settle);
	return true;
}

static void
wide_phy_down(void)
{
	if (csiphy != 0) {
		wr(csiphy + 0x814, 0);
		delay_for_interval(5, NSEC_PER_USEC);
		wr(csiphy + 0x818, 0);
		wr(csiphy + 0x81c, 0);
		wr(csiphy + 0x800, 1);
		delay_for_interval(1, NSEC_PER_USEC);
		wr(csiphy + 0x800, 0);
		csiphy = 0;
	}
}

static const uint8_t *wide_tables, *wide_tables_end;
static bool wide_phy_vote, wide_timer_saved, wide_streaming;
static vm_offset_t refgen;
static uint32_t old_timer, old_timer_branch, old_phy_branch, old_refgen, timer_cmd;

static bool
sensor_stream_start(struct bus *bus, bool front, uint32_t stride, uint32_t height, uint32_t address)
{
	uint64_t length;
	const char *name = front ? "IMX596" : "OV64B40";
	unsigned mode_index = front ? 1 : WIDE_MODE, phy = front ? 4 : WIDE_PHY;
	unsigned lanes = front ? 4 : 3, lane_map = front ? 0x3210 : 0x210;
	const uint8_t *table = hakios_image_file("fw", front ? "camera-munch-imx596.tab" : "camera-munch-ov64b40.tab", &length, NULL);
	uint32_t head[8];
	struct wide_mode mode;

	if (frames == 0 || table == NULL || length < 40 || memcmp(table, "RWCTAB02", 8) != 0) {
		say("munch-cam: no %s table or frame buffer\n", name);
		return false;
	}
	memcpy(head, table + 8, sizeof(head));
	if (head[0] != 0x20 || head[4] != (front ? 0x0596 : 0x5664) || head[6] != lane_map || head[7] <= mode_index ||
	    head[7] > 16 || length < 40 + head[7] * sizeof(mode)) {
		say("munch-cam: unexpected %s table header\n", name);
		return false;
	}
	memcpy(&mode, table + 40 + mode_index * sizeof(mode), sizeof(mode));
	if (((mode.width * 5 / 4 + 15) & ~15u) != stride || mode.height != height || mode.lanes != lanes ||
	    mode.cphy != !front || mode.data_type != RAW10_DT || mode.settle > 0xffff) {
		say("munch-cam: unexpected %s mode\n", name);
		return false;
	}
	const uint8_t *at = table + 40 + head[7] * sizeof(mode);
	const uint8_t *end = table + length;
	for (unsigned i = 0; i < 2; i++) {
		uint32_t count;
		if ((size_t)(end - at) < 12) {
			return false;
		}
		memcpy(&count, at + 8, sizeof(count));
		if (count > (size_t)(end - at - 12) / sizeof(struct sensor_setting)) {
			return false;
		}
		at += 12 + count * sizeof(struct sensor_setting);
	}
	if (!wide_write_table(bus, at, end, "init") || !wide_write_table(bus, at, end, front ? "res1" : "res2") ||
	    !wide_write_table(bus, at, end, "tpgoff")) {
		return false;
	}
	csid = ml_io_map(CSID_PHYS, 0x1000);
	ife = ml_io_map(IFE_PHYS, 0x2200);
	if (!pll0_up() || !titan_clocks_up() || !smmu_up() || !csid_reset() ||
	    !csid_config(false, (lanes - 1) | lane_map << 4 | phy << 20 | (uint32_t)!front << 24) ||
	    !ife_config(stride, height, address)) {
		say("munch-cam: wide Titan setup failed\n");
		goto fail;
	}
	if (!hakios_munch_vreg("ldoa9", 1200, true, 7)) {
		goto fail;
	}
	wide_phy_vote = true;
	refgen = ml_io_map(0x088e7000UL, 0x1000);
	if (refgen == 0) {
		goto fail;
	}
	old_refgen = rd(refgen + 0x80);
	wr(refgen + 0x80, old_refgen | 1);
	delay_for_interval(5, NSEC_PER_USEC);
	timer_cmd = 0x6000 + 0x20 * phy;
	old_timer = rd(camcc_map + timer_cmd + 4);
	old_timer_branch = rd(camcc_map + timer_cmd + 0x18);
	old_phy_branch = rd(camcc_map + timer_cmd + 0x1c);
	wide_timer_saved = true;
	wr(camcc_map + timer_cmd + 4, (old_timer & ~0x371fu) | 2u << 8 | 3u);
	wr(camcc_map + timer_cmd, rd(camcc_map + timer_cmd) | 1);
	if (!camcc_wait(timer_cmd, 1, 0, 1000)) {
		goto fail;
	}
	wr(camcc_map + timer_cmd + 0x18, old_timer_branch | 1);
	wr(camcc_map + timer_cmd + 0x1c, old_phy_branch | 1);
	if (!camcc_wait(timer_cmd + 0x18, CBCR_OFF, 0, 1000) || !camcc_wait(timer_cmd + 0x1c, CBCR_OFF, 0, 1000) ||
	    !sensor_phy_up(phy, !front, mode.settle, (uint64_t)mode.pixel_clock * 10 / lanes)) {
		goto fail;
	}
	wide_tables = at;
	wide_tables_end = end;
	show_paths(front ? "front configured" : "wide configured");
	wide_streaming = true;
	if (wide_write_table(bus, at, end, "on")) {
		return true;
	}

fail:
	wide_stream_stop(bus);
	return false;
}

static void
wide_stream_stop(struct bus *bus)
{
	if (wide_streaming) {
		if (!leaving) {
			(void)wide_write_table(bus, wide_tables, wide_tables_end, "off");
		}
		wide_streaming = false;
	}
	wide_phy_down();
	if (wide_timer_saved) {
		wr(camcc_map + timer_cmd + 0x1c, old_phy_branch);
		wr(camcc_map + timer_cmd + 0x18, old_timer_branch);
		wr(camcc_map + timer_cmd + 4, old_timer);
		wr(camcc_map + timer_cmd, rd(camcc_map + timer_cmd) | 1);
		(void)camcc_wait(timer_cmd, 1, 0, 1000);
		wide_timer_saved = false;
	}
	if (refgen != 0) {
		wr(refgen + 0x80, old_refgen);
		refgen = 0;
	}
	if (wide_phy_vote && !leaving) {
		(void)hakios_munch_vreg("ldoa9", 0, false, 0);
		wide_phy_vote = false;
	}
	paths_down();
	smmu_down();
	titan_clocks_down();
	pll0_down();
	if (page_tables != 0 && !leaving) {
		kmem_free(kernel_map, page_tables, page_table_pages * 4096);
		page_tables = 0;
	}
}

static void
capture_wide(struct bus *bus)
{
	struct cam_header *h = (struct cam_header *)frames;

	if (frames == 0) {
		return;
	}
	frames_header(WIDE_WIDTH, WIDE_HEIGHT, WIDE_STRIDE, 3, "OV64B40");
	if (sensor_stream_start(bus, false, WIDE_STRIDE, WIDE_HEIGHT, FRAME_ADDRESSES + CAM_HEADER_BYTES)) {
		unsigned done = 0;
		for (unsigned ms = 0; ms < 1500 && !leaving; ms += 2) {
			wait_ms(2);
			if (!(rd(ife + BUS_IRQ_STATUS0) & (1u << 4))) {
				continue;
			}
			done++;
			say("munch-cam: OV64B40 frame %u done after %u ms\n", done, ms + 2);
			if (done == 4) {
				h->frames = 1;
				break;
			}
			wr(ife + BUS_IRQ_CLEAR0, 1u << 4);
			wr(ife + BUS_IRQ_GLOBAL_CLEAR, 1);
			wr(ife + WM0_IMAGE_ADDR, (uint32_t)(FRAME_ADDRESSES + CAM_HEADER_BYTES));
			wr(ife + IFE_REG_UPDATE, 1);
		}
		show_paths("after wide capture");
	}
	wide_stream_stop(bus);
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	say("munch-cam: OV64B40 capture finished, frames %u\n", h->frames);
}

/*
 * The common camera driver owns the device node and frame ring. Munch owns
 * sensor power, CSI routing, and the IFE-lite 480 register layout.
 */
static struct sensor_power live_power;
static const struct probe_sensor *live_sensor;
static bool live_powered, live_clocks;

/* Sunny OV64B40 CamX actuator data: CCI address 0x18, VAF L7P, DAC 180 + 400 steps. */
#define DW9800V_ADDRESS 0x0c
#define DW9800V_VAF_LDO 7
#define DW9800V_MIN 180u
#define DW9800V_MAX 580u
static bool vaf_enabled;
static uint32_t live_focus = HAKIOS_CAMERA_NO_FOCUS;

static bool
dw9800v_write(uint8_t reg, uint16_t value)
{
	struct bus *bus = &live_power.bus;
	if (!bus_take(bus)) {
		return false;
	}
	i2c_start(bus);
	bool ok = i2c_write(bus, DW9800V_ADDRESS << 1) && i2c_write(bus, reg);
	if (ok && reg == 0x03) {
		ok = i2c_write(bus, value >> 8);
	}
	ok = ok && i2c_write(bus, value & 0xff);
	i2c_stop(bus);
	bus_give_back(bus);
	return ok;
}

static void
dw9800v_down(void)
{
	if (live_focus != HAKIOS_CAMERA_NO_FOCUS) {
		while (live_focus > DW9800V_MIN) {
			uint32_t next = MAX(DW9800V_MIN, live_focus - 32);
			if (!dw9800v_write(0x03, next)) {
				break;
			}
			live_focus = next;
			wait_ms(2);
		}
		(void)dw9800v_write(0x02, 0x01);
		live_focus = HAKIOS_CAMERA_NO_FOCUS;
	}
	if (vaf_enabled) {
		broke = false;
		(void)pm8008_put(PM8008_REGULATORS, LDO_BASE(DW9800V_VAF_LDO) + LDO_ENABLE, 0);
		vaf_enabled = false;
	}
}

static void
dw9800v_up(void)
{
	struct bus *bus = &live_power.bus;
	uint8_t id = 0;
	if (!probe_ldo(DW9800V_VAF_LDO, 2800, &vaf_enabled)) {
		goto fail;
	}
	wait_ms(1);
	if (!bus_take(bus)) {
		goto fail;
	}
	i2c_start(bus);
	bool ok = i2c_write(bus, DW9800V_ADDRESS << 1) && i2c_write(bus, 0);
	if (ok) {
		i2c_start(bus);
		ok = i2c_write(bus, DW9800V_ADDRESS << 1 | 1);
		if (ok) {
			id = i2c_read(bus, false);
		}
	}
	i2c_stop(bus);
	bus_give_back(bus);
	if (!ok || id != 0xeb || !dw9800v_write(0x02, 0x00)) {
		goto fail;
	}
	wait_ms(1);
	if (!dw9800v_write(0x02, 0x02) || !dw9800v_write(0x06, 0x00) ||
	    !dw9800v_write(0x07, 0x73) || !dw9800v_write(0x03, DW9800V_MIN)) {
		(void)dw9800v_write(0x02, 0x01);
		goto fail;
	}
	live_focus = DW9800V_MIN;
	say("munch-cam: DW9800V focus ready at %u\n", live_focus);
	return;

fail:
	say("munch-cam: DW9800V focus setup failed (ID %#x)\n", id);
	dw9800v_down();
}

void
hakios_munch_camera_stream_focus(uint32_t position)
{
	if (live_focus == HAKIOS_CAMERA_NO_FOCUS) {
		return;
	}
	position = MAX(DW9800V_MIN, MIN(DW9800V_MAX, position));
	position = MAX(live_focus - 48, MIN(live_focus + 48, position));
	if (position != live_focus && dw9800v_write(0x03, position)) {
		live_focus = position;
	}
}

uint32_t
hakios_munch_camera_focus_position(uint32_t *minimum, uint32_t *maximum)
{
	if (live_focus != HAKIOS_CAMERA_NO_FOCUS) {
		*minimum = DW9800V_MIN;
		*maximum = DW9800V_MAX;
	}
	return live_focus;
}


void
hakios_munch_camera_stream_stop(void)
{
	dw9800v_down();
	wide_stream_stop(&live_power.bus);
	if (live_powered) {
		sensor_power_down(live_sensor, &live_power);
		live_powered = false;
	}
	if (live_clocks) {
		sensor_supply_down();
		live_clocks = false;
	}
	frames = 0;
	frames_bytes = 0;
}

bool
hakios_munch_camera_stream_start(vm_offset_t buffer, uint64_t size, uint32_t stride, uint32_t height, unsigned sensor)
{
	bool front = sensor == 2;

	if ((sensor != 1 && !front) || size < (uint64_t)stride * height || leaving) {
		return false;
	}
	tlmm_north = ml_io_map(TLMM_NORTH_PHYS, TLMM_TILE_SIZE);
	tlmm_south = ml_io_map(TLMM_SOUTH_PHYS, TLMM_TILE_SIZE);
	if (tlmm_north == 0 || tlmm_south == 0) {
		return false;
	}
	frames = buffer;
	frames_bytes = size;
	broke = false;
	live_clocks = true;
	if (!clocks_up() || !pm8008_release()) {
		goto fail;
	}
	memset(&live_power, 0, sizeof(live_power));
	live_sensor = front ? &front_probe : &wide_probe;
	const char *model = front ? "IMX596" : "OV64B40";
	live_powered = sensor_power_up(live_sensor, &live_power);
	if (!live_powered || !probe_id(&live_power.bus, 0x10, front ? 0x0016 : 0x300a, front ? 0x0596 : 0x5664, model) ||
	    !sensor_stream_start(&live_power.bus, front, stride, height, FRAME_ADDRESSES)) {
		goto fail;
	}
	if (!front) {
		dw9800v_up();
	}
	say("munch-cam: %s live stream started\n", model);
	return true;

fail:
	hakios_munch_camera_stream_stop();
	return false;
}

void
hakios_munch_camera_stream_status(bool *sof, bool *done)
{
	uint32_t top = rd(ife + IFE_IRQ_STATUS1);
	uint32_t bus = rd(ife + BUS_IRQ_STATUS0);

	*sof = (top & 1) != 0;
	*done = (bus & (1u << 4)) != 0;
	if (*sof) {
		wr(ife + IFE_IRQ_CLEAR1, 1);
		wr(ife + IFE_IRQ_GLOBAL_CLEAR, 1);
	}
	if (*done) {
		wr(ife + BUS_IRQ_CLEAR0, 1u << 4);
		wr(ife + BUS_IRQ_GLOBAL_CLEAR, 1);
	}
}

void
hakios_munch_camera_stream_queue(uint64_t offset)
{
	wr(ife + WM0_IMAGE_ADDR, (uint32_t)(FRAME_ADDRESSES + offset));
	wr(ife + IFE_REG_UPDATE, 1);
}

bool
hakios_munch_camera_stream_faulted(void)
{
	return (rd(smmu_context + SMMU_CB_FSR) & SMMU_FSR_ALL) != 0 ||
	    rd(ife + BUS_OVERFLOW_STATUS) != 0 || rd(ife + BUS_IMAGE_SIZE_VIOLATION) != 0;
}

bool
hakios_munch_camera_stream_exposure(uint32_t lines, uint32_t gain, uint32_t frame_lines)
{
	struct bus *bus = &live_power.bus;

	if (!wide_streaming || !wide_write_table(bus, wide_tables, wide_tables_end, "holdon")) {
		return false;
	}
	bool ok;
	if (live_sensor == &front_probe) {
		/* IMX596 encodes gain as 1024 - 1024 / G. The media layer uses linear Q8. */
		uint32_t code = 1024 - 262144 / gain;
		ok = wide_write(bus, 0x0340, frame_lines >> 8) && wide_write(bus, 0x0341, frame_lines & 0xff) &&
		    wide_write(bus, 0x0202, lines >> 8) && wide_write(bus, 0x0203, lines & 0xff) &&
		    wide_write(bus, 0x0204, code >> 8) && wide_write(bus, 0x0205, code & 0xff);
	} else {
		ok = wide_write(bus, 0x380e, frame_lines >> 8) && wide_write(bus, 0x380f, frame_lines & 0xff) &&
	    wide_write(bus, 0x3500, lines >> 16) && wide_write(bus, 0x3501, lines >> 8) &&
	    wide_write(bus, 0x3502, lines & 0xff) && wide_write(bus, 0x3508, gain >> 8) &&
	    wide_write(bus, 0x3509, gain & 0xff);
	}
	return wide_write_table(bus, wide_tables, wide_tables_end, "holdoff") && ok;
}

static bool titan_base_up;

static void
titan_down(void)
{
	paths_down();
	smmu_down();
	titan_clocks_down();
	pll0_down();
	if (titan_base_up) {
		clocks_down();          /* (camera_ahb, camcc's gdsc clock, titan_top_gdsc: as munch_cam=2 found them) */
		titan_base_up = false;
	}
}

static void
capture_tpg(void)
{
	struct cam_header *h;

	say("munch-cam: munch_cam=3: Titan 480's CSID-lite 0 test pattern through RDI 0 and IFE-lite 0 into memory, no sensor\n");
	if (frames == 0) {
		return;
	}
	frames_header(TPG_WIDTH, TPG_HEIGHT, TPG_STRIDE, 0, "CSID TPG");
	csid = ml_io_map(CSID_PHYS, 0x1000);
	ife = ml_io_map(IFE_PHYS, 0x2200);
	titan_base_up = true;           /* (whatever clocks_up() got as far as, clocks_down() puts back) */
	if (!clocks_up() || !pll0_up() || !titan_clocks_up() || !smmu_up() || !csid_reset() || !csid_config(true, 3 | 0x3210u << 4) || !ife_config(TPG_STRIDE, TPG_HEIGHT, FRAME_ADDRESSES + CAM_HEADER_BYTES)) {
		say("munch-cam: Titan not brought up: everything taken back\n");
		titan_down();
		return;
	}
	show_paths("configured");
	if (!blk_put(csid, "CSID", CSID_TPG_CTRL, 4u << 20 | 0x80u << 8 | 3u << 4 | 7u, true)) {
		titan_down();
		return;
	}
	tpg_running = true;
	h = (struct cam_header *)frames;
	for (unsigned ms = 0; ms < 2000; ms += 50) {
		wait_ms(50);
		if (rd(ife + BUS_IRQ_STATUS0) & 1u << 4) {
			h->frames = 1;
			say("munch-cam: RDI 0's write master says its frame is done after %u ms\n", ms + 50);
			wait_ms(100);           /* (a second one, for the host) */
			h->frames = 2;
			break;
		}
	}
	show_paths("after the frames");
	smmu_faults();
	titan_down();
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES + 256; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	const uint8_t *p = (const uint8_t *)(frames + CAM_HEADER_BYTES);
	say("munch-cam: the frame's first bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x ... (frames done: %u)\n", p[0], p[1],
	    p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], h->frames);
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
}

/* From drivers/camera/hakios.camera.c's hakios_camera_quiesce() on this phone: the run is ending, perhaps with a capture on. */
void
hakios_munch_camera_quiesce(void)
{
	if (!titan_base_up && !pll_on && !smmu_on && !csid_running && !ife_running) {
		return;
	}
	leaving = true;
	say_quiet = true;
	if (wide_streaming || wide_timer_saved) {
		wide_stream_stop(&live_power.bus);
	}
	if (live_powered) {
		wr(GPIO_IO(live_sensor->reset), 0);
		probe_pin_restore(&live_power.vana1);
		probe_pin_restore(&live_power.vana);
	}
	titan_down();
}

/* ---- the run ------------------------------------------------------------------------------------------------------ */

static uint32_t level;

static void
camera_thread(__unused void *parameter, __unused wait_result_t result)
{
	if (level == 3) {
		(void)frames_alloc(TPG_STRIDE, TPG_HEIGHT, CAM_FRAMES);
	} else if (level == 5) {
		(void)frames_alloc(WIDE_STRIDE, WIDE_HEIGHT, 1);
	}
	delay_for_interval(15, NSEC_PER_SEC);
	tlmm_north = ml_io_map(TLMM_NORTH_PHYS, TLMM_TILE_SIZE);
	tlmm_south = ml_io_map(TLMM_SOUTH_PHYS, TLMM_TILE_SIZE);
	say("munch-cam: a look at the cameras as the boot chain left them (munch_cam=1)\n");
	look_clocks();
	show_pin(95, "the macro's MCLK1");
	show_pin(92, "the macro's reset");
	show_pin(103, "CCI 0 master 1 data");
	show_pin(104, "CCI 0 master 1 clock");
	show_pin(94, "the wide's MCLK0");
	show_pin(93, "the wide's reset");
	show_pin(13, "the wide's VANA switch");
	show_pin(22, "the wide's VANA1 switch");
	look_pm8008();
	if (level == 2) {
		macro_chip_id();
	} else if (level == 4 || level == 5) {
		probe_cameras(level == 5);
	} else if (level == 3) {
		capture_tpg();
	}
	thread_terminate(current_thread());
}

__startup_func
static void
camera_init(void)
{
	thread_t thread;

	if (!hakios_soc_is("sm8250") || !PE_parse_boot_argn("munch_cam", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (kernel_thread_start(camera_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, camera_init);
