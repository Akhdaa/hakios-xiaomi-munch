/*
 * The battery of the POCO F4 (munch), as its fuel gauge says it is: a TI bq27z561 (Android builds
 * Xiaomi's bq27z561_fg_psyche.c for it), read and never written. Only with boot-arg battery=1 and
 * on munch (hw=munch); redwood's battery (hakios_battery.c, hakios_adsp.c) comes another way.
 *
 * The gauge is at 0x55 on the I2C bus of the third QUP's serial engine 16 (0x888000; GPIO 48 data,
 * 49 clock, function qup16, the south tile of the pin controller). Every ten seconds its
 * standard registers are read -- 16 bits each, the least significant byte first (SMBus words) --
 * and what they say goes to the power source iOS reads, through hakios_battery_report():
 *   0x06 temperature, tenths of a kelvin    0x08 voltage, mV        0x0a battery status (bit 6:
 *   0x0c current, mA (more than 0: charging)                         discharging, bit 5: full)
 *   0x10 remaining capacity, mAh            0x12 full capacity, mAh 0x2a cycles
 *   0x2c relative state of charge, percent  0x3c design capacity, mAh
 * NOTHING IS WRITTEN TO THE GAUGE: a read is the register's number sent, then the value read back.
 * Charging stays the phone's own (the charger and its limits are not touched). Whether a charger is
 * there and charging is the charger's own word, read from the PM8150B (hakios_munch_charger(),
 * bsd/dev/hakios_buttons.c: read only): a computer's port gives less than the phone takes at full
 * speed, and the gauge then says discharging with the cable in. Only if that read fails is it
 * reckoned from the gauge: not discharging, and current going in or the battery full.
 *
 * I2C is done by hand, as hakios_munch_speaker.c does it: a pin is either pulled low or let go
 * (the board's pull-ups raise it), at about 50 kHz, and the two pins are taken from the serial
 * engine as GPIOs only for the transfers and given back as they were found.
 *
 * Numbers: the phone's own device tree (i2c@888000, bq27z561@55, qupv3_se16_i2c_active), its
 * Android kernel's drivers/power/supply/qcom/bq27z561_fg_psyche.c (bq27z561_regs[]) and
 * drivers/pinctrl/qcom/pinctrl-kona.c.
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

static vm_offset_t tile_south;

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

/*
 * boot-arg battery_stop=<percent>: the run handed back to the loader before the battery is flat, as upstream's
 * battguard does; three looks in a row at or below it (30 s), and a look is every 10 s. Upstream's waits while a
 * charger is attached. Here that is no reason: from a computer's port this phone takes less than it uses, and the
 * charge falls with the cable in. So what counts is the gauge's own current: going out of the battery, whatever is
 * plugged in; a charge that rises never stops a run. Nothing is written to the gauge or the charger.
 */
extern void loader_return(void);
extern void hakios_var_save_on_return(void);
extern void hakios_adreno_stop(void);
extern void hakios_wifi_stop(void);
extern void hakios_adsp_stop(void);

static uint32_t stop_percent;

static void
battery_guard(uint32_t percent, int current_ma)
{
	static unsigned strikes;

	strikes = stop_percent != 0 && percent <= stop_percent && current_ma < 0 ? strikes + 1 : 0;
	if (strikes == 3) {
		printf("battery: munch: %u%%, at or below battery_stop=%u, %d mA going out, three looks in a row: back to the "
		    "loader\n", percent, stop_percent, current_ma);
		hakios_var_save_on_return();
		hakios_adreno_stop();
		hakios_wifi_stop();
		hakios_adsp_stop();
		loader_return();
	}
}

/* ---- pins (TLMM south tile 0x0f500000; a pin at its tile + 0x1000 * its number) ---------------------- */

#define PIN_SDA                 48
#define PIN_SCL                 49
#define GAUGE                   0x55

static vm_offset_t
pin_cfg(unsigned pin)
{
	return tile_south + 0x1000UL * pin;     /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
}

static bool
pin_in(unsigned pin)
{
	return rd(pin_cfg(pin) + 4) & 1;
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
		wr(pin_cfg(pin), 1u << 9);              /* output, 2 mA, low */
	}
	sleep_us(HALF_BIT_US);
}

/* The clock let go, and held by the gauge for as long as it wants (it stretches the clock). */
static void
clock_up(void)
{
	line(PIN_SCL, true);
	for (int i = 0; i < 2000 && !pin_in(PIN_SCL); i++) {
		sleep_us(HALF_BIT_US);
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

/* One byte out; true if it was acknowledged. */
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
	line(PIN_SDA, last);                    /* acknowledge all but the last */
	clock_up();
	line(PIN_SCL, false);
	line(PIN_SDA, true);
	return byte;
}

/* A register of the gauge: its number written, then two bytes read, the low one first. */
static bool
gauge_read(uint8_t reg, uint16_t *value)
{
	bool ok;

	i2c_start();
	ok = i2c_out(GAUGE << 1) && i2c_out(reg);
	if (ok) {
		i2c_start();
		ok = i2c_out(GAUGE << 1 | 1);
		if (ok) {
			uint8_t low = i2c_in(false), high = i2c_in(true);

			*value = (uint16_t)(high << 8 | low);
		}
	}
	i2c_stop();
	return ok;
}

/* ---- the readings ---------------------------------------------------------------------------------- */

#define REG_TEMPERATURE         0x06
#define REG_VOLTAGE             0x08
#define REG_STATUS              0x0a
#define REG_CURRENT             0x0c
#define REG_REMAINING           0x10
#define REG_FULL                0x12
#define REG_CYCLES              0x2a
#define REG_SOC                 0x2c
#define REG_DESIGN              0x3c
#define STATUS_DISCHARGING      (1u << 6)
#define STATUS_FULL             (1u << 5)
/* the PM8150B, as bsd/dev/hakios_buttons.c reads it (hakios_munch_charger()) */
#define USBIN_PLUGIN            (1u << 4)
#define PORT_SDP                (1u << 0)
#define PORT_CDP                (1u << 2)
#define PORT_DCP                (1u << 3)
#define PORT_QC2                (1u << 5)
#define PORT_QC3                (1u << 6)
#define CHARGER_TRICKLE         1
#define CHARGER_TAPER           4
#define CHARGER_TERMINATED      5

static uint64_t reads, failures;

/* All of the registers in one go, the pins taken from the serial engine meanwhile; false if any read failed. */
static bool
gauge_sample(uint16_t values[9])
{
	static const uint8_t regs[9] = { REG_SOC, REG_VOLTAGE, REG_CURRENT, REG_TEMPERATURE, REG_STATUS, REG_REMAINING,
		                         REG_FULL, REG_DESIGN, REG_CYCLES };
	uint32_t sda = rd(pin_cfg(PIN_SDA)), scl = rd(pin_cfg(PIN_SCL));
	bool ok = true;

	line(PIN_SDA, true);
	line(PIN_SCL, true);
	if (!pin_in(PIN_SDA) || !pin_in(PIN_SCL)) {
		/* (a transfer left half-done: nine clocks let the gauge finish its byte) */
		for (int i = 0; i < 9 && !pin_in(PIN_SDA); i++) {
			line(PIN_SCL, false);
			clock_up();
		}
		i2c_stop();
	}
	for (unsigned i = 0; i < 9 && ok; i++) {
		ok = gauge_read(regs[i], &values[i]);
		if (!ok) {
			ok = gauge_read(regs[i], &values[i]);    /* once more: the gauge may have been busy */
		}
	}
	wr(pin_cfg(PIN_SDA), sda);
	wr(pin_cfg(PIN_SCL), scl);
	return ok;
}

static void
battery_thread(__unused void *parameter, __unused wait_result_t result)
{
	tile_south = ml_io_map(0x0f500000UL, 0x80000UL);
	PE_parse_boot_argn("battery_stop", &stop_percent, sizeof(stop_percent));
	if (stop_percent > 99) {
		stop_percent = 0;
	}
	if (stop_percent != 0) {
		printf("battery: munch: the run ends at %u%% or less with the charge going out (battery_stop)\n", stop_percent);
	}
	printf("battery: munch: the fuel gauge (bq27z561 at %#x, I2C by hand on gpio%u/%u), read every 10 s; nothing written\n",
	    GAUGE, PIN_SDA, PIN_SCL);
	for (unsigned tick = 0;; tick++) {
		uint16_t v[9];

		reads++;
		if (!gauge_sample(v) || v[0] > 100) {
			failures++;
			if (failures <= 3 || failures % 30 == 0) {
				printf("battery: munch: the gauge did not answer (%llu of %llu reads failed)\n", failures, reads);
			}
		} else {
			struct hakios_battery_state b;
			uint8_t input = 0, port = 0, state = 0;
			bool pmic;
			int16_t current = (int16_t)v[2];
			bool discharging = v[4] & STATUS_DISCHARGING, full = v[4] & STATUS_FULL;

			bzero(&b, sizeof(b));
			b.size = sizeof(b);
			b.percent_hundredths = v[0] * 100;
			b.voltage_uv = v[1] * 1000;
			b.current_ua = current * 1000;
			b.temperature_hundredths = ((int)v[3] - 2731) * 10;
			b.present = 1;
			if (hakios_munch_charger(&input, &port, &state)) {
				/* the charger's own word: plugged in, and charging while it is in trickle, pre-charge, full current or taper */
				state &= 7;
				b.external = (input & USBIN_PLUGIN) != 0;
				b.status = !b.external ? 2 : state == CHARGER_TERMINATED || full ? 4 :
				    state >= CHARGER_TRICKLE && state <= CHARGER_TAPER ? 1 : 3;
				b.usb_type = !b.external ? 0 : port & (PORT_DCP | PORT_QC2 | PORT_QC3) ? 2 : port & PORT_CDP ? 3 :
				    port & PORT_SDP ? 1 : 5;
				/* (what the port is allowed to give by its kind, not a measurement: there is no ADC read here) */
				b.usb_voltage_uv = b.external ? 5000000 : 0;
				b.usb_current_ua = !b.external ? 0 : b.usb_type == 1 ? 500000 : 1500000;
				pmic = true;
			} else {
				/* without the charger's word, the gauge's: not discharging, and current going in or the battery full */
				b.external = !discharging && (current > 0 || full);
				b.status = b.external ? (full ? 4 : current > 0 ? 1 : 3) : 2;
				pmic = false;
			}
			b.charge_uah = v[5] * 1000;
			b.full_uah = v[6] * 1000;
			b.design_uah = v[7] * 1000;
			b.cycles = v[8];
			if (tick % 30 == 0) {
				printf("battery: munch: %u%%, %u mV, %d mA, %d.%d C, status %#x (%s), %u of %u mAh (design %u), %u cycles\n",
				    v[0], v[1], current, b.temperature_hundredths / 100, (b.temperature_hundredths % 100) / 10, v[4],
				    b.status == 1 ? "charging" : b.status == 4 ? "full" : b.status == 3 ? "not charging" : "discharging",
				    v[5], v[6], v[7], v[8]);
				if (pmic) {
					printf("battery: munch: the charger (PM8150B): USB input %#x (%s), port %#x, state %u\n", input,
					    b.external ? "plugged in" : "nothing", port, state);
				}
			}
			hakios_battery_report(&b);
			battery_guard(v[0], current);
		}
		delay_for_interval(10000, NSEC_PER_MSEC);
	}
}

static void
hakios_munch_battery_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("battery", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_munch_battery_init);
