// Run B's diagnostics for the Reachability pink (spec metal-ca-reachability-pink, task 9), the pure parts of
// userland/src/hm_ca_diag.h and the tint programs' bitcode (userland/src/hm_ca_tint_bc.h, hm-ca-tint-bc.sh):
//   - metal_ca_tint=1 / =2 read from the boot arguments, anything else 0 (today's behaviour)
//   - which surface planes the plug-in samples raw although they are planar or YCbCr (only formats 500 and 540 over
//     plane 0 of two are converted)
//   - the render target kinds table of the 2 s line: one kind per format, size, load action and (for a Clear) clear
//     colour, told once, its passes counted per period, past its room counted as dropped
//   - with the compiler (MESA=1): both tint fragment functions link with a vertex stage for the float formats a
//     CoreAnimation target has, and are refused for an integer one (the plug-in then keeps the draw's own way)
//
// SPDX-License-Identifier: MIT
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <tuple>
#include <vector>
#include "../../userland/src/hm_ca_diag.h"
#include "../../userland/src/hm_ca_tint_bc.h"
#include "hakios-metal/hakios_metal.h"
#include "test.hpp"

namespace {
std::vector<uint8_t> readAll(const char* path) {
    std::vector<uint8_t> bytes;
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return bytes;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    fclose(f);
    return bytes;
}
}  // namespace

TEST(ca_diag_tint_from_bootargs) {
    CHECK_EQ(hm_ca_tint_from_bootargs(nullptr), 0);
    CHECK_EQ(hm_ca_tint_from_bootargs(""), 0);
    CHECK_EQ(hm_ca_tint_from_bootargs("metal=1 metal_fps=1"), 0);
    CHECK_EQ(hm_ca_tint_from_bootargs("metal_ca_tint=1"), 1);
    CHECK_EQ(hm_ca_tint_from_bootargs("metal=1 metal_ca_tint=2 metal_fps=1"), 2);
    CHECK_EQ(hm_ca_tint_from_bootargs("metal_ca_tint=0"), 0);
    CHECK_EQ(hm_ca_tint_from_bootargs("metal_ca_tint=3"), 0);
    CHECK_EQ(hm_ca_tint_from_bootargs("metal_ca_tint=12"), 0);
    CHECK_EQ(hm_ca_tint_from_bootargs("metal_ca_tint="), 0);
    CHECK_EQ(hm_ca_tint_from_bootargs("xmetal_ca_tint=1"), 0);   // a word of its own only
    CHECK_EQ(hm_ca_tint_from_bootargs("metal_ca_tint=1\tmetal_ca_tint=2"), 2);   // the last one wins
    CHECK_EQ(hm_ca_tint_from_bootargs("metal_ca_tint=2 metal_ca_tint=0"), 0);
}

TEST(ca_diag_planar_surfaces_sampled_raw) {
    const uint32_t f420 = HM_CA_FOURCC('4', '2', '0', 'f'), v420 = HM_CA_FOURCC('4', '2', '0', 'v');
    const uint32_t x420 = HM_CA_FOURCC('x', '4', '2', '0'), a8v0 = HM_CA_FOURCC('&', '8', 'v', '0');
    const uint32_t bgra = HM_CA_FOURCC('B', 'G', 'R', 'A'), l10r = HM_CA_FOURCC('l', '1', '0', 'r');
    CHECK(hm_ca_ycbcr_fourcc(f420) && hm_ca_ycbcr_fourcc(v420) && hm_ca_ycbcr_fourcc(x420) && hm_ca_ycbcr_fourcc(a8v0));
    CHECK(!hm_ca_ycbcr_fourcc(bgra) && !hm_ca_ycbcr_fourcc(l10r) && !hm_ca_ycbcr_fourcc(0));
    // what the plug-in converts: formats 500 and 540 over plane 0 of two
    CHECK(!hm_ca_planar_unconverted(f420, 2, 0, 500));
    CHECK(!hm_ca_planar_unconverted(f420, 2, 0, 540));
    CHECK(!hm_ca_planar_unconverted(a8v0, 2, 0, 540));
    // raw: a plain format over a YCbCr plane, the second plane, three planes, a one-plane YCbCr surface, another format
    CHECK(hm_ca_planar_unconverted(f420, 2, 0, 10));    // R8Unorm over the luma plane
    CHECK(hm_ca_planar_unconverted(f420, 2, 1, 30));    // RG8Unorm over the chroma plane
    CHECK(hm_ca_planar_unconverted(f420, 2, 1, 500));   // the conversion takes plane 0 only
    CHECK(hm_ca_planar_unconverted(x420, 2, 0, 501));
    CHECK(hm_ca_planar_unconverted(bgra, 3, 0, 500));   // three planes, whatever the characters
    CHECK(hm_ca_planar_unconverted(HM_CA_FOURCC('2', 'v', 'u', 'y'), 0, 0, 80));
    // not planar, not YCbCr: an ordinary surface is not told, whatever the format
    CHECK(!hm_ca_planar_unconverted(bgra, 0, 0, 80));
    CHECK(!hm_ca_planar_unconverted(bgra, 1, 0, 80));
    CHECK(!hm_ca_planar_unconverted(l10r, 1, 0, 94));
}

TEST(ca_diag_render_target_kinds) {
    static hm_ca_rt_table t;   // (192 kinds: static, not on the stack)
    memset(&t, 0, sizeof t);
    const double pink[4] = {1.0, 0.5, 0.5, 1.0}, other[4] = {0.0, 0.0, 0.0, 0.0}, wide[4] = {-1.0, 2.0, 0.25, 1.0};
    CHECK_EQ(hm_ca_rt_note(&t, 80, 1170, 2532, 1, nullptr), 0);
    CHECK_EQ(hm_ca_rt_note(&t, 80, 1170, 2532, 1, pink), 0);   // the colour is read for a Clear only
    CHECK_EQ(hm_ca_rt_note(&t, 80, 1170, 2532, 2, pink), 1);
    CHECK_EQ(hm_ca_rt_note(&t, 80, 1170, 2532, 2, other), 2);
    CHECK_EQ(hm_ca_rt_note(&t, 115, 1170, 2532, 2, pink), 3);
    CHECK_EQ(hm_ca_rt_note(&t, 80, 1127, 2438, 0, nullptr), 4);
    CHECK_EQ(hm_ca_rt_note(&t, 80, 1170, 2532, 2, pink), 1);
    CHECK_EQ(hm_ca_rt_note(&t, 43, 8, 8, 2, wide), 5);
    CHECK_EQ(t.count, 6u);
    CHECK_EQ(t.e[0].passes, 2u);
    CHECK_EQ(t.e[1].passes, 2u);
    CHECK(t.e[1].clear[0] == 255 && t.e[1].clear[1] == 128 && t.e[1].clear[2] == 128 && t.e[1].clear[3] == 255);
    CHECK(t.e[5].clear[0] == 0 && t.e[5].clear[1] == 255 && t.e[5].clear[2] == 64 && t.e[5].clear[3] == 255);   // clamped
    CHECK(t.e[0].clear[0] == 0 && t.e[0].clear[3] == 0);
    CHECK_EQ(t.e[0].told, 0);
    // past the room: counted, not kept
    for (unsigned i = 0; i < 2 * HM_CA_RT_MAX; i++) hm_ca_rt_note(&t, 70, 16 + i, 16, 1, nullptr);
    CHECK_EQ(t.count, (unsigned)HM_CA_RT_MAX);
    CHECK_EQ(t.dropped, 2u * HM_CA_RT_MAX - (HM_CA_RT_MAX - 6));
    CHECK_EQ(hm_ca_rt_note(&t, 80, 1170, 2532, 1, nullptr), 0);   // a kind already kept is still counted
}

// Property: for any sequence of passes, the table keeps one entry per distinct kind (format, size, load and, for a
// Clear, the colour's bytes) up to its room, in first-seen order, and every pass is counted once, kept or dropped.
TEST(ca_diag_property_render_target_kinds_count_every_pass) {
    std::mt19937 rng(20261118);
    static hm_ca_rt_table t;
    for (int round = 0; round < 200; round++) {
        memset(&t, 0, sizeof t);
        std::map<std::tuple<unsigned, unsigned, unsigned, unsigned, uint32_t>, unsigned> passes;
        std::vector<std::tuple<unsigned, unsigned, unsigned, unsigned, uint32_t>> order;
        const unsigned n = 1 + rng() % 600, sizes = 1 + rng() % 120;
        unsigned dropped = 0;
        for (unsigned i = 0; i < n; i++) {
            const unsigned formats[] = {70, 80, 81, 115, 43, 10, 30};
            const unsigned format = formats[rng() % 7], w = 16 + rng() % sizes, h = 16 + rng() % 3, load = rng() % 3;
            const double levels[] = {0.0, 0.5, 1.0};
            const double c[4] = {levels[rng() % 3], levels[rng() % 3], 0.0, 1.0};
            uint32_t cb = 0;
            if (load == 2) cb = (uint32_t)hm_ca_rt_byte(c[0]) << 24 | (uint32_t)hm_ca_rt_byte(c[1]) << 16 | 0xffu;
            auto key = std::make_tuple(format, w, h, load, cb);
            const bool known = passes.count(key) != 0;
            if (!known && order.size() == HM_CA_RT_MAX) {
                dropped++;
                CHECK_EQ(hm_ca_rt_note(&t, format, w, h, load, c), -1);
                continue;
            }
            if (!known) order.push_back(key);
            passes[key]++;
            const int k = hm_ca_rt_note(&t, format, w, h, load, c);
            CHECK(k >= 0 && (size_t)k < order.size() && order[k] == key);
        }
        CHECK_EQ(t.count, (unsigned)order.size());
        CHECK_EQ(t.dropped, dropped);
        unsigned counted = 0;
        for (unsigned k = 0; k < t.count; k++) {
            CHECK_EQ(t.e[k].passes, passes[order[k]]);
            counted += t.e[k].passes;
        }
        CHECK_EQ(counted + t.dropped, n);
    }
}

TEST(ca_diag_tint_programs_link) {
    CHECK(sizeof(hm_ca_tint_green_fs_bc) > 64 && sizeof(hm_ca_tint_cyan_fs_bc) > 64);
    CHECK(memcmp(hm_ca_tint_green_fs_bc, hm_ca_tint_cyan_fs_bc, 4) == 0);   // (both one wrapper's bitcode)
    if (!hakios_metal_has_compiler()) {
        std::printf("       (no compiler in this build: the tint programs were not linked)\n");
        return;
    }
    std::vector<uint8_t> vs = readAll("out/host/kernels/tri_flat_vs.bc");
    CHECK(!vs.empty());
    if (vs.empty()) return;
    const struct {
        const unsigned char* bc;
        size_t size;
        const char* name;
    } tints[] = {{hm_ca_tint_green_fs_bc, sizeof hm_ca_tint_green_fs_bc, "tri_flat_vs+hm_ca_tint_green_fs"},
                 {hm_ca_tint_cyan_fs_bc, sizeof hm_ca_tint_cyan_fs_bc, "tri_flat_vs+hm_ca_tint_cyan_fs"}};
    for (const auto& tint : tints) {
        // RGBA8Unorm, BGRA8Unorm, its sRGB form, RGBA16Float, BGR5A1Unorm, R8Unorm: the targets CoreAnimation draws into
        for (unsigned format : {70u, 80u, 81u, 115u, 43u, 10u}) {
            hakios_metal_program* p =
                hakios_metal_program_new_bitcode_constants(vs.data(), vs.size(), nullptr, 0, tint.bc, tint.size, nullptr, 0, format);
            CHECK(p != nullptr);
            if (p == nullptr) {
                std::printf("       %s for format %u: %s\n", tint.name, format, hakios_metal_error());
                continue;
            }
            CHECK(strcmp(hakios_metal_program_name(p), tint.name) == 0);
            CHECK_EQ(hakios_metal_program_resource_count(p, 1), 0u);   // it reads nothing the draw binds
            hakios_metal_program_free(p);
        }
        // an integer target: refused, and the plug-in keeps the draw's own way
        CHECK(hakios_metal_program_new_bitcode_constants(vs.data(), vs.size(), nullptr, 0, tint.bc, tint.size, nullptr, 0, 53) == nullptr);
    }
}
