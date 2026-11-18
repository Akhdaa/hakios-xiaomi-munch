/*
 * The brightness of the POCO F4's (munch's) panel. The panel keeps its own level: a DCS command, "set display brightness"
 * (0x51) with two bytes, high byte first, 1 to 2047 (Android's dt, dsi-panel-l11r-38-08-0a-dsc-cmd.dtsi:
 * mi,mdss-dsi-bl-dcs-big-endian-type; its HBM commands write 51 07 FF for the top of the normal range). The boot chain has
 * left the display running, DSI0 in command mode at 0xae94000, and nobody here drives it. So one packet goes through the
 * DSI host's own command FIFO, as Android's dsi_ctrl_hw_cmn_kickoff_fifo_command() has it (techpack/display/msm/dsi/
 * dsi_ctrl_hw_cmn.c, qcom,mdss-dsi-dma-trigger = "trigger_sw"): no memory, no SMMU.
 *
 * When: between two frames. The display controller sends each frame to the panel's memory in a burst of some 8.6 ms of
 * the 16.5 of a refresh, and its interface's line count runs only during it (MiniIOMobileFramebuffer.cpp, fb_beam). A packet
 * triggered during a burst stayed behind it for good, the host stuck (status 0x7: the command engine, its DMA and the MDP's
 * stream all busy; the old tree's attempt, 2 November). Here the packet is triggered the moment a burst is seen to end,
 * with eight milliseconds before the next one; the packet itself takes microseconds.
 *
 *   panel=1    a look: the bus clocks and the DSI host's registers, nothing written
 *   panel=2    ... and brightness: panel_level= (700 of 2047 if not given) once the system is up, 40 s after the start,
 *              and then sysctl kern.hakios.panel_brightness (1 to 2047)
 *   panel=3    ... and first 300, 1500, 600, 2047, four seconds each, to be seen from outside, then panel_level
 *   panel_hs=1 the packet in high speed (by default in low power, as the boot chain sends its own)
 *
 * Nothing is sent outside 1..2047, and nothing is written to the panel but this one command. A packet that does not finish
 * in 20 ms stops all of it for the rest of the run.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.version.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

SYSCTL_DECL(_kern_hakios);

static LCK_GRP_DECLARE(panel_lock_group, "munch-panel");
static LCK_MTX_DECLARE(panel_lock, &panel_lock_group);

static int say(const char *format, ...) __printflike(1, 2);

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
	return printf("%s", line);
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
}

/* the bus clocks without which the display's registers hang the bus (Android's gcc-kona.c, dispcc-kona.c) */
#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x10000UL
#define GCC_DISP_AHB_CBCR       0xb00c
#define DISPCC_PHYS             0x0af00000UL
#define DISPCC_SIZE             0x4000UL
#define DISPCC_MDSS_AHB_CBCR    0x2080
#define DISPCC_MDSS_GDSCR       0x3000
#define CBCR_OFF                (1u << 31)
#define GDSC_POWER_ON           (1u << 31)

/* DSI0's host (dsi_ctrl_hw_cmn.c, dsi_ctrl_reg.h) */
#define DSI_PHYS                0x0ae94000UL
#define DSI_SIZE                0x400UL
#define DSI_CTRL                0x004
#define DSI_STATUS              0x008
#define DSI_FIFO_STATUS         0x00c
#define DSI_DMA_CTRL            0x03c
#define DSI_MDP_CTRL            0x040
#define DSI_DMA_CMD_LENGTH      0x04c
#define DSI_TRIG_CTRL           0x084
#define DSI_DMA_SW_TRIGGER      0x090
#define DSI_INT_CTRL            0x110
#define DSI_TPG_CTRL            0x15c
#define DSI_TPG_DMA_INIT        0x17c
#define DSI_TPG_FIFO_RESET      0x1ec
#define DSI_VERSION             0x1f4
#define DMA_CTRL_BROADCAST      (1u << 31)
#define DMA_CTRL_MASTER         (1u << 30)
#define DMA_CTRL_EMBEDDED       (1u << 28)
#define DMA_CTRL_LOW_POWER      (1u << 26)
#define STATUS_BUSY             7u              /* the command engine, its DMA, the MDP's stream */
#define INT_DMA_DONE            1u
#define INT_ENABLES             0xaaaaaaaau     /* the odd bits enable, the even ones are statuses, cleared by a 1 */
#define TPG_FIFO_MODE           (1u << 1 | 1u << 2 | 3u << 16)

/* the interface the panel is on, its line count: running during a burst, resting at 1 between them (read only) */
#define INTF1_PHYS              0x0ae6b800UL
#define INTF_LINE_COUNT         0x0b0

#define BRIGHTNESS_MAX          2047

static vm_offset_t dsi, intf;
static uint32_t found_dma_ctrl, found_dma_length, found_tpg;     /* as the boot chain left them: put back after each packet */
static uint32_t hs;                                             /* boot-arg panel_hs=1: the packet in high speed */
static bool usable;
static uint32_t level;
static uint32_t start_level = 700;      /* boot-arg panel_level: what the panel is set to once the system is up */
static int brightness = -1;
static uint32_t requested_brightness;
static thread_call_t brightness_call;
static bool brightness_unavailable;
static bool asleep;
static uint32_t sent, waits_long;

static bool
clock_on(const char *name, vm_offset_t cbcr)
{
	uint32_t was = rd(cbcr);

	say("munch-panel: %s: %#x\n", name, was);
	if (was & CBCR_OFF) {
		say("munch-panel: %s is off: nothing is touched\n", name);
		return false;
	}
	return true;
}

static bool
look(void)
{
	vm_offset_t gcc = ml_io_map(GCC_PHYS, GCC_SIZE), dispcc, page;
	uint32_t power;

	if (!clock_on("the display's clock controller's bus clock", gcc + GCC_DISP_AHB_CBCR)) {
		return false;
	}
	dispcc = ml_io_map(DISPCC_PHYS, DISPCC_SIZE);
	power = rd(dispcc + DISPCC_MDSS_GDSCR);
	say("munch-panel: the display block's power switch: %#x\n", power);
	if (!(power & GDSC_POWER_ON)) {
		say("munch-panel: the display block has no power: nothing more is read\n");
		return false;
	}
	if (!clock_on("the display block's bus clock", dispcc + DISPCC_MDSS_AHB_CBCR)) {
		return false;
	}
	dsi = ml_io_map(DSI_PHYS, DSI_SIZE);
	found_dma_ctrl = rd(dsi + DSI_DMA_CTRL);
	found_dma_length = rd(dsi + DSI_DMA_CMD_LENGTH);
	found_tpg = rd(dsi + DSI_TPG_CTRL);
	page = ml_io_map(INTF1_PHYS & ~0x3fffUL, 0x4000);
	intf = page + (INTF1_PHYS & 0x3fffUL);
	say("munch-panel: DSI host version %#x, ctrl %#x, status %#x, fifo status %#x, dma ctrl %#x, mdp ctrl %#x, trigger ctrl "
	    "%#x, interrupts %#x, tpg %#x; the interface's line %u\n", rd(dsi + DSI_VERSION), rd(dsi + DSI_CTRL), rd(dsi + DSI_STATUS),
	    rd(dsi + DSI_FIFO_STATUS), rd(dsi + DSI_DMA_CTRL), rd(dsi + DSI_MDP_CTRL), rd(dsi + DSI_TRIG_CTRL), rd(dsi + DSI_INT_CTRL),
	    rd(dsi + DSI_TPG_CTRL), rd(intf + INTF_LINE_COUNT));
	return true;
}

/*
 * Until a burst has just ended: the line count seen running, then at rest (1 or 0), and the host idle. Up to 50 ms (three
 * refreshes); false if no burst was seen to end in that time. Interrupts are off for the last stretch only (a burst).
 */
static bool
burst_ended(void)
{
	uint64_t until, now;
	boolean_t interrupts;
	bool running = false;

	nanoseconds_to_absolutetime(50 * NSEC_PER_MSEC, &until);
	until += mach_absolute_time();
	/* a burst under way, with interrupts on */
	while (rd(intf + INTF_LINE_COUNT) <= 1) {
		if (mach_absolute_time() >= until) {
			return false;
		}
	}
	interrupts = ml_set_interrupts_enabled(FALSE);
	while ((now = mach_absolute_time()) < until) {
		uint32_t line = rd(intf + INTF_LINE_COUNT);

		if (line > 1) {
			running = true;
		} else if (running && (rd(dsi + DSI_STATUS) & STATUS_BUSY) == 0) {
			return true;                    /* (interrupts stay off: the caller triggers at once and turns them on) */
		}
	}
	(void)ml_set_interrupts_enabled(interrupts);
	return false;
}

/*
 * The DMA's done, cleared, and nothing else: the statuses are cleared by writing them 1, and the boot chain's own
 * display driver, which goes on sending frames from its core, waits for its frames' done (bits 8 and 10, set when found:
 * 0x500). Writing back what was read cleared those as well, and it waited for good: one packet went through, and the
 * frames' stream stopped behind it (6 and 7 November, the second packet always stuck at status 0x7).
 */
static void
dma_done_clear(void)
{
	wr(dsi + DSI_INT_CTRL, (rd(dsi + DSI_INT_CTRL) & INT_ENABLES) | INT_DMA_DONE);
}

/* The registers a packet sets, as they were found */
static void
put_back(void)
{
	wr(dsi + DSI_DMA_CTRL, found_dma_ctrl);
	wr(dsi + DSI_DMA_CMD_LENGTH, found_dma_length);
	wr(dsi + DSI_TPG_CTRL, found_tpg);
	__builtin_arm_dsb(15);
}

/*
 * One packet. The level: DCS long write (0x39), three bytes: 0x51, the level's high byte, its low one. A command of no
 * parameter (display off 0x28, sleep in 0x10, sleep out 0x11, display on 0x29): DCS short write (0x05), the command in the
 * header's first data byte and nothing after the header (Linux's dsi_cmd_dma_add() for a short packet).
 */
static int send_packet_locked(uint32_t header, uint32_t payload, uint32_t length, const char *what);

static int
send_locked(uint32_t value)
{
	return send_packet_locked(0xc0390003, 0xff000051 | (value >> 8 & 0xff) << 8 | (value & 0xff) << 16, 8, "a level");
}

static bool
send_short_locked(uint8_t command)
{
	int error = send_packet_locked(0x80050000 | command, 0, 4, "a command");

	return error == 0 || error == EAGAIN;
}

static int
send_packet_locked(uint32_t header, uint32_t payload, uint32_t length, const char *what)
{
	uint32_t dma_ctrl, waited;

	dma_done_clear();         /* (a done of before, cleared) */
	wr(dsi + DSI_TPG_CTRL, TPG_FIFO_MODE);
	/*
	 * The header as the host wants it in its FIFO (Linux's msm dsi_host.c, dsi_cmd_dma_add()): the word count, the data
	 * type, then bit 7 for the last packet and bit 6 for a long one. Without bit 6 (0x80390003, the old tree's and this
	 * driver's first runs) 0x39 went out as a short packet, the panel waited for its three bytes, and the frames' stream
	 * stuck behind the first packet.
	 */
	wr(dsi + DSI_TPG_DMA_INIT, header);             /* the level's: 0xc0390003, 3 bytes, 0x39, long, the last packet */
	if (length > 4) {
		wr(dsi + DSI_TPG_DMA_INIT, payload);
	}
	/*
	 * As the boot chain sends its own commands (embedded, in low power: 0x14000000 found): the first run of this, in high
	 * speed and with the master bit, got one packet through and left the frames' stream stuck behind it (6 November:
	 * the second packet found the line count at rest and the host busy, status 0x7).
	 */
	dma_ctrl = found_dma_ctrl | DMA_CTRL_EMBEDDED;
	if (hs) {
		dma_ctrl &= ~DMA_CTRL_LOW_POWER;
	} else {
		dma_ctrl |= DMA_CTRL_LOW_POWER;
	}
	wr(dsi + DSI_DMA_CTRL, dma_ctrl);
	wr(dsi + DSI_DMA_CMD_LENGTH, length);
	__builtin_arm_dsb(15);
	if (!burst_ended()) {
		waits_long++;
		say("munch-panel: no end of a frame's burst seen in 50 ms (line %u, status %#x): %s (%#x) not sent\n",
		    rd(intf + INTF_LINE_COUNT), rd(dsi + DSI_STATUS), what, header);
		wr(dsi + DSI_TPG_FIFO_RESET, 1);
		wr(dsi + DSI_TPG_FIFO_RESET, 0);
		put_back();
		return EAGAIN;
	}
	wr(dsi + DSI_DMA_SW_TRIGGER, 1);
	(void)ml_set_interrupts_enabled(TRUE);
	wr(dsi + DSI_TPG_FIFO_RESET, 1);
	wr(dsi + DSI_TPG_FIFO_RESET, 0);
	for (waited = 0; !(rd(dsi + DSI_INT_CTRL) & INT_DMA_DONE) && waited < 200; waited++) {
		delay_for_interval(100, NSEC_PER_USEC);
	}
	dma_done_clear();
	if (waited < 200) {
		put_back();
	}
	sent++;
	return waited < 200 ? 0 : EIO;
}

static int
apply_brightness_locked(uint32_t value)
{
	int error;

	if (!usable) {
		return ENXIO;
	}
	if (asleep) {
		return EAGAIN;
	}
	error = send_locked(value);
	if (error == 0) {
		brightness = (int)value;
	} else if (error == EIO) {
		usable = false;
		__atomic_store_n(&brightness_unavailable, true, __ATOMIC_RELAXED);
		say("munch-panel: the packet for %u did not finish in 20 ms (status %#x, interrupts %#x, line %u): no more of them\n",
		    value, rd(dsi + DSI_STATUS), rd(dsi + DSI_INT_CTRL), rd(intf + INTF_LINE_COUNT));
	}
	return error;
}

int
hakios_munch_panel_brightness(uint32_t value)
{
	int error;

	if (value < 1 || value > BRIGHTNESS_MAX) {
		return EINVAL;
	}
	lck_mtx_lock(&panel_lock);
	error = apply_brightness_locked(value);
	lck_mtx_unlock(&panel_lock);
	return error;
}

/* The latest slider value wins. Keep the DSI wait off the caller's thread. */
bool
hakios_munch_panel_queue_brightness(uint32_t value)
{
	const uint32_t span = BRIGHTNESS_MAX - 1;
	uint32_t position;

	if (brightness_call == NULL || __atomic_load_n(&brightness_unavailable, __ATOMIC_RELAXED) ||
	    value < 1 || value > BRIGHTNESS_MAX) {
		return false;
	}
	/* Spread the dim range across more of the slider: halfway gives 40% of the panel range. */
	position = value - 1;
	value = 1 + (position * (6 * span + 4 * position) + 5 * span) / (10 * span);
	__atomic_store_n(&requested_brightness, value, __ATOMIC_RELAXED);
	thread_call_enter(brightness_call);
	return true;
}

static void
brightness_worker(__unused thread_call_param_t a, __unused thread_call_param_t b)
{
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		uint32_t value;
		int error;

		lck_mtx_lock(&panel_lock);
		value = __atomic_load_n(&requested_brightness, __ATOMIC_RELAXED);
		if (!usable || asleep || value == 0 || (int)value == brightness) {
			lck_mtx_unlock(&panel_lock);
			return;
		}
		error = apply_brightness_locked(value);
		lck_mtx_unlock(&panel_lock);
		if (error == 0) {
			say("munch-panel: slider level %u applied\n", value);
		}
		if (error != EAGAIN) {
			return;
		}
	}
}

/*
 * The panel asleep while backboardd has the screen blanked, awake when it unblanks (MiniIOMobileFramebuffer's power
 * change, boot-arg fb_sleep): display off, then sleep in; sleep out, 120 ms (the MIPI DCS wait before display on), display
 * on, and the level it had. The frames go on reaching the panel either way (the boot chain's display driver sends them);
 * asleep it neither shows nor lights them. 0, or an error.
 */
extern void hakios_cpufreq_screen(bool off) __attribute__((weak));     /* drivers/power/hakios.cpufreq.c */

int
hakios_munch_panel_power(bool on)
{
	int error = 0;

	lck_mtx_lock(&panel_lock);
	if (!usable) {
		error = ENXIO;
	} else if (on == !asleep) {
		error = 0;                              /* (as it is already) */
	} else if (!on) {
		if (send_short_locked(0x28) && send_short_locked(0x10)) {
			asleep = true;
		} else {
			error = EIO;
		}
	} else {
		if (send_short_locked(0x11)) {
			lck_mtx_unlock(&panel_lock);
			delay_for_interval(120, NSEC_PER_MSEC);
			lck_mtx_lock(&panel_lock);
		}
		if (usable && send_short_locked(0x29)) {
			asleep = false;
			if (brightness >= 1) {
				(void)send_locked((uint32_t)brightness);
			}
		} else {
			error = EIO;
		}
	}
	lck_mtx_unlock(&panel_lock);
	if (error == 0 && on && brightness_call != NULL) {
		thread_call_enter(brightness_call);
	}
	if (error == 0 && hakios_cpufreq_screen != NULL) {
		hakios_cpufreq_screen(!on);
	}
	say("munch-panel: %s: %s (%u packets sent, %u without a moment)\n", on ? "awake" : "asleep", error == 0 ? "done" :
	    error == ENXIO ? "not yet (panel=2 takes the panel 40 s after the start)" : "FAILED", sent, waits_long);
	return error;
}

static int
brightness_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int value, error;

	lck_mtx_lock(&panel_lock);
	value = brightness;
	lck_mtx_unlock(&panel_lock);
	error = sysctl_handle_int(oidp, &value, 0, req);

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	return value < 0 ? EINVAL : hakios_munch_panel_brightness((uint32_t)value);
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, panel_brightness, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, NULL, 0,
    brightness_sysctl, "I", "the POCO F4 panel's level, 1 to 2047; -1: not set yet (panel=2)");

static void
panel_thread(__unused void *parameter, __unused wait_result_t result)
{
	static const uint32_t steps[] = { 300, 1500, 600, 2047 };

	delay_for_interval(40, NSEC_PER_SEC);
	if (look() && level >= 2) {
		lck_mtx_lock(&panel_lock);
		usable = true;
		lck_mtx_unlock(&panel_lock);
		for (unsigned i = 0; level >= 3 && i < sizeof(steps) / sizeof(steps[0]); i++) {
			int error = hakios_munch_panel_brightness(steps[i]);

			say("munch-panel: brightness %u: %s (status %#x, interrupts %#x; %u sent, %u without a moment)\n", steps[i],
			    error == 0 ? "done" : "FAILED", rd(dsi + DSI_STATUS), rd(dsi + DSI_INT_CTRL), sent, waits_long);
			if (error != 0) {
				break;
			}
			delay_for_interval(4, NSEC_PER_SEC);
		}
		/*
		 * The boot chain leaves the panel at a level of its own, and 2047 is glaring (7 November): a level that reads well
		 * indoors, until something sets another.
		 */
		if (start_level >= 1 && start_level <= BRIGHTNESS_MAX) {
			int error = hakios_munch_panel_brightness(start_level);

			say("munch-panel: the panel at %u of %u (panel_level): %s\n", start_level, BRIGHTNESS_MAX, error == 0 ? "done" : "FAILED");
		}
		if (brightness_call != NULL) {
			thread_call_enter(brightness_call);
		}
	} else {
		__atomic_store_n(&brightness_unavailable, true, __ATOMIC_RELAXED);
	}
	thread_terminate(current_thread());
}

__startup_func
static void
panel_init(void)
{
	thread_t thread;

	if (!hakios_soc_is("sm8250") || !PE_parse_boot_argn("panel", &level, sizeof(level)) || level == 0) {
		return;
	}
	PE_parse_boot_argn("panel_hs", &hs, sizeof(hs));
	PE_parse_boot_argn("panel_level", &start_level, sizeof(start_level));
	if (level >= 2) {
		brightness_call = thread_call_allocate_with_options(brightness_worker, NULL, THREAD_CALL_PRIORITY_KERNEL,
		    THREAD_CALL_OPTIONS_ONCE);
	}
	if (kernel_thread_start(panel_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, panel_init);
