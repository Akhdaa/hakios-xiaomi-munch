/* Run B's diagnostics for the Reachability pink (spec metal-ca-reachability-pink, task 9; reachability/runA-analysis.md
 * §5): the pure parts the plug-in's new metal_fps lines and its metal_ca_tint switch are made of. Plain C, no
 * Objective-C, so the host tests can include it (as hm_ca_label.h).
 *
 *   hm_ca_tint_from_bootargs    metal_ca_tint=1 or =2 among the boot arguments, else 0 (today's behaviour)
 *   hm_ca_ycbcr_fourcc          whether an IOSurface pixel format (four characters) is a YCbCr one
 *   hm_ca_planar_unconverted    whether a texture over an IOSurface plane is a planar or YCbCr surface the plug-in
 *                               samples raw (it converts only formats 500 and 540, two planes, plane 0)
 *   hm_ca_rt_note               the render targets drawn into, by kind (format, size, load action, clear colour), for
 *                               the 2 s line of each kind not told before */
#ifndef HM_CA_DIAG_H
#define HM_CA_DIAG_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* the chip the switch and the lines are for (a6xx::kA650ChipId) */
#define HM_CA_DIAG_A650 0x06050002u

/* metal_ca_tint: 0 off (the default: left-out draws are left out, every draw keeps its program), 1 the draws D2 leaves
 * out are drawn in opaque green, 2 also the draws an uber shader's own program draws (waiting for its specialisation,
 * or past it) in opaque cyan. Only the exact words metal_ca_tint=1 and metal_ca_tint=2 count. */
static inline int
hm_ca_tint_from_bootargs(const char *args)
{
	static const char opt[] = "metal_ca_tint=";
	const size_t n = sizeof(opt) - 1;
	int mode = 0;

	for (const char *p = args; p != NULL && *p != '\0';) {
		size_t len;

		while (*p == ' ' || *p == '\t' || *p == '\n') {
			p++;
		}
		len = strcspn(p, " \t\n");
		if (len == n + 1 && memcmp(p, opt, n) == 0 && (p[n] == '1' || p[n] == '2')) {
			mode = p[n] - '0';   /* (the last one given wins, as for a repeated boot argument) */
		} else if (len >= n && memcmp(p, opt, n) == 0) {
			mode = 0;
		}
		p += len;
	}
	return mode;
}

#define HM_CA_FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* CoreVideo's YCbCr pixel formats, bi- and tri-planar and packed: 8-bit 4:2:0, 4:2:2, 4:4:4 video and full range,
 * the 10-bit x4yy/xfyy ones, Apple's private compressed &8v0 family and the packed 2vuy/yuvs/v210 */
static inline int
hm_ca_ycbcr_fourcc(uint32_t v)
{
	static const uint32_t known[] = {
		HM_CA_FOURCC('4', '2', '0', 'v'), HM_CA_FOURCC('4', '2', '0', 'f'), HM_CA_FOURCC('4', '2', '2', 'v'),
		HM_CA_FOURCC('4', '2', '2', 'f'), HM_CA_FOURCC('4', '4', '4', 'v'), HM_CA_FOURCC('4', '4', '4', 'f'),
		HM_CA_FOURCC('x', '4', '2', '0'), HM_CA_FOURCC('x', '4', '2', '2'), HM_CA_FOURCC('x', '4', '4', '4'),
		HM_CA_FOURCC('x', 'f', '2', '0'), HM_CA_FOURCC('x', 'f', '2', '2'), HM_CA_FOURCC('x', 'f', '4', '4'),
		HM_CA_FOURCC('&', '8', 'v', '0'), HM_CA_FOURCC('&', '8', 'f', '0'), HM_CA_FOURCC('&', 'x', 'v', '0'),
		HM_CA_FOURCC('&', 'x', 'f', '0'), HM_CA_FOURCC('y', '4', '2', '0'), HM_CA_FOURCC('f', '4', '2', '0'),
		HM_CA_FOURCC('2', 'v', 'u', 'y'), HM_CA_FOURCC('y', 'u', 'v', 's'), HM_CA_FOURCC('y', 'u', 'v', 'f'),
		HM_CA_FOURCC('v', '2', '1', '0'), HM_CA_FOURCC('a', '2', 'v', 'y'), HM_CA_FOURCC('v', '0', 'a', '8'),
	};

	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		if (known[i] == v) return 1;
	}
	return 0;
}

/* whether the plug-in's YCbCr conversion takes a texture of pixel format FORMAT over plane PLANE of a surface of
 * PLANES planes (HakiosMetalCA.m's condition, the same words) */
static inline int
hm_ca_ycbcr_converted(unsigned format, size_t planes, size_t plane)
{
	return (format == 500 || format == 540) && planes == 2 && plane == 0;
}

/* a texture the conversion does not take over a surface that is planar (more than one plane) or YCbCr by its four
 * characters FOURCC: its texels are the plane's raw bytes (Y, Cb, Cr), which a bright picture turns salmon pink */
static inline int
hm_ca_planar_unconverted(uint32_t fourcc, size_t planes, size_t plane, unsigned format)
{
	return (planes > 1 || hm_ca_ycbcr_fourcc(fourcc)) && !hm_ca_ycbcr_converted(format, planes, plane);
}

/* The render targets passes draw into, by kind: pixel format, size, MTLLoadAction (0 DontCare, 1 Load, 2 Clear) and,
 * for a Clear, the clear colour as four bytes (0..255 each, rounded; a colour outside 0..1 clamped). Each kind is told
 * once (TOLD), the first time it is seen; PASSES counts its passes in the current period. Past HM_CA_RT_MAX kinds the
 * passes are counted in DROPPED. */
#define HM_CA_RT_MAX 192

struct hm_ca_rt_kind {
	unsigned format, width, height, load;
	uint8_t clear[4];
	unsigned passes;
	int told;
};

struct hm_ca_rt_table {
	unsigned count, dropped;
	struct hm_ca_rt_kind e[HM_CA_RT_MAX];
};

static inline uint8_t
hm_ca_rt_byte(double v)
{
	if (!(v > 0.0)) return 0;   /* (NaN too) */
	if (v >= 1.0) return 255;
	return (uint8_t)(v * 255.0 + 0.5);
}

/* one pass into a target of FORMAT, WIDTH x HEIGHT, LOAD, CLEAR (read only for a Clear): its kind's index, or -1 past
 * the table */
static inline int
hm_ca_rt_note(struct hm_ca_rt_table *t, unsigned format, unsigned width, unsigned height, unsigned load, const double clear[4])
{
	uint8_t c[4] = {0, 0, 0, 0};
	unsigned k;

	if (load == 2 && clear != NULL) {
		for (int i = 0; i < 4; i++) c[i] = hm_ca_rt_byte(clear[i]);
	}
	for (k = 0; k < t->count; k++) {
		const struct hm_ca_rt_kind *e = &t->e[k];

		if (e->format == format && e->width == width && e->height == height && e->load == load && memcmp(e->clear, c, 4) == 0) break;
	}
	if (k == t->count) {
		if (k == HM_CA_RT_MAX) {
			t->dropped++;
			return -1;
		}
		t->e[k].format = format, t->e[k].width = width, t->e[k].height = height, t->e[k].load = load;
		memcpy(t->e[k].clear, c, 4);
		t->e[k].passes = 0;
		t->e[k].told = 0;
		t->count++;
	}
	t->e[k].passes++;
	return (int)k;
}

#endif
