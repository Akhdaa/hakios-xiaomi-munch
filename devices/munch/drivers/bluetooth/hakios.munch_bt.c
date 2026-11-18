/*
 * QCA6390 Bluetooth on munch. The E404 device tree puts it on QUP0 SE6
 * (Android /dev/ttyHS0). Wi-Fi already votes for the shared power rails.
 * munch_bt=1 queries the ROM, 2 probes HCI and BLE, and 3 exposes HCI to iOS.
 * munch_bt=4 tests the iOS bridge at 460800 baud after the ROM query.
 */
#include <stdint.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.version.h>
#include <sys/random.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern kern_return_t thread_terminate(thread_t thread);
extern bool munch_bt_host_start(const uint8_t *address);
extern unsigned munch_bt_read_tx(uint8_t *bytes);
extern unsigned munch_bt_h4_length(const uint8_t *bytes, unsigned have);
extern void munch_btsk_deliver(const uint8_t *bytes, unsigned length);
void hakios_munch_bt_radio_ready(void);
void hakios_munch_bt_quiesce(void);
void hakios_munch_bt_stop(void);

#define GCC_PHYS        0x00100000UL
#define TLMM_PHYS       0x0f900000UL
#define UART_PHYS       0x00998000UL
#define GCC_M_AHB       0x17004
#define GCC_S_AHB       0x17008
#define GCC_SE6_CBCR    0x1772c
#define GCC_SE6_CMD     0x17730
#define GCC_SE6_CFG     0x17734
#define GCC_SE6_M       0x17738
#define GCC_SE6_N       0x1773c
#define GCC_SE6_D       0x17740
#define GCC_SE6_DFS     0x17744
#define GCC_SE6_PERF_CFG 0x1774c
#define GCC_SE6_PERF_M  0x1778c
#define GCC_SE6_PERF_N  0x177cc
#define GCC_AP_VOTE     0x52008
#define SE6_VOTE        (1u << 16)
#define CLK_OFF         (1u << 31)
#define CLK_HW_GATE     (1u << 1)

#define GPIO_CFG(pin)   (0x1000 * (pin))
#define GPIO_IO(pin)    (GPIO_CFG(pin) + 4)
#define GPIO_OUTPUT     (1u << 9)
#define GPIO_QUP6       (1u << 2)
#define WLAN_EN         20
#define BT_EN           21
#define SW_CTRL         124

#define GENI_STATUS     0x040
#define GENI_ACTIVE     ((1u << 12) | 1u)
#define GENI_M_CLOCK    0x048
#define GENI_S_CLOCK    0x04c
#define GENI_IF_DISABLE 0x064
#define GENI_PROTOCOL   0x068
#define GENI_CLK_SEL    0x07c
#define GENI_M_CMD      0x600
#define GENI_M_CTRL     0x604
#define GENI_M_IRQ      0x610
#define GENI_M_CLEAR    0x618
#define GENI_S_CMD      0x630
#define GENI_S_CTRL     0x634
#define GENI_S_IRQ      0x640
#define GENI_S_CLEAR    0x648
#define GENI_TX_FIFO    0x700
#define GENI_RX_FIFO    0x780
#define GENI_TX_STATUS  0x800
#define GENI_RX_STATUS  0x804
#define GENI_IOS        0x908
#define GENI_HW_PARAM_0 0xe24
#define GENI_ABORT      (1u << 1)
#define UART_TX_LEN     0x270
#define UART_SAMPLE_HZ  (115200u * 16)
#define QCA_EDL         0xfc00
#define QCA_SET_BAUD    0xfc48
#define QCA_BAUD_460800 7
#define QCA_VERSION     0x19
#define QCA_TLV         0x1e
#define QCA_SEGMENT     243
#define QCA_IBS_WAKE_ACK 0xfc
#define QCA_IBS_WAKE_IND 0xfd
#define QCA_IBS_SLEEP_IND 0xfe
#define H4_FRAME_MAX    2048
#define HCI_READ_SUPPORTED_COMMANDS 0x1002
#define HCI_SET_EVENT_MASK 0x0c01
#define HCI_WRITE_ERRONEOUS_REPORTING 0x0c5b
#define HCI_ERRONEOUS_REPORTING_BIT 147
#define HCI_LE_SET_EVENT_MASK 0x2001
#define HCI_LE_READ_LOCAL_FEATURES 0x2003
#define HCI_LE_SET_SCAN_PARAMETERS 0x200b
#define HCI_LE_SET_SCAN_ENABLE 0x200c
#define HCI_LE_READ_PERIODIC_LIST_SIZE 0x204a

static vm_offset_t gcc, tlmm, uart;
static const unsigned bt_pins[] = { 16, 17, 18, 19, BT_EN, SW_CTRL };
static const unsigned rcg_offsets[] = { GCC_SE6_M, GCC_SE6_N, GCC_SE6_D, GCC_SE6_CFG };
static uint32_t saved_rcg[sizeof(rcg_offsets) / sizeof(rcg_offsets[0])];
static bool rcg_changed, vote_added;
static uint32_t clock_level, clock_divider;
static uint32_t rx_word;
static unsigned rx_left, probe_level;
static unsigned rx_bytes, rx_wake, rx_sleep, rx_wake_ack;
static uint8_t bt_address[6];
static bool bt_powered, bt_stopping, bt_worker_active;
static bool emulate_sco_reporting;

static void
bt_log(const char *format, ...) __printflike(1, 2);

static void
bt_log(const char *format, ...)
{
	char line[192];
	va_list args;

	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	for (const char *p = line; *p != '\0'; p++) {
		loader_log_putc(*p);
	}
	printf("%s", line);
}

static uint32_t
rd(vm_offset_t base, unsigned offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static void
wr(vm_offset_t base, unsigned offset, uint32_t value)
{
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

static bool
wait_reg(vm_offset_t base, unsigned offset, uint32_t mask, uint32_t value, unsigned timeout_ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(timeout_ms, NSEC_PER_MSEC, &deadline);
	do {
		if ((rd(base, offset) & mask) == value) {
			return true;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	} while (mach_absolute_time() < deadline);
	return false;
}

static uint32_t
dfs_rate(unsigned level)
{
	static const uint32_t parent[] = { 19200000, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint32_t cfg = rd(gcc, GCC_SE6_PERF_CFG + 4 * level);
	uint32_t div = cfg & 31;
	uint64_t hz = parent[(cfg >> 8) & 7];

	if (div != 0) {
		hz = hz * 2 / (div + 1);
	}
	if (cfg & (3u << 12)) {
		uint32_t m = rd(gcc, GCC_SE6_PERF_M + 4 * level) & 0xffff;
		uint32_t n = (~rd(gcc, GCC_SE6_PERF_N + 4 * level) & 0xffff) + m;
		hz = n != 0 ? hz * m / n : 0;
	}
	return (uint32_t)hz;
}

static bool
clock_start(void)
{
	bt_log("munch-bt: checking SE6 clock ownership\n");
	if ((rd(gcc, GCC_M_AHB) & CLK_OFF) ||
	    (rd(gcc, GCC_S_AHB) & (CLK_OFF | CLK_HW_GATE)) == CLK_OFF ||
	    (rd(gcc, GCC_AP_VOTE) & SE6_VOTE)) {
		bt_log("munch-bt: SE6 ownership blocked: M_AHB=%#x S_AHB=%#x AP vote=%u\n",
		    rd(gcc, GCC_M_AHB), rd(gcc, GCC_S_AHB), !!(rd(gcc, GCC_AP_VOTE) & SE6_VOTE));
		return false;
	}
	if (rd(gcc, GCC_SE6_DFS) & 1) {
		for (unsigned i = 0; i < 8; i++) {
			uint32_t hz = dfs_rate(i);
			if (hz != 0 && hz % UART_SAMPLE_HZ == 0) {
				clock_level = i;
				clock_divider = hz / UART_SAMPLE_HZ;
				break;
			}
		}
		if (clock_divider == 0) {
			bt_log("munch-bt: no SE6 DFS preset for 115200 baud\n");
			return false;
		}
	} else {
		bt_log("munch-bt: selecting the XO clock for SE6\n");
		for (unsigned i = 0; i < sizeof(rcg_offsets) / sizeof(rcg_offsets[0]); i++) {
			saved_rcg[i] = rd(gcc, rcg_offsets[i]);
		}
		rcg_changed = true;
		/* 19.2 MHz XO * 48/125 = 7.3728 MHz, divided by 4 for 16x sampling. */
		wr(gcc, GCC_SE6_M, 48);
		wr(gcc, GCC_SE6_N, ~(125u - 48u) & 0xffff);
		wr(gcc, GCC_SE6_D, ~125u & 0xffff);
		wr(gcc, GCC_SE6_CFG, (saved_rcg[3] & ~0x371fu) | 0x2001);
		wr(gcc, GCC_SE6_CMD, rd(gcc, GCC_SE6_CMD) | 1);
		if (!wait_reg(gcc, GCC_SE6_CMD, 1, 0, 5)) {
			return false;
		}
		clock_divider = 4;
	}
	bt_log("munch-bt: voting for the SE6 clock\n");
	wr(gcc, GCC_AP_VOTE, rd(gcc, GCC_AP_VOTE) | SE6_VOTE);
	vote_added = true;
	return wait_reg(gcc, GCC_SE6_CBCR, CLK_OFF, 0, 10);
}

static void
clock_stop(void)
{
	if (vote_added) {
		wr(gcc, GCC_AP_VOTE, rd(gcc, GCC_AP_VOTE) & ~SE6_VOTE);
	}
	if (rcg_changed) {
		for (unsigned i = 0; i < sizeof(rcg_offsets) / sizeof(rcg_offsets[0]); i++) {
			wr(gcc, rcg_offsets[i], saved_rcg[i]);
		}
		wr(gcc, GCC_SE6_CMD, rd(gcc, GCC_SE6_CMD) | 1);
		(void)wait_reg(gcc, GCC_SE6_CMD, 1, 0, 5);
	}
}

static void
uart_setup(void)
{
	const uint32_t clock = (clock_divider << 4) | 1;

	bt_log("munch-bt: configuring GENI IO defaults\n");
	wr(uart, 0x028, rd(uart, 0x028) | 0x7f);    /* internal clock gating */
	wr(uart, 0x024, 0x7f);                     /* default IO levels */
	wr(uart, 0x020, 1);
	bt_log("munch-bt: configuring GENI packing and framing\n");
	wr(uart, 0x258, rd(uart, 0x258) & ~1u);     /* FIFO mode */
	wr(uart, 0x254, 0);
	/* Four 8-bit characters per FIFO word, low byte first. */
	wr(uart, 0x260, 0x4380e);
	wr(uart, 0x264, 0xc3e0e);
	wr(uart, 0x284, 0x4380e);
	wr(uart, 0x288, 0xc3e0e);
	wr(uart, 0x22c, 0);                        /* no loopback */
	wr(uart, 0x25c, 0);                        /* hardware CTS, no parity */
	wr(uart, 0x268, 8);
	wr(uart, 0x26c, 0);                        /* one stop bit */
	wr(uart, 0x280, 0);
	wr(uart, 0x28c, 8);
	wr(uart, 0x294, 160);                      /* RX stale count */
	wr(uart, 0x2a4, 0);
	wr(uart, 0x2a8, 0);
	wr(uart, 0x2ac, 0);                        /* hardware RTS */
	bt_log("munch-bt: programming UART serial clocks\n");
	wr(uart, GENI_CLK_SEL, clock_level);
	wr(uart, GENI_M_CLOCK, clock);
	wr(uart, GENI_S_CLOCK, clock);
	wr(uart, GENI_M_CLEAR, UINT32_MAX);
	wr(uart, GENI_S_CLEAR, UINT32_MAX);
	bt_log("munch-bt: starting the GENI receiver\n");
	wr(uart, GENI_S_CMD, (1u << 27) | (1u << 7)); /* START_READ, RFR_OPEN */
}

static bool
uart_send(const uint8_t *bytes, unsigned length)
{
	unsigned depth = (rd(uart, GENI_HW_PARAM_0) >> 16) & 0x3f;
	unsigned timeout_ms = 50 + length / 10;
	uint64_t deadline;

	if (depth == 0 || __atomic_load_n(&bt_stopping, __ATOMIC_ACQUIRE)) {
		return false;
	}
	wr(uart, GENI_M_CLEAR, UINT32_MAX);
	wr(uart, UART_TX_LEN, length);
	wr(uart, GENI_M_CMD, 1u << 27);
	clock_interval_to_deadline(timeout_ms, NSEC_PER_MSEC, &deadline);
	for (unsigned sent = 0; sent < length;) {
		uint32_t word = 0;
		while ((rd(uart, GENI_TX_STATUS) & 0x0fffffff) >= depth) {
			if (mach_absolute_time() >= deadline || __atomic_load_n(&bt_stopping, __ATOMIC_ACQUIRE)) {
				return false;
			}
			delay_for_interval(50, NSEC_PER_USEC);
		}
		for (unsigned shift = 0; shift < 32 && sent < length; shift += 8) {
			word |= (uint32_t)bytes[sent++] << shift;
		}
		wr(uart, GENI_TX_FIFO, word);
	}
	return wait_reg(uart, GENI_M_IRQ, 1, 1, timeout_ms);
}

static bool
uart_read_byte(uint64_t deadline, uint8_t *byte)
{
	while (rx_left == 0) {
		uint32_t status = rd(uart, GENI_RX_STATUS);
		unsigned words = status & 0x1ffffff;
		if (words != 0) {
			rx_left = (words == 1 && (status >> 31)) ? (status >> 28) & 7 : 4;
			if (rx_left == 0 || rx_left > 4) {
				rx_left = 4;
			}
			rx_word = rd(uart, GENI_RX_FIFO);
			break;
		}
		if (mach_absolute_time() >= deadline || __atomic_load_n(&bt_stopping, __ATOMIC_ACQUIRE)) {
			return false;
		}
		delay_for_interval(50, NSEC_PER_USEC);
	}
	*byte = (uint8_t)rx_word;
	rx_word >>= 8;
	rx_left--;
	rx_bytes++;
	return true;
}

struct hci_event {
	uint8_t code, length;
	uint8_t data[255];
};

static bool
read_event(uint64_t deadline, struct hci_event *event)
{
	static const uint8_t wake_ack = QCA_IBS_WAKE_ACK;
	static uint8_t packet[3 + 255];
	static unsigned have, wanted;
	uint8_t byte;

	/* A scan deadline may fall inside a packet. Keep its prefix for the next command. */
	while (mach_absolute_time() < deadline) {
		if (!uart_read_byte(deadline, &byte)) {
			return false;
		}
		if (have == 0) {
			if (byte != 4) {
				if (byte == QCA_IBS_WAKE_IND) {
					rx_wake++;
					if (!uart_send(&wake_ack, 1)) {
						return false;
					}
				}
				rx_sleep += byte == QCA_IBS_SLEEP_IND;
				rx_wake_ack += byte == QCA_IBS_WAKE_ACK;
				continue;
			}
			wanted = 3;
		}
		packet[have++] = byte;
		if (have == 3) {
			wanted += packet[2];
		}
		if (have == wanted) {
			event->code = packet[1];
			event->length = packet[2];
			memcpy(event->data, packet + 3, event->length);
			have = 0;
			return true;
		}
	}
	return false;
}

static uint16_t
read_le16(const uint8_t *p)
{
	return p[0] | ((uint16_t)p[1] << 8);
}

static bool
hci_command(uint16_t opcode, const uint8_t *parameters, uint8_t length, struct hci_event *reply)
{
	uint8_t packet[4 + 255] = { 1, (uint8_t)opcode, (uint8_t)(opcode >> 8), length };
	uint64_t deadline;

	if (length != 0) {
		memcpy(packet + 4, parameters, length);
	}
	if (!uart_send(packet, 4 + length)) {
		bt_log("munch-bt: command %#x TX timeout, IOS=%#x IRQ=%#x\n", opcode, rd(uart, GENI_IOS), rd(uart, GENI_M_IRQ));
		return false;
	}
	if (reply == NULL) {
		return true;
	}
	clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
	while (read_event(deadline, reply)) {
		if (reply->code == 0x0e && reply->length >= 4 && read_le16(reply->data + 1) == opcode) {
			if (reply->data[3] != 0) {
				bt_log("munch-bt: command %#x returned status %#x\n", opcode, reply->data[3]);
				return false;
			}
			return true;
		}
	}
	bt_log("munch-bt: command %#x RX timeout, IOS=%#x IRQ=%#x\n", opcode, rd(uart, GENI_IOS), rd(uart, GENI_S_IRQ));
	return false;
}

static bool
switch_operating_baud(void)
{
	const uint8_t baud = QCA_BAUD_460800;

	/* The existing serial clock is exact at 4x baud with one quarter of the divider. */
	if (clock_divider < 4 || clock_divider % 4 != 0) {
		bt_log("munch-bt: SE6 clock cannot divide exactly to 460800 baud\n");
		return false;
	}
	if (!hci_command(QCA_SET_BAUD, &baud, sizeof(baud), NULL)) {
		return false;
	}
	/* The controller switches after receiving the command at the ROM's rate. */
	delay_for_interval(300, NSEC_PER_MSEC);
	wr(uart, GENI_S_CTRL, GENI_ABORT);
	if (!wait_reg(uart, GENI_STATUS, GENI_ACTIVE, 0, 10)) {
		bt_log("munch-bt: SE6 did not stop before the baud switch\n");
		return false;
	}
	uint32_t clock = ((clock_divider / 4) << 4) | 1;
	rx_left = 0;
	wr(uart, GENI_M_CLOCK, clock);
	wr(uart, GENI_S_CLOCK, clock);
	wr(uart, GENI_M_CLEAR, UINT32_MAX);
	wr(uart, GENI_S_CLEAR, UINT32_MAX);
	wr(uart, GENI_S_CMD, (1u << 27) | (1u << 7));
	const uint8_t version = QCA_VERSION;
	struct hci_event event;
	if (!hci_command(QCA_EDL, &version, sizeof(version), &event) ||
	    event.length != 18 || event.data[4] != QCA_VERSION) {
		return false;
	}
	bt_log("munch-bt: controller confirmed at 460800 baud\n");
	return true;
}

static bool
send_tlv(const uint8_t *image, size_t size, bool skip)
{
	uint8_t data[2 + QCA_SEGMENT] = { QCA_TLV };
	struct hci_event reply;

	for (size_t offset = 0; offset < size;) {
		size_t count = size - offset;
		if (count > QCA_SEGMENT) {
			count = QCA_SEGMENT;
		}
		data[1] = (uint8_t)count;
		memcpy(data + 2, image + offset, count);
		bool ack = !skip || offset + count == size;
		if (!hci_command(QCA_EDL, data, (uint8_t)(count + 2), ack ? &reply : NULL) ||
		    (ack && (reply.length != 5 || reply.data[4] != QCA_TLV))) {
			bt_log("munch-bt: TLV transfer stopped at byte %lu of %lu\n", (unsigned long)offset, (unsigned long)size);
			return false;
		}
		offset += count;
	}
	return true;
}

static size_t
tlv_length(const uint8_t *data)
{
	return data[1] | ((size_t)data[2] << 8) | ((size_t)data[3] << 16);
}

static bool
prepare_nvm(uint8_t *nvm, size_t size)
{
	bool address = false, transport = false;

	if (size < 4 || nvm[0] != 2 || tlv_length(nvm) != size - 4) {
		return false;
	}
	for (size_t offset = 4; offset < size;) {
		if (size - offset < 12) {
			return false;
		}
		uint16_t id = read_le16(nvm + offset);
		uint16_t length = read_le16(nvm + offset + 2);
		offset += 12;
		if (length > size - offset) {
			return false;
		}
		uint8_t *tag = nvm + offset;
		if (id == 2 && length == 6) {
			read_random(tag, 6);
			tag[5] = (tag[5] | 2) & ~1u;  /* local unicast address, HCI byte order */
			memcpy(bt_address, tag, sizeof(bt_address));
			address = true;
		} else if (id == 17 && length >= 3) {
			tag[0] &= ~0x80u;            /* no in-band sleep while polling */
			tag[1] = probe_level == 4 ? QCA_BAUD_460800 : 0;
			transport = true;
		} else if (id == 27 && length != 0) {
			tag[0] &= ~1u;
		}
		offset += length;
	}
	return address && transport;
}

static bool
load_firmware(uint16_t product, uint16_t rom)
{
	uint64_t patch_size, nvm_size;
	const uint8_t *patch = hakios_image_file("fw", "htbtfw20.tlv", &patch_size, NULL);
	const uint8_t *original = hakios_image_file("fw", "htnv20.bin", &nvm_size, NULL);

	if (patch == NULL || original == NULL || patch_size < 28 || patch_size > (1u << 20) ||
	    nvm_size < 4 || nvm_size > (1u << 16) || patch[0] != 1 ||
	    tlv_length(patch) != patch_size - 4 || read_le16(patch + 16) != product ||
	    read_le16(patch + 18) != rom || (patch[14] != 0 && patch[14] != 3)) {
		bt_log("munch-bt: missing or incompatible htbtfw20.tlv / htnv20.bin\n");
		return false;
	}
	uint8_t *nvm = kalloc_data((size_t)nvm_size, Z_WAITOK);
	if (nvm == NULL) {
		return false;
	}
	memcpy(nvm, original, (size_t)nvm_size);
	bool ok = false;
	if (prepare_nvm(nvm, (size_t)nvm_size)) {
		bt_log("munch-bt: downloading Bluetooth patch and board configuration at %u baud\n",
		    probe_level == 4 ? 460800 : 115200);
		if (send_tlv(patch, (size_t)patch_size, patch[14] == 3)) {
			delay_for_interval(10, NSEC_PER_MSEC);
			ok = send_tlv(nvm, (size_t)nvm_size, false);
		}
	} else {
		bt_log("munch-bt: Bluetooth NVM tags invalid\n");
	}
	kfree_data(nvm, (size_t)nvm_size);
	return ok;
}

static void
apple_vendor_command(const uint8_t *packet, unsigned length)
{
	uint8_t reply[7 + 64] = { 4, 0x0e, 4, 1, packet[1], packet[2], 0 };
	uint16_t opcode = read_le16(packet + 1);
	unsigned extra = 0;

	/* Reply to the Broadcom setup queries that iOS 16.5 uses on H4BC.
	 * Qualcomm vendor opcodes share this range and must stay on the boot path. */
	switch (opcode) {
	case 0xfce9: case 0xfd47: case 0xfd4e:
		break;
	case 0xfd48:
		extra = 1;
		break;
	case 0xfd1b:
		if (length > 4 && packet[4] == 0x51) {
			extra = 64;
			reply[7] = 0x51;
		} else {
			reply[6] = 1;
		}
		break;
	case 0xfce2:
		if (length > 4 && (packet[4] == 0x15 || packet[4] == 0x36)) {
			extra = 16;
		} else if (length <= 4 || (packet[4] != 0x0b && packet[4] != 0x1c && packet[4] != 0x26)) {
			reply[6] = 1;
		}
		break;
	default:
		reply[6] = 1;
		break;
	}
	reply[2] += extra;
	munch_btsk_deliver(reply, 7 + extra);
	bt_log("munch-bt: Apple vendor command %#x status %#x\n", opcode, reply[6]);
}

static void
deliver_hci_reply(uint8_t *packet, unsigned length)
{
	static unsigned replies_logged;
	static unsigned adv_events;

	if (length >= 5 && packet[0] == 4 && packet[1] == 0x3e && packet[3] == 0x02) {
		if (++adv_events <= 3 || adv_events % 32 == 0) {
			bt_log("munch-bt: controller LE advertising event %u, reports=%u\n", adv_events, packet[4]);
		}
	}

	if (length >= 7 && packet[0] == 4 && (packet[1] == 0x0e || packet[1] == 0x0f)) {
		bool complete = packet[1] == 0x0e;
		uint16_t opcode = read_le16(packet + (complete ? 4 : 5));
		uint8_t status = packet[complete ? 6 : 3];

		if (complete && status == 0 && opcode == HCI_READ_SUPPORTED_COMMANDS && length == 7 + 64) {
			uint8_t *octet = &packet[7 + HCI_ERRONEOUS_REPORTING_BIT / 8];
			const uint8_t bit = 1u << (HCI_ERRONEOUS_REPORTING_BIT % 8);

			/* iOS refuses to start unless it can configure this optional SCO command. */
			emulate_sco_reporting = (*octet & bit) == 0;
			*octet |= bit;

			/* iOS uses legacy scanning on H4BC. Keep extended advertising
			 * commands out of that HCI session, including periodic list queries. */
			uint8_t *commands = packet + 7;
			commands[36] &= 0x01; /* Bits 289–310: extended and periodic advertising/scan. */
			commands[37] = 0;
			commands[38] &= 0x80;
			commands[40] &= 0x1f; /* Bits 325–329: periodic advertising sync. */
			commands[41] &= 0xfc;
		}
		if (complete && status == 0 && opcode == HCI_LE_READ_LOCAL_FEATURES && length == 7 + 8) {
			packet[8] &= (uint8_t)~0x30; /* LE extended and periodic advertising. */
			packet[10] &= (uint8_t)~0x03; /* Periodic advertising sync transfer. */
		}
		/* iOS initializes the optional SCO error-reporting switch even on
		 * controllers that do not implement it. Preserve command credits. */
		if (length == 7 && opcode == HCI_WRITE_ERRONEOUS_REPORTING && (status == 0x01 || status == 0x11)) {
			packet[1] = 0x0e;
			packet[3] = packet[complete ? 3 : 4];
			packet[4] = (uint8_t)opcode;
			packet[5] = (uint8_t)(opcode >> 8);
			packet[6] = 0;
			bt_log("munch-bt: optional SCO reporting %#x unavailable (%#x), acknowledged to iOS\n", opcode, status);
		} else if (replies_logged++ < 48 || status != 0 ||
		    opcode == HCI_LE_SET_SCAN_PARAMETERS || opcode == HCI_LE_SET_SCAN_ENABLE) {
			bt_log("munch-bt: iOS HCI reply %#x status %#x\n", opcode, status);
		}
	}
	munch_btsk_deliver(packet, length);
}

static void
bridge_hci(void)
{
	static uint8_t rx[H4_FRAME_MAX], tx[H4_FRAME_MAX];
	static const uint8_t wake_ack = QCA_IBS_WAKE_ACK;
	unsigned have = 0;
	unsigned reported = 0;

	if (!munch_bt_host_start(bt_address)) {
		bt_log("munch-bt: iOS transport setup failed\n");
		return;
	}
	bt_log("munch-bt: iOS H4BC transport ready\n");
	while (!__atomic_load_n(&bt_stopping, __ATOMIC_ACQUIRE)) {
		uint64_t deadline;
		uint8_t byte;

		clock_interval_to_deadline(1, NSEC_PER_MSEC, &deadline);
		while (mach_absolute_time() < deadline && uart_read_byte(mach_absolute_time(), &byte)) {
			if (have == 0 && byte == QCA_IBS_WAKE_IND) {
				(void)uart_send(&wake_ack, 1);
				continue;
			}
			rx[have++] = byte;
			unsigned length = munch_bt_h4_length(rx, have);
			if (length == UINT32_MAX) {
				have = 0;
			} else if (length != 0 && length == have) {
				if (rx[0] != 1) {
					deliver_hci_reply(rx, length);
				}
				have = 0;
			}
		}
		unsigned length = munch_bt_read_tx(tx);
		if (length == 0) {
			/* At 460800 baud a quarter millisecond is less than 12 RX bytes. */
			assert_wait_timeout((event_t)&bt_stopping, THREAD_UNINT, 250, NSEC_PER_USEC);
			thread_block(THREAD_CONTINUE_NULL);
			continue;
		}
		if (tx[0] == 4) {
			continue;
		}
		uint16_t opcode = tx[0] == 1 ? read_le16(tx + 1) : 0;
		if (tx[0] == 1 && (tx[2] >> 2) == 0x3f) {
			apple_vendor_command(tx, length);
		} else if (emulate_sco_reporting && tx[0] == 1 && length == 5 &&
		    opcode == HCI_WRITE_ERRONEOUS_REPORTING && tx[4] <= 1) {
			const uint8_t reply[] = { 4, 0x0e, 4, 1, tx[1], tx[2], 0 };
			munch_btsk_deliver(reply, sizeof(reply));
			bt_log("munch-bt: iOS SCO reporting setup acknowledged locally\n");
		} else if (opcode == HCI_LE_READ_PERIODIC_LIST_SIZE && length == 4) {
			const uint8_t reply[] = { 4, 0x0e, 5, 1, tx[1], tx[2], 0, 0 };
			munch_btsk_deliver(reply, sizeof(reply));
			bt_log("munch-bt: iOS periodic advertiser list is disabled\n");
		} else if (!uart_send(tx, length)) {
			bt_log("munch-bt: iOS H4 TX timeout (type %u, %u bytes)\n", tx[0], length);
			return;
		} else if (opcode == HCI_SET_EVENT_MASK && length == 12) {
			bt_log("munch-bt: iOS HCI event mask LE meta=%u\n", !!(tx[11] & 0x20));
		} else if (opcode == HCI_LE_SET_EVENT_MASK && length == 12) {
			bt_log("munch-bt: iOS LE event mask legacy advertising=%u\n", !!(tx[4] & 0x02));
		} else if (opcode == HCI_LE_SET_SCAN_PARAMETERS && length == 11) {
			bt_log("munch-bt: iOS LE scan type=%u interval=%u window=%u address type=%u filter policy=%u\n",
			    tx[4], read_le16(tx + 5), read_le16(tx + 7), tx[9], tx[10]);
		} else if (opcode == HCI_LE_SET_SCAN_ENABLE && length == 6) {
			bt_log("munch-bt: iOS LE scan enable %u\n", tx[4]);
		} else if (tx[0] == 1 && reported++ < 24) {
			bt_log("munch-bt: iOS HCI command %#x sent\n", opcode);
		}
	}
}

void
hakios_munch_bt_quiesce(void)
{
	__atomic_store_n(&bt_stopping, true, __ATOMIC_RELEASE);
	if (__atomic_exchange_n(&bt_powered, false, __ATOMIC_ACQ_REL)) {
		wr(tlmm, GPIO_IO(BT_EN), 0);
	}
}

/* Wait for the UART worker before loader return stops the other CPUs. */
void
hakios_munch_bt_stop(void)
{
	uint64_t deadline;

	__atomic_store_n(&bt_stopping, true, __ATOMIC_RELEASE);
	clock_interval_to_deadline(1500, NSEC_PER_MSEC, &deadline);
	while (__atomic_load_n(&bt_worker_active, __ATOMIC_ACQUIRE)) {
		if (mach_absolute_time() >= deadline) {
			bt_log("munch-bt: UART worker did not stop before loader return\n");
			return;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
}

static void
probe_controller(void)
{
	static const uint8_t version = QCA_VERSION;
	struct hci_event event;

	if (!hci_command(QCA_EDL, &version, 1, &event) ||
	    event.length != 18 || event.data[4] != QCA_VERSION) {
		return;
	}
	uint16_t product = read_le16(event.data + 6);
	uint16_t rom = read_le16(event.data + 12);
	bt_log("munch-bt: ROM response, product=%#x ROM=%#x\n", product, rom);
	if (probe_level < 2 || (probe_level == 4 && !switch_operating_baud()) || !load_firmware(product, rom)) {
		return;
	}
	static const uint16_t queries[] = { 0x0c03, 0x1001, 0x1003, 0x1005, 0x2002 };
	for (unsigned i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
		if (!hci_command(queries[i], NULL, 0, &event)) {
			return;
		}
		bt_log("munch-bt: HCI command %#x passed\n", queries[i]);
	}
	if (probe_level >= 3) {
		bridge_hci();
		return;
	}
	const uint8_t event_mask[8] = { [7] = 0x20 }; /* LE Meta Event */
	const uint8_t le_event_mask[8] = { 0x02 };     /* LE Advertising Report */
	const uint8_t scan_parameters[] = { 0, 0x30, 0, 0x30, 0, 0, 0 };
	const uint8_t enable_scan[] = { 1, 1 }, disable_scan[] = { 0, 0 };
	if (!hci_command(0x0c01, event_mask, sizeof(event_mask), &event) ||
	    !hci_command(0x2001, le_event_mask, sizeof(le_event_mask), &event) ||
	    !hci_command(0x200b, scan_parameters, sizeof(scan_parameters), &event)) {
		return;
	}
	const unsigned scan_ms[] = { 150, 5000 };
	for (unsigned pass = 0; pass < sizeof(scan_ms) / sizeof(scan_ms[0]); pass++) {
		uint64_t deadline;
		unsigned events = 0, reports = 0, start_bytes;

		if (!hci_command(0x200c, enable_scan, sizeof(enable_scan), &event)) {
			return;
		}
		wr(uart, GENI_S_CLEAR, UINT32_MAX);
		start_bytes = rx_bytes;
		rx_wake = rx_sleep = rx_wake_ack = 0;
		bt_log("munch-bt: scan enabled, status=%#x IOS=%#x\n", rd(uart, GENI_STATUS), rd(uart, GENI_IOS));
		clock_interval_to_deadline(scan_ms[pass], NSEC_PER_MSEC, &deadline);
		while (read_event(deadline, &event)) {
			events++;
			if (event.code == 0x3e && event.length >= 2 && event.data[0] == 2) {
				reports += event.data[1];
			}
		}
		bt_log("munch-bt: scan %u ms: %u reports, %u events, %u RX bytes, status=%#x IOS=%#x IRQ=%#x\n",
		    scan_ms[pass], reports, events, rx_bytes - start_bytes,
		    rd(uart, GENI_STATUS), rd(uart, GENI_IOS), rd(uart, GENI_S_IRQ));
		if (!hci_command(0x200c, disable_scan, sizeof(disable_scan), &event)) {
			bt_log("munch-bt: scan stop failed, status=%#x, RX wake/sleep/ack=%u/%u/%u\n",
			    rd(uart, GENI_STATUS), rx_wake, rx_sleep, rx_wake_ack);
			return;
		}
		bt_log("munch-bt: scan disabled after %u ms, RX wake/sleep/ack=%u/%u/%u\n",
		    scan_ms[pass], rx_wake, rx_sleep, rx_wake_ack);
	}
}

static void
probe(void)
{
	uint32_t saved_cfg[sizeof(bt_pins) / sizeof(bt_pins[0])];
	uint32_t saved_io[sizeof(bt_pins) / sizeof(bt_pins[0])];

	if (__atomic_load_n(&bt_stopping, __ATOMIC_ACQUIRE)) {
		return;
	}
	bt_log("munch-bt: probe %u entered\n", probe_level);
	gcc = ml_io_map(GCC_PHYS, 0x60000);
	tlmm = ml_io_map(TLMM_PHYS, 0x80000);
	if (gcc == 0 || tlmm == 0) {
		bt_log("munch-bt: cannot map GCC or TLMM\n");
		return;
	}
	if (!(rd(tlmm, GPIO_CFG(WLAN_EN)) & GPIO_OUTPUT) || !(rd(tlmm, GPIO_IO(WLAN_EN)) & 1)) {
		bt_log("munch-bt: shared radio power is off; start Wi-Fi for this probe\n");
		return;
	}
	if (!clock_start()) {
		bt_log("munch-bt: SE6 clock unavailable\n");
		goto done;
	}
	bt_log("munch-bt: SE6 clock running, checking the UART protocol\n");
	uart = ml_io_map(UART_PHYS, 0x4000);
	if (uart == 0 || ((rd(uart, GENI_PROTOCOL) >> 8) & 0xff) != 2 ||
	    (rd(uart, GENI_IF_DISABLE) & 1) || (rd(uart, GENI_STATUS) & GENI_ACTIVE)) {
		bt_log("munch-bt: SE6 is not an idle FIFO UART\n");
		goto done;
	}
	for (unsigned i = 0; i < sizeof(bt_pins) / sizeof(bt_pins[0]); i++) {
		saved_cfg[i] = rd(tlmm, GPIO_CFG(bt_pins[i]));
		saved_io[i] = rd(tlmm, GPIO_IO(bt_pins[i]));
	}
	bt_log("munch-bt: configuring the Bluetooth pins and resetting the controller\n");
	wr(tlmm, GPIO_IO(BT_EN), 0);
	wr(tlmm, GPIO_CFG(BT_EN), GPIO_OUTPUT);
	delay_for_interval(50, NSEC_PER_MSEC);
	for (unsigned pin = 16; pin <= 19; pin++) {
		wr(tlmm, GPIO_CFG(pin), GPIO_QUP6);
	}
	wr(tlmm, GPIO_CFG(SW_CTRL), 0);
	bt_log("munch-bt: configuring the UART FIFOs\n");
	uart_setup();
	wr(tlmm, GPIO_IO(BT_EN), 2);
	__atomic_store_n(&bt_powered, true, __ATOMIC_RELEASE);
	delay_for_interval(100, NSEC_PER_MSEC);
	bt_log("munch-bt: SE6 at 115200, SW_CTRL=%u, DFS=%u, divider=%u\n",
	    rd(tlmm, GPIO_IO(SW_CTRL)) & 1, clock_level, clock_divider);
	probe_controller();
	hakios_munch_bt_quiesce();
	wr(uart, GENI_S_CTRL, GENI_ABORT);
	if (rd(uart, GENI_STATUS) & 1) {
		wr(uart, GENI_M_CTRL, GENI_ABORT);
	}
	(void)wait_reg(uart, GENI_STATUS, GENI_ACTIVE, 0, 10);
	wr(uart, GENI_M_CLOCK, 0);
	wr(uart, GENI_S_CLOCK, 0);
	for (unsigned i = 0; i < sizeof(bt_pins) / sizeof(bt_pins[0]); i++) {
		wr(tlmm, GPIO_IO(bt_pins[i]), saved_io[i] & 2);
		wr(tlmm, GPIO_CFG(bt_pins[i]), saved_cfg[i]);
	}
done:
	clock_stop();
	bt_log("munch-bt: UART cleanup complete, AP vote=%u\n", !!(rd(gcc, GCC_AP_VOTE) & SE6_VOTE));
}

static void
probe_thread(__unused void *arg, __unused wait_result_t result)
{
	probe();
	__atomic_store_n(&bt_worker_active, false, __ATOMIC_RELEASE);
	thread_terminate(current_thread());
}

void
hakios_munch_bt_radio_ready(void)
{
	static bool started;
	unsigned level;
	thread_t thread;

	if (!PE_parse_boot_argn("munch_bt", &level, sizeof(level)) || level == 0 || level > 4 ||
	    __atomic_exchange_n(&started, true, __ATOMIC_ACQ_REL)) {
		return;
	}
	probe_level = level;
	__atomic_store_n(&bt_worker_active, true, __ATOMIC_RELEASE);
	if (kernel_thread_start(probe_thread, NULL, &thread) != KERN_SUCCESS) {
		__atomic_store_n(&bt_worker_active, false, __ATOMIC_RELEASE);
		__atomic_store_n(&started, false, __ATOMIC_RELEASE);
		bt_log("munch-bt: could not start the Bluetooth worker\n");
		return;
	}
	thread_deallocate(thread);
}
