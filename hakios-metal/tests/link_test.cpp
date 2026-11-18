// render::link over the stage fixtures and over stages built by hand: the numbers ir3_link_shaders would give (worked
// by hand in air-vs-fs.md from the same tables, and with MESA=1 held to Mesa's own function by compiler_test through
// linkProgram), and the sentence each refusal comes with. Every positive check has beside it a mutation the link
// refuses, so that no check passes for want of looking.
//
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/compiler.hpp"
#include "hakios-metal/render.hpp"
#include "fixtures/tri_color_fs_air.hpp"
#include "fixtures/tri_color_vs_air.hpp"
#include "fixtures/tri_flat_fs_air.hpp"
#include "fixtures/tri_flat_vs_air.hpp"
#include "fixtures/var_a_vs_air.hpp"
#include "fixtures/var_ab_vs_air.hpp"
#include "fixtures/var_ac_fs_air.hpp"
#include "fixtures/var_b_fs_air.hpp"
#include "fixtures/var_ba_fs_air.hpp"
#include "test.hpp"

using namespace hakios::metal;
namespace kernels = hakios::metal::kernels;

namespace {

// gl_varying_slot / gl_frag_result numbers as the fixtures carry them (Mesa shader_enums.h:320, 359, 987)
constexpr uint8_t kSlotPos = 0;
constexpr uint8_t kSlotVar0 = 32;
constexpr uint8_t kSlotVar1 = 33;
constexpr uint8_t kFragData0 = 4;
constexpr uint32_t regid(uint32_t n, uint32_t c) { return n * 4 + c; }

void expectRefusal(const std::string& got, const char* want) {
    CHECK(got == want);
    if (got != want) std::fprintf(stderr, "  got      \"%s\"\n  expected \"%s\"\n", got.c_str(), want);
}

bool sameVar(const Linkage::Var& v, uint8_t slot, uint8_t regid_, uint8_t compmask, uint8_t loc) {
    bool same = v.slot == slot && v.regid == regid_ && v.compmask == compmask && v.loc == loc;
    if (!same) std::fprintf(stderr, "  var {%u, 0x%02x, 0x%x, %u}, expected {%u, 0x%02x, 0x%x, %u}\n", v.slot, v.regid, v.compmask, v.loc, slot, regid_, compmask, loc);
    return same;
}

bool allZero(const uint32_t* a, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != 0) return false;
    }
    return true;
}

}  // namespace

// The fixtures themselves say what the listings said (tools/kernels/tri_*.ll, the footers in the fixtures): the shape
// the link reads. Each number here is from the ir3 listing of 29 October, not from the link.
TEST(stage_fixtures_carry_what_the_listings_say) {
    const Stage& fvs = kernels::tri_flat_vs_air;
    CHECK_EQ(fvs.kind, uint8_t(kStageVertex));
    CHECK_EQ(fvs.numInputs, 1u);
    CHECK_EQ(fvs.inputs[0].slot, 9u);        // SYSTEM_VALUE_VERTEX_ID
    CHECK(fvs.inputs[0].sysval && !fvs.inputs[0].bary);
    CHECK_EQ(fvs.inputs[0].regid, regid(0, 0));
    CHECK_EQ(fvs.vertexIdRegid, regid(0, 0));
    CHECK_EQ(fvs.instanceIdRegid, a6xx::kRegidNone);
    CHECK_EQ(fvs.numOutputs, 1u);
    CHECK_EQ(fvs.outputs[0].slot, kSlotPos);
    CHECK_EQ(fvs.positionRegid, uint32_t(fvs.outputs[0].regid));   // r1.z in the listing
    CHECK_EQ(fvs.positionRegid, regid(1, 2));
    CHECK_EQ(fvs.numAttributes, 0u);
    CHECK_EQ(fvs.numColorOutputs, 0u);
    CHECK_EQ(fvs.k.constlen, 8u);            // ir3's gen-6 floor for a VS (ir3_compiler_nir.c:6035-6037)
    // instrlen is ir3's DIV_ROUND_UP(instr_count, 16) over the ENCODED instructions (ir3.c:264; a (rptN) counts once),
    // 16 here with `end`; the binary is padded to at least four nops past them and then to 128 bytes (ir3.c:270,
    // ir3_shader.c:112), which is why the words are two groups long while INSTRLEN says one -- as turnip programs it
    CHECK_EQ(fvs.k.instrlen, 1u);
    CHECK_EQ(fvs.k.numWords, 64u);
    CHECK_EQ(fvs.k.numUboRanges, 0u);
    CHECK_EQ(fvs.k.numSsbos, 0u);
    CHECK_EQ(fvs.k.numUbos, 0u);
    CHECK(fvs.writesPos);

    const Stage& ffs = kernels::tri_flat_fs_air;
    CHECK_EQ(ffs.kind, uint8_t(kStageFragment));
    CHECK_EQ(ffs.totalIn, 0u);
    CHECK_EQ(ffs.numInputs, 1u);             // the unused BARYCENTRIC_PERSP_PIXEL entry at r63.x ir3 always adds
    CHECK_EQ(ffs.inputs[0].slot, 56u);
    CHECK(ffs.inputs[0].sysval && !ffs.inputs[0].bary);
    CHECK_EQ(ffs.inputs[0].regid, a6xx::kRegidNone);
    CHECK_EQ(ffs.ijRegid[0], a6xx::kRegidNone);
    CHECK_EQ(ffs.numColorOutputs, 1u);
    CHECK_EQ(ffs.colorRegid[0], regid(0, 0));
    CHECK_EQ(ffs.outputs[0].slot, kFragData0);
    CHECK_EQ(ffs.outputs[0].pad, 0u);        // the output's class: float
    CHECK_EQ(ffs.k.constlen, 0u);
    CHECK_EQ(ffs.k.instrlen, 1u);
    CHECK_EQ(ffs.k.numImmediates, 0u);       // the colour is four mov immediates, no const
    CHECK(!ffs.writesPos && !ffs.writesSmask && !ffs.writesStencilref && !ffs.hasKill && !ffs.fbRead);
    CHECK_EQ(ffs.numSamplerPrefetch, 0u);

    const Stage& cvs = kernels::tri_color_vs_air;
    CHECK_EQ(cvs.numOutputs, 2u);
    CHECK_EQ(cvs.outputs[0].slot, kSlotPos);
    CHECK_EQ(cvs.outputs[1].slot, kSlotVar0);
    CHECK_EQ(cvs.positionRegid, regid(3, 1));        // r3.y
    CHECK_EQ(cvs.outputs[1].regid, regid(4, 1));     // r4.y
    CHECK_EQ(cvs.k.constlen, 8u);
    CHECK_EQ(cvs.k.instrlen, 2u);            // 31 encoded instructions with `end`: two groups; 96 words with the padding
    CHECK_EQ(cvs.k.numWords, 96u);

    const Stage& cfs = kernels::tri_color_fs_air;
    CHECK_EQ(cfs.totalIn, 4u);
    CHECK_EQ(cfs.numInputs, 2u);
    CHECK_EQ(cfs.inputs[0].slot, kSlotVar0);
    CHECK(cfs.inputs[0].bary && !cfs.inputs[0].sysval && !cfs.inputs[0].flat);
    CHECK_EQ(cfs.inputs[0].compmask, 0xfu);
    CHECK_EQ(cfs.inputs[0].inloc, 0u);
    CHECK_EQ(cfs.inputs[0].regid, a6xx::kRegidNone);        // a bary input has no register of its own: bary.f fetches it
    CHECK_EQ(cfs.inputs[1].slot, 56u);               // SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL at r0.x
    CHECK_EQ(cfs.inputs[1].regid, regid(0, 0));
    CHECK_EQ(cfs.ijRegid[0], regid(0, 0));
    for (uint32_t i = 1; i < kIjCount; i++) CHECK_EQ(cfs.ijRegid[i], a6xx::kRegidNone);
    CHECK_EQ(cfs.colorRegid[0], regid(0, 2));        // r0.z
    CHECK_EQ(cfs.k.constlen, 0u);
}

// pair 1: no varying, the position alone at location 0 (air-vs-fs.md, "Pair 1" worked by hand)
TEST(link_tri_flat_is_the_position_alone) {
    Linkage l{};
    std::string why;
    CHECK(render::link(kernels::tri_flat_vs_air, kernels::tri_flat_fs_air, &l, &why));
    if (!why.empty()) std::printf("       %s\n", why.c_str());
    CHECK_EQ(l.cnt, 1u);
    CHECK_EQ(l.maxLoc, 4u);
    CHECK_EQ(l.positionLoc, 0u);
    CHECK(sameVar(l.var[0], kSlotPos, uint8_t(regid(1, 2)), 0xf, 0));
    CHECK(allZero(l.varmask, 4));           // VPC_VAR_DISABLE = ~0 four times: the position's locations are not varyings
    CHECK(allZero(l.interpMode, 8));
    CHECK(allZero(l.psReplMode, 8));
    CHECK_EQ(l.mrtComponents, 0xfu);
    // the mutations: the same link refused, with the sentence
    Stage vs = kernels::tri_flat_vs_air;
    vs.positionRegid = a6xx::kRegidNone;
    CHECK(!render::link(vs, kernels::tri_flat_fs_air, &l, &why));
    expectRefusal(why, "tri_flat_vs_air writes no position: nothing to rasterize");
    vs = kernels::tri_flat_vs_air;
    vs.positionRegid |= 0x100;              // ir3_get_output_regid's HALF_REG_ID
    CHECK(!render::link(vs, kernels::tri_flat_fs_air, &l, &why));
    expectRefusal(why, "tri_flat_vs_air writes its position in a half register: not served");
    CHECK(!render::link(kernels::tri_flat_fs_air, kernels::tri_flat_fs_air, &l, &why));
    expectRefusal(why, "tri_flat_fs_air is not a vertex stage");
    CHECK(!render::link(kernels::tri_flat_vs_air, kernels::tri_flat_vs_air, &l, &why));
    expectRefusal(why, "tri_flat_vs_air is not a fragment stage");
    // a colour output the FS does not write leaves its nibble clear
    Stage fs = kernels::tri_flat_fs_air;
    fs.colorRegid[0] = a6xx::kRegidNone;
    CHECK(render::link(kernels::tri_flat_vs_air, fs, &l, &why));
    CHECK_EQ(l.mrtComponents, 0u);
}

// pair 2: the colour varying at location 0 (the FS's inloc), the position after it at 4 ("Pair 2" worked by hand)
TEST(link_tri_color_puts_the_varying_first_and_the_position_at_4) {
    Linkage l{};
    std::string why;
    CHECK(render::link(kernels::tri_color_vs_air, kernels::tri_color_fs_air, &l, &why));
    if (!why.empty()) std::printf("       %s\n", why.c_str());
    CHECK_EQ(l.cnt, 2u);
    CHECK_EQ(l.maxLoc, 8u);
    CHECK_EQ(l.positionLoc, 4u);
    CHECK(sameVar(l.var[0], kSlotVar0, uint8_t(regid(4, 1)), 0xf, 0));
    CHECK(sameVar(l.var[1], kSlotPos, uint8_t(regid(3, 1)), 0xf, 4));
    CHECK_EQ(l.varmask[0], 0xfu);           // VPC_VAR_DISABLE[0] = 0xfffffff0
    CHECK_EQ(l.varmask[1], 0u);
    CHECK_EQ(l.varmask[2], 0u);
    CHECK_EQ(l.varmask[3], 0u);
    CHECK(allZero(l.interpMode, 8));        // smooth: INTERP_SMOOTH is 0
    CHECK(allZero(l.psReplMode, 8));
    CHECK_EQ(l.mrtComponents, 0xfu);
    // a flat varying: INTERP_FLAT (1) in each component's two bits at inloc * 2
    Stage fs = kernels::tri_color_fs_air;
    fs.inputs[0].flat = 1;
    CHECK(render::link(kernels::tri_color_vs_air, fs, &l, &why));
    CHECK_EQ(l.interpMode[0], 0x55u);
    CHECK(allZero(l.interpMode + 1, 7));
    // a varying at inloc 8 with two components: the locations move with it, the varmask follows
    fs = kernels::tri_color_fs_air;
    fs.inputs[0].inloc = 8;
    fs.inputs[0].compmask = 0x3;
    fs.inputs[0].flat = 1;
    fs.totalIn = 10;
    CHECK(render::link(kernels::tri_color_vs_air, fs, &l, &why));
    CHECK_EQ(l.cnt, 2u);
    CHECK(sameVar(l.var[0], kSlotVar0, uint8_t(regid(4, 1)), 0x3, 8));
    CHECK_EQ(l.maxLoc, 14u);
    CHECK_EQ(l.positionLoc, 10u);
    CHECK_EQ(l.varmask[0], 0x3u << 8);
    CHECK_EQ(l.interpMode[0], 0x5u << 16);
    // ir3_link_shaders' own rule: an input at or past total_in is a varying ir3 allocated and found unread -- skipped,
    // so the link is the position alone (the mutation that shows totalIn is looked at)
    fs = kernels::tri_color_fs_air;
    fs.totalIn = 0;
    CHECK(render::link(kernels::tri_color_vs_air, fs, &l, &why));
    CHECK_EQ(l.cnt, 1u);
    CHECK_EQ(l.maxLoc, 4u);
    CHECK_EQ(l.positionLoc, 0u);
    // and a bary input with an empty compmask is not a varying either (ir3_next_varying: compmask && bary)
    fs = kernels::tri_color_fs_air;
    fs.inputs[0].compmask = 0;
    CHECK(render::link(kernels::tri_color_vs_air, fs, &l, &why));
    CHECK_EQ(l.cnt, 1u);
}

// an unused varying is fine (the VS writes VAR0, the FS reads nothing); a varying the VS does not write is not
TEST(link_refuses_a_varying_the_vertex_stage_does_not_write) {
    Linkage l{};
    std::string why;
    CHECK(render::link(kernels::tri_color_vs_air, kernels::tri_flat_fs_air, &l, &why));
    CHECK_EQ(l.cnt, 1u);
    CHECK_EQ(l.maxLoc, 4u);
    CHECK(sameVar(l.var[0], kSlotPos, uint8_t(regid(3, 1)), 0xf, 0));
    // the fixtures crossed the other way
    CHECK(!render::link(kernels::tri_flat_vs_air, kernels::tri_color_fs_air, &l, &why));
    expectRefusal(why, "the fragment stage reads varying slot 32 (VAR0), which the vertex stage does not write");
    // built by hand: the FS wants VAR1, the VS writes VAR0
    Stage fs = kernels::tri_color_fs_air;
    fs.inputs[0].slot = kSlotVar1;
    CHECK(!render::link(kernels::tri_color_vs_air, fs, &l, &why));
    expectRefusal(why, "the fragment stage reads varying slot 33 (VAR1), which the vertex stage does not write");
    // and the VS renamed to serve it links again: the refusal was about the slot and nothing else
    Stage vs = kernels::tri_color_vs_air;
    vs.outputs[1].slot = kSlotVar1;
    CHECK(render::link(vs, fs, &l, &why));
    CHECK_EQ(l.cnt, 2u);
    CHECK(sameVar(l.var[0], kSlotVar1, uint8_t(regid(4, 1)), 0xf, 0));
    // a VS with no position at all (its one output renamed away) is refused by the position rule, after the varyings
    vs = kernels::tri_color_vs_air;
    vs.positionRegid = a6xx::kRegidNone;
    CHECK(!render::link(vs, kernels::tri_color_fs_air, &l, &why));
    expectRefusal(why, "tri_color_vs_air writes no position: nothing to rasterize");
}

// the link's own capacity: var[] holds kMaxStageOutputs entries, the position needs one of them
TEST(link_refuses_more_varyings_than_the_table_holds) {
    Stage vs = kernels::tri_color_vs_air;
    Stage fs = kernels::tri_color_fs_air;
    // 32 varyings VAR0..VAR31, one component each at inloc i, every one written by the VS
    vs.numOutputs = kMaxStageOutputs;
    vs.outputs[0] = StageOutput{kSlotPos, uint8_t(regid(3, 1)), 0, 0};
    for (uint32_t i = 1; i < kMaxStageOutputs; i++) vs.outputs[i] = StageOutput{uint8_t(kSlotVar0 + i - 1), uint8_t(i), 0, 0};
    fs.numInputs = kMaxStageOutputs - 1;
    for (uint32_t i = 0; i < fs.numInputs; i++) fs.inputs[i] = StageInput{uint8_t(kSlotVar0 + i), a6xx::kRegidNone, 0x1, uint8_t(i), 0, 1, 0, 0};
    fs.totalIn = fs.numInputs;
    Linkage l{};
    std::string why;
    // 31 varyings + the position = 32: fits exactly
    CHECK(render::link(vs, fs, &l, &why));
    CHECK_EQ(l.cnt, kMaxStageOutputs);
    CHECK_EQ(l.positionLoc, 31u);
    CHECK_EQ(l.maxLoc, 35u);
    // the VS writing VAR31 too and the FS reading it: 32 varyings, no room for the position
    vs.numOutputs = kMaxStageOutputs;
    vs.outputs[0] = StageOutput{uint8_t(kSlotVar0 + 31), 40, 0, 0};   // the position entry replaced; positionRegid still says r3.y
    fs.numInputs = kMaxStageOutputs;
    fs.inputs[31] = StageInput{uint8_t(kSlotVar0 + 31), a6xx::kRegidNone, 0x1, 31, 0, 1, 0, 0};
    fs.totalIn = 32;
    CHECK(!render::link(vs, fs, &l, &why));
    expectRefusal(why, "the link holds 32 entries and has no room left for the position");
}

// --- the A650 link by varying identity (.kiro/specs/metal-var1-linkage-crash-fix, Property 1) -----------------------------
// Metal pairs a vertex output with a fragment input by the string after air.vertex_output / air.fragment_input, not by
// the order the two functions declare them; the front end numbers each stage's varyings VAR0 + n in its own declaration
// order (air_to_nir.c). These tests hold the A650 link to the identity. Before the fix a650Link is render::link alone,
// and they fail: the reorder and subset pairs link with the wrong registers, the unmatched pair is refused at VAR1.
namespace {

// the identities tools/kernels/var_*.ll declare
const std::string kIdA = "generated(1aDv4_f)", kIdB = "generated(1bDv4_f)", kIdC = "generated(1cDv4_f)";

// The A650 link of a VS and an FS with their identity tables (CompiledStage::varyingNames' shape: slot - VAR0 indexes
// them). UNMATCHED gets "VARn 'identity'" for each FS input that no VS output of that identity serves.
// USED, when given, gets the fragment Stage the link was computed over (the name-matched copy after the fix), so that
// the preservation tests can see it left byte-identical where the names agree with the slots.
// Before the fix it was render::link alone, the names unread; the fix (task 4.3) points it at matchVaryingsByName +
// render::link over a copy of FS, as linkProgram does.
bool a650Link(const Stage& vs, const std::vector<std::string>& vsNames, const Stage& fs, const std::vector<std::string>& fsNames, Linkage* l,
              std::string* why, std::vector<std::string>* unmatched, Stage* used = nullptr) {
    unmatched->clear();
    Stage f2 = fs;
    render::matchVaryingsByName(vs, vsNames, &f2, fsNames, unmatched);
    if (used != nullptr) *used = f2;
    return render::link(vs, f2, l, why);
}

uint32_t lastBitOf(uint32_t mask) {
    uint32_t n = 0;
    for (; mask != 0; mask >>= 1) n++;
    return n;
}

// an interpolated user varying: what the name match touches (design.md, userVaryingInputs)
bool isUserVarying(const Stage& fs, const StageInput& in) {
    return in.bary && in.compmask != 0 && in.inloc < fs.totalIn && in.slot >= kSlotVar0 && in.slot < kSlotVar0 + 32;
}

std::string nameAt(const std::vector<std::string>& names, uint32_t slot) {
    uint32_t i = slot - kSlotVar0;
    return slot >= kSlotVar0 && i < names.size() ? names[i] : std::string();
}

int outputBySlot(const Stage& vs, uint32_t slot) {
    for (uint32_t j = 0; j < vs.numOutputs; j++) {
        if (vs.outputs[j].slot == slot) return int(j);
    }
    return -1;
}

// nameLink: the VS output with that identity, -1 for none
int outputByName(const Stage& vs, const std::vector<std::string>& vsNames, const std::string& name) {
    for (uint32_t k = 0; k < vsNames.size(); k++) {
        if (vsNames[k] == name) return outputBySlot(vs, kSlotVar0 + k);
    }
    return -1;
}

// design.md isBugCondition, for two stages known to be the A650's: some user varying whose slot link is none or is not
// its name link (an input or a VS table with no identity stays slot-linked and does not count)
bool isBugCondition(const Stage& vs, const std::vector<std::string>& vsNames, const Stage& fs, const std::vector<std::string>& fsNames) {
    for (const std::string& n : vsNames) {
        if (n.empty()) return false;
    }
    for (uint32_t i = 0; i < fs.numInputs; i++) {
        const StageInput& in = fs.inputs[i];
        if (!isUserVarying(fs, in)) continue;
        std::string name = nameAt(fsNames, in.slot);
        if (name.empty()) continue;
        int s = outputBySlot(vs, in.slot), n = outputByName(vs, vsNames, name);
        if (s < 0 || s != n) return true;
    }
    return false;
}

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%02x", v);
    return b;
}

std::string list(const std::vector<std::string>& v) {
    std::string s = "{";
    for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + v[i];
    return s + "}";
}

// which VS varying a register is, by its identity, for the counterexample
std::string vsVaryingOfRegid(const Stage& vs, const std::vector<std::string>& vsNames, uint32_t regid_) {
    for (uint32_t j = 0; j < vs.numOutputs; j++) {
        if (vs.outputs[j].regid == regid_ && vs.outputs[j].slot >= kSlotVar0) {
            return "VS VAR" + std::to_string(vs.outputs[j].slot - kSlotVar0) + " '" + nameAt(vsNames, vs.outputs[j].slot) + "'";
        }
    }
    return "no VS varying";
}

// Property 1's assertions over one link (design.md, Fix Checking): empty when they hold, else the first that does not.
// UNMATCHED may be null (the note is not visible on the code before the fix).
std::string byIdentity(const Stage& vs, const std::vector<std::string>& vsNames, const Stage& fs, const std::vector<std::string>& fsNames, bool linked,
                       const Linkage& l, const std::string& why, const std::vector<std::string>* unmatched) {
    if (!linked) return "refused: \"" + why + "\"";
    std::vector<std::string> want;
    for (uint32_t i = 0; i < fs.numInputs; i++) {
        const StageInput& in = fs.inputs[i];
        if (!isUserVarying(fs, in)) continue;
        std::string name = nameAt(fsNames, in.slot);
        if (name.empty()) continue;
        std::string what = "FS input VAR" + std::to_string(in.slot - kSlotVar0) + " '" + name + "' at inloc " + std::to_string(in.inloc);
        const Linkage::Var* at = nullptr;
        for (uint32_t j = 0; j < l.cnt && j < kMaxStageOutputs; j++) {
            if (l.var[j].loc == in.inloc) at = &l.var[j];
        }
        int n = outputByName(vs, vsNames, name);
        if (n >= 0) {
            uint32_t wantRegid = vs.outputs[n].regid;
            std::string wantWhat = hex(wantRegid) + " (VS VAR" + std::to_string(vs.outputs[n].slot - kSlotVar0) + ")";
            if (at == nullptr) return what + ": no var[] entry, expected regid " + wantWhat;
            if (at->regid != wantRegid) {
                return what + ": linked to regid " + hex(at->regid) + " (" + vsVaryingOfRegid(vs, vsNames, at->regid) + "), expected " + wantWhat;
            }
            if (at->compmask != in.compmask) return what + ": compmask " + hex(at->compmask) + ", expected " + hex(in.compmask);
        } else {
            want.push_back("VAR" + std::to_string(in.slot - kSlotVar0) + " '" + name + "'");
            if (at != nullptr) {
                return what + ": no VS output has its identity, yet it is linked to regid " + hex(at->regid) + " (" +
                       vsVaryingOfRegid(vs, vsNames, at->regid) + "); expected no vertex register (reads zeros)";
            }
        }
        // the location's components in varmask and under maxLoc, as ir3_link_add sets them (regid(63, 0) included)
        for (uint32_t c = 0; c < lastBitOf(in.compmask); c++) {
            uint32_t loc = in.inloc + c;
            if (((l.varmask[loc / 32] >> (loc % 32)) & 1) == 0) return what + ": varmask bit " + std::to_string(loc) + " clear";
            if (((in.compmask >> c) & 1) == 0) continue;
            uint32_t mode = (l.interpMode[loc / 16] >> ((loc % 16) * 2)) & 3;
            if (mode != (in.flat ? 1u : 0u)) return what + ": interp mode " + std::to_string(mode) + " at location " + std::to_string(loc);
        }
        if (l.maxLoc < in.inloc + lastBitOf(in.compmask)) return what + ": maxLoc " + std::to_string(l.maxLoc) + " short of it";
    }
    if (unmatched != nullptr && *unmatched != want) return "unmatched " + list(*unmatched) + ", expected " + list(want);
    return "";
}

void expectByIdentity(const char* pair, const Stage& vs, const std::vector<std::string>& vsNames, const Stage& fs,
                      const std::vector<std::string>& fsNames, const std::vector<std::string>& wantUnmatched) {
    CHECK(isBugCondition(vs, vsNames, fs, fsNames));   // the pair is one the fix is for
    Linkage l{};
    std::string why;
    std::vector<std::string> unmatched;
    bool ok = a650Link(vs, vsNames, fs, fsNames, &l, &why, &unmatched);
    std::string bad = byIdentity(vs, vsNames, fs, fsNames, ok, l, why, &unmatched);
    CHECK(bad.empty());
    if (!bad.empty()) std::fprintf(stderr, "  %s: %s\n", pair, bad.c_str());
    CHECK(unmatched == wantUnmatched);
    if (unmatched != wantUnmatched) std::fprintf(stderr, "  %s: unmatched %s, expected %s\n", pair, list(unmatched).c_str(), list(wantUnmatched).c_str());
}

}  // namespace

// VS {a, b}, FS {b, a}: by slot the two inputs swap without a word
TEST(a650_link_reorder_links_each_input_to_its_own_identity) {
    expectByIdentity("var_ab_vs+var_ba_fs", kernels::var_ab_vs_air, {kIdA, kIdB}, kernels::var_ba_fs_air, {kIdB, kIdA}, {});
}

// VS {a, b}, FS {b}: by slot the FS's b (its VAR0) reads the VS's a
TEST(a650_link_subset_links_the_input_to_its_own_identity) {
    expectByIdentity("var_ab_vs+var_b_fs", kernels::var_ab_vs_air, {kIdA, kIdB}, kernels::var_b_fs_air, {kIdB}, {});
}

// VS {a}, FS {a, c}: by slot refused at VAR1 (the phone's sentence); on the A650 a links and c reads zeros, noted
TEST(a650_link_unmatched_input_reads_zeros_and_is_noted) {
    expectByIdentity("var_a_vs+var_ac_fs", kernels::var_a_vs_air, {kIdA}, kernels::var_ac_fs_air, {kIdA, kIdC}, {"VAR1 '" + kIdC + "'"});
}

// Property 1 over generated pairs: a VS writing a random subset of a pool of identities in a random order, an FS
// reading a random subset of them plus identities the VS does not have, in another order; slots numbered per stage in
// declaration order as air_to_nir.c does, inlocs packed. Fixed seed; the first failing case is printed.
TEST(a650_link_by_identity_holds_over_random_pairs) {
    std::mt19937 rng(0x0a650u);
    const uint32_t kCases = 3000;
    std::vector<std::string> pool;
    for (uint32_t k = 0; k < 12; k++) pool.push_back("user(locn" + std::to_string(k) + ")");
    static const uint8_t kMasks[] = {0x1, 0x3, 0x7, 0xf};
    uint32_t bugCases = 0, failed = 0;
    for (uint32_t t = 0; t < kCases; t++) {
        std::vector<std::string> ids = pool;
        std::shuffle(ids.begin(), ids.end(), rng);
        uint32_t n = rng() % 9;   // VS user varyings, 0..8
        std::vector<std::string> vsNames(ids.begin(), ids.begin() + n);
        Stage vs = kernels::var_ab_vs_air;
        for (uint32_t j = 0; j < kMaxStageOutputs; j++) vs.outputs[j] = StageOutput{0, 0, 0, 0};
        vs.outputs[0] = StageOutput{kSlotPos, uint8_t(regid(1, 3)), 0, 0};
        vs.positionRegid = regid(1, 3);
        for (uint32_t j = 0; j < n; j++) vs.outputs[1 + j] = StageOutput{uint8_t(kSlotVar0 + j), uint8_t(regid(2 + j, 0)), 0, 0};
        vs.numOutputs = 1 + n;
        std::vector<std::string> fsNames;
        for (uint32_t j = 0; j < n; j++) {
            if (rng() % 2) fsNames.push_back(vsNames[j]);
        }
        uint32_t extra = rng() % 3;   // identities no output of the VS has
        for (uint32_t e = 0; e < extra; e++) fsNames.push_back(ids[n + e]);
        std::shuffle(fsNames.begin(), fsNames.end(), rng);
        Stage fs = kernels::var_ba_fs_air;
        for (uint32_t j = 0; j < kMaxStageInputs; j++) fs.inputs[j] = StageInput{0, 0, 0, 0, 0, 0, 0, 0};
        uint32_t m = uint32_t(fsNames.size()), loc = 0;
        for (uint32_t j = 0; j < m; j++) {
            uint8_t cm = kMasks[rng() % 4];
            fs.inputs[j] = StageInput{uint8_t(kSlotVar0 + j), a6xx::kRegidNone, cm, uint8_t(loc), 0, 1, uint8_t(rng() % 2), 0};
            loc += lastBitOf(cm);
        }
        fs.inputs[m] = StageInput{56, uint8_t(regid(0, 0)), 0x3, 0, 1, 0, 0, 0};   // SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL
        fs.numInputs = m + 1;
        fs.totalIn = loc;
        if (!isBugCondition(vs, vsNames, fs, fsNames)) continue;   // Property 2's (task 3)
        bugCases++;
        Linkage l{};
        std::string why;
        std::vector<std::string> unmatched;
        bool ok = a650Link(vs, vsNames, fs, fsNames, &l, &why, &unmatched);
        std::string bad = byIdentity(vs, vsNames, fs, fsNames, ok, l, why, &unmatched);
        if (!bad.empty() && failed++ == 0) {
            std::fprintf(stderr, "  first counterexample, case %u: VS %s, FS %s\n    %s\n", t, list(vsNames).c_str(), list(fsNames).c_str(), bad.c_str());
        }
    }
    std::printf("       %u of %u generated pairs meet the bug condition; %u of them not linked by identity\n", bugCases, kCases, failed);
    CHECK(bugCases > 0);
    CHECK_EQ(failed, 0u);
}

// With the compiler in (MESA=1): the same synthetic pairs compiled for the A650 and linked by linkProgram, which holds
// render::link to Mesa's ir3_link_shaders and refuses a disagreement; the draw's re-link from Program::vs/fs must find
// the same Linkage. The FS identities are the .ll's (before the fix the front end does not record them).
namespace {

std::vector<uint8_t> bitcodeFile(const char* name) {
    std::ifstream f(std::string("out/host/kernels/") + name + ".bc", std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::shared_ptr<CompiledStage> a650Stage(const char* name) {
    std::vector<uint8_t> bc = bitcodeFile(name);
    if (bc.empty()) {
        std::printf("       out/host/kernels/%s.bc is missing (make test MESA=1 assembles it)\n", name);
        return nullptr;
    }
    std::string why;
    std::shared_ptr<CompiledStage> s = compileStageForChip(bc.data(), bc.size(), a6xx::kA650ChipId, &why);
    if (s == nullptr) std::printf("       %s: %s\n", name, why.c_str());
    return s;
}

bool sameLinkage(const Linkage& a, const Linkage& b) {
    if (a.cnt != b.cnt || a.maxLoc != b.maxLoc || a.positionLoc != b.positionLoc || a.psizeLoc != b.psizeLoc || a.mrtComponents != b.mrtComponents) return false;
    for (uint32_t i = 0; i < a.cnt && i < kMaxStageOutputs; i++) {
        if (a.var[i].slot != b.var[i].slot || a.var[i].regid != b.var[i].regid || a.var[i].compmask != b.var[i].compmask || a.var[i].loc != b.var[i].loc) return false;
    }
    for (uint32_t i = 0; i < 4; i++) {
        if (a.varmask[i] != b.varmask[i]) return false;
    }
    for (uint32_t i = 0; i < 8; i++) {
        if (a.interpMode[i] != b.interpMode[i] || a.psReplMode[i] != b.psReplMode[i]) return false;
    }
    return true;
}

}  // namespace

TEST(a650_linkProgram_links_the_synthetic_pairs_by_identity) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: the A650 linkProgram pairs did not run)\n");
        return;
    }
    struct Pair {
        const char* vs;
        std::vector<std::string> vsNames;
        const char* fs;
        std::vector<std::string> fsNames;
    };
    const Pair pairs[] = {
        {"var_ab_vs", {kIdA, kIdB}, "var_ba_fs", {kIdB, kIdA}},   // reorder
        {"var_ab_vs", {kIdA, kIdB}, "var_b_fs", {kIdB}},          // subset
        {"var_a_vs", {kIdA}, "var_ac_fs", {kIdA, kIdC}},          // unmatched
    };
    for (const Pair& p : pairs) {
        auto vs = a650Stage(p.vs);
        auto fs = a650Stage(p.fs);
        CHECK(vs != nullptr && fs != nullptr);
        if (vs == nullptr || fs == nullptr) continue;
        CHECK(vs->varyingNames == p.vsNames);   // the VS side the front end records already
        CHECK(isBugCondition(vs->stage, p.vsNames, fs->stage, p.fsNames));
        std::string why;
        auto prog = linkProgram(vs, fs, PixelFormat::kRGBA8Unorm, &why);
        CHECK(prog != nullptr);
        if (prog == nullptr) {
            std::fprintf(stderr, "  %s+%s (A650): refused: \"%s\"\n", p.vs, p.fs, why.c_str());
            continue;
        }
        std::string bad = byIdentity(vs->stage, p.vsNames, fs->stage, p.fsNames, true, prog->program.link, "", nullptr);
        CHECK(bad.empty());
        if (!bad.empty()) std::fprintf(stderr, "  %s+%s (A650): %s\n", p.vs, p.fs, bad.c_str());
        Linkage again{};
        std::string w;
        CHECK(render::link(prog->program.vs, prog->program.fs, &again, &w));
        CHECK(sameLinkage(again, prog->program.link));
        // the link note: one line naming both functions and the unmatched identity, and nothing for a full match
        const std::string& note = prog->linkNote;
        CHECK(note.find('\n') == std::string::npos);
        if (p.fsNames.size() == 2 && p.fsNames[1] == kIdC) {
            CHECK(note.find(prog->name + ":") == 0);
            CHECK(note.find(std::string("VAR1 '") + kIdC + "'") != std::string::npos);
            CHECK(note.find("read zeros") != std::string::npos);
            std::printf("       %s\n", note.c_str());
        } else {
            CHECK(note.empty());
        }
    }
}

// --- Property 2: Preservation (.kiro/specs/metal-var1-linkage-crash-fix, task 3) ----------------------------------------
// Where the bug condition does not hold -- a stage compiled for another chip, or an A650 pair whose names agree with
// its slots -- the link must be what it was before the fix: the same Linkage field for field, or the same refusal word
// for word. The oracle is a copy of render::link as it stood before the fix (src/link.cpp at aba653b), kept here so
// that the fix cannot move both sides at once.
namespace oracle {

constexpr uint8_t kPos = 0, kVar0 = 32, kPsiz = 12, kPntc = 25;
constexpr uint32_t kInterpFlat = 1, kInterpZero = 2, kInterpOne = 3, kPsReplS = 1, kPsReplT = 2;

std::string slotName(uint8_t slot) {
    if (slot == kPos) return "POS";
    if (slot >= kVar0) return "VAR" + std::to_string(slot - kVar0);
    return "slot " + std::to_string(slot);
}

uint32_t lastBit(uint32_t mask) {
    uint32_t n = 0;
    while (mask != 0) {
        n++;
        mask >>= 1;
    }
    return n;
}

int findOutput(const Stage& vs, uint8_t slot) {
    for (uint32_t j = 0; j < vs.numOutputs; j++) {
        if (vs.outputs[j].slot == slot) return int(j);
    }
    return -1;
}

bool linkAdd(Linkage& l, uint8_t slot, uint32_t regid_, uint8_t compmask, uint8_t loc) {
    for (uint32_t j = 0; j < lastBit(compmask); j++) {
        uint32_t comploc = loc + j;
        l.varmask[comploc / 32] |= 1u << (comploc % 32);
    }
    uint32_t end = loc + lastBit(compmask);
    if (end > l.maxLoc) l.maxLoc = end;
    if (regid_ != a6xx::kRegidNone) {
        if (l.cnt >= kMaxStageOutputs) return false;
        l.var[l.cnt] = Linkage::Var{slot, uint8_t(regid_), compmask, loc};
        l.cnt++;
    }
    return true;
}

bool refuse(std::string* why, const std::string& text) {
    if (why != nullptr) *why = text;
    return false;
}

bool link(const Stage& vs, const Stage& fs, Linkage* out, std::string* why) {
    if (vs.kind != kStageVertex) return refuse(why, std::string(vs.k.name ? vs.k.name : "the first stage") + " is not a vertex stage");
    if (fs.kind != kStageFragment) return refuse(why, std::string(fs.k.name ? fs.k.name : "the second stage") + " is not a fragment stage");
    Linkage l{};
    for (uint32_t j = 0; j < fs.numInputs; j++) {
        const StageInput& in = fs.inputs[j];
        if (in.compmask == 0 || !in.bary) continue;
        if (in.inloc >= fs.totalIn) continue;
        if (in.slot == kPntc) {
            if (!linkAdd(l, in.slot, a6xx::kRegidNone, in.compmask, in.inloc)) {
                return refuse(why, "the link holds " + std::to_string(kMaxStageOutputs) + " entries and the fragment stage reads more varyings than that");
            }
            uint32_t loc = in.inloc;
            const uint32_t repl[2] = {kPsReplS, kPsReplT}, interp[2] = {kInterpZero, kInterpOne};
            for (uint32_t c = 0; c < 4; c++) {
                if (((in.compmask >> c) & 1) == 0) continue;
                if (c < 2) l.psReplMode[loc / 16] |= repl[c] << ((loc % 16) * 2);
                else l.interpMode[loc / 16] |= interp[c - 2] << ((loc % 16) * 2);
                loc++;
            }
            continue;
        }
        int k = findOutput(vs, in.slot);
        if (k < 0) {
            return refuse(why, "the fragment stage reads varying slot " + std::to_string(in.slot) + " (" + slotName(in.slot) +
                                   "), which the vertex stage does not write");
        }
        if (!linkAdd(l, in.slot, vs.outputs[k].regid, in.compmask, in.inloc)) {
            return refuse(why, "the link holds " + std::to_string(kMaxStageOutputs) + " entries and the fragment stage reads more varyings than that");
        }
        if (in.flat) {
            for (uint32_t c = 0; c < lastBit(in.compmask); c++) {
                if (((in.compmask >> c) & 1) == 0) continue;
                uint32_t loc = in.inloc + c;
                l.interpMode[loc / 16] |= kInterpFlat << ((loc % 16) * 2);
            }
        }
    }
    if (vs.positionRegid == a6xx::kRegidNone) {
        return refuse(why, std::string(vs.k.name ? vs.k.name : "the vertex stage") + " writes no position: nothing to rasterize");
    }
    if (vs.positionRegid > 0xff) {
        return refuse(why, std::string(vs.k.name ? vs.k.name : "the vertex stage") + " writes its position in a half register: not served");
    }
    uint32_t varyingsOnly[4] = {l.varmask[0], l.varmask[1], l.varmask[2], l.varmask[3]};
    l.positionLoc = l.maxLoc;
    if (!linkAdd(l, kPos, vs.positionRegid, 0xf, uint8_t(l.maxLoc))) {
        return refuse(why, "the link holds " + std::to_string(kMaxStageOutputs) + " entries and has no room left for the position");
    }
    for (uint32_t i = 0; i < 4; i++) l.varmask[i] = varyingsOnly[i];
    if (vs.pointSizeRegid != a6xx::kRegidNone) {
        l.psizeLoc = l.maxLoc;
        if (!linkAdd(l, kPsiz, uint8_t(vs.pointSizeRegid), 0x1, uint8_t(l.maxLoc))) {
            return refuse(why, "the link holds " + std::to_string(kMaxStageOutputs) + " entries and has no room left for the point size");
        }
        for (uint32_t i = 0; i < 4; i++) l.varmask[i] = varyingsOnly[i];
    }
    for (uint32_t i = 0; i < fs.numColorOutputs && i < kMaxColorOutputs; i++) {
        if (fs.colorRegid[i] != a6xx::kRegidNone) l.mrtComponents |= 0xfu << (i * 4);
    }
    *out = l;
    return true;
}

}  // namespace oracle

namespace {

// the link under test against the oracle over one pair: empty when they agree (both refused with the same sentence, or
// both linked to the same Linkage), else what differs
std::string againstOracle(bool ok, const Linkage& l, const std::string& why, const Stage& vs, const Stage& fs) {
    Linkage o{};
    std::string owhy;
    bool ook = oracle::link(vs, fs, &o, &owhy);
    if (ok != ook) return ook ? "refused (\"" + why + "\"), the oracle links" : "linked, the oracle refuses (\"" + owhy + "\")";
    if (!ok) return why == owhy ? "" : "refused \"" + why + "\", the oracle \"" + owhy + "\"";
    if (!sameLinkage(l, o)) {
        return "Linkage differs: cnt " + std::to_string(l.cnt) + "/" + std::to_string(o.cnt) + ", maxLoc " + std::to_string(l.maxLoc) + "/" +
               std::to_string(o.maxLoc) + ", positionLoc " + std::to_string(l.positionLoc) + "/" + std::to_string(o.positionLoc) + ", psizeLoc " +
               std::to_string(l.psizeLoc) + "/" + std::to_string(o.psizeLoc);
    }
    return "";
}

// the varying fields of two fragment Stages, byte for byte (StageInput and StageOutput are all uint8_t: no padding)
bool sameVaryingFields(const Stage& a, const Stage& b) {
    return a.numInputs == b.numInputs && a.totalIn == b.totalIn && a.numOutputs == b.numOutputs &&
           std::memcmp(a.inputs, b.inputs, sizeof a.inputs) == 0 && std::memcmp(a.outputs, b.outputs, sizeof a.outputs) == 0;
}

// a VS's outputs drawn at random: the position, the point size, GL built-ins hakios routes apart (clip distance 17,
// layer 22, viewport 23), and generic varyings; a position register that is sometimes none or half
Stage randomVs(std::mt19937& rng) {
    static const uint8_t kBuiltins[] = {kSlotPos, 12, 17, 22, 23};
    Stage vs = kernels::var_ab_vs_air;
    vs.kind = rng() % 50 == 0 ? uint8_t(kStageFragment) : uint8_t(kStageVertex);
    for (uint32_t j = 0; j < kMaxStageOutputs; j++) vs.outputs[j] = StageOutput{0, 0, 0, 0};
    vs.numOutputs = rng() % (kMaxStageOutputs + 1);
    for (uint32_t j = 0; j < vs.numOutputs; j++) {
        uint8_t slot = rng() % 4 == 0 ? kBuiltins[rng() % 5] : uint8_t(kSlotVar0 + rng() % 32);
        vs.outputs[j] = StageOutput{slot, uint8_t(rng() % 0xfc), 0, 0};
    }
    uint32_t p = rng() % 20;
    vs.positionRegid = p == 0 ? a6xx::kRegidNone : p == 1 ? (0x100 | (rng() % 0xfc)) : rng() % 0xfc;
    vs.pointSizeRegid = rng() % 4 == 0 ? rng() % 0xfc : a6xx::kRegidNone;
    return vs;
}

// an FS's inputs drawn at random: generic varyings (mostly slots the VS writes), [[point_coord]], system values, stray
// entries with bary clear; random compmask, inloc, flat and totalIn (inloc kept below 117 so the locations, the
// position's and the point size's included, stay inside varmask's 128 bits)
Stage randomFs(std::mt19937& rng, const Stage& vs) {
    Stage fs = kernels::var_ba_fs_air;
    fs.kind = rng() % 50 == 0 ? uint8_t(kStageVertex) : uint8_t(kStageFragment);
    for (uint32_t j = 0; j < kMaxStageInputs; j++) fs.inputs[j] = StageInput{0, 0, 0, 0, 0, 0, 0, 0};
    fs.numInputs = rng() % 3 == 0 ? rng() % (kMaxStageInputs + 1) : rng() % 8;
    uint32_t loc = 0;
    for (uint32_t j = 0; j < fs.numInputs; j++) {
        StageInput in{0, a6xx::kRegidNone, uint8_t(rng() % 16), 0, 0, 1, uint8_t(rng() % 2), 0};
        uint32_t what = rng() % 10;
        if (what < 6) {
            in.slot = vs.numOutputs != 0 && rng() % 5 != 0 ? vs.outputs[rng() % vs.numOutputs].slot : uint8_t(kSlotVar0 + rng() % 32);
        } else if (what == 6) {
            in.slot = 25;   // VARYING_SLOT_PNTC
        } else if (what == 7) {
            in.slot = uint8_t(56 + rng() % 8);   // a SYSTEM_VALUE_BARYCENTRIC_* or the like
            in.regid = uint8_t(rng() % 0xfc);
            in.sysval = 1;
            in.bary = 0;
        } else {
            in.slot = uint8_t(rng() % 64);
            in.bary = uint8_t(rng() % 2);
        }
        in.inloc = uint8_t(rng() % 4 == 0 ? rng() % 117 : std::min<uint32_t>(loc, 116));
        loc += lastBitOf(in.compmask);
        fs.inputs[j] = in;
    }
    fs.totalIn = rng() % 4 == 0 ? rng() % 129 : std::min<uint32_t>(loc, 128);
    fs.numColorOutputs = rng() % (kMaxColorOutputs + 1);
    for (uint32_t i = 0; i < kMaxColorOutputs; i++) fs.colorRegid[i] = rng() % 3 == 0 ? a6xx::kRegidNone : rng() % 0xfc;
    return fs;
}

}  // namespace

// The synthetic pairs as compiled for the default chip (the fixtures are 0x06030500's): linked by slot, and the
// unmatched one refused with the phone's sentence, as before the fix (3.1, 3.8)
TEST(slot_link_of_the_synthetic_pairs_is_unchanged_off_the_a650) {
    Linkage l{};
    std::string why;
    CHECK(!render::link(kernels::var_a_vs_air, kernels::var_ac_fs_air, &l, &why));
    expectRefusal(why, "the fragment stage reads varying slot 33 (VAR1), which the vertex stage does not write");
    const std::pair<const Stage*, const Stage*> linked[] = {{&kernels::var_ab_vs_air, &kernels::var_ba_fs_air},
                                                            {&kernels::var_ab_vs_air, &kernels::var_b_fs_air}};
    for (const auto& p : linked) {
        const Stage& vs = *p.first;
        const Stage& fs = *p.second;
        bool ok = render::link(vs, fs, &l, &why);
        CHECK(ok);
        if (!ok) {
            std::fprintf(stderr, "  %s+%s: refused \"%s\"\n", vs.k.name, fs.k.name, why.c_str());
            continue;
        }
        std::string bad = againstOracle(ok, l, why, vs, fs);
        CHECK(bad.empty());
        if (!bad.empty()) std::fprintf(stderr, "  %s+%s: %s\n", vs.k.name, fs.k.name, bad.c_str());
        // by slot: each user input at its inloc reads the VS register of ITS slot, whatever the names say
        uint32_t seen = 0;
        for (uint32_t i = 0; i < fs.numInputs; i++) {
            const StageInput& in = fs.inputs[i];
            if (!isUserVarying(fs, in)) continue;
            int k = outputBySlot(vs, in.slot);
            CHECK(k >= 0);
            if (k < 0) continue;
            bool found = false;
            for (uint32_t j = 0; j < l.cnt; j++) {
                if (l.var[j].loc == in.inloc) found = sameVar(l.var[j], in.slot, vs.outputs[k].regid, in.compmask, in.inloc);
            }
            CHECK(found);
            seen++;
        }
        CHECK(seen > 0);
    }
}

// Property 2 over generated Stage pairs (no kVaryingSlotUnserved input, as no non-A650 stage carries one): render::link
// equals the oracle on every pair, refusals included. Fixed seed; the first differing case is printed.
// **Validates: Requirements 3.1, 3.4, 3.5, 3.6, 3.8**
TEST(slot_link_is_preserved_over_random_pairs) {
    std::mt19937 rng(0x5107u);
    const uint32_t kCases = 4000;
    uint32_t linked = 0, refused = 0, failed = 0;
    for (uint32_t t = 0; t < kCases; t++) {
        Stage vs = randomVs(rng);
        Stage fs = randomFs(rng, vs);
        Linkage l{};
        std::string why;
        bool ok = render::link(vs, fs, &l, &why);
        (ok ? linked : refused)++;
        std::string bad = againstOracle(ok, l, why, vs, fs);
        if (!bad.empty() && failed++ == 0) {
            std::fprintf(stderr, "  first counterexample, case %u: VS %u outputs, FS %u inputs (totalIn %u): %s\n", t, vs.numOutputs, fs.numInputs, fs.totalIn,
                         bad.c_str());
        }
    }
    std::printf("       %u generated pairs: %u linked, %u refused; %u differ from the link before the fix\n", kCases, linked, refused, failed);
    CHECK(linked > kCases / 10);
    CHECK(refused > kCases / 10);
    CHECK_EQ(failed, 0u);
}

// Property 2 over generated A650 pairs where the bug condition does not hold: names that agree with the slots, an FS
// table with no names, or a VS table with an empty name (both keep the slot link). a650Link must leave the fragment
// Stage byte-identical, report nothing unmatched, and give the oracle's Linkage or refusal.
// **Validates: Requirements 3.1, 3.4, 3.5, 3.8**
TEST(a650_link_of_agreeing_pairs_is_the_slot_link) {
    std::mt19937 rng(0xa9e5u);
    const uint32_t kCases = 3000;
    std::vector<std::string> pool;
    for (uint32_t k = 0; k < 12; k++) pool.push_back("user(locn" + std::to_string(k) + ")");
    uint32_t agreeing = 0, unnamed = 0, failed = 0;
    for (uint32_t t = 0; t < kCases; t++) {
        std::vector<std::string> ids = pool;
        std::shuffle(ids.begin(), ids.end(), rng);
        uint32_t n = rng() % 9;
        std::vector<std::string> vsNames(ids.begin(), ids.begin() + n);
        Stage vs = kernels::var_ab_vs_air;
        for (uint32_t j = 0; j < kMaxStageOutputs; j++) vs.outputs[j] = StageOutput{0, 0, 0, 0};
        // the VS's outputs in a random order: the position and VAR0..VARn-1, each at its own register
        std::vector<StageOutput> outs{StageOutput{kSlotPos, uint8_t(regid(1, 3)), 0, 0}};
        for (uint32_t j = 0; j < n; j++) outs.push_back(StageOutput{uint8_t(kSlotVar0 + j), uint8_t(regid(2 + j, rng() % 4)), 0, 0});
        if (rng() % 4 == 0) {
            outs.push_back(StageOutput{12, uint8_t(regid(12, 0)), 0, 0});   // a [[point_size]] too
            vs.pointSizeRegid = regid(12, 0);
        } else {
            vs.pointSizeRegid = a6xx::kRegidNone;
        }
        std::shuffle(outs.begin(), outs.end(), rng);
        for (uint32_t j = 0; j < outs.size(); j++) vs.outputs[j] = outs[j];
        vs.numOutputs = uint32_t(outs.size());
        vs.positionRegid = regid(1, 3);
        // the FS: slots the VS writes (and now and then one it does not), [[point_coord]] and the barycentric sysval
        uint32_t mode = rng() % 4;   // 0, 1: names agree with the slots; 2: the FS records no names; 3: a VS name is empty
        Stage fs = kernels::var_ba_fs_air;
        for (uint32_t j = 0; j < kMaxStageInputs; j++) fs.inputs[j] = StageInput{0, 0, 0, 0, 0, 0, 0, 0};
        uint32_t m = 0, loc = 0, maxSlot = 0;
        for (uint32_t k = 0; k < n + 1; k++) {
            if (rng() % 3 == 0) continue;
            if (k == n && mode < 2) continue;   // a slot the VS does not write: only where the names cannot say otherwise
            uint8_t cm = uint8_t(1 + rng() % 15);
            fs.inputs[m++] = StageInput{uint8_t(kSlotVar0 + k), a6xx::kRegidNone, cm, uint8_t(loc), 0, 1, uint8_t(rng() % 2), 0};
            loc += lastBitOf(cm);
            maxSlot = k + 1;
        }
        if (rng() % 4 == 0) {
            fs.inputs[m++] = StageInput{25, a6xx::kRegidNone, 0x3, uint8_t(loc), 0, 1, 0, 0};   // [[point_coord]]
            loc += 2;
        }
        fs.inputs[m++] = StageInput{56, uint8_t(regid(0, 0)), 0x3, 0, 1, 0, 0, 0};   // SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL
        std::shuffle(fs.inputs, fs.inputs + m, rng);
        fs.numInputs = m;
        fs.totalIn = loc;
        std::vector<std::string> fsNames;
        if (mode < 2) {
            fsNames.assign(vsNames.begin(), vsNames.begin() + std::min(maxSlot, n));
            agreeing++;
        } else {
            if (mode == 3) {
                if (n == 0) vsNames.push_back("");
                else vsNames[rng() % n] = "";
                fsNames = pool;   // names the VS may or may not have; the empty VS name keeps every input on its slot
                std::shuffle(fsNames.begin(), fsNames.end(), rng);
            }
            unnamed++;
        }
        if (isBugCondition(vs, vsNames, fs, fsNames)) {
            CHECK(!"the generator made a pair the bug condition holds for");
            continue;
        }
        Linkage l{};
        std::string why;
        std::vector<std::string> unmatched;
        Stage used{};
        bool ok = a650Link(vs, vsNames, fs, fsNames, &l, &why, &unmatched, &used);
        std::string bad = againstOracle(ok, l, why, vs, fs);
        if (bad.empty() && !unmatched.empty()) bad = "unmatched " + list(unmatched) + ", expected none";
        if (bad.empty() && !sameVaryingFields(used, fs)) bad = "the fragment Stage the link used is not the one given";
        if (!bad.empty() && failed++ == 0) {
            std::fprintf(stderr, "  first counterexample, case %u (mode %u): VS %s, FS %s\n    %s\n", t, mode, list(vsNames).c_str(), list(fsNames).c_str(),
                         bad.c_str());
        }
    }
    std::printf("       %u generated A650 pairs (%u with agreeing names, %u slot-linked for want of names): %u differ from the slot link\n", kCases,
                agreeing, unnamed, failed);
    CHECK(agreeing > 0 && unnamed > 0);
    CHECK_EQ(failed, 0u);
}
