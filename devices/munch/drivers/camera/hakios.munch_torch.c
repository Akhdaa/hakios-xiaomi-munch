/*
 * The POCO F4's (munch's) flashlight: the PM8150L's flash LED module (SPMI slave 5, peripheral 0xd3: Android's
 * pm8150l.dtsi flash_led, "qcom,qpnp-flash-led-v2"), its torch channel 0 (pm8150l_torch0, what munch-sm8250-camera-sensor-
 * mtp.dtsi gives every camera as its torch-source). Registers and the switching on as Android's leds-qpnp-flash-v2.c has
 * them (qpnp_flash_led_switch_set, read as data):
 *
 *   IRES (0x47)         [1:0] per channel: 0 = 12.5 mA steps ("qcom,ires-ua" 12500)
 *   STROBE_CTRL0 (0x49) bit 2 clear: software strobe
 *   TGR_CURRENT0 (0x43) [6:0] = mA / 12.5 - 1 (Android's torch: 300 mA, its most 500)
 *   SAFETY_TMR0 (0x40)  0x13: no safety timer, as Android's torch has it
 *   MOD_CTRL (0x46)     bit 7: the module on
 *   EN_LED_CTRL (0x4c)  bit 0: channel 0 on
 *
 *   munch_torch=1   a look: the module's type, subtype and status, its channel at the arbiter and whether it is ours
 *   munch_torch=2   ... and, 45 s after the start, the torch on for six seconds at munch_torch_ma (150 mA if not given, never over 300: Android's
 *                   own), then off; then sysctl kern.hakios.torch (0 or 1) switches it, for the system's flashlight
 *   munch_torch=3   no light at the start; the light is Darwin's: /dev/hakios-torch, the POCO X5 Pro's driver's device
 *                   (drivers/camera/hakios.torch.c), which mediaserverd's camera device (userland/src/camera_device.c)
 *                   writes the back camera's TorchLevel to, in thousandths -- what Control Center's flashlight sets. 1000
 *                   is munch_torch_ma; less is that share of it, never under 25 mA. Off by itself after munch_torch_secs
 *                   (120) at a time, and when the run ends in order (hakios_munch_torch_stop()).
 *
 * Only the six registers above are ever written, each read back; a write that does not read back, or a status bit that
 * says the LED is open, shorted or too hot, and it is switched off and left alone for the rest of the run.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.version.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <kern/thread_call.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);

SYSCTL_DECL(_kern_hakios);

static LCK_GRP_DECLARE(torch_lock_group, "munch-torch");
static LCK_MTX_DECLARE(torch_lock, &torch_lock_group);

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
	__asm__ volatile ("dsb sy");
}

/* ---- the PMIC arbiter (version 5), one transfer at a time with the other drivers ---------------------------- */

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

/* the flash LED module (pm8150l.dtsi: qcom,pm8150l@5, qcom,leds@d300) */
#define FLASH_ID                0x5d3
#define FLASH_TYPE              0x04
#define FLASH_SUBTYPE           0x05
#define FLASH_STATUS1           0x08
#define FLASH_STATUS2           0x09
#define FLASH_INT_RT_STS        0x10
#define FLASH_SAFETY_TMR0       0x40
#define FLASH_TGR_CURRENT0      0x43
#define FLASH_MOD_CTRL          0x46
#define FLASH_IRES              0x47
#define FLASH_STROBE_CTRL0      0x49
#define FLASH_EN_LED_CTRL       0x4c
#define SAFETY_TMR_DISABLED     0x13
#define MOD_ENABLE              0x80
#define TORCH_MA_MOST           300             /* Android's torch current (pm8150l_torch0 qcom,current-ma) */
#define IRES_UA                 12500

static vm_offset_t arb_core, arb_cnfg, arb_observer, flash_write;
static int flash_channel = -1;
static bool flash_ours, broken, module_switched;
static uint32_t level, torch_ma = 150;
static uint32_t torch_share = 1000;     /* of torch_ma, in thousandths: Darwin's level (munch_torch=3) */
static int torch_on;

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
flash_get(uint8_t reg, uint8_t *value)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * (uint32_t)flash_channel;
	const char *problem;

	hakios_spmi_lock();
	wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4);
	problem = transfer_wait(channel);
	*value = (uint8_t)rd(channel + OBSERVER_DATA0);
	hakios_spmi_unlock();
	return problem;
}

/* one of the six registers, written and read back; false (and no more writing) if it cannot be */
static bool
flash_put(uint8_t reg, uint8_t value)
{
	static const uint8_t allowed[] = { FLASH_SAFETY_TMR0, FLASH_TGR_CURRENT0, FLASH_MOD_CTRL, FLASH_IRES, FLASH_STROBE_CTRL0,
		                           FLASH_EN_LED_CTRL };
	const char *problem = NULL;
	uint8_t back = 0xff;
	bool ok = false;

	for (unsigned i = 0; i < sizeof(allowed); i++) {
		ok |= allowed[i] == reg;
	}
	if (!ok || !flash_ours || flash_write == 0 || ((rd(arb_core + ARB_CHANNEL(flash_channel)) >> 8) & 0xfff) != FLASH_ID) {
		say("munch-torch: %#x <- %#x is not to be written\n", reg, value);
		broken = true;
		return false;
	}
	hakios_spmi_lock();
	wr(flash_write + CHANNEL_WDATA0, value);
	wr(flash_write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
	problem = transfer_wait(flash_write);
	hakios_spmi_unlock();
	if (problem == NULL) {
		problem = flash_get(reg, &back);
	}
	if (problem != NULL || back != value) {
		say("munch-torch: %#x <- %#x: %s (reads %#x): no more writing\n", reg, value, problem ? problem : "not read back", back);
		broken = true;
		return false;
	}
	return true;
}

static bool
look(void)
{
	uint8_t type = 0, subtype = 0, s1 = 0, s2 = 0, rt = 0, mod = 0, en = 0;
	const char *problem;

	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	for (int n = 0; n < ARB_MAX_CHANNELS; n++) {
		if (((rd(arb_core + ARB_CHANNEL(n)) >> 8) & 0xfff) == FLASH_ID) {
			flash_channel = n;
			flash_ours = (rd(arb_cnfg + ARB_OWNER(n)) & 7) == ARB_SYSTEM;
			break;
		}
	}
	if (flash_channel < 0) {
		say("munch-torch: the PM8150L's flash LED module (%#x) has no channel at the arbiter\n", FLASH_ID);
		return false;
	}
	if ((problem = flash_get(FLASH_TYPE, &type)) != NULL || (problem = flash_get(FLASH_SUBTYPE, &subtype)) != NULL ||
	    (problem = flash_get(FLASH_STATUS1, &s1)) != NULL || (problem = flash_get(FLASH_STATUS2, &s2)) != NULL ||
	    (problem = flash_get(FLASH_INT_RT_STS, &rt)) != NULL || (problem = flash_get(FLASH_MOD_CTRL, &mod)) != NULL ||
	    (problem = flash_get(FLASH_EN_LED_CTRL, &en)) != NULL) {
		say("munch-torch: reading the flash LED module: %s\n", problem);
		return false;
	}
	say("munch-torch: the PM8150L's flash LED module: channel %d (%s), type %#x, subtype %#x, status %#x %#x, interrupts "
	    "%#x, module %#x, channels on %#x\n", flash_channel, flash_ours ? "ours to write" : "NOT ours to write", type, subtype,
	    s1, s2, rt, mod, en);
	if (flash_ours) {
		flash_write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * (uint32_t)flash_channel, 0x1000);
	}
	return true;
}

/* The torch on (at torch_ma) or off; under torch_lock. */
static bool
torch_locked(bool on)
{
	uint8_t mod = 0, s1 = 0, s2 = 0;
	uint32_t ma = torch_ma * torch_share / 1000;

	ma = ma < 25 ? 25 : ma > TORCH_MA_MOST ? TORCH_MA_MOST : ma;
	uint8_t code = (uint8_t)((ma * 1000 + IRES_UA / 2) / IRES_UA - 1);

	if (!on) {
		(void)flash_put(FLASH_EN_LED_CTRL, 0);
		if (module_switched) {
			(void)flash_put(FLASH_MOD_CTRL, 0);
			module_switched = false;
		}
		torch_on = 0;
		return true;
	}
	if (broken || !flash_ours) {
		return false;
	}
	if (flash_get(FLASH_MOD_CTRL, &mod) != NULL) {
		return false;
	}
	if (!flash_put(FLASH_IRES, 0) || !flash_put(FLASH_STROBE_CTRL0, 0) || !flash_put(FLASH_TGR_CURRENT0, code) ||
	    !flash_put(FLASH_SAFETY_TMR0, SAFETY_TMR_DISABLED)) {
		return false;
	}
	if (!(mod & MOD_ENABLE)) {
		if (!flash_put(FLASH_MOD_CTRL, MOD_ENABLE)) {
			return false;
		}
		module_switched = true;
	}
	if (!flash_put(FLASH_EN_LED_CTRL, 1)) {
		torch_locked(false);
		return false;
	}
	torch_on = 1;
	delay_for_interval(20, NSEC_PER_MSEC);
	(void)flash_get(FLASH_STATUS1, &s1);
	(void)flash_get(FLASH_STATUS2, &s2);
	say("munch-torch: THE TORCH IS ON at %u mA (code %#x); status %#x %#x\n", ma, code, s1, s2);
	return true;
}

static int
torch_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int value = torch_on, error = sysctl_handle_int(oidp, &value, 0, req);

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (level < 2 || flash_channel < 0) {
		return ENXIO;
	}
	lck_mtx_lock(&torch_lock);
	error = torch_locked(value != 0) ? 0 : EIO;
	lck_mtx_unlock(&torch_lock);
	return error;
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, torch, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, NULL, 0, torch_sysctl,
    "I", "the POCO F4's torch: 1 on, 0 off (munch_torch=2)");

/* ---- munch_torch=3: the light for Darwin (/dev/hakios-torch, as drivers/camera/hakios.torch.c has it) ---- */

static uint32_t shown, torch_secs = 120;
static thread_call_t limit_call;

static void
limit_reached(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	lck_mtx_lock(&torch_lock);
	if (torch_on) {
		torch_locked(false);
		shown = 0;
		say("munch-torch: lit for %u s: off (munch_torch_secs)\n", torch_secs);
	}
	lck_mtx_unlock(&torch_lock);
}

static int
torchdev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value = 0;
	int error;
	bool ok;

	while (uio_resid(uio) >= (user_ssize_t)sizeof(value)) {
		if ((error = uiomove((caddr_t)&value, sizeof(value), uio)) != 0) {
			return error;
		}
	}
	value = value > 1000 ? 1000 : value;
	lck_mtx_lock(&torch_lock);
	if (value == 0) {
		ok = torch_locked(false);
	} else {
		torch_share = value;
		ok = torch_locked(true);
	}
	shown = ok ? value : 0;
	lck_mtx_unlock(&torch_lock);
	(void)thread_call_cancel(limit_call);
	if (ok && value != 0) {
		uint64_t deadline;

		clock_interval_to_deadline(torch_secs, NSEC_PER_SEC, &deadline);
		thread_call_enter_delayed(limit_call, deadline);
	}
	say("munch-torch: Darwin asks for %u of 1000: %s\n", value, !ok ? "NOT DONE" : value ? "lit" : "dark");
	return ok ? 0 : EIO;
}

static int
torchdev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value = shown;

	return uiomove((caddr_t)&value, (int)MIN(sizeof(value), (size_t)uio_resid(uio)), uio);
}

static int
torchdev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw torchdev_cdevsw = {
	.d_open = torchdev_open, .d_close = torchdev_open, .d_read = torchdev_read, .d_write = torchdev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* From osfmk/arm64/loader_return.c, in a thread, before a run's ordinary end: no light left on behind us. */
void
hakios_munch_torch_stop(void)
{
	if (level < 2 || flash_channel < 0) {
		return;
	}
	lck_mtx_lock(&torch_lock);
	if (torch_on) {
		torch_locked(false);
		shown = 0;
	}
	lck_mtx_unlock(&torch_lock);
}

static void
torch_thread(__unused void *parameter, __unused wait_result_t result)
{
	if (level >= 3) {
		int major;

		/* (the device before mediaserverd looks for it; the module itself is looked at once, 5 s in) */
		delay_for_interval(5, NSEC_PER_SEC);
		say("munch-torch: a look at the flash LED module (munch_torch=%u)\n", level);
		if (!look()) {
			thread_terminate(current_thread());
		}
		limit_call = thread_call_allocate(limit_reached, NULL);
		major = cdevsw_add(-1, &torchdev_cdevsw);
		if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-torch") == NULL) {
			say("munch-torch: /dev/hakios-torch cannot be made\n");
		} else {
			say("munch-torch: THE LIGHT IS DARWIN'S (/dev/hakios-torch): at most %u mA, at most %u s at a time\n",
			    torch_ma > TORCH_MA_MOST ? TORCH_MA_MOST : torch_ma, torch_secs);
		}
		thread_terminate(current_thread());
	}
	/* (45 s: past the Apple logo, so that the light is seen; it was missed at 20 s) */
	delay_for_interval(45, NSEC_PER_SEC);
	say("munch-torch: a look at the flash LED module (munch_torch=%u)\n", level);
	if (look() && level >= 2) {
		lck_mtx_lock(&torch_lock);
		if (torch_locked(true)) {
			lck_mtx_unlock(&torch_lock);
			delay_for_interval(6, NSEC_PER_SEC);
			lck_mtx_lock(&torch_lock);
			torch_locked(false);
			say("munch-torch: the torch off again; kern.hakios.torch switches it from here on\n");
		}
		lck_mtx_unlock(&torch_lock);
	}
	thread_terminate(current_thread());
}

__startup_func
static void
torch_init(void)
{
	thread_t thread;

	if (!hakios_soc_is("sm8250") || !PE_parse_boot_argn("munch_torch", &level, sizeof(level)) || level == 0) {
		return;
	}
	PE_parse_boot_argn("munch_torch_ma", &torch_ma, sizeof(torch_ma));
	PE_parse_boot_argn("munch_torch_secs", &torch_secs, sizeof(torch_secs));
	torch_secs = torch_secs < 1 ? 1 : torch_secs > 600 ? 600 : torch_secs;
	if (kernel_thread_start(torch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, torch_init);
