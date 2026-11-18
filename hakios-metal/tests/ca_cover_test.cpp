// The RAM plugin's vertex coverage guard for CoreAnimation pipelines (spec metal-ca-render-glitch), as the pure
// function hm_ca_cover_vertex of userland/src/hm_ca_cover.h. A test-local model of today's selection (both orders of
// hm_bitcode_for: the committed one and the RAM build's) gives the selected vertex; a test-local oracle answers
// Usable(v) = R(f) ⊆ W(v) AND A(v) ⊆ D AND fn(v) == fn(selected) over synthetic identity, attribute and function sets.
//
// Property 1 (Bug Condition): when the selected vertex does not write every identity the fragment reads and some
// library vertex is usable, the result is usable and is the first usable one of [table, pairing, sorted library].
//
// Property 2 (Preservation): everywhere else the result is the selected vertex, the oracle is asked about each distinct
// non-NULL candidate at most once, and -1 counts as not covering. With MESA=1 the oracle's meaning is held to the
// compiler: on the A650 an empty link note is R ⊆ W; off the A650 the note is always empty and a pair links by slot.
//
// The names below are CoreAnimation's variant labels used as plain strings; W, A and fn are synthetic stand-ins
// shaped after design.md (task 1.3), not data taken from Apple's modules.
//
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>
#include "../../userland/src/hm_ca_cover.h"
#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/compiler.hpp"
#include "hakios-metal/render.hpp"
#include "test.hpp"

namespace {

// identities (bits of W and R)
constexpr unsigned kColor = 1u << 0, kTex0A = 1u << 1, kTex1A = 1u << 2, kTex0B = 1u << 3;
// vertex functions (fn)
constexpr int kSpc = 0, kGen = 1;   // fixed_vert_lph_spc, fixed_vert_lph_gen
// attribute masks (bits of A and D)
constexpr unsigned kLayout48 = 0xfu;            // attributes 0-3
constexpr unsigned kLayoutLln = 1u | (1u << 3);  // attributes 0 and 3

struct Variant {
    std::string name;
    unsigned w;   // identities written
    unsigned a;   // attributes read
    int fn;       // vertex function it specialises
};

enum class Order { Committed, Ram };

struct Pipeline {
    int label = -2;        // -2: no label; -1: a label that names no library variant; >= 0: that variant
    bool library = false;  // hm_ca_pipelines has the pipeline
    int table = -1;        // hm_ca_pipelines[pipeline][0] (library pipelines only)
    int paired = -1;       // hm_ca_frag_vert[fsChosen]
    bool runtime = false;  // a com.apple.coreanimation.draw. pipeline the library does not name
    unsigned r = 0;        // identities the chosen fragment reads
    unsigned d = 0;        // attributes the vertex descriptor describes
};

// today's hm_bitcode_for for the vertex stage: the variant index, or -1 for the function's own module
int selectToday(const Pipeline& p, Order order) {
    if (order == Order::Committed) {
        if (p.label >= 0) return p.label;                // label, then table, then the run-time branch
        if (p.library) return p.table;
        if (p.runtime) return p.paired;
        return -1;
    }
    if (!p.library && p.runtime && p.paired >= 0) return p.paired;   // RAM: run-time branch first, then label, table
    if (p.label >= 0) return p.label;
    if (p.library) return p.table;
    return -1;
}

bool covers(const Variant& v, const Pipeline& p) { return (p.r & ~v.w) == 0; }

bool usable(const std::vector<Variant>& vs, int v, int selected, const Pipeline& p) {
    if (v == selected) return covers(vs[v], p);   // its own link note decides
    return covers(vs[v], p) && (vs[v].a & ~p.d) == 0 && vs[v].fn == vs[selected].fn;
}

struct Oracle {
    const std::vector<Variant>* vs;
    const Pipeline* p;
    int selected;
    std::vector<std::string> asked;
};

int oracleCovers(void* ctx, const char* vertex) {
    Oracle* o = static_cast<Oracle*>(ctx);
    o->asked.push_back(vertex != nullptr ? vertex : "(null)");
    if (vertex == nullptr) return -1;
    for (size_t i = 0; i < o->vs->size(); i++) {
        if ((*o->vs)[i].name == vertex) return usable(*o->vs, (int)i, o->selected, *o->p) ? 1 : 0;
    }
    return -1;
}

bool isBugCondition(const std::vector<Variant>& vs, int selected, const Pipeline& p) {
    if (selected < 0 || covers(vs[selected], p)) return false;
    for (size_t i = 0; i < vs.size(); i++) {
        if (usable(vs, (int)i, selected, p)) return true;
    }
    return false;
}

std::vector<int> sortedByName(const std::vector<Variant>& vs) {
    std::vector<int> order(vs.size());
    for (size_t i = 0; i < vs.size(); i++) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [&](int x, int y) { return vs[x].name < vs[y].name; });
    return order;
}

// the first usable candidate of [table, pairing, sorted library] and the way it is reached
int expectedChoice(const std::vector<Variant>& vs, int selected, const Pipeline& p, hm_ca_cover_how* how) {
    if (p.table >= 0 && usable(vs, p.table, selected, p)) return *how = HM_CA_COVER_PIPELINE, p.table;
    if (p.paired >= 0 && usable(vs, p.paired, selected, p)) return *how = HM_CA_COVER_PAIRED, p.paired;
    for (int i : sortedByName(vs)) {
        if (usable(vs, i, selected, p)) return *how = HM_CA_COVER_SUPERSET, i;
    }
    return *how = HM_CA_COVER_NONE, selected;
}

struct Result {
    const char* vertex;
    hm_ca_cover_how how;
    std::vector<std::string> asked;
};

Result runGuard(const std::vector<Variant>& vs, int selected, const Pipeline& p) {
    std::vector<int> order = sortedByName(vs);
    std::vector<const char*> names;
    for (int i : order) names.push_back(vs[i].name.c_str());
    Oracle o{&vs, &p, selected, {}};
    hm_ca_cover_how how = HM_CA_COVER_NONE;
    const char* got = hm_ca_cover_vertex(vs[selected].name.c_str(), p.table >= 0 ? vs[p.table].name.c_str() : nullptr,
                                         p.paired >= 0 ? vs[p.paired].name.c_str() : nullptr, names.data(), names.size(),
                                         oracleCovers, &o, &how);
    return Result{got, how, o.asked};
}

const char* howName(hm_ca_cover_how how) {
    switch (how) {
        case HM_CA_COVER_KEPT: return "KEPT";
        case HM_CA_COVER_PIPELINE: return "PIPELINE";
        case HM_CA_COVER_PAIRED: return "PAIRED";
        case HM_CA_COVER_SUPERSET: return "SUPERSET";
        case HM_CA_COVER_NONE: return "NONE";
    }
    return "?";
}

std::string identities(unsigned mask) {
    static const char* const names[] = {"color", "texcoord0A", "texcoord1A", "texcoord0B", "id4", "id5", "id6", "id7"};
    std::string s;
    for (int i = 0; i < 8; i++) {
        if (mask & (1u << i)) s += std::string(s.empty() ? "" : ",") + names[i];
    }
    return "{" + s + "}";
}

// the 7 library vertex variants, with synthetic W, A and fn (A after task 1.3; fn after the task 1.5 decision)
std::vector<Variant> library() {
    return {
        {"VfxLlnXh", kColor, kLayoutLln, kSpc},
        {"VfxU010Xh", kColor | kTex1A, 1u | 4u | 8u, kSpc},
        {"VfxU100Xh", kTex0A | kColor, 1u | 2u | 8u, kSpc},
        {"VfxU110Xh", kTex0A | kTex1A | kColor, kLayout48, kSpc},
        {"VfxU200Xh", kTex0A | kTex0B | kColor, 1u | 2u | 8u, kSpc},
        {"VfxXgh", kTex0A | kColor, 0u, kGen},
        {"VfxXh", kColor, kLayoutLln, kSpc},
    };
}

int indexOf(const std::vector<Variant>& vs, const char* name) {
    for (size_t i = 0; i < vs.size(); i++) {
        if (vs[i].name == name) return (int)i;
    }
    return -1;
}

}  // namespace

// The bug condition from design: PBGRAXm_TimgA2Xhf_Icir, labelled VfxXh, table and pairing VfxU100Xh, R = {texcoord0A},
// 48-byte layout. Both orders select VfxXh; the guard must draw it with VfxU100Xh from the pipeline table.
TEST(ca_cover_label_vfxxh_with_timg_is_drawn_with_the_table_vertex) {
    const std::vector<Variant> vs = library();
    Pipeline p;
    p.label = indexOf(vs, "VfxXh");
    p.library = true;
    p.table = p.paired = indexOf(vs, "VfxU100Xh");
    p.r = kTex0A;
    p.d = kLayout48;
    for (Order order : {Order::Committed, Order::Ram}) {
        const int selected = selectToday(p, order);
        CHECK_EQ(selected, indexOf(vs, "VfxXh"));
        CHECK(isBugCondition(vs, selected, p));
        Result r = runGuard(vs, selected, p);
        if (std::strcmp(r.vertex, "VfxU100Xh") != 0 || r.how != HM_CA_COVER_PIPELINE) {
            std::fprintf(stderr, "  counterexample: label VfxXh, table VfxU100Xh, R %s, D 0x%x (%s order): returned %s (%s), "
                         "which leaves %s unserved; expected VfxU100Xh (PIPELINE)\n", identities(p.r).c_str(), p.d,
                         order == Order::Committed ? "committed" : "RAM", r.vertex, howName(r.how),
                         identities(p.r & ~vs[indexOf(vs, r.vertex)].w).c_str());
        }
        CHECK(std::strcmp(r.vertex, "VfxU100Xh") == 0);
        CHECK_EQ(r.how, HM_CA_COVER_PIPELINE);
    }
}

// PBGRAXm_TimgA2S1Xhf_Isrc: labelled VfxLlnXh, but created with the 48-byte layout (task 1.3), table VfxU100Xh.
TEST(ca_cover_label_vfxllnxh_on_the_48_byte_layout_is_drawn_with_the_table_vertex) {
    const std::vector<Variant> vs = library();
    Pipeline p;
    p.label = indexOf(vs, "VfxLlnXh");
    p.library = true;
    p.table = p.paired = indexOf(vs, "VfxU100Xh");
    p.r = kTex0A;
    p.d = kLayout48;
    const int selected = selectToday(p, Order::Committed);
    CHECK(isBugCondition(vs, selected, p));
    Result r = runGuard(vs, selected, p);
    if (std::strcmp(r.vertex, "VfxU100Xh") != 0) {
        std::fprintf(stderr, "  counterexample: label VfxLlnXh, table VfxU100Xh, R %s, D 0x%x: returned %s (%s), "
                     "expected VfxU100Xh (PIPELINE)\n", identities(p.r).c_str(), p.d, r.vertex, howName(r.how));
    }
    CHECK(std::strcmp(r.vertex, "VfxU100Xh") == 0);
    CHECK_EQ(r.how, HM_CA_COVER_PIPELINE);
}

// Task 1.5 decision 1 and the fn rule: the same pipeline on the 16-byte Lln layout (D = {0, 3}). VfxU100Xh covers
// but reads attribute 1, the other VfxU* variants read 1 or 2, and VfxXgh specialises another function: nothing is
// usable, so the selected vertex stays, with NONE (the zero read and its note, as today).
TEST(ca_cover_lln_layout_keeps_the_selected_vertex_with_none) {
    const std::vector<Variant> vs = library();
    Pipeline p;
    p.label = indexOf(vs, "VfxLlnXh");
    p.library = true;
    p.table = p.paired = indexOf(vs, "VfxU100Xh");
    p.r = kTex0A;
    p.d = kLayoutLln;
    const int selected = selectToday(p, Order::Committed);
    CHECK_EQ(selected, indexOf(vs, "VfxLlnXh"));
    CHECK(!isBugCondition(vs, selected, p));
    CHECK(!usable(vs, indexOf(vs, "VfxXgh"), selected, p));   // covers, reads no attribute, but another function
    Result r = runGuard(vs, selected, p);
    if (std::strcmp(r.vertex, "VfxLlnXh") != 0 || r.how != HM_CA_COVER_NONE) {
        std::fprintf(stderr, "  descriptor case: label VfxLlnXh, table VfxU100Xh, D 0x%x: returned %s (%s), expected "
                     "VfxLlnXh (NONE)\n", p.d, r.vertex, howName(r.how));
    }
    CHECK(std::strcmp(r.vertex, "VfxLlnXh") == 0);
    CHECK_EQ(r.how, HM_CA_COVER_NONE);
}

// Property 1 over seeded random worlds: up to 7 variants over up to 8 identities, 4 attributes and 2 functions,
// random R and D, a random label, table, pairing and library-pipeline flag, under both selection orders.
// **Validates: Requirements 1.2, 1.3, 2.2, 2.3**
TEST(ca_cover_property_1_bug_condition_gets_the_first_usable_candidate) {
    std::mt19937 rng(0x6ca0c0e5u);
    auto pick = [&](unsigned n) { return (unsigned)(rng() % n); };
    int bugCases = 0, failed = 0, printed = 0;
    for (int round = 0; round < 4000; round++) {
        const unsigned nVariants = 1 + pick(7), nIds = 1 + pick(8), idMask = (1u << nIds) - 1;
        std::vector<Variant> vs;
        for (unsigned i = 0; i < nVariants; i++) {
            vs.push_back(Variant{"v" + std::to_string(nVariants - 1 - i), (unsigned)rng() & idMask, (unsigned)rng() & 0xfu,
                                 pick(5) == 0 ? kGen : kSpc});   // names out of index order, so sorting matters
        }
        Pipeline p;
        p.library = pick(2) == 0;
        p.table = p.library ? (int)pick(nVariants) : -1;
        p.paired = pick(4) == 0 ? -1 : (int)pick(nVariants);
        p.runtime = !p.library && pick(4) != 0;
        const unsigned labelKind = pick(4);
        p.label = labelKind == 0 ? -2 : labelKind == 1 ? -1 : (int)pick(nVariants);
        p.r = ((unsigned)rng() & idMask) | (1u << pick(nIds));   // a fragment reads at least one identity
        p.d = (unsigned)rng() & 0xfu;
        for (Order order : {Order::Committed, Order::Ram}) {
            const int selected = selectToday(p, order);
            if (!isBugCondition(vs, selected, p)) continue;
            bugCases++;
            hm_ca_cover_how wantHow;
            const int want = expectedChoice(vs, selected, p, &wantHow);
            Result r = runGuard(vs, selected, p);
            const int got = r.vertex != nullptr ? indexOf(vs, r.vertex) : -1;
            const bool ok = got >= 0 && usable(vs, got, selected, p) && got == want && r.how == wantHow;
            if (ok) continue;
            failed++;
            if (printed++ < 5) {
                std::fprintf(stderr, "  counterexample (round %d, %s order): label %s, table %s, pairing %s, library %d, "
                             "R %s, D 0x%x; selected %s writes %s: returned %s (%s), expected %s (%s)\n", round,
                             order == Order::Committed ? "committed" : "RAM",
                             p.label >= 0 ? vs[p.label].name.c_str() : p.label == -1 ? "non-library" : "none",
                             p.table >= 0 ? vs[p.table].name.c_str() : "none",
                             p.paired >= 0 ? vs[p.paired].name.c_str() : "none", p.library, identities(p.r).c_str(), p.d,
                             vs[selected].name.c_str(), identities(vs[selected].w).c_str(), r.vertex ? r.vertex : "(null)",
                             howName(r.how), vs[want].name.c_str(), howName(wantHow));
                for (const Variant& v : vs) {
                    std::fprintf(stderr, "    %s: W %s, A 0x%x, fn %d\n", v.name.c_str(), identities(v.w).c_str(), v.a, v.fn);
                }
            }
        }
    }
    std::fprintf(stderr, "  property 1: %d bug-condition cases, %d failed\n", bugCases, failed);
    CHECK(bugCases > 100);   // the generator reaches the bug condition
    CHECK_EQ(failed, 0);
}

// --- Property 2: Preservation (task 3) -----------------------------------------------------------------------------
// Observed on the unfixed stub and required of the fix: outside the bug condition the selected vertex is drawn. HOW is
// KEPT when the selected vertex covers; when it does not and nothing is usable, HOW is NONE once the guard asks (the
// fix) and KEPT when it asks nothing (the stub, which is today's behaviour). The oracle is asked about each distinct
// non-NULL label at most once, and only about labels it was offered; a covering selected vertex is the only one asked.
namespace {

// the guard over a hand-built candidate list (NULLs and repeats allowed), with an oracle that answers -1 instead of 0
// for the variants whose bit is set in MINUS_ONE
struct RawOracle {
    const std::vector<Variant>* vs;
    const Pipeline* p;
    int selected;
    unsigned minusOne;
    std::vector<std::string> asked;
};

int rawOracleCovers(void* ctx, const char* vertex) {
    RawOracle* o = static_cast<RawOracle*>(ctx);
    o->asked.push_back(vertex != nullptr ? vertex : "(null)");
    const int i = vertex != nullptr ? indexOf(*o->vs, vertex) : -1;
    if (i < 0) return -1;
    if (usable(*o->vs, i, o->selected, *o->p)) return 1;
    return (o->minusOne >> i) & 1u ? -1 : 0;
}

Result runGuardRaw(const std::vector<Variant>& vs, int selected, const Pipeline& p, const char* table, const char* paired,
                   const std::vector<const char*>& lib, unsigned minusOne) {
    RawOracle o{&vs, &p, selected, minusOne, {}};
    hm_ca_cover_how how = HM_CA_COVER_PIPELINE;   // a value the guard must overwrite for a non-bug case
    const char* got = hm_ca_cover_vertex(vs[selected].name.c_str(), table, paired, lib.data(), lib.size(), rawOracleCovers,
                                         &o, &how);
    return Result{got, how, o.asked};
}

// "" when R keeps today's choice for a pipeline outside the bug condition, else what is wrong
std::string preservationFailure(const Result& r, const std::vector<Variant>& vs, int selected, const Pipeline& p,
                                const std::set<std::string>& offered) {
    const std::string sel = vs[selected].name;
    if (r.vertex == nullptr || sel != r.vertex) return "returned " + std::string(r.vertex ? r.vertex : "(null)") + ", not " + sel;
    if (covers(vs[selected], p)) {
        if (r.how != HM_CA_COVER_KEPT) return std::string("selected covers, but HOW is ") + howName(r.how);
        for (const std::string& a : r.asked) {
            if (a != sel) return "selected covers, but the oracle was asked about " + a;
        }
    } else if (!(r.how == HM_CA_COVER_NONE || (r.how == HM_CA_COVER_KEPT && r.asked.empty()))) {
        return std::string("nothing usable, but HOW is ") + howName(r.how) + " after " + std::to_string(r.asked.size()) + " questions";
    }
    std::set<std::string> seen;
    for (const std::string& a : r.asked) {
        if (a == "(null)") return "the oracle was asked about NULL";
        if (offered.count(a) == 0) return "the oracle was asked about " + a + ", which was not offered";
        if (!seen.insert(a).second) return "the oracle was asked twice about " + a;
    }
    return "";
}

void expectPreserved(const char* what, const Result& r, const std::vector<Variant>& vs, int selected, const Pipeline& p,
                     const std::set<std::string>& offered) {
    const std::string bad = preservationFailure(r, vs, selected, p, offered);
    CHECK(bad.empty());
    if (!bad.empty()) std::fprintf(stderr, "  %s: %s\n", what, bad.c_str());
}

std::set<std::string> allNames(const std::vector<Variant>& vs) {
    std::set<std::string> s;
    for (const Variant& v : vs) s.insert(v.name);
    return s;
}

}  // namespace

// 3.4: pipelines whose selected vertex covers keep it, whichever path selected it. PBGRABsovXm_Tc3bA2S1Xhf_Idst by label
// (VfxXh; the fragment reads the destination and no texcoord), a VfxLlnXh + TcmaA2LlnXhf library pipeline on the Lln
// layout, and the RAM build's run-time branch for PBGRABdotXm_TimgA2Xhf_Icir (label VfxXh, drawn with VfxU100Xh) and
// PBGRABsovXm_TmuaA2Xhf_IsrcCcl_Isrg (label VfxXh, drawn with VfxU110Xh). R is synthetic.
// **Validates: Requirements 3.4**
TEST(ca_cover_preservation_covering_selection_is_kept) {
    const std::vector<Variant> vs = library();
    const int vfxXh = indexOf(vs, "VfxXh"), vfxLlnXh = indexOf(vs, "VfxLlnXh");
    const int u100 = indexOf(vs, "VfxU100Xh"), u110 = indexOf(vs, "VfxU110Xh");
    struct Case {
        const char* what;
        Pipeline p;
        Order order;
        int want;
    };
    Pipeline tc3b, tcma, timg, tmua;
    tc3b.label = vfxXh, tc3b.library = true, tc3b.table = vfxXh, tc3b.paired = vfxXh, tc3b.r = 0, tc3b.d = kLayout48;
    tcma.label = vfxLlnXh, tcma.library = true, tcma.table = vfxLlnXh, tcma.paired = vfxLlnXh, tcma.r = kColor, tcma.d = kLayoutLln;
    timg.label = vfxXh, timg.runtime = true, timg.paired = u100, timg.r = kTex0A, timg.d = kLayout48;
    tmua.label = vfxXh, tmua.runtime = true, tmua.paired = u110, tmua.r = kTex0A | kTex1A, tmua.d = kLayout48;
    const Case cases[] = {
        {"VfxXh + Tc3bA2S1Xhf_Idst (committed)", tc3b, Order::Committed, vfxXh},
        {"VfxXh + Tc3bA2S1Xhf_Idst (RAM)", tc3b, Order::Ram, vfxXh},
        {"VfxLlnXh + TcmaA2LlnXhf (committed)", tcma, Order::Committed, vfxLlnXh},
        {"VfxU100Xh + TimgA2Xhf_Icir (RAM, run-time branch)", timg, Order::Ram, u100},
        {"VfxU110Xh + TmuaA2Xhf_IsrcCcl_Isrg (RAM, run-time branch)", tmua, Order::Ram, u110},
    };
    for (const Case& c : cases) {
        const int selected = selectToday(c.p, c.order);
        CHECK_EQ(selected, c.want);
        if (selected < 0) continue;
        CHECK(covers(vs[selected], c.p));
        CHECK(!isBugCondition(vs, selected, c.p));
        Result r = runGuard(vs, selected, c.p);
        expectPreserved(c.what, r, vs, selected, c.p, allNames(vs));
        CHECK_EQ(r.how, HM_CA_COVER_KEPT);
    }
}

// Task 1.5 decision 1 with the label VfxXh: table VfxU100Xh covers but reads attribute 1, which the Lln layout (D = {0, 3})
// does not describe; VfxXgh is another function. Nothing is usable, so VfxXh stays (its zero read and note, as today).
// The candidates come with a NULL, repeats of the selected vertex, and -1 answers for the unusable ones.
// **Validates: Requirements 2.4, 3.3**
TEST(ca_cover_preservation_attribute_outside_the_descriptor_keeps_the_selected_vertex) {
    const std::vector<Variant> vs = library();
    Pipeline p;
    p.label = indexOf(vs, "VfxXh");
    p.library = true;
    p.table = p.paired = indexOf(vs, "VfxU100Xh");
    p.r = kTex0A;
    p.d = kLayoutLln;
    const int selected = selectToday(p, Order::Committed);
    CHECK_EQ(selected, indexOf(vs, "VfxXh"));
    CHECK(!isBugCondition(vs, selected, p));
    CHECK(covers(vs[p.table], p) && !usable(vs, p.table, selected, p));
    for (unsigned minusOne : {0u, 0x7fu}) {
        Result r = runGuard(vs, selected, p);
        expectPreserved("VfxXh, table VfxU100Xh, Lln layout", r, vs, selected, p, allNames(vs));
        std::vector<const char*> lib = {nullptr, "VfxXh", "VfxU100Xh", nullptr, "VfxXh", "VfxXgh", "VfxU110Xh", nullptr};
        Result raw = runGuardRaw(vs, selected, p, "VfxXh", "VfxXh", lib, minusOne);
        expectPreserved("VfxXh, NULL and repeated candidates", raw, vs, selected, p, allNames(vs));
        raw = runGuardRaw(vs, selected, p, nullptr, nullptr, {}, minusOne);
        expectPreserved("VfxXh, no candidates at all", raw, vs, selected, p, allNames(vs));
    }
}

// Property 2 over seeded random worlds (the generator of Property 1, another seed): every pipeline outside the bug
// condition, under both selection orders, keeps its selected vertex. The candidate list carries NULLs and repeats, the
// table or pairing may be NULL or the selected vertex itself, and unusable answers are 0 or -1 at random.
// **Validates: Requirements 2.4, 3.3, 3.4, 3.5, 3.6**
TEST(ca_cover_property_2_preservation_outside_the_bug_condition) {
    std::mt19937 rng(0x6ca0c0e6u);
    auto pick = [&](unsigned n) { return (unsigned)(rng() % n); };
    int coveringCases = 0, noneCases = 0, attributeCases = 0, failed = 0, printed = 0;
    for (int round = 0; round < 4000; round++) {
        const unsigned nVariants = 1 + pick(7), nIds = 1 + pick(8), idMask = (1u << nIds) - 1;
        std::vector<Variant> vs;
        for (unsigned i = 0; i < nVariants; i++) {
            vs.push_back(Variant{"v" + std::to_string(nVariants - 1 - i), (unsigned)rng() & idMask, (unsigned)rng() & 0xfu,
                                 pick(5) == 0 ? kGen : kSpc});
        }
        Pipeline p;
        p.library = pick(2) == 0;
        p.table = p.library ? (int)pick(nVariants) : -1;
        p.paired = pick(4) == 0 ? -1 : (int)pick(nVariants);
        p.runtime = !p.library && pick(4) != 0;
        const unsigned labelKind = pick(4);
        p.label = labelKind == 0 ? -2 : labelKind == 1 ? -1 : (int)pick(nVariants);
        p.r = ((unsigned)rng() & idMask) | (1u << pick(nIds));
        p.d = (unsigned)rng() & 0xfu;
        for (Order order : {Order::Committed, Order::Ram}) {
            const int selected = selectToday(p, order);
            if (selected < 0 || isBugCondition(vs, selected, p)) continue;   // the function's own module: no guard
            const bool selCovers = covers(vs[selected], p);
            bool someCovers = false;
            for (const Variant& v : vs) someCovers = someCovers || covers(v, p);
            (selCovers ? coveringCases : noneCases)++;
            if (!selCovers && someCovers) attributeCases++;   // covered, but not usable (attributes or function)
            // the candidates: the sorted library with NULLs and repeats mixed in
            std::vector<const char*> lib;
            for (int i : sortedByName(vs)) {
                if (pick(4) == 0) lib.push_back(nullptr);
                lib.push_back(vs[i].name.c_str());
                if (pick(6) == 0) lib.push_back(vs[pick(nVariants)].name.c_str());
            }
            const unsigned tableKind = pick(4), pairedKind = pick(4);
            const char* table = tableKind == 0 ? nullptr : tableKind == 1 ? vs[selected].name.c_str()
                                : p.table >= 0 ? vs[p.table].name.c_str() : nullptr;
            const char* paired = pairedKind == 0 ? nullptr : pairedKind == 1 ? table
                                 : p.paired >= 0 ? vs[p.paired].name.c_str() : nullptr;
            const unsigned minusOne = (unsigned)rng() & 0x7fu;
            Result r = runGuardRaw(vs, selected, p, table, paired, lib, minusOne);
            const std::string bad = preservationFailure(r, vs, selected, p, allNames(vs));
            if (bad.empty()) continue;
            failed++;
            if (printed++ < 5) {
                std::fprintf(stderr, "  counterexample (round %d, %s order): selected %s writes %s, R %s, D 0x%x, table %s, "
                             "pairing %s: %s\n", round, order == Order::Committed ? "committed" : "RAM",
                             vs[selected].name.c_str(), identities(vs[selected].w).c_str(), identities(p.r).c_str(), p.d,
                             table ? table : "NULL", paired ? paired : "NULL", bad.c_str());
                for (const Variant& v : vs) {
                    std::fprintf(stderr, "    %s: W %s, A 0x%x, fn %d\n", v.name.c_str(), identities(v.w).c_str(), v.a, v.fn);
                }
            }
        }
    }
    std::fprintf(stderr, "  property 2: %d covering, %d nothing usable (%d of them covered but not usable), %d failed\n",
                 coveringCases, noneCases, attributeCases, failed);
    CHECK(coveringCases > 100);
    CHECK(noneCases > 100);
    CHECK(attributeCases > 50);
    CHECK_EQ(failed, 0);
}

// --- The oracle's meaning, held to the compiler (MESA=1) -----------------------------------------------------------
// On the A650 the plugin's oracle reads "covers" as an empty link note. Over every pairing of the synthetic vertex
// stages (var_ab_vs, var_a_vs) with the fragment stages (var_ba_fs, var_b_fs, var_ac_fs): the note is empty exactly when
// the fragment's identities R are a subset of the vertex's W, and otherwise it names each identity of R \ W and none
// of R ∩ W. Off the A650 (compileStage, the default chip) the fragment records no identities, the note is always empty,
// and a pair is refused with today's sentence exactly when the fragment reads a varying slot the vertex does not write;
// otherwise it links by slot (var_a_vs + var_b_fs feeds a into b, as VfxXh + TimgA2Xhf_Icir feeds color.xy into
// texcoord0A off the A650: today's behaviour, out of scope).
namespace {

std::vector<uint8_t> varBitcode(const char* name) {
    std::ifstream f(std::string("out/host/kernels/") + name + ".bc", std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// the first varying slot the fragment reads that the vertex does not write, or -1
int firstUnwrittenSlot(const hakios::metal::Stage& vs, const hakios::metal::Stage& fs) {
    for (uint32_t i = 0; i < fs.numInputs; i++) {
        const hakios::metal::StageInput& in = fs.inputs[i];
        if (in.sysval || in.compmask == 0 || in.slot < 32) continue;
        bool written = false;
        for (uint32_t j = 0; j < vs.numOutputs; j++) written = written || vs.outputs[j].slot == in.slot;
        if (!written) return in.slot;
    }
    return -1;
}

const char* const kVarVs[] = {"var_ab_vs", "var_a_vs"};
const char* const kVarFs[] = {"var_ba_fs", "var_b_fs", "var_ac_fs"};

}  // namespace

// **Validates: Requirements 2.4, 3.3**
TEST(ca_cover_a650_note_is_empty_exactly_when_the_vertex_covers) {
    using namespace hakios::metal;
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: the A650 coverage pairs did not run)\n");
        return;
    }
    std::map<std::string, std::shared_ptr<CompiledStage>> s;
    for (const char* name : {"var_ab_vs", "var_a_vs", "var_ba_fs", "var_b_fs", "var_ac_fs"}) {
        std::vector<uint8_t> bc = varBitcode(name);
        std::string why;
        s[name] = bc.empty() ? nullptr : std::shared_ptr<CompiledStage>(compileStageForChip(bc.data(), bc.size(), a6xx::kA650ChipId, &why));
        CHECK(s[name] != nullptr);
        if (s[name] == nullptr) {
            std::printf("       %s: %s\n", name, bc.empty() ? "out/host/kernels/*.bc missing" : why.c_str());
            return;
        }
    }
    int covering = 0, notCovering = 0;
    for (const char* v : kVarVs) {
        for (const char* f : kVarFs) {
            const std::vector<std::string>& w = s[v]->varyingNames;
            const std::vector<std::string>& r = s[f]->varyingNames;
            CHECK(!r.empty());
            std::vector<std::string> unserved;
            for (const std::string& id : r) {
                if (std::find(w.begin(), w.end(), id) == w.end()) unserved.push_back(id);
            }
            std::string why;
            auto prog = linkProgram(s[v], s[f], PixelFormat::kRGBA8Unorm, &why);
            CHECK(prog != nullptr);   // VAR1: the A650 links a pair by identity and refuses none for an unserved varying
            if (prog == nullptr) {
                std::fprintf(stderr, "  %s+%s (A650): refused: \"%s\"\n", v, f, why.c_str());
                continue;
            }
            const std::string& note = prog->linkNote;
            (unserved.empty() ? covering : notCovering)++;
            CHECK(note.empty() == unserved.empty());
            for (const std::string& id : r) {
                const bool named = note.find("'" + id + "'") != std::string::npos;
                const bool isUnserved = std::find(unserved.begin(), unserved.end(), id) != unserved.end();
                CHECK(named == isUnserved);
                if (named != isUnserved) std::fprintf(stderr, "  %s+%s (A650): %s %s in the note \"%s\"\n", v, f, id.c_str(),
                                                      named ? "named" : "missing", note.c_str());
            }
        }
    }
    CHECK(covering >= 2);      // var_ab_vs with var_ba_fs and var_b_fs
    CHECK(notCovering >= 3);   // var_ab_vs + var_ac_fs, var_a_vs + var_ba_fs, var_a_vs + var_ac_fs
}

// **Validates: Requirements 3.6**
TEST(ca_cover_default_chip_note_is_always_empty_and_pairs_link_by_slot) {
    using namespace hakios::metal;
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: the default-chip coverage pairs did not run)\n");
        return;
    }
    std::map<std::string, std::shared_ptr<CompiledStage>> s;
    for (const char* name : {"var_ab_vs", "var_a_vs", "var_ba_fs", "var_b_fs", "var_ac_fs"}) {
        std::vector<uint8_t> bc = varBitcode(name);
        std::string why;
        s[name] = bc.empty() ? nullptr : std::shared_ptr<CompiledStage>(compileStage(bc.data(), bc.size(), &why));
        CHECK(s[name] != nullptr);
        if (s[name] == nullptr) {
            std::printf("       %s: %s\n", name, bc.empty() ? "out/host/kernels/*.bc missing" : why.c_str());
            return;
        }
    }
    for (const char* f : kVarFs) CHECK(s[f]->varyingNames.empty());
    int linked = 0, refused = 0;
    for (const char* v : kVarVs) {
        for (const char* f : kVarFs) {
            const int missing = firstUnwrittenSlot(s[v]->stage, s[f]->stage);
            std::string why;
            auto prog = linkProgram(s[v], s[f], PixelFormat::kRGBA8Unorm, &why);
            if (missing < 0) {
                linked++;
                CHECK(prog != nullptr);
                if (prog == nullptr) std::fprintf(stderr, "  %s+%s (default chip): refused: \"%s\"\n", v, f, why.c_str());
                if (prog != nullptr) CHECK(prog->linkNote.empty());
            } else {
                refused++;
                CHECK(prog == nullptr);
                const std::string want = "the fragment stage reads varying slot " + std::to_string(missing) + " (VAR" +
                                         std::to_string(missing - 32) + "), which the vertex stage does not write";
                CHECK(why == want);
                if (why != want) std::fprintf(stderr, "  %s+%s (default chip): got \"%s\"\n  expected \"%s\"\n", v, f, why.c_str(), want.c_str());
            }
        }
    }
    CHECK(linked >= 1);    // at least var_a_vs + var_b_fs, which links by slot without covering
    CHECK(refused >= 1);   // at least var_a_vs + var_ac_fs (VAR1)
}
