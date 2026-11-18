/*
 * /dev/hakios-touch: a queue of touch samples (see sys/hakios.input.h).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/sysctl.h>
#include <pexpert/pexpert.h>

#define TOUCH_QUEUE     512             /* records; a power of two */

/*
 * Boot-arg touch_lat=1: for measuring how long a touch takes to reach the panel, a line for every
 * sample posted ("tl: p TYPE FINGER X Y at US", the time in microseconds of mach_absolute_time),
 * to be matched with the window server's lines about reading it (userland/src/inject.c), drawing
 * (eagl.c) and the display driver's about the frame (MiniIOMobileFramebuffer.cpp): see
 * tools/latency.py.
 */
static uint32_t touch_lat = 2;          /* 2: not yet looked up */

extern void loader_log_line(const char *line);

/*
 * A "tl:" line, for the kernel's drivers: into the loader's log as it is written -- the kernel's own
 * log keeps only the last of a run, and a live look at the loader's (XL_FOLLOW) then misses nothing.
 */
void hakios_tl(const char *format, ...) __printflike(1, 2);

void
hakios_tl(const char *format, ...)
{
	char line[192];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
}

static bool
latency_lines(void)
{
	if (touch_lat == 2) {
		uint32_t on = 0;

		touch_lat = PE_parse_boot_argn("touch_lat", &on, sizeof(on)) && on != 0;
	}
	return touch_lat != 0;
}

static LCK_GRP_DECLARE(touch_lck_grp, "hakios-touch");
static LCK_MTX_DECLARE(touch_lock, &touch_lck_grp);
static struct hakios_touch touch_queue[TOUCH_QUEUE];
static uint32_t touch_head, touch_tail;        /* read from head, add at tail */

/*
 * The way out when nothing else is at hand: five fingers held on the screen for a second and a
 * half end the run -- control goes back to the loader, as when loader_return_after expires.
 * Only a driver's fingers count, not samples a program writes to the device.
 */
#define EXIT_FINGERS    5
#define EXIT_HOLD_MS    1500

extern void loader_return(void);

static thread_call_t exit_call;
static uint32_t fingers_down;           /* one bit for each finger a driver says is down */

static void
exit_hold_elapsed(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	printf("\n--- %d fingers held on the screen for %d ms: back to the loader ---\n", EXIT_FINGERS, EXIT_HOLD_MS);
	hakios_adreno_stop();
	loader_return();
}

/* With touch_lock held. */
static void
count_fingers(uint32_t type, uint32_t finger)
{
	int before = __builtin_popcount(fingers_down), now;
	uint64_t deadline;

	if (finger >= 32 || exit_call == NULL || (type != HAKIOS_TOUCH_DOWN && type != HAKIOS_TOUCH_UP)) {
		return;
	}
	if (type == HAKIOS_TOUCH_DOWN) {
		fingers_down |= 1u << finger;
	} else {
		fingers_down &= ~(1u << finger);
	}
	now = __builtin_popcount(fingers_down);
	if (now >= EXIT_FINGERS && before < EXIT_FINGERS) {
		clock_interval_to_deadline(EXIT_HOLD_MS, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(exit_call, deadline);
	} else if (now < EXIT_FINGERS && before >= EXIT_FINGERS) {
		thread_call_cancel(exit_call);
	}
}

/*
 * What the fingers' samples were like, said every 8 strokes: the lines of touch_lat=1 are many, and a long run's log keeps only its
 * beginning and its end. From the first move after a finger came down, between moves, and from the last move to its lift.
 */
static struct {
	uint64_t last;
	uint32_t strokes, moves, firsts, slow_moves, longest_move, first_sum, first_longest, lift_sum, lift_longest;
	uint32_t finger;
	bool down, moved;
} touch_feel;

static void
touch_feel_add(uint32_t type, uint32_t finger, uint64_t now)
{
	uint64_t nanos;
	uint32_t ms;

	if (type == HAKIOS_TOUCH_DOWN && !touch_feel.down) {
		touch_feel.down = true;
		touch_feel.moved = false;
		touch_feel.finger = finger;
		touch_feel.last = now;
		return;
	}
	if (!touch_feel.down || finger != touch_feel.finger) {
		return;
	}
	absolutetime_to_nanoseconds(now - touch_feel.last, &nanos);
	ms = (uint32_t)(nanos / 1000000);
	touch_feel.last = now;
	if (type == HAKIOS_TOUCH_UP) {
		touch_feel.down = false;
		touch_feel.strokes++;
		touch_feel.lift_sum += ms;
		touch_feel.lift_longest = ms > touch_feel.lift_longest ? ms : touch_feel.lift_longest;
		if (touch_feel.strokes % 8 == 0) {
			printf("touch: %u strokes: first move after %u ms on average, %u at most; %u moves, %u of them over 13 ms apart, %u at most; "
			    "the lift %u ms after the last move on average, %u at most\n", touch_feel.strokes,
			    touch_feel.firsts != 0 ? touch_feel.first_sum / touch_feel.firsts : 0, touch_feel.first_longest, touch_feel.moves,
			    touch_feel.slow_moves, touch_feel.longest_move, touch_feel.lift_sum / touch_feel.strokes, touch_feel.lift_longest);
		}
	} else if (!touch_feel.moved) {
		touch_feel.moved = true;
		touch_feel.firsts++;
		touch_feel.first_sum += ms;
		touch_feel.first_longest = ms > touch_feel.first_longest ? ms : touch_feel.first_longest;
	} else {
		touch_feel.moves++;
		touch_feel.slow_moves += ms > 13;
		touch_feel.longest_move = ms > touch_feel.longest_move ? ms : touch_feel.longest_move;
	}
}

/*
 * Auto-Lock, done here (9 November: SpringBoard's own never fires on this phone -- its timeouts are to come from the
 * AttentionAwareness service in backboardd, which is connected to and never sends them). SpringBoard's library
 * (userland/src/inject_sb.c) writes the timeout its idle timer has now into kern.hakios.autolock: the setting's, the
 * lock screen's shorter one, 0 while something holds the screen on (a video) or Auto-Lock is Never. When no touch and
 * no key has come from a driver for that long and the screen is on, the power key is pressed as the lock button's own
 * press is posted -- what a hand would do. The screen's state is the display driver's (hakios_input_screen()).
 */
static uint64_t last_activity;          /* mach_absolute_time() of the last touch or key from a driver */
static volatile bool screen_on = true;
static int autolock_seconds;
static thread_call_t autolock_call;

SYSCTL_DECL(_kern_hakios);
SYSCTL_INT(_kern_hakios, OID_AUTO, autolock, CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, &autolock_seconds, 0,
    "lock the screen after this many seconds without a touch or a key (0: never); SpringBoard's library keeps it");

static void touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, bool from_driver);

void
hakios_input_screen(bool on)
{
	if (on && !screen_on) {
		last_activity = mach_absolute_time();   /* (a screen just woken is not idle) */
	}
	screen_on = on;
}

static void
autolock_check(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	static bool pressed;
	uint64_t now = mach_absolute_time(), idle_ns, deadline;
	int seconds = autolock_seconds;

	absolutetime_to_nanoseconds(now - last_activity, &idle_ns);
	if (!screen_on) {
		pressed = false;
	} else if (seconds > 0 && !pressed && idle_ns >= (uint64_t)seconds * NSEC_PER_SEC) {
		pressed = true;                 /* (once: until the screen has been off) */
		char line[128];

		snprintf(line, sizeof(line), "hakios: autolock: %llu s without a touch or a key (Auto-Lock %d s): the power key is pressed\n",
		    idle_ns / NSEC_PER_SEC, seconds);
		loader_log_line(line);          /* (the loader's log: a thread call's printf did not reach it, run 211052) */
		printf("%s", line);
		touch_post(HAKIOS_BUTTON_DOWN, HAKIOS_BUTTON_LOCK, 0, 0, false);
		delay_for_interval(120, NSEC_PER_MSEC);
		touch_post(HAKIOS_BUTTON_UP, HAKIOS_BUTTON_LOCK, 0, 0, false);
	}
	clock_interval_to_deadline(2, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(autolock_call, deadline);
}

static void
touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, bool from_driver)
{
	struct hakios_touch *t;

	lck_mtx_lock(&touch_lock);
	if (touch_tail - touch_head == TOUCH_QUEUE) {
		touch_head++;                   /* nobody reads: drop the oldest */
	}
	t = &touch_queue[touch_tail++ % TOUCH_QUEUE];
	t->time = mach_absolute_time();
	if (from_driver) {
		touch_feel_add(type, finger, t->time);
		last_activity = t->time;
	}
	if (latency_lines()) {
		uint64_t ns;

		absolutetime_to_nanoseconds(t->time, &ns);
		hakios_tl("tl: p %u %u %u %u at %llu\n", type, finger, x, y, ns / 1000);
	}
	t->type = type;
	t->finger = finger;
	t->x = x > 65535 ? 65535 : x;
	t->y = y > 65535 ? 65535 : y;
	if (from_driver) {
		count_fingers(type, finger);
	}
	lck_mtx_unlock(&touch_lock);
	wakeup(&touch_queue);
}

void
hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	touch_post(type, finger, x, y, true);
}

static int
touch_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
touch_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
touch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_touch t;
	int error = 0, moved = 0;

	/* a buffer smaller than one record can never be served: say so, instead of blocking for data and then returning 0 (end of file) */
	if (uio_resid(uio) == 0) {
		return 0;
	}
	if (uio_resid(uio) < (user_ssize_t)sizeof(t)) {
		return EINVAL;
	}
	lck_mtx_lock(&touch_lock);
	while (touch_head == touch_tail) {
		error = msleep(&touch_queue, &touch_lock, PZERO | PCATCH, "hakios-touch", NULL);
		if (error) {
			lck_mtx_unlock(&touch_lock);
			return error;
		}
	}
	while (touch_head != touch_tail && uio_resid(uio) >= (user_ssize_t)sizeof(t)) {
		t = touch_queue[touch_head++ % TOUCH_QUEUE];
		lck_mtx_unlock(&touch_lock);
		error = uiomove((caddr_t)&t, sizeof(t), uio);
		lck_mtx_lock(&touch_lock);
		if (error) {
			/* the copy-out failed: put the record back at the head so a lost DOWN or UP does not leave a finger stuck (only if the ring still has room for it) */
			if (touch_tail - touch_head < TOUCH_QUEUE) {
				touch_queue[--touch_head % TOUCH_QUEUE] = t;
			}
			break;
		}
		moved++;
	}
	lck_mtx_unlock(&touch_lock);
	return moved ? 0 : error;
}

static int
touch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_touch t;
	int error = 0;

	while (uio_resid(uio) >= (user_ssize_t)sizeof(t)) {
		error = uiomove((caddr_t)&t, sizeof(t), uio);
		if (error) {
			break;
		}
		if (t.type < HAKIOS_TOUCH_DOWN || t.type > HAKIOS_BUTTON_UP) {
			return EINVAL;
		}
		touch_post(t.type, t.finger, t.x, t.y, false);
	}
	return error;
}

static const struct cdevsw touch_cdevsw = {
	.d_open = touch_open, .d_close = touch_close, .d_read = touch_read, .d_write = touch_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

void
hakios_input_init(void)
{
	int major = cdevsw_add(-1, &touch_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-touch") == NULL) {
		printf("hakios-touch: cannot create the device\n");
	}
	exit_call = thread_call_allocate(exit_hold_elapsed, NULL);
	last_activity = mach_absolute_time();
	autolock_call = thread_call_allocate(autolock_check, NULL);
	autolock_check(NULL, NULL);
	hakios_cpufreq_init();
	hakios_virtio_input_init();
	hakios_goodix_init();
	hakios_buttons_init();
	hakios_battery_init();
	hakios_ufs_init();
	hakios_net_init();
	hakios_adreno_init();
	hakios_wifi_init();
	hakios_adsp_init();
	hakios_mpss_init();
}
