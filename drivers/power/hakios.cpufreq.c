/*
 * The clock speed of the processor cores the kernel runs on, on the POCO X5 Pro (redwood).
 * Only with boot-arg cpufreq=1: the addresses are this phone's.
 *
 * The boot loader leaves each group of cores at a modest frequency and expects the operating
 * system to ask for more. On this SoC that is one register write: the hardware ("EPSS") has
 * a table of frequencies per group and switches the clock source and the supply voltage by
 * itself when told which row is wanted. There is no reason for us ever to want less than the
 * top row, for every group that has a core of ours in it (one core, unless boot-arg cpus=N
 * says more).
 *
 * Register layout: Linux drivers/cpufreq/qcom-cpufreq-hw.c ("epss" variant).
 *
 * The POCO F4 (munch, hw=munch) has the same block at the same address (dt qcom,cpufreq-hw-epss,
 * a 4-byte row as here) and the same groups: cores 0-3 Cortex-A55s, 4-6 Cortex-A77s, 7 the prime
 * A77. Its prime core is not raised to its top row (3187 MHz) but to the highest at or under
 * 2800 MHz: boot-arg cpufreq_prime_mhz=N for another limit, 0 for none. It goes back to the highest at
 * or under 2400 MHz (2361.6), what it ran at before, while bsd/dev/hakios_thermal.c finds the phone
 * hot (hakios_cpufreq_hot()). Its memory bandwidth vote is off unless ddr=2 asks for it. There, the rows are not
 * kept at the top: the governor below (cpugov) moves each group between its lowest row and that limit with the load.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/locks.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/hakios.version.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern void hakios_tl(const char *format, ...) __printflike(1, 2);
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);
extern lck_mtx_t hakios_rsc_lock;      /* RPMh's sequencer, shared with the supplies' and the touch screen's drivers */

/*
 * The loader's log as well: this runs early, and the kernel's own log of a run keeps only its end. With
 * interrupts off -- on the way back to the loader (hakios_ddr_quiesce()) -- the loader's log only: the
 * kernel's console may not be written there, and printf was a panic ("Console I/O from interrupt-disabled
 * context") at the end of every run once the memory vote was taken away on the way back.
 */
static void
tell(const char *format, ...) __printflike(1, 2);

static void
tell(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

#define EPSS_PHYS               0x18591000UL    /* three groups ("domains") of 0x1000: small cores, big, biggest */
#define EPSS_DOMAIN_SIZE        0x1000UL
#define EPSS_DOMAINS            3
#define EPSS_FREQ_TABLE         0x100           /* a word per row: [31:30] clock source, [7:0] multiple of 19.2 MHz */
#define EPSS_TABLE_ROWS         40
#define EPSS_PERF_STATE         0x320           /* the row that is wanted */
#define XO_KHZ                  19200
#define ALTERNATE_KHZ           300000          /* source 0: a fixed 300 MHz */

/* How fast a simple loop runs, in thousands of iterations per millisecond. */
static uint32_t
loop_speed(void)
{
	volatile uint64_t sink = 0;
	uint64_t start = mach_absolute_time(), nanos;

	for (uint32_t i = 0; i < 2000000; i++) {
		sink += i;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	return nanos ? (uint32_t)(2000000ULL * 1000 / nanos) : 0;
}

/* The group of the core with that number in its MPIDR (the device tree's "reg": the number is the second field). */
static uint32_t
domain_of(uint32_t phys_id)
{
	uint32_t core = (phys_id >> 8) & 0xff;

	return core < 4 ? 0 : core < 7 ? 1 : 2;
}

/* The most a group is raised to, in kHz; 0: its top row. */
static uint32_t domain_cap_khz[EPSS_DOMAINS];
#define PRIME_HOT_KHZ           2400000         /* the most the prime core runs at while the phone is hot */
#define PRIME_DOMAIN            2
static vm_offset_t prime_base;
static uint32_t prime_row, prime_hot_row, prime_khz, prime_hot_khz;

/*
 * The governor (boot-arg cpugov; on by default on munch, cpugov=0 keeps every group at its top row as
 * before), made to do what the E404 kernel's configuration does on this phone (schedutil, the device
 * tree's qcom,cpufreq-table-N, no effcpu, no input boost):
 *
 *   the rows     only those whose frequency is in the device tree's qcom,cpufreq-table-N for the group
 *                (qcom-cpufreq-hw.c invalidates the others): small 883.2 to 1804.8 MHz, big 825.6 to 2419.2,
 *                prime 844.8 to 3187.2 -- under the cap (cpufreq_prime_mhz=, 2800) and the prime core's hot
 *                row while hakios_thermal.c finds the phone hot
 *   the load     each core's busy share every GOV_EVERY_MS, made frequency-invariant (times the row's speed
 *                over the group's top speed, as the scheduler's capacity is); per group the busiest core's,
 *                kept as PELT keeps a utilisation: an average that halves in GOV_HALF_LIFE_MS, but never below
 *                what was just seen (util_est: up at once, down at PELT's pace)
 *   the speed    schedutil's get_next_freq(): 1.25 x the top speed x the utilisation, the lowest allowed row at
 *                or above it; every look may change it (E404's rate_limit_us is 2000, under one look)
 */
#define GOV_EVERY_MS            10
#define GOV_HALF_LIFE_MS        32
#define GOV_DECAY_256           206             /* 256 x 0.5^(10/32) */
#define GOV_HEADROOM            125             /* percent */
#define GOV_NOTE_SECONDS        30
static bool gov_on;
static vm_offset_t gov_base[EPSS_DOMAINS];
static uint32_t gov_khz[EPSS_DOMAINS][EPSS_TABLE_ROWS], gov_now[EPSS_DOMAINS], gov_cores[EPSS_DOMAINS], gov_top_khz[EPSS_DOMAINS];
static bool gov_allowed[EPSS_DOMAINS][EPSS_TABLE_ROWS];
static volatile uint32_t gov_cap[EPSS_DOMAINS];        /* the highest row allowed */
/*
 * While the panel sleeps (the screen blanked: hakios_cpufreq_screen()), no group above these, as the POCO F4 port of
 * danya2271/ios-munch caps them (devices/poco-f4/POWER-MANAGEMENT.md): what runs then is background work.
 */
static const uint32_t gov_screen_off_khz[EPSS_DOMAINS] = { 1171200, 1382400, 1401600 };
static volatile bool gov_screen_off;

/* munch's dt cpufreq-hw node: qcom,cpufreq-table-0..2 (kHz), the frequencies E404 lets each group run at */
static const uint32_t munch_table0[] = { 883200, 979200, 1075200, 1171200, 1248000, 1344000, 1420800, 1516800, 1612800,
	                                  1708800, 1804800 };
static const uint32_t munch_table1[] = { 825600, 940800, 1056000, 1171200, 1286400, 1382400, 1478400, 1574400, 1670400,
	                                  1766400, 1862400, 1958400, 2054400, 2150400, 2246400, 2342400, 2419200 };
static const uint32_t munch_table2[] = { 844800, 960000, 1075200, 1190400, 1305600, 1401600, 1516800, 1632000, 1747200,
	                                  1862400, 1977600, 2073600, 2169600, 2265600, 2361600, 2457600, 2553600, 2649600,
	                                  2745600, 2841600, 3187200 };

static bool
gov_in_table(uint32_t domain, uint32_t khz)
{
	const uint32_t *t = domain == 0 ? munch_table0 : domain == 1 ? munch_table1 : munch_table2;
	unsigned n = domain == 0 ? sizeof(munch_table0) / 4 : domain == 1 ? sizeof(munch_table1) / 4 : sizeof(munch_table2) / 4;

	if (!hakios_soc_is("sm8250")) {
		return true;
	}
	for (unsigned i = 0; i < n; i++) {
		if (t[i] == khz) {
			return true;
		}
	}
	return false;
}

static void
raise_domain(uint32_t domain, uint32_t cores)
{
	uint32_t top = 0, top_khz = 0, was;
	vm_offset_t base;

	base = ml_io_map(EPSS_PHYS + domain * EPSS_DOMAIN_SIZE, EPSS_DOMAIN_SIZE);
	for (uint32_t row = 0; row < EPSS_TABLE_ROWS; row++) {
		uint32_t entry = *(volatile uint32_t *)(base + EPSS_FREQ_TABLE + 4 * row);
		uint32_t khz = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;

		if (row > 0 && khz <= top_khz) {
			break;                  /* the table ends by repeating its last row */
		}
		gov_top_khz[domain] = khz;      /* (the group's top speed, cap or not: the scale of its utilisation) */
		if (row > 0 && domain_cap_khz[domain] != 0 && khz > domain_cap_khz[domain]) {
			break;                  /* (the rows go up: the ones after this are faster still) */
		}
		top = row;
		top_khz = khz;
		gov_khz[domain][row] = khz;
		gov_allowed[domain][row] = gov_in_table(domain, khz);
		if (domain == PRIME_DOMAIN && khz <= PRIME_HOT_KHZ) {
			prime_hot_row = row;
			prime_hot_khz = khz;
		}
	}
	if (domain == PRIME_DOMAIN) {
		prime_base = base;
		prime_row = top;
		prime_khz = top_khz;
	}
	gov_base[domain] = base;
	gov_cores[domain] = cores;
	gov_cap[domain] = top;
	gov_now[domain] = top;
	was = *(volatile uint32_t *)(base + EPSS_PERF_STATE);
	*(volatile uint32_t *)(base + EPSS_PERF_STATE) = top;
	delay_for_interval(20, NSEC_PER_MSEC);
	tell("cpufreq: group %u (%u of our cores): row %u -> %u of the frequency table, %u MHz%s\n", domain, cores, was, top,
	    top_khz / 1000, domain_cap_khz[domain] != 0 ? " (limited)" : "");
}

static void
gov_set(uint32_t domain, uint32_t row)
{
	if (row != gov_now[domain]) {
		gov_now[domain] = row;
		*(volatile uint32_t *)(gov_base[domain] + EPSS_PERF_STATE) = row;
	}
}

static void
gov_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	static const char *const names[EPSS_DOMAINS] = { "small", "big", "prime" };
	uint64_t idle_before[MAX_CPUS] = { 0 }, then, last_note, note_every, khz_sum[EPSS_DOMAINS] = { 0 }, samples = 0;
	uint32_t changes = 0, at_top[EPSS_DOMAINS] = { 0 }, util[EPSS_DOMAINS] = { 0 };      /* utilisation in 1/1024 of the top */

	nanoseconds_to_absolutetime((uint64_t)GOV_NOTE_SECONDS * NSEC_PER_SEC, &note_every);
	for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
		(void)ml_cpu_idle_time((int)i, &idle_before[i]);
	}
	then = last_note = mach_absolute_time();
	tell("cpugov: schedutil as E404 has it: every %u ms, the device tree's cpufreq tables, 1.25 x the utilisation (half-life "
	    "%u ms); cpugov=0: always the top row\n", GOV_EVERY_MS, GOV_HALF_LIFE_MS);
	for (;;) {
		uint32_t seen[EPSS_DOMAINS] = { 0 };
		uint64_t now;

		delay_for_interval(GOV_EVERY_MS, NSEC_PER_MSEC);
		now = mach_absolute_time();
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
			uint64_t idle = 0, busy;
			uint32_t d = domain_of(topology->cpus[i].phys_id), invariant;

			if (now <= then || !ml_cpu_idle_time((int)i, &idle) || gov_top_khz[d] == 0) {
				continue;
			}
			busy = (now - then) > (idle - idle_before[i]) ? (now - then) - (idle - idle_before[i]) : 0;
			idle_before[i] = idle;
			invariant = (uint32_t)(1024 * busy / (now - then) * gov_khz[d][gov_now[d]] / gov_top_khz[d]);
			seen[d] = invariant > seen[d] ? invariant : seen[d];
		}
		then = now;
		for (uint32_t d = 0; d < EPSS_DOMAINS; d++) {
			uint32_t cap = gov_cap[d], want = 0, before = gov_now[d];
			uint64_t need;

			if (gov_cores[d] == 0 || gov_base[d] == 0) {
				continue;
			}
			while (gov_screen_off && cap > 0 && gov_khz[d][cap] > gov_screen_off_khz[d]) {
				cap--;
			}
			util[d] = (util[d] * GOV_DECAY_256 + seen[d] * (256 - GOV_DECAY_256)) / 256;
			util[d] = seen[d] > util[d] ? seen[d] : util[d];
			need = (uint64_t)gov_top_khz[d] * util[d] * GOV_HEADROOM / 100 / 1024;
			/* the lowest allowed row at or above it; the highest allowed one under the cap if none is */
			for (want = 0; want < cap && !(gov_allowed[d][want] && gov_khz[d][want] >= need); want++) {
			}
			while (want > 0 && !gov_allowed[d][want]) {
				want--;
			}
			gov_set(d, want);
			changes += gov_now[d] != before;
			khz_sum[d] += gov_khz[d][gov_now[d]];
			at_top[d] += gov_now[d] == cap;
		}
		samples++;
		if (now - last_note >= note_every && samples != 0) {
			char line[200];
			int at = snprintf(line, sizeof(line), "cpugov: the last %u s on average:", GOV_NOTE_SECONDS);

			for (uint32_t d = 0; d < EPSS_DOMAINS; d++) {
				if (gov_cores[d] != 0 && at < (int)sizeof(line) - 40) {
					at += snprintf(line + at, sizeof(line) - at, " %s %llu MHz (%llu %% at the top),", names[d],
					    khz_sum[d] / samples / 1000, 100ull * at_top[d] / samples);
				}
				khz_sum[d] = 0;
				at_top[d] = 0;
			}
			tell("%s %u changes\n", line, changes);
			samples = 0;
			changes = 0;
			last_note = now;
		}
	}
}

/* From the panel's driver (devices/munch/drivers/display/hakios.munch_panel.c): the screen is blanked, or back. */
void
hakios_cpufreq_screen(bool off)
{
	if (gov_on && gov_screen_off != off) {
		gov_screen_off = off;
		tell("cpugov: the screen is %s: %s\n", off ? "off" : "on", off ? "no group above 1171/1382/1401 MHz" : "the caps lifted");
	}
}

/* From bsd/dev/hakios_thermal.c: the prime core down to where it is cool, or back up. */
void
hakios_cpufreq_hot(bool hot)
{
	if (prime_base == 0 || prime_hot_row >= prime_row) {
		return;
	}
	if (gov_on) {
		/* (the governor keeps to it from its next look on, at most GOV_EVERY_MS from now) */
		gov_cap[PRIME_DOMAIN] = hot ? prime_hot_row : prime_row;
		tell("cpufreq: the prime core at most %u MHz (row %u)\n", (hot ? prime_hot_khz : prime_khz) / 1000,
		    hot ? prime_hot_row : prime_row);
		return;
	}
	*(volatile uint32_t *)(prime_base + EPSS_PERF_STATE) = hot ? prime_hot_row : prime_row;
	tell("cpufreq: the prime core at %u MHz (row %u)\n", (hot ? prime_hot_khz : prime_khz) / 1000, hot ? prime_hot_row : prime_row);
}

/*
 * Boot-arg membench=1: how fast this processor reads, writes and copies memory, in MB a second
 * (32 MB at a time, far more than any cache): what the memory's clock allows a processor, which
 * nobody here asks to be raised.
 */
static void
memory_bench(void)
{
	const vm_size_t size = 32u << 20;
	vm_offset_t a = 0, b = 0;
	uint64_t began, ns[3] = { 0, 0, 0 };
	volatile uint64_t sum = 0;

	if (kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		printf("membench: no memory\n");
		return;
	}
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	for (int round = 0; round < 4; round++) {
		uint64_t t;

		began = mach_absolute_time();
		memset((void *)a, round, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[0] += t;
		began = mach_absolute_time();
		for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
			sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[1] += t;
		began = mach_absolute_time();
		memcpy((void *)b, (const void *)a, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[2] += t;
	}
	tell("membench: on core %llu: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n",
	    (__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff, 4ull * (size >> 20) * 1000000000ull / ns[0],
	    4ull * (size >> 20) * 1000000000ull / ns[1], 4ull * (size >> 20) * 1000000000ull / ns[2]);
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

/*
 * Boot-arg membench=2: the same on every core of ours at once (a thread each, started together, 16 MB
 * at a time), 2 s after this starts, so that all the cores are up: the sum of what they get is what
 * the memory gives at the clock it has been left at.
 */
#define BENCH_THREADS   8
static struct {
	volatile uint32_t ready, go, done;
	uint64_t ns[BENCH_THREADS][3];
	uint32_t core[BENCH_THREADS];
} bench;

static void
bench_thread(void *arg, wait_result_t wr __unused)
{
	const vm_size_t size = 16u << 20;
	uintptr_t index = (uintptr_t)arg;
	vm_offset_t a = 0, b = 0;
	volatile uint64_t sum = 0;

	if (kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		os_atomic_inc(&bench.ready, relaxed);
		os_atomic_inc(&bench.done, relaxed);
		return;
	}
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	os_atomic_inc(&bench.ready, relaxed);
	while (!os_atomic_load(&bench.go, acquire)) {
	}
	bench.core[index] = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	for (int round = 0; round < 4; round++) {
		uint64_t began, t;

		began = mach_absolute_time();
		memset((void *)a, round, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][0] += t;
		began = mach_absolute_time();
		for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
			sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][1] += t;
		began = mach_absolute_time();
		memcpy((void *)b, (const void *)a, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][2] += t;
	}
	os_atomic_inc(&bench.done, release);
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

static void
bench_all(void *arg __unused, wait_result_t wr __unused)
{
	const uint32_t threads = MIN(ml_get_topology_info()->num_cpus, BENCH_THREADS);
	const uint64_t mb = 4 * 16;                     /* per thread and test */
	uint64_t total[3] = { 0, 0, 0 };

	delay_for_interval(2000, NSEC_PER_MSEC);
	for (uintptr_t i = 0; i < threads; i++) {
		thread_t thread;

		if (kernel_thread_start(bench_thread, (void *)i, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			os_atomic_inc(&bench.ready, relaxed);
			os_atomic_inc(&bench.done, relaxed);
		}
	}
	while (os_atomic_load(&bench.ready, relaxed) < threads) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	os_atomic_store(&bench.go, 1, release);
	while (os_atomic_load(&bench.done, acquire) < threads) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	for (uint32_t i = 0; i < threads; i++) {
		tell("membench: all at once: core %u: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n", bench.core[i],
		    bench.ns[i][0] ? mb * 1000000000ull / bench.ns[i][0] : 0, bench.ns[i][1] ? mb * 1000000000ull / bench.ns[i][1] : 0,
		    bench.ns[i][2] ? mb * 1000000000ull / bench.ns[i][2] : 0);
		for (int k = 0; k < 3; k++) {
			total[k] += bench.ns[i][k] ? mb * 1000000000ull / bench.ns[i][k] : 0;
		}
	}
	tell("membench: all %u at once, together: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n", threads, total[0],
	    total[1], total[2]);
}

/*
 * Boot-arg cpuload=MS: every MS milliseconds, how busy each of our cores was meanwhile, in
 * percent, to the loader's log (tools/frames.py shows it per gesture):
 *   tl: u at US 7:NN 6:NN ... 1:NN      the core's number (as in its MPIDR), then its share
 * of the time it was not idle (interrupts included).
 */
static uint32_t load_every_ms;

static void
load_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t idle_before[MAX_CPUS] = { 0 }, then = mach_absolute_time();

	for (;;) {
		char line[192];
		int at;
		uint64_t now, us;

		delay_for_interval(load_every_ms, NSEC_PER_MSEC);
		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now, &us);
		at = snprintf(line, sizeof(line), "tl: u at %llu", us / 1000);
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
			uint64_t idle = 0, busy;

			if (!ml_cpu_idle_time((int)i, &idle)) {
				continue;
			}
			busy = (now - then) > (idle - idle_before[i]) ? (now - then) - (idle - idle_before[i]) : 0;
			idle_before[i] = idle;
			if (at < (int)sizeof(line) - 12) {
				at += snprintf(line + at, sizeof(line) - at, " %u:%llu", (topology->cpus[i].phys_id >> 8) & 0xff,
				    100 * busy / (now - then));
			}
		}
		then = now;
		hakios_tl("%s\n", line);
	}
}

static void
load_start(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("cpuload", &load_every_ms, sizeof(load_every_ms)) || load_every_ms == 0) {
		return;
	}
	if (kernel_thread_start(load_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * Boot-arg ddr=1: the memory's bus clock managers ("BCM") in the command database the boot loader
 * left, with what is needed to vote for bandwidth on them -- only read. (membench=2 says the cores
 * together get no more from memory than one alone: 12 GB/s; nobody here votes for more.)
 */
#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define CMD_DB_BCM              5               /* the kind of resource in a slave's id: bus clock managers */

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};
struct cmd_db_bcm {                             /* a BCM's auxiliary data (Linux: struct bcm_db) */
	uint32_t unit;
	uint16_t width;
	uint8_t vcd, reserved;
};

/* (the same condition as the cores' clocks: the addresses are this phone's) */
static bool
cpufreq_wanted(void)
{
	uint32_t on = 0;

	return PE_parse_boot_argn("cpufreq", &on, sizeof(on)) && on != 0;
}

static void
ddr_look(void)
{
	/* (memory, not registers: mapped so that reads of any size and alignment are allowed) */
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		tell("ddr: no command database\n");
		return;
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			struct cmd_db_bcm aux = { 0 };
			char name[9] = { 0 };

			memcpy(name, entry[i].id, 8);
			/* (the data starts after the header, so the header counts: this keeps the read inside the mapped database) */
			if (entry[i].length >= sizeof(aux) &&
			    sizeof(*db) + slave->data_offset + entry[i].offset + sizeof(aux) <= CMD_DB_SIZE) {
				memcpy(&aux, db->data + slave->data_offset + entry[i].offset, sizeof(aux));
			}
			tell("ddr: BCM %-8s at %#x: unit %u, width %u, vcd %u (aux %u bytes)\n", name, entry[i].address, aux.unit,
			    aux.width, aux.vcd, entry[i].length);
		}
	}
}

/*
 * ... and what the boot loader's firmware left in the application processor's RPMh command slots
 * ("DRV 2"; Linux: rpmh-rsc.c): 2 active, 3 sleep, 3 wake slot sets and a control one, of 16 commands
 * each (sc7280's qcom,tcs-config): every command with an address, and which of them go to the BCMs
 * above -- the bandwidth it asked for, and that is in force whenever the processors are awake.
 */
#define RSC_PHYS                0x18220000UL
#define RSC_SIZE                0x10000UL
#define RSC_TCS(n, reg)         (0xd00 + 0x2a0 * (n) + (reg))
#define RSC_TCS_COUNT           9
#define RSC_CMDS                16

static void
ddr_slots(void)
{
	vm_offset_t rsc = ml_io_map(RSC_PHYS, RSC_SIZE);

	for (unsigned t = 0; t < RSC_TCS_COUNT; t++) {
		uint32_t enabled = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x1c));

		for (unsigned c = 0; c < RSC_CMDS; c++) {
			uint32_t msgid = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x30 + 20 * c));
			uint32_t address = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x34 + 20 * c));
			uint32_t data = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x38 + 20 * c));

			if (address == 0 && data == 0) {
				continue;
			}
			tell("ddr: slot set %u (%s) command %u%s: message %#x, address %#x, data %#x%s\n", t,
			    t < 2 ? "active" : t < 5 ? "sleep" : t < 8 ? "wake" : "control", c, enabled & (1u << c) ? " (on)" : "",
			    msgid, address, data, (address >> 16 & 0xf) == CMD_DB_BCM ? " <- a BCM" : "");
		}
	}
}

/*
 * The memory's bandwidth (on by default; boot-arg ddr=0: not): a vote through RPMh -- the most there
 * is -- on the bus clock managers of the memory controller (MC0: the memory's clock) and of the
 * shared hub in front of it with the system cache (SH0), as Linux's interconnect driver casts for
 * a busy GPU (icc-bcm-voter.c: a BCM command is commit, valid, the average and the peak, 14 bits each,
 * in the BCM's units; the BCM takes the highest of its levels the vote reaches), and the memory kept
 * on for this processor (ACV, bit 3, which the boot loader's firmware had set already, the same).
 * All cores together got 12.4 GB/s writing without it (membench=2: what one core gets alone), 14.8
 * with it; frames over 16.7 ms 28% instead of 34-36%. ddr_vote=N: N instead of the most (16383).
 * When the kernel goes back to the loader (hakios_ddr_quiesce(), on every way back, a panic's too)
 * the vote goes down to ddr_rest=N (default 1024). What the firmware had voted cannot be read back,
 * but what the cores get from memory can be measured (membench=2, writing): with any vote from 4
 * to 4096 12.0 to 12.4 GB/s, as before the first vote (12.4); with the most, 15.4; with none at all
 * ("no request", valid clear: what Linux sends for nothing) 0.2 -- the memory at its floor, and the
 * loader's USB transfers at 8 MB/s instead of 30. So the firmware had a vote of its own in our name,
 * of that middle range, and one from the middle of it is left.
 */
#define BCM_COMMIT              (1u << 30)
#define BCM_VALID               (1u << 29)
#define BCM_VOTE(x, y)          (BCM_COMMIT | BCM_VALID | ((uint32_t)(x) & 0x3fff) << 14 | ((uint32_t)(y) & 0x3fff))
#define TCS_REG(reg)            (ddr_rsc + 0xd00 + (reg))       /* command slot set 0, an "active" one */
#define BCM_ACV_APPS            8
#define DDR_REST                1024            /* the vote left for the loader (ddr_rest=N); see hakios_ddr_quiesce() */

static vm_offset_t ddr_rsc;
static uint32_t ddr_mc0, ddr_sh0, ddr_acv;
static bool ddr_voted;

static uint32_t
cmd_db_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

/* Up to three writes through command slot set 0, and the wait until RPMh has done them (as drivers/input/hakios.goodix.c) */
static bool
ddr_rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = *(volatile uint32_t *)TCS_REG(0x00);
	bool done = false;

	for (int i = 0; *(volatile uint32_t *)TCS_REG(0x18) == 0; i++) {
		if (i > 2000) {
			tell("ddr: RPMh: the command slots stay busy\n");
			return false;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 24);
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 16);
	*(volatile uint32_t *)TCS_REG(0x08) = 1;
	for (unsigned i = 0; i < count; i++) {
		*(volatile uint32_t *)TCS_REG(0x30 + 20 * i) = 0x10008 | 0x100;        /* a 4-byte write, with a response */
		*(volatile uint32_t *)TCS_REG(0x34 + 20 * i) = address[i];
		*(volatile uint32_t *)TCS_REG(0x38 + 20 * i) = data[i];
	}
	*(volatile uint32_t *)TCS_REG(0x10) = (1u << count) - 1;
	*(volatile uint32_t *)TCS_REG(0x1c) = (1u << count) - 1;
	*(volatile uint32_t *)TCS_REG(0x00) = irq_enable | 1;
	*(volatile uint32_t *)TCS_REG(0x14) = 1u << 16;
	*(volatile uint32_t *)TCS_REG(0x14) = 1u << 16 | 1u << 24;
	for (int i = 0; i < 2000 && !done; i++) {
		done = (*(volatile uint32_t *)TCS_REG(0x04) & 1) != 0;
		if (!done) {
			delay_for_interval(100, NSEC_PER_USEC);
		}
	}
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 24);
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 16);
	*(volatile uint32_t *)TCS_REG(0x1c) = 0;
	*(volatile uint32_t *)TCS_REG(0x10) = 0;
	*(volatile uint32_t *)TCS_REG(0x08) = 1;
	*(volatile uint32_t *)TCS_REG(0x00) = irq_enable;
	if (!done) {
		tell("ddr: RPMh: no answer to the vote\n");
	}
	return done;
}

static bool
ddr_vote(uint32_t vote)
{
	uint32_t address[3] = { ddr_mc0, ddr_sh0, ddr_acv };
	uint32_t data[3] = { BCM_VOTE(vote, vote), BCM_VOTE(vote, vote), BCM_VOTE(0, BCM_ACV_APPS) };

	return ddr_rpmh_write(address, data, 3);
}

static void
ddr_raise(void)
{
	uint32_t vote = 0x3fff;

	PE_parse_boot_argn("ddr_vote", &vote, sizeof(vote));
	ddr_mc0 = cmd_db_address("MC0");
	ddr_sh0 = cmd_db_address("SH0");
	ddr_acv = cmd_db_address("ACV");
	if (ddr_mc0 == 0 || ddr_sh0 == 0 || ddr_acv == 0 || vote == 0 || vote > 0x3fff) {
		tell("ddr: no vote (MC0 %#x, SH0 %#x, ACV %#x, vote %u)\n", ddr_mc0, ddr_sh0, ddr_acv, vote);
		return;
	}
	ddr_rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	ddr_voted = ddr_vote(vote);
	tell("ddr: memory bandwidth vote %u on MC0 (%#x) and SH0 (%#x)%s\n", vote, ddr_mc0, ddr_sh0, ddr_voted ? "" : ": FAILED");
}

/*
 * Boot-arg ddr=3 (munch): what each vote gives the memory, before a governor for it can be written (E404 lowers the
 * memory's clock with the load; here nobody knows yet which vote is which clock). 20 s after the start, each vote of
 * the list on MC0 and SH0 in turn, 300 ms to settle, then one core writing and reading 16 MB four times: a line each.
 * Then DDR_REST (ddr_rest=N), the middle of the range the firmware had voted in our name (see ddr_raise()), which the
 * way back to the loader leaves too.
 */
static void
ddr_sweep(void *arg __unused, wait_result_t wr __unused)
{
	static const uint32_t votes[] = { 0x3fff, 8192, 4096, 2048, 1024, 512, 256, 128, 64, 16, 4 };
	const vm_size_t size = 16u << 20;
	vm_offset_t a = 0, b = 0;
	uint32_t rest = DDR_REST;
	volatile uint64_t sum = 0;

	delay_for_interval(20, NSEC_PER_SEC);
	ddr_mc0 = cmd_db_address("MC0");
	ddr_sh0 = cmd_db_address("SH0");
	ddr_acv = cmd_db_address("ACV");
	if (ddr_mc0 == 0 || ddr_sh0 == 0 || ddr_acv == 0 ||
	    kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		tell("ddr: sweep: not done (MC0 %#x, SH0 %#x, ACV %#x, or no memory)\n", ddr_mc0, ddr_sh0, ddr_acv);
		return;
	}
	ddr_rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	tell("ddr: sweep: each vote on MC0 (%#x) and SH0 (%#x), then one core writing and reading 16 MB four times\n", ddr_mc0, ddr_sh0);
	for (unsigned v = 0; v < sizeof(votes) / sizeof(votes[0]); v++) {
		uint64_t ns[2] = { 0, 0 };
		bool voted;

		lck_mtx_lock(&hakios_rsc_lock);
		voted = ddr_vote(votes[v]);
		lck_mtx_unlock(&hakios_rsc_lock);

		ddr_voted |= voted;
		delay_for_interval(300, NSEC_PER_MSEC);
		for (int round = 0; round < 4; round++) {
			uint64_t began = mach_absolute_time(), t;

			memset((void *)a, round, size);
			absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
			ns[0] += t;
			began = mach_absolute_time();
			for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
				sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
			}
			absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
			ns[1] += t;
		}
		tell("ddr: sweep: vote %5u%s: writing %llu MB/s, reading %llu MB/s (on core %llu)\n", votes[v], voted ? "" : " (NOT TAKEN)",
		    ns[0] ? 4ull * (size >> 20) * 1000000000ull / ns[0] : 0, ns[1] ? 4ull * (size >> 20) * 1000000000ull / ns[1] : 0,
		    (__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff);
	}
	PE_parse_boot_argn("ddr_rest", &rest, sizeof(rest));
	rest = MIN(rest, 0x3fff);
	lck_mtx_lock(&hakios_rsc_lock);
	bool left = ddr_vote(rest);
	lck_mtx_unlock(&hakios_rsc_lock);
	tell("ddr: sweep: done; the vote left at %u: %s\n", rest, left ? "taken" : "NOT TAKEN");
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

/* On the way back to the loader (osfmk/arm64/loader_return.c: loader_return(), whatever the reason; the other processors are stopped) */
void hakios_ddr_quiesce(void);

void
hakios_ddr_quiesce(void)
{
	uint32_t address[2] = { ddr_mc0, ddr_sh0 };
	uint32_t rest = DDR_REST, data[2];
	bool done;

	if (!ddr_voted) {
		return;
	}
	ddr_voted = false;
	PE_parse_boot_argn("ddr_rest", &rest, sizeof(rest));
	rest = MIN(rest, 0x3fff);
	data[0] = data[1] = rest != 0 ? BCM_VOTE(rest, rest) : BCM_COMMIT;     /* 0: no request */
	done = ddr_rpmh_write(address, data, 2);
	tell("ddr: memory bandwidth vote %u for the loader: %s\n", rest, done ? "done" : "FAILED");
}

void
hakios_cpufreq_init(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t enabled = 0, membench = 0, cores[EPSS_DOMAINS] = { 0 }, before, here;

	load_start();
	enabled = 2;
	if (hakios_soc_is("sm8250")) {
		uint32_t prime_mhz = 2800;

		PE_parse_boot_argn("cpufreq_prime_mhz", &prime_mhz, sizeof(prime_mhz));
		domain_cap_khz[2] = prime_mhz * 1000;
		enabled = 0;                            /* (no memory bandwidth vote unless ddr=2) */
	} else if (!hakios_soc_is("sc7280")) {
		return;                                 /* another device's clocks are elsewhere */
	}
	PE_parse_boot_argn("ddr", &enabled, sizeof(enabled));
	if (enabled == 1) {
		ddr_look();
		ddr_slots();
	} else if (enabled == 2 && cpufreq_wanted()) {
		ddr_raise();
	} else if (enabled == 3) {
		thread_t thread;

		if (kernel_thread_start(ddr_sweep, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
	enabled = 0;
	if (!PE_parse_boot_argn("cpufreq", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		cores[domain_of(topology->cpus[i].phys_id)]++;
	}
	/* (whichever core this thread happens to be on, and stays on or not: it only goes to the log) */
	here = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	before = loop_speed();
	for (uint32_t domain = 0; domain < EPSS_DOMAINS; domain++) {
		if (cores[domain] != 0) {
			raise_domain(domain, cores[domain]);
		}
	}
	printf("cpufreq: on core %u a loop ran at %u, now at %u\n", here, before, loop_speed());
	{
		uint32_t gov = hakios_soc_is("sm8250") ? 1 : 0;
		thread_t thread;

		PE_parse_boot_argn("cpugov", &gov, sizeof(gov));
		gov_on = gov != 0;
		if (gov_on && kernel_thread_start(gov_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			gov_on = false;
		}
	}
	/* membench has its own variable: when the boot-arg is absent, the cpufreq value (2 means on) must not start the benchmark */
	PE_parse_boot_argn("membench", &membench, sizeof(membench));
	if (membench == 1) {
		memory_bench();
	} else if (membench == 2) {
		thread_t thread;

		if (kernel_thread_start(bench_all, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
}
