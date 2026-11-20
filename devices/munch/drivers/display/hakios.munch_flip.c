/*
 * The POCO F4's display engine as Mu-Silicium leaves it, read only: step 1 of page flips without a CPU copy (the
 * Redmi K40's way, xiongnemo/hakios-alioth bsd/dev/alioth_dsi.c: the frame drawn into another buffer, or CoreAnimation's
 * surface itself, and the source pipe pointed at it). Nothing is written here.
 *
 *   munch_flip=1   once, 5 s after the start: every source pipe in use (size, place, address, stride, format), the
 *                  control paths (layers, flush, top), the layer mixers, VIG0's scaler version, and INTF_1's
 *                  tear-check and auto-refresh (whether frames go out by themselves at each TE)
 *
 * Then the flips themselves (step 2, from alioth_dsi.c's "Page flips" and "Scan-out of a surface", as they are, the
 * register writes only: no DCS, the POCO F4's panel has its own driver): hakios_munch_flip_probe() checks the display
 * engine is as the probe found it on this phone (VIG0 at 0x9c000000, stride 4320, 1080x2400, CTL0 layered, INTF_1
 * auto-refreshing at each TE); hakios_munch_flip_to/_issue/_taken/_wait() point VIG0 at another buffer (SRC0_ADDR, CTL0's
 * VIG0 flush, taken at the next frame); hakios_munch_scan_source() points it at a surface of another size and scales it
 * with VIG0's QSEED3 (v3.0, read here: 0x3000); hakios_munch_flip_misr() is INTF_1's signature of the frame it sends,
 * by which the framebuffer checks the engine really reads its buffer; hakios_munch_display_quiesce() puts VIG0 back as
 * Silicium had it, on the way back to the loader.
 *
 * Registers as alioth_dpu_probe() reads them (kona DPU 6.0, offsets from MDSS 0xae00000: sde_hw_sspp.c, sde_hw_ctl.c,
 * sde_hw_lm.c, sde_hw_intf.c); INTF_1 at 0xae6b800, the one the POCO F4's DSI0 panel is on (MiniIOMobileFramebuffer).
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hakios.version.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void hakios_tl(const char *format, ...) __printflike(1, 2);
extern kern_return_t thread_terminate(thread_t thread);

#define MDSS_PHYS               0x0ae00000UL
#define INTF1_PHYS              0x0ae6b800UL
#define TEAR_SYNC_CONFIG_VSYNC  0x288
#define TEAR_SYNC_CONFIG_HEIGHT 0x28c
#define TEAR_LINE_COUNT         0x2b0
#define TEAR_AUTOREFRESH_CONFIG 0x2b4
#define INTF_FRAME_COUNT        0x0ac
#define INTF_MISR_CTRL          0x180
#define INTF_MISR_SIGNATURE     0x184
#define MISR_ENABLE             (1u << 8)
#define MISR_STATUS_CLEAR       (1u << 10)
#define MISR_INPUT_SEL_DATA     (1u << 24)
#define MISR_FREE_RUN           (1u << 31)
#define FB_PHYS                 0x9c000000u     /* the framebuffer Silicium scans (the probe: VIG0's address) */
#define VIG0_OFF                0x5000
#define SSPP_SRC_SIZE           0x00
#define SSPP_SRC_XY             0x08
#define SSPP_OUT_SIZE           0x0c
#define SSPP_SRC0_ADDR          0x14
#define SSPP_SRC_YSTRIDE0       0x24
#define SSPP_SRC_OP_MODE        0x38
#define SSPP_PE_C0_LR           0x100
#define VIG0_SCALER             (VIG0_OFF + 0xa00)
#define QS_HW_VERSION           0x00
#define QS_OP_MODE              0x04
#define QS_PHASE_STEP_Y_H       0x10
#define QS_PHASE_STEP_Y_V       0x14
#define QS_PHASE_STEP_UV_H      0x18
#define QS_PHASE_STEP_UV_V      0x1c
#define QS_PRELOAD              0x20
#define QS_SRC_SIZE_Y_RGB_A     0x40
#define QS_SRC_SIZE_UV          0x44
#define QS_DST_SIZE             0x48
#define QS_PHASE_INIT_Y_H       0x90
#define CTL0_OFF                0x2000
#define CTL_LAYER0              0x00
#define CTL_FLUSH               0x18
#define CTL_FLUSH_VIG0          (1u << 0)

static const struct { const char *name; uint32_t off; } sspp[] = {
	{ "VIG0", 0x5000 }, { "VIG1", 0x7000 }, { "VIG2", 0x9000 }, { "VIG3", 0xb000 },
	{ "DMA0", 0x25000 }, { "DMA1", 0x27000 }, { "DMA2", 0x29000 }, { "DMA3", 0x2b000 },
};
static const uint32_t ctl[] = { 0x2000, 0x2200, 0x2400, 0x2600, 0x2800, 0x2a00 };
static const uint32_t lm[] = { 0x45000, 0x46000 };

static void
look(void)
{
	vm_offset_t m = ml_io_map(MDSS_PHYS, 0x90000), page = ml_io_map(INTF1_PHYS & ~0x3fffUL, 0x4000);

	if (m == 0 || page == 0) {
		hakios_tl("munch flip: the display engine's registers cannot be mapped\n");
		return;
	}
#define MD(o) (*(volatile uint32_t *)(m + (o)))
#define IF(o) (*(volatile uint32_t *)(page + (INTF1_PHYS & 0x3fffUL) + (o)))
	hakios_tl("munch flip: MDSS hw %#x, MDP top %#x\n", MD(0x0), MD(0x1000));
	for (unsigned i = 0; i < sizeof(sspp) / sizeof(sspp[0]); i++) {
		uint32_t b = sspp[i].off, size = MD(b + 0x00), addr = MD(b + 0x14);

		if (size == 0 && addr == 0) {
			continue;
		}
		hakios_tl("munch flip: %s src %ux%u at %u,%u -> out %ux%u at %u,%u; addr %#x %#x stride %#x %#x format %#x unpack %#x op %#x\n",
		    sspp[i].name, size & 0xffff, size >> 16, MD(b + 0x08) & 0xffff, MD(b + 0x08) >> 16,
		    MD(b + 0x0c) & 0xffff, MD(b + 0x0c) >> 16, MD(b + 0x10) & 0xffff, MD(b + 0x10) >> 16,
		    addr, MD(b + 0x18), MD(b + 0x24), MD(b + 0x28), MD(b + 0x30), MD(b + 0x34), MD(b + 0x38));
	}
	hakios_tl("munch flip: VIG0 scaler hw %#x op %#x\n", MD(0x5000 + 0xa00), MD(0x5000 + 0xa00 + 0x04));
	for (unsigned i = 0; i < sizeof(ctl) / sizeof(ctl[0]); i++) {
		uint32_t b = ctl[i], layer0 = MD(b + 0x00), top = MD(b + 0x14);

		if (layer0 == 0 && top == 0) {
			continue;
		}
		hakios_tl("munch flip: CTL%u layer0 %#x layer1 %#x top %#x flush %#x intf_active %#x\n", i, layer0, MD(b + 0x04), top,
		    MD(b + 0x18), MD(b + 0xf4));
	}
	for (unsigned i = 0; i < sizeof(lm) / sizeof(lm[0]); i++) {
		hakios_tl("munch flip: LM%u op %#x out %ux%u border %#x\n", i, MD(lm[i]), MD(lm[i] + 4) & 0xffff, MD(lm[i] + 4) >> 16,
		    MD(lm[i] + 8));
	}
	hakios_tl("munch flip: INTF_1 frames %u, tear vsync %#x height %#x, line %u, autorefresh %#x (bit 31: frames go out at each TE)\n",
	    IF(INTF_FRAME_COUNT), IF(TEAR_SYNC_CONFIG_VSYNC), IF(TEAR_SYNC_CONFIG_HEIGHT), IF(TEAR_LINE_COUNT) & 0xffff,
	    IF(TEAR_AUTOREFRESH_CONFIG));
#undef MD
#undef IF
}

/* ---- the flips (from xiongnemo/hakios-alioth alioth_dsi.c) ---- */

static vm_offset_t mdss, intf;
static uint32_t flip_latch_us_max, flip_timeouts;
static uint64_t flip_issued;
static bool scan_saved, misr_on;
static uint32_t silicium_src_size, silicium_stride, silicium_qs_op, silicium_pe[9];
static const uint32_t pe_regs[9] = { 0x100, 0x104, 0x108, 0x110, 0x114, 0x118, 0x120, 0x124, 0x128 };

static inline uint32_t md_rd(uint32_t off) { return *(volatile uint32_t *)(mdss + off); }
static inline void
md_wr(uint32_t off, uint32_t v)
{
	__asm__ volatile("dsb sy" ::: "memory");       /* the pixels before the address */
	*(volatile uint32_t *)(mdss + off) = v;
	__asm__ volatile("dsb sy" ::: "memory");
}
static inline uint32_t if_rd(uint32_t off) { return *(volatile uint32_t *)(intf + (INTF1_PHYS & 0x3fffUL) + off); }
static inline void
if_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(intf + (INTF1_PHYS & 0x3fffUL) + off) = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static bool
maps(void)
{
	if (mdss == 0) mdss = ml_io_map(MDSS_PHYS, 0x90000);
	if (intf == 0) intf = ml_io_map(INTF1_PHYS & ~0x3fffUL, 0x4000);
	return mdss != 0 && intf != 0;
}

/* VIG0 as Silicium sets it: `fb_phys`, the panel's size, unscaled. Taken at the next frame. */
static void
scan_restore(uint64_t fb_phys, uint32_t stride, uint32_t width, uint32_t height)
{
	md_wr(VIG0_OFF + SSPP_SRC_SIZE, height << 16 | width);
	md_wr(VIG0_OFF + SSPP_SRC_XY, 0);
	md_wr(VIG0_OFF + SSPP_SRC_YSTRIDE0, stride);
	for (unsigned c = 0; c < 3; c++) {
		md_wr(VIG0_OFF + SSPP_PE_C0_LR + 0x10 * c, 0);
		md_wr(VIG0_OFF + SSPP_PE_C0_LR + 0x10 * c + 4, 0);
		md_wr(VIG0_OFF + SSPP_PE_C0_LR + 0x10 * c + 8, height << 16 | width);
	}
	md_wr(VIG0_SCALER + QS_OP_MODE, 0);
	md_wr(VIG0_OFF + SSPP_SRC0_ADDR, (uint32_t)fb_phys);
	md_wr(CTL0_OFF + CTL_FLUSH, CTL_FLUSH_VIG0);
}

int hakios_munch_flip_wait(uint32_t timeout_us);

/* The pipe, the path and the panel as a flip needs them (as the probe found them); 1 when they are. */
int
hakios_munch_flip_probe(uint64_t fb_phys, uint32_t stride, uint32_t width, uint32_t height)
{
	if (!hakios_soc_is("sm8250") || !maps()) return 0;
	{
		uint32_t at = md_rd(VIG0_OFF + SSPP_SRC0_ADDR), sz = md_rd(VIG0_OFF + SSPP_SRC_SIZE);

		if (at >= (uint32_t)fb_phys && at < (uint32_t)fb_phys + 0x2300000u &&
		    (at != (uint32_t)fb_phys || sz != (height << 16 | width) || (md_rd(VIG0_SCALER + QS_OP_MODE) & 1))) {
			/* a kernel before this one (same loader session) left another buffer or a surface shown */
			hakios_tl("munch flip: an earlier run left VIG0 at %#x (%ux%u): back to Silicium's setting\n", at, sz & 0xffff, sz >> 16);
			scan_restore(fb_phys, stride, width, height);
			flip_issued = mach_absolute_time();
			(void)hakios_munch_flip_wait(100000);
		}
	}
	uint32_t addr = md_rd(VIG0_OFF + SSPP_SRC0_ADDR), ys = md_rd(VIG0_OFF + SSPP_SRC_YSTRIDE0) & 0xffff;
	uint32_t size = md_rd(VIG0_OFF + SSPP_SRC_SIZE), layer = md_rd(CTL0_OFF + CTL_LAYER0), refresh = if_rd(TEAR_AUTOREFRESH_CONFIG);

	hakios_tl("munch flip: VIG0 addr %#x stride %u size %ux%u, CTL0 layer0 %#x, autorefresh %#x\n", addr, ys, size & 0xffff,
	    size >> 16, layer, refresh);
	if (addr != (uint32_t)fb_phys || ys != stride || (size & 0xffff) != width || (size >> 16) != height || (layer & 7) == 0 ||
	    !(refresh & 0x80000000u)) {
		hakios_tl("munch flip: the display engine is not set up as expected: no flips\n");
		return 0;
	}
	return 1;
}

/* Point VIG0 at `phys` and wait until the engine has taken it (the flush bit clears). 1 when it has. */
int
hakios_munch_flip_to(uint64_t phys, uint32_t timeout_us)
{
	uint64_t start = mach_absolute_time(), ns;

	if (mdss == 0) return 0;
	md_wr(VIG0_OFF + SSPP_SRC0_ADDR, (uint32_t)phys);
	md_wr(CTL0_OFF + CTL_FLUSH, CTL_FLUSH_VIG0);
	for (;;) {
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
		if (!(md_rd(CTL0_OFF + CTL_FLUSH) & CTL_FLUSH_VIG0)) {
			if (ns / 1000 > flip_latch_us_max) flip_latch_us_max = (uint32_t)(ns / 1000);
			return 1;
		}
		if (ns / 1000 >= timeout_us) {
			if (flip_timeouts++ < 8) hakios_tl("munch flip: %#llx not taken after %u us\n", phys, timeout_us);
			return 0;
		}
		delay_for_interval(timeout_us - ns / 1000 > 2000 ? 500 : 50, NSEC_PER_USEC);
	}
}

uint32_t hakios_munch_flip_latch_max_us(void) { return flip_latch_us_max; }

/* The same in two halves: point VIG0 at `phys` now, and (before drawing into the old buffer) wait for it. */
void
hakios_munch_flip_issue(uint64_t phys)
{
	if (mdss == 0) return;
	md_wr(VIG0_OFF + SSPP_SRC0_ADDR, (uint32_t)phys);
	md_wr(CTL0_OFF + CTL_FLUSH, CTL_FLUSH_VIG0);
	flip_issued = mach_absolute_time();
}

int
hakios_munch_flip_taken(void)
{
	return mdss != 0 && !(md_rd(CTL0_OFF + CTL_FLUSH) & CTL_FLUSH_VIG0);
}

int
hakios_munch_flip_wait(uint32_t timeout_us)
{
	uint64_t start = mach_absolute_time(), ns;

	if (mdss == 0) return 0;
	for (;;) {
		if (!(md_rd(CTL0_OFF + CTL_FLUSH) & CTL_FLUSH_VIG0)) {
			absolutetime_to_nanoseconds(mach_absolute_time() - flip_issued, &ns);
			if (ns / 1000 > flip_latch_us_max) flip_latch_us_max = (uint32_t)(ns / 1000);
			return 1;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
		if (ns / 1000 >= timeout_us) {
			if (flip_timeouts++ < 8) hakios_tl("munch flip: not taken after %u us more\n", timeout_us);
			return 0;
		}
		delay_for_interval(timeout_us - ns / 1000 > 2000 ? 100 : 50, NSEC_PER_USEC);
	}
}

static void
scan_save(void)
{
	if (scan_saved || mdss == 0) return;
	silicium_src_size = md_rd(VIG0_OFF + SSPP_SRC_SIZE);
	silicium_stride = md_rd(VIG0_OFF + SSPP_SRC_YSTRIDE0);
	silicium_qs_op = md_rd(VIG0_SCALER + QS_OP_MODE);
	for (unsigned i = 0; i < 9; i++) silicium_pe[i] = md_rd(VIG0_OFF + pe_regs[i]);
	scan_saved = true;
}

/*
 * VIG0 to scan `phys` (w x h, `stride` bytes a row) onto the panel's whole size, scaled with QSEED3 when it is not that
 * size (mainline dpu_hw_setup_scaler3's values, as alioth_scan_source()); w = 0: back to Silicium's setting at `phys`.
 * Taken at the next frame, like a flip.
 */
void
hakios_munch_scan_source(uint64_t phys, uint32_t w, uint32_t h, uint32_t stride)
{
	uint32_t out, ow, oh;

	if (mdss == 0) return;
	scan_save();
	out = md_rd(VIG0_OFF + SSPP_OUT_SIZE);
	ow = out & 0xffff;
	oh = out >> 16;
	if (w == 0 || (w == ow && h == oh)) {
		md_wr(VIG0_OFF + SSPP_SRC_SIZE, w == 0 ? silicium_src_size : (h << 16 | w));
		md_wr(VIG0_OFF + SSPP_SRC_YSTRIDE0, w == 0 ? silicium_stride : stride);
		for (unsigned i = 0; i < 9; i++) md_wr(VIG0_OFF + pe_regs[i], silicium_pe[i]);
		md_wr(VIG0_SCALER + QS_OP_MODE, silicium_qs_op);
	} else {
		uint32_t step_x = (uint32_t)(((uint64_t)w << 21) / ow), step_y = (uint32_t)(((uint64_t)h << 21) / oh);

		md_wr(VIG0_OFF + SSPP_SRC_SIZE, h << 16 | w);
		md_wr(VIG0_OFF + SSPP_SRC_XY, 0);
		md_wr(VIG0_OFF + SSPP_SRC_YSTRIDE0, stride);
		for (unsigned c = 0; c < 3; c++) {
			md_wr(VIG0_OFF + SSPP_PE_C0_LR + 0x10 * c, 0);
			md_wr(VIG0_OFF + SSPP_PE_C0_LR + 0x10 * c + 4, 0);
			md_wr(VIG0_OFF + SSPP_PE_C0_LR + 0x10 * c + 8, h << 16 | w);
		}
		for (unsigned k = 0; k < 4; k++) md_wr(VIG0_SCALER + QS_PHASE_INIT_Y_H + 4 * k, 0);
		md_wr(VIG0_SCALER + QS_PHASE_STEP_Y_H, step_x & 0xffffff);
		md_wr(VIG0_SCALER + QS_PHASE_STEP_Y_V, step_y & 0xffffff);
		md_wr(VIG0_SCALER + QS_PHASE_STEP_UV_H, step_x & 0xffffff);
		md_wr(VIG0_SCALER + QS_PHASE_STEP_UV_V, step_y & 0xffffff);
		md_wr(VIG0_SCALER + QS_PRELOAD, 4u | 2u << 8 | 4u << 16 | 2u << 24);
		md_wr(VIG0_SCALER + QS_SRC_SIZE_Y_RGB_A, h << 16 | w);
		md_wr(VIG0_SCALER + QS_SRC_SIZE_UV, h << 16 | w);
		md_wr(VIG0_SCALER + QS_DST_SIZE, oh << 16 | ow);
		md_wr(VIG0_SCALER + QS_OP_MODE, 1u << 0 | 3u << 16 | 1u << 14 | 1u << 31);
	}
	md_wr(VIG0_OFF + SSPP_SRC0_ADDR, (uint32_t)phys);
	md_wr(CTL0_OFF + CTL_FLUSH, CTL_FLUSH_VIG0);
	flip_issued = mach_absolute_time();
}

/* On the way back to the loader (loader_return.c): VIG0 as Silicium had it, so that what the loader draws is seen. */
void
hakios_munch_display_quiesce(void)
{
	if (mdss == 0) return;
	if (scan_saved) {
		scan_restore(FB_PHYS, silicium_stride, silicium_src_size & 0xffff, silicium_src_size >> 16);
	} else if (md_rd(VIG0_OFF + SSPP_SRC0_ADDR) != FB_PHYS) {
		md_wr(VIG0_OFF + SSPP_SRC0_ADDR, FB_PHYS);
		md_wr(CTL0_OFF + CTL_FLUSH, CTL_FLUSH_VIG0);
	}
}

/* INTF_1's signature of the frame it sends, after `frames` more frames have gone out. */
uint32_t
hakios_munch_flip_misr(unsigned frames)
{
	if (intf == 0) return 0;
	if (!misr_on) {
		if_wr(INTF_MISR_CTRL, MISR_STATUS_CLEAR);
		if_wr(INTF_MISR_CTRL, MISR_ENABLE | MISR_FREE_RUN | MISR_INPUT_SEL_DATA | 1);
		misr_on = true;
		frames++;
	}
	for (unsigned i = 0; i < frames; i++) {
		delay_for_interval(20, NSEC_PER_MSEC);  /* a frame at 60 Hz is 16.5 ms */
	}
	return if_rd(INTF_MISR_SIGNATURE);
}

void
hakios_munch_flip_misr_off(void)
{
	if (intf) if_wr(INTF_MISR_CTRL, MISR_STATUS_CLEAR);
	misr_on = false;
}

static void
look_thread(__unused void *arg, __unused wait_result_t wr)
{
	delay_for_interval(5000, NSEC_PER_MSEC);
	look();
	thread_terminate(current_thread());
}

static void
hakios_munch_flip_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("munch_flip", &on, sizeof(on)) || on == 0 || !hakios_soc_is("sm8250")) {
		return;
	}
	if (kernel_thread_start(look_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_munch_flip_init);
