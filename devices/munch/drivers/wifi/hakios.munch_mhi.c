/*
 * The POCO F4's (munch) QCA6390 started as a host of the MHI protocol starts it: steps 6 and 7 of
 * the Wi-Fi, after hakios_munch_pcie.c has the link up and the card's registers at BAR0 (step 5).
 * Linux's MHI host (drivers/bus/mhi) is GPL-2.0 and is not used: this is written from what the
 * protocol is -- registers, contexts, rings and their elements -- and from what ath11k (BSD-3-Clause-
 * Clear: pci.c, mhi.c) does to the chip around it.
 *
 *   pcie_probe=6   the chip reset as ath11k_pci_sw_reset() resets it at power-on (the link's state machine,
 *                  its pending interrupts, the fuse power control, the L1 sub-state fix of the QCA6390's
 *                  PHY, BHIe's vector registers, the debug words the firmware's warm path reads, the
 *                  global reset, MHI reset), its view of our memory made (stream 0x1c01 of the SMMU, the
 *                  card's own: hakios_wifi_radio_space_want(); pcie_sid=N for another), bus mastering on, and the first
 *                  512 KB of its firmware (fw/amss20.bin: the phone's /vendor/firmware_mnt/image/qca6390)
 *                  given to its boot ROM through BHI, which checks and runs it: the "SBL"
 *   pcie_probe=7   ... and MHI itself: its contexts and rings in memory of ours (128 channel contexts, of
 *                  which 20 and 21, QRTR's "IPCR", are ath11k's; two event rings of 32 and 256 elements; a
 *                  command ring), the MHI READY waited for, the registers that say where all of it is, the
 *                  event rings handed over, the state M0 asked for, and the whole firmware through BHIe (a
 *                  table of 512 KB pieces); then what the chip says on its event rings, until it is in
 *                  "mission mode" (AMSS) or pcie_mhi_wait=S seconds (default 20) are over
 *   pcie_probe=8   ... and QRTR on channels 20 and 21 (below, before hakios_munch_mhi_run())
 * No interrupt is taken: the chip's MSI is on (msi_on(): it is so for ath11k), but its registers and
 * rings are looked at. Nothing here is left behind: hakios_munch_mhi_stop() switches the MSI off and
 * puts the chip in MHI reset, and bus mastering is taken away; the card is then held in reset and unpowered by hakios_munch_pcie.c, and the SMMU stream
 * put back.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/random.h>
#include <sys/hakios.input.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

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
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static vm_offset_t bar;

static inline uint32_t
rd(uint32_t offset)
{
	return *(volatile uint32_t *)(bar + offset);
}

static inline void
wr(uint32_t offset, uint32_t value)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(bar + offset) = value;
}

/* ---- the chip's own registers beyond the first 512 KB, through ath11k's window ----------------------- */

#define WINDOW                  0x310c          /* bit 30 on, [5:0] the offset's bits [24:19] */
#define WINDOW_START            0x80000

static uint32_t
far_rd(uint32_t offset)
{
	if (offset < WINDOW_START) {
		return rd(offset);
	}
	wr(WINDOW, 1u << 30 | (offset >> 19 & 0x3f));
	(void)rd(WINDOW);
	return rd(WINDOW_START + (offset & 0x7ffff));
}

static void
far_wr(uint32_t offset, uint32_t value)
{
	if (offset < WINDOW_START) {
		wr(offset, value);
		return;
	}
	wr(WINDOW, 1u << 30 | (offset >> 19 & 0x3f));
	(void)rd(WINDOW);
	wr(WINDOW_START + (offset & 0x7ffff), value);
}

/* ath11k_pci_set_link_reg(): write until it reads back, ten times at most */
static void
far_set(const char *name, uint32_t offset, uint32_t value, uint32_t mask)
{
	uint32_t v = far_rd(offset);

	for (unsigned i = 0; i < 10 && (v & mask) != value; i++) {
		far_wr(offset, (v & ~mask) | value);
		v = far_rd(offset);
		if ((v & mask) != value) {
			sleep_ms(2);
		}
	}
	say("mhi: %s (%#x) = %#x%s\n", name, offset, v, (v & mask) == value ? "" : ": DOES NOT TAKE IT");
}

/* numbers: ath11k's pci.h, mhi.h and hw.c (qca6390_regs) */
#define PCIE_SOC_GLOBAL_RESET   0x3008
#define PCIE_TXVECDB            0x360
#define PCIE_TXVECSTATUS        0x368
#define PCIE_RXVECDB            0x394
#define PCIE_RXVECSTATUS        0x39c
#define PCIE_PARF_LTSSM         0x1e081b0
#define PARF_LTSSM_VALUE        0x111
#define GCC_PCIE_HOT_RST        0x1e402bc
#define PCIE_INT_ALL_CLEAR      0x1e08228
#define WLAON_QFPROM_PWR_CTRL   0x1f8031c       /* bit 2: VDD4BLOW */
#define PCIE_Q6_COOKIE          0x1f80500
#define WLAON_WARM_SW_ENTRY     0x1f80504
#define WLAON_SOC_RESET_CAUSE   0x1f8060c       /* cleared by reading it */
#define QSERDES_SYSCLK_EN_SEL   0x1e0c0ac
#define PCS_OSC_DTCT_CONFIG     0x1e0c628       /* 1, 2 at + 4, 4 at + 0xc */

/* ---- MHI's registers (at the start of BAR0) and BHI's, BHIe's ------------------------------------------- */

#define MHICFG                  0x10            /* [31:24] hardware event rings, [23:16] event rings */
#define CHDBOFF                 0x18
#define ERDBOFF                 0x20
#define BHIOFF                  0x28
#define BHIEOFF                 0x2c
#define MHICTRL                 0x38            /* [15:8] the state asked for, bit 1 reset */
#define MHISTATUS               0x48            /* [15:8] the state, bit 2 system error, bit 0 ready */
#define CCABAP                  0x58            /* the channel contexts: low word, then high */
#define ECABAP                  0x60            /* the event rings' */
#define CRCBAP                  0x68            /* the command ring's */
#define CRDB                    0x70            /* the command ring's doorbell */
#define MHICTRLBASE             0x80
#define MHICTRLLIMIT            0x88
#define MHIDATABASE             0x98
#define MHIDATALIMIT            0xa0
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
#define BHIE_TXVECADDR_LOW      0x2c
#define BHIE_TXVECADDR_HIGH     0x30
#define BHIE_TXVECSIZE          0x34
#define BHIE_TXVECDB            0x3c            /* [29:0] a sequence number */
#define BHIE_TXVECSTATUS        0x44            /* [29:0] its number, [31:30] 2 done, 3 failed */
#define STATE_RESET             0
#define STATE_READY             1
#define STATE_M0                2
#define STATE_SYS_ERR           0xff
#define EE_PBL                  0
#define EE_SBL                  1
#define EE_AMSS                 2
#define DEV_WAKE_DOORBELL       127             /* a channel doorbell that only keeps the chip awake */

static const char *const ee_names[] = { "PBL", "SBL", "AMSS (mission mode)", "RDDM (crash dump)", "WFW", "PTHRU", "EDL",
	                                "FP" };
static uint32_t bhi, bhie, chdb, erdb;

static const char *
ee_name(uint32_t ee)
{
	return ee < sizeof(ee_names) / sizeof(ee_names[0]) ? ee_names[ee] : "?";
}

static uint32_t
mhi_state(void)
{
	return rd(MHISTATUS) >> 8 & 0xff;
}

static void
write64(uint32_t offset, uint64_t value)
{
	wr(offset + 4, (uint32_t)(value >> 32));
	wr(offset, (uint32_t)value);
}

static void
doorbell(uint32_t offset, uint64_t value)
{
	write64(offset, value);
}

/* ---- memory the chip reads and writes: in one piece, uncached, behind its SMMU stream ------------------ */

static uint64_t dma_last_physical;

static void *
dma_alloc(vm_size_t size, uint64_t *address)
{
	vm_offset_t cached = 0, uncached;
	uint64_t physical;

	size = round_page(size);
	if (kmem_alloc_contig(kernel_map, &cached, size, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		say("mhi: no %lu KB of memory in one piece\n", (unsigned long)(size >> 10));
		return NULL;
	}
	physical = kvtophys(cached);
	for (vm_offset_t line = cached; line < cached + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	dma_last_physical = physical;
	uncached = ml_io_map_unmappable(physical, size, VM_WIMG_WCOMB);
	if (uncached == 0 || !hakios_wifi_radio_map(physical, size, (unsigned long long *)address)) {
		say("mhi: %lu KB could not be shown to the chip\n", (unsigned long)(size >> 10));
		return NULL;
	}
	return (void *)uncached;
}

/* ---- MHI's structures in memory: contexts (44 bytes each) and ring elements (16) --------------------- */

struct context {
	uint32_t word0;                 /* channel: [7:0] state, [9:8] doorbell mode; event ring: [31:16] moderation (ms) */
	uint32_t word1;                 /* channel: 1 to the chip, 2 from it; event ring: 1 (valid) */
	uint32_t word2;                 /* channel: its event ring; event ring: its MSI vector */
	uint64_t base, length, read, write;
} __attribute__((packed));

struct element {
	uint64_t pointer;
	uint32_t word0, word1;
};

#define CHANNELS                128             /* ath11k_mhi_config_qca6390.max_channels */
#define EVENT_RINGS             2
#define CHANNEL_DOORBELL_DISABLE (2u << 8)      /* MHI_DB_BRST_DISABLE */
#define EVENT_STATE_CHANGE      0x20
#define EVENT_COMMAND_DONE      0x21
#define EVENT_TRANSFER          0x22
#define EVENT_EE                0x40

/* where each thing is in the one piece of memory: a ring at a multiple of its length, as MHI wants */
#define AT_CHANNEL_CONTEXTS     0x0000          /* 128 x 44 */
#define AT_EVENT_CONTEXTS       0x2000
#define AT_COMMAND_CONTEXT      0x2100
#define AT_EVENT_RING1          0x3000          /* 256 elements */
#define AT_EVENT_RING0          0x4000          /* 32 */
#define AT_COMMAND_RING         0x4800          /* 128 */
#define AT_CHANNEL20_RING       0x5000          /* 64 */
#define AT_CHANNEL21_RING       0x5400
#define MEMORY_SIZE             0x6000

static const struct {
	uint32_t at, elements, moderation_ms, vector;
} event_rings[EVENT_RINGS] = {
	{ AT_EVENT_RING0, 32, 0, 1 },           /* control: state changes, commands done */
	{ AT_EVENT_RING1, 256, 1, 2 },          /* data: QRTR's */
};

static uint8_t *memory;
static uint64_t memory_address;
static uint32_t event_read[EVENT_RINGS];        /* the element of each event ring to be looked at next */
static uint32_t last_ee = ~0u;
static uint32_t command_code;
static bool command_finished_flag;
static void transfer_done(unsigned channel, uint64_t element, uint32_t length, uint32_t code);

static struct context *
context(uint32_t at, unsigned n)
{
	return (struct context *)(memory + at) + n;
}

static void
contexts_make(void)
{
	struct context *c;

	for (unsigned n = 20; n <= 21; n++) {
		c = context(AT_CHANNEL_CONTEXTS, n);
		c->word0 = CHANNEL_DOORBELL_DISABLE;    /* disabled until started */
		c->word1 = n == 20 ? 1 : 2;
		c->word2 = 1;
	}
	for (unsigned n = 0; n < EVENT_RINGS; n++) {
		c = context(AT_EVENT_CONTEXTS, n);
		c->word0 = event_rings[n].moderation_ms << 16;
		c->word1 = 1;
		c->word2 = event_rings[n].vector;
		c->base = c->read = c->write = memory_address + event_rings[n].at;
		c->length = 16ull * event_rings[n].elements;
		event_read[n] = 0;
	}
	c = context(AT_COMMAND_CONTEXT, 0);
	c->base = c->read = c->write = memory_address + AT_COMMAND_RING;
	c->length = 16ull * 128;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* What the chip has put on event ring n since it was last looked at; each element given back. */
static void
events_look(unsigned n)
{
	struct context *c = context(AT_EVENT_CONTEXTS, n);
	uint64_t base = memory_address + event_rings[n].at;
	uint32_t elements = event_rings[n].elements;
	unsigned seen = 0;

	for (;;) {
		uint64_t device_read = c->read;
		struct element *e;
		uint32_t type, code;

		if (device_read < base || device_read >= base + 16ull * elements) {
			say("mhi: event ring %u: the chip's read pointer %#llx is outside the ring\n", n, device_read);
			return;
		}
		if ((device_read - base) / 16 == event_read[n] || seen == elements) {
			break;
		}
		e = (struct element *)(memory + event_rings[n].at) + event_read[n];
		type = e->word1 >> 16 & 0xff;
		code = e->word0 >> 24;
		switch (type) {
		case EVENT_STATE_CHANGE:
			say("mhi: event ring %u: STATE CHANGE to %u%s\n", n, code, code == STATE_M0 ? " (M0)" : code == STATE_READY ?
			    " (READY)" : code == STATE_SYS_ERR ? " (SYSTEM ERROR)" : "");
			break;
		case EVENT_EE:
			say("mhi: event ring %u: EXECUTION ENVIRONMENT %u: %s\n", n, code, ee_name(code));
			last_ee = code;
			break;
		case EVENT_COMMAND_DONE:
			say("mhi: event ring %u: command at %#llx done, code %u%s\n", n, e->pointer, code, code == 1 ? " (success)" : "");
			command_code = code;
			command_finished_flag = true;
			break;
		case EVENT_TRANSFER:
			transfer_done(e->word1 >> 24, e->pointer, e->word0 & 0xffff, code);
			break;
		default:
			say("mhi: event ring %u: element of type %#x: %#llx %#x %#x\n", n, type, e->pointer, e->word0, e->word1);
			break;
		}
		seen++;
		event_read[n] = (event_read[n] + 1) % elements;
		/* given back: the chip may write up to the element before the next to be read */
		c->write = base + 16ull * ((event_read[n] + elements - 1) % elements);
		__asm__ volatile ("dsb sy" ::: "memory");
		doorbell(erdb + 8 * n, c->write);
	}
}

/* ---- the reset, as ath11k_pci_sw_reset(ab, true) -------------------------------------------------------- */

static void
chip_reset(void)
{
	uint32_t v;

	say("mhi: step 6: the chip reset as ath11k does it at power-on\n");
	sleep_ms(100);
	v = far_rd(PCIE_PARF_LTSSM);
	for (unsigned i = 0; v != PARF_LTSSM_VALUE && i < 5; i++) {
		far_wr(PCIE_PARF_LTSSM, PARF_LTSSM_VALUE);
		v = far_rd(PCIE_PARF_LTSSM);
	}
	far_wr(GCC_PCIE_HOT_RST, far_rd(GCC_PCIE_HOT_RST) | 0x10);
	say("mhi: the chip's parf ltssm %#x, gcc pcie_hot_rst %#x\n", v, far_rd(GCC_PCIE_HOT_RST));
	sleep_ms(5);
	far_wr(PCIE_INT_ALL_CLEAR, 0xffffffff);
	far_wr(WLAON_QFPROM_PWR_CTRL, far_rd(WLAON_QFPROM_PWR_CTRL) & ~4u);
	far_set("qserdes sysclk_en_sel", QSERDES_SYSCLK_EN_SEL, 0x10, 0xffffffff);
	far_set("pcs osc_dtct_config1", PCS_OSC_DTCT_CONFIG, 0x02, 0xff);
	far_set("pcs osc_dtct_config2", PCS_OSC_DTCT_CONFIG + 4, 0x52, 0xff);
	far_set("pcs osc_dtct_config4", PCS_OSC_DTCT_CONFIG + 0xc, 0xff, 0xff);
	wr(PCIE_TXVECDB, 0);
	wr(PCIE_TXVECSTATUS, 0);
	wr(PCIE_RXVECDB, 0);
	wr(PCIE_RXVECSTATUS, 0);
	say("mhi: q6 cookie %#x, warm entry %#x\n", far_rd(PCIE_Q6_COOKIE), far_rd(WLAON_WARM_SW_ENTRY));
	sleep_ms(10);
	far_wr(WLAON_WARM_SW_ENTRY, 0);
	sleep_ms(10);
	say("mhi: warm entry now %#x, the last reset's cause %#x\n", far_rd(WLAON_WARM_SW_ENTRY), far_rd(WLAON_SOC_RESET_CAUSE));
	v = far_rd(PCIE_SOC_GLOBAL_RESET);
	far_wr(PCIE_SOC_GLOBAL_RESET, v | 1);
	sleep_ms(10);
	far_wr(PCIE_SOC_GLOBAL_RESET, v & ~1u);
	sleep_ms(10);
	say("mhi: global reset done (%#x); MHI status %#x, then MHI reset\n", far_rd(PCIE_SOC_GLOBAL_RESET), rd(MHISTATUS));
	wr(MHICTRL, 1u << 1);
	sleep_ms(10);
}

/* ---- the firmware -------------------------------------------------------------------------------------- */

#define SBL_SIZE                (512u * 1024)   /* ath11k: sbl_size, seg_len */
#define SEGMENT                 (512u * 1024)

static const uint8_t *firmware;
static uint64_t firmware_size;

static bool
wait_for(const char *what, uint32_t ms, bool (*done)(void))
{
	for (uint32_t t = 0; t < ms; t += 10) {
		if (done()) {
			return true;
		}
		if (t != 0 && t % 2000 == 0) {
			say("mhi: waiting for %s, %u s: MHI status %#x, ctrl %#x, environment %s, BHI status %#x, error %#x\n", what,
			    t / 1000, rd(MHISTATUS), rd(MHICTRL), ee_name(rd(bhi + BHI_EXECENV)), rd(bhi + BHI_STATUS),
			    rd(bhi + BHI_ERRCODE));
		}
		if (rd(MHISTATUS) & 4) {
			say("mhi: waiting for %s: THE CHIP SAYS SYSTEM ERROR (status %#x, environment %s)\n", what, rd(MHISTATUS),
			    ee_name(rd(bhi + BHI_EXECENV)));
			return false;
		}
		sleep_ms(10);
	}
	say("mhi: %s did not come in %u ms\n", what, ms);
	return false;
}

static bool
bhi_done(void)
{
	return (rd(bhi + BHI_STATUS) >> 30) >= 2;
}

static bool
sbl_load(void)
{
	uint64_t address;
	uint8_t *copy;
	uint32_t session, status;

	if (rd(bhi + BHI_EXECENV) != EE_PBL) {
		say("mhi: the chip is in %s, not its boot ROM: no SBL to give it\n", ee_name(rd(bhi + BHI_EXECENV)));
		return false;
	}
	copy = dma_alloc(SBL_SIZE, &address);
	if (copy == NULL) {
		return false;
	}
	memcpy(copy, firmware, SBL_SIZE);
	__asm__ volatile ("dsb sy" ::: "memory");
	read_random(&session, sizeof(session));
	session = (session & 0x3fffffff) | 1;
	say("mhi: BHI: the first %u KB of the firmware at %#llx, session %#x\n", SBL_SIZE >> 10, address, session);
	wr(bhi + BHI_INTVEC, 0);
	wr(bhi + BHI_IMGADDR_HIGH, (uint32_t)(address >> 32));
	wr(bhi + BHI_IMGADDR_LOW, (uint32_t)address);
	wr(bhi + BHI_IMGSIZE, SBL_SIZE);
	wr(bhi + BHI_IMGTXDB, session);
	if (!wait_for("BHI's end", 5000, bhi_done)) {
		return false;
	}
	status = rd(bhi + BHI_STATUS);
	if ((status >> 30) != 2) {
		say("mhi: BHI FAILED: status %#x, error %#x (%#x %#x %#x)\n", status, rd(bhi + BHI_ERRCODE), rd(bhi + BHI_ERRDBG1),
		    rd(bhi + BHI_ERRDBG2), rd(bhi + BHI_ERRDBG3));
		return false;
	}
	say("mhi: BHI DONE: THE BOOT ROM HAS TAKEN THE SBL (status %#x); MHI status %#x, environment %s\n", status, rd(MHISTATUS),
	    ee_name(rd(bhi + BHI_EXECENV)));
	return true;
}

static bool
ready(void)
{
	return !(rd(MHICTRL) & 1u << 1) && (rd(MHISTATUS) & 1);
}

static bool
in_m0(void)
{
	events_look(0);
	return mhi_state() == STATE_M0;
}

static bool
in_sbl(void)
{
	events_look(0);
	return rd(bhi + BHI_EXECENV) == EE_SBL || last_ee == EE_SBL;
}

static bool
in_amss(void)
{
	events_look(0);
	events_look(1);
	return last_ee == EE_AMSS;
}

static uint32_t bhie_sequence;
static void registers_dump(void);

static bool
bhie_done(void)
{
	uint32_t status = rd(bhie + BHIE_TXVECSTATUS);

	return (status & 0x3fffffff) == bhie_sequence && (status >> 30) >= 2;
}

/*
 * A crash of the firmware: the chip goes to the environment RDDM and writes its memory to where BHIe's
 * receive side says (Linux: mhi_rddm_prepare(), and ath11k's RDDM_DUMP_SIZE; dt qcom,wlan-ramdump-dynamic).
 * With pcie_rddm=1 that is got ready, and what comes is left for `fastboot oem sound` the way the
 * sound's recording is (loader_sound[], osfmk/arm64/loader_return.c), to be looked at on the computer.
 */
#define RDDM_SIZE               0x420000u
#define BHIE_RXVECADDR_LOW      0x60
#define BHIE_RXVECADDR_HIGH     0x64
#define BHIE_RXVECSIZE          0x68
#define BHIE_RXVECDB            0x70            /* [29:0] a sequence number */
#define BHIE_RXVECSTATUS        0x78            /* [29:0] its number, [31:30] 2 done, 3 failed */

extern uint64_t loader_sound[3];
static uint8_t *rddm;
static uint64_t rddm_physical;

static void
rddm_prepare(void)
{
	uint32_t want = 0, pieces = (RDDM_SIZE + SEGMENT - 1) / SEGMENT, sequence;
	uint64_t address;
	uint64_t *table;

	if (!PE_parse_boot_argn("pcie_rddm", &want, sizeof(want)) || want == 0) {
		return;
	}
	rddm = dma_alloc(RDDM_SIZE + 4096, &address);
	if (rddm == NULL) {
		return;
	}
	rddm_physical = dma_last_physical;
	table = (uint64_t *)(rddm + RDDM_SIZE);
	for (uint32_t i = 0; i < pieces; i++) {
		table[2 * i] = address + (uint64_t)i * SEGMENT;
		table[2 * i + 1] = MIN(SEGMENT, RDDM_SIZE - i * SEGMENT);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	read_random(&sequence, sizeof(sequence));
	sequence = (sequence & 0x3fffffff) | 1;
	wr(bhie + BHIE_RXVECADDR_HIGH, (uint32_t)((address + RDDM_SIZE) >> 32));
	wr(bhie + BHIE_RXVECADDR_LOW, (uint32_t)(address + RDDM_SIZE));
	wr(bhie + BHIE_RXVECSIZE, 16 * pieces);
	wr(bhie + BHIE_RXVECDB, (rd(bhie + BHIE_RXVECDB) & ~0x3fffffffu) | sequence);
	say("mhi: a crash dump would go to %#llx, %u pieces (sequence %#x)\n", address, pieces, sequence);
}

/* After the chip has said it has crashed: its dump, up to five seconds. */
static void
rddm_fetch(void)
{
	uint32_t status = 0;

	if (rddm == NULL) {
		return;
	}
	for (unsigned ms = 0; ms < 5000 && ((status = rd(bhie + BHIE_RXVECSTATUS)) >> 30) < 2; ms += 10) {
		sleep_ms(10);
	}
	say("mhi: the crash dump: status %#x%s\n", status, status >> 30 == 2 ? ": THE CHIP HAS WRITTEN IT (xl: oem sound)" : "");
	loader_sound[0] = rddm_physical;
	loader_sound[1] = RDDM_SIZE;
	loader_sound[2] = (uint64_t)rddm;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* MHI's own start: READY, where everything is, the event rings handed over, M0 */
static bool
mhi_up(void)
{
	uint32_t cfg;

	memory = dma_alloc(MEMORY_SIZE, &memory_address);
	if (memory == NULL) {
		return false;
	}
	contexts_make();
	say("mhi: step 7: contexts and rings at %#llx; waiting for MHI READY (status %#x, ctrl %#x)\n", memory_address,
	    rd(MHISTATUS), rd(MHICTRL));
	if (!wait_for("MHI READY", 20000, ready)) {        /* ath11k: MHI_TIMEOUT_DEFAULT_MS */
		registers_dump();
		return false;
	}
	chdb = rd(CHDBOFF);
	erdb = rd(ERDBOFF);
	cfg = rd(MHICFG);
	say("mhi: READY; channel doorbells at %#x, event rings' at %#x, cfg %#x\n", chdb, erdb, cfg);
	wr(MHICFG, (cfg & 0x0000ffff) | EVENT_RINGS << 16);
	write64(CCABAP, memory_address + AT_CHANNEL_CONTEXTS);
	write64(ECABAP, memory_address + AT_EVENT_CONTEXTS);
	write64(CRCBAP, memory_address + AT_COMMAND_CONTEXT);
	write64(MHICTRLBASE, 0);
	write64(MHICTRLLIMIT, 0xfffffffffull);  /* ath11k: the DMA mask, 36 bits */
	write64(MHIDATABASE, 0);
	write64(MHIDATALIMIT, 0xfffffffffull);
	for (unsigned n = 0; n < EVENT_RINGS; n++) {
		struct context *c = context(AT_EVENT_CONTEXTS, n);

		c->write = c->base + c->length - 16;
		__asm__ volatile ("dsb sy" ::: "memory");
		doorbell(erdb + 8 * n, c->write);
	}
	wr(MHICTRL, (rd(MHICTRL) & ~0xff02u) | STATE_M0 << 8);
	doorbell(chdb + 8 * DEV_WAKE_DOORBELL, 1);
	if (!wait_for("M0", 5000, in_m0)) {
		return false;
	}
	say("mhi: M0: MHI IS UP (status %#x), environment %s\n", rd(MHISTATUS), ee_name(rd(bhi + BHI_EXECENV)));
	return true;
}

static bool
amss_load(void)
{
	uint32_t pieces = (uint32_t)((firmware_size + SEGMENT - 1) / SEGMENT), status;
	uint64_t table_address, piece_address;
	uint64_t *table;

	if (!wait_for("the SBL's environment", 10000, in_sbl)) {
		return false;
	}
	bhie = rd(BHIEOFF);
	say("mhi: the SBL runs; BHIe at %#x; the whole firmware (%llu KB) in %u pieces of %u KB\n", bhie, firmware_size >> 10,
	    pieces, SEGMENT >> 10);
	table = dma_alloc(16 * pieces, &table_address);
	if (table == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < pieces; i++) {
		uint32_t length = (uint32_t)MIN(SEGMENT, firmware_size - (uint64_t)i * SEGMENT);
		uint8_t *piece = dma_alloc(SEGMENT, &piece_address);

		if (piece == NULL) {
			return false;
		}
		memcpy(piece, firmware + (uint64_t)i * SEGMENT, length);
		table[2 * i] = piece_address;
		table[2 * i + 1] = length;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	read_random(&bhie_sequence, sizeof(bhie_sequence));
	bhie_sequence = (bhie_sequence & 0x3fffffff) | 1;
	say("mhi: BHIe: the table at %#llx, sequence %#x\n", table_address, bhie_sequence);
	wr(bhie + BHIE_TXVECADDR_HIGH, (uint32_t)(table_address >> 32));
	wr(bhie + BHIE_TXVECADDR_LOW, (uint32_t)table_address);
	wr(bhie + BHIE_TXVECSIZE, 16 * pieces);
	wr(bhie + BHIE_TXVECDB, (rd(bhie + BHIE_TXVECDB) & ~0x3fffffffu) | bhie_sequence);
	if (!wait_for("BHIe's end", 10000, bhie_done)) {
		say("mhi: BHIe status %#x\n", rd(bhie + BHIE_TXVECSTATUS));
		return false;
	}
	status = rd(bhie + BHIE_TXVECSTATUS);
	if ((status >> 30) != 2) {
		say("mhi: BHIe FAILED: status %#x\n", status);
		return false;
	}
	say("mhi: BHIe DONE: THE SBL HAS TAKEN THE FIRMWARE (status %#x)\n", status);
	rddm_prepare();
	return true;
}

/*
 * The chip's MSI: on before MHI starts, as ath11k_pci_power_up() has it (ath11k_pci_msi_enable() before
 * ath11k_mhi_start()), 32 vectors -- ath11k's for the QCA6390: MHI's 0 to 2, then the copy engines',
 * the wake's and the data path's -- written to the GIC's register that raises SPI 800 + v, which the
 * SMMU context shows the chip as a device's page (hakios_wifi_radio_msi()). Nobody takes those SPIs
 * yet: the GIC holds them, and the rings are looked at instead.
 */
static vm_offset_t config;
static uint32_t msi_at;

static bool
msi_on(void)
{
	unsigned long long address;
	uint32_t data, first, word;

	for (msi_at = *(volatile uint32_t *)(config + 0x34) & 0xfc; msi_at != 0;
	    msi_at = *(volatile uint32_t *)(config + msi_at) >> 8 & 0xfc) {
		if ((*(volatile uint32_t *)(config + msi_at) & 0xff) == 5) {
			break;
		}
	}
	if (msi_at == 0 || !hakios_wifi_radio_msi(&address, &data, &first)) {
		say("mhi: no MSI for the chip\n");
		msi_at = 0;
		return false;
	}
	*(volatile uint32_t *)(config + msi_at + 4) = (uint32_t)address;
	*(volatile uint32_t *)(config + msi_at + 8) = data;
	word = *(volatile uint32_t *)(config + msi_at);
	*(volatile uint32_t *)(config + msi_at) = (word & ~(7u << 20)) | 5u << 20 | 1u << 16;
	__asm__ volatile ("dsb sy" ::: "memory");
	say("mhi: MSI on: 32 vectors, %#x + v written at %#llx (capability at %#x: %#x %#x %#x)\n", data, address, msi_at,
	    *(volatile uint32_t *)(config + msi_at), *(volatile uint32_t *)(config + msi_at + 4),
	    *(volatile uint32_t *)(config + msi_at + 8));
	return true;
}

static void
registers_dump(void)
{
	for (uint32_t at = 0; at < 0x200; at += 0x20) {
		say("mhi: %03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", at, rd(at), rd(at + 4), rd(at + 8), rd(at + 12),
		    rd(at + 16), rd(at + 20), rd(at + 24), rd(at + 28));
	}
}

/* ---- step 8: channels 20 and 21 ("IPCR"), and QRTR over them ----------------------------------------- */

/*
 *   pcie_probe=8   ... and the two channels ath11k's QCA6390 has, started with the command ring's "start
 *                  channel": 20 to the chip, 21 from it, both on event ring 1; 16 buffers of 8 KB given to
 *                  21 for what comes; then QRTR on them, the layer redwood's radio speaks over GLINK
 *                  (hakios_wifi.c: hakios_wifi_qrtr_over()): one datagram in each transfer, as Linux's
 *                  net/qrtr/mhi.c has them. For pcie_qrtr_wait=S seconds (default 5) what comes is read and
 *                  answered: the chip's hello, and the services it tells of -- 69 (0x45) is the Wi-Fi
 *                  firmware's, WLFW, which ath11k's qmi.c speaks to next
 */
#define COMMAND_START_CHANNEL   0x12
#define TRANSFER_TYPE           (2u << 16)
#define TRANSFER_IEOT           (1u << 9)       /* an event when this element is done */
#define CHANNEL_ELEMENTS        64
#define BUFFERS                 16
#define BUFFER_SIZE             8192            /* ath11k_mhi_config_qca6390.buf_len */

static struct {
	uint32_t number, at;
	uint32_t write;                         /* the element to be filled next */
	uint8_t *buffers;
	uint64_t buffers_address;
	uint8_t slot_buffer[CHANNEL_ELEMENTS];  /* which buffer each element was given */
	uint32_t pending;
	bool started;
} channels[2] = { { .number = 20, .at = AT_CHANNEL20_RING }, { .number = 21, .at = AT_CHANNEL21_RING } };
static uint32_t received, sent;
static LCK_GRP_DECLARE(mhi_lock_group, "redwood-munch-mhi");
static LCK_MTX_DECLARE(transmit_lock, &mhi_lock_group);

static bool
command_finished(void)
{
	events_look(0);
	return command_finished_flag;
}

static bool
command(uint32_t type, uint32_t channel)
{
	static uint32_t command_write;
	struct context *c = context(AT_COMMAND_CONTEXT, 0);
	struct element *e = (struct element *)(memory + AT_COMMAND_RING) + command_write;

	e->pointer = 0;
	e->word0 = 0;
	e->word1 = channel << 24 | type << 16;
	command_write = (command_write + 1) % 128;
	c->write = memory_address + AT_COMMAND_RING + 16ull * command_write;
	command_finished_flag = false;
	__asm__ volatile ("dsb sy" ::: "memory");
	doorbell(CRDB, c->write);
	return wait_for("the command's end", 2000, command_finished) && command_code == 1;
}

/* One element onto channel i's ring, for the buffer b with `length` bytes (out) or room for them (in), and the doorbell. */
static void
channel_queue(unsigned i, unsigned b, uint32_t length)
{
	struct context *c = context(AT_CHANNEL_CONTEXTS, channels[i].number);
	struct element *e = (struct element *)(memory + channels[i].at) + channels[i].write;

	e->pointer = channels[i].buffers_address + (uint64_t)b * BUFFER_SIZE;
	e->word0 = length;
	e->word1 = TRANSFER_TYPE | TRANSFER_IEOT;
	channels[i].slot_buffer[channels[i].write] = (uint8_t)b;
	channels[i].write = (channels[i].write + 1) % CHANNEL_ELEMENTS;
	channels[i].pending++;
	c->write = memory_address + channels[i].at + 16ull * channels[i].write;
	__asm__ volatile ("dsb sy" ::: "memory");
	doorbell(chdb + 8 * channels[i].number, c->write);
}

static bool
channel_start(unsigned i)
{
	struct context *c = context(AT_CHANNEL_CONTEXTS, channels[i].number);

	channels[i].buffers = dma_alloc(BUFFERS * BUFFER_SIZE, &channels[i].buffers_address);
	if (channels[i].buffers == NULL) {
		return false;
	}
	channels[i].write = channels[i].pending = 0;
	c->base = c->read = c->write = memory_address + channels[i].at;
	c->length = 16ull * CHANNEL_ELEMENTS;
	c->word0 = (c->word0 & ~0xffu) | 1;     /* enabled */
	__asm__ volatile ("dsb sy" ::: "memory");
	if (!command(COMMAND_START_CHANNEL, channels[i].number)) {
		say("mhi: channel %u DID NOT START (code %u, context state %u)\n", channels[i].number, command_code, c->word0 & 0xff);
		return false;
	}
	channels[i].started = true;
	say("mhi: channel %u started (context state %u: %s)\n", channels[i].number, c->word0 & 0xff,
	    (c->word0 & 0xff) == 2 ? "running" : "?");
	if (i == 1) {
		for (unsigned b = 0; b < BUFFERS; b++) {
			channel_queue(1, b, BUFFER_SIZE);
		}
	}
	return true;
}

/* What the chip says is done on a channel: a datagram that has come (21), or one sent (20). From the polling thread. */
static void
transfer_done(unsigned channel, uint64_t element, uint32_t length, uint32_t code)
{
	unsigned i = channel == 20 ? 0 : channel == 21 ? 1 : 2, slot;

	if (i == 2 || element < memory_address + channels[i].at || element >= memory_address + channels[i].at + 16ull *
	    CHANNEL_ELEMENTS) {
		say("mhi: a transfer done on channel %u (element %#llx, code %u, %u bytes): not ours\n", channel, element, code, length);
		return;
	}
	slot = (unsigned)((element - memory_address - channels[i].at) / 16);
	if (i == 0) {
		lck_mtx_lock(&transmit_lock);
		channels[0].pending -= channels[0].pending != 0;
		lck_mtx_unlock(&transmit_lock);
		if (code != 2) {
			say("mhi: a datagram to the chip: code %u\n", code);
		}
		return;
	}
	channels[1].pending -= channels[1].pending != 0;
	if (code == 2 || code == 3) {           /* the end of a transfer, or more than the buffer */
		uint8_t *data = channels[1].buffers + (uint64_t)channels[1].slot_buffer[slot] * BUFFER_SIZE;

		received++;
		hakios_wifi_qrtr_input(data, length);
	} else {
		say("mhi: from the chip on channel 21: code %u, %u bytes\n", code, length);
	}
	channel_queue(1, channels[1].slot_buffer[slot], BUFFER_SIZE);
}

/* QRTR's sender (hakios_wifi_qrtr_over()): a datagram to the chip on channel 20; 0, or an errno. */
int
hakios_munch_qrtr_send(const void *data, uint32_t length)
{
	unsigned b;

	if (length > BUFFER_SIZE) {
		return EMSGSIZE;
	}
	lck_mtx_lock(&transmit_lock);
	if (!channels[0].started || channels[0].pending >= BUFFERS - 1) {
		lck_mtx_unlock(&transmit_lock);
		return channels[0].started ? ENOBUFS : ENXIO;
	}
	b = channels[0].write % BUFFERS;
	memcpy(channels[0].buffers + (uint64_t)b * BUFFER_SIZE, data, length);
	channel_queue(0, b, length);
	sent++;
	lck_mtx_unlock(&transmit_lock);
	return 0;
}

static bool qrtr_on;
static volatile bool polling_wanted, polling;

/* wifi=1: nothing else looks at the rings; QRTR's callbacks run here, and may sleep. A millisecond, as redwood's link. */
static void
polling_thread(__unused void *parameter, __unused wait_result_t result)
{
	polling = true;
	while (polling_wanted) {
		events_look(0);
		events_look(1);
		hakios_wifi_qrtr_input(NULL, 0);
		if (rd(MHISTATUS) & 4) {
			say("mhi: THE CHIP SAYS SYSTEM ERROR (status %#x, environment %s): the rings are no longer looked at\n", rd(MHISTATUS),
			    ee_name(rd(bhi + BHI_EXECENV)));
			registers_dump();
			say("mhi: q6 cookie %#x, warm entry %#x, reset cause %#x\n", far_rd(PCIE_Q6_COOKIE), far_rd(WLAON_WARM_SW_ENTRY),
			    far_rd(WLAON_SOC_RESET_CAUSE));
			rddm_fetch();
			break;
		}
		sleep_ms(1);
	}
	polling = false;
	thread_terminate(current_thread());
}

static void
qrtr_listen(void)
{
	uint32_t wait_s = 5;
	/* a hello of ours to whoever is there (QRTR v1: version, type, from node, port, confirm, size, to node, port; the command) */
	uint32_t hello[9] = { 1, 2, 1, 0xfffffffe, 0, 4, 0xffffffff, 0xfffffffe, 2 };

	PE_parse_boot_argn("pcie_qrtr_wait", &wait_s, sizeof(wait_s));
	hakios_wifi_qrtr_over(hakios_munch_qrtr_send);
	qrtr_on = true;
	say("mhi: QRTR over channels 20 and 21; listening %u s\n", wait_s);
	for (uint32_t ms = 0; ms < wait_s * 1000; ms++) {
		events_look(0);
		events_look(1);
		hakios_wifi_qrtr_input(NULL, 0);
		if (ms == 1000 && received == 0) {
			say("mhi: nothing from the chip in a second: our hello first (%s)\n", hakios_munch_qrtr_send(hello,
			    sizeof(hello)) == 0 ? "sent" : "NOT SENT");
		}
		sleep_ms(1);
	}
	say("mhi: QRTR: %u datagrams from the chip, %u to it\n", received, sent);
}

static bool chip_mastering;

bool
hakios_munch_mhi_run(vm_offset_t registers, vm_offset_t configuration, unsigned level)
{
	uint32_t wait_s = 20, stream = 0x1c01, stream_mask = 0;

	bar = registers;
	config = configuration;
	PE_parse_boot_argn("pcie_mhi_wait", &wait_s, sizeof(wait_s));
	firmware = hakios_image_file("fw", "amss20.bin", &firmware_size, NULL);
	if (firmware == NULL || firmware_size < SBL_SIZE) {
		say("mhi: no fw/amss20.bin in the image (the phone's /vendor/firmware_mnt/image/qca6390/amss20.bin, into fw-wifi/)\n");
		return false;
	}
	say("mhi: the firmware: fw/amss20.bin, %llu KB\n", firmware_size >> 10);
	bhi = rd(BHIOFF);
	chip_reset();
	say("mhi: after the reset: MHI status %#x, ctrl %#x, environment %s, bhi at %#x\n", rd(MHISTATUS), rd(MHICTRL),
	    ee_name(rd(bhi + BHI_EXECENV)), bhi);
	PE_parse_boot_argn("pcie_sid", &stream, sizeof(stream));
	PE_parse_boot_argn("pcie_sid_mask", &stream_mask, sizeof(stream_mask));
	say("mhi: the card's view of memory: SMMU stream %#x, mask %#x (pcie_sid, pcie_sid_mask)\n", stream, stream_mask);
	hakios_wifi_radio_space_want(stream, stream_mask);
	chip_mastering = true;
	if (!msi_on() || !sbl_load()) {
		return false;
	}
	if (level < 7) {
		return true;
	}
	if (!mhi_up() || !amss_load()) {
		return false;
	}
	if (!wait_for("mission mode", wait_s * 1000, in_amss)) {
		say("mhi: the chip is in %s, MHI status %#x\n", ee_name(rd(bhi + BHI_EXECENV)), rd(MHISTATUS));
		return false;
	}
	say("mhi: THE CHIP IS IN MISSION MODE (AMSS); MHI status %#x\n", rd(MHISTATUS));
	sleep_ms(500);
	events_look(0);
	events_look(1);
	if (level < 8) {
		return true;
	}
	say("mhi: step 8: the channels of QRTR\n");
	if (!channel_start(0) || !channel_start(1)) {
		return false;
	}
	if (level >= 9) {
		thread_t thread;

		polling_wanted = true;
		if (kernel_thread_start(polling_thread, NULL, &thread) != KERN_SUCCESS) {
			polling_wanted = false;
			return false;
		}
		thread_deallocate(thread);
		return true;
	}
	qrtr_listen();
	return true;
}

void
hakios_munch_mhi_stop(void)
{
	if (!chip_mastering) {
		return;
	}
	polling_wanted = false;
	while (polling) {
		sleep_ms(1);
	}
	if (qrtr_on) {
		hakios_wifi_qrtr_over(NULL);
		qrtr_on = false;
	}
	channels[0].started = channels[1].started = false;
	if (msi_at != 0) {
		*(volatile uint32_t *)(config + msi_at) &= ~(1u << 16);
		msi_at = 0;
	}
	if (chdb != 0) {
		doorbell(chdb + 8 * DEV_WAKE_DOORBELL, 0);
	}
	wr(MHICTRL, 1u << 1);
	sleep_ms(10);
	say("mhi: MHI reset (status %#x, environment %s)\n", rd(MHISTATUS), bhi ? ee_name(rd(bhi + BHI_EXECENV)) : "?");
	chip_mastering = false;
}

/* On the way back to the loader: nobody looks at the rings any more and nothing may sleep; the chip stops asking for memory. */
void
hakios_munch_mhi_quiesce(void)
{
	polling_wanted = false;
	if (chip_mastering && msi_at != 0) {
		*(volatile uint32_t *)(config + msi_at) &= ~(1u << 16);
	}
	chip_mastering = false;
}
