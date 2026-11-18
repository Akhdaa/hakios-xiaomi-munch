/*
 * iOS 16.5's bluetoothd reaches the controller through a kernel control
 * and a Skywalk H4BC channel. The control ABI also works on the MTK port.
 */
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/termios.h>
#include <sys/ttycom.h>
#include <sys/uio.h>
#include <uuid/uuid.h>

#define H4_FRAME_MAX 2048u
#define H4_INVALID UINT32_MAX
#define BT_CHANNEL_UUID 0x40105412u

extern bool munch_btsk_init(void);
extern void munch_btsk_close(void);
extern bool munch_btsk_uuid(uuid_t out);
extern bool munch_bt_publish(const uint8_t *address);
bool munch_bt_host_start(const uint8_t *address);
bool munch_bt_queue_tx(const uint8_t *bytes, unsigned length);
unsigned munch_bt_read_tx(uint8_t *bytes);
unsigned munch_bt_h4_length(const uint8_t *bytes, unsigned have);

static LCK_GRP_DECLARE(bt_host_group, "munch-bt-host");
static LCK_MTX_DECLARE(bt_tx_lock, &bt_host_group);
static uint8_t tx_queue[8192];
static unsigned tx_count;
static kern_ctl_ref control;
static bool host_ready, control_connected;

static bool
bt_daemon(void)
{
	char name[32] = "";
	proc_selfname(name, sizeof(name));
	return strcmp(name, "bluetoothd") == 0 || strcmp(name, "BlueTool") == 0;
}

unsigned
munch_bt_h4_length(const uint8_t *bytes, unsigned have)
{
	static const uint8_t header[] = { 0, 4, 5, 4, 3, 5 };
	if (have == 0) {
		return 0;
	}
	unsigned type = bytes[0];
	if (type == 0 || type >= sizeof(header)) {
		return H4_INVALID;
	}
	unsigned start = header[type];
	if (have < start) {
		return 0;
	}
	unsigned payload = bytes[start - 1];
	if (type == 2 || type == 5) {
		payload = bytes[3] | ((payload & (type == 5 ? 0x3f : 0xff)) << 8);
	}
	return payload <= H4_FRAME_MAX - start ? start + payload : H4_INVALID;
}

bool
munch_bt_queue_tx(const uint8_t *bytes, unsigned length)
{
	lck_mtx_lock(&bt_tx_lock);
	bool queued = length <= sizeof(tx_queue) - tx_count;
	if (queued) {
		memcpy(tx_queue + tx_count, bytes, length);
		tx_count += length;
	}
	lck_mtx_unlock(&bt_tx_lock);
	return queued;
}

unsigned
munch_bt_read_tx(uint8_t *bytes)
{
	unsigned skipped = 0, length;

	lck_mtx_lock(&bt_tx_lock);
	do {
		length = munch_bt_h4_length(tx_queue + skipped, tx_count - skipped);
		if (length != H4_INVALID) {
			break;
		}
		skipped++;
	} while (skipped < tx_count);
	if (length == H4_INVALID || length > tx_count - skipped) {
		length = 0;
	}
	memcpy(bytes, tx_queue + skipped, length);
	tx_count -= skipped + length;
	memmove(tx_queue, tx_queue + skipped + length, tx_count);
	lck_mtx_unlock(&bt_tx_lock);
	return length;
}

static errno_t
control_connect(__unused kern_ctl_ref ctl, __unused struct sockaddr_ctl *addr, __unused void **context)
{
	if (!bt_daemon()) {
		return EPERM;
	}
	if (!__atomic_load_n(&host_ready, __ATOMIC_ACQUIRE)) {
		return ENXIO;
	}
	lck_mtx_lock(&bt_tx_lock);
	bool already = control_connected;
	control_connected = true;
	lck_mtx_unlock(&bt_tx_lock);
	if (already) {
		return EBUSY;
	}
	printf("munch-bt: bluetoothd transport control connected\n");
	return 0;
}

static errno_t
control_disconnect(__unused kern_ctl_ref ctl, __unused uint32_t unit, __unused void *context)
{
	lck_mtx_lock(&bt_tx_lock);
	control_connected = false;
	tx_count = 0;
	lck_mtx_unlock(&bt_tx_lock);
	return 0;
}

static errno_t
control_send(__unused kern_ctl_ref ctl, __unused uint32_t unit, __unused void *context, mbuf_t packet, __unused int flags)
{
	size_t length = mbuf_pkthdr_len(packet);
	errno_t result = ENOBUFS;

	lck_mtx_lock(&bt_tx_lock);
	if (length <= sizeof(tx_queue) - tx_count) {
		result = mbuf_copydata(packet, 0, length, tx_queue + tx_count);
		if (result == 0) {
			tx_count += (unsigned)length;
		}
	}
	lck_mtx_unlock(&bt_tx_lock);
	mbuf_freem(packet);
	return result;
}

static errno_t
control_setopt(__unused kern_ctl_ref ctl, __unused uint32_t unit, __unused void *context,
    __unused int option, __unused void *data, __unused size_t length)
{
	/* bluetoothd supplies its channel specification before asking for the UUID. */
	return 0;
}

static errno_t
control_getopt(__unused kern_ctl_ref ctl, __unused uint32_t unit, __unused void *context, int option, void *data, size_t *length)
{
	uuid_t uuid;
	if ((unsigned)option != BT_CHANNEL_UUID) {
		return ENOPROTOOPT;
	}
	if (data == NULL || *length < sizeof(uuid) || !munch_btsk_uuid(uuid)) {
		return EINVAL;
	}
	memcpy(data, uuid, sizeof(uuid));
	*length = sizeof(uuid);
	return 0;
}

/* QCA in-band sleep handles wake on the UART. These nodes satisfy the
 * separate Apple GPIO interface while the controller is owned by the driver. */
static int
bt_node_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *proc)
{
	return bt_daemon() ? 0 : EPERM;
}

static int
bt_node_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *proc)
{
	return 0;
}

static int
bt_node_read(__unused dev_t dev, __unused struct uio *uio, __unused int flags)
{
	return 0;
}

static int
bt_node_write(__unused dev_t dev, struct uio *uio, __unused int flags)
{
	uio_setresid(uio, 0);
	return 0;
}

static int
bt_node_ioctl(__unused dev_t dev, u_long command, caddr_t data, __unused int flag, __unused struct proc *proc)
{
	switch (command) {
	case TIOCGETA: {
		struct termios *settings = (struct termios *)(void *)data;
		bzero(settings, sizeof(*settings));
		settings->c_cflag = CS8 | CREAD | CLOCAL;
		settings->c_ispeed = settings->c_ospeed = 115200;
		return 0;
	}
	case TIOCMGET:
		*(int *)(void *)data = TIOCM_CTS | TIOCM_DSR | TIOCM_CAR | TIOCM_RTS | TIOCM_DTR;
		return 0;
	case FIONREAD:
		*(int *)(void *)data = 0;
		return 0;
	case TIOCSETA: case TIOCSETAW: case TIOCSETAF: case TIOCEXCL: case TIOCNXCL:
	case TIOCSDTR: case TIOCCDTR: case TIOCMBIS: case TIOCMBIC: case TIOCMSET:
	case TIOCFLUSH: case TIOCSBRK: case TIOCCBRK: case TIOCDRAIN: case FIONBIO: case FIOASYNC:
		return 0;
	default:
		return ENOTTY;
	}
}

static const struct cdevsw bt_cdev = {
	.d_open = bt_node_open, .d_close = bt_node_close, .d_read = bt_node_read, .d_write = bt_node_write,
	.d_ioctl = bt_node_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat,
};

bool
munch_bt_host_start(const uint8_t *address)
{
	static const char *const names[] = { "btwake", "btreset", "btpoweroff" };
	void *nodes[sizeof(names) / sizeof(names[0])] = { NULL };
	struct kern_ctl_reg registration = {
		.ctl_name = "com.apple.uart.sk.bluetooth",
		.ctl_flags = CTL_FLAG_REG_SOCK_STREAM,
		.ctl_sendsize = sizeof(tx_queue),
		.ctl_recvsize = 65536,
		.ctl_connect = control_connect,
		.ctl_disconnect = control_disconnect,
		.ctl_send = control_send,
		.ctl_setopt = control_setopt,
		.ctl_getopt = control_getopt,
	};
	if (!munch_btsk_init()) {
		return false;
	}
	int major = cdevsw_add(-1, &bt_cdev);
	if (major < 0) {
		goto fail;
	}
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		nodes[i] = devfs_make_node(makedev(major, i), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "%s", names[i]);
		if (nodes[i] == NULL) {
			goto fail;
		}
	}
	errno_t error = ctl_register(&registration, &control);
	if (error != 0 || !munch_bt_publish(address)) {
		printf("munch-bt: control or registry setup failed (%d)\n", error);
		goto fail;
	}
	__atomic_store_n(&host_ready, true, __ATOMIC_RELEASE);
	return true;
fail:
	if (control != NULL) {
		(void)ctl_deregister(control);
		control = NULL;
	}
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (nodes[i] != NULL) {
			devfs_remove(nodes[i]);
		}
	}
	if (major >= 0) {
		(void)cdevsw_remove(major, &bt_cdev);
	}
	munch_btsk_close();
	return false;
}
