/*
 * The POCO F4's (munch's) modem: a Snapdragon X55 of its own beside the SoC, not the Hexagon inside it that
 * drivers/modem/hakios.mpss.c starts on the SM7325 (an SM8250 has no modem of its own). It is a PCIe device (17cb:0306)
 * on the SoC's PCIe 2, spoken to over MHI as the Wi-Fi chip on PCIe 0 is (hakios.munch_pcie.c, hakios.munch_mhi.c); its
 * boot loader is given sbl1.mbn over BHI and asks for the rest over Sahara, and it has four GPIOs of its own besides
 * (Android's dt: kona.dtsi qcom,mdm0 "qcom,ext-sdx55m", kona-pcie.dtsi pcie2, kona-mhi.dtsi; gcc-kona.c; docs: the
 * modem plan in the munch port's notes).
 *
 *   munch_modem=1  a look, nothing written: the four GPIOs (AP2MDM status 56 and errfatal 57, MDM2AP status 3 and
 *                  errfatal 1) and PCIe 2's (PERST 85, WAKE 87) as the boot chain left them, PCIe 2's power switch
 *                  and clocks, and -- only if its bus clock runs: its registers hang the bus otherwise -- its link;
 *                  and the modem's own PMIC (PMX55, SPMI slave 8): its power-on block and its channel at the arbiter
 *   munch_modem=2  ... and it is switched on, as Android's esoc driver does it the first time (esoc-mdm-pon.c,
 *                  mdm4x_do_first_power_on with sdx55m_pon_ops; qpnp-power-on.c qpnp_pon_modem_pwr_off): a warm reset of
 *                  its PMIC (PON 0x800: SW_RST_S2_CTL2 0, S2_CTL 1 = warm reset, S2_CTL2 0x80, SW_RST_GO 0xa5), 150 ms,
 *                  AP2MDM status (GPIO 56) driven high; then MDM2AP status (GPIO 3) is watched for ten seconds:
 *                  high is the modem's boot ROM up and waiting for its link (on the phone the PMIC's power-on block is
 *                  not this system's to write: the X55 is found powered, and left as it is)
 *   munch_modem=3  ... and PCIe 2 up, as pci-msm.c brings it up for the modem (and hakios.munch_pcie.c for the Wi-Fi on
 *                  PCIe 0): PERST low, its supplies (L9 1.2 V, L5 0.88 V through RPMh: the PCIe 0's too, so not with
 *                  wifi=1 yet), its power switch and clocks, its PHY's 136 writes, PERST high, the link trained, and the
 *                  device on bus 1 read: 17cb:0306 is the X55. Left up for what comes next; nothing given to the modem.
 *   munch_modem=4  ... and its registers: the root port a bridge to bus 1, its stream table (bus 1 is stream 0x1d00 + 1),
 *                  the X55's BAR0 sized and put at the start of PCIe 2's memory range (0x64300000), memory decoding on, and
 *                  its MHI and BHI registers read: what state, what execution environment (its boot ROM: PBL)
 *   munch_modem=5  ... and its boot loader given: bus mastering on, our memory shown to it through a context of the SMMU
 *                  for stream 0x1d01 (hakios.wifi.c's, so not with wifi=1 yet), sbl1.mbn (/fw/sdx55m in the system image)
 *                  copied there and handed to its boot ROM over BHI, as mhi_fw_load_sbl() does; then its environment
 *                  read again (SBL: its boot loader runs, and will ask for the rest over Sahara)
 *
 * The modem's own data (its EFS: the partitions mdm1m9kefs1-3 and mdmddr, with its IMEI and calibration) is not read
 * here, and will never be written by anything of this port.
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.version.h>
#include <kern/locks.h>
#include <sys/systm.h>
#include <machine/machine_routines.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);
extern bool hakios_munch_vreg(const char *name, uint32_t millivolts, bool on, uint32_t mode);      /* hakios.munch_touch.c */
extern void hakios_wifi_radio_space_want(uint32_t stream, uint32_t mask);                           /* hakios.wifi.c */
extern void hakios_wifi_radio_space_base(uint64_t base);
extern bool hakios_wifi_radio_map(uint64_t physical, uint64_t size, unsigned long long *address);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);
#include <sys/random.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

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

/* the pins: all on the south tile of the pin controller (pinctrl-kona.c) */
#define TLMM_SOUTH_PHYS         0x0f500000UL
#define TLMM_SOUTH_SIZE         0x60000UL
#define GPIO_CFG(n)             (0x1000u * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output enable */
#define GPIO_IO(n)              (0x1000u * (n) + 4)     /* bit 0 in, bit 1 out */

/* PCIe 2's power switch and clocks (gcc-kona.c) */
#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x90000UL
#define GCC_PCIE_2_GDSCR        0x6004          /* dt: qcom,gdsc@106004 */
#define GCC_PCIE_2_SLV_Q2A_CBCR 0x6010
#define GCC_PCIE_2_SLV_AXI_CBCR 0x6014
#define GCC_PCIE_2_MSTR_AXI_CBCR 0x601c
#define GCC_PCIE_2_CFG_AHB_CBCR 0x6024
#define GCC_PCIE_2_AUX_CBCR     0x6028
#define GCC_PCIE_2_PIPE_CBCR    0x602c
#define GCC_PCIE2_PHY_REFGEN    0x6f034
#define GCC_PCIE_MDM_CLKREF     0x8c00c         /* the reference clock out to the modem */
#define GCC_VOTE_PCIE_2         0x52010         /* bits 10-15: q2a, slv, mstr, cfg, aux, pipe */
#define CBCR_OFF                (1u << 31)
#define GDSC_POWER_ON           (1u << 31)

/* PCIe 2's controller (dt: pcie2 reg "parf") */
#define PARF_PHYS               0x01c10000UL
#define PARF_SIZE               0x3000UL
#define PARF_PHY_CTRL           0x40
#define PARF_LTSSM              0x1b0           /* [5:0] the link's training state (0x11: L0, up) */

static uint32_t look_level;
static vm_offset_t tlmm_south;

/* ---- the PMIC arbiter (version 5, as the buttons' and the torch's drivers have it), one transfer at a time ---------- */

#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_CHNLS_PHYS          0x0c600000UL
#define ARB_SYSTEM              0
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))
#define ARB_OWNER(n)            (0x700 + 4 * (n))
#define ARB_MAX_CHANNELS        512
#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10
#define OBSERVER_DATA0          0x18
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_WRITE           (0u << 27)
#define COMMAND_READ            (1u << 27)

/* the modem's PMIC's power-on block (kona-pmic-overlay.dtsi: qcom,pmxprairie@8, qcom,power-on@800, qcom,modem-reset) */
#define PON_ID                  0x808
#define PON_REVISION2           0x01
#define PON_TYPE                0x04
#define PON_SUBTYPE             0x05
#define PON_REASON1             0x08
#define PON_SW_RST_S2_CTL       0x62
#define PON_SW_RST_S2_CTL2      0x63
#define PON_SW_RST_GO           0x64
#define PON_WARM_RESET          0x01
#define PON_RESET_EN            0x80
#define PON_SW_RST_GO_VAL       0xa5

static vm_offset_t arb_core, arb_cnfg, arb_observer, pon_write;
static int pon_channel = -1;
static bool pon_ours;

static const char *
transfer_wait(vm_offset_t channel)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(2000 * NSEC_PER_USEC, &interval);
	do {
		status = rd(channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	return !(status & STATUS_DONE) ? "no answer from the arbiter" : (status & STATUS_DENIED) ? "the arbiter refuses" :
	       (status & STATUS_FAILURE) ? "the transfer failed" : (status & STATUS_DROPPED) ? "the transfer was dropped" : NULL;
}

static const char *
pon_read(uint8_t reg, uint8_t *value)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * (uint32_t)pon_channel;
	const char *problem;

	hakios_spmi_lock();
	wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4);
	problem = transfer_wait(channel);
	*value = (uint8_t)rd(channel + OBSERVER_DATA0);
	hakios_spmi_unlock();
	return problem;
}

static const char *
pon_put(uint8_t reg, uint8_t value)
{
	const char *problem;

	if (!pon_ours || pon_write == 0 || ((rd(arb_core + ARB_CHANNEL(pon_channel)) >> 8) & 0xfff) != PON_ID) {
		return "not ours to write";
	}
	hakios_spmi_lock();
	wr(pon_write + CHANNEL_WDATA0, value);
	wr(pon_write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
	problem = transfer_wait(pon_write);
	hakios_spmi_unlock();
	return problem;
}

/* the modem PMIC's power-on block: its channel at the arbiter, who may write through it, what it says */
static bool
look_pmic(void)
{
	uint8_t revision = 0, type = 0, subtype = 0, reason = 0, ctl = 0, ctl2 = 0;
	const char *problem;

	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	for (int n = 0; n < ARB_MAX_CHANNELS; n++) {
		if (((rd(arb_core + ARB_CHANNEL(n)) >> 8) & 0xfff) == PON_ID) {
			pon_channel = n;
			pon_ours = (rd(arb_cnfg + ARB_OWNER(n)) & 7) == ARB_SYSTEM;
			break;
		}
	}
	if (pon_channel < 0) {
		say("munch-modem: the modem's PMIC (PMX55, power-on block %#x) has no channel at the arbiter\n", PON_ID);
		return false;
	}
	if ((problem = pon_read(PON_REVISION2, &revision)) != NULL || (problem = pon_read(PON_TYPE, &type)) != NULL ||
	    (problem = pon_read(PON_SUBTYPE, &subtype)) != NULL || (problem = pon_read(PON_REASON1, &reason)) != NULL ||
	    (problem = pon_read(PON_SW_RST_S2_CTL, &ctl)) != NULL || (problem = pon_read(PON_SW_RST_S2_CTL2, &ctl2)) != NULL) {
		say("munch-modem: reading the modem's PMIC's power-on block: %s\n", problem);
		return false;
	}
	say("munch-modem: the modem's PMIC (PMX55): power-on block on channel %d (%s), revision %#x, type %#x, subtype %#x, "
	    "power-on reason %#x, software reset %#x %#x\n", pon_channel, pon_ours ? "ours to write" : "NOT ours to write", revision,
	    type, subtype, reason, ctl, ctl2);
	if (pon_ours) {
		pon_write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * (uint32_t)pon_channel, 0x1000);
	}
	return true;
}

/* ---- PCIe 2 (kona-pcie.dtsi pcie2, gcc-kona.c, pci-msm.c's msm_pcie_enable and msm_pcie_link_train) ------------------ */

#define PIN_PERST               85
#define PIN_CLKREQ              86              /* function 1: pci_e2 */
#define PIN_WAKE                87
#define CFG_PULL_DOWN           1u
#define CFG_PULL_UP             3u
#define CFG_FUNCTION(n)         ((n) << 2)
#define CFG_MA(n)               (((n) / 2 - 1) << 6)
#define CFG_OUTPUT              (1u << 9)
#define VRM_MODE_HPM            7

#define GCC_PCIE_2_BCR          0x6000
#define GCC_PCIE_2_AUX_CMD      0x6038
#define GCC_PCIE_2_AUX_CFG      0x603c
#define GCC_PCIE_2_PHY_BCR      0x1f01c
#define GCC_TBU_CBCR            0x9000c
#define GCC_SF_TBU_CBCR         0x8d058
#define VOTE_PIPE               (1u << 15)
#define VOTE_OTHERS             (0x1fu << 10)   /* slv_q2a, slv_axi, mstr_axi, cfg_ahb, aux */
#define GDSC_COLLAPSE           1u

#define PARF_SYS_CTRL           0x00
#define PARF_PM_CTRL            0x20
#define PARF_MHI_CLOCK_RESET    0x174
#define PARF_AXI_MSTR_WR_HALT   0x1a8
#define PARF_INT_ALL_MASK       0x22c
#define PARF_DBI_BASE_ADDR      0x350
#define PARF_SLV_ADDR_SPACE     0x358
#define PARF_DEVICE_TYPE        0x1000

#define PHY_PHYS                0x01c16000UL
#define PHY_SIZE                0x2000UL
#define PHY_STATUS              0xa14           /* dt: qcom,phy-status-offset, bit 6: not ready */
#define PHY_POWER_DOWN          0xa40

#define DBI_PHYS                0x64000000UL    /* dt: dm_core; elbi at +0xf20 */
#define IATU_PHYS               0x64001000UL
#define CONF_PHYS               0x64100000UL
#define ELBI_SYS_STTS           (0xf20 + 0x08)  /* bit 10: the link up; [17:12] its training state */
#define DBI_DEVICE_CONTROL      0x78
#define DBI_LINK_STATUS         0x80            /* [19:16] speed, [25:20] width, bit 29 the data link up */
#define DBI_GEN3_SPCIE_CAP      0x154
#define DBI_GEN2_CTRL           0x80c
#define DBI_GEN3_RELATED        0x890
#define DBI_GEN3_MISC_CONTROL   0x8bc
#define DBI_AUX_CLK_FREQ        0xb40
#define IATU_CTRL1(n)           (0x200 * (n) + 0x00)
#define IATU_CTRL2(n)           (0x200 * (n) + 0x04)
#define IATU_LBAR(n)            (0x200 * (n) + 0x08)
#define IATU_UBAR(n)            (0x200 * (n) + 0x0c)
#define IATU_LAR(n)             (0x200 * (n) + 0x10)
#define IATU_LTAR(n)            (0x200 * (n) + 0x14)
#define IATU_UTAR(n)            (0x200 * (n) + 0x18)
#define IATU_TYPE_CFG0          4
#define X55_ID                  0x030617cbu     /* device 0x0306, vendor 0x17cb (mhi_qcom.c: sdx55m) */

/* dt: qcom,phy-sequence of pcie2 (PHY version 1102): offset, value, delay in us (none has one) */
static const struct {
	uint16_t offset;
	uint8_t value, delay;
} pcie2_phy_sequence[] = {
	{ 0x0a40, 0x03, 0 },
	{ 0x0010, 0x01, 0 },
	{ 0x001c, 0x31, 0 },
	{ 0x0020, 0x01, 0 },
	{ 0x0024, 0xde, 0 },
	{ 0x0028, 0x07, 0 },
	{ 0x0030, 0x4c, 0 },
	{ 0x0034, 0x06, 0 },
	{ 0x0048, 0x90, 0 },
	{ 0x0058, 0x0f, 0 },
	{ 0x0074, 0x06, 0 },
	{ 0x0078, 0x06, 0 },
	{ 0x007c, 0x16, 0 },
	{ 0x0080, 0x16, 0 },
	{ 0x0084, 0x36, 0 },
	{ 0x0088, 0x36, 0 },
	{ 0x0094, 0x08, 0 },
	{ 0x00a4, 0x42, 0 },
	{ 0x00ac, 0x0a, 0 },
	{ 0x00b0, 0x1a, 0 },
	{ 0x00b4, 0x14, 0 },
	{ 0x00b8, 0x34, 0 },
	{ 0x00bc, 0x82, 0 },
	{ 0x00c4, 0x68, 0 },
	{ 0x00cc, 0x55, 0 },
	{ 0x00d0, 0x55, 0 },
	{ 0x00d4, 0x03, 0 },
	{ 0x00d8, 0xab, 0 },
	{ 0x00dc, 0xaa, 0 },
	{ 0x00e0, 0x02, 0 },
	{ 0x010c, 0x02, 0 },
	{ 0x0110, 0x24, 0 },
	{ 0x0118, 0xb4, 0 },
	{ 0x011c, 0x03, 0 },
	{ 0x0154, 0x34, 0 },
	{ 0x0158, 0x01, 0 },
	{ 0x016c, 0x08, 0 },
	{ 0x01ac, 0xca, 0 },
	{ 0x01b0, 0x1e, 0 },
	{ 0x01b4, 0xa2, 0 },
	{ 0x01b8, 0x18, 0 },
	{ 0x01bc, 0x11, 0 },
	{ 0x023c, 0x11, 0 },
	{ 0x0284, 0x75, 0 },
	{ 0x029c, 0x12, 0 },
	{ 0x0304, 0x02, 0 },
	{ 0x0408, 0x0c, 0 },
	{ 0x0414, 0x03, 0 },
	{ 0x0434, 0x7f, 0 },
	{ 0x0444, 0x70, 0 },
	{ 0x0460, 0x30, 0 },
	{ 0x04d4, 0x04, 0 },
	{ 0x04d8, 0x07, 0 },
	{ 0x04dc, 0x1b, 0 },
	{ 0x04e8, 0x04, 0 },
	{ 0x04ec, 0x0e, 0 },
	{ 0x04f0, 0x4a, 0 },
	{ 0x04f4, 0x0f, 0 },
	{ 0x04f8, 0xc0, 0 },
	{ 0x04fc, 0x00, 0 },
	{ 0x0510, 0x17, 0 },
	{ 0x0518, 0x1c, 0 },
	{ 0x051c, 0x03, 0 },
	{ 0x0524, 0x1e, 0 },
	{ 0x0570, 0xbf, 0 },
	{ 0x0574, 0x3f, 0 },
	{ 0x0578, 0xff, 0 },
	{ 0x057c, 0x7f, 0 },
	{ 0x0580, 0x15, 0 },
	{ 0x0584, 0x24, 0 },
	{ 0x0588, 0xe4, 0 },
	{ 0x058c, 0xec, 0 },
	{ 0x0590, 0x3b, 0 },
	{ 0x0594, 0x36, 0 },
	{ 0x0598, 0xd4, 0 },
	{ 0x059c, 0x54, 0 },
	{ 0x05a0, 0xdb, 0 },
	{ 0x05a4, 0x3b, 0 },
	{ 0x05a8, 0x31, 0 },
	{ 0x05bc, 0x0c, 0 },
	{ 0x05b8, 0x38, 0 },
	{ 0x063c, 0x11, 0 },
	{ 0x0684, 0x75, 0 },
	{ 0x069c, 0x12, 0 },
	{ 0x0704, 0x20, 0 },
	{ 0x0808, 0x0c, 0 },
	{ 0x0814, 0x03, 0 },
	{ 0x0834, 0x7f, 0 },
	{ 0x0844, 0x70, 0 },
	{ 0x0860, 0x30, 0 },
	{ 0x08d4, 0x04, 0 },
	{ 0x08d8, 0x07, 0 },
	{ 0x08dc, 0x1b, 0 },
	{ 0x08e8, 0x04, 0 },
	{ 0x08ec, 0x0e, 0 },
	{ 0x08f0, 0x4a, 0 },
	{ 0x08f4, 0x0f, 0 },
	{ 0x08f8, 0xc0, 0 },
	{ 0x08fc, 0x00, 0 },
	{ 0x0910, 0x17, 0 },
	{ 0x0918, 0x1c, 0 },
	{ 0x091c, 0x03, 0 },
	{ 0x0924, 0x1e, 0 },
	{ 0x0970, 0xbf, 0 },
	{ 0x0974, 0x3f, 0 },
	{ 0x0978, 0xff, 0 },
	{ 0x097c, 0x7f, 0 },
	{ 0x0980, 0x15, 0 },
	{ 0x0984, 0x24, 0 },
	{ 0x0988, 0xe4, 0 },
	{ 0x098c, 0xec, 0 },
	{ 0x0990, 0x3b, 0 },
	{ 0x0994, 0x36, 0 },
	{ 0x0998, 0xd4, 0 },
	{ 0x099c, 0x54, 0 },
	{ 0x09a0, 0xdb, 0 },
	{ 0x09a4, 0x3b, 0 },
	{ 0x09a8, 0x31, 0 },
	{ 0x09bc, 0x0c, 0 },
	{ 0x09b8, 0x38, 0 },
	{ 0x0adc, 0x05, 0 },
	{ 0x0b88, 0x77, 0 },
	{ 0x0b98, 0x0b, 0 },
	{ 0x0ba4, 0x01, 0 },
	{ 0x0be0, 0x0f, 0 },
	{ 0x0e0c, 0x0d, 0 },
	{ 0x0e14, 0x07, 0 },
	{ 0x0e1c, 0xc1, 0 },
	{ 0x0e40, 0x01, 0 },
	{ 0x0e48, 0x01, 0 },
	{ 0x0e90, 0x00, 0 },
	{ 0x0eb4, 0x33, 0 },
	{ 0x0ebc, 0x00, 0 },
	{ 0x0ee0, 0x58, 0 },
	{ 0x0a00, 0x00, 0 },
	{ 0x0a44, 0x03, 0 },
};

static vm_offset_t gcc, parf, phy, dbi, iatu, conf;
static bool look_clock(vm_offset_t gcc, const char *name, uint32_t offset);

/* what pcie2_up() found, for modem_down() to put back */
static bool pcie2_touched;
static uint32_t found_perst_cfg, found_perst_io, found_clkreq_cfg, found_wake_cfg;
static uint32_t found_gdsc, found_vote, found_clkref, found_refgen;

static void
put(vm_offset_t base, uint32_t offset, uint32_t value)
{
	wr(base + offset, value);
}

static void
put_mask(vm_offset_t base, uint32_t offset, uint32_t clear, uint32_t set)
{
	wr(base + offset, (rd(base + offset) & ~clear) | set);
}

static void
pin_put(unsigned pin, uint32_t cfg, uint32_t out)
{
	wr(tlmm_south + GPIO_IO(pin), out ? 1u << 1 : 0);
	wr(tlmm_south + GPIO_CFG(pin), cfg);
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

/* a write of the clock controller's, told first and given 20 ms to fault before the next (an SError comes late) */
static void
gcc_step(const char *what, uint32_t offset, uint32_t value)
{
	say("munch-modem: gcc %#lx <- %#x (%s; was %#x)\n", GCC_PHYS + offset, value, what, rd(gcc + offset));
	wr(gcc + offset, value);
	delay_for_interval(20, NSEC_PER_MSEC);
}

static bool
pcie2_up(void)
{
	uint32_t tbu, sf_tbu, stts = 0, status = 0, id = 0xffffffff;

	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	parf = ml_io_map(PARF_PHYS, PARF_SIZE);
	phy = ml_io_map(PHY_PHYS, PHY_SIZE);
	dbi = ml_io_map(DBI_PHYS, 0x1000);
	iatu = ml_io_map(IATU_PHYS, 0x1000);
	conf = ml_io_map(CONF_PHYS, 0x1000);

	say("munch-modem: PCIE 2 UP (munch_modem=3): the modem held in reset (PERST low), the controller's supplies, power "
	    "switch and clocks\n");
	found_perst_cfg = rd(tlmm_south + GPIO_CFG(PIN_PERST));
	found_perst_io = rd(tlmm_south + GPIO_IO(PIN_PERST));
	found_clkreq_cfg = rd(tlmm_south + GPIO_CFG(PIN_CLKREQ));
	found_wake_cfg = rd(tlmm_south + GPIO_CFG(PIN_WAKE));
	found_gdsc = rd(gcc + GCC_PCIE_2_GDSCR);
	found_vote = rd(gcc + GCC_VOTE_PCIE_2);
	found_clkref = rd(gcc + GCC_PCIE_MDM_CLKREF);
	found_refgen = rd(gcc + GCC_PCIE2_PHY_REFGEN);
	pcie2_touched = true;
	pin_put(PIN_PERST, CFG_PULL_DOWN | CFG_MA(2) | CFG_OUTPUT, 0);
	pin_put(PIN_CLKREQ, CFG_PULL_UP | CFG_MA(2) | CFG_FUNCTION(1), 0);
	pin_put(PIN_WAKE, CFG_PULL_UP | CFG_MA(2), 0);
	sleep_us(1000);
	if (!hakios_munch_vreg("ldoa9", 1200, true, VRM_MODE_HPM) || !hakios_munch_vreg("ldoa5", 880, true, VRM_MODE_HPM)) {
		say("munch-modem: PCIe 2's supplies not given\n");
		return false;
	}
	gcc_step("pcie_2_gdsc on", GCC_PCIE_2_GDSCR, rd(gcc + GCC_PCIE_2_GDSCR) & ~GDSC_COLLAPSE);
	for (unsigned i = 0; i < 100 && !(rd(gcc + GCC_PCIE_2_GDSCR) & GDSC_POWER_ON); i++) {
		sleep_us(10);
	}
	if (!(rd(gcc + GCC_PCIE_2_GDSCR) & GDSC_POWER_ON)) {
		say("munch-modem: pcie_2_gdsc does not come on (%#x)\n", rd(gcc + GCC_PCIE_2_GDSCR));
		return false;
	}
	say("munch-modem: pcie_2_gdsc on (%#x)\n", rd(gcc + GCC_PCIE_2_GDSCR));
	gcc_step("aux: the crystal, divided by 1", GCC_PCIE_2_AUX_CFG, 0 << 8 | 1);
	gcc_step("aux: take it", GCC_PCIE_2_AUX_CMD, rd(gcc + GCC_PCIE_2_AUX_CMD) | 1);
	for (unsigned i = 0; i < 100 && (rd(gcc + GCC_PCIE_2_AUX_CMD) & 1); i++) {
		sleep_us(10);
	}
	/* (the SMMU's PCIe TBU clocks: a register that reads 0 is left alone, as on PCIe 0: writing one stopped the chip) */
	/*
	 * The SMMU's PCIe TBU clocks (aggre_noc_pcie_tbu 0x9000c, ddrss_pcie_sf_tbu 0x8d058) are for the modem's DMA, not for
	 * the link, and are left alone here: with PCIe 2's power switch just on, the kernel took an SError right where they
	 * were read (7 November 12:31, "Unhandled implementation specific error"). For when DMA needs them.
	 */
	tbu = sf_tbu = 0;
	(void)tbu;
	(void)sf_tbu;
	gcc_step("the modem's reference clock", GCC_PCIE_MDM_CLKREF, rd(gcc + GCC_PCIE_MDM_CLKREF) | 1);
	gcc_step("PHY refgen", GCC_PCIE2_PHY_REFGEN, rd(gcc + GCC_PCIE2_PHY_REFGEN) | 1);
	for (uint32_t bit = 10; bit <= 14; bit++) {
		gcc_step("a clock vote", GCC_VOTE_PCIE_2, rd(gcc + GCC_VOTE_PCIE_2) | 1u << bit);
	}
	sleep_us(100);
	look_clock(gcc, "aux", GCC_PCIE_2_AUX_CBCR);
	look_clock(gcc, "mstr_axi", GCC_PCIE_2_MSTR_AXI_CBCR);
	look_clock(gcc, "reference out to the modem", GCC_PCIE_MDM_CLKREF);
	if (!look_clock(gcc, "cfg_ahb", GCC_PCIE_2_CFG_AHB_CBCR)) {
		say("munch-modem: the clock of PCIe 2's registers is off: not touching them\n");
		return false;
	}
	put(gcc, GCC_PCIE_2_BCR, 1);
	sleep_us(1000);
	put(gcc, GCC_PCIE_2_BCR, 0);
	put(gcc, GCC_PCIE_2_PHY_BCR, 1);
	sleep_us(1000);
	put(gcc, GCC_PCIE_2_PHY_BCR, 0);

	put(parf, PARF_DEVICE_TYPE, 4);                         /* a root port */
	put_mask(parf, PARF_PM_CTRL, 1u << 5, 0);
	put_mask(parf, PARF_PHY_CTRL, 1u << 0, 0);
	put(parf, PARF_DBI_BASE_ADDR, 0);
	put(parf, PARF_SYS_CTRL, 0x365e);
	put_mask(parf, PARF_MHI_CLOCK_RESET, 0, 1u << 4);
	put(parf, PARF_INT_ALL_MASK, 0);
	put(parf, PARF_SLV_ADDR_SPACE, 0x4000000);             /* dt: qcom,slv-addr-space-size */
	put_mask(parf, PARF_AXI_MSTR_WR_HALT, 0, 1u << 31);
	for (unsigned i = 0; i < sizeof(pcie2_phy_sequence) / sizeof(pcie2_phy_sequence[0]); i++) {
		put(phy, pcie2_phy_sequence[i].offset, pcie2_phy_sequence[i].value);
		if (pcie2_phy_sequence[i].delay) {
			sleep_us(pcie2_phy_sequence[i].delay);
		}
	}
	sleep_us(1000);
	put(gcc, GCC_VOTE_PCIE_2, rd(gcc + GCC_VOTE_PCIE_2) | VOTE_PIPE);
	for (unsigned i = 0; i < 10 && (rd(phy + PHY_STATUS) & 1u << 6); i++) {
		sleep_us(1000);
	}
	look_clock(gcc, "pipe", GCC_PCIE_2_PIPE_CBCR);
	if (rd(phy + PHY_STATUS) & 1u << 6) {
		say("munch-modem: PCIE 2'S PHY DOES NOT COME UP (status %#x)\n", rd(phy + PHY_STATUS));
		return false;
	}
	say("munch-modem: PCIe 2's PHY is ready (status %#x); %u ms, then the modem let out of reset (PERST high)\n",
	    rd(phy + PHY_STATUS), 10);
	delay_for_interval(10, NSEC_PER_MSEC);                  /* dt: qcom,ep-latency */
	pin_put(PIN_PERST, CFG_PULL_DOWN | CFG_MA(2) | CFG_OUTPUT, 1);
	sleep_us(1000);
	put_mask(dbi, DBI_GEN2_CTRL, 0x1f00, 1u << 8);
	put_mask(dbi, DBI_GEN3_RELATED, 1u << 0, 0);
	put_mask(dbi, DBI_GEN3_MISC_CONTROL, 0, 1u << 0);
	put(dbi, DBI_GEN3_SPCIE_CAP, 0x55555555);
	put_mask(dbi, DBI_GEN3_MISC_CONTROL, 1u << 0, 0);
	put_mask(dbi, DBI_DEVICE_CONTROL, 0x7000, 5u << 12);
	put_mask(parf, PARF_LTSSM, 0, 1u << 8);
	/* the X55's boot ROM may take its time: up to a second */
	for (unsigned i = 0; i < 200; i++) {
		delay_for_interval(5, NSEC_PER_MSEC);
		stts = rd(dbi + ELBI_SYS_STTS);
		status = rd(dbi + DBI_LINK_STATUS);
		if ((stts & 1u << 10) && (status & 1u << 29)) {
			break;
		}
	}
	say("munch-modem: elbi sys_stts %#x (training state %#x), link status %#x, parf ltssm %#x\n", stts, stts >> 12 & 0x3f,
	    status, rd(parf + PARF_LTSSM));
	if (!(stts & 1u << 10) || !(status & 1u << 29)) {
		say("munch-modem: PCIE 2'S LINK DOES NOT COME UP: the modem does not answer on it\n");
		return false;
	}
	say("munch-modem: PCIE 2'S LINK IS UP: gen %u, x%u\n", status >> 16 & 0xf, status >> 20 & 0x3f);
	put(iatu, IATU_CTRL2(0), 0);
	put(iatu, IATU_CTRL1(0), IATU_TYPE_CFG0);
	put(iatu, IATU_LBAR(0), (uint32_t)CONF_PHYS);
	put(iatu, IATU_UBAR(0), 0);
	put(iatu, IATU_LAR(0), (uint32_t)CONF_PHYS + 0xfff);
	put(iatu, IATU_LTAR(0), 1u << 24);                      /* bus 1, device 0 */
	put(iatu, IATU_UTAR(0), 0);
	put(iatu, IATU_CTRL2(0), 1u << 31);
	put(dbi, DBI_AUX_CLK_FREQ, 0x14);                       /* dt: qcom,use-19p2mhz-aux-clk */
	for (unsigned ms = 0; ms < 1000 && (id = rd(conf)) == 0xffffffff; ms++) {
		sleep_us(1000);
	}
	if (id == 0xffffffff) {
		say("munch-modem: the device on PCIe 2 does not answer its configuration space\n");
		return false;
	}
	say("munch-modem: THE DEVICE ON PCIE 2 ANSWERS: vendor %#x, device %#x%s\n", id & 0xffff, id >> 16,
	    id == X55_ID ? ": THE SNAPDRAGON X55" : " (not the X55 expected)");
	say("munch-modem: its command/status %#x, class %#x, header %#x, BAR0 %#x, BAR1 %#x, BAR2 %#x, subsystem %#x\n",
	    rd(conf + 4), rd(conf + 8), rd(conf + 12), rd(conf + 0x10), rd(conf + 0x14), rd(conf + 0x18), rd(conf + 0x2c));
	say("munch-modem: MDM2AP status now %u, errfatal %u\n", rd(tlmm_south + GPIO_IO(3)) & 1, rd(tlmm_south + GPIO_IO(1)) & 1);
	return id == X55_ID;
}

/* ---- the X55's registers: BAR0, MHI, BHI (pci-msm.c's enumeration, mhi_init.c, mhi_boot.c) ---------------------------- */

#define MEM_PHYS                0x64300000UL    /* dt: ranges, 32-bit memory: 0x64300000 + 0x3d00000 */
#define CONF_COMMAND            0x04            /* bit 1 memory, bit 2 bus master */
#define CONF_BAR0               0x10
#define CONF_BAR1               0x14
#define DBI_BUS_NUMBERS         0x18
#define DBI_MEMORY_WINDOW       0x20
#define PARF_BDF_TO_SID         0x2000          /* 256 words: bdf [31:16], stream - base [15:8], next [7:0] */
#define BDF_BUS1_HASH           0x15            /* crc8 of the bdf 0x0100 (as on PCIe 0) */
#define MHI_REGLEN              0x00
#define MHI_VER                 0x08
#define MHI_CFG                 0x10
#define MHI_CHDBOFF             0x18
#define MHI_ERDBOFF             0x20
#define MHI_BHIOFF              0x28
#define MHI_BHIEOFF             0x2c
#define MHI_CTRL                0x38
#define MHI_STATUS              0x48            /* [15:8] the state, bit 2 system error, bit 0 ready */
#define MHI_SOC_RESET_REQ       0xb0            /* 1: the device resets itself, the whole SoC (Linux mhi_soc_reset()) */
#define BHI_VERSION_MINOR       0x00
#define BHI_VERSION_MAJOR       0x04
#define BHI_IMGADDR_LOW         0x08
#define BHI_IMGADDR_HIGH        0x0c
#define BHI_IMGSIZE             0x10
#define BHI_IMGTXDB             0x18            /* [29:0] a session number, not 0 */
#define BHI_INTVEC              0x20
#define BHI_EXECENV             0x28
#define BHI_STATUS              0x2c            /* [31:30] 2 done, 3 failed */
#define BHI_ERRCODE             0x30
#define BHI_ERRDBG1             0x34
#define BHI_ERRDBG2             0x38
#define BHI_ERRDBG3             0x3c
#define BHI_SERIALNU            0x40
#define EE_PBL                  0
#define X55_STREAM              0x1d01

static const char *const mhi_states[] = { "RESET", "READY", "M0", "M1", "M2", "M3", "M3_FAST", "BHI" };
static const char *const exec_envs[] = { "PBL", "SBL", "AMSS", "RDDM", "WFW", "PTHRU", "EDL", "FP" };
static vm_offset_t bar, bhi;
static uint32_t bar_size;

static const char *
ee_name(uint32_t env)
{
	return env < 8 ? exec_envs[env] : "?";
}

static void
mhi_say(void)
{
	uint32_t state = rd(bar + MHI_STATUS), env = rd(bhi + BHI_EXECENV);

	say("munch-modem: MHI: ctrl %#x, status %#x: state %s%s%s; BHI: environment %u (%s), status %#x, error %#x\n",
	    rd(bar + MHI_CTRL), state, (state >> 8 & 0xff) < 8 ? mhi_states[state >> 8 & 0xff] : "?", (state & 1) ? ", ready" : "",
	    (state & 4) ? ", SYSTEM ERROR" : "", env, ee_name(env), rd(bhi + BHI_STATUS), rd(bhi + BHI_ERRCODE));
}

static bool
x55_registers(void)
{
	uint32_t low, high, size, off;

	say("munch-modem: the root port a bridge to bus 1, bus 1 stream %#x, the X55's BAR0 at %#lx (munch_modem=4)\n",
	    X55_STREAM, MEM_PHYS);
	put(dbi, DBI_BUS_NUMBERS, (rd(dbi + DBI_BUS_NUMBERS) & 0xff000000) | 1u << 16 | 1u << 8);
	for (unsigned i = 0; i < 256; i++) {
		put(parf, PARF_BDF_TO_SID + 4 * i, 0);
	}
	put(parf, PARF_BDF_TO_SID + 4 * BDF_BUS1_HASH, 0x0100u << 16 | 1u << 8);
	put(conf, CONF_BAR0, 0xffffffff);
	put(conf, CONF_BAR1, 0xffffffff);
	low = rd(conf + CONF_BAR0);
	high = rd(conf + CONF_BAR1);
	size = (high != 0xffffffff || (low & ~0xfu) == 0) ? 0 : ~(low & ~0xfu) + 1;
	say("munch-modem: BAR0 sized: %#x %#x: %u KB%s\n", low, high, size >> 10, (low & 6) == 4 ? ", 64-bit" : "");
	if (size < 0x1000 || size > 0x1000000) {
		say("munch-modem: BAR0's size is not one to map\n");
		return false;
	}
	put(conf, CONF_BAR0, (uint32_t)MEM_PHYS | (low & 0xf));
	put(conf, CONF_BAR1, 0);
	put(dbi, DBI_MEMORY_WINDOW, ((uint32_t)(MEM_PHYS + size - 1) & 0xfff00000) | ((uint32_t)MEM_PHYS >> 16 & 0xfff0));
	put(dbi, CONF_COMMAND, (rd(dbi + CONF_COMMAND) & ~7u) | 1u << 1);
	put(conf, CONF_COMMAND, (rd(conf + CONF_COMMAND) & ~7u) | 1u << 1);
	bar = ml_io_map(MEM_PHYS, size);
	bar_size = size;
	say("munch-modem: MHI: reglen %#x, version %#x, cfg %#x (%u channels, %u event rings), chdb at %#x, erdb at %#x, bhi at "
	    "%#x, bhie at %#x\n", rd(bar + MHI_REGLEN), rd(bar + MHI_VER), rd(bar + MHI_CFG), rd(bar + MHI_CFG) & 0xff,
	    rd(bar + MHI_CFG) >> 16 & 0xff, rd(bar + MHI_CHDBOFF), rd(bar + MHI_ERDBOFF), rd(bar + MHI_BHIOFF), rd(bar + MHI_BHIEOFF));
	off = rd(bar + MHI_BHIOFF);
	if (off == 0 || off > size - 0x100) {
		say("munch-modem: no BHI where MHI says\n");
		return false;
	}
	bhi = bar + off;
	say("munch-modem: BHI version %u.%u, serial %#x\n", rd(bhi + BHI_VERSION_MAJOR), rd(bhi + BHI_VERSION_MINOR),
	    rd(bhi + BHI_SERIALNU) != 0);
	mhi_say();
	return true;
}

/* memory in one piece, uncached, that the X55 sees through its stream (as hakios.munch_mhi.c's dma_alloc) */
static uint64_t dma_last_physical;      /* the last dma_alloc()'s, for munch_modem_phys */
static vm_offset_t dma_last_cached;     /* ... and its cached view, to be cleaned after the copy */
static vm_size_t dma_last_size;

static void *
dma_alloc(vm_size_t size, uint64_t *address)
{
	vm_offset_t cached = 0, uncached;
	uint64_t physical;

	size = round_page(size);
	if (kmem_alloc_contig(kernel_map, &cached, size, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		say("munch-modem: no %lu KB of memory in one piece\n", (unsigned long)(size >> 10));
		return NULL;
	}
	physical = kvtophys(cached);
	dma_last_physical = physical;
	dma_last_cached = cached;
	dma_last_size = size;
	for (vm_offset_t line = cached; line < cached + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	uncached = ml_io_map_unmappable(physical, size, VM_WIMG_WCOMB);
	if (uncached == 0 || !hakios_wifi_radio_map(physical, size, (unsigned long long *)address)) {
		say("munch-modem: %lu KB could not be shown to the X55\n", (unsigned long)(size >> 10));
		return NULL;
	}
	return (void *)uncached;
}

/* what the SMMU refused the X55, if anything: its context's fault registers (the context hakios.wifi.c took: 7 on the phone) */
#define SMMU_PHYS               0x15000000UL
#define SMMU_CB(n)              (0x80000UL + 0x1000UL * (n))    /* 128 pages of 4 KB before the contexts' */
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_SGFSR              0x48
#define SMMU_SGFSYNR1           0x54

static void
smmu_faults(void)
{
	vm_offset_t smmu = ml_io_map(SMMU_PHYS, 0x100000);

	for (unsigned n = 6; n <= 8; n++) {
		vm_offset_t cb = smmu + SMMU_CB(n);

		say("munch-modem: SMMU context %u: sctlr %#x, fsr %#x, far %#x %#x, fsynr0 %#x\n", n, rd(cb), rd(cb + SMMU_CB_FSR),
		    rd(cb + SMMU_CB_FAR + 4), rd(cb + SMMU_CB_FAR), rd(cb + SMMU_CB_FSYNR0));
	}
	say("munch-modem: SMMU as a whole: sgfsr %#x, sgfsynr1 %#x (the stream)\n", rd(smmu + SMMU_SGFSR), rd(smmu + SMMU_SGFSYNR1));
}

static bool
sbl_load(void)
{
	const uint8_t *image;
	uint64_t size = 0, address = 0;
	uint8_t *copy;
	uint32_t session, status = 0, env;
	unsigned ms;

	env = rd(bhi + BHI_EXECENV);
	if (env != EE_PBL) {
		say("munch-modem: the X55 is in %s, not its boot ROM: no boot loader to give it\n", ee_name(env));
		return false;
	}
	image = hakios_image_file("fw/sdx55m", "sbl1.mbn", &size, NULL);
	if (image == NULL || size == 0 || size > 0x200000) {
		say("munch-modem: no fw/sdx55m/sbl1.mbn in the system image (fw-modem/, devices/munch/pull-firmware.sh)\n");
		return false;
	}
	say("munch-modem: bus mastering on; our memory shown to the X55 through stream %#x (munch_modem=5)\n", X55_STREAM);
	put(dbi, CONF_COMMAND, rd(dbi + CONF_COMMAND) | 1u << 2);
	put(conf, CONF_COMMAND, rd(conf + CONF_COMMAND) | 1u << 2);
	/*
	 * The SMMU's PCIe TBU clock that DMA from PCIe needs (gcc_ddrss_pcie_sf_tbu_clk, as the Wi-Fi's bring-up turns it on):
	 * without it the X55's boot ROM failed to read its boot loader (BHI error 0xef0e0003 with our address in its
	 * debug register, then the link gone; 9 November). Only now, with the link up: read right after PCIe 2's power switch
	 * came on, the TBU clocks took the kernel down with an SError. aggre_noc_pcie_tbu (0x9000c) is left alone.
	 */
	{
		uint32_t sf = rd(gcc + GCC_SF_TBU_CBCR);

		say("munch-modem: ddrss_pcie_sf_tbu (%#lx) = %#x\n", GCC_PHYS + GCC_SF_TBU_CBCR, sf);
		if (sf != 0 && (sf & CBCR_OFF)) {
			gcc_step("ddrss_pcie_sf_tbu on", GCC_SF_TBU_CBCR, sf | 1);
			say("munch-modem: ddrss_pcie_sf_tbu now %#x\n", rd(gcc + GCC_SF_TBU_CBCR));
		}
	}
	/*
	 * And the aggregate NoC's PCIe TBU, the SMMU unit PCIe's own reads go through (the Wi-Fi's bring-up, whose boot ROM
	 * read its boot loader, turns on both; 9 November evening: with only the system fabric's, the X55's boot ROM still
	 * failed with our address in its debug register and the SMMU saw nothing -- the reads never reached it). Read here,
	 * with the link long up, as the other one is: right after the power switch came on, reading it was an SError.
	 * Boot-arg munch_modem_tbu=0: left as it is.
	 */
	{
		uint32_t want = 1, tbu;

		PE_parse_boot_argn("munch_modem_tbu", &want, sizeof(want));
		if (want) {
			tbu = rd(gcc + GCC_TBU_CBCR);
			say("munch-modem: aggre_noc_pcie_tbu (%#lx) = %#x\n", GCC_PHYS + GCC_TBU_CBCR, tbu);
			if (tbu != 0 && (tbu & CBCR_OFF)) {
				gcc_step("aggre_noc_pcie_tbu on", GCC_TBU_CBCR, tbu | 1);
				delay_for_interval(1, NSEC_PER_MSEC);
				say("munch-modem: aggre_noc_pcie_tbu now %#x\n", rd(gcc + GCC_TBU_CBCR));
			}
		}
	}
	hakios_wifi_radio_space_want(X55_STREAM, 0);
	{
		/* the device tree's pool for the X55 (mhi_0_iommu_group: 0x20000000), as Android has it; munch_modem_iova=N for another */
		uint64_t iova = 0x20000000;

		PE_parse_boot_argn("munch_modem_iova", &iova, sizeof(iova));
		hakios_wifi_radio_space_base(iova);
	}
	copy = dma_alloc(size, &address);
	if (copy == NULL) {
		return false;
	}
	memcpy(copy, image, size);
	__asm__ volatile ("dsb sy" ::: "memory");
	/*
	 * PCIe 2 is dma-coherent (dt): the X55's reads look into the processors' caches. The copy went in through the
	 * uncached view, but the memory's cached view may hold lines filled before it (the zeroes of the allocation,
	 * fetched again by the processor on its own), and a snooping read takes those over memory: the boot ROM read the
	 * whole image through our SMMU context and refused it (0xef0e0003; 10 November, munch_modem_phys=1 showed the reads
	 * do go through the context). So the cached view is cleaned and invalidated after the copy too.
	 */
	for (vm_offset_t line = dma_last_cached; line < dma_last_cached + dma_last_size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	{
		const uint8_t *seen = (const uint8_t *)dma_last_cached;

		say("munch-modem: through the cached view: %02x %02x %02x %02x ... ends %02x %02x\n", seen[0], seen[1], seen[2], seen[3],
		    seen[size - 2], seen[size - 1]);
	}
	/*
	 * Boot-arg munch_modem_phys=1: the boot ROM is given the copy's physical address instead of the one our SMMU context
	 * shows it at. If its reads did not go through that context (no fault in it, ever, though the boot ROM reads the whole
	 * image), this one works; if they do, the context reports a fault at this address. Either way, what the reads go
	 * through is known.
	 */
	{
		uint32_t phys = 0;

		if (PE_parse_boot_argn("munch_modem_phys", &phys, sizeof(phys)) && phys) {
			say("munch-modem: munch_modem_phys=1: the boot ROM gets the physical address %#llx instead of %#llx\n",
			    dma_last_physical, address);
			address = dma_last_physical;
		}
	}
	say("munch-modem: the copy begins %02x %02x %02x %02x ... ends %02x %02x (the image: %02x %02x %02x %02x)\n", copy[0], copy[1],
	    copy[2], copy[3], copy[size - 2], copy[size - 1], image[0], image[1], image[2], image[3]);
	read_random(&session, sizeof(session));
	session = (session & 0x3fffffff) | 1;
	say("munch-modem: BHI: sbl1.mbn (%llu KB) at %#llx, session %#x\n", size >> 10, address, session);
	wr(bhi + BHI_STATUS, 0);
	wr(bhi + BHI_INTVEC, 0);
	wr(bhi + BHI_IMGADDR_HIGH, (uint32_t)(address >> 32));
	wr(bhi + BHI_IMGADDR_LOW, (uint32_t)address);
	wr(bhi + BHI_IMGSIZE, (uint32_t)size);
	wr(bhi + BHI_IMGTXDB, session);
	for (ms = 0; ms < 5000; ms += 10) {
		status = rd(bhi + BHI_STATUS);
		if ((status >> 30) == 2 || (status >> 30) == 3) {
			break;
		}
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	if ((status >> 30) != 2) {
		say("munch-modem: BHI %s after %u ms: status %#x, error %#x (%#x %#x %#x)\n", (status >> 30) == 3 ? "FAILED" : "NO ANSWER",
		    ms, status, rd(bhi + BHI_ERRCODE), rd(bhi + BHI_ERRDBG1), rd(bhi + BHI_ERRDBG2), rd(bhi + BHI_ERRDBG3));
		mhi_say();
		smmu_faults();
		return false;
	}
	say("munch-modem: BHI DONE after %u ms: THE X55'S BOOT ROM HAS TAKEN ITS BOOT LOADER (status %#x)\n", ms, status);
	for (ms = 0; ms < 3000 && rd(bhi + BHI_EXECENV) == EE_PBL; ms += 10) {
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	mhi_say();
	say("munch-modem: MDM2AP status %u, errfatal %u\n", rd(tlmm_south + GPIO_IO(3)) & 1, rd(tlmm_south + GPIO_IO(1)) & 1);
	return true;
}

/* As Android switches it on the first time: a warm reset of its PMIC, then AP2MDM status high; MDM2AP status watched. */
static void
power_on(void)
{
	const char *problem;
	uint32_t ms;

	if (!pon_ours) {
		say("munch-modem: the modem's PMIC's power-on block is not ours to write: the modem is left off\n");
		return;
	}
	say("munch-modem: THE MODEM IS SWITCHED ON: a warm reset of its PMIC, then AP2MDM status high (munch_modem=2)\n");
	if ((problem = pon_put(PON_SW_RST_S2_CTL2, 0)) != NULL) {
		goto failed;
	}
	delay_for_interval(500, NSEC_PER_USEC);
	if ((problem = pon_put(PON_SW_RST_S2_CTL, PON_WARM_RESET)) != NULL ||
	    (problem = pon_put(PON_SW_RST_S2_CTL2, PON_RESET_EN)) != NULL) {
		goto failed;
	}
	delay_for_interval(500, NSEC_PER_USEC);
	if ((problem = pon_put(PON_SW_RST_GO, PON_SW_RST_GO_VAL)) != NULL) {
		goto failed;
	}
	delay_for_interval(150, NSEC_PER_MSEC);
	/* AP2MDM status: a GPIO (function 0), output, 2 mA, no pull, driven high */
	wr(tlmm_south + GPIO_IO(56), 1u << 1);
	wr(tlmm_south + GPIO_CFG(56), 1u << 9);
	say("munch-modem: AP2MDM status high (GPIO 56: cfg %#x, io %#x)\n", rd(tlmm_south + GPIO_CFG(56)), rd(tlmm_south + GPIO_IO(56)));
	for (ms = 0; ms < 10000; ms += 10) {
		if (rd(tlmm_south + GPIO_IO(3)) & 1) {
			break;
		}
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	say("munch-modem: MDM2AP status %s after %u ms; MDM2AP errfatal %u\n", (rd(tlmm_south + GPIO_IO(3)) & 1) ?
	    "HIGH: THE MODEM IS UP, its boot ROM waits for its link" : "still low", ms, rd(tlmm_south + GPIO_IO(1)) & 1);
	return;
failed:
	say("munch-modem: switching the modem on: %s; left as it is\n", problem);
}

static const struct {
	uint32_t pin;
	const char *name;
} pins[] = {
	{ 56, "AP2MDM status" }, { 57, "AP2MDM errfatal" }, { 3, "MDM2AP status" }, { 1, "MDM2AP errfatal" },
	{ 85, "PCIe 2 PERST" }, { 87, "PCIe 2 WAKE" },
};

static void
look_pins(void)
{
	vm_offset_t tlmm = tlmm_south = ml_io_map(TLMM_SOUTH_PHYS, TLMM_SOUTH_SIZE);

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(tlmm + GPIO_CFG(pins[i].pin)), io = rd(tlmm + GPIO_IO(pins[i].pin));

		say("munch-modem: GPIO %u (%s): function %u, %s, pull %u, drive %u mA; in %u, out %u\n", pins[i].pin, pins[i].name,
		    cfg >> 2 & 15, (cfg & (1u << 9)) ? "output" : "input", cfg & 3, ((cfg >> 6 & 7) + 1) * 2, io & 1, io >> 1 & 1);
	}
}

static bool
look_clock(vm_offset_t gcc, const char *name, uint32_t offset)
{
	uint32_t value = rd(gcc + offset);

	say("munch-modem: PCIe 2 %s (%#lx) = %#x: %s\n", name, GCC_PHYS + offset, value, (value & CBCR_OFF) ? "off" : "running");
	return !(value & CBCR_OFF);
}

static void
look_pcie(void)
{
	vm_offset_t gcc = ml_io_map(GCC_PHYS, GCC_SIZE), parf;
	uint32_t gdsc = rd(gcc + GCC_PCIE_2_GDSCR);
	bool ahb;

	say("munch-modem: PCIe 2's power switch (%#lx) = %#x: %s; clock votes %#x\n", GCC_PHYS + GCC_PCIE_2_GDSCR, gdsc,
	    (gdsc & GDSC_POWER_ON) ? "on" : "off", rd(gcc + GCC_VOTE_PCIE_2) >> 10 & 0x3f);
	look_clock(gcc, "slv_q2a_axi", GCC_PCIE_2_SLV_Q2A_CBCR);
	look_clock(gcc, "slv_axi", GCC_PCIE_2_SLV_AXI_CBCR);
	look_clock(gcc, "mstr_axi", GCC_PCIE_2_MSTR_AXI_CBCR);
	ahb = look_clock(gcc, "cfg_ahb", GCC_PCIE_2_CFG_AHB_CBCR);
	look_clock(gcc, "aux", GCC_PCIE_2_AUX_CBCR);
	look_clock(gcc, "pipe", GCC_PCIE_2_PIPE_CBCR);
	look_clock(gcc, "phy refgen", GCC_PCIE2_PHY_REFGEN);
	look_clock(gcc, "reference out to the modem", GCC_PCIE_MDM_CLKREF);
	if (!(gdsc & GDSC_POWER_ON) || !ahb) {
		say("munch-modem: PCIe 2 is not powered and clocked: its controller is not read\n");
		return;
	}
	parf = ml_io_map(PARF_PHYS, PARF_SIZE);
	say("munch-modem: PCIe 2's controller: phy ctrl %#x, link state %#x (0x11: up)\n", rd(parf + PARF_PHY_CTRL),
	    rd(parf + PARF_LTSSM));
}

/*
 * PCIe 2 and the X55 put back, the reverse of pcie2_up() and sbl_load(): the root port no longer passes the modem's
 * requests on (bus master off: its DMA stops), the modem held in reset again (PERST low), the link's training off, the
 * clock votes, the modem's reference clock, the PHY's refgen and the power switch as they were found, the pins too.
 * The endpoint's own configuration space is not touched: with the link gone (after a failed BHI it is), a read of it
 * can be an SError. The supplies (L9, L5) keep our vote: the rails are shared. Until 10 November there was none of
 * this, and every run with the modem hung on its way back to the loader. `leaving': on that way, interrupts off --
 * waits by spinning, nothing to the console; and the radio's SMMU context is the Wi-Fi driver's to take back there.
 */
extern void hakios_wifi_radio_space_off(void);

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
modem_down(bool leaving)
{
	if (!pcie2_touched) {
		return;
	}
	pcie2_touched = false;
	if (leaving) {
		say_quiet = true;
	}
	if (dbi != 0 && (rd(gcc + GCC_PCIE_2_GDSCR) & GDSC_POWER_ON) && !(rd(gcc + GCC_PCIE_2_CFG_AHB_CBCR) & CBCR_OFF)) {
		put_mask(dbi, CONF_COMMAND, 1u << 2, 0);         /* the root port: bus master off */
		put_mask(parf, PARF_LTSSM, 1u << 8, 0);          /* the link's training off */
	}
	pin_put(PIN_PERST, CFG_PULL_DOWN | CFG_MA(2) | CFG_OUTPUT, 0);
	if (leaving) {
		spin_us(1000);
	} else {
		hakios_wifi_radio_space_off();
		sleep_us(1000);
	}
	wr(gcc + GCC_VOTE_PCIE_2, found_vote);
	wr(gcc + GCC_PCIE_MDM_CLKREF, found_clkref);
	wr(gcc + GCC_PCIE2_PHY_REFGEN, found_refgen);
	if (leaving) {
		spin_us(100);
	} else {
		sleep_us(100);
	}
	wr(gcc + GCC_PCIE_2_GDSCR, found_gdsc);
	wr(tlmm_south + GPIO_CFG(PIN_WAKE), found_wake_cfg);
	wr(tlmm_south + GPIO_CFG(PIN_CLKREQ), found_clkreq_cfg);
	wr(tlmm_south + GPIO_IO(PIN_PERST), found_perst_io & ~(1u << 1));        /* (in reset either way) */
	wr(tlmm_south + GPIO_CFG(PIN_PERST), found_perst_cfg);
	say("munch-modem: PCIe 2 and the X55 put back: bus master and the link off, the modem in reset, clocks, power switch "
	    "and pins as they were found (vote %#x, gdsc %#x)\n", rd(gcc + GCC_VOTE_PCIE_2), rd(gcc + GCC_PCIE_2_GDSCR));
}

/* From osfmk/arm64/loader_return.c (a weak hook): the run is ending, whatever the modem was doing. */
void
hakios_munch_modem_quiesce(void)
{
	modem_down(true);
}

/*
 * What Android's eSoC driver does before the link and the boot loader, the part of it that is ours (esoc-mdm-4x.c
 * ESOC_PWR_ON, esoc-mdm-pon.c mdm4x_do_first_power_on()): AP2MDM errfatal low, AP2MDM status high -- "the host is up"
 * --, 150 ms, and the 200 ms it waits for the boot ROM. (Its warm reset of the modem's PMIC goes through a block of the
 * PMIC arbiter that is not ours to write: munch_modem=2.) Until 10 November the levels 3 to 5 left both pins as the boot
 * chain had them, and the X55's boot ROM read its whole boot loader over BHI and refused it (0xef0e0003).
 * Boot-arg munch_modem_ap2mdm=0: left as they are.
 */
static void
ap2mdm_ready(void)
{
	uint32_t want = 1;

	PE_parse_boot_argn("munch_modem_ap2mdm", &want, sizeof(want));
	if (!want) {
		return;
	}
	wr(tlmm_south + GPIO_IO(57), 0);
	wr(tlmm_south + GPIO_CFG(57), 1u << 9);         /* AP2MDM errfatal: a GPIO, output, 2 mA, no pull, low */
	wr(tlmm_south + GPIO_IO(56), 1u << 1);
	wr(tlmm_south + GPIO_CFG(56), 1u << 9);         /* AP2MDM status: the same, high */
	delay_for_interval(350, NSEC_PER_MSEC);
	say("munch-modem: AP2MDM errfatal low (cfg %#x, io %#x), status high (cfg %#x, io %#x); MDM2AP status %u, errfatal %u\n",
	    rd(tlmm_south + GPIO_CFG(57)), rd(tlmm_south + GPIO_IO(57)), rd(tlmm_south + GPIO_CFG(56)), rd(tlmm_south + GPIO_IO(56)),
	    rd(tlmm_south + GPIO_IO(3)) & 1, rd(tlmm_south + GPIO_IO(1)) & 1);
}

static void
modem_thread(__unused void *parameter, __unused wait_result_t result)
{
	delay_for_interval(10, NSEC_PER_SEC);
	say("munch-modem: a look at the Snapdragon X55 as the boot chain left it\n");
	look_pins();
	look_pcie();
	if (look_pmic() && look_level == 2) {
		power_on();
	}
	if (look_level >= 3) {
		ap2mdm_ready();
	}
	/*
	 * Boot-arg munch_modem_socreset=1: a fresh boot ROM first. Android warm-resets the X55 through its PMIC's power-on
	 * block before the boot loader goes over BHI; that block's channel at the SPMI arbiter belongs to another execution
	 * environment (channel 13, "NOT ours to write") and is not written. The modem can be asked to reset itself instead,
	 * through its own MHI registers on our link (the SoC reset request, as Linux's mhi_soc_reset()); then the link is
	 * taken down and brought up again around it, and the boot loader goes to what comes up.
	 */
	{
		uint32_t socreset = 0;

		PE_parse_boot_argn("munch_modem_socreset", &socreset, sizeof(socreset));
		if (socreset && look_level >= 5 && pcie2_up() && x55_registers()) {
			say("munch-modem: the X55 asked to reset itself (MHI SoC reset request), then PCIe 2 down and up again\n");
			wr(bar + MHI_SOC_RESET_REQ, 1);
			delay_for_interval(500, NSEC_PER_MSEC);
			modem_down(false);
			delay_for_interval(500, NSEC_PER_MSEC);
		}
	}
	if (look_level >= 3 && pcie2_up() && look_level >= 4 && x55_registers() && look_level >= 5) {
		(void)sbl_load();
	}
	/* (nothing uses the modem after these steps yet: put back now, not at the run's end; munch_modem_keep=1: left up) */
	{
		uint32_t keep = 0;

		PE_parse_boot_argn("munch_modem_keep", &keep, sizeof(keep));
		if (!keep) {
			modem_down(false);
		}
	}
	thread_terminate(current_thread());
}

__startup_func
static void
modem_init(void)
{
	thread_t thread;

	if (!hakios_soc_is("sm8250") || !PE_parse_boot_argn("munch_modem", &look_level, sizeof(look_level)) || look_level == 0) {
		return;
	}
	if (kernel_thread_start(modem_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, modem_init);
