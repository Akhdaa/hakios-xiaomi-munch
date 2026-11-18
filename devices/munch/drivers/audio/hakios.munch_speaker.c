/*
 * The speakers of the POCO F4 (munch): their two amplifiers, NXP TFA98xx "smart PAs" (Android builds
 * its tfa9874 driver), looked at and not touched. Only with boot-arg speaker_probe=N and on munch
 * (hw=munch).
 *
 * Each speaker has its amplifier on the I2C bus of the first QUP's serial engine 3 (0x98c000;
 * GPIO 119 data, 120 clock, function qup3), at 0x34 (reset on GPIO 114, interrupt GPIO 113, a speaker
 * switch on GPIO 127) and 0x35 (reset GPIO 140, interrupt GPIO 112), and both are fed by the third
 * MI2S interface (GPIO 133 bit clock, 135 word select, 134 and 137 data), which the audio DSP drives.
 *
 *   speaker_probe=1    reads the pins and nothing else
 *   speaker_probe=2    ... and reads registers of the two amplifiers over I2C: the revision (0x03),
 *                      and the control and status words around it. NOTHING IS WRITTEN TO EITHER
 *                      CHIP: a read is the register's number sent, then the value read back. Their
 *                      reset pins are not moved: a chip held in reset by them does not answer, and
 *                      that is said.
 *
 * I2C is done by hand, as hakios_speaker.c does it: a pin is either pulled low or let go (the
 * board's pull-ups raise it), at about 50 kHz, and the two pins are taken from the serial engine as
 * GPIOs only for the transfers and given back as they were found.
 *
 * Numbers: the phone's own device tree (i2c@98c000, tfa98xx@34 and @35, qupv3_se3_i2c_active,
 * tert_mi2s_*_active), its Android kernel's drivers/pinctrl/qcom/pinctrl-kona.c (the tiles) and
 * techpack/audio/asoc/codecs/tfa9874 (the registers: 16 bits each, the most significant byte first).
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

static vm_offset_t tile_west, tile_south, tile_north;
static uint32_t probe_level;

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

/* ---- pins (TLMM: west 0x0f100000, south 0x0f500000, north 0x0f900000; a pin at its tile + 0x1000 * its number) */

#define PIN_SDA                 119
#define PIN_SCL                 120

static vm_offset_t
pin_cfg(unsigned pin)
{
	vm_offset_t tile = pin >= 133 ? tile_west : pin == 127 ? tile_south : tile_north;

	return tile + 0x1000UL * pin;           /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
}

static bool
pin_in(unsigned pin)
{
	return rd(pin_cfg(pin) + 4) & 1;
}

static const struct {
	unsigned pin;
	const char *what;
} pins[] = {
	{ 119, "I2C data (qup3)" }, { 120, "I2C clock (qup3)" },
	{ 114, "0x34's reset (high: in reset)" }, { 113, "0x34's interrupt" }, { 127, "0x34's speaker switch" },
	{ 140, "0x35's reset (high: in reset)" }, { 112, "0x35's interrupt" },
	{ 133, "MI2S 2 bit clock" }, { 135, "MI2S 2 word select" }, { 134, "MI2S 2 data 0" }, { 137, "MI2S 2 data 1" },
};

static void
show_pins(void)
{
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(pin_cfg(pins[i].pin)), io = rd(pin_cfg(pins[i].pin) + 4);

		printf("speaker: gpio%u (%s): cfg %#x (function %u, %s, pull %u, %u mA), in %u, out %u\n", pins[i].pin, pins[i].what,
		    cfg, cfg >> 2 & 0xf, (cfg & (1u << 9)) ? "output" : "input", cfg & 3, ((cfg >> 6 & 7) + 1) * 2, io & 1,
		    io >> 1 & 1);
	}
}

/* ---- I2C by hand: low = an output driving 0; released = an input, raised by the pull-up ------------- */

#define HALF_BIT_US             10

static void
line(unsigned pin, bool high)
{
	if (high) {
		wr(pin_cfg(pin), 0);                    /* input, no pull of ours */
	} else {
		wr(pin_cfg(pin) + 4, 0);
		wr(pin_cfg(pin), 1u << 9 | (2 / 2 - 1) << 6);   /* output, 2 mA, low */
	}
	sleep_us(HALF_BIT_US);
}

static void
i2c_start(void)
{
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	line(PIN_SDA, false);
	line(PIN_SCL, false);
}

static void
i2c_stop(void)
{
	line(PIN_SDA, false);
	line(PIN_SCL, true);
	line(PIN_SDA, true);
}

/* One byte out; true if it was acknowledged. */
static bool
i2c_out(uint8_t byte)
{
	bool ack;

	for (int bit = 7; bit >= 0; bit--) {
		line(PIN_SDA, byte >> bit & 1);
		line(PIN_SCL, true);
		for (int i = 0; i < 100 && !pin_in(PIN_SCL); i++) {
			sleep_us(HALF_BIT_US);  /* (a device stretching the clock) */
		}
		line(PIN_SCL, false);
	}
	line(PIN_SDA, true);
	line(PIN_SCL, true);
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
		line(PIN_SCL, true);
		byte = (uint8_t)(byte << 1 | pin_in(PIN_SDA));
		line(PIN_SCL, false);
	}
	line(PIN_SDA, last);                    /* acknowledge all but the last */
	line(PIN_SCL, true);
	line(PIN_SCL, false);
	line(PIN_SDA, true);
	return byte;
}

/* A register of a TFA98xx: its number written, then two bytes read. Nothing is written into the chip. */
static bool
tfa_read(uint8_t address, uint8_t reg, uint16_t *value)
{
	bool ok;

	i2c_start();
	ok = i2c_out((uint8_t)(address << 1)) && i2c_out(reg);
	if (ok) {
		i2c_start();
		ok = i2c_out((uint8_t)(address << 1 | 1));
		if (ok) {
			uint8_t high = i2c_in(false), low = i2c_in(true);

			*value = (uint16_t)(high << 8 | low);
		}
	}
	i2c_stop();
	return ok;
}

static const char *
tfa_name(uint16_t revision)
{
	switch (revision & 0xff) {
	case 0x74: return "TFA9874";
	case 0x72: return "TFA9872";
	case 0x73: return "TFA9873";
	case 0x78: return "TFA9878";
	case 0x94: return "TFA9894";
	case 0x12: return "TFA9912";
	default: return "a chip this does not know";
	}
}

static void
probe_amplifiers(void)
{
	static const uint8_t addresses[2] = { 0x34, 0x35 };
	static const uint8_t regs[] = { 0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13 };
	uint32_t sda = rd(pin_cfg(PIN_SDA)), scl = rd(pin_cfg(PIN_SCL));

	printf("speaker: I2C by hand on gpio%u/%u (taken from the serial engine: cfg %#x %#x, given back after)\n", PIN_SDA,
	    PIN_SCL, sda, scl);
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	if (!pin_in(PIN_SDA) || !pin_in(PIN_SCL)) {
		printf("speaker: a line stays low when let go (data %u, clock %u): no transfers\n", pin_in(PIN_SDA), pin_in(PIN_SCL));
		goto back;
	}
	for (unsigned a = 0; a < 2; a++) {
		uint16_t value = 0;
		char row[160];
		int at = 0;

		if (!tfa_read(addresses[a], 0x03, &value)) {
			printf("speaker: %#x does not answer (held in reset, without power, or not there)\n", addresses[a]);
			continue;
		}
		printf("speaker: %#x answers: revision %#06x, %s\n", addresses[a], value, tfa_name(value));
		row[0] = 0;
		for (unsigned r = 0; r < sizeof(regs); r++) {
			if (tfa_read(addresses[a], regs[r], &value)) {
				at += snprintf(row + at, sizeof(row) - (size_t)at, " %02x=%04x", regs[r], value);
			} else {
				at += snprintf(row + at, sizeof(row) - (size_t)at, " %02x=?", regs[r]);
			}
		}
		printf("speaker: %#x registers:%s\n", addresses[a], row);
	}
back:
	wr(pin_cfg(PIN_SDA), sda);
	wr(pin_cfg(PIN_SCL), scl);
}

static void
probe_thread(__unused void *parameter, __unused wait_result_t result)
{
	tile_west = ml_io_map(0x0f100000UL, 0x90000UL);
	tile_south = ml_io_map(0x0f500000UL, 0x80000UL);
	tile_north = ml_io_map(0x0f900000UL, 0x79000UL);
	printf("speaker: the two amplifiers (speaker_probe=%u): the pins as they are\n", probe_level);
	show_pins();
	if (probe_level >= 2) {
		probe_amplifiers();
	}
	printf("speaker: over\n");
	thread_terminate(current_thread());
}

static void
hakios_munch_speaker_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("speaker_probe", &probe_level, sizeof(probe_level)) ||
	    probe_level == 0) {
		return;
	}
	if (kernel_thread_start(probe_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_munch_speaker_init);
