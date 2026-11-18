/*
 * The speakers of the POCO F4 (munch): its two NXP TFA9874 amplifiers, driven as Android's driver
 * (techpack/audio/asoc/codecs/tfa98xx: tfa_dsp.c, tfa_init.c, tfa_container.c) drives them, for
 * hakios_sound.c on munch (sound=3 and 4; hakios_speaker.c is redwood's).
 *
 * The amplifiers sit on the I2C bus of the first QUP's serial engine 3 (GPIO 119 data, 120 clock),
 * at 0x34 (the top one, "RCV" in Android's configuration) and 0x35 (the bottom one, "SPK"), and
 * take their samples from the third MI2S interface, 16-bit stereo at 48 kHz: 0x34 the left slot,
 * 0x35 the right. They are "non-DSP" chips: what Android has for their protection -- the speaker
 * model, the excursion filter, the volume -- runs in the audio DSP, not here. In its place the
 * samples go through hakios_guard.c, and the volume is put on the samples on their way to the DSP
 * (hakios_munch_amp_scale()): each chip plays at the gain Android's configuration gives it.
 *
 * What is written, in the order Android writes it (tfaRunStartup, tfa_dev_start, tfa_dev_stop):
 *   - the chip's registers to their power-on values (I2CR), then the registers NXP's code has for
 *     this revision (0x0c74, tfa9874_specific(), behind the keys that unhide them)
 *   - the configuration: /fw/tfa98xx.cnt, the phone's /vendor/firmware/tfa98xx.cnt: every bitfield
 *     of the chip's device list, then of its "stereo" profile (its patch is an empty one, and the
 *     DSP messages of its "calibrate" profile are for the audio DSP)
 *   - the interface's slots to 32 bits (TDMNBCK 2, TDMSLLN and TDMSSIZE 31), as they read while Android plays
 *     for a 16-bit stream: the configuration has them at 32
 *   - to start: out of powerdown (MANSCONF, PWDN 0), the clocks waited for, the amplifier on (AMPE),
 *     and the device manager waited for in its operating state (9)
 *   - to stop: the amplifier off and powerdown (tfa98xx_powerdown), the manager waited for below 4
 * The chips' status (registers 0x10 and 0x13: over-temperature, under- and over-voltage and over-
 * current, the alarm state) is looked at while they play; on a fault both are powered down at once.
 *
 * I2C is done by hand, as hakios_munch_speaker.c does it: a pin is either pulled low or let go,
 * at about 50 kHz, and the two pins are taken from the serial engine as GPIOs only for the
 * transfers. Registers are 16 bits, the most significant byte first.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/startup.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>
#include <sys/hakios.version.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);

static vm_offset_t tile_north;
static LCK_GRP_DECLARE(amp_lock_group, "munch-amp");
static LCK_MTX_DECLARE(amp_lock, &amp_lock_group);     /* the bus */

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
	return length;
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

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

/* ---- I2C by hand on GPIO 119 (data) and 120 (clock), the north tile 0x0f900000 ------------------------ */

#define PIN_SDA                 119
#define PIN_SCL                 120
#define HALF_BIT_US             10

static vm_offset_t
pin_cfg(unsigned pin)
{
	return tile_north + 0x1000UL * pin;     /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
}

static bool
pin_in(unsigned pin)
{
	return rd(pin_cfg(pin) + 4) & 1;
}

static void
line(unsigned pin, bool high)
{
	if (high) {
		wr(pin_cfg(pin), 0);                    /* input, no pull of ours */
	} else {
		wr(pin_cfg(pin) + 4, 0);
		wr(pin_cfg(pin), 1u << 9);              /* output, 2 mA, low */
	}
	sleep_us(HALF_BIT_US);
}

static void
clock_up(void)
{
	line(PIN_SCL, true);
	for (int i = 0; i < 100 && !pin_in(PIN_SCL); i++) {
		sleep_us(HALF_BIT_US);  /* (a device stretching the clock) */
	}
}

static void
i2c_start(void)
{
	line(PIN_SDA, true);
	clock_up();
	line(PIN_SDA, false);
	line(PIN_SCL, false);
}

static void
i2c_stop(void)
{
	line(PIN_SDA, false);
	clock_up();
	line(PIN_SDA, true);
}

static bool
i2c_out(uint8_t byte)
{
	bool ack;

	for (int bit = 7; bit >= 0; bit--) {
		line(PIN_SDA, byte >> bit & 1);
		clock_up();
		line(PIN_SCL, false);
	}
	line(PIN_SDA, true);
	clock_up();
	ack = !pin_in(PIN_SDA);
	line(PIN_SCL, false);
	return ack;
}

static uint8_t
i2c_in(bool last)
{
	uint8_t byte = 0;

	line(PIN_SDA, true);
	for (int bit = 7; bit >= 0; bit--) {
		clock_up();
		byte = (uint8_t)(byte << 1 | pin_in(PIN_SDA));
		line(PIN_SCL, false);
	}
	line(PIN_SDA, last);
	clock_up();
	line(PIN_SCL, false);
	line(PIN_SDA, true);
	return byte;
}

static uint32_t saved_sda, saved_scl;
static volatile bool leaving;           /* on a panic's way out */

/* The two pins taken from the serial engine for a run of transfers, and given back after. */
static void
bus_take(void)
{
	/* (on a panic's way out the other processors are stopped, maybe one of them with the lock: it is not taken then) */
	if (!leaving) {
		lck_mtx_lock(&amp_lock);
	}
	saved_sda = rd(pin_cfg(PIN_SDA));
	saved_scl = rd(pin_cfg(PIN_SCL));
	line(PIN_SDA, true);
	line(PIN_SCL, true);
}

static void
bus_give(void)
{
	wr(pin_cfg(PIN_SDA), saved_sda);
	wr(pin_cfg(PIN_SCL), saved_scl);
	if (!leaving) {
		lck_mtx_unlock(&amp_lock);
	}
}

/* ---- the amplifiers' registers --------------------------------------------------------------------- */

struct amplifier {
	uint8_t address;
	const char *where, *name;       /* (name: its device list in the configuration) */
	bool there, prepared, started;
};

static struct amplifier amplifiers[2] = {
	{ 0x34, "0x34 (top)", "RCV", false, false, false },
	{ 0x35, "0x35 (bottom)", "SPK", false, false, false },
};
static bool broken;                     /* a fault, or a write that failed: both powered down, not started again */

static bool
reg_read(struct amplifier *a, uint8_t reg, uint16_t *value)
{
	bool ok;

	i2c_start();
	ok = i2c_out((uint8_t)(a->address << 1)) && i2c_out(reg);
	if (ok) {
		i2c_start();
		ok = i2c_out((uint8_t)(a->address << 1 | 1));
		if (ok) {
			uint8_t high = i2c_in(false), low = i2c_in(true);

			*value = (uint16_t)(high << 8 | low);
		}
	}
	i2c_stop();
	return ok;
}

static bool
reg_write(struct amplifier *a, uint8_t reg, uint16_t value)
{
	bool ok;

	i2c_start();
	ok = i2c_out((uint8_t)(a->address << 1)) && i2c_out(reg) && i2c_out((uint8_t)(value >> 8)) && i2c_out((uint8_t)value);
	i2c_stop();
	if (!ok) {
		say("speaker: the amplifier %s did not take %#06x into register %#04x\n", a->where, value, reg);
	}
	return ok;
}

/* A bitfield as NXP's code names them: register << 8 | first bit << 4 | (bits - 1). */
#define BF(reg, pos, len)       ((reg) << 8 | (pos) << 4 | ((len) - 1))
#define BF_PWDN                 BF(0x00, 0, 1)
#define BF_I2CR                 BF(0x00, 1, 1)
#define BF_AMPE                 BF(0x00, 3, 1)
#define BF_MANSCONF             BF(0x01, 2, 1)
#define BF_TDMNBCK              BF(0x20, 12, 4)
#define BF_TDMSLLN              BF(0x21, 4, 5)
#define BF_TDMSSIZE             BF(0x22, 2, 5)
#define REG_STATUS0             0x10            /* 1 OCP of the boost ok, 2 no over-temperature, 3 over-current (amplifier),
                                                 * 4 no under-voltage, 5 the alarm state, 7 lost clock */
#define REG_STATUS1             0x11            /* 6 PLL locked, 7 clocks stable, 13 amplifier engaged, 14 enabled */
#define REG_STATUS3             0x13            /* 8 no over-voltage, 9 clipping */
#define REG_STATUS4             0x14            /* [6:3] the device manager's state: 9 operating */
#define REG_REVISION            0x03
#define REVISION                0x0c74

static bool
bf_set(struct amplifier *a, uint16_t field, uint16_t value)
{
	uint8_t reg = (uint8_t)(field >> 8);
	unsigned pos = field >> 4 & 0xf, len = (field & 0xf) + 1;
	uint16_t mask = (uint16_t)(((1u << len) - 1) << pos), old;

	if (!reg_read(a, reg, &old)) {
		say("speaker: the amplifier %s did not answer for register %#04x\n", a->where, reg);
		return false;
	}
	return reg_write(a, reg, (uint16_t)((old & ~mask) | (value << pos & mask)));
}

static int
manager_state(struct amplifier *a)
{
	uint16_t value;

	return reg_read(a, REG_STATUS4, &value) ? (int)(value >> 3 & 0xf) : -1;
}

/* ---- the configuration (tfa98xx.cnt; tfa98xx_parameters.h: all little-endian, packed) -------------------- */

#define DSC_PROFILE             1
#define DSC_REGISTER            2
#define DSC_STRING              3
#define DSC_BITFIELD            16
#define DSC_DEFAULT             17
#define CNT_INDEX               46              /* the container header's size */

static const uint8_t *cnt;
static uint64_t cnt_size;

static uint32_t
le32(uint64_t at)
{
	return at + 4 <= cnt_size ? (uint32_t)cnt[at] | (uint32_t)cnt[at + 1] << 8 | (uint32_t)cnt[at + 2] << 16 |
	       (uint32_t)cnt[at + 3] << 24 : 0;
}

static uint16_t
le16(uint64_t at)
{
	return at + 2 <= cnt_size ? (uint16_t)(cnt[at] | cnt[at + 1] << 8) : 0;
}

/* A descriptor: where its data is (from the container's start), and its kind. */
static uint32_t
dsc_offset(uint64_t at)
{
	return le32(at) & 0xffffff;
}

static unsigned
dsc_type(uint64_t at)
{
	return le32(at) >> 24;
}

static bool
dsc_name_is(uint64_t at, const char *name)
{
	uint32_t offset = dsc_offset(at);
	size_t length = strlen(name);

	return dsc_type(at) == DSC_STRING && offset + length < cnt_size && memcmp(cnt + offset, name, length) == 0 &&
	       cnt[offset + length] == 0;
}

/*
 * The items of a list written to the chip: bitfields and register patches, up to the end or, in a
 * profile, its "default" section (what Android puts back when it leaves the profile). Other items --
 * the patch, the profiles, the DSP's messages -- are not the chip's registers and are passed over.
 */
static bool
write_items(struct amplifier *a, uint64_t items, unsigned count, unsigned *written)
{
	for (unsigned i = 0; i < count; i++) {
		uint64_t at = items + 4 * i;
		uint32_t offset = dsc_offset(at);

		switch (dsc_type(at)) {
		case DSC_BITFIELD:
			if (!bf_set(a, le16(offset + 2), le16(offset))) {
				return false;
			}
			(*written)++;
			break;
		case DSC_REGISTER: {
			uint8_t reg = offset < cnt_size ? cnt[offset] : 0;
			uint16_t value = le16(offset + 1), mask = le16(offset + 3), old;

			if (!reg_read(a, reg, &old) || !reg_write(a, reg, (uint16_t)((old & ~mask) | (value & mask)))) {
				return false;
			}
			(*written)++;
			break;
		}
		case DSC_DEFAULT:
			return true;
		default:
			break;
		}
	}
	return true;
}

/* The chip's device list, then its "stereo" profile; false if either is not found or a write fails. */
static bool
write_configuration(struct amplifier *a)
{
	unsigned devices, written = 0;
	uint64_t list = 0, profile = 0, items;
	unsigned count = 0, profile_count = 0;

	if (cnt_size < CNT_INDEX || memcmp(cnt, "PM3_", 4) != 0) {
		say("speaker: /fw/tfa98xx.cnt is not NXP's container\n");
		return false;
	}
	devices = le16(40);
	for (unsigned d = 0; d < devices && list == 0; d++) {
		uint64_t at = dsc_offset(CNT_INDEX + 4 * d);

		if (at + 12 <= cnt_size && cnt[at + 2] == a->address) {
			list = at;
			count = cnt[at];
		}
	}
	if (list == 0) {
		say("speaker: the configuration has no device list for %#x\n", a->address);
		return false;
	}
	items = list + 12;
	for (unsigned i = 0; i < count && profile == 0; i++) {
		uint32_t offset = dsc_offset(items + 4 * i);

		if (dsc_type(items + 4 * i) == DSC_PROFILE && dsc_name_is(offset + 4, "stereo")) {
			profile = offset;
			profile_count = (le32(offset) & 0xff) - 1;
		}
	}
	if (profile == 0) {
		say("speaker: the configuration has no profile \"stereo\" for %#x\n", a->address);
		return false;
	}
	if (!write_items(a, items, count, &written) || !write_items(a, profile + 8, profile_count, &written)) {
		return false;
	}
	say("speaker: the amplifier %s: %u settings of the configuration's \"%s\" list and its \"stereo\" profile written\n",
	    a->where, written, a->name);
	return true;
}

/* ---- set up, start, stop ------------------------------------------------------------------------------ */

/* tfa9874_specific(), revision 0x0c74 ("V1.14"): register, value. */
static const uint16_t optimal[][2] = {
	{ 0x02, 0x22c8 }, { 0x52, 0x57dc }, { 0x53, 0x003e }, { 0x56, 0x0400 }, { 0x61, 0x0110 }, { 0x6f, 0x00a5 },
	{ 0x70, 0x07f8 }, { 0x73, 0x0047 }, { 0x74, 0x5098 }, { 0x75, 0x8d28 }, { 0x80, 0x0000 }, { 0x83, 0x0799 },
	{ 0x84, 0x0081 },
};

static uint32_t slot_bits = 32;                 /* hakios_munch_amp_slots() */

void
hakios_munch_amp_slots(uint32_t bits)
{
	slot_bits = bits;
}

static bool
prepare_one(struct amplifier *a)
{
	uint16_t revision = 0, value = 0;

	if (!reg_read(a, REG_REVISION, &revision)) {
		say("speaker: the amplifier %s does not answer\n", a->where);
		return false;
	}
	if (revision != REVISION) {
		say("speaker: the amplifier %s has revision %#06x, not the TFA9874 %#06x this is written for\n", a->where, revision,
		    REVISION);
		return false;
	}
	a->there = true;
	/* tfa98xx_init(): every register to its power-on value (the chip in powerdown after it) */
	if (!reg_write(a, 0x00, 1u << 1)) {
		return false;
	}
	sleep_us(2000);
	/* tfa9874_specific(): the keys that unhide its registers, then its optimal settings */
	if (!reg_write(a, 0x0f, 0x5a6b) || !reg_read(a, 0xfb, &value) || !reg_write(a, 0xa0, value ^ 0x005a) ||
	    !reg_write(a, 0x0f, 0x5a6b) || !reg_write(a, 0xa1, 0x005a) || !reg_write(a, 0x0f, 0)) {
		return false;
	}
	for (unsigned i = 0; i < sizeof(optimal) / sizeof(optimal[0]); i++) {
		if (!reg_write(a, (uint8_t)optimal[i][0], optimal[i][1])) {
			return false;
		}
	}
	if (!write_configuration(a)) {
		return false;
	}
	/* the slots as they read while Android plays: 32 bits */
	if (!bf_set(a, BF_TDMNBCK, slot_bits == 16 ? 0 : 2) || !bf_set(a, BF_TDMSLLN, slot_bits - 1) ||
	    !bf_set(a, BF_TDMSSIZE, slot_bits - 1) || !bf_set(a, BF_PWDN, 1)) {
		return false;
	}
	reg_read(a, REG_STATUS0, &value);       /* (the sticky flags, cleared by the read) */
	a->prepared = true;
	return true;
}

/*
 * Its registers 0x00-0x8f as they are once it operates, those that are not 0, eight to a line: to be put
 * beside what Android's driver leaves in them while it plays (read through its sysfs "rw", read only).
 */
static uint32_t dump_wanted;                    /* boot-arg sound_dump=1: every start says all its registers */

static void
dump(struct amplifier *a)
{
	char line[200];
	unsigned length = 0, count = 0;

	for (unsigned reg = 0; reg < 0x90; reg++) {
		uint16_t value;

		if (!reg_read(a, (uint8_t)reg, &value) || value == 0) {
			continue;
		}
		length += (unsigned)snprintf(line + length, sizeof(line) - length, " %02x=%04x", reg, value);
		if (++count % 8 == 0) {
			say("speaker: %s:%s\n", a->where, line);
			length = 0;
		}
	}
	if (length != 0) {
		say("speaker: %s:%s\n", a->where, line);
	}
}

static void
power_down(struct amplifier *a)
{
	bf_set(a, BF_AMPE, 0);
	bf_set(a, BF_PWDN, 1);
	a->started = false;
}

static void
power_down_all(const char *why)
{
	say("speaker: both amplifiers powered down: %s\n", why);
	for (unsigned i = 0; i < 2; i++) {
		if (amplifiers[i].there) {
			power_down(&amplifiers[i]);
		}
	}
	broken = true;
}

/*
 * The north tile's addresses taken at the start: the amplifiers are prepared once the DSP is up, and by then the GPU's
 * buffers and Wi-Fi's may have taken the kernel's room for mappings (hakios.sound.c, sound_reserve()).
 */
__startup_func
static void
munch_amp_reserve(void)
{
	uint32_t wanted = 0;

	if (PE_parse_boot_argn("sound", &wanted, sizeof(wanted)) && wanted >= 4 && hakios_soc_is("sm8250")) {
		tile_north = ml_io_map(0x0f900000UL, 0x79000UL);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, munch_amp_reserve);

bool
hakios_munch_amp_prepare(const uint8_t *configuration, uint64_t size)
{
	bool ok = true;

	if (tile_north == 0) {
		tile_north = ml_io_map(0x0f900000UL, 0x79000UL);
	}
	cnt = configuration;
	cnt_size = size;
	broken = false;
	bus_take();
	for (unsigned i = 0; i < 2 && ok; i++) {
		ok = prepare_one(&amplifiers[i]);
	}
	if (!ok) {
		for (unsigned i = 0; i < 2; i++) {
			if (amplifiers[i].there) {
				bf_set(&amplifiers[i], BF_PWDN, 1);
			}
			amplifiers[i].prepared = false;
		}
	}
	bus_give();
	cnt = NULL;
	cnt_size = 0;
	if (ok) {
		say("speaker: both TFA9874s set up from Android's configuration, in powerdown\n");
	}
	return ok;
}

static volatile uint32_t gain_q15;      /* what hakios_munch_amp_scale() puts on the samples: 0 (silence) to 32767 */

bool
hakios_munch_amp_start(void)
{
	bool ok = true;

	if (broken) {
		return false;
	}
	PE_parse_boot_argn("sound_dump", &dump_wanted, sizeof(dump_wanted));
	gain_q15 = 0;
	bus_take();
	for (unsigned i = 0; i < 2 && ok; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t status = 0;
		int state = -1;

		if (!a->prepared) {
			ok = false;
			break;
		}
		/* tfa_dev_set_state(OPERATING) for a chip without a DSP of its own: out of powerdown, then the amplifier */
		ok = bf_set(a, BF_MANSCONF, 1) && bf_set(a, BF_PWDN, 0);
		for (unsigned wait = 0; ok && wait < 50; wait++) {
			if (reg_read(a, REG_STATUS1, &status) && (status & 1u << 7)) {
				break;
			}
			sleep_us(1000);
		}
		if (ok && !(status & 1u << 7)) {
			say("speaker: the amplifier %s: its clocks are not stable (status %#06x): no MI2S clock?\n", a->where, status);
			ok = false;
		}
		ok = ok && bf_set(a, BF_AMPE, 1);
		for (unsigned wait = 0; ok && wait < 100 && (state = manager_state(a)) != 9; wait++) {
			sleep_us(1000);
		}
		if (ok && state != 9) {
			say("speaker: the amplifier %s does not reach its operating state (the manager is in state %d)\n", a->where,
			    state);
			ok = false;
		}
		a->started = ok;
		if (ok) {
			reg_read(a, REG_STATUS1, &status);
			say("speaker: the amplifier %s is operating (status %#06x)\n", a->where, status);
			if (dump_wanted) {
				dump(a);        /* (some 150 ms of the bus each, while a sound waits for it) */
			}
		}
	}
	if (!ok) {
		for (unsigned i = 0; i < 2; i++) {
			if (amplifiers[i].there) {
				power_down(&amplifiers[i]);
			}
		}
	}
	bus_give();
	return ok;
}

/* A fault on either: both powered down at once. */
bool
hakios_munch_amp_healthy(void)
{
	bool ok = !broken;

	if (!ok) {
		return false;
	}
	bus_take();
	for (unsigned i = 0; i < 2 && ok; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t s0, s3;

		if (!a->started) {
			continue;
		}
		if (!reg_read(a, REG_STATUS0, &s0) || !reg_read(a, REG_STATUS3, &s3)) {
			continue;               /* (a read the bus lost: not a fault of the chip's) */
		}
		if (!(s0 & 1u << 2) || (s0 & 1u << 3) || !(s0 & 1u << 4) || (s0 & 1u << 5) || !(s3 & 1u << 8)) {
			say("speaker: the amplifier %s reports a fault: status %#06x %#06x (%s%s%s%s%s)\n", a->where, s0, s3,
			    !(s0 & 1u << 2) ? "over-temperature " : "", (s0 & 1u << 3) ? "over-current " : "",
			    !(s0 & 1u << 4) ? "under-voltage " : "", (s0 & 1u << 5) ? "alarm " : "", !(s3 & 1u << 8) ? "over-voltage" : "");
			power_down_all("an amplifier reports a fault");
			ok = false;
		}
	}
	bus_give();
	return ok;
}

/* ---- the volume: on the samples ----------------------------------------------------------------------- */

/* 10^(-k / 160) in Q15 for k = 0 to 47 eighths of a dB; each 48 more halve it (6.02 dB). */
static const uint16_t eighths[48] = {
	32767, 32300, 31838, 31383, 30935, 30493, 30057, 29628, 29205, 28787, 28376, 27970, 27571, 27177, 26789, 26406,
	26029, 25657, 25290, 24929, 24573, 24221, 23875, 23534, 23198, 22867, 22540, 22218, 21900, 21587, 21279, 20975,
	20675, 20380, 20089, 19802, 19519, 19240, 18965, 18694, 18427, 18164, 17904, 17648, 17396, 17147, 16902, 16661,
};
#define LEVEL_SILENT            (96 * 8)        /* 96 dB under: nothing */

static uint32_t level_below = LEVEL_SILENT;

static uint32_t
gain_of(uint32_t below)
{
	return below >= LEVEL_SILENT ? 0 : (uint32_t)eighths[below % 48] >> (below / 48);
}

/* To `below' eighths of a dB under Android's level, 6 dB a step, step_ms apart (hakios_speaker_volume()'s way). */
bool
hakios_munch_amp_volume(uint32_t below, uint32_t step_ms)
{
	if (broken) {
		return false;
	}
	if (below > LEVEL_SILENT) {
		below = LEVEL_SILENT;
	}
	while (level_below != below) {
		if (level_below > below) {
			level_below = level_below - below > 48 ? level_below - 48 : below;
		} else {
			level_below = below - level_below > 48 ? level_below + 48 : below;
		}
		gain_q15 = gain_of(level_below);
		if (level_below != below && step_ms) {
			delay_for_interval(step_ms, NSEC_PER_MSEC);
		}
	}
	return true;
}

/* Stereo frames on their way to the DSP, in place: the volume on them (silence while the amplifiers are off). */
void
hakios_munch_amp_scale(int16_t *frames, uint32_t count)
{
	uint32_t gain = gain_q15;

	if (gain >= 32767) {
		return;
	}
	for (uint32_t i = 0; i < 2 * count; i++) {
		frames[i] = (int16_t)(((int32_t)frames[i] * (int32_t)gain) >> 15);
	}
}

void
hakios_munch_amp_stop(void)
{
	bool any = false;

	for (unsigned i = 0; i < 2; i++) {
		any |= amplifiers[i].started;
	}
	if (!any) {
		return;
	}
	hakios_munch_amp_volume(LEVEL_SILENT, 10);
	bus_take();
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		int state = -1;

		if (!a->started) {
			continue;
		}
		power_down(a);
		for (unsigned wait = 0; wait < 100 && (state = manager_state(a)) >= 4; wait++) {
			sleep_us(1000);
		}
		say("speaker: the amplifier %s is stopped and in powerdown (the manager in state %d)\n", a->where, state);
	}
	bus_give();
}

/* On a panic's way out: powered down at once, the lock not waited for. */
void
hakios_munch_amp_quiesce(void)
{
	bool any = false;

	for (unsigned i = 0; i < 2; i++) {
		any |= amplifiers[i].started;
	}
	if (!any || tile_north == 0) {
		return;
	}
	leaving = true;
	for (unsigned i = 0; i < 2; i++) {
		if (amplifiers[i].started) {
			power_down(&amplifiers[i]);
		}
	}
}
