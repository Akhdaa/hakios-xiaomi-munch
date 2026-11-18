// The RAM plug-in's stand-in for a CoreAnimation fragment variant the library lacks (spec metal-ca-reachability-pink),
// as the pure function hm_ca_label_nearest of userland/src/hm_ca_label.h. HM_CA_LABEL_OLD is today's hm_ca_nearest;
// HM_CA_LABEL_NEW is the rule the plug-in will use (until task 5.1, the same as OLD).
//
// A test-local oracle, with its own parse (std::regex, the plug-in's expression), answers the owner's rule of the stop
// gate (task 2): a stand-in must have the asked effect key (T, B, D, A, L and every input term, kind and modifiers, in
// order) and differ only in the harmless set H = {±c when neither label reads buffer 4 colorP, ±x, ±u, -n, ±S1 when
// neither label reads a destination attachment}. A label reads colorP when it has flag g or an input modifier N<digit>
// (the 27 labels of the library that bind buffer 4, logs/munch-metal/coreanimation/reachability/colorp.sh); it reads a
// destination as readsDestination says (destread.py there). When no library label qualifies, the result is the D2
// fallback: NULL, with the reason set.
//
// Property 1 (Bug Condition): for every asked label the library lacks, the result qualifies if some library label
// does, and is NULL with a reason otherwise. On the unfixed rule this fails (the exploration of task 3).
//
// Property 2 (Preservation): outside the bug condition the new rule gives what the old one gives: every library label
// (which the plug-in compiles from its own variant without asking the rule), every missing label whose old stand-in
// already qualifies, every label outside the grammar (NULL); the old rule (metal_m2v=1, metal_ca_nearest=old) stays
// today's hm_ca_nearest for every label; random strings crash neither rule.
//
// The names are CoreAnimation's variant labels used as plain strings (decision D5: names only, no Apple code);
// fixtures/ca_frag_labels.hpp holds the library's 444, checked against sysimage/ when it is present.
//
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <regex>
#include <set>
#include <string>
#include <vector>
#include "../../userland/src/hm_ca_label.h"
#include "fixtures/ca_frag_labels.hpp"
#include "test.hpp"

namespace {

const std::string kPrefix = HM_CA_LABEL_PREFIX;

// the library: full labels, in the fixture's order
std::vector<std::string> libraryLabels() {
    std::vector<std::string> out;
    for (const char* s : fixtures::kCaFragLabels) out.push_back(kPrefix + s);
    return out;
}

struct Spec {
    std::string part[6];   // T B D A S L
    std::string flags;
    std::vector<std::string> inputs;
};

bool parseSpec(const std::string& spec, Spec* out) {
    static const std::regex form("^(T[a-z0-9]{3})?(B[a-z]{3})?(D[a-z]{2})?(A[0-9])(S[0-9])?(L[a-z]{2})?X([a-z]*)((?:_[A-Za-z0-9]+)*)$");
    std::smatch m;
    if (!std::regex_match(spec, m, form)) return false;
    for (int i = 0; i < 6; i++) out->part[i] = m[i + 1].str();
    out->flags = m[7].str();
    out->inputs.clear();
    const std::string in = m[8].str();
    for (size_t at = 0; at < in.size();) {
        size_t next = in.find('_', at + 1);
        if (next == std::string::npos) next = in.size();
        out->inputs.push_back(in.substr(at + 1, next - at - 1));
        at = next;
    }
    return true;
}

bool readsColorP(const Spec& s) {
    static const std::regex n("N[0-9]");
    if (s.flags.find('g') != std::string::npos) return true;
    for (const std::string& t : s.inputs) {
        if (t.size() > 4 && std::regex_search(t.begin() + 4, t.end(), n)) return true;
    }
    return false;
}

// A label reads a destination attachment (framebuffer fetch of dest_N) when it has a B part (a blend done in the
// shader), T Tbds/Taup/Tvcm, flag g, an input Idst or an input modifier M<digit>. Checked against the modules'
// bindings (logs/munch-metal/coreanimation/reachability/destread.py): it names all 161 library labels that read a
// dest_N and 5 more (the Bclr ones, which bind dest_0 unused), so it errs on the side of "reads".
bool readsDestination(const Spec& s) {
    static const std::regex m("M[0-9]");
    if (!s.part[1].empty() || s.part[0] == "Tbds" || s.part[0] == "Taup" || s.part[0] == "Tvcm") return true;
    if (s.flags.find('g') != std::string::npos) return true;
    for (const std::string& t : s.inputs) {
        if (t.compare(0, 4, "Idst") == 0) return true;
        if (t.size() > 4 && std::regex_search(t.begin() + 4, t.end(), m)) return true;
    }
    return false;
}

// "" when GIVEN may stand in for ASKED under the gate's rule, else what differs. ±S1 is in H only when neither label
// reads a destination (the orchestrator's reading of the gate, task 4): for a label that reads one, S1 moves the read
// from attachment 0 to 1 (inspection.md §2, 5 of 58 pairs), which is another effect.
std::string disqualifies(const Spec& asked, const Spec& given) {
    static const char* const names[6] = {"T", "B", "D", "A", "S", "L"};
    for (int i = 0; i < 6; i++) {
        if (i == 4) continue;
        if (asked.part[i] != given.part[i]) return std::string(names[i]) + " " + asked.part[i] + " -> " + given.part[i];
    }
    if (asked.part[4] != given.part[4]) {
        const bool s1 = (asked.part[4].empty() && given.part[4] == "S1") || (asked.part[4] == "S1" && given.part[4].empty());
        if (!s1) return "S " + asked.part[4] + " -> " + given.part[4];
        if (readsDestination(asked) || readsDestination(given)) {
            return "S " + asked.part[4] + " -> " + given.part[4] + " (destination read: another attachment)";
        }
    }
    if (asked.inputs != given.inputs) {
        std::string a, g;
        for (const std::string& t : asked.inputs) a += "_" + t;
        for (const std::string& t : given.inputs) g += "_" + t;
        return "inputs " + a + " -> " + g;
    }
    const bool colorP = readsColorP(asked) || readsColorP(given);
    std::string outside;
    for (char c : asked.flags) {   // dropped
        if (given.flags.find(c) != std::string::npos) continue;
        if (c == 'x' || c == 'u' || c == 'n' || (c == 'c' && !colorP)) continue;
        outside += std::string(outside.empty() ? "" : " ") + "-" + c;
    }
    for (char c : given.flags) {   // added
        if (asked.flags.find(c) != std::string::npos) continue;
        if (c == 'x' || c == 'u' || (c == 'c' && !colorP)) continue;
        outside += std::string(outside.empty() ? "" : " ") + "+" + c;
    }
    if (!outside.empty()) return "flags " + asked.flags + " -> " + given.flags + " (" + outside + " outside H" +
                                 (colorP && outside.find('c') != std::string::npos ? ", colorP read" : "") + ")";
    return "";
}

std::string spec(const std::string& label) { return label.compare(0, kPrefix.size(), kPrefix) == 0 ? label.substr(kPrefix.size()) : label; }

struct Parsed {
    std::string label;
    Spec spec;
};

// the library, parsed once (std::regex is slow)
const std::vector<Parsed>& parsedLibrary() {
    static const std::vector<Parsed> all = [] {
        std::vector<Parsed> out;
        for (const std::string& l : libraryLabels()) {
            Parsed p{l, {}};
            if (parseSpec(spec(l), &p.spec)) out.push_back(p);
        }
        return out;
    }();
    return all;
}

// the library labels other than ASKED_LABEL that may stand in for ASKED (a left-out label is the only one removed)
std::vector<std::string> qualifying(const Spec& asked, const std::string& askedLabel) {
    std::vector<std::string> out;
    for (const Parsed& p : parsedLibrary()) {
        if (p.label != askedLabel && disqualifies(asked, p.spec).empty()) out.push_back(p.label);
    }
    return out;
}

struct Outcome {
    const char* given;
    const char* why;
};

Outcome runRule(const std::string& askedSpec, const std::vector<std::string>& lib, hm_ca_label_mode mode) {
    std::vector<const char*> names;
    for (const std::string& l : lib) names.push_back(l.c_str());
    const std::string asked = kPrefix + askedSpec;
    Outcome o{nullptr, nullptr};
    o.given = hm_ca_label_nearest(asked.c_str(), names.data(), names.size(), nullptr, mode, &o.why);
    return o;
}

// "" when OUT is the expected behaviour for ASKED (a parsed spec the library lacks), else the counterexample's text
// (Q: the library labels that may stand in for it)
std::string violation(const Spec& asked, const Outcome& out, const std::vector<std::string>& q) {
    if (out.given != nullptr) {
        Spec g;
        if (!parseSpec(spec(out.given), &g)) return std::string("given ") + out.given + " does not parse";
        const std::string bad = disqualifies(asked, g);
        if (bad.empty()) return "";
        return "given " + spec(out.given) + ": " + bad + "; expected " +
               (q.empty() ? std::string("the D2 fallback (no same-effect variant)") : spec(q.front()) +
                (q.size() > 1 ? " (or another of " + std::to_string(q.size()) + " same-effect variants)" : ""));
    }
    if (!q.empty()) return "nothing given (" + std::string(out.why ? out.why : "no reason") + "), but " + spec(q.front()) + " has the same effect";
    if (out.why == nullptr || out.why[0] == '\0') return "nothing given and no reason set";
    if (std::strcmp(out.why, HM_CA_LABEL_UNPARSED) == 0) return "a parsed label reported as not parsing";
    // the D2 outcome (task 5.2): left out, never the function's own module or another program
    if (hm_ca_label_outcome_of(out.given, out.why, HM_CA_LABEL_NEW) != HM_CA_LABEL_LEAVE_OUT) return "nothing given, but not the D2 outcome";
    return "";
}

// the 17 November run's 15 stand-ins (inspection.md §1), and TvdkA2Xhf_Isrc of 14 November: asked, today's
// stand-in (nullptr: "nothing of the same kind"), the gate's expected one (nullptr: the D2 fallback)
struct RunCase {
    const char* asked;
    const char* today;
    const char* expected;
};

const RunCase kRunCases[] = {
    {"TimgBdsoA2Xhfn_Isrc", "TimgBdsoA2Xhfcn_Isrc", "TimgBdsoA2Xhfcn_Isrc"},              // 1
    {"TmuaA2Xhfn_Isrc_Isqr", "TmuaA2Xhf_Isrc_Isqr", "TmuaA2Xhf_Isrc_Isqr"},              // 2
    {"TmuaA2S1Xhf_Isrc_Isqr", "TmuaA2S1Xhfcx_Isrc_Isqr", "TmuaA2S1Xhfcx_Isrc_Isqr"},     // 3
    {"TimgA2S1Xhf_Ixrg", "TimgA2S1Xhfcx_Ixrg", "TimgA2S1Xhfcx_Ixrg"},                    // 4
    {"TmuaA2S1Xhf_Ixrg_Isqr", "TmuaA2S1Xhfcx_Isrc_Isqr", "TmuaA2Xhfcx_Ixrg_Isqr"},       // 5
    {"TimgA2Xhfn_IsrcN3Oc3mtc4", "TimgA2Xhf_IsrcN3Oc3mtc4", "TimgA2Xhf_IsrcN3Oc3mtc4"},  // 6
    {"TmuaA2Xhf_IsrcCcl_Isrg", "TmuaA2Xhfcx_IsrcCcl_Isrg", "TmuaA2Xhfcx_IsrcCcl_Isrg"},  // 7
    {"TcimA2Xhfn_IsrcN3Oc3mtc4", "TcimA2Xhf_IsrcN3Oc3mtc4", "TcimA2Xhf_IsrcN3Oc3mtc4"},  // 8
    {"TcimA2S1Xhfn_Isrc", "TcimA2S1Xhf_Isrc", "TcimA2S1Xhf_Isrc"},                      // 9
    {"TmuaA2Xhf_IsrcM1_Isqr", "TmuaA2Xhf_IsrcM1_Icir", nullptr},                         // 10
    {"Tm1aA2S1Xhf_Isup_Isup", "Tm1aA2S1Xhfcx_Isup_Isup", "Tm1aA2S1Xhfcx_Isup_Isup"},     // 11
    {"TmapA2Xhf_Idst_Isr1Clm", "TmapA2Xhfcx_Idst_Isr1Clm", "TmapA2Xhfcx_Idst_Isr1Clm"},  // 12
    {"TcimBdsoA2Xhf_Icir", "TcimBdsoA2Xhf_Ialp", nullptr},                               // 13
    {"TcimA2S1Xhfn_IsrcN3Oc3mtc4", "TcimA2S1Xhf_IalpCcl", "TcimA2Xhf_IsrcN3Oc3mtc4"},    // 14
    {"TbdsA2Xhf_Isrg_IsrcM1", "TbdsA2Xhf_Icir_IsrcM1", nullptr},                         // 15
    {"TvdkA2Xhf_Isrc", nullptr, nullptr},                                                // 14 Nov
};

// the other 11 labels logged in the 14 and 17 November runs (scans/nearest-old-all.out), with today's stand-in
const RunCase kOlderRunCases[] = {
    {"Tc33A2S1Xhf_Idst", "Tc33A2S1Xhfcx_Idst", nullptr},
    {"Tc33A2Xhf_Isrc", "Tc33A2Xhfcx_Isrc", nullptr},
    {"TcimA2Xhf_Isqr", "TcimA2Xhfcx_Isqr", nullptr},
    {"TcimBpldA2Xhf_Isup", "TcimBpldA2Xhfcx_Isup", nullptr},
    {"TcimBvcmA2Xhf_Icir", "TcimBvcmA2Xhfcx_Icir", nullptr},
    {"TcmaBpldA2LlnXhf", "TcmaBpldA2LlnXhfcx", nullptr},
    {"TimgA2S1Xhf_Ired", "TimgA2S1Xhfcx_Ired", nullptr},
    {"TimgA2S1Xhfn_IsrcN3Oc3mtc4", "TimgA2S1Xhfcx_IsrcN3Oc3mtc4", nullptr},
    {"TimgA2Xhf_Isqr", "TimgA2Xhf_Ialp", nullptr},
    {"TmpcA2S1Xhf_Idst", "TmpcA2S1Xhfcx_Idst", nullptr},
    {"TmuaA2S1Xhf_Ialp_Isqr", "TmuaA2S1Xhfcx_Isrc_Isqr", nullptr},
};

std::string str(const char* s) { return s != nullptr ? spec(s) : std::string("(none)"); }

}  // namespace

// The committed names are the library's: every sysimage fragment label of pipelines.desc section 1 (the plug-in's
// hm_ca_load reading), when sysimage/ is present.
TEST(ca_label_fixture_matches_the_sysimage_library) {
    std::vector<std::string> lib = libraryLabels();
    CHECK_EQ(lib.size(), (size_t)444);
    std::ifstream f("../sysimage/rootfs/System/Library/Frameworks/QuartzCore.framework/default.pipelinelib/pipelines.desc",
                    std::ios::binary);
    if (!f) {
        std::printf("       (no sysimage/ default.pipelinelib here: the fixture's names were not compared)\n");
        return;
    }
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u32 = [&](size_t o) {
        uint32_t v = 0;
        if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4);
        return v;
    };
    std::set<std::string> fromImage;
    const uint32_t sections = u32(0), off1 = u32(8);
    CHECK(sections > 7);
    for (uint32_t i = 0, n = u32(off1); sections > 7 && i < n && off1 + 8 + 4 * i <= d.size(); i++) {
        const size_t at = (size_t)off1 + u32(off1 + 4 + 4 * i) + 16;
        if (at >= d.size()) continue;
        const std::string label(reinterpret_cast<const char*>(d.data() + at), strnlen(reinterpret_cast<const char*>(d.data() + at), d.size() - at));
        if (label.compare(0, kPrefix.size(), kPrefix) == 0) fromImage.insert(label);
    }
    CHECK(fromImage == std::set<std::string>(lib.begin(), lib.end()));
}

// The seam: HM_CA_LABEL_OLD gives the stand-in each of the 27 labels logged in the 14 and 17 November runs got
// (nearest-old.py reproduced the same 27), "nothing of the same kind" for TvdkA2Xhf_Isrc.
TEST(ca_label_old_rule_reproduces_the_logged_stand_ins) {
    const std::vector<std::string> lib = libraryLabels();
    int n = 0;
    for (const RunCase* set : {kRunCases, kOlderRunCases}) {
        const size_t count = set == kRunCases ? std::size(kRunCases) : std::size(kOlderRunCases);
        for (size_t i = 0; i < count; i++, n++) {
            const RunCase& c = set[i];
            Outcome o = runRule(c.asked, lib, HM_CA_LABEL_OLD);
            const bool ok = c.today == nullptr ? o.given == nullptr && o.why != nullptr && std::strcmp(o.why, HM_CA_LABEL_NOTHING) == 0
                                               : o.given != nullptr && spec(o.given) == c.today && o.why == nullptr;
            if (!ok) std::fprintf(stderr, "  %s: old rule gives %s, logged %s\n", c.asked, str(o.given).c_str(), c.today ? c.today : "(none)");
            CHECK(ok);
        }
    }
    CHECK_EQ(n, 27);
    // labels outside the grammar return NULL without a stand-in, as hm_ca_nearest's nil
    for (const char* bad : {"", "com.apple.coreanimation.vert.VfxXh", "com.apple.coreanimation.frag.", "com.apple.coreanimation.frag.Timg",
                            "com.apple.coreanimation.frag.TimgA2Xhf_", "com.apple.coreanimation.frag.TimgA2Xhf__Isrc"}) {
        std::vector<const char*> names;
        for (const std::string& l : lib) names.push_back(l.c_str());
        const char* why = nullptr;
        CHECK(hm_ca_label_nearest(bad, names.data(), names.size(), nullptr, HM_CA_LABEL_OLD, &why) == nullptr);
        CHECK(why != nullptr && std::strcmp(why, HM_CA_LABEL_UNPARSED) == 0);
    }
}

// Property 1, the concrete cases of the runs: the five effect-changing stand-ins of 17 November, TvdkA2Xhf_Isrc, and
// the rest of the 15 (which keep today's stand-in). The gate's expected result (inspection.md §9) for each.
// **Validates: Requirements 1.2, 2.2**
TEST(ca_label_property_1_run_labels_get_the_asked_effect) {
    const std::vector<std::string> lib = libraryLabels();
    const std::set<std::string> libset(lib.begin(), lib.end());
    int failed = 0;
    for (const RunCase& c : kRunCases) {
        Spec asked;
        CHECK(parseSpec(c.asked, &asked));
        CHECK(libset.count(kPrefix + c.asked) == 0);
        Outcome o = runRule(c.asked, lib, HM_CA_LABEL_NEW);
        std::string bad = violation(asked, o, qualifying(asked, kPrefix + c.asked));
        if (bad.empty() && c.expected != nullptr && (o.given == nullptr || spec(o.given) != c.expected)) {
            bad = "given " + str(o.given) + ", expected " + c.expected;
        }
        if (!bad.empty()) {
            failed++;
            std::fprintf(stderr, "  counterexample: %s -> %s: %s\n", c.asked, str(o.given).c_str(), bad.c_str());
        }
    }
    std::fprintf(stderr, "  property 1 (run labels): %zu asked, %d failed\n", std::size(kRunCases), failed);
    CHECK_EQ(failed, 0);
}

// Property 1 over the library: every one of the 444 labels removed in turn and asked for (leave-one-out), then seeded
// mutants of library labels (one input's kind replaced, its modifiers replaced, or a modifier added) that the library
// does not have. Bug-condition cases are those where today's rule gives a label of another effect or nothing.
// **Validates: Requirements 1.2, 2.2**
TEST(ca_label_property_1_left_out_and_mutated_labels_get_the_asked_effect) {
    const std::vector<std::string> lib = libraryLabels();
    const std::set<std::string> libset(lib.begin(), lib.end());
    int asked = 0, bugCases = 0, failed = 0, printed = 0;
    auto check = [&](const std::string& askedSpec, const std::vector<std::string>& from, const char* how) {
        Spec a;
        if (!parseSpec(askedSpec, &a)) return;
        asked++;
        const std::vector<std::string> q = qualifying(a, kPrefix + askedSpec);
        Outcome old = runRule(askedSpec, from, HM_CA_LABEL_OLD);
        if (!violation(a, old, q).empty()) bugCases++;
        Outcome o = runRule(askedSpec, from, HM_CA_LABEL_NEW);
        const std::string bad = violation(a, o, q);
        if (bad.empty()) return;
        failed++;
        if (printed++ < 12) std::fprintf(stderr, "  counterexample (%s): %s -> %s: %s\n", how, askedSpec.c_str(), str(o.given).c_str(), bad.c_str());
    };
    // leave-one-out, exhaustive
    for (size_t i = 0; i < lib.size(); i++) {
        std::vector<std::string> rest = lib;
        rest.erase(rest.begin() + (long)i);
        check(spec(lib[i]), rest, "left out");
    }
    const int leftOut = asked, leftOutBug = bugCases, leftOutFailed = failed;
    // the kinds and modifier strings the library uses
    std::vector<std::string> kinds, mods;
    {
        std::set<std::string> k, m;
        for (const std::string& l : lib) {
            Spec s;
            if (!parseSpec(spec(l), &s)) continue;
            for (const std::string& t : s.inputs) {
                k.insert(t.substr(0, 4));
                if (t.size() > 4) m.insert(t.substr(4));
            }
        }
        kinds.assign(k.begin(), k.end());
        mods.assign(m.begin(), m.end());
    }
    std::mt19937 rng(0xca1abe15u);
    auto pick = [&](size_t n) { return (size_t)(rng() % n); };
    printed = 0;   // the mutants' counterexamples printed too
    for (int round = 0; round < 3000; round++) {
        Spec s;
        const std::string base = spec(lib[pick(lib.size())]);
        if (!parseSpec(base, &s) || s.inputs.empty()) continue;
        std::string& t = s.inputs[pick(s.inputs.size())];
        const unsigned op = (unsigned)pick(3);
        if (op == 0) {
            t = kinds[pick(kinds.size())] + t.substr(4);
        } else if (op == 1) {
            t = t.substr(0, 4) + (pick(3) == 0 ? std::string() : mods[pick(mods.size())]);
        } else {
            t += mods[pick(mods.size())];
        }
        std::string mutant = s.part[0] + s.part[1] + s.part[2] + s.part[3] + s.part[4] + s.part[5] + "X" + s.flags;
        for (const std::string& in : s.inputs) mutant += "_" + in;
        if (libset.count(kPrefix + mutant) != 0) continue;
        check(mutant, lib, op == 0 ? "kind replaced" : op == 1 ? "modifiers replaced" : "modifier added");
    }
    std::fprintf(stderr, "  property 1 (left out): %d asked, %d bug-condition cases under today's rule, %d failed\n", leftOut,
                 leftOutBug, leftOutFailed);
    std::fprintf(stderr, "  property 1 (mutated): %d asked, %d bug-condition cases under today's rule, %d failed\n",
                 asked - leftOut, bugCases - leftOutBug, failed - leftOutFailed);
    CHECK(leftOutBug > 10);                 // the generators reach the bug condition
    CHECK(bugCases - leftOutBug > 100);
    CHECK_EQ(failed, 0);
}

// ---- Property 2: Preservation ----------------------------------------------------------------------------------------

namespace {

// Today's hm_ca_nearest (HakiosMetalCA.m 1537-1610), restated on the test's own std::regex parse: the same T, B, D, A,
// S, L and number of inputs; 100 plus the length difference per input of another kind, 3 per asked flag the candidate
// lacks, 10 per flag it adds; the lowest cost, ties by name. EXCLUDE (a full label, or "") is left out of the library.
// It pins HM_CA_LABEL_OLD (the rule of metal_m2v=1 and of metal_ca_nearest=old) to today's choice whatever task 5.1
// does to the header.
const char* oldReference(const std::string& askedLabel, const std::string& exclude) {
    if (askedLabel.compare(0, kPrefix.size(), kPrefix) != 0) return nullptr;
    Spec w;
    if (!parseSpec(askedLabel.substr(kPrefix.size()), &w)) return nullptr;
    const char* best = nullptr;
    long bestCost = 0;
    for (const Parsed& p : parsedLibrary()) {
        if (p.label == exclude) continue;
        const Spec& h = p.spec;
        bool same = true;
        for (int i = 0; same && i < 6; i++) same = w.part[i] == h.part[i];
        if (!same || w.inputs.size() != h.inputs.size()) continue;
        long cost = 0;
        for (size_t i = 0; i < w.inputs.size(); i++) {
            if (w.inputs[i] != h.inputs[i]) cost += 100 + std::labs((long)h.inputs[i].size() - (long)w.inputs[i].size());
        }
        for (char c : w.flags) cost += h.flags.find(c) == std::string::npos ? 3 : 0;
        for (char c : h.flags) cost += w.flags.find(c) == std::string::npos ? 10 : 0;
        if (best == nullptr || cost < bestCost || (cost == bestCost && std::strcmp(p.label.c_str(), best) < 0)) {
            best = p.label.c_str();
            bestCost = cost;
        }
    }
    return best;
}

bool sameText(const char* a, const char* b) { return (a == nullptr || b == nullptr) ? a == b : std::strcmp(a, b) == 0; }

// labels the library lacks, as Property 1's generators make them: SEED's mutants of library labels (one input's kind
// replaced, its modifiers replaced, or a modifier added)
std::vector<std::string> mutants(uint32_t seed, int rounds) {
    const std::vector<std::string> lib = libraryLabels();
    const std::set<std::string> libset(lib.begin(), lib.end());
    std::vector<std::string> kinds, mods;
    {
        std::set<std::string> k, m;
        for (const Parsed& p : parsedLibrary()) {
            for (const std::string& t : p.spec.inputs) {
                k.insert(t.substr(0, 4));
                if (t.size() > 4) m.insert(t.substr(4));
            }
        }
        kinds.assign(k.begin(), k.end());
        mods.assign(m.begin(), m.end());
    }
    std::mt19937 rng(seed);
    auto pick = [&](size_t n) { return (size_t)(rng() % n); };
    std::vector<std::string> out;
    for (int round = 0; round < rounds; round++) {
        Spec s;
        if (!parseSpec(spec(lib[pick(lib.size())]), &s) || s.inputs.empty()) continue;
        std::string& t = s.inputs[pick(s.inputs.size())];
        const unsigned op = (unsigned)pick(3);
        if (op == 0) {
            t = kinds[pick(kinds.size())] + t.substr(4);
        } else if (op == 1) {
            t = t.substr(0, 4) + (pick(3) == 0 ? std::string() : mods[pick(mods.size())]);
        } else {
            t += mods[pick(mods.size())];
        }
        std::string m = s.part[0] + s.part[1] + s.part[2] + s.part[3] + s.part[4] + s.part[5] + "X" + s.flags;
        for (const std::string& in : s.inputs) m += "_" + in;
        if (libset.count(kPrefix + m) == 0) out.push_back(m);
    }
    return out;
}

// a library of every label but EXCLUDE
std::vector<std::string> without(const std::vector<std::string>& lib, const std::string& exclude) {
    std::vector<std::string> rest;
    for (const std::string& l : lib) {
        if (l != exclude) rest.push_back(l);
    }
    return rest;
}

}  // namespace

// The plug-in compiles a library label from its own variant without asking the rule (observed on the unfixed
// HakiosMetalCA.m: hm_bitcode_for and hm_m2v_variant take hm_ca_variants[label] first, and call hm_ca_nearest only
// under hm_ca_variants[frag] == nil). Both rules, asked for a library label, return it. And the plug-in's call sites
// keep that guard (checked in the source when it is present).
// **Validates: Requirements 3.2**
TEST(ca_label_property_2_library_labels_are_their_own_variant) {
    const std::vector<std::string> lib = libraryLabels();
    int mismatched = 0;
    for (const std::string& l : lib) {
        const std::string s = spec(l);
        Outcome o = runRule(s, lib, HM_CA_LABEL_OLD), n = runRule(s, lib, HM_CA_LABEL_NEW);
        if (!sameText(o.given, l.c_str()) || !sameText(n.given, l.c_str()) || o.why != nullptr || n.why != nullptr) {
            if (mismatched++ < 8) std::fprintf(stderr, "  %s: old %s, new %s\n", s.c_str(), str(o.given).c_str(), str(n.given).c_str());
        }
    }
    std::fprintf(stderr, "  property 2 (library labels): %zu asked, %d not given themselves\n", lib.size(), mismatched);
    CHECK_EQ(mismatched, 0);

    std::ifstream f("../userland/src/HakiosMetalCA.m");
    if (!f) {
        std::printf("       (no ../userland/src/HakiosMetalCA.m here: the plug-in's call sites were not checked)\n");
        return;
    }
    std::vector<std::string> lines;
    for (std::string line; std::getline(f, line);) lines.push_back(line);
    int calls = 0, guarded = 0, labelFirst = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        // the library label path, before any stand-in: "if (label != nil && (variant = hm_ca_variants[label]) != nil)"
        static const std::regex own("variant = hm_ca_variants\\[f?label\\]\\) != nil");
        if (std::regex_search(lines[i], own)) labelFirst++;
        // calls only: hm_ca_nearest (metal_m2v=1's) and, from task 5.3, hm_ca_nearest_by (the draws', with the mode)
        if (lines[i].find("= hm_ca_nearest(") == std::string::npos && lines[i].find("= hm_ca_nearest_by(") == std::string::npos) continue;
        calls++;
        size_t j = i;
        while (j > 0 && lines[j - 1].find_first_not_of(" \t") == std::string::npos) j--;
        if (j > 0 && lines[j - 1].find("hm_ca_variants[") != std::string::npos && lines[j - 1].find("== nil") != std::string::npos) {
            guarded++;
        } else {
            std::fprintf(stderr, "  HakiosMetalCA.m:%zu calls hm_ca_nearest without the hm_ca_variants[...] == nil guard\n", i + 1);
        }
    }
    std::fprintf(stderr, "  plug-in: %d calls of hm_ca_nearest, %d guarded; %d library-label-first lookups\n", calls, guarded, labelFirst);
    CHECK(calls >= 2);
    CHECK_EQ(guarded, calls);
    CHECK(labelFirst >= 2);
}

// Missing labels outside the bug condition: today's stand-in already has the asked effect key and a flag difference
// in H. The new rule gives the same label. Concrete: the 17 November run's #1-4, 6-9, 11, 12 (the screens that look
// right: Home, Settings, App Library, lock screen, the status bar's TimgBdsoA2Xhfn_Isrc). Generated: leave-one-out
// over the 444 and seeded mutants.
// **Validates: Requirements 3.2, 3.3**
TEST(ca_label_property_2_harmless_stand_ins_are_kept) {
    const std::vector<std::string> lib = libraryLabels();
    int kept = 0, failed = 0, printed = 0;
    auto check = [&](const std::string& askedSpec, const std::vector<std::string>& from, const std::string& exclude, const char* how) {
        Spec a;
        if (!parseSpec(askedSpec, &a)) return false;
        Outcome old = runRule(askedSpec, from, HM_CA_LABEL_OLD);
        if (old.given == nullptr || !violation(a, old, qualifying(a, exclude)).empty()) return false;   // a bug-condition case
        kept++;
        Outcome n = runRule(askedSpec, from, HM_CA_LABEL_NEW);
        if (sameText(n.given, old.given) && n.why == nullptr) return true;
        failed++;
        if (printed++ < 12) std::fprintf(stderr, "  counterexample (%s): %s -> old %s, new %s (%s)\n", how, askedSpec.c_str(),
                                         str(old.given).c_str(), str(n.given).c_str(), n.why ? n.why : "no reason");
        return true;
    };
    int runKept = 0;
    for (const RunCase& c : kRunCases) {
        const bool harmless = c.today != nullptr && c.expected != nullptr && std::strcmp(c.today, c.expected) == 0;
        if (check(c.asked, lib, kPrefix + c.asked, "run") != harmless) {
            std::fprintf(stderr, "  %s: expected %s the non-bug set\n", c.asked, harmless ? "in" : "outside");
            failed++;
        }
        runKept += harmless;
    }
    CHECK_EQ(runKept, 10);
    const int afterRun = kept;
    for (const std::string& l : lib) check(spec(l), without(lib, l), l, "left out");
    const int afterLeftOut = kept;
    for (const std::string& m : mutants(0x9e3a11edu, 3000)) check(m, lib, kPrefix + m, "mutated");
    std::fprintf(stderr, "  property 2 (harmless stand-ins): %d run, %d left out, %d mutated outside the bug condition, %d changed\n",
                 afterRun, afterLeftOut - afterRun, kept - afterLeftOut, failed);
    CHECK(afterLeftOut - afterRun > 100);   // the generators reach the non-bug set
    CHECK(kept - afterLeftOut > 20);
    CHECK_EQ(failed, 0);
}

// metal_m2v=1 and metal_ca_nearest=old take HM_CA_LABEL_OLD: for every label (the library's, left out, mutated, the
// runs'), it gives today's hm_ca_nearest choice and reason, whatever HARMLESS says.
// **Validates: Requirements 3.6**
TEST(ca_label_property_2_old_rule_is_todays_choice_for_every_label) {
    const std::vector<std::string> lib = libraryLabels();
    int asked = 0, failed = 0;
    auto check = [&](const std::string& askedSpec, const std::string& exclude) {
        const std::vector<std::string> from = exclude.empty() ? lib : without(lib, exclude);
        std::vector<const char*> names;
        for (const std::string& l : from) names.push_back(l.c_str());
        const std::string label = kPrefix + askedSpec;
        const char* ref = oldReference(label, exclude);
        Spec a;
        const char* refWhy = !parseSpec(askedSpec, &a) ? HM_CA_LABEL_UNPARSED : ref == nullptr ? HM_CA_LABEL_NOTHING : nullptr;
        for (const char* harmless : {(const char*)nullptr, "", "+c -c +x -x +u -u -n +S1 -S1"}) {
            const char* why = "unset";
            const char* got = hm_ca_label_nearest(label.c_str(), names.data(), names.size(), harmless, HM_CA_LABEL_OLD, &why);
            asked++;
            if (sameText(got, ref) && sameText(why, refWhy)) continue;
            if (failed++ < 12) std::fprintf(stderr, "  %s: old rule %s (%s), today %s (%s)\n", askedSpec.c_str(), str(got).c_str(),
                                            why ? why : "-", str(ref).c_str(), refWhy ? refWhy : "-");
        }
    };
    for (const std::string& l : lib) check(spec(l), "");
    for (const std::string& l : lib) check(spec(l), l);
    for (const RunCase* set : {kRunCases, kOlderRunCases}) {
        const size_t count = set == kRunCases ? std::size(kRunCases) : std::size(kOlderRunCases);
        for (size_t i = 0; i < count; i++) check(set[i].asked, "");
    }
    for (const std::string& m : mutants(0x01d0c0deu, 2000)) check(m, "");
    std::fprintf(stderr, "  property 2 (old rule): %d asked, %d differ from today's hm_ca_nearest\n", asked, failed);
    CHECK_EQ(failed, 0);
}

// Random strings, truncated and edited labels: neither rule crashes; the header's parse agrees with the plug-in's
// regular expression; a label outside the grammar (or without the prefix) gives NULL, "does not parse", under both
// rules; no library (NULL, empty, NULL entries) gives NULL with a reason.
// **Validates: Requirements 3.2**
TEST(ca_label_property_2_random_strings_never_crash_either_rule) {
    const std::vector<std::string> lib = libraryLabels();
    std::vector<const char*> names;
    for (const std::string& l : lib) names.push_back(l.c_str());
    std::vector<std::string> inputs;
    std::mt19937 rng(0x5eed2b0bu);
    auto pick = [&](size_t n) { return (size_t)(rng() % n); };
    static const char alphabet[] = "TBDASLXI_abcdefghijklmnopqrstuvwxyzACDGMNOSXZ0123456789.-";
    for (int i = 0; i < 1500; i++) {
        std::string s;
        for (size_t n = pick(40); n > 0; n--) s += alphabet[pick(sizeof(alphabet) - 1)];
        inputs.push_back(s);
    }
    for (int i = 0; i < 1500; i++) {
        std::string s = spec(lib[pick(lib.size())]);
        switch (pick(4)) {
        case 0: s = s.substr(0, pick(s.size() + 1)); break;                                    // truncated
        case 1: s[pick(s.size())] = alphabet[pick(sizeof(alphabet) - 1)]; break;               // one character replaced
        case 2: s.insert(pick(s.size() + 1), 1, alphabet[pick(sizeof(alphabet) - 1)]); break;  // one inserted
        default: s.erase(pick(s.size()), 1); break;                                            // one removed
        }
        inputs.push_back(s);
    }
    int parsed = 0, unparsed = 0, failed = 0;
    for (const std::string& s : inputs) {
        Spec a;
        struct hm_ca_label h;
        const bool ours = parseSpec(s, &a), theirs = hm_ca_label_parse(s.c_str(), &h) != 0;
        if (ours != theirs) {
            if (failed++ < 12) std::fprintf(stderr, "  \"%s\": regular expression %d, header parse %d\n", s.c_str(), ours, theirs);
            continue;
        }
        for (const std::string& asked : {kPrefix + s, s, "com.apple.coreanimation.vert." + s}) {
            const bool fragment = asked.compare(0, kPrefix.size(), kPrefix) == 0;
            for (hm_ca_label_mode mode : {HM_CA_LABEL_OLD, HM_CA_LABEL_NEW}) {
                const char* why = "unset";
                const char* got = hm_ca_label_nearest(asked.c_str(), names.data(), names.size(), nullptr, mode, &why);
                bool ok;
                if (!fragment || !ours) {
                    ok = got == nullptr && sameText(why, HM_CA_LABEL_UNPARSED);
                } else if (mode == HM_CA_LABEL_OLD) {
                    ok = sameText(got, oldReference(asked, "")) && (got != nullptr ? why == nullptr : why != nullptr);
                } else {
                    ok = got != nullptr ? why == nullptr : why != nullptr && why[0] != '\0';
                }
                if (!ok && failed++ < 12) std::fprintf(stderr, "  \"%s\" (%s rule): %s (%s)\n", asked.c_str(),
                                                       mode == HM_CA_LABEL_OLD ? "old" : "new", str(got).c_str(), why ? why : "-");
            }
        }
        (ours ? parsed : unparsed)++;
    }
    // no library at all, an empty one, one of NULL entries and labels of another kind
    const char* odd[] = {nullptr, "com.apple.coreanimation.vert.VfxXh", "", nullptr};
    for (hm_ca_label_mode mode : {HM_CA_LABEL_OLD, HM_CA_LABEL_NEW}) {
        const char* why = nullptr;
        CHECK(hm_ca_label_nearest("com.apple.coreanimation.frag.TimgA2Xhf_Isrc", nullptr, 5, nullptr, mode, &why) == nullptr);
        CHECK(why != nullptr && std::strcmp(why, HM_CA_LABEL_UNPARSED) != 0);
        why = nullptr;
        CHECK(hm_ca_label_nearest("com.apple.coreanimation.frag.TimgA2Xhf_Isrc", odd, 0, nullptr, mode, &why) == nullptr);
        CHECK(why != nullptr && std::strcmp(why, HM_CA_LABEL_UNPARSED) != 0);
        why = nullptr;
        CHECK(hm_ca_label_nearest("com.apple.coreanimation.frag.TimgA2Xhf_Isrc", odd, std::size(odd), nullptr, mode, &why) == nullptr);
        CHECK(why != nullptr && std::strcmp(why, HM_CA_LABEL_UNPARSED) != 0);
        why = nullptr;
        CHECK(hm_ca_label_nearest(nullptr, names.data(), names.size(), nullptr, mode, &why) == nullptr);
        CHECK(why != nullptr && std::strcmp(why, HM_CA_LABEL_UNPARSED) == 0);
        CHECK(hm_ca_label_nearest("com.apple.coreanimation.frag.Timg", names.data(), names.size(), nullptr, mode, nullptr) == nullptr);
    }
    std::fprintf(stderr, "  property 2 (random strings): %zu strings, %d parse, %d do not, %d failed\n", inputs.size(), parsed, unparsed, failed);
    CHECK(parsed > 100 && unparsed > 1000);
    CHECK_EQ(failed, 0);
}

// D2 (task 5.2): what the plug-in does with each result of the rule. A stand-in is compiled; under the new rule a parsed
// label with no same-effect variant is left out (#10, #13, #15 of 17 November and TvdkA2Xhf_Isrc), with its once-per-label
// line; the old rule (metal_m2v=1, the opt-out) and labels outside the grammar keep the function's own module.
TEST(ca_label_d2_outcome_and_its_line) {
    const std::vector<std::string> lib = libraryLabels();
    CHECK(hm_ca_label_outcome_of("com.apple.coreanimation.frag.TimgA2Xhf_Isrc", nullptr, HM_CA_LABEL_NEW) == HM_CA_LABEL_STAND_IN);
    CHECK(hm_ca_label_outcome_of("com.apple.coreanimation.frag.TimgA2Xhf_Isrc", nullptr, HM_CA_LABEL_OLD) == HM_CA_LABEL_STAND_IN);
    CHECK(hm_ca_label_outcome_of(nullptr, HM_CA_LABEL_NO_EFFECT, HM_CA_LABEL_NEW) == HM_CA_LABEL_LEAVE_OUT);
    CHECK(hm_ca_label_outcome_of(nullptr, HM_CA_LABEL_FLAGS_OUTSIDE, HM_CA_LABEL_NEW) == HM_CA_LABEL_LEAVE_OUT);
    CHECK(hm_ca_label_outcome_of(nullptr, HM_CA_LABEL_NOTHING, HM_CA_LABEL_OLD) == HM_CA_LABEL_OWN_MODULE);
    CHECK(hm_ca_label_outcome_of(nullptr, HM_CA_LABEL_UNPARSED, HM_CA_LABEL_NEW) == HM_CA_LABEL_OWN_MODULE);
    CHECK(hm_ca_label_outcome_of(nullptr, nullptr, HM_CA_LABEL_NEW) == HM_CA_LABEL_OWN_MODULE);
    int leftOut = 0;
    for (const RunCase& c : kRunCases) {
        Outcome n = runRule(c.asked, lib, HM_CA_LABEL_NEW), o = runRule(c.asked, lib, HM_CA_LABEL_OLD);
        const hm_ca_label_outcome got = hm_ca_label_outcome_of(n.given, n.why, HM_CA_LABEL_NEW);
        CHECK(got == (c.expected == nullptr ? HM_CA_LABEL_LEAVE_OUT : HM_CA_LABEL_STAND_IN));
        CHECK(hm_ca_label_outcome_of(o.given, o.why, HM_CA_LABEL_OLD) == (c.today == nullptr ? HM_CA_LABEL_OWN_MODULE : HM_CA_LABEL_STAND_IN));
        if (got == HM_CA_LABEL_LEAVE_OUT) leftOut++;
    }
    CHECK_EQ(leftOut, 4);
    char line[768];
    hm_ca_label_leave_out_line(line, sizeof(line), "com.apple.coreanimation.frag.TvdkA2Xhf_Isrc", HM_CA_LABEL_NO_EFFECT, "10:09:03");
    CHECK(std::strstr(line, "left out: CoreAnimation's TvdkA2Xhf_Isrc has no stand-in of the same effect (" HM_CA_LABEL_NO_EFFECT ")") == line);
    CHECK(std::strstr(line, "draws are left out") != nullptr);
    CHECK(std::strstr(line, "; device time 10:09:03\n") != nullptr && line[std::strlen(line) - 1] == '\n');
    hm_ca_label_leave_out_line(line, sizeof(line), "TcimBdsoA2Xhf_Icir", nullptr, nullptr);
    CHECK(std::strstr(line, "CoreAnimation's TcimBdsoA2Xhf_Icir has no stand-in of the same effect (no reason)") != nullptr);
    CHECK(std::strstr(line, "device time") == nullptr);
}

// Task 5.3: the opt-out metal_ca_nearest=old (a whole boot argument, nothing else) and the "differs in" text of the
// stand-in lines, for the flag-only and the kind-changing stand-ins of 17 November.
TEST(ca_label_opt_out_and_difference_text) {
    CHECK(hm_ca_label_mode_from_bootargs(nullptr) == HM_CA_LABEL_NEW);
    CHECK(hm_ca_label_mode_from_bootargs("") == HM_CA_LABEL_NEW);
    CHECK(hm_ca_label_mode_from_bootargs("gpu=1 metal=1 metal_fps=1") == HM_CA_LABEL_NEW);
    CHECK(hm_ca_label_mode_from_bootargs("metal=1 metal_ca_nearest=old metal_fps=1") == HM_CA_LABEL_OLD);
    CHECK(hm_ca_label_mode_from_bootargs("metal_ca_nearest=old") == HM_CA_LABEL_OLD);
    CHECK(hm_ca_label_mode_from_bootargs("metal_ca_nearest=older") == HM_CA_LABEL_NEW);
    CHECK(hm_ca_label_mode_from_bootargs("xmetal_ca_nearest=old") == HM_CA_LABEL_NEW);
    CHECK(hm_ca_label_mode_from_bootargs("metal_ca_nearest=new") == HM_CA_LABEL_NEW);
    char d[256];
    hm_ca_label_difference("com.apple.coreanimation.frag.TimgBdsoA2Xhfn_Isrc", "com.apple.coreanimation.frag.TimgBdsoA2Xhfcn_Isrc", d, sizeof(d));
    CHECK(std::string(d) == "flags +c");
    hm_ca_label_difference("TcimA2S1Xhfn_IsrcN3Oc3mtc4", "TcimA2S1Xhf_IalpCcl", d, sizeof(d));
    CHECK(std::string(d) == "flags -n; input 1 IsrcN3Oc3mtc4 -> IalpCcl");
    hm_ca_label_difference("TmuaA2S1Xhf_Ixrg_Isqr", "TmuaA2S1Xhfcx_Isrc_Isqr", d, sizeof(d));
    CHECK(std::string(d) == "flags +c +x; input 1 Ixrg -> Isrc");
    hm_ca_label_difference("TmuaA2Xhf_Isrc_Isqr", "TmuaA2S1Xhfcx_Isrc_Isqr", d, sizeof(d));
    CHECK(std::string(d) == "S none -> S1; flags +c +x");
    hm_ca_label_difference("TimgA2Xhf_Isrc", "TimgA2Xhf_Isrc", d, sizeof(d));
    CHECK(std::string(d) == "same");
    hm_ca_label_difference("TimgA2Xhf_Isrc", "TimgA2Xhf", d, sizeof(d));
    CHECK(std::string(d) == "inputs 1 -> 0");
    hm_ca_label_difference("not a label", "TimgA2Xhf_Isrc", d, sizeof(d));
    CHECK(std::string(d) == "unparsed");
    char small[6];
    CHECK(hm_ca_label_difference("TcimA2S1Xhfn_IsrcN3Oc3mtc4", "TcimA2S1Xhf_IalpCcl", small, sizeof(small)) < sizeof(small));
    CHECK(std::strlen(small) < sizeof(small));
}
