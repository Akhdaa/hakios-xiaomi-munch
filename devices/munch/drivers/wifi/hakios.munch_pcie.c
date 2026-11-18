/*
 * The POCO F4's (munch) first PCIe controller, where its Wi-Fi chip (a QCA6390) is: step 1 of the
 * Wi-Fi, which looks and does not touch, and the steps after it (pcie_probe=2 to 4, below), which do.
 * Only with boot-arg pcie_probe=N and on munch (hw=munch).
 *
 * STEP 1 ONLY READS. It says how the boot loader left the controller: its power switch
 * (pcie_0_gdsc) and its clocks in the global clock controller, the three pins the Wi-Fi hangs on
 * (the controller's reset of the card, PERST#, on GPIO 79; the card's WAKE# on GPIO 81; the
 * QCA6390's enable, wlan-en, on GPIO 20), and -- only when the switch is on and the clock of its
 * configuration registers runs, for a register without its clock does not answer and can stop
 * the bus -- the controller's own status registers ("PARF"): the state of the link's training.
 * Steps 2 to 4 power the card, bring the controller up and train the link; what follows that, the
 * card's MHI and the radio's driver, is for later steps.
 *
 * Numbers: the phone's own device tree (qcom,pcie@1c00000: parf 0x1c00000, phy 0x1c06000,
 * dm_core 0x60000000; perst-gpio, wake-gpio; qcom,cnss-qca6390: wlan-en-gpio; qcom,gdsc@16b004),
 * its Android kernel's drivers/clk/qcom/gcc-kona.c and pinctrl-kona.c, and Linux's
 * drivers/pci/controller/dwc/pcie-qcom.c (PARF register names).
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x90000UL
#define GCC_VOTE                0x52008         /* bits 0-5: PCIe 0's slv_axi, mstr_axi, cfg_ahb, aux, pipe, slv_q2a_axi */
#define GCC_PCIE_0_GDSCR        0x6b004         /* dt: qcom,gdsc@16b004 (pcie_0_gdsc): bit 31 on, bit 0 collapsed */
#define GCC_PCIE_0_SLV_Q2A_CBCR 0x6b010
#define GCC_PCIE_0_SLV_AXI_CBCR 0x6b014
#define GCC_PCIE_0_MSTR_AXI_CBCR 0x6b01c
#define GCC_PCIE_0_CFG_AHB_CBCR 0x6b024
#define GCC_PCIE_0_AUX_CBCR     0x6b028
#define GCC_PCIE_0_PIPE_CBCR    0x6b02c
#define GCC_PCIE0_PHY_REFGEN    0x6f02c
#define GCC_PCIE_WIFI_CLKREF    0x8c004         /* the reference clock out to the card */
#define CBCR_OFF                (1u << 31)
#define GDSC_POWER_ON           (1u << 31)

#define TLMM_NORTH_PHYS         0x0f900000UL    /* the three pins are all of the north tile */
#define TLMM_NORTH_SIZE         0x60000UL
#define PIN_WLAN_EN             20
#define PIN_PERST               79              /* a GPIO the driver moves (dt pcie0_perst_default) */
#define PIN_WAKE                81

#define PARF_PHYS               0x01c00000UL
#define PARF_SIZE               0x3000UL
#define PARF_SYS_CTRL           0x00
#define PARF_PM_STTS            0x24
#define PARF_PHY_CTRL           0x40            /* bit 0: the PHY in power-down */
#define PARF_LTSSM              0x1b0           /* [5:0] the link's training state (0x11: L0, up); bit 8 training enabled */

static void
show_clock(vm_offset_t gcc, const char *name, uint32_t offset)
{
	uint32_t value = rd(gcc + offset);

	printf("pcie: gcc %s (%#lx) = %#x: %s\n", name, GCC_PHYS + offset, value, (value & CBCR_OFF) ? "off" : "running");
}

static void
show_pin(vm_offset_t tlmm, const char *name, unsigned pin)
{
	uint32_t cfg = rd(tlmm + 0x1000UL * pin), io = rd(tlmm + 0x1000UL * pin + 4);

	printf("pcie: gpio%u (%s): cfg %#x (function %u, %s, pull %u, %u mA), in %u, out %u\n", pin, name, cfg, cfg >> 2 & 0xf,
	    (cfg & (1u << 9)) ? "output" : "input", cfg & 3, ((cfg >> 6 & 7) + 1) * 2, io & 1, io >> 1 & 1);
}

/* ======== steps 2 to 4: they write, and put back what they wrote when they are over ======================== */

/*
 *   pcie_probe=2   the QCA6390 powered as Android's cnss2 powers it (power.c, cnss_power_on_device):
 *                  its supplies voted for through RPMh in the order of cnss_vreg_list[] -- vdd-wlan-io
 *                  (smpa4, 1.8 V), -aon and -dig (smpa6, 0.95 V), -rfa1 (smpa5, 1.9 V), -rfa2 (smpc8,
 *                  1.35 V), -3antenna (ldoa16, 3.02 V) --, then wlan-en (GPIO 20, output high, 16 mA,
 *                  pull-up: cnss_wlan_en_active) after the 100 ms the chip wants after BT_EN
 *   pcie_probe=3   ... and the controller as pci-msm.c's msm_pcie_enable() brings it up, the card held in
 *                  reset (PERST# low): its PHY's supplies (ldoa9 1.2 V, ldoa5 0.88 V), its power switch,
 *                  its clocks (the auxiliary one at 19.2 MHz from the crystal), its two resets pulsed, the
 *                  PARF set up for a root port, the PHY's 99 writes of the device tree's
 *                  qcom,phy-sequence, the pipe clock, and the PHY waited for (qcom,phy-status-offset
 *                  0x814, bit 6 clear: ready)
 *   pcie_probe=4   ... and the card let out of reset after the device tree's qcom,ep-latency (10 ms),
 *                  the link trained (msm_pcie_link_train: XMLH_LINK_UP in ELBI_SYS_STTS and the data
 *                  link up in the link status, every 5 ms, 20 times at most), the first address
 *                  translation region pointed at the card's configuration space (bus 1, device 0, function
 *                  0, as msm_pcie_cfg_bdf), and that space read: QCA6390 is vendor 0x17cb, device 0x1101
 * pcie_hold=MS keeps what was reached that long before it is all taken down (default 1000). Nothing
 * of the card is started: no memory is given to it, no firmware, nothing goes through the SMMU.
 *
 * Numbers: gcc-kona.c (the clocks: votes in 0x52008, the auxiliary clock's source 0x6b038, the
 * reference clock out to the card 0x8c004, the PCIe TBU clocks 0x9000c and 0x8d058; resets
 * GCC_PCIE_0_BCR 0x6b000, GCC_PCIE_0_PHY_BCR 0x6c01c), pci-msm.c (PARF and DBI offsets), the live
 * device tree (qcom,pcie@1c00000: dm_core 0x60000000, elbi 0x60000f20, iatu 0x60001000, conf
 * 0x60100000; qcom,cnss-qca6390@b0000000; the pins pcie0_*_default and cnss_wlan_en_active).
 */
extern bool hakios_munch_vreg(const char *name, uint32_t millivolts, bool on, uint32_t mode);      /* hakios_munch_touch.c */
extern void loader_log_putc(char c);

static uint32_t step_level, hold_ms = 1000;
static uint32_t wait_s, slow_ms;      /* pcie_wait=S, pcie_slow=MS: see hakios_munch_pcie_init() */
static vm_offset_t gcc, tlmm, parf, phy, dbi, iatu, conf;
static char last_write[96] = "nothing written";

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char text[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(text, sizeof(text), format, list);
	va_end(list);
	for (const char *c = text; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", text);
	return length;
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
put(vm_offset_t base, uint32_t phys, uint32_t offset, uint32_t value)
{
	if (slow_ms != 0) {
		say("pcie: about to write %#x at %#x\n", value, phys + offset);
		sleep_us(slow_ms * 1000);
	}
	snprintf(last_write, sizeof(last_write), "wrote %#x at %#x", value, phys + offset);
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
put_mask(vm_offset_t base, uint32_t phys, uint32_t offset, uint32_t clear, uint32_t set)
{
	if (slow_ms != 0) {
		say("pcie: about to read %#x\n", phys + offset);
		sleep_us(slow_ms * 1000);
	}
	put(base, phys, offset, (rd(base + offset) & ~clear) | set);
}

#define GCC_PUT(o, v)           put(gcc, GCC_PHYS, (o), (v))
#define PARF_PUT(o, v)          put(parf, PARF_PHYS, (o), (v))
#define PARF_MASK(o, c, s)      put_mask(parf, PARF_PHYS, (o), (c), (s))
#define DBI_PHYS                0x60000000UL    /* dt: dm_core; elbi at +0xf20 */
#define IATU_PHYS               0x60001000UL
#define CONF_PHYS               0x60100000UL
#define PHY_PHYS                0x01c06000UL
#define DBI_PUT(o, v)           put(dbi, DBI_PHYS, (o), (v))
#define DBI_MASK(o, c, s)       put_mask(dbi, DBI_PHYS, (o), (c), (s))

/* ---- the pins ---------------------------------------------------------------------------------------- */

#define PIN_BT_EN               21
#define PIN_CLKREQ              80
#define CFG_PULL_DOWN           1u
#define CFG_PULL_UP             3u
#define CFG_FUNCTION(n)         ((n) << 2)
#define CFG_MA(n)               (((n) / 2 - 1) << 6)
#define CFG_OUTPUT              (1u << 9)

static const unsigned our_pins[] = { PIN_WLAN_EN, PIN_PERST, PIN_CLKREQ, PIN_WAKE };
static uint32_t found_cfg[4], found_io[4];
static bool pins_saved;

static void
pins_save(void)
{
	for (unsigned i = 0; i < 4; i++) {
		found_cfg[i] = rd(tlmm + 0x1000UL * our_pins[i]);
		found_io[i] = rd(tlmm + 0x1000UL * our_pins[i] + 4);
	}
	pins_saved = true;
}

static void
pin_put(unsigned pin, uint32_t cfg, uint32_t out)
{
	put(tlmm, TLMM_NORTH_PHYS, 0x1000u * pin + 4, out ? 2 : 0);
	put(tlmm, TLMM_NORTH_PHYS, 0x1000u * pin, cfg);
}

static void
pins_restore(void)
{
	if (!pins_saved) {
		return;
	}
	for (unsigned i = 0; i < 4; i++) {
		put(tlmm, TLMM_NORTH_PHYS, 0x1000u * our_pins[i], found_cfg[i]);
		put(tlmm, TLMM_NORTH_PHYS, 0x1000u * our_pins[i] + 4, found_io[i] & 2);
	}
	pins_saved = false;
}

/* ---- step 2: the QCA6390's supplies and wlan-en ---------------------------------------------------------- */

#define VRM_MODE_HPM            7               /* an LDO's high-power mode (the SMPS' modes are left alone) */

static const struct {
	const char *name, *what;
	uint32_t millivolts, mode;
} card_supplies[] = {
	{ "smpa4", "vdd-wlan-io", 1800, 0 },
	{ "smpa6", "vdd-wlan-aon, vdd-wlan-dig", 950, 0 },
	{ "smpa5", "vdd-wlan-rfa1", 1900, 0 },
	{ "smpc8", "vdd-wlan-rfa2", 1350, 0 },
	{ "ldoa16", "vdd-wlan-3antenna", 3020, VRM_MODE_HPM },
};
#define CARD_SUPPLIES           (sizeof(card_supplies) / sizeof(card_supplies[0]))
static unsigned card_supplies_on;
static bool wlan_en_on;

static bool
card_power_on(void)
{
	say("pcie: step 2: the QCA6390's supplies through RPMh, as cnss2 orders them\n");
	for (; card_supplies_on < CARD_SUPPLIES; card_supplies_on++) {
		say("pcie: %s (%s) at %u mV\n", card_supplies[card_supplies_on].name, card_supplies[card_supplies_on].what,
		    card_supplies[card_supplies_on].millivolts);
		if (!hakios_munch_vreg(card_supplies[card_supplies_on].name, card_supplies[card_supplies_on].millivolts, true,
		    card_supplies[card_supplies_on].mode)) {
			return false;
		}
	}
	say("pcie: BT_EN (gpio%u) is %u; 100 ms, then wlan-en (gpio%u) high\n", PIN_BT_EN,
	    rd(tlmm + 0x1000UL * PIN_BT_EN + 4) & 1, PIN_WLAN_EN);
	delay_for_interval(100, NSEC_PER_MSEC);
	pin_put(PIN_WLAN_EN, CFG_PULL_UP | CFG_MA(16) | CFG_OUTPUT, 1);
	wlan_en_on = true;
	sleep_us(1000);                         /* WLAN_ENABLE_DELAY */
	show_pin(tlmm, "wlan-en", PIN_WLAN_EN);
	return true;
}

static void
card_power_off(void)
{
	if (wlan_en_on) {
		pin_put(PIN_WLAN_EN, CFG_PULL_DOWN | CFG_MA(2), 0);     /* (cnss_wlan_en_sleep: low) */
		wlan_en_on = false;
		say("pcie: wlan-en low\n");
	}
	while (card_supplies_on > 0) {
		card_supplies_on--;
		hakios_munch_vreg(card_supplies[card_supplies_on].name, 0, false, 0);
	}
}

/* ---- step 3: the controller and its PHY ----------------------------------------------------------------- */

#define GCC_PCIE_0_BCR          0x6b000
#define GCC_PCIE_0_PHY_BCR      0x6c01c
#define GCC_PCIE_0_AUX_CMD      0x6b038         /* bit 0: take the configuration, bit 31: off */
#define GCC_PCIE_0_AUX_CFG      0x6b03c         /* [4:0] 2 x divider - 1, [10:8] source (0: the crystal) */
#define GCC_TBU_CBCR            0x9000c         /* gcc_aggre_noc_pcie_tbu_clk */
#define GCC_SF_TBU_CBCR         0x8d058         /* gcc_ddrss_pcie_sf_tbu_clk */
#define VOTE_PIPE               (1u << 4)
#define VOTE_OTHERS             0x2fu           /* slv_axi, mstr_axi, cfg_ahb, aux, slv_q2a */
#define GDSC_COLLAPSE           1u

#define PARF_DEVICE_TYPE        0x1000          /* 4: a root port */
#define PARF_PM_CTRL            0x20
#define PARF_DBI_BASE_ADDR      0x350
#define PARF_SLV_ADDR_SPACE     0x358
#define PARF_MHI_CLOCK_RESET    0x174
#define PARF_AXI_MSTR_WR_HALT   0x1a8
#define PARF_INT_ALL_MASK       0x22c
#define PHY_STATUS              0x814           /* dt: qcom,phy-status-offset; bit 6 set: not ready */
#define PHY_POWER_DOWN          0x840           /* dt: qcom,phy-power-down-offset */

/* dt: qcom,phy-sequence: offset, value, delay in us (none of them has one) */
static const struct {
	uint16_t offset;
	uint8_t value, delay;
} phy_sequence[] = {
	{ 0x840, 0x03 }, { 0x094, 0x08 }, { 0x154, 0x34 }, { 0x16c, 0x08 }, { 0x058, 0x0f }, { 0x0a4, 0x42 },
	{ 0x110, 0x24 }, { 0x11c, 0x03 }, { 0x118, 0xb4 }, { 0x10c, 0x02 }, { 0x1bc, 0x11 }, { 0x0bc, 0x82 },
	{ 0x0d4, 0x03 }, { 0x0d0, 0x55 }, { 0x0cc, 0x55 }, { 0x0b0, 0x1a }, { 0x0ac, 0x0a }, { 0x0c4, 0x68 },
	{ 0x0e0, 0x02 }, { 0x0dc, 0xaa }, { 0x0d8, 0xab }, { 0x0b8, 0x34 }, { 0x0b4, 0x14 }, { 0x158, 0x01 },
	{ 0x074, 0x06 }, { 0x07c, 0x16 }, { 0x084, 0x36 }, { 0x078, 0x06 }, { 0x080, 0x16 }, { 0x088, 0x36 },
	{ 0x1b0, 0x1e }, { 0x1ac, 0xca }, { 0x1b8, 0x18 }, { 0x1b4, 0xa2 }, { 0x050, 0x07 }, { 0x010, 0x01 },
	{ 0x01c, 0x31 }, { 0x020, 0x01 }, { 0x024, 0xde }, { 0x028, 0x07 }, { 0x030, 0x4c }, { 0x034, 0x06 },
	{ 0x29c, 0x12 }, { 0x284, 0x35 }, { 0x23c, 0x11 }, { 0x51c, 0x03 }, { 0x518, 0x1c }, { 0x524, 0x1e },
	{ 0x4e8, 0x00 }, { 0x4ec, 0x0e }, { 0x4f0, 0x4a }, { 0x4f4, 0x0f }, { 0x5b4, 0x04 }, { 0x434, 0x7f },
	{ 0x444, 0x70 }, { 0x510, 0x17 }, { 0x4d4, 0x04 }, { 0x4d8, 0x07 }, { 0x598, 0xd4 }, { 0x59c, 0x54 },
	{ 0x5a0, 0xdb }, { 0x5a4, 0x3b }, { 0x5a8, 0x31 }, { 0x584, 0x24 }, { 0x588, 0xe4 }, { 0x58c, 0xec },
	{ 0x590, 0x3b }, { 0x594, 0x36 }, { 0x570, 0x3f }, { 0x574, 0x3f }, { 0x578, 0xff }, { 0x57c, 0x7f },
	{ 0x580, 0x14 }, { 0x4fc, 0x00 }, { 0x4f8, 0xc0 }, { 0x460, 0x30 }, { 0x464, 0x00 }, { 0x5bc, 0x0c },
	{ 0x4dc, 0x1b }, { 0x408, 0x0c }, { 0x414, 0x03 }, { 0x5b8, 0x30 }, { 0x9a4, 0x01 }, { 0xc90, 0x00 },
	{ 0xc40, 0x01 }, { 0xc48, 0x01 }, { 0xc50, 0x00 }, { 0xcb4, 0x33 }, { 0xcbc, 0x00 }, { 0xce0, 0x58 },
	{ 0xca4, 0x0f }, { 0x048, 0x90 }, { 0xc1c, 0xc1 }, { 0x988, 0x77 }, { 0x998, 0x0b }, { 0x8dc, 0x0d },
	{ 0x9ec, 0x12 }, { 0x800, 0x00 }, { 0x844, 0x03 },
};

static uint32_t saved_vote, saved_aux_cfg, saved_tbu, saved_sf_tbu, saved_clkref;
static bool controller_vregs, gdsc_on, clocks_on, pipe_on, phy_on;

static bool
gdsc_wait(bool on)
{
	for (unsigned i = 0; i < 100; i++) {
		if (!!(rd(gcc + GCC_PCIE_0_GDSCR) & GDSC_POWER_ON) == on) {
			return true;
		}
		sleep_us(10);
	}
	return false;
}

static bool
controller_up(void)
{
	say("pcie: step 3: the card held in reset (PERST# low), the controller's supplies, power switch and clocks\n");
	pin_put(PIN_PERST, CFG_PULL_DOWN | CFG_MA(2) | CFG_OUTPUT, 0);
	pin_put(PIN_CLKREQ, CFG_PULL_UP | CFG_MA(2) | CFG_FUNCTION(1), 0);
	pin_put(PIN_WAKE, CFG_PULL_UP | CFG_MA(2), 0);
	sleep_us(1000);                         /* PERST_PROPAGATION_DELAY_US */
	controller_vregs = true;
	if (slow_ms != 0) {
		say("pcie: about to vote ldoa9 1200 mV and ldoa5 880 mV (RPMh)\n");
		sleep_us(slow_ms * 1000);
	}
	if (!hakios_munch_vreg("ldoa9", 1200, true, VRM_MODE_HPM) || !hakios_munch_vreg("ldoa5", 880, true, VRM_MODE_HPM)) {
		return false;
	}
	GCC_PUT(GCC_PCIE_0_GDSCR, rd(gcc + GCC_PCIE_0_GDSCR) & ~GDSC_COLLAPSE);
	gdsc_on = true;
	if (!gdsc_wait(true)) {
		say("pcie: pcie_0_gdsc does not come on (%#x)\n", rd(gcc + GCC_PCIE_0_GDSCR));
		return false;
	}
	say("pcie: pcie_0_gdsc on (%#x)\n", rd(gcc + GCC_PCIE_0_GDSCR));
	saved_vote = rd(gcc + GCC_VOTE);
	saved_aux_cfg = rd(gcc + GCC_PCIE_0_AUX_CFG);
	saved_tbu = rd(gcc + GCC_TBU_CBCR);
	saved_sf_tbu = rd(gcc + GCC_SF_TBU_CBCR);
	saved_clkref = rd(gcc + GCC_PCIE_WIFI_CLKREF);
	clocks_on = true;
	GCC_PUT(GCC_PCIE_0_AUX_CFG, 0 << 8 | 1);        /* 19.2 MHz: the crystal, divided by 1 */
	GCC_PUT(GCC_PCIE_0_AUX_CMD, rd(gcc + GCC_PCIE_0_AUX_CMD) | 1);
	for (unsigned i = 0; i < 100 && (rd(gcc + GCC_PCIE_0_AUX_CMD) & 1); i++) {
		sleep_us(10);
	}
	/*
	 * The two clocks of the SMMU's PCIe TBU: only for DMA (stream 0x1c00), not for the link. On the phone, with nothing
	 * else of the PCIe up, 0x9000c reads 0 (not even a CBCR's "off" bit) and writing it stopped the whole chip (run of
	 * 29 Oct 22:03): such a register is left alone until DMA needs it.
	 */
	say("pcie: aggre_noc_pcie_tbu (%#lx) = %#x, ddrss_pcie_sf_tbu (%#lx) = %#x\n", GCC_PHYS + GCC_TBU_CBCR, saved_tbu,
	    GCC_PHYS + GCC_SF_TBU_CBCR, saved_sf_tbu);
	if (saved_tbu != 0) {
		GCC_PUT(GCC_TBU_CBCR, saved_tbu | 1);
	}
	if (saved_sf_tbu != 0) {
		GCC_PUT(GCC_SF_TBU_CBCR, saved_sf_tbu | 1);
	}
	GCC_PUT(GCC_PCIE_WIFI_CLKREF, saved_clkref | 1);
	GCC_PUT(GCC_PCIE0_PHY_REFGEN, rd(gcc + GCC_PCIE0_PHY_REFGEN) | 1);
	GCC_PUT(GCC_VOTE, saved_vote | VOTE_OTHERS);
	sleep_us(100);
	show_clock(gcc, "pcie_0_aux", GCC_PCIE_0_AUX_CBCR);
	show_clock(gcc, "pcie_0_cfg_ahb", GCC_PCIE_0_CFG_AHB_CBCR);
	show_clock(gcc, "pcie_0_mstr_axi", GCC_PCIE_0_MSTR_AXI_CBCR);
	show_clock(gcc, "pcie_0_slv_axi", GCC_PCIE_0_SLV_AXI_CBCR);
	show_clock(gcc, "pcie_wifi_clkref", GCC_PCIE_WIFI_CLKREF);
	if (rd(gcc + GCC_PCIE_0_CFG_AHB_CBCR) & CBCR_OFF) {
		say("pcie: the clock of the controller's registers is off: not touching them\n");
		return false;
	}
	say("pcie: the controller's and the PHY's resets pulsed (gcc %#lx, %#lx)\n", GCC_PHYS + GCC_PCIE_0_BCR,
	    GCC_PHYS + GCC_PCIE_0_PHY_BCR);
	GCC_PUT(GCC_PCIE_0_BCR, 1);
	sleep_us(1000);
	GCC_PUT(GCC_PCIE_0_BCR, 0);
	GCC_PUT(GCC_PCIE_0_PHY_BCR, 1);
	sleep_us(1000);
	GCC_PUT(GCC_PCIE_0_PHY_BCR, 0);

	say("pcie: the PARF set up for a root port (msm_pcie_enable)\n");
	PARF_PUT(PARF_DEVICE_TYPE, 4);
	PARF_MASK(PARF_PM_CTRL, 1u << 5, 0);
	PARF_MASK(PARF_PHY_CTRL, 1u << 0, 0);
	PARF_PUT(PARF_DBI_BASE_ADDR, 0);
	PARF_PUT(PARF_SYS_CTRL, 0x365e);
	PARF_MASK(PARF_MHI_CLOCK_RESET, 0, 1u << 4);
	PARF_PUT(PARF_INT_ALL_MASK, 0);
	PARF_PUT(PARF_SLV_ADDR_SPACE, 0x4000000);       /* dt: qcom,slv-addr-space-size */
	PARF_MASK(PARF_AXI_MSTR_WR_HALT, 0, 1u << 31);

	say("pcie: the PHY's %u writes (dt qcom,phy-sequence), then its pipe clock\n",
	    (unsigned)(sizeof(phy_sequence) / sizeof(phy_sequence[0])));
	phy_on = true;
	for (unsigned i = 0; i < sizeof(phy_sequence) / sizeof(phy_sequence[0]); i++) {
		put(phy, PHY_PHYS, phy_sequence[i].offset, phy_sequence[i].value);
		if (phy_sequence[i].delay) {
			sleep_us(phy_sequence[i].delay);
		}
	}
	sleep_us(1000);                         /* PHY_STABILIZATION_DELAY_US */
	GCC_PUT(GCC_VOTE, rd(gcc + GCC_VOTE) | VOTE_PIPE);
	pipe_on = true;
	for (unsigned i = 0; i < 10 && (rd(phy + PHY_STATUS) & 1u << 6); i++) {
		sleep_us(1000);
	}
	show_clock(gcc, "pcie_0_pipe", GCC_PCIE_0_PIPE_CBCR);
	if (rd(phy + PHY_STATUS) & 1u << 6) {
		say("pcie: THE PHY DOES NOT COME UP (status %#x)\n", rd(phy + PHY_STATUS));
		return false;
	}
	say("pcie: THE PHY IS READY (status %#x)\n", rd(phy + PHY_STATUS));
	return true;
}

static void
controller_down(void)
{
	if (phy_on) {
		put(phy, PHY_PHYS, PHY_POWER_DOWN, 0);
		phy_on = false;
	}
	if (pipe_on) {
		GCC_PUT(GCC_VOTE, rd(gcc + GCC_VOTE) & ~VOTE_PIPE);
		pipe_on = false;
	}
	if (clocks_on) {
		GCC_PUT(GCC_VOTE, (rd(gcc + GCC_VOTE) & ~VOTE_OTHERS) | (saved_vote & VOTE_OTHERS));
		GCC_PUT(GCC_PCIE_WIFI_CLKREF, saved_clkref);
		if (saved_sf_tbu != 0) {
			GCC_PUT(GCC_SF_TBU_CBCR, saved_sf_tbu);
		}
		if (saved_tbu != 0) {
			GCC_PUT(GCC_TBU_CBCR, saved_tbu);
		}
		GCC_PUT(GCC_PCIE_0_AUX_CFG, saved_aux_cfg);
		GCC_PUT(GCC_PCIE_0_AUX_CMD, rd(gcc + GCC_PCIE_0_AUX_CMD) | 1);
		clocks_on = false;
	}
	if (gdsc_on) {
		GCC_PUT(GCC_PCIE_0_GDSCR, rd(gcc + GCC_PCIE_0_GDSCR) | GDSC_COLLAPSE);
		say("pcie: pcie_0_gdsc %s (%#x)\n", gdsc_wait(false) ? "off" : "DOES NOT GO OFF", rd(gcc + GCC_PCIE_0_GDSCR));
		gdsc_on = false;
	}
	if (controller_vregs) {
		hakios_munch_vreg("ldoa5", 0, false, 0);
		hakios_munch_vreg("ldoa9", 0, false, 0);
		controller_vregs = false;
	}
}

/* ---- step 4: the link, and the card's configuration space -------------------------------------------- */

#define ELBI_SYS_STTS           (0xf20 + 0x08)  /* bit 10: XMLH_LINK_UP; [17:12] the training state */
#define DBI_LINK_STATUS         0x80            /* PCIE20_CAP + 0x10: [19:16] speed, [25:20] width, bit 29 the data link up */
#define DBI_DEVICE_CONTROL      0x78
#define DBI_GEN2_CTRL           0x80c
#define DBI_GEN3_RELATED        0x890
#define DBI_GEN3_MISC_CONTROL   0x8bc
#define DBI_GEN3_SPCIE_CAP      0x154
#define DBI_AUX_CLK_FREQ        0xb40
#define IATU_CTRL1(n)           (0x200 * (n) + 0x00)
#define IATU_CTRL2(n)           (0x200 * (n) + 0x04)
#define IATU_LBAR(n)            (0x200 * (n) + 0x08)
#define IATU_UBAR(n)            (0x200 * (n) + 0x0c)
#define IATU_LAR(n)             (0x200 * (n) + 0x10)
#define IATU_LTAR(n)            (0x200 * (n) + 0x14)
#define IATU_UTAR(n)            (0x200 * (n) + 0x18)
#define IATU_TYPE_CFG0          4
#define QCA6390_ID              0x110117cbu     /* device 0x1101, vendor 0x17cb */

static bool perst_released, training, iatu_on;

static bool
link_up(void)
{
	uint32_t stts = 0, status = 0;

	say("pcie: step 4: %u ms (dt qcom,ep-latency), then the card let out of reset (PERST# high)\n", 10);
	delay_for_interval(10, NSEC_PER_MSEC);
	pin_put(PIN_PERST, CFG_PULL_DOWN | CFG_MA(2) | CFG_OUTPUT, 1);
	perst_released = true;
	sleep_us(1000);
	say("pcie: the link's training (msm_pcie_link_train)\n");
	DBI_MASK(DBI_GEN2_CTRL, 0x1f00, 1u << 8);
	DBI_MASK(DBI_GEN3_RELATED, 1u << 0, 0);
	DBI_MASK(DBI_GEN3_MISC_CONTROL, 0, 1u << 0);
	DBI_PUT(DBI_GEN3_SPCIE_CAP, 0x55555555);        /* PCIE_GEN3_PRESET_DEFAULT */
	DBI_MASK(DBI_GEN3_MISC_CONTROL, 1u << 0, 0);
	DBI_MASK(DBI_DEVICE_CONTROL, 0x7000, 5u << 12);  /* PCIE_TLP_RD_SIZE */
	PARF_MASK(PARF_LTSSM, 0, 1u << 8);
	training = true;
	for (unsigned i = 0; i <= 20; i++) {
		delay_for_interval(5, NSEC_PER_MSEC);
		stts = rd(dbi + ELBI_SYS_STTS);
		status = rd(dbi + DBI_LINK_STATUS);
		if ((stts & 1u << 10) && (status & 1u << 29)) {
			break;
		}
	}
	say("pcie: elbi sys_stts %#x (training state %#x), link status %#x, parf ltssm %#x\n", stts, stts >> 12 & 0x3f, status,
	    rd(parf + PARF_LTSSM));
	if (!(stts & 1u << 10) || !(status & 1u << 29)) {
		say("pcie: THE LINK DOES NOT COME UP\n");
		return false;
	}
	say("pcie: THE LINK IS UP: gen %u, x%u; the root port is %#x\n", status >> 16 & 0xf, status >> 20 & 0x3f, rd(dbi));
	return true;
}

static bool
card_config(void)
{
	uint32_t id = 0xffffffff;

	say("pcie: the first translation region to the card's configuration space (bus 1, device 0)\n");
	put(iatu, IATU_PHYS, IATU_CTRL2(0), 0);
	put(iatu, IATU_PHYS, IATU_CTRL1(0), IATU_TYPE_CFG0);
	put(iatu, IATU_PHYS, IATU_LBAR(0), (uint32_t)CONF_PHYS);
	put(iatu, IATU_PHYS, IATU_UBAR(0), 0);
	put(iatu, IATU_PHYS, IATU_LAR(0), (uint32_t)CONF_PHYS + 0xfff);
	put(iatu, IATU_PHYS, IATU_LTAR(0), 1u << 24);
	put(iatu, IATU_PHYS, IATU_UTAR(0), 0);
	put(iatu, IATU_PHYS, IATU_CTRL2(0), 1u << 31);
	iatu_on = true;
	DBI_PUT(DBI_AUX_CLK_FREQ, 0x14);                /* dt: qcom,use-19p2mhz-aux-clk */
	for (unsigned ms = 0; ms < 1000 && (id = rd(conf)) == 0xffffffff; ms++) {
		sleep_us(1000);
	}
	if (id == 0xffffffff) {
		say("pcie: the card's configuration space does not answer\n");
		return false;
	}
	say("pcie: THE CARD ANSWERS: vendor %#x, device %#x%s\n", id & 0xffff, id >> 16, id == QCA6390_ID ? " (QCA6390)" :
	    " (not the QCA6390 expected)");
	say("pcie: card: command/status %#x, class %#x, header %#x, BAR0 %#x, BAR1 %#x, subsystem %#x\n", rd(conf + 4),
	    rd(conf + 8), rd(conf + 12), rd(conf + 0x10), rd(conf + 0x14), rd(conf + 0x2c));
	return id == QCA6390_ID;
}

/* ---- step 5: the card's registers (BAR0) and its MHI, read ------------------------------------------- */

/*
 *   pcie_probe=5   ... and the card's memory: the root port numbered as a bridge to bus 1 with a memory window
 *                  (Linux's enumeration does that, pci-msm.c leaves it to it), the controller's table that
 *                  gives each requester its SMMU stream (msm_pcie_config_sid: bus 1 is stream 0x1c00 + 1),
 *                  the card's BAR0 sized and put at the start of the controller's memory range (dt ranges:
 *                  0x60300000, 1:1: no translation region is needed for memory, what no region matches goes
 *                  out untranslated), memory decoding switched on in both -- not bus mastering: the card is
 *                  given nothing to reach --, the capabilities listed, and what an MHI host reads first: the
 *                  MHI registers at the start of BAR0, the BHI ones where they point, and three words of the
 *                  card's own through the register window (ath11k's pci.c)
 */
#define MEM_PHYS                0x60300000UL    /* dt: ranges, 32-bit memory: 0x60300000 + 0x3d00000 */
#define CONF_COMMAND            0x04            /* bit 1 memory, bit 2 bus master */
#define CONF_BAR0               0x10
#define CONF_BAR1               0x14
#define CONF_CAPABILITIES       0x34
#define DBI_BUS_NUMBERS         0x18
#define DBI_MEMORY_WINDOW       0x20
#define PARF_BDF_TO_SID         0x2000          /* 256 words: bdf [31:16], stream - base [15:8], next [7:0] */
#define BDF_BUS1_HASH           0x15            /* crc8 (polynomial 7, msb first) of the bdf 0x0100, big-endian */
#define MHI_REGLEN              0x00
#define MHI_VER                 0x08
#define MHI_CFG                 0x10            /* [31:24] hardware event rings, [23:16] event rings, [15:8] hardware channels, [7:0] channels */
#define MHI_CHDBOFF             0x18
#define MHI_ERDBOFF             0x20
#define MHI_BHIOFF              0x28
#define MHI_BHIEOFF             0x2c
#define MHI_CTRL                0x38            /* [15:8] the state asked for, bit 1 reset */
#define MHI_STATUS              0x48            /* [15:8] the state, bit 2 system error, bit 0 ready */
#define BHI_VERSION_MINOR       0x00
#define BHI_VERSION_MAJOR       0x04
#define BHI_EXECENV             0x28
#define BHI_STATUS              0x2c
#define BHI_ERRCODE             0x30
#define BHI_SERIALNU            0x40
#define CARD_HW_VERSION         0x224           /* TCSR_SOC_HW_VERSION: [11:8] major, [7:0] minor */
#define CARD_WINDOW             0x310c          /* ATH11K_PCI_WINDOW_REG_ADDRESS: bit 30 on, [5:0] offset [24:19] */
#define CARD_WINDOW_START       0x80000

static const char *const mhi_states[] = { "RESET", "READY", "M0", "M1", "M2", "M3", "M3_FAST", "BHI" };
static const char *const exec_envs[] = { "PBL", "SBL", "AMSS", "RDDM", "WFW", "PTHRU", "EDL", "FP" };
static vm_offset_t bar;
static uint32_t bar_size;
static uint32_t found_rc_command, found_bus_numbers, found_window, found_bar0, found_bar1;
static bool card_mem_on;

static uint32_t
card_far(uint32_t offset)
{
	put(bar, MEM_PHYS, CARD_WINDOW, 1u << 30 | (offset >> 19 & 0x3f));
	(void)rd(bar + CARD_WINDOW);
	return rd(bar + CARD_WINDOW_START + (offset & 0x7ffff));
}

static bool
card_registers(void)
{
	uint32_t low, high, size, at, bhi, state;

	say("pcie: step 5: the root port as the bridge to bus 1, its stream table, the card's BAR0 at %#lx\n", MEM_PHYS);
	found_rc_command = rd(dbi + CONF_COMMAND);
	found_bus_numbers = rd(dbi + DBI_BUS_NUMBERS);
	found_window = rd(dbi + DBI_MEMORY_WINDOW);
	found_bar0 = rd(conf + CONF_BAR0);
	found_bar1 = rd(conf + CONF_BAR1);
	card_mem_on = true;
	DBI_PUT(DBI_BUS_NUMBERS, (found_bus_numbers & 0xff000000) | 1u << 16 | 1u << 8);       /* primary 0, secondary 1, last 1 */
	for (unsigned i = 0; i < 256; i++) {
		PARF_PUT(PARF_BDF_TO_SID + 4 * i, 0);
	}
	PARF_PUT(PARF_BDF_TO_SID + 4 * BDF_BUS1_HASH, 0x0100u << 16 | 1u << 8);

	put(conf, CONF_PHYS, CONF_BAR0, 0xffffffff);
	put(conf, CONF_PHYS, CONF_BAR1, 0xffffffff);
	low = rd(conf + CONF_BAR0);
	high = rd(conf + CONF_BAR1);
	size = (high != 0xffffffff || (low & ~0xfu) == 0) ? 0 : ~(low & ~0xfu) + 1;
	say("pcie: BAR0 sized: %#x %#x: %u KB%s\n", low, high, size >> 10, (low & 6) == 4 ? ", 64-bit" : "");
	if (size < 2 * CARD_WINDOW_START || size > 0x400000) {
		say("pcie: BAR0's size is not what ath11k's register window needs (1 MB to 4 MB): not mapped\n");
		return false;
	}
	put(conf, CONF_PHYS, CONF_BAR0, (uint32_t)MEM_PHYS | (low & 0xf));
	put(conf, CONF_PHYS, CONF_BAR1, 0);
	DBI_PUT(DBI_MEMORY_WINDOW, ((uint32_t)(MEM_PHYS + size - 1) & 0xfff00000) | ((uint32_t)MEM_PHYS >> 16 & 0xfff0));
	DBI_PUT(CONF_COMMAND, (found_rc_command & ~7u) | 1u << 1);
	put(conf, CONF_PHYS, CONF_COMMAND, (rd(conf + CONF_COMMAND) & ~7u) | 1u << 1);
	say("pcie: root port: command %#x, buses %#x, window %#x; card: command %#x, BAR0 %#x %#x\n", rd(dbi + CONF_COMMAND),
	    rd(dbi + DBI_BUS_NUMBERS), rd(dbi + DBI_MEMORY_WINDOW), rd(conf + CONF_COMMAND), rd(conf + CONF_BAR0), rd(conf + CONF_BAR1));

	for (at = rd(conf + CONF_CAPABILITIES) & 0xfc; at != 0 && at < 0x100; at = rd(conf + at) >> 8 & 0xfc) {
		uint32_t word = rd(conf + at);

		say("pcie: card capability %#x at %#x: %#x %#x %#x%s\n", word & 0xff, at, word, rd(conf + at + 4), rd(conf + at + 8),
		    (word & 0xff) == 5 ? " (MSI)" : (word & 0xff) == 0x10 ? " (PCIe)" : (word & 0xff) == 1 ? " (power)" : "");
		if ((word & 0xff) == 5) {
			say("pcie: MSI: %u vectors possible, %u on, %s addresses, %s\n", 1u << (word >> 17 & 7), 1u << (word >> 20 & 7),
			    (word & 1u << 23) ? "64-bit" : "32-bit", (word & 1u << 16) ? "ON" : "off");
		}
	}
	for (at = 0x100; at != 0 && at < 0x1000; at = rd(conf + at) >> 20 & 0xffc) {
		uint32_t word = rd(conf + at);

		if (word == 0 || word == 0xffffffff) {
			break;
		}
		say("pcie: card extended capability %#x (version %u) at %#x\n", word & 0xffff, word >> 16 & 0xf, at);
	}

	if (bar == 0) {
		bar = ml_io_map(MEM_PHYS, size);
	}
	bar_size = size;
	say("pcie: MHI: reglen %#x, version %#x, cfg %#x (%u channels, %u event rings), chdb at %#x, erdb at %#x, bhi at %#x, "
	    "bhie at %#x\n", rd(bar + MHI_REGLEN), rd(bar + MHI_VER), rd(bar + MHI_CFG), rd(bar + MHI_CFG) & 0xff,
	    rd(bar + MHI_CFG) >> 16 & 0xff, rd(bar + MHI_CHDBOFF), rd(bar + MHI_ERDBOFF), rd(bar + MHI_BHIOFF), rd(bar + MHI_BHIEOFF));
	state = rd(bar + MHI_STATUS);
	say("pcie: MHI: ctrl %#x, status %#x: state %s%s%s\n", rd(bar + MHI_CTRL), state,
	    (state >> 8 & 0xff) < 8 ? mhi_states[state >> 8 & 0xff] : (state >> 8 & 0xff) == 0xff ? "SYS_ERR" : "?", (state & 1) ? ", ready" : "",
	    (state & 4) ? ", SYSTEM ERROR" : "");
	bhi = rd(bar + MHI_BHIOFF);
	if (bhi != 0 && bhi < size - 0x100) {
		uint32_t env = rd(bar + bhi + BHI_EXECENV);

		say("pcie: BHI: version %u.%u, execution environment %u (%s), status %#x, error %#x, serial %#x\n",
		    rd(bar + bhi + BHI_VERSION_MAJOR), rd(bar + bhi + BHI_VERSION_MINOR), env, env < 8 ? exec_envs[env] : "?",
		    rd(bar + bhi + BHI_STATUS), rd(bar + bhi + BHI_ERRCODE), rd(bar + bhi + BHI_SERIALNU));
	}
	say("pcie: card: hardware version %#x (major %u, minor %u); through the window: parf ltssm %#x, q6 cookie %#x, warm "
	    "entry %#x\n", rd(bar + CARD_HW_VERSION), rd(bar + CARD_HW_VERSION) >> 8 & 0xf, rd(bar + CARD_HW_VERSION) & 0xff,
	    card_far(0x1e081b0), card_far(0x1f80500), card_far(0x1f80504));
	return true;
}

/*
 * Steps 6 and 7 (hakios_munch_mhi.c): the card may reach our memory from here on, through the SMMU
 * stream (0x1c01) that the MHI host sets up; until then it is shown nothing.
 */
extern bool hakios_munch_mhi_run(vm_offset_t registers, vm_offset_t configuration, unsigned level);
extern void hakios_munch_mhi_stop(void);

static bool
card_start(void)
{
	say("pcie: step 6: bus mastering on, in the root port and the card\n");
	DBI_PUT(CONF_COMMAND, rd(dbi + CONF_COMMAND) | 1u << 2);
	put(conf, CONF_PHYS, CONF_COMMAND, rd(conf + CONF_COMMAND) | 1u << 2);
	return hakios_munch_mhi_run(bar, conf, step_level);
}

static void
card_registers_off(void)
{
	if (!card_mem_on) {
		return;
	}
	hakios_munch_mhi_stop();
	put(conf, CONF_PHYS, CONF_COMMAND, rd(conf + CONF_COMMAND) & ~7u);
	put(conf, CONF_PHYS, CONF_BAR0, found_bar0);
	put(conf, CONF_PHYS, CONF_BAR1, found_bar1);
	DBI_PUT(CONF_COMMAND, found_rc_command);
	DBI_PUT(DBI_MEMORY_WINDOW, found_window);
	DBI_PUT(DBI_BUS_NUMBERS, found_bus_numbers);
	card_mem_on = false;
}

static void
link_down(void)
{
	card_registers_off();
	if (iatu_on) {
		put(iatu, IATU_PHYS, IATU_CTRL2(0), 0);
		iatu_on = false;
	}
	if (perst_released) {
		pin_put(PIN_PERST, CFG_PULL_DOWN | CFG_MA(2) | CFG_OUTPUT, 0);
		perst_released = false;
		say("pcie: the card held in reset again\n");
	}
	if (training) {
		PARF_MASK(PARF_LTSSM, 1u << 8, 0);
		training = false;
	}
}

static bool
steps_up(void)
{
	bool ok;

	if (gcc == 0) {
		gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
		tlmm = ml_io_map(TLMM_NORTH_PHYS, TLMM_NORTH_SIZE);
		parf = ml_io_map(PARF_PHYS, PARF_SIZE);
		phy = ml_io_map(PHY_PHYS, 0x1000);
		dbi = ml_io_map(DBI_PHYS, 0x1000);
		iatu = ml_io_map(IATU_PHYS, 0x1000);
		conf = ml_io_map(CONF_PHYS, 0x1000);
	}
	pins_save();
	ok = card_power_on();
	if (ok && step_level >= 3) {
		ok = controller_up();
	}
	if (ok && step_level >= 4) {
		ok = link_up();
		if (ok) {
			ok = card_config();
		}
		if (ok && step_level >= 5) {
			ok = card_registers();
		}
		if (ok && step_level >= 6) {
			ok = card_start();
		}
	}
	return ok;
}

static void
steps_down(void)
{
	link_down();
	controller_down();
	card_power_off();
	pins_restore();
	say("pcie: everything of the steps is taken down and put back; last: %s\n", last_write);
}

static void
steps_run(void)
{
	bool ok;

	if (wait_s != 0) {
		say("pcie: waiting %u s before the steps (pcie_wait)%s\n", wait_s, slow_ms ? ", then every write said first" : "");
		delay_for_interval(wait_s * 1000, NSEC_PER_MSEC);
	}
	say("pcie: steps up to %u (pcie_probe), held %u ms, then all of it taken down\n", step_level, hold_ms);
	ok = steps_up();
	say("pcie: %s; holding %u ms\n", ok ? "THE STEPS ARE DONE" : "STOPPED SHORT", hold_ms);
	delay_for_interval(hold_ms, NSEC_PER_MSEC);
	link_down();
	if (step_level >= 6) {
		hakios_wifi_radio_space_off();         /* (the card is in reset: it asks for nothing any more) */
	}
	controller_down();
	card_power_off();
	pins_restore();
	say("pcie: everything of the steps is taken down and put back; last: %s\n", last_write);
}

/*
 * wifi=1: the same steps without a test around them, for the driver (lx_os_rproc_boot()): up to the
 * firmware in mission mode and QRTR running, level 9 leaving a thread that looks at the rings.
 */
static bool wifi_is_up;
extern void hakios_munch_mhi_quiesce(void);
extern void hakios_munch_bt_radio_ready(void) __attribute__((weak));

int
hakios_munch_wifi_up(void)
{
	if (wifi_is_up) {
		return 0;
	}
	step_level = 9;
	if (!steps_up()) {
		say("pcie: the QCA6390 does not come up\n");
		steps_down();
		return ENXIO;
	}
	wifi_is_up = true;
	if (hakios_munch_bt_radio_ready != NULL) {
		hakios_munch_bt_radio_ready();
	}
	return 0;
}

void
hakios_munch_wifi_down(void)
{
	if (wifi_is_up) {
		wifi_is_up = false;
		steps_down();
	}
}

void *
hakios_munch_wifi_registers(unsigned long *size)
{
	*size = bar_size;
	return (void *)bar;
}

/*
 * No locks, no sleeping, and after this the card reaches no memory: its bus mastering and MSI off,
 * held in reset, wlan-en low. Its supplies stay voted for: RPMh is not to be asked from here.
 */
void
hakios_munch_wifi_quiesce(void)
{
	if (!wifi_is_up) {
		return;
	}
	wifi_is_up = false;
	hakios_munch_mhi_quiesce();
	*(volatile uint32_t *)(conf + CONF_COMMAND) &= ~7u;
	*(volatile uint32_t *)(tlmm + 0x1000UL * PIN_PERST + 4) = 0;
	*(volatile uint32_t *)(tlmm + 0x1000UL * PIN_WLAN_EN + 4) = 0;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
probe_thread(__unused void *parameter, __unused wait_result_t result)
{
	vm_offset_t gcc = ml_io_map(GCC_PHYS, GCC_SIZE), tlmm = ml_io_map(TLMM_NORTH_PHYS, TLMM_NORTH_SIZE);
	uint32_t vote = rd(gcc + GCC_VOTE), gdsc = rd(gcc + GCC_PCIE_0_GDSCR), cfg_ahb = rd(gcc + GCC_PCIE_0_CFG_AHB_CBCR);

	printf("pcie: step 1 (reads only): PCIe 0, the Wi-Fi's, as the boot loader left it\n");
	printf("pcie: gcc vote (%#lx) = %#x: PCIe 0's clocks voted:%s%s%s%s%s%s\n", GCC_PHYS + GCC_VOTE, vote,
	    (vote & 1) ? " slv_axi" : "", (vote & 2) ? " mstr_axi" : "", (vote & 4) ? " cfg_ahb" : "", (vote & 8) ? " aux" : "",
	    (vote & 16) ? " pipe" : "", (vote & 32) ? " slv_q2a" : "");
	printf("pcie: gcc pcie_0_gdscr (%#lx) = %#x: %s\n", GCC_PHYS + GCC_PCIE_0_GDSCR, gdsc,
	    (gdsc & GDSC_POWER_ON) ? "powered" : "off");
	show_clock(gcc, "pcie_0_slv_q2a_axi", GCC_PCIE_0_SLV_Q2A_CBCR);
	show_clock(gcc, "pcie_0_slv_axi", GCC_PCIE_0_SLV_AXI_CBCR);
	show_clock(gcc, "pcie_0_mstr_axi", GCC_PCIE_0_MSTR_AXI_CBCR);
	show_clock(gcc, "pcie_0_cfg_ahb", GCC_PCIE_0_CFG_AHB_CBCR);
	show_clock(gcc, "pcie_0_aux", GCC_PCIE_0_AUX_CBCR);
	show_clock(gcc, "pcie_0_pipe", GCC_PCIE_0_PIPE_CBCR);
	show_clock(gcc, "pcie0_phy_refgen", GCC_PCIE0_PHY_REFGEN);
	show_clock(gcc, "pcie_wifi_clkref", GCC_PCIE_WIFI_CLKREF);
	show_pin(tlmm, "wlan-en: the QCA6390 on", PIN_WLAN_EN);
	show_pin(tlmm, "PERST#: the card held in reset while low", PIN_PERST);
	show_pin(tlmm, "WAKE#", PIN_WAKE);

	if (!(gdsc & GDSC_POWER_ON) || (cfg_ahb & CBCR_OFF)) {
		printf("pcie: the controller's registers are not read: %s\n",
		    !(gdsc & GDSC_POWER_ON) ? "its power switch is off" : "the clock of its configuration registers is off");
	} else {
		vm_offset_t parf = ml_io_map(PARF_PHYS, PARF_SIZE);
		uint32_t ltssm = rd(parf + PARF_LTSSM);

		printf("pcie: parf sys_ctrl %#x, pm_stts %#x, phy_ctrl %#x (%s), ltssm %#x: state %#x%s\n", rd(parf + PARF_SYS_CTRL),
		    rd(parf + PARF_PM_STTS), rd(parf + PARF_PHY_CTRL), (rd(parf + PARF_PHY_CTRL) & 1) ? "PHY powered down" : "PHY on",
		    ltssm, ltssm & 0x3f, (ltssm & 0x3f) == 0x11 ? " (L0: the link is up)" : "");
	}
	printf("pcie: step 1 done\n");
	if (step_level >= 2) {
		steps_run();
	}
	thread_terminate(current_thread());
}

static void
hakios_munch_pcie_init(void)
{
	uint32_t probe = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("pcie_probe", &probe, sizeof(probe)) || probe == 0) {
		return;
	}
	step_level = probe > 8 ? 8 : probe;
	PE_parse_boot_argn("pcie_hold", &hold_ms, sizeof(hold_ms));
	/*
	 * For a step that stops the whole chip (a register reached before its clock is on: the loader's core stops
	 * with it, and its log is lost): pcie_wait=S starts the steps S seconds late, once xl follows the log, and
	 * pcie_slow=MS says every write before it is made and then waits, so that the log's last line is the culprit.
	 */
	PE_parse_boot_argn("pcie_wait", &wait_s, sizeof(wait_s));
	PE_parse_boot_argn("pcie_slow", &slow_ms, sizeof(slow_ms));
	if (kernel_thread_start(probe_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_munch_pcie_init);
