// metal-ca-stale-frame: CoreAnimation's IOSurfaces on the A650, drawn through a surface target of the driver's.
//
// Property 1 (Bug Condition): on chip 0x06050002 with metal_blit on, for a qualifying surface (BGRA8 format 80, one
// plane, rotation 0, rows a multiple of 64 bytes, the descriptor the surface's size), after each command buffer that
// draws into it completes, the surface's bytes are the model frame -- this command buffer's drawing over
// CoreAnimation's own previous drawing, whatever was written into the surface's memory from outside in between --
// no clear or copy of the command buffer has the surface's memory as its destination, the only write to it is one
// whole-surface copy after every pass, and the submission names the surface for writing (so the display's wait on
// the surface's fence covers that copy).
//
// The frames are modelled with what the fake GPU carries out: a Clear (the 2D engine's solid fill) and 2D copies of
// damage rectangles from a small texture (it rasterizes no draw). The writes from outside stand for anything that
// leaves the surface's pages different from CoreAnimation's last drawing.
//
// On the code before the fix (the seam: hakios_metal_texture_new_iosurface_target gives the surface texture,
// hakios_metal_cb_resolve_surface_targets records nothing) this property FAILS: that is the bug.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "hakios-metal/hakios_metal.h"
#include "test.hpp"

namespace {
constexpr uint64_t kA650 = 0x06050002;
constexpr uint32_t kFourccBGRA = 0x42475241u;   // 'BGRA'
constexpr unsigned kBGRA8Unorm = 80;
constexpr unsigned kSurface = 1;
constexpr unsigned kSourceSide = 256;            // the small texture the damage is copied from

struct Rect {
    unsigned x = 0, y = 0, w = 0, h = 0;
};
// one piece of work of a command buffer, into texture TEX over the surface: a Clear of the whole texture to PX (its
// bytes in memory, B G R A), or a copy of DST.w x DST.h from (SX, SY) of the small texture into DST
struct Op {
    bool clear = false;
    unsigned tex = 0;
    uint8_t px[4] = {0, 0, 0, 0};
    Rect dst;
    unsigned sx = 0, sy = 0;
};
struct Frame {
    std::vector<std::vector<Op>> cbs;     // 1-2 command buffers, each 0 or more ops
    std::vector<unsigned> outsideRows;    // rows written from outside after this frame
};
struct Scenario {
    unsigned width = 0, height = 0, rowBytes = 0, textures = 1;
    uint32_t seed = 0;                    // the surface's first contents, the small texture's, the outside bytes
    std::vector<Frame> frames;
};

// what one command buffer did, against expectedBehavior
struct CbVerdict {
    bool checked = false;           // it wrote the surface (expectedBehavior is over the written surfaces)
    bool surfaceNamed = false;      // the submission names the surface's buffer...
    bool namedForWrite = false;     // ...with the write flag
    unsigned passWritesToSurface = 0;   // clears and damage copies whose destination is the surface's memory
    uint64_t firstPassWriteBase = 0;
    bool firstPassWriteFill = false;
    unsigned wholeCopies = 0;       // whole-surface copies into the surface's memory
    bool wholeCopyLast = false;     // ...and the last write of the submission
    unsigned rowsWrong = 0;         // rows of the surface that are not the model frame's
    unsigned rowsWrongOutside = 0;  // ...of which every wrong byte is the last outside write's
    unsigned firstWrongRow = 0;
    unsigned fillsIntoSurface = 0;  // (metal-ca-perf) clears whose destination is the surface's memory
    bool copyLast = false;          // ...and a copy (whole or the damage) into it the last write of the submission
    bool ok() const {
        return !checked || (surfaceNamed && namedForWrite && passWritesToSurface == 0 && wholeCopies == 1 && wholeCopyLast && rowsWrong == 0);
    }
    // with the damage copy on (metal-ca-perf task 5.3): the copy may be the damage alone, the surface still the frame
    bool okDamage() const { return !checked || (surfaceNamed && namedForWrite && fillsIntoSurface == 0 && copyLast && rowsWrong == 0); }
};

uint8_t outsideByte(uint32_t seed, unsigned row, unsigned i) { return uint8_t(0x5a ^ (seed * 31 + row * 7 + i * 13)); }

std::string describe(const Scenario& s) {
    std::string r = std::to_string(s.width) + "x" + std::to_string(s.height) + " rows " + std::to_string(s.rowBytes) + " bytes, " +
                    std::to_string(s.textures) + " texture(s), " + std::to_string(s.frames.size()) + " frame(s):";
    for (size_t f = 0; f < s.frames.size(); f++) {
        r += " [";
        for (size_t c = 0; c < s.frames[f].cbs.size(); c++) {
            r += c ? " | cb:" : "cb:";
            for (const Op& o : s.frames[f].cbs[c]) {
                char t[96];
                if (o.clear) {
                    snprintf(t, sizeof(t), " Clear(t%u)", o.tex);
                } else {
                    snprintf(t, sizeof(t), " copy(t%u %ux%u@%u,%u)", o.tex, o.dst.w, o.dst.h, o.dst.x, o.dst.y);
                }
                r += t;
            }
            if (s.frames[f].cbs[c].empty()) r += " Load only";
        }
        r += "] outside " + std::to_string(s.frames[f].outsideRows.size()) + " rows";
    }
    return r;
}

// Runs S on a fake A650 through the plug-in's calls; one verdict per command buffer, in order. *SETUP_OK false when
// the run could not be set up at all (a refusal by the C API). DAMAGE: metal-ca-perf's damage copy on, and the target
// marked whole after each write from outside (what the plug-in does when the surface's seed moved); off by default --
// metal_damage=0, the whole copy these properties were written for.
std::vector<CbVerdict> runFrames(const Scenario& s, bool* setupOk, std::string* why, bool damage = false) {
    std::vector<CbVerdict> verdicts;
    *setupOk = false;
    hakios_metal_device* d = hakios_metal_open_fake_chip(kA650);
    if (d == nullptr || hakios_metal_gpu_id(d) != kA650) {
        *why = "no fake A650";
        if (d != nullptr) hakios_metal_close(d);
        return verdicts;
    }
    hakios_metal_surface_target_set_damage(d, damage ? 1 : 0);
    const unsigned W = s.width, H = s.height, RB = s.rowBytes, rowLen = W * 4;
    hakios_metal_fake_add_surface(d, kSurface, W, H, RB, kFourccBGRA);
    // the surface's first contents (what was in it before CoreAnimation took it), and the model frame from them
    std::mt19937 rng(s.seed);
    std::vector<uint8_t> model(size_t(rowLen) * H);
    {
        std::vector<uint8_t> row(rowLen);
        for (unsigned y = 0; y < H; y++) {
            for (unsigned i = 0; i < rowLen; i++) row[i] = uint8_t(rng());
            hakios_metal_fake_write_surface(d, kSurface, uint64_t(y) * RB, row.data(), rowLen);
            memcpy(model.data() + size_t(y) * rowLen, row.data(), rowLen);
        }
    }
    std::vector<hakios_metal_texture*> tex;
    for (unsigned i = 0; i < s.textures; i++) {
        int targeted = -1;
        hakios_metal_texture* t = hakios_metal_texture_new_iosurface_target(d, kSurface, kBGRA8Unorm, W, H,
                                                                            HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET | 1, 1, &targeted);
        if (t == nullptr) {
            *why = std::string("texture over the surface refused: ") + hakios_metal_error();
            for (hakios_metal_texture* u : tex) hakios_metal_texture_free(u);
            hakios_metal_close(d);
            return verdicts;
        }
        tex.push_back(t);
    }
    hakios_metal_texture* src = hakios_metal_texture_new(d, 2, kBGRA8Unorm, kSourceSide, kSourceSide, 1);
    std::vector<uint8_t> srcPx(size_t(kSourceSide) * kSourceSide * 4);
    for (uint8_t& b : srcPx) b = uint8_t(rng());
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    if (src == nullptr || q == nullptr || hakios_metal_texture_replace(src, 0, 0, kSourceSide, kSourceSide, srcPx.data(), kSourceSide * 4) != 0) {
        *why = std::string("source texture or queue refused: ") + hakios_metal_error();
        if (q != nullptr) hakios_metal_queue_free(q);
        if (src != nullptr) hakios_metal_texture_free(src);
        for (hakios_metal_texture* u : tex) hakios_metal_texture_free(u);
        hakios_metal_close(d);
        return verdicts;
    }
    *setupOk = true;
    std::vector<int64_t> outsideAt(H, -1);   // the seed of the last outside write a row holds (-1: none)
    std::vector<uint8_t> now;
    std::vector<hakios_metal_fake_bo> bos(256);
    std::vector<hakios_metal_fake_write> writes(4096);
    for (size_t f = 0; f < s.frames.size(); f++) {
        const Frame& fr = s.frames[f];
        for (const std::vector<Op>& ops : fr.cbs) {
            hakios_metal_fake_writes(d, nullptr, 0, 1);   // the record from here on is this command buffer's
            hakios_metal_cb* cb = hakios_metal_cb_new(q);
            bool recorded = true;
            for (const Op& o : ops) {
                if (o.clear) {
                    hakios_metal_render_pass pass{};
                    pass.color0 = tex[o.tex];
                    pass.load_action = 2;
                    pass.store_action = 1;
                    pass.clear_color[0] = o.px[2] / 255.0;   // R
                    pass.clear_color[1] = o.px[1] / 255.0;   // G
                    pass.clear_color[2] = o.px[0] / 255.0;   // B
                    pass.clear_color[3] = o.px[3] / 255.0;   // A
                    recorded = recorded && hakios_metal_cb_clear(cb, &pass) == 0;
                    for (unsigned y = 0; y < H; y++)
                        for (unsigned x = 0; x < W; x++) memcpy(model.data() + size_t(y) * rowLen + x * 4, o.px, 4);
                } else {
                    recorded = recorded && hakios_metal_cb_blit(cb, src, 0, o.sx, o.sy, o.dst.w, o.dst.h, tex[o.tex], 0, o.dst.x, o.dst.y, o.dst.w,
                                                                o.dst.h, 0) == 0;
                    for (unsigned y = 0; y < o.dst.h; y++)
                        memcpy(model.data() + size_t(o.dst.y + y) * rowLen + o.dst.x * 4, srcPx.data() + (size_t(o.sy + y) * kSourceSide + o.sx) * 4,
                               size_t(o.dst.w) * 4);
                }
            }
            if (!recorded && why->empty()) *why = std::string("work refused: ") + hakios_metal_error();
            // what -commit does after the turned loop: the targets this command buffer wrote copied into their surfaces
            if (hakios_metal_cb_resolve_surface_targets(cb) < 0 && why->empty()) *why = std::string("resolve: ") + hakios_metal_error();
            hakios_metal_cb_commit(cb, nullptr, nullptr);
            hakios_metal_cb_wait(cb);
            CbVerdict v;
            if (!ops.empty()) {
                v.checked = true;
                if (hakios_metal_cb_status(cb) != HAKIOS_METAL_COMPLETED && why->empty()) *why = std::string("cb: ") + hakios_metal_cb_error(cb);
                unsigned fence = 0;
                const int n = hakios_metal_fake_last_submission(d, &fence, bos.data(), unsigned(bos.size()));
                uint64_t sbase = 0, ssize = 0;
                for (int i = 0; i < n && i < int(bos.size()); i++) {
                    if (bos[i].surface == kSurface) {
                        v.surfaceNamed = true;
                        v.namedForWrite = (bos[i].flags & 2) != 0;
                        sbase = bos[i].gpu_address;
                        ssize = bos[i].size;
                    }
                }
                const int nw = hakios_metal_fake_writes(d, writes.data(), unsigned(writes.size()), 1);
                int lastOfSubmission = -1, wholeAt = -1, copyAt = -1;
                for (int i = 0; i < nw && i < int(writes.size()); i++) {
                    const hakios_metal_fake_write& w = writes[i];
                    if (w.submission != fence) continue;
                    lastOfSubmission = i;
                    const bool intoSurface = v.surfaceNamed && w.base >= sbase && w.base < sbase + ssize;
                    if (!intoSurface) continue;
                    if (w.fill) {
                        v.fillsIntoSurface++;
                    } else {
                        copyAt = i;
                    }
                    const bool whole = !w.fill && w.x0 == 0 && w.y0 == 0 && w.x1 == W - 1 && w.y1 == H - 1;
                    if (whole) {
                        v.wholeCopies++;
                        wholeAt = i;
                    } else if (v.passWritesToSurface++ == 0) {
                        v.firstPassWriteBase = w.base;
                        v.firstPassWriteFill = w.fill != 0;
                    }
                }
                v.wholeCopyLast = wholeAt >= 0 && wholeAt == lastOfSubmission;
                v.copyLast = copyAt >= 0 && copyAt == lastOfSubmission;
                const int64_t size = hakios_metal_fake_surface_bytes(d, kSurface, nullptr, 0);
                now.resize(size_t(size > 0 ? size : 0));
                hakios_metal_fake_surface_bytes(d, kSurface, now.data(), now.size());
                for (unsigned y = 0; y < H; y++) {
                    if (size_t(y) * RB + rowLen > now.size()) break;
                    if (memcmp(now.data() + size_t(y) * RB, model.data() + size_t(y) * rowLen, rowLen) == 0) continue;
                    if (v.rowsWrong++ == 0) v.firstWrongRow = y;
                    // a wrong row whose every wrong byte is the last outside write's: what was written into the
                    // surface's memory from outside stayed, where CoreAnimation's own drawing should be
                    if (outsideAt[y] >= 0) {
                        bool all = true;
                        for (unsigned i = 0; i < rowLen && all; i++) {
                            const uint8_t got = now[size_t(y) * RB + i];
                            all = got == model[size_t(y) * rowLen + i] || got == outsideByte(uint32_t(outsideAt[y]), y, i);
                        }
                        if (all) v.rowsWrongOutside++;
                    }
                }
            }
            verdicts.push_back(v);
            hakios_metal_cb_free(cb);
        }
        // between frames: rows of the surface's memory changed from outside
        std::vector<uint8_t> row(rowLen);
        const uint32_t seed = s.seed + uint32_t(f) * 977u;
        for (unsigned y : fr.outsideRows) {
            for (unsigned i = 0; i < rowLen; i++) row[i] = outsideByte(seed, y, i);
            hakios_metal_fake_write_surface(d, kSurface, uint64_t(y) * RB, row.data(), rowLen);
            outsideAt[y] = int64_t(seed);
        }
        if (damage && !fr.outsideRows.empty()) hakios_metal_surface_target_mark_whole(tex[0]);   // (the seed moved)
    }
    hakios_metal_queue_free(q);
    hakios_metal_texture_free(src);
    for (hakios_metal_texture* t : tex) hakios_metal_texture_free(t);
    hakios_metal_close(d);
    return verdicts;
}

std::string verdictText(const CbVerdict& v, unsigned W, unsigned H) {
    char first[96] = "";
    if (v.passWritesToSurface != 0) {
        snprintf(first, sizeof(first), ", the first a %s at the surface's GPU address 0x%llx", v.firstPassWriteFill ? "Clear (fill)" : "damage copy",
                 (unsigned long long)v.firstPassWriteBase);
    }
    char t[512];
    snprintf(t, sizeof(t),
             "surface named %s (write flag %s); %u clear/copy write(s) into the surface's memory%s; %u whole %ux%u copy(ies) into it%s; "
             "%u row(s) not the model frame's (first row %u), %u of them holding the outside bytes",
             v.surfaceNamed ? "yes" : "no", v.namedForWrite ? "yes" : "no", v.passWritesToSurface, first, v.wholeCopies, W, H,
             v.wholeCopies ? (v.wholeCopyLast ? ", last in the submission" : ", NOT last") : "", v.rowsWrong, v.firstWrongRow, v.rowsWrongOutside);
    return t;
}

Op clearOp(unsigned tex, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    Op o;
    o.clear = true;
    o.tex = tex;
    o.px[0] = b;
    o.px[1] = g;
    o.px[2] = r;
    o.px[3] = a;
    return o;
}
Op copyOp(unsigned tex, unsigned sx, unsigned sy, unsigned x, unsigned y, unsigned w, unsigned h) {
    Op o;
    o.tex = tex;
    o.sx = sx;
    o.sy = sy;
    o.dst = Rect{x, y, w, h};
    return o;
}
}  // namespace

// The scoped case: the phone's display surface (1125x2436 BGRA, 4544-byte rows), frame 1 a Clear of the whole texture,
// 40 rows written from outside, frame 2 a Load with a 200x200 damage copy from a small texture.
// **Validates: Requirements 1.1, 1.2, 1.3, 1.4**
TEST(surface_target_property_1_scoped_display_surface_load_keeps_ca_drawing) {
    Scenario s;
    s.width = 1125;
    s.height = 2436;
    s.rowBytes = 4544;
    s.seed = 0x5f0a650u;
    Frame f1, f2;
    f1.cbs.push_back({clearOp(0, 0x20, 0x40, 0x60, 0xff)});
    for (unsigned i = 0; i < 40; i++) f1.outsideRows.push_back(100 + 50 * i);   // rows 100 .. 2050
    f2.cbs.push_back({copyOp(0, 0, 0, 400, 1000, 200, 200)});
    s.frames = {f1, f2};
    bool setupOk = false;
    std::string why;
    std::vector<CbVerdict> v = runFrames(s, &setupOk, &why);
    CHECK(setupOk);
    if (!why.empty()) std::fprintf(stderr, "  scoped: %s\n", why.c_str());
    CHECK_EQ(v.size(), size_t(2));
    for (size_t i = 0; i < v.size(); i++) {
        if (!v[i].ok()) std::fprintf(stderr, "  counterexample (scoped, frame %zu): %s\n", i + 1, verdictText(v[i], s.width, s.height).c_str());
        CHECK(v[i].checked);
        CHECK(v[i].surfaceNamed && v[i].namedForWrite);
        CHECK_EQ(v[i].passWritesToSurface, 0u);
        CHECK_EQ(v[i].wholeCopies, 1u);
        CHECK(v[i].wholeCopyLast);
        CHECK_EQ(v[i].rowsWrong, 0u);
    }
}

// Property 1 over seeded generated frame sequences: 1-6 frames, Clear or Load, 0-3 damage rectangles, 0-50 rows
// written from outside between frames, 1-2 textures over the surface, 1-2 command buffers a frame, surfaces from
// 64x64 to 1125x2436 with the qualifying rows (the width's bytes rounded up to 64; 4544 for 1125).
// **Validates: Requirements 1.1, 1.2, 1.3, 1.4**
TEST(surface_target_property_1_bug_condition_whole_frames_from_a_target) {
    std::mt19937 rng(0xca650b17u);
    auto pick = [&](unsigned n) { return n == 0 ? 0u : unsigned(rng() % n); };
    int bugCases = 0, failed = 0, printed = 0, setupFailures = 0;
    int passWrites = 0, noWholeCopy = 0, notNamedForWrite = 0, wrongBytes = 0, wrongOutside = 0;
    int damageRuns = 0, damageFailed = 0;
    for (int round = 0; round < 40; round++) {
        Scenario s;
        if (pick(5) == 0) {
            s.width = 1125;
            s.height = 2436;
        } else {
            s.width = 64 + pick(1125 - 64 + 1);
            s.height = 64 + pick(pick(2) ? 400 : 2436 - 64 + 1);
        }
        s.rowBytes = (s.width * 4 + 63) / 64 * 64;
        s.textures = 1 + pick(2);
        s.seed = uint32_t(rng());
        const unsigned frames = 1 + pick(6);
        for (unsigned f = 0; f < frames; f++) {
            Frame fr;
            const unsigned ncb = 1 + pick(2);
            fr.cbs.resize(ncb);
            if (pick(3) == 0) {
                fr.cbs[0].push_back(clearOp(pick(s.textures), uint8_t(rng()), uint8_t(rng()), uint8_t(rng()), uint8_t(rng())));
            }
            const unsigned damage = pick(4);
            for (unsigned k = 0; k < damage; k++) {
                const unsigned w = 1 + pick(std::min(200u, s.width)), h = 1 + pick(std::min(200u, s.height));
                fr.cbs[pick(ncb)].push_back(copyOp(pick(s.textures), pick(kSourceSide - w + 1), pick(kSourceSide - h + 1), pick(s.width - w + 1),
                                                   pick(s.height - h + 1), w, h));
            }
            const unsigned outside = pick(51);
            for (unsigned k = 0; k < outside; k++) fr.outsideRows.push_back(pick(s.height));
            s.frames.push_back(fr);
        }
        bool setupOk = false;
        std::string why;
        std::vector<CbVerdict> v = runFrames(s, &setupOk, &why);
        if (!setupOk) {
            setupFailures++;
            if (printed++ < 5) std::fprintf(stderr, "  round %d not run: %s\n", round, why.c_str());
            continue;
        }
        for (size_t i = 0; i < v.size(); i++) {
            if (!v[i].checked) continue;
            bugCases++;
            if (v[i].ok()) continue;
            failed++;
            passWrites += v[i].passWritesToSurface != 0;
            noWholeCopy += v[i].wholeCopies != 1 || !v[i].wholeCopyLast;
            notNamedForWrite += !(v[i].surfaceNamed && v[i].namedForWrite);
            wrongBytes += v[i].rowsWrong != 0;
            wrongOutside += v[i].rowsWrongOutside != 0;
            if (printed++ < 5) {
                std::fprintf(stderr, "  counterexample (round %d, command buffer %zu%s%s): %s\n    %s\n", round, i + 1, why.empty() ? "" : "; ", why.c_str(),
                             describe(s).c_str(), verdictText(v[i], s.width, s.height).c_str());
            }
        }
        // the same with metal-ca-perf's damage copy on: whole frames still, the copy the damage alone or whole
        bool setupOkDamage = false;
        std::string whyDamage;
        const std::vector<CbVerdict> vd = runFrames(s, &setupOkDamage, &whyDamage, true);
        damageRuns++;
        if (!setupOkDamage || !whyDamage.empty() || vd.size() != v.size()) {
            damageFailed++;
            if (printed++ < 5) std::fprintf(stderr, "  round %d with the damage copy: %s\n", round, whyDamage.c_str());
            continue;
        }
        for (size_t i = 0; i < vd.size(); i++) {
            if (vd[i].okDamage()) continue;
            damageFailed++;
            if (printed++ < 5) {
                std::fprintf(stderr, "  counterexample with the damage copy (round %d, command buffer %zu): %s\n    %s; %u fill(s) into it, a copy last: %s\n",
                             round, i + 1, describe(s).c_str(), verdictText(vd[i], s.width, s.height).c_str(), vd[i].fillsIntoSurface,
                             vd[i].copyLast ? "yes" : "no");
            }
        }
    }
    std::fprintf(stderr, "  property 1 with the damage copy on (outside writes marking the target whole): %d rounds, %d failed\n", damageRuns, damageFailed);
    CHECK_EQ(damageFailed, 0);
    std::fprintf(stderr,
                 "  property 1: %d command buffers drawing a qualifying surface, %d failed: %d with a clear or copy into the surface's memory, "
                 "%d without one whole copy last, %d not naming the surface for writing, %d with rows not the model frame's (%d with rows "
                 "holding the outside bytes)\n",
                 bugCases, failed, passWrites, noWholeCopy, notNamedForWrite, wrongBytes, wrongOutside);
    CHECK_EQ(setupFailures, 0);
    CHECK(bugCases > 40);   // the generator reaches the bug condition
    CHECK_EQ(failed, 0);
}

// Property 2 (Preservation): for every input where the bug condition does NOT hold -- another chip (redwood 0x06030500,
// 0 for a kernel that does not say), the A650 with metal_blit=0, a surface whose rows are not a multiple of 64 bytes
// (the shadow case, 4512 for 1125 pixels), two planes, a format outside the qualifying set, a descriptor one pixel off
// the surface's size, or a texture not over a surface at all (the probes' hakios_metal_texture_new_levels) -- the
// plug-in's path (hakios_metal_texture_new_iosurface_target, and hakios_metal_cb_resolve_surface_targets at commit)
// gives what today's path (hakios_metal_texture_new_iosurface, no resolve) gives: the same texture (GPU address, pitch,
// format, size, origin, IOSurface) or the same refusal, the same command-stream words word for word, the same 2D writes,
// the same buffers named by the submission, the same surface bytes, and *TARGETED 0.
//
// The observations they are written from, made on the code before the fix (see
// surface_target_preservation_observed_unfixed below): a texture over a fake 'BGRA' surface is the surface's own
// buffer -- its GPU address the surface buffer's, its pitch the surface's rows (4544 for 1125 pixels); a Clear is one
// 2D fill and a copy one 2D copy, both with that address as their base and that pitch; the surface's bytes after the
// command buffer are the clear and the copy over its old bytes; resolve records nothing (0, and not one word more).
// A rotation (1-3) is not an argument of the texture call (the plug-in's turned branch makes its own texture); it is in
// the gate's unit test.
namespace {
constexpr uint64_t kRedwood = 0x06030500;
constexpr uint32_t kFourccLA08 = 0x4c413038u, kFourccL008 = 0x4c303038u, kFourccRGhA = 0x52476841u;
constexpr unsigned kUsage = HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET | 1;
constexpr unsigned kSmall = 64;   // the small texture the copy comes from

struct Input {
    uint64_t chip = kRedwood;
    int blitOn = 1;
    bool overSurface = true;          // false: a texture of hakios_metal_texture_new_levels (the probes' kind)
    unsigned sw = 0, sh = 0, rowBytes = 0, planes = 1;
    uint32_t fourcc = kFourccBGRA;
    unsigned format = kBGRA8Unorm, w = 0, h = 0;
    uint32_t seed = 0;
    std::string kind;
};

struct Observed {
    bool setup = false;
    bool made = false;
    std::string refusal;
    int targeted = -1;
    uint64_t address = 0, pitch = 0;
    unsigned format = 0, width = 0, height = 0, origin = 0, iosurface = 0;
    int clearRc = 0, copyRc = 0, resolveRc = 0, status = 0;
    unsigned fence = 0;
    std::vector<hakios_metal_fake_bo> bos;
    std::vector<hakios_metal_fake_write> writes;
    std::vector<uint32_t> words;
    std::vector<uint8_t> surface;
};

unsigned bytesOf(unsigned format) {
    switch (format) {
    case 10: return 1;
    case 30: return 2;
    case 115: return 8;
    default: return 4;
    }
}

// INPUT run on a fresh fake: the texture (THROUGH_TARGET the plug-in's call, otherwise today's), a command buffer of a
// Clear of the whole texture and a 2D copy from a small texture into it, resolve (THROUGH_TARGET only), commit, wait
Observed observe(const Input& in, bool throughTarget) {
    Observed o;
    hakios_metal_device* d = hakios_metal_open_fake_chip(in.chip);
    if (d == nullptr) {
        o.refusal = std::string("no fake: ") + hakios_metal_error();
        return o;
    }
    std::mt19937 rng(in.seed);
    if (in.overSurface) {
        hakios_metal_fake_add_surface_planes(d, kSurface, in.sw, in.sh, in.rowBytes, in.fourcc, in.planes);
        const int64_t size = hakios_metal_fake_surface_bytes(d, kSurface, nullptr, 0);
        std::vector<uint8_t> first(size_t(size > 0 ? size : 0));
        for (uint8_t& b : first) b = uint8_t(rng());
        hakios_metal_fake_write_surface(d, kSurface, 0, first.data(), first.size());
    }
    hakios_metal_texture* t = nullptr;
    if (!in.overSurface) {
        t = hakios_metal_texture_new_levels(d, 2, in.format, in.w, in.h, 1, kUsage);
    } else if (throughTarget) {
        t = hakios_metal_texture_new_iosurface_target(d, kSurface, in.format, in.w, in.h, kUsage, in.blitOn, &o.targeted);
    } else {
        t = hakios_metal_texture_new_iosurface(d, kSurface, 0, in.format, in.w, in.h, kUsage);
    }
    o.setup = true;
    o.made = t != nullptr;
    if (t == nullptr) {
        o.refusal = hakios_metal_error();
    } else {
        o.address = hakios_metal_texture_gpu_address(t);
        o.pitch = hakios_metal_texture_bytes_per_row(t);
        o.format = hakios_metal_texture_format(t);
        o.width = hakios_metal_texture_width(t);
        o.height = hakios_metal_texture_height(t);
        o.origin = hakios_metal_texture_origin(t);
        o.iosurface = hakios_metal_texture_iosurface(t);
        const unsigned bpp = bytesOf(in.format);
        hakios_metal_texture* src = hakios_metal_texture_new(d, 2, in.format, kSmall, kSmall, 1);
        std::vector<uint8_t> px(size_t(kSmall) * kSmall * bpp);
        for (uint8_t& b : px) b = uint8_t(rng());
        hakios_metal_queue* q = hakios_metal_queue_new(d);
        if (src == nullptr || q == nullptr || hakios_metal_texture_replace(src, 0, 0, kSmall, kSmall, px.data(), kSmall * bpp) != 0) {
            o.setup = false;
            o.refusal = std::string("source or queue: ") + hakios_metal_error();
        } else {
            hakios_metal_cb* cb = hakios_metal_cb_new(q);
            hakios_metal_render_pass pass{};
            pass.color0 = t;
            pass.load_action = 2;
            pass.store_action = 1;
            for (double& c : pass.clear_color) c = double(rng() % 256) / 255.0;
            o.clearRc = hakios_metal_cb_clear(cb, &pass);
            const unsigned cw = 1 + rng() % std::min(kSmall, o.width), ch = 1 + rng() % std::min(kSmall, o.height);
            const unsigned dx = rng() % (o.width - cw + 1), dy = rng() % (o.height - ch + 1);
            o.copyRc = hakios_metal_cb_blit(cb, src, 0, 0, 0, cw, ch, t, 0, dx, dy, cw, ch, 0);
            if (throughTarget) o.resolveRc = hakios_metal_cb_resolve_surface_targets(cb);
            hakios_metal_cb_commit(cb, nullptr, nullptr);
            hakios_metal_cb_wait(cb);
            o.status = int(hakios_metal_cb_status(cb));
            hakios_metal_cb_free(cb);
        }
        if (q != nullptr) hakios_metal_queue_free(q);
        if (src != nullptr) hakios_metal_texture_free(src);
        hakios_metal_texture_free(t);
    }
    std::vector<hakios_metal_fake_bo> bos(256);
    const int nb = hakios_metal_fake_last_submission(d, &o.fence, bos.data(), unsigned(bos.size()));
    o.bos.assign(bos.begin(), bos.begin() + std::max(0, std::min(nb, int(bos.size()))));
    std::vector<hakios_metal_fake_write> writes(4096);
    const int nw = hakios_metal_fake_writes(d, writes.data(), unsigned(writes.size()), 1);
    o.writes.assign(writes.begin(), writes.begin() + std::max(0, std::min(nw, int(writes.size()))));
    const int64_t nwords = hakios_metal_fake_words(d, nullptr, 0, 0);
    o.words.resize(size_t(nwords > 0 ? nwords : 0));
    hakios_metal_fake_words(d, o.words.data(), o.words.size(), 1);
    if (in.overSurface) {
        const int64_t size = hakios_metal_fake_surface_bytes(d, kSurface, nullptr, 0);
        o.surface.resize(size_t(size > 0 ? size : 0));
        hakios_metal_fake_surface_bytes(d, kSurface, o.surface.data(), o.surface.size());
    }
    hakios_metal_close(d);
    return o;
}

// where the plug-in's path differs from today's, in words; empty when it does not
std::string difference(const Observed& a, const Observed& b) {
    if (a.setup != b.setup) return "set up on one path only";
    if (a.made != b.made) return std::string("a texture on one path only (") + (a.made ? b.refusal : a.refusal) + ")";
    if (a.refusal != b.refusal) return "refusals differ: '" + a.refusal + "' against '" + b.refusal + "'";
    char t[256];
    if (a.address != b.address || a.pitch != b.pitch || a.format != b.format || a.width != b.width || a.height != b.height || a.origin != b.origin ||
        a.iosurface != b.iosurface) {
        snprintf(t, sizeof(t), "texture 0x%llx pitch %llu format %u %ux%u origin %u surface %u against 0x%llx pitch %llu format %u %ux%u origin %u surface %u",
                 (unsigned long long)a.address, (unsigned long long)a.pitch, a.format, a.width, a.height, a.origin, a.iosurface,
                 (unsigned long long)b.address, (unsigned long long)b.pitch, b.format, b.width, b.height, b.origin, b.iosurface);
        return t;
    }
    if (a.clearRc != b.clearRc || a.copyRc != b.copyRc || a.status != b.status) {
        snprintf(t, sizeof(t), "clear %d copy %d status %d against clear %d copy %d status %d", a.clearRc, a.copyRc, a.status, b.clearRc, b.copyRc, b.status);
        return t;
    }
    if (a.words != b.words) {
        size_t i = 0;
        while (i < a.words.size() && i < b.words.size() && a.words[i] == b.words[i]) i++;
        snprintf(t, sizeof(t), "command streams differ at word %zu (%zu words against %zu)", i, a.words.size(), b.words.size());
        return t;
    }
    if (a.fence != b.fence || a.bos.size() != b.bos.size()) return "the submissions differ (fence or buffer count)";
    for (size_t i = 0; i < a.bos.size(); i++) {
        if (a.bos[i].gpu_address != b.bos[i].gpu_address || a.bos[i].size != b.bos[i].size || a.bos[i].flags != b.bos[i].flags ||
            a.bos[i].surface != b.bos[i].surface)
            return "the submission's buffer " + std::to_string(i) + " differs";
    }
    if (a.writes.size() != b.writes.size()) return "2D write counts differ";
    for (size_t i = 0; i < a.writes.size(); i++) {
        const hakios_metal_fake_write &x = a.writes[i], &y = b.writes[i];
        if (x.submission != y.submission || x.fill != y.fill || x.base != y.base || x.pitch != y.pitch || x.bytes_per_pixel != y.bytes_per_pixel ||
            x.x0 != y.x0 || x.y0 != y.y0 || x.x1 != y.x1 || x.y1 != y.y1)
            return "2D write " + std::to_string(i) + " differs";
    }
    if (a.surface != b.surface) return "the surface's bytes differ";
    return "";
}

unsigned rows64(unsigned bytes) { return (bytes + 63) / 64 * 64; }
}  // namespace

// The observations of the code before the fix, written down: redwood and the A650 with metal_blit=0, the phone's display
// surface (1125x2436 'BGRA', 4544-byte rows). **Validates: Requirements 3.5, 3.9**
TEST(surface_target_preservation_observed_unfixed) {
    const uint64_t chips[] = {kRedwood, kA650};
    for (uint64_t chip : chips) {
        Input in;
        in.chip = chip;
        in.blitOn = chip == kA650 ? 0 : 1;
        in.sw = in.w = 1125;
        in.sh = in.h = 2436;
        in.rowBytes = 4544;
        in.seed = 0x0b5e7u;
        const Observed o = observe(in, true);
        CHECK(o.setup && o.made);
        if (!o.made) {
            std::fprintf(stderr, "  observed (chip 0x%llx): refused: %s\n", (unsigned long long)chip, o.refusal.c_str());
            continue;
        }
        CHECK_EQ(o.targeted, 0);
        CHECK_EQ(o.resolveRc, 0);
        CHECK_EQ(o.status, int(HAKIOS_METAL_COMPLETED));
        CHECK_EQ(o.origin, 1u);             // an IOSurface's pages
        CHECK_EQ(o.iosurface, kSurface);
        CHECK_EQ(o.pitch, uint64_t(4544));  // the surface's rows
        CHECK_EQ(o.format, kBGRA8Unorm);
        // the texture's GPU address is the surface buffer's, named by the submission for writing
        bool named = false;
        for (const hakios_metal_fake_bo& b : o.bos) {
            if (b.surface != kSurface) continue;
            named = true;
            CHECK_EQ(b.gpu_address, o.address);
            CHECK((b.flags & 2) != 0);
        }
        CHECK(named);
        // the Clear is one 2D fill of the whole texture and the copy one 2D copy, both at the surface's address and pitch
        CHECK_EQ(o.writes.size(), size_t(2));
        if (o.writes.size() == 2) {
            CHECK(o.writes[0].fill != 0);
            CHECK(o.writes[1].fill == 0);
            for (const hakios_metal_fake_write& w : o.writes) {
                CHECK_EQ(w.base, o.address);
                CHECK_EQ(w.pitch, uint64_t(4544));
                CHECK_EQ(w.bytes_per_pixel, 4u);
                CHECK_EQ(w.submission, o.fence);
            }
            CHECK_EQ(o.writes[0].x1, 1124u);
            CHECK_EQ(o.writes[0].y1, 2435u);
        }
        // resolve adds nothing: the same words as today's path, which calls no resolve
        const Observed today = observe(in, false);
        CHECK(today.words == o.words);
        CHECK(!o.words.empty());
        std::fprintf(stderr, "  observed (chip 0x%llx, metal_blit %d): texture 0x%llx pitch %llu, %zu writes at the surface's base, %zu stream words, "
                             "resolve %d, targeted %d\n",
                     (unsigned long long)chip, in.blitOn, (unsigned long long)o.address, (unsigned long long)o.pitch, o.writes.size(), o.words.size(),
                     o.resolveRc, o.targeted);
    }
    // the shadow case (rows of 4512 bytes, not a multiple of 64) is refused by name on today's path: the plug-in's
    // shadow branch handles it, and the target path refuses it the same
    Input sh;
    sh.chip = kA650;
    sh.sw = sh.w = 1125;
    sh.sh = sh.h = 64;
    sh.rowBytes = 4512;
    const Observed a = observe(sh, false), b = observe(sh, true);
    CHECK(!a.made && !b.made);
    CHECK(a.refusal.find("multiples of 64") != std::string::npos);
    const std::string diff = difference(a, b);
    if (!diff.empty()) std::fprintf(stderr, "  shadow case: %s\n", diff.c_str());
    CHECK(diff.empty());
}

// The gate, each condition of the glossary's qualifying surface false on its own: false. (Before the fix the gate is
// false everywhere, which passes too.) **Validates: Requirements 3.5, 3.9**
TEST(surface_target_wanted_each_condition_false_alone) {
    struct Args {
        uint64_t chip = kA650;
        int blit = 1;
        unsigned format = kBGRA8Unorm, w = 1125, h = 2436, sw = 1125, sh = 2436, planes = 1, plane = 0, rotation = 0;
        uint64_t rowBytes = 4544;
    };
    auto wanted = [](const Args& a) {
        return hakios_metal_surface_target_wanted(a.chip, a.blit, a.format, a.w, a.h, a.sw, a.sh, a.planes, a.plane, a.rotation, a.rowBytes);
    };
    std::vector<std::pair<const char*, Args>> off;
    Args a;
    a.chip = kRedwood;
    off.push_back({"redwood", a});
    a = Args{};
    a.chip = 0;
    off.push_back({"chip 0", a});
    a = Args{};
    a.blit = 0;
    off.push_back({"metal_blit=0", a});
    for (unsigned f : {73u, 72u, 55u, 115u, 10u, 30u, 500u, 252u}) {
        a = Args{};
        a.format = f;
        off.push_back({"a format outside the set", a});
    }
    a = Args{};
    a.w = 1124;
    off.push_back({"width one less", a});
    a = Args{};
    a.h = 2437;
    off.push_back({"height one more", a});
    a = Args{};
    a.planes = 2;
    off.push_back({"two planes", a});
    a = Args{};
    a.plane = 1;
    off.push_back({"plane 1", a});
    for (unsigned r : {1u, 2u, 3u}) {
        a = Args{};
        a.rotation = r;
        off.push_back({"a rotation", a});
    }
    a = Args{};
    a.rowBytes = 4512;
    off.push_back({"rows of 4512 bytes (shadow)", a});
    for (const auto& c : off) {
        if (wanted(c.second)) std::fprintf(stderr, "  the gate holds for %s\n", c.first);
        CHECK(!wanted(c.second));
    }
}

// Property 2 over seeded generated non-bug inputs. **Validates: Requirements 3.1, 3.2, 3.3, 3.5, 3.9**
TEST(surface_target_property_2_preservation_outside_the_bug_condition) {
    std::mt19937 rng(0x9e5e2u);
    auto pick = [&](unsigned n) { return n == 0 ? 0u : unsigned(rng() % n); };
    const unsigned qualifying[] = {80, 81, 70, 71};
    int cases = 0, failed = 0, made = 0, refused = 0;
    std::map<std::string, int> kinds;
    for (int round = 0; round < 60; round++) {
        Input in;
        in.seed = uint32_t(rng());
        if (pick(6) == 0) {
            in.sw = 1125;
            in.sh = 2436;
        } else {
            in.sw = 64 + pick(1125 - 64 + 1);
            in.sh = 64 + pick(pick(2) ? 400 : 2436 - 64 + 1);
        }
        in.w = in.sw;
        in.h = in.sh;
        in.format = qualifying[pick(4)];
        in.rowBytes = rows64(in.sw * 4);
        in.chip = kA650;
        in.blitOn = 1;
        switch (pick(7)) {
        case 0:
            in.chip = pick(2) ? kRedwood : 0;
            in.blitOn = int(pick(2));
            in.kind = "another chip";
            break;
        case 1:
            in.blitOn = 0;
            in.kind = "metal_blit=0";
            break;
        case 2:
            in.rowBytes = (in.sw * 4 + 31) / 32 * 32;
            if (in.rowBytes % 64 == 0) in.rowBytes += 32;
            in.kind = "rows not a multiple of 64";
            break;
        case 3:
            in.planes = 2;
            in.kind = "two planes";
            break;
        case 4: {
            struct F { unsigned format; uint32_t fourcc; };
            const F others[] = {{73, kFourccBGRA}, {72, kFourccBGRA}, {55, kFourccBGRA}, {60, kFourccBGRA}, {92, kFourccBGRA},
                                {30, kFourccLA08}, {10, kFourccL008}, {115, kFourccRGhA}};
            const F f = others[pick(8)];
            in.format = f.format;
            in.fourcc = f.fourcc;
            in.rowBytes = rows64(in.sw * bytesOf(f.format));
            in.kind = "a format outside the set";
            break;
        }
        case 5:
            switch (pick(4)) {
            case 0: in.w = in.sw - 1; break;
            case 1: in.h = in.sh - 1; break;
            case 2: in.w = in.sw + 1; break;
            default: in.h = in.sh + 1; break;
            }
            in.kind = "a size one pixel off";
            break;
        default:
            in.overSurface = false;
            in.chip = pick(3) == 0 ? kRedwood : kA650;
            in.blitOn = int(pick(2));
            in.kind = "not over a surface";
            break;
        }
        const Observed today = observe(in, false), plugin = observe(in, true);
        cases++;
        kinds[in.kind]++;
        (today.made ? made : refused)++;
        std::string diff = difference(today, plugin);
        if (diff.empty() && in.overSurface && plugin.targeted != 0) diff = "targeted " + std::to_string(plugin.targeted);
        if (diff.empty() && plugin.resolveRc != 0) diff = "resolve returned " + std::to_string(plugin.resolveRc);
        // (observed: a Clear the 2D engine does not take -- RGBA8Snorm 72, RGBA8Uint 73 -- is refused on both paths and the
        // command buffer ends in error with nothing submitted: the same, which is what is held; a stream is asked for
        // only of a command buffer that completed)
        if (diff.empty() && today.made && today.status == int(HAKIOS_METAL_COMPLETED) && today.words.empty())
            diff = "no command stream observed (clear " + std::to_string(today.clearRc) + ", copy " + std::to_string(today.copyRc) + ", status " +
                   std::to_string(today.status) + ")";
        if (diff.empty()) continue;
        if (failed++ < 5) {
            std::fprintf(stderr, "  counterexample (round %d, %s: chip 0x%llx metal_blit %d, %ux%u surface rows %u planes %u fourcc 0x%08x, format %u %ux%u): %s\n",
                         round, in.kind.c_str(), (unsigned long long)in.chip, in.blitOn, in.sw, in.sh, in.rowBytes, in.planes, in.fourcc, in.format, in.w,
                         in.h, diff.c_str());
        }
    }
    std::string k;
    for (const auto& e : kinds) k += " " + e.first + " " + std::to_string(e.second) + ";";
    std::fprintf(stderr, "  property 2: %d non-bug inputs (%d textures, %d refusals on both paths),%s %d differ\n", cases, made, refused, k.c_str(), failed);
    CHECK_EQ(failed, 0);
    CHECK(made > 10);
    CHECK_EQ(kinds.size(), size_t(7));
}

// --- unit tests of the C API's surface targets (task 4.1) ----------------------------------------------------------------
namespace {
// a fake A650 with a 'BGRA' surface ID of W x H, rows rounded up to 64 bytes, filled with SEED's bytes
hakios_metal_device* a650WithSurface(unsigned id, unsigned w, unsigned h, uint8_t seed) {
    hakios_metal_device* d = hakios_metal_open_fake_chip(kA650);
    hakios_metal_fake_add_surface(d, id, w, h, rows64(w * 4), kFourccBGRA);
    std::vector<uint8_t> b(size_t(rows64(w * 4)) * h, seed);
    hakios_metal_fake_write_surface(d, id, 0, b.data(), b.size());
    return d;
}
hakios_metal_texture* targetOver(hakios_metal_device* d, unsigned id, unsigned w, unsigned h, int* targeted, unsigned format = kBGRA8Unorm) {
    return hakios_metal_texture_new_iosurface_target(d, id, format, w, h, kUsage, 1, targeted);
}
int clearInto(hakios_metal_cb* cb, hakios_metal_texture* t, double v) {
    hakios_metal_render_pass pass{};
    pass.color0 = t;
    pass.load_action = 2;
    pass.store_action = 1;
    for (double& c : pass.clear_color) c = v;
    return hakios_metal_cb_clear(cb, &pass);
}
}  // namespace

// A target is made once per surface ID, filled from the surface, shared by a second texture over it (also in the sRGB
// format of the same bytes), kept while any lives, freed with the last; a new one is filled from the surface again.
TEST(surface_target_lifetime_shared_and_freed_with_the_last_texture) {
    hakios_metal_device* d = a650WithSurface(7, 128, 64, 0x33);
    int ta = -1, tb = -1;
    hakios_metal_texture* a = targetOver(d, 7, 128, 64, &ta);
    hakios_metal_texture* b = targetOver(d, 7, 128, 64, &tb, 81);
    CHECK(a != nullptr && b != nullptr);
    CHECK_EQ(ta, 1);
    CHECK_EQ(tb, 1);
    unsigned live = 0, made = 0, filled = 0, copies = 0, over = 0;
    uint64_t bytes = 0;
    hakios_metal_surface_target_stats(d, &live, &bytes, &made, &filled, &copies, &over);
    CHECK_EQ(live, 1u);
    CHECK_EQ(made, 1u);
    CHECK_EQ(filled, 1u);
    CHECK(bytes >= uint64_t(128) * 4 * 64);
    // the target's memory, not the surface's; the two share it; the link answers the surface's texture
    CHECK(hakios_metal_texture_gpu_address(a) == hakios_metal_texture_gpu_address(b));
    hakios_metal_texture* s = hakios_metal_texture_surface_of(a);
    CHECK(s != nullptr);
    CHECK(hakios_metal_texture_gpu_address(s) != hakios_metal_texture_gpu_address(a));
    CHECK_EQ(hakios_metal_texture_iosurface(s), 7u);
    CHECK(hakios_metal_texture_surface_of(s) == nullptr);
    // filled from the surface
    std::vector<uint8_t> px(128 * 4 * 64);
    CHECK_EQ(hakios_metal_texture_read(a, 0, 0, 128, 64, px.data(), 128 * 4), 0);
    CHECK(std::all_of(px.begin(), px.end(), [](uint8_t v) { return v == 0x33; }));
    hakios_metal_texture_free(a);
    hakios_metal_surface_target_stats(d, &live, nullptr, nullptr, nullptr, nullptr, nullptr);
    CHECK_EQ(live, 1u);
    hakios_metal_texture_free(b);
    hakios_metal_surface_target_stats(d, &live, &bytes, nullptr, nullptr, nullptr, nullptr);
    CHECK_EQ(live, 0u);
    CHECK_EQ(bytes, uint64_t(0));
    // a new one starts from the surface's own pixels
    std::vector<uint8_t> fresh(size_t(rows64(128 * 4)) * 64, 0x44);
    hakios_metal_fake_write_surface(d, 7, 0, fresh.data(), fresh.size());
    int tc = -1;
    hakios_metal_texture* c = targetOver(d, 7, 128, 64, &tc);
    CHECK_EQ(tc, 1);
    CHECK_EQ(hakios_metal_texture_read(c, 0, 0, 128, 64, px.data(), 128 * 4), 0);
    CHECK(std::all_of(px.begin(), px.end(), [](uint8_t v) { return v == 0x44; }));
    hakios_metal_surface_target_stats(d, &live, nullptr, &made, &filled, nullptr, nullptr);
    CHECK_EQ(live, 1u);
    CHECK_EQ(made, 1u);
    hakios_metal_texture_free(c);
    hakios_metal_close(d);
}

// Past 128 MiB of targets a surface is drawn straight (the surface texture, *TARGETED 0, counted); room again once a
// target goes.
TEST(surface_target_budget_edge) {
    hakios_metal_device* d = hakios_metal_open_fake_chip(kA650);
    const unsigned W = 4096, H = 2800;   // 45.9 MB a target: two fit in 128 MiB, a third does not
    for (unsigned id = 1; id <= 3; id++) hakios_metal_fake_add_surface(d, id, W, H, W * 4, kFourccBGRA);
    int t1 = -1, t2 = -1, t3 = -1;
    hakios_metal_texture* a = targetOver(d, 1, W, H, &t1);
    hakios_metal_texture* b = targetOver(d, 2, W, H, &t2);
    hakios_metal_texture* c = targetOver(d, 3, W, H, &t3);
    CHECK(a != nullptr && b != nullptr && c != nullptr);
    CHECK_EQ(t1, 1);
    CHECK_EQ(t2, 1);
    CHECK_EQ(t3, 0);
    CHECK_EQ(hakios_metal_texture_iosurface(c), 3u);   // the surface's own pages
    unsigned live = 0, over = 0;
    uint64_t bytes = 0;
    hakios_metal_surface_target_stats(d, &live, &bytes, nullptr, nullptr, nullptr, &over);
    CHECK_EQ(live, 2u);
    CHECK_EQ(over, 1u);
    CHECK(bytes <= (uint64_t(128) << 20));
    hakios_metal_texture_free(a);
    hakios_metal_texture_free(c);
    c = targetOver(d, 3, W, H, &t3);
    CHECK_EQ(t3, 1);
    hakios_metal_texture_free(b);
    hakios_metal_texture_free(c);
    hakios_metal_close(d);
}

// resolve: one copy per target written, after every pass, none twice for two passes into one target, none for a
// command buffer that wrote none (and none on a second call with nothing new), the same with deferred recording.
TEST(surface_target_resolve_one_copy_per_written_target) {
    for (int deferred = 0; deferred < 2; deferred++) {
        hakios_metal_device* d = a650WithSurface(1, 256, 128, 0x10);
        std::vector<uint8_t> b(size_t(rows64(192 * 4)) * 64, 0x20);
        hakios_metal_fake_add_surface(d, 2, 192, 64, rows64(192 * 4), kFourccBGRA);
        hakios_metal_fake_write_surface(d, 2, 0, b.data(), b.size());
        int ta = -1, tb = -1;
        hakios_metal_texture* x = targetOver(d, 1, 256, 128, &ta);
        hakios_metal_texture* y = targetOver(d, 2, 192, 64, &tb);
        hakios_metal_texture* plain = hakios_metal_texture_new(d, 2, kBGRA8Unorm, 64, 64, kUsage);
        hakios_metal_queue* q = hakios_metal_queue_new(d);
        hakios_metal_queue_set_deferred(q, deferred);
        CHECK(ta == 1 && tb == 1);
        uint8_t one[4];
        hakios_metal_texture_read(x, 0, 0, 1, 1, one, 4);   // (waits for the targets' fills, so their writes are out of the record below)
        hakios_metal_texture_read(y, 0, 0, 1, 1, one, 4);
        unsigned copies = 0;
        hakios_metal_surface_target_stats(d, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        // two passes into X, one into Y: two copies, X's and Y's, last
        hakios_metal_cb* cb = hakios_metal_cb_new(q);
        CHECK_EQ(clearInto(cb, x, 1.0), 0);
        CHECK_EQ(clearInto(cb, y, 0.0), 0);
        CHECK_EQ(clearInto(cb, x, 0.0), 0);
        hakios_metal_fake_writes(d, nullptr, 0, 1);
        CHECK_EQ(hakios_metal_cb_resolve_surface_targets(cb), 2);
        CHECK_EQ(hakios_metal_cb_resolve_surface_targets(cb), 0);
        hakios_metal_cb_commit(cb, nullptr, nullptr);
        hakios_metal_cb_wait(cb);
        CHECK_EQ(int(hakios_metal_cb_status(cb)), int(HAKIOS_METAL_COMPLETED));
        hakios_metal_cb_free(cb);
        std::vector<hakios_metal_fake_write> w(64);
        const int n = hakios_metal_fake_writes(d, w.data(), unsigned(w.size()), 1);
        CHECK_EQ(n, 5);
        if (n == 5) {
            const uint64_t sx = hakios_metal_texture_gpu_address(hakios_metal_texture_surface_of(x));
            const uint64_t sy = hakios_metal_texture_gpu_address(hakios_metal_texture_surface_of(y));
            CHECK_EQ(w[3].base, sx);
            CHECK_EQ(w[4].base, sy);
            CHECK(w[3].fill == 0 && w[3].x1 == 255 && w[3].y1 == 127);
            CHECK(w[4].fill == 0 && w[4].x1 == 191 && w[4].y1 == 63);
        }
        // the surfaces hold the last drawing: X black (0), Y black (0) -- B G R A all zero
        std::vector<uint8_t> s(size_t(hakios_metal_fake_surface_bytes(d, 1, nullptr, 0)));
        hakios_metal_fake_surface_bytes(d, 1, s.data(), s.size());
        CHECK(s[0] == 0 && s[3] == 0 && s[size_t(127) * rows64(256 * 4) + 255 * 4 + 3] == 0);
        hakios_metal_surface_target_stats(d, nullptr, nullptr, nullptr, nullptr, &copies, nullptr);
        CHECK_EQ(copies, 2u);
        // a command buffer that wrote no target: nothing recorded
        cb = hakios_metal_cb_new(q);
        CHECK_EQ(clearInto(cb, plain, 1.0), 0);
        CHECK_EQ(hakios_metal_cb_resolve_surface_targets(cb), 0);
        hakios_metal_cb_commit(cb, nullptr, nullptr);
        hakios_metal_cb_wait(cb);
        hakios_metal_cb_free(cb);
        CHECK_EQ(hakios_metal_fake_writes(d, w.data(), unsigned(w.size()), 1), 1);
        hakios_metal_queue_free(q);
        hakios_metal_texture_free(plain);
        hakios_metal_texture_free(x);
        hakios_metal_texture_free(y);
        hakios_metal_close(d);
    }
}

// replaceRegion's C API part: a targeted texture's region written into the target and, through the link, the surface.
TEST(surface_target_replace_writes_through_the_link) {
    hakios_metal_device* d = a650WithSurface(5, 64, 64, 0x00);
    int t = -1;
    hakios_metal_texture* x = targetOver(d, 5, 64, 64, &t);
    CHECK_EQ(t, 1);
    std::vector<uint8_t> px(16 * 4 * 8, 0x7e);
    CHECK_EQ(hakios_metal_texture_replace(x, 8, 4, 16, 8, px.data(), 16 * 4), 0);
    CHECK_EQ(hakios_metal_texture_replace(hakios_metal_texture_surface_of(x), 8, 4, 16, 8, px.data(), 16 * 4), 0);
    std::vector<uint8_t> got(16 * 4 * 8);
    CHECK_EQ(hakios_metal_texture_read(x, 8, 4, 16, 8, got.data(), 16 * 4), 0);
    CHECK(got == px);
    std::vector<uint8_t> s(size_t(hakios_metal_fake_surface_bytes(d, 5, nullptr, 0)));
    hakios_metal_fake_surface_bytes(d, 5, s.data(), s.size());
    CHECK_EQ(s[size_t(4) * 256 + 8 * 4], 0x7e);
    CHECK_EQ(s[0], 0x00);
    hakios_metal_texture_free(x);
    hakios_metal_close(d);
}

// --- metal-ca-perf, Property 1 (Bug Condition), C2: only the damage copied ---------------------------------------------
// On the A650 with metal_blit on, the commit copy of a surface target (hakios_metal_cb_resolve_surface_targets) SHALL
// copy only what the command buffer wrote into the target since its last resolve -- its damage, aligned outward to 16
// pixels by 4 rows and clamped to the target (the whole target for a Clear) -- and the surface's bytes after the command
// buffer SHALL equal what the whole-target copy gives (the target's bytes: the surface is the target after each resolve).
// The damage here: draws scissored to a rectangle (with the compiler in the build: tri_flat, a Load pass) and copies of a
// rectangle of a small texture into the target (the 2D engine; the fake carries those out).
//
// On the code before the fix (the seam: CommandBuffer::damageOf reports the whole target) the copy is whole for any
// damage, so this property FAILS for every Load with a damage smaller than the target: that is the bug.
namespace {
struct DamageOp {
    bool clear = false, draw = false;   // a Clear of the whole target; a draw scissored to R; else a copy into R
    bool dontCare = false;              // (a draw) its pass loads with DontCare: the whole target is damage
    unsigned target = 0;
    Rect r;
    unsigned sx = 0, sy = 0;
    uint8_t px[4] = {0, 0, 0, 0};
};
struct DamageCase {
    std::vector<std::pair<unsigned, unsigned>> sizes;   // per target (one surface each): width, height
    uint32_t seed = 0;
    std::vector<std::vector<DamageOp>> frames;          // one command buffer a frame
};
struct DamageVerdict {
    unsigned frame = 0, target = 0;
    unsigned W = 0, H = 0;
    Rect damage;                 // aligned and clamped
    bool damageWhole = false;
    unsigned copies = 0;         // 2D copies into the surface's memory in the submission
    Rect copied;                 // the last of them
    bool copyWithinDamage = true;
    unsigned rowsWrong = 0;      // rows of the surface that are not the target's
    bool ok() const { return copies >= 1 && copyWithinDamage && rowsWrong == 0; }
};
// the damage union R grown by A, aligned outward to 16 pixels by 4 rows and clamped to W x H
void growAligned(Rect* acc, int* any, const Rect& a, unsigned W, unsigned H) {
    unsigned x0 = a.x & ~15u, y0 = a.y & ~3u;
    unsigned x1 = std::min((a.x + a.w - 1) | 15u, W - 1), y1 = std::min((a.y + a.h - 1) | 3u, H - 1);
    if (*any) {
        x0 = std::min(x0, acc->x);
        y0 = std::min(y0, acc->y);
        x1 = std::max(x1, acc->x + acc->w - 1);
        y1 = std::max(y1, acc->y + acc->h - 1);
    }
    *acc = Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    *any = 1;
}
std::string damageText(const DamageVerdict& v) {
    char t[384];
    snprintf(t, sizeof(t),
             "frame %u target %u (%ux%u): damage %ux%u at %u,%u%s (%.1f %% of the target); %u copy(ies) into the surface, the last %ux%u at "
             "%u,%u%s; %u row(s) of the surface not the target's",
             v.frame + 1, v.target, v.W, v.H, v.damage.w, v.damage.h, v.damage.x, v.damage.y, v.damageWhole ? " (whole)" : "",
             100.0 * v.damage.w * v.damage.h / (double(v.W) * v.H), v.copies, v.copied.w, v.copied.h, v.copied.x, v.copied.y,
             v.copyWithinDamage ? "" : ", NOT within the damage", v.rowsWrong);
    return t;
}
// Runs C on a fake A650 through the plug-in's calls; one verdict per target written per command buffer. PROGRAM the
// draws' (nullptr: no draws in C). *WHY a refusal. STREAMS (when given) gets per command buffer an FNV-1a 64 of the
// command-stream words the fake's CP read for it (metal-ca-perf's preservation property).
// DAMAGE_OFF: the device's damage copy off (hakios_metal_surface_target_set_damage 0, metal_damage=0).
std::vector<DamageVerdict> runDamage(const DamageCase& c, hakios_metal_program* program, std::string* why, std::vector<uint64_t>* streams = nullptr,
                                     bool damageOff = false) {
    std::vector<DamageVerdict> out;
    hakios_metal_device* d = hakios_metal_open_fake_chip(kA650);
    if (d == nullptr) {
        *why = "no fake A650";
        return out;
    }
    if (damageOff) hakios_metal_surface_target_set_damage(d, 0);
    std::mt19937 rng(c.seed);
    const unsigned n = unsigned(c.sizes.size());
    std::vector<hakios_metal_texture*> tex(n, nullptr);
    std::vector<uint64_t> sbase(n), ssize(n);
    for (unsigned i = 0; i < n; i++) {
        const unsigned W = c.sizes[i].first, H = c.sizes[i].second, RB = rows64(W * 4);
        hakios_metal_fake_add_surface(d, 1 + i, W, H, RB, kFourccBGRA);
        std::vector<uint8_t> b(size_t(RB) * H);
        for (uint8_t& v : b) v = uint8_t(rng());
        hakios_metal_fake_write_surface(d, 1 + i, 0, b.data(), b.size());
        int targeted = -1;
        tex[i] = hakios_metal_texture_new_iosurface_target(d, 1 + i, kBGRA8Unorm, W, H, kUsage, 1, &targeted);
        if (tex[i] == nullptr || targeted != 1) {
            *why = std::string("no surface target: ") + hakios_metal_error();
            for (hakios_metal_texture* t : tex)
                if (t != nullptr) hakios_metal_texture_free(t);
            hakios_metal_close(d);
            return out;
        }
        uint8_t one[4];
        hakios_metal_texture_read(tex[i], 0, 0, 1, 1, one, 4);   // (the target's fill done before the record below)
    }
    hakios_metal_texture* src = hakios_metal_texture_new(d, 2, kBGRA8Unorm, kSourceSide, kSourceSide, 1);
    std::vector<uint8_t> srcPx(size_t(kSourceSide) * kSourceSide * 4);
    for (uint8_t& b : srcPx) b = uint8_t(rng());
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    if (src == nullptr || q == nullptr || hakios_metal_texture_replace(src, 0, 0, kSourceSide, kSourceSide, srcPx.data(), kSourceSide * 4) != 0) {
        *why = std::string("source texture or queue refused: ") + hakios_metal_error();
    }
    std::vector<hakios_metal_fake_bo> bos(256);
    std::vector<hakios_metal_fake_write> writes(4096);
    for (size_t f = 0; f < c.frames.size() && why->empty(); f++) {
        hakios_metal_fake_writes(d, nullptr, 0, 1);
        if (streams != nullptr) hakios_metal_fake_words(d, nullptr, 0, 1);
        hakios_metal_cb* cb = hakios_metal_cb_new(q);
        std::vector<Rect> dmg(n);
        std::vector<int> any(n, 0);
        std::vector<bool> whole(n, false), written(n, false);
        for (const DamageOp& o : c.frames[f]) {
            const unsigned W = c.sizes[o.target].first, H = c.sizes[o.target].second;
            int rc = 0;
            if (o.clear) {
                hakios_metal_render_pass pass{};
                pass.color0 = tex[o.target];
                pass.load_action = 2;
                pass.store_action = 1;
                pass.clear_color[0] = o.px[2] / 255.0;
                pass.clear_color[1] = o.px[1] / 255.0;
                pass.clear_color[2] = o.px[0] / 255.0;
                pass.clear_color[3] = o.px[3] / 255.0;
                rc = hakios_metal_cb_clear(cb, &pass);
                whole[o.target] = true;
            } else if (o.draw) {
                hakios_metal_render_pass pass{};
                pass.color0 = tex[o.target];
                pass.load_action = o.dontCare ? 0 : 1;
                pass.store_action = 1;
                struct hakios_metal_scissor sc = {o.r.x, o.r.y, o.r.w, o.r.h};
                rc = hakios_metal_cb_draw(cb, program, &pass, nullptr, &sc, 3, 0, 3, 1, nullptr, nullptr, 0, nullptr, nullptr, 0);
                if (o.dontCare) {
                    whole[o.target] = true;
                } else {
                    growAligned(&dmg[o.target], &any[o.target], o.r, W, H);
                }
            } else {
                rc = hakios_metal_cb_blit(cb, src, 0, o.sx, o.sy, o.r.w, o.r.h, tex[o.target], 0, o.r.x, o.r.y, o.r.w, o.r.h, 0);
                growAligned(&dmg[o.target], &any[o.target], o.r, W, H);
            }
            if (rc != 0 && why->empty()) *why = std::string("work refused: ") + hakios_metal_error();
            written[o.target] = true;
        }
        if (hakios_metal_cb_resolve_surface_targets(cb) < 0 && why->empty()) *why = std::string("resolve: ") + hakios_metal_error();
        hakios_metal_cb_commit(cb, nullptr, nullptr);
        hakios_metal_cb_wait(cb);
        if (hakios_metal_cb_status(cb) != HAKIOS_METAL_COMPLETED && why->empty()) *why = std::string("cb: ") + hakios_metal_cb_error(cb);
        if (streams != nullptr) {
            std::vector<uint32_t> words(size_t(std::max<int64_t>(0, hakios_metal_fake_words(d, nullptr, 0, 0))));
            hakios_metal_fake_words(d, words.data(), words.size(), 1);
            uint64_t h = 0xcbf29ce484222325ull;
            for (uint32_t w : words) {
                for (int k = 0; k < 4; k++) h = (h ^ ((w >> (8 * k)) & 0xff)) * 0x100000001b3ull;
            }
            streams->push_back(words.empty() ? 0 : h);
        }
        unsigned fence = 0;
        const int nb = hakios_metal_fake_last_submission(d, &fence, bos.data(), unsigned(bos.size()));
        for (unsigned i = 0; i < n; i++) sbase[i] = ssize[i] = 0;
        for (int i = 0; i < nb && i < int(bos.size()); i++) {
            if (bos[i].surface >= 1 && bos[i].surface <= n) {
                sbase[bos[i].surface - 1] = bos[i].gpu_address;
                ssize[bos[i].surface - 1] = bos[i].size;
            }
        }
        const int nw = hakios_metal_fake_writes(d, writes.data(), unsigned(writes.size()), 1);
        for (unsigned t = 0; t < n; t++) {
            if (!written[t]) continue;
            DamageVerdict v;
            v.frame = unsigned(f);
            v.target = t;
            v.W = c.sizes[t].first;
            v.H = c.sizes[t].second;
            v.damageWhole = whole[t] || !any[t];
            v.damage = v.damageWhole ? Rect{0, 0, v.W, v.H} : dmg[t];
            for (int i = 0; i < nw && i < int(writes.size()); i++) {
                const hakios_metal_fake_write& w = writes[i];
                if (w.submission != fence || w.fill || ssize[t] == 0 || w.base < sbase[t] || w.base >= sbase[t] + ssize[t]) continue;
                v.copies++;
                v.copied = Rect{w.x0, w.y0, w.x1 - w.x0 + 1, w.y1 - w.y0 + 1};
                const bool within = w.x0 >= v.damage.x && w.y0 >= v.damage.y && w.x1 < v.damage.x + v.damage.w && w.y1 < v.damage.y + v.damage.h;
                v.copyWithinDamage = v.copyWithinDamage && within;
            }
            // the whole-copy model: the surface holds the target's bytes
            const unsigned RB = rows64(v.W * 4), rowLen = v.W * 4;
            std::vector<uint8_t> target(size_t(rowLen) * v.H), surface(size_t(hakios_metal_fake_surface_bytes(d, 1 + t, nullptr, 0)));
            hakios_metal_texture_read(tex[t], 0, 0, v.W, v.H, target.data(), rowLen);
            hakios_metal_fake_surface_bytes(d, 1 + t, surface.data(), surface.size());
            for (unsigned y = 0; y < v.H; y++) {
                if (size_t(y) * RB + rowLen > surface.size() || memcmp(surface.data() + size_t(y) * RB, target.data() + size_t(y) * rowLen, rowLen) != 0)
                    v.rowsWrong++;
            }
            out.push_back(v);
        }
        hakios_metal_cb_free(cb);
    }
    if (q != nullptr) hakios_metal_queue_free(q);
    if (src != nullptr) hakios_metal_texture_free(src);
    for (hakios_metal_texture* t : tex) hakios_metal_texture_free(t);
    hakios_metal_close(d);
    return out;
}
// tri_flat linked for BGRA8Unorm, nullptr without the compiler (or its bitcode)
hakios_metal_program* damageProgram() {
    if (!hakios_metal_has_compiler()) return nullptr;
    auto read = [](const char* path) {
        std::vector<uint8_t> b;
        if (FILE* f = fopen(path, "rb")) {
            uint8_t chunk[4096];
            for (size_t k; (k = fread(chunk, 1, sizeof chunk, f)) > 0;) b.insert(b.end(), chunk, chunk + k);
            fclose(f);
        }
        return b;
    };
    std::vector<uint8_t> vs = read("out/host/kernels/tri_flat_vs.bc"), fs = read("out/host/kernels/tri_flat_fs.bc");
    if (vs.empty() || fs.empty()) return nullptr;
    return hakios_metal_program_new_bitcode(vs.data(), vs.size(), fs.data(), fs.size(), kBGRA8Unorm);
}
DamageOp clearDamage(unsigned t) {
    DamageOp o;
    o.clear = true;
    o.target = t;
    o.px[0] = 0x20, o.px[1] = 0x40, o.px[2] = 0x60, o.px[3] = 0xff;
    return o;
}
}  // namespace

// The scoped case: the phone's 1125x2436 target, frame 1 a Clear, frame 2 a Load with a 200x200 damage: a copy of a
// 200x200 rectangle into it at (400, 1000) and, with the compiler, a draw scissored to 200x200 at (400, 1000).
// **Validates: Requirements 1.4, 2.4**
TEST(surface_target_damage_property_1_scoped_200x200_on_the_display_target) {
    hakios_metal_program* p = damageProgram();
    for (int withDraw = 0; withDraw < (p != nullptr ? 2 : 1); withDraw++) {
        DamageCase c;
        c.sizes = {{1125, 2436}};
        c.seed = 0xda3a6e50u + unsigned(withDraw);
        DamageOp op;
        op.draw = withDraw != 0;
        op.r = Rect{400, 1000, 200, 200};
        c.frames = {{clearDamage(0)}, {op}};
        std::string why;
        std::vector<DamageVerdict> v = runDamage(c, p, &why);
        if (!why.empty()) std::fprintf(stderr, "  scoped (%s): %s\n", withDraw ? "draw" : "copy", why.c_str());
        CHECK(why.empty());
        CHECK_EQ(v.size(), size_t(2));
        for (const DamageVerdict& x : v) {
            if (!x.ok()) std::fprintf(stderr, "  counterexample (scoped, a %s): %s\n", withDraw ? "scissored draw" : "copy", damageText(x).c_str());
            CHECK(x.ok());
        }
    }
    if (p == nullptr) std::printf("       (no compiler in this build: the scoped damage ran with a copy, not a draw)\n");
    if (p != nullptr) hakios_metal_program_free(p);
}

// Generated: 1-4 frames of one command buffer each, Clear or Load, 0-3 damage rectangles (scissored draws with the
// compiler, copies from a small texture) into 1-2 targets of 64x64 to 1125x2436.
// **Validates: Requirements 1.4, 2.4**
TEST(surface_target_damage_property_1_generated_only_the_damage_copied) {
    hakios_metal_program* p = damageProgram();
    std::mt19937 rng(0xd4a6a650u);
    auto pick = [&](unsigned k) { return k == 0 ? 0u : unsigned(rng() % k); };
    int checked = 0, bugCases = 0, failed = 0, notWithin = 0, wrongBytes = 0, printed = 0;
    for (int round = 0; round < 30; round++) {
        DamageCase c;
        const unsigned targets = 1 + pick(2);
        for (unsigned t = 0; t < targets; t++) {
            if (pick(4) == 0) {
                c.sizes.push_back({1125, 2436});
            } else {
                c.sizes.push_back({64 + pick(1125 - 64 + 1), 64 + pick(pick(2) ? 400 : 2436 - 64 + 1)});
            }
        }
        c.seed = uint32_t(rng());
        const unsigned frames = 1 + pick(4);
        for (unsigned f = 0; f < frames; f++) {
            std::vector<DamageOp> ops;
            if (pick(3) == 0) ops.push_back(clearDamage(pick(targets)));
            const unsigned k = pick(4);
            for (unsigned i = 0; i < k; i++) {
                DamageOp o;
                o.target = pick(targets);
                const unsigned W = c.sizes[o.target].first, H = c.sizes[o.target].second;
                o.draw = p != nullptr && pick(2) == 0;
                const unsigned w = 1 + pick(std::min(200u, W)), h = 1 + pick(std::min(200u, H));
                o.r = Rect{pick(W - w + 1), pick(H - h + 1), w, h};
                o.sx = pick(kSourceSide - w + 1);
                o.sy = pick(kSourceSide - h + 1);
                ops.push_back(o);
            }
            c.frames.push_back(ops);
        }
        std::string why;
        std::vector<DamageVerdict> v = runDamage(c, p, &why);
        if (!why.empty()) {
            if (printed++ < 5) std::fprintf(stderr, "  round %d: %s\n", round, why.c_str());
            CHECK(why.empty());
        }
        for (const DamageVerdict& x : v) {
            checked++;
            const bool bug = !x.damageWhole && uint64_t(x.damage.w) * x.damage.h < uint64_t(x.W) * x.H;
            bugCases += bug;
            if (x.ok()) continue;
            failed++;
            notWithin += !x.copyWithinDamage || x.copies == 0;
            wrongBytes += x.rowsWrong != 0;
            if (printed++ < 5) std::fprintf(stderr, "  counterexample (round %d): %s\n", round, damageText(x).c_str());
        }
    }
    std::fprintf(stderr,
                 "  damage property 1: %d targets written by a command buffer, %d with a damage smaller than the target, %d failed: %d with a copy "
                 "not within the aligned damage (or none), %d with surface rows not the target's\n",
                 checked, bugCases, failed, notWithin, wrongBytes);
    CHECK(bugCases > 20);   // the generator reaches the bug condition
    CHECK_EQ(failed, 0);
    if (p != nullptr) hakios_metal_program_free(p);
}

// --- metal-ca-perf, Property 2 (Preservation): the commit copy outside the damage bug condition ------------------------
// Observed on the code before the fix (CommandBuffer::damageOf reports nothing, the copy is whole) and written down here,
// to hold after it (task 5.6):
// - where every target a command buffer writes has a damage that is the whole target (a Clear, a pass that loads with
//   DontCare, a draw over the whole target), the copy into its surface is one whole-target copy and the command stream
//   is word for word the one observed: the FNV-1a 64 of every command buffer's words, combined per generated round,
//   golden below (a table without draws, which both builds run, and one with draws, which needs the compiler);
// - for every generated workload, whole damage or not, the surface's bytes after each command buffer are the target's
//   (the whole-copy model: the stale-frame fix's invariant).
// metal_blit=0 and the surfaces that do not qualify are the earlier Property 2 above (surface_target_property_2_*).
// Damage off (metal_damage=0) has no switch before task 5.3; its stream is the whole-damage one.
namespace {
// a round of whole-damage frames: 1-2 targets, 1-3 frames, every target written in a frame first wholly damaged (a Clear;
// with DRAWS also a DontCare draw or a draw over the whole target), then 0-3 rectangles (copies; with DRAWS also draws)
DamageCase wholeDamageRound(std::mt19937& rng, bool draws) {
    auto pick = [&](unsigned k) { return k == 0 ? 0u : unsigned(rng() % k); };
    DamageCase c;
    const unsigned targets = 1 + pick(2);
    for (unsigned t = 0; t < targets; t++) {
        if (pick(4) == 0) {
            c.sizes.push_back({1125, 2436});
        } else {
            c.sizes.push_back({64 + pick(1125 - 64 + 1), 64 + pick(pick(2) ? 400 : 2436 - 64 + 1)});
        }
    }
    c.seed = uint32_t(rng());
    const unsigned frames = 1 + pick(3);
    for (unsigned f = 0; f < frames; f++) {
        std::vector<DamageOp> ops;
        std::vector<unsigned> written;
        for (unsigned t = 0; t < targets; t++)
            if (t == 0 || pick(2) == 0) written.push_back(t);
        for (unsigned t : written) {
            const unsigned W = c.sizes[t].first, H = c.sizes[t].second;
            const unsigned how = draws ? pick(3) : 0;
            if (how == 0) {
                DamageOp o = clearDamage(t);
                o.px[0] = uint8_t(rng()), o.px[1] = uint8_t(rng()), o.px[2] = uint8_t(rng()), o.px[3] = uint8_t(rng());
                ops.push_back(o);
            } else {
                DamageOp o;
                o.draw = true;
                o.target = t;
                o.dontCare = how == 1;
                o.r = how == 1 ? Rect{pick(W / 2), pick(H / 2), 1 + pick(W / 2), 1 + pick(H / 2)} : Rect{0, 0, W, H};
                ops.push_back(o);
            }
        }
        const unsigned k = pick(4);
        for (unsigned i = 0; i < k; i++) {
            DamageOp o;
            o.target = written[pick(unsigned(written.size()))];
            const unsigned W = c.sizes[o.target].first, H = c.sizes[o.target].second;
            o.draw = draws && pick(2) == 0;
            const unsigned w = 1 + pick(std::min(200u, W)), h = 1 + pick(std::min(200u, H));
            o.r = Rect{pick(W - w + 1), pick(H - h + 1), w, h};
            o.sx = pick(kSourceSide - w + 1);
            o.sy = pick(kSourceSide - h + 1);
            ops.push_back(o);
        }
        c.frames.push_back(ops);
    }
    return c;
}
uint64_t combineStreams(const std::vector<uint64_t>& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t v : s) {
        for (int k = 0; k < 8; k++) h = (h ^ ((v >> (8 * k)) & 0xff)) * 0x100000001b3ull;
    }
    return h;
}
// the rounds' stream hashes observed on the code before the fix
const std::vector<uint64_t> kWholeDamageStreams = {
    0x0c8d24d107069f50ull, 0xf5a3d423c664ab77ull, 0x3605b3e7ecf62427ull, 0x2c4f8270e973230dull, 0x0f1ba9e7fa118ea2ull,
    0x6955f2bb8ea8e003ull, 0x93d564455a37a5daull, 0x7366fbedd963cc1full, 0x01a786051aa856c3ull, 0x03ac46feafa5762aull,
    0x02a6954f1691e024ull, 0x2b8f23752aef0690ull, 0xbf36c890c6c7c405ull, 0xabdac13ec932cb06ull,
};
const std::vector<uint64_t> kWholeDamageStreamsWithDraws = {
    0x9e075d5166ff0b44ull, 0x72eb79fd550afb4aull, 0x94b117a249af0416ull, 0x502d511941365b20ull, 0xa3562a1f99bfd1a8ull,
    0xebd2886d1ca17963ull, 0x6036d794b6f2c147ull, 0x5679d14a97451788ull, 0x79ace34b0e117e64ull, 0xc19c9b56bd2ecbe2ull,
};
}  // namespace

// Whole damage: one whole copy, the command stream word for word the one observed. **Validates: Requirements 3.4, 3.5, 3.7**
TEST(surface_target_damage_property_2_whole_damage_stream_word_for_word) {
    hakios_metal_program* p = damageProgram();
    for (int withDraws = 0; withDraws < 2; withDraws++) {
        if (withDraws && p == nullptr) {
            std::printf("       (no compiler in this build: the whole-damage rounds with draws did not run)\n");
            break;
        }
        const std::vector<uint64_t>& golden = withDraws ? kWholeDamageStreamsWithDraws : kWholeDamageStreams;
        std::mt19937 rng(withDraws ? 0x3a01d2a1u : 0x3a01d2a0u);
        const int rounds = withDraws ? 10 : 14;
        int checked = 0, notWhole = 0, wrongBytes = 0, unstable = 0, streamWrong = 0, printed = 0;
        std::string observed;
        for (int round = 0; round < rounds; round++) {
            const DamageCase c = wholeDamageRound(rng, withDraws != 0);
            std::string why, why2;
            std::vector<uint64_t> s1, s2;
            const std::vector<DamageVerdict> v = runDamage(c, p, &why, &s1);
            runDamage(c, p, &why2, &s2);
            if (!why.empty() && printed++ < 5) std::fprintf(stderr, "  round %d: %s\n", round, why.c_str());
            CHECK(why.empty());
            CHECK_EQ(s1.size(), c.frames.size());
            if (s1 != s2) unstable++;
            for (const DamageVerdict& x : v) {
                checked++;
                const bool whole = x.copies == 1 && x.copied.x == 0 && x.copied.y == 0 && x.copied.w == x.W && x.copied.h == x.H;
                notWhole += !whole;
                wrongBytes += x.rowsWrong != 0;
                if ((!whole || x.rowsWrong != 0) && printed++ < 5) std::fprintf(stderr, "  counterexample (round %d): %s\n", round, damageText(x).c_str());
            }
            const uint64_t h = combineStreams(s1);
            char t[32];
            snprintf(t, sizeof(t), "0x%016llxull, ", (unsigned long long)h);
            observed += t;
            if (size_t(round) < golden.size() && golden[size_t(round)] != h) {
                streamWrong++;
                if (printed++ < 5)
                    std::fprintf(stderr, "  counterexample (round %d%s): command stream 0x%016llx, observed before the fix 0x%016llx\n", round,
                                 withDraws ? ", with draws" : "", (unsigned long long)h, (unsigned long long)golden[size_t(round)]);
            }
        }
        std::fprintf(stderr, "  damage property 2 (whole damage%s): %d targets written, %d not one whole copy, %d with surface rows not the target's, "
                             "%d rounds not repeatable, %d of %zu golden streams differ\n    observed streams: %s\n",
                     withDraws ? ", with draws" : "", checked, notWhole, wrongBytes, unstable, streamWrong, golden.size(), observed.c_str());
        CHECK(checked > 15);
        CHECK_EQ(notWhole, 0);
        CHECK_EQ(wrongBytes, 0);
        CHECK_EQ(unstable, 0);
        CHECK_EQ(golden.size(), size_t(rounds));
        CHECK_EQ(streamWrong, 0);
    }
    if (p != nullptr) hakios_metal_program_free(p);
}

// Every workload, whole damage or not: after each command buffer the surface is the target (the whole-copy model).
// **Validates: Requirements 3.4, 3.5**
TEST(surface_target_damage_property_2_surface_equals_target_everywhere) {
    hakios_metal_program* p = damageProgram();
    std::mt19937 rng(0x5afe0a65u);
    auto pick = [&](unsigned k) { return k == 0 ? 0u : unsigned(rng() % k); };
    int checked = 0, partial = 0, wrongBytes = 0, noCopy = 0, printed = 0;
    for (int round = 0; round < 24; round++) {
        DamageCase c;
        if (pick(2) == 0) {
            c = wholeDamageRound(rng, p != nullptr);
        } else {
            const unsigned targets = 1 + pick(2);
            for (unsigned t = 0; t < targets; t++) c.sizes.push_back({64 + pick(1125 - 64 + 1), 64 + pick(pick(2) ? 400 : 2436 - 64 + 1)});
            c.seed = uint32_t(rng());
            const unsigned frames = 1 + pick(4);
            for (unsigned f = 0; f < frames; f++) {
                std::vector<DamageOp> ops;
                if (pick(3) == 0) ops.push_back(clearDamage(pick(targets)));
                const unsigned k = pick(4);
                for (unsigned i = 0; i < k; i++) {
                    DamageOp o;
                    o.target = pick(targets);
                    const unsigned W = c.sizes[o.target].first, H = c.sizes[o.target].second;
                    o.draw = p != nullptr && pick(2) == 0;
                    o.dontCare = o.draw && pick(4) == 0;
                    const unsigned w = 1 + pick(std::min(200u, W)), h = 1 + pick(std::min(200u, H));
                    o.r = Rect{pick(W - w + 1), pick(H - h + 1), w, h};
                    o.sx = pick(kSourceSide - w + 1);
                    o.sy = pick(kSourceSide - h + 1);
                    ops.push_back(o);
                }
                c.frames.push_back(ops);
            }
        }
        std::string why;
        const std::vector<DamageVerdict> v = runDamage(c, p, &why);
        if (!why.empty() && printed++ < 5) std::fprintf(stderr, "  round %d: %s\n", round, why.c_str());
        CHECK(why.empty());
        for (const DamageVerdict& x : v) {
            checked++;
            partial += !x.damageWhole && uint64_t(x.damage.w) * x.damage.h < uint64_t(x.W) * x.H;
            wrongBytes += x.rowsWrong != 0;
            noCopy += x.copies == 0;
            if ((x.rowsWrong != 0 || x.copies == 0) && printed++ < 5) std::fprintf(stderr, "  counterexample (round %d): %s\n", round, damageText(x).c_str());
        }
    }
    std::fprintf(stderr, "  damage property 2 (surface = target): %d targets written (%d with a damage smaller than the target), %d without a copy, "
                         "%d with surface rows not the target's\n",
                 checked, partial, noCopy, wrongBytes);
    CHECK(checked > 30);
    CHECK_EQ(noCopy, 0);
    CHECK_EQ(wrongBytes, 0);
    if (p != nullptr) hakios_metal_program_free(p);
}

// --- metal-ca-perf task 5.3: metal_damage=0 (hakios_metal_surface_target_set_damage 0) keeps the stalefix run's copy -----
// Workloads whose damage is smaller than the target (the bug condition's), run with the damage copy off: every target
// written is copied whole, once, the surface is the target, and the command stream is word for word the one observed on
// the code before the fix (golden below, observed there: the damage-off switch then changed nothing).
namespace {
DamageCase partialDamageRound(std::mt19937& rng, bool draws) {
    auto pick = [&](unsigned k) { return k == 0 ? 0u : unsigned(rng() % k); };
    DamageCase c;
    const unsigned targets = 1 + pick(2);
    for (unsigned t = 0; t < targets; t++) {
        if (pick(4) == 0) {
            c.sizes.push_back({1125, 2436});
        } else {
            c.sizes.push_back({64 + pick(1125 - 64 + 1), 64 + pick(pick(2) ? 400 : 2436 - 64 + 1)});
        }
    }
    c.seed = uint32_t(rng());
    const unsigned frames = 1 + pick(3);
    for (unsigned f = 0; f < frames; f++) {
        std::vector<DamageOp> ops;
        if (f == 0 || pick(4) == 0) ops.push_back(clearDamage(pick(targets)));
        const unsigned k = 1 + pick(3);
        for (unsigned i = 0; i < k; i++) {
            DamageOp o;
            o.target = pick(targets);
            const unsigned W = c.sizes[o.target].first, H = c.sizes[o.target].second;
            o.draw = draws && pick(2) == 0;
            const unsigned w = 1 + pick(std::min(200u, W)), h = 1 + pick(std::min(200u, H));
            o.r = Rect{pick(W - w + 1), pick(H - h + 1), w, h};
            o.sx = pick(kSourceSide - w + 1);
            o.sy = pick(kSourceSide - h + 1);
            ops.push_back(o);
        }
        c.frames.push_back(ops);
    }
    return c;
}
const std::vector<uint64_t> kDamageOffStreams = {
    0xb4a9bb893cc4b46bull, 0x30d5be6d69b6de8full, 0x50a7f600ed306c29ull, 0xdf0758d12d50ea9full,
    0x900205034bf24d46ull, 0x5ba6dbe81ee9cab2ull, 0x51c92376832bfae8ull, 0xb9a4ad765a616d00ull,
};
const std::vector<uint64_t> kDamageOffStreamsWithDraws = {
    0x64670adad3596980ull, 0x70f2b722987a50afull, 0x466705c80de35688ull, 0xce0e8d7320bcd6c1ull,
    0xedee9fe88befa26full, 0xcb9dcac93f276b1full, 0x962a45f0fdf3ca3eull, 0xede629c34dfbb037ull,
};
}  // namespace

// **Validates: Requirements 3.7, 3.4**
TEST(surface_target_damage_off_whole_copy_stream_word_for_word) {
    hakios_metal_program* p = damageProgram();
    for (int withDraws = 0; withDraws < 2; withDraws++) {
        if (withDraws && p == nullptr) {
            std::printf("       (no compiler in this build: the damage-off rounds with draws did not run)\n");
            break;
        }
        const std::vector<uint64_t>& golden = withDraws ? kDamageOffStreamsWithDraws : kDamageOffStreams;
        std::mt19937 rng(withDraws ? 0x0ffda7a1u : 0x0ffda7a0u);
        const int rounds = 8;
        int checked = 0, partial = 0, notWhole = 0, wrongBytes = 0, streamWrong = 0, printed = 0;
        std::string observed;
        for (int round = 0; round < rounds; round++) {
            const DamageCase c = partialDamageRound(rng, withDraws != 0);
            std::string why;
            std::vector<uint64_t> s;
            const std::vector<DamageVerdict> v = runDamage(c, p, &why, &s, true);
            if (!why.empty() && printed++ < 5) std::fprintf(stderr, "  round %d: %s\n", round, why.c_str());
            CHECK(why.empty());
            for (const DamageVerdict& x : v) {
                checked++;
                partial += !x.damageWhole && uint64_t(x.damage.w) * x.damage.h < uint64_t(x.W) * x.H;
                const bool whole = x.copies == 1 && x.copied.x == 0 && x.copied.y == 0 && x.copied.w == x.W && x.copied.h == x.H;
                notWhole += !whole;
                wrongBytes += x.rowsWrong != 0;
                if ((!whole || x.rowsWrong != 0) && printed++ < 5) std::fprintf(stderr, "  counterexample (round %d): %s\n", round, damageText(x).c_str());
            }
            const uint64_t h = combineStreams(s);
            char t[32];
            snprintf(t, sizeof(t), "0x%016llxull, ", (unsigned long long)h);
            observed += t;
            if (size_t(round) < golden.size() && golden[size_t(round)] != h) {
                streamWrong++;
                if (printed++ < 5)
                    std::fprintf(stderr, "  counterexample (round %d%s): command stream 0x%016llx, observed before the fix 0x%016llx\n", round,
                                 withDraws ? ", with draws" : "", (unsigned long long)h, (unsigned long long)golden[size_t(round)]);
            }
        }
        std::fprintf(stderr, "  damage off%s: %d targets written (%d with a damage smaller than the target), %d not one whole copy, %d with surface "
                             "rows not the target's, %d of %zu golden streams differ\n    observed streams: %s\n",
                     withDraws ? ", with draws" : "", checked, partial, notWhole, wrongBytes, streamWrong, golden.size(), observed.c_str());
        CHECK(partial > 4);
        CHECK_EQ(notWhole, 0);
        CHECK_EQ(wrongBytes, 0);
        CHECK_EQ(golden.size(), size_t(rounds));
        CHECK_EQ(streamWrong, 0);
    }
    if (p != nullptr) hakios_metal_program_free(p);
}
