// hm-seed's work (tools/hm-seed.cpp; metal-ca-perf, design 1.5): the compiled-stage seed for the A650 built on the host
// from a phone run's log, header only so that the tests (tests/hm_seed_test.cpp) run the same code as the tool.
//
// The phone's plug-in (userland/src/HakiosMetalCA.m, hm_perf_stage and hm_perf_start, under metal_fps=1) writes
//   HakiosMetal[backboardd 14]: stage cache: build B, layout 1448/848, RAM cache ... | off (metal_cache=0)
//   HakiosMetal[backboardd 14]: stage miss: fs #3 key /<32 hex>.stage words 96 digest <16 hex> in 1628.0 ms; bitcode
//       sha256 <64 hex> (N bytes); constants 3=0x1 5=0 | none; ibo 4; known none | binding 0 bytes <hex, 256 bytes at most>
// a miss longer than 900 characters cut into pieces: the first ends in " ...", the rest are
//   HakiosMetal[backboardd 14]: stage miss: #3 continued: <the next 900 characters>[ ...]
// cut anywhere, a hex number included, so the pieces are joined as they are.
//
// For each miss: the bitcode found by its SHA-256 among what the host has (QuartzCore's default.pipelinelib modules,
// the merged QuartzCore library's functions, the driver's fixture kernels), compiled with compileStageForChip for
// the A650 with the logged constants, IBO base and known buffer into a scratch writable cache, and kept -- moved into the
// seed directory under its cache name -- only when the digest and the word count are the phone's (and the cache name
// too, when the phone had one: with metal_cache=0 it logs "-"). The binary must have been compiled with the phone's
// HAKIOS_METAL_CACHE_BUILD (the build of the "stage cache:" line), or the names it files are names the phone never asks
// for: checkBuild refuses a log of another build or layout.
//
// Nothing here writes outside the directories it is given; nothing is sent anywhere.
#pragma once

#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/compiler.hpp"
#include "hakios-metal/metallib.hpp"

namespace hakios::metal::seed {

// --- SHA-256 (FIPS 180-4), for the bitcode's identity as the plug-in's CC_SHA256 gives it -----------------------------
inline std::string sha256Hex(const uint8_t* data, size_t size) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    auto block = [&](const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    };
    size_t at = 0;
    for (; at + 64 <= size; at += 64) block(data + at);
    uint8_t tail[128] = {};
    const size_t rest = size - at;
    if (rest > 0) memcpy(tail, data + at, rest);
    tail[rest] = 0x80;
    const size_t tailSize = rest < 56 ? 64 : 128;
    const uint64_t bits = uint64_t(size) * 8;
    for (int i = 0; i < 8; i++) tail[tailSize - 1 - i] = uint8_t(bits >> (8 * i));
    block(tail);
    if (tailSize == 128) block(tail + 64);
    char out[65];
    for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", h[i]);
    return std::string(out, 64);
}
inline std::string sha256Hex(const std::vector<uint8_t>& v) { return sha256Hex(v.data(), v.size()); }

// --- the log ----------------------------------------------------------------------------------------------------------
// one "stage cache:" line
struct CacheLine {
    std::string who;      // "HakiosMetal[backboardd 14]", "" when the line has no such prefix
    std::string build;    // the compiler build (the part of hakios_metal_cache_build() before the layout)
    std::string layout;   // "STAGE/KERNEL"
};
// one stage compiled on the phone (a "stage miss:" line and its continuations, joined)
struct Miss {
    std::string who;
    unsigned number = 0;   // #N, per process
    bool fragment = false;
    std::string key;       // the phone's cache name ("/<32 hex>.stage"), "-" when it had none (metal_cache=0)
    uint32_t words = 0;
    uint64_t digest = 0;
    std::string sha256;    // 64 lower-case hex
    size_t size = 0;       // the bitcode's bytes
    FunctionConstants constants;
    uint32_t iboBase = 0;
    bool hasKnown = false;
    KnownBuffer known;
};
struct Log {
    std::vector<CacheLine> caches;
    std::vector<Miss> misses;
    unsigned unreadable = 0;   // a miss whose joined text does not parse (a cut line, a piece lost)
    unsigned unfinished = 0;   // a miss whose continuation never came
    unsigned duplicates = 0;   // a miss line seen again (overlapping log snapshots)
    std::vector<std::string> notes;
};

namespace detail {
// a cursor over one miss's text: literals expected in order, numbers and words between them
struct Cursor {
    const std::string& s;
    size_t p = 0;
    bool ok = true;
    bool lit(const char* l) {
        const size_t n = strlen(l);
        if (!ok || s.compare(p, n, l) != 0) return ok = false;
        p += n;
        return true;
    }
    std::string word() {   // up to a space or ';' (at least one character)
        const size_t b = p;
        while (p < s.size() && s[p] != ' ' && s[p] != ';') p++;
        if (p == b) ok = false;
        return s.substr(b, p - b);
    }
    bool num(const std::string& w, int base, uint64_t* v) {
        if (!ok || w.empty()) return ok = false;
        char* end = nullptr;
        errno = 0;
        const unsigned long long x = strtoull(w.c_str(), &end, base);
        if (errno != 0 || end == nullptr || *end != 0 || w[0] == '-') return ok = false;
        *v = x;
        return true;
    }
    uint64_t number(int base) {
        uint64_t v = 0;
        num(word(), base, &v);
        return v;
    }
};
inline bool isHex(const std::string& h, size_t n) {
    if (n != 0 && h.size() != n) return false;
    for (char c : h)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
// the whole text of one miss (after "stage miss: "), as the plug-in's snprintf made it
inline bool parseMissBody(const std::string& body, Miss* m) {
    Cursor c{body};
    const std::string kind = c.word();
    if (kind != "vs" && kind != "fs") return false;
    m->fragment = kind == "fs";
    c.lit(" #");
    m->number = unsigned(c.number(10));
    c.lit(" key ");
    m->key = c.word();
    c.lit(" words ");
    m->words = uint32_t(c.number(10));
    c.lit(" digest ");
    const std::string d = c.word();
    if (!isHex(d, 16)) return false;
    c.num(d, 16, &m->digest);
    c.lit(" in ");
    c.word();   // the time
    c.lit(" ms; bitcode sha256 ");
    m->sha256 = c.word();
    if (!isHex(m->sha256, 64)) return false;
    c.lit(" (");
    m->size = size_t(c.number(10));
    c.lit(" bytes); constants");
    if (c.ok && body.compare(c.p, 5, " none") == 0 && (c.p + 5 == body.size() || body[c.p + 5] == ';')) {
        c.p += 5;
    } else {
        while (c.ok && c.p < body.size() && body[c.p] == ' ') {
            c.p++;
            const std::string w = c.word();
            const size_t eq = w.find('=');
            uint64_t index = 0, bits = 0;
            if (eq == std::string::npos || !c.num(w.substr(0, eq), 10, &index) || !c.num(w.substr(eq + 1), 0, &bits)) return false;
            m->constants.push_back({uint32_t(index), bits});
        }
    }
    c.lit("; ibo ");
    m->iboBase = uint32_t(c.number(10));
    c.lit("; known ");
    if (c.ok && body.compare(c.p, std::string::npos, "none") == 0) {
        m->hasKnown = false;
        c.p = body.size();
    } else {
        c.lit("binding ");
        m->hasKnown = true;
        m->known.binding = uint32_t(c.number(10));
        c.lit(" bytes");
        if (c.ok && c.p < body.size() && body[c.p] == ' ') c.p++;
        const std::string hex = c.ok && c.p <= body.size() ? body.substr(c.p) : std::string();
        if (!c.ok || hex.size() % 2 != 0 || !isHex(hex, 0)) return false;
        for (size_t i = 0; i < hex.size(); i += 2) m->known.bytes.push_back(uint8_t(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
        c.p = body.size();
    }
    return c.ok && c.p == body.size();
}
// "HakiosMetal[...]" before position AT of LINE, "" when there is none
inline std::string whoBefore(const std::string& line, size_t at) {
    const size_t b = line.rfind("HakiosMetal[", at);
    if (b == std::string::npos) return {};
    const size_t e = line.find(']', b);
    if (e == std::string::npos || e > at) return {};
    return line.substr(b, e + 1 - b);
}
}  // namespace detail

inline Log parseLog(const std::string& text) {
    Log log;
    std::map<std::pair<std::string, unsigned>, std::string> pending;   // (who, #N) -> the text so far
    std::set<std::pair<std::string, unsigned>> done;
    std::set<std::string> cacheSeen;
    static const char kMore[] = " ...";
    auto finish = [&](const std::string& who, const std::string& body) {
        Miss m;
        m.who = who;
        if (detail::parseMissBody(body, &m)) {
            log.misses.push_back(std::move(m));
        } else {
            log.unreadable++;
            if (log.notes.size() < 20) log.notes.push_back("unreadable miss: " + body.substr(0, 160));
        }
    };
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        size_t p = line.find("stage cache: build ");
        if (p != std::string::npos) {
            const std::string rest = line.substr(p + strlen("stage cache: build "));
            const size_t lay = rest.find(", layout ");
            if (lay == std::string::npos) continue;
            CacheLine c;
            c.who = detail::whoBefore(line, p);
            c.build = rest.substr(0, lay);
            const std::string after = rest.substr(lay + strlen(", layout "));
            c.layout = after.substr(0, after.find(','));
            if (cacheSeen.insert(c.who + "\n" + c.build + "\n" + c.layout).second) log.caches.push_back(c);
            continue;
        }
        p = line.find("stage miss: ");
        if (p == std::string::npos) continue;
        const std::string who = detail::whoBefore(line, p);
        std::string body = line.substr(p + strlen("stage miss: "));
        const bool more = body.size() >= 4 && body.compare(body.size() - 4, 4, kMore) == 0;
        if (more) body.resize(body.size() - 4);
        if (body.size() > 1 && body[0] == '#') {
            // a continuation: "#N continued: piece"
            const size_t c = body.find(" continued: ");
            if (c == std::string::npos) continue;
            const unsigned n = unsigned(strtoul(body.c_str() + 1, nullptr, 10));
            const auto k = std::make_pair(who, n);
            auto it = pending.find(k);
            if (it == pending.end()) {
                if (done.count(k) == 0) log.unreadable++;   // a piece of a miss whose start is not in the log
                continue;
            }
            it->second += body.substr(c + strlen(" continued: "));
            if (!more) {
                finish(who, it->second);
                pending.erase(it);
                done.insert(k);
            }
            continue;
        }
        // a first piece: "vs #N ..." or "fs #N ..."
        const size_t hash = body.find(" #");
        const unsigned n = hash != std::string::npos ? unsigned(strtoul(body.c_str() + hash + 2, nullptr, 10)) : 0;
        const auto k = std::make_pair(who, n);
        if (done.count(k) != 0 || pending.count(k) != 0) {
            log.duplicates++;
            continue;
        }
        if (more) {
            pending[k] = body;
        } else {
            finish(who, body);
            done.insert(k);
        }
    }
    log.unfinished = unsigned(pending.size());
    return log;
}

// HOST_BUILD: this binary's stageCacheBuild() ("BUILD STAGE/KERNEL"). True when the log has a "stage cache:" line and
// every one of them has this build and layout; false with *why otherwise.
inline bool checkBuild(const Log& log, const std::string& hostBuild, std::string* why) {
    const size_t sp = hostBuild.rfind(' ');
    const std::string build = sp == std::string::npos ? hostBuild : hostBuild.substr(0, sp);
    const std::string layout = sp == std::string::npos ? std::string() : hostBuild.substr(sp + 1);
    if (build.empty()) {
        *why = "this hm-seed has no compiler (built without MESA=1)";
        return false;
    }
    if (log.caches.empty()) {
        *why = "the log has no \"stage cache:\" line (metal_fps=1 and a plug-in with metal-ca-perf's measurement): the phone's build is unknown";
        return false;
    }
    for (const CacheLine& c : log.caches) {
        if (c.layout != layout) {
            *why = "the phone's layout " + c.layout + " (" + c.who + ") is not this host's " + layout;
            return false;
        }
        if (c.build != build) {
            *why = "the phone's compiler build " + c.build + " (" + c.who + ") is not this hm-seed's " + build +
                   " (make out/host/hm-seed CACHE_BUILD=<the phone's>)";
            return false;
        }
    }
    return true;
}

// The plug-in's miss text for a request (hm_perf_stage's snprintf, for the tests' synthetic logs), and its lines as
// hm_print writes them with WHO before each (900 characters a piece).
inline std::string missText(const Miss& m, double ms) {
    char b[256];
    std::string s;
    snprintf(b, sizeof b, "%s #%u key %s words %u digest %016llx in %.1f ms; bitcode sha256 ", m.fragment ? "fs" : "vs", m.number,
             m.key.empty() ? "-" : m.key.c_str(), m.words, (unsigned long long)m.digest, ms);
    s += b;
    s += m.sha256;
    snprintf(b, sizeof b, " (%zu bytes); constants", m.size);
    s += b;
    for (const FunctionConstant& c : m.constants) {
        snprintf(b, sizeof b, " %u=%#llx", c.index, (unsigned long long)c.bits);
        s += b;
    }
    if (m.constants.empty()) s += " none";
    snprintf(b, sizeof b, "; ibo %u; known ", m.iboBase);
    s += b;
    if (!m.hasKnown) {
        s += "none";
    } else {
        snprintf(b, sizeof b, "binding %u bytes ", m.known.binding);
        s += b;
        for (size_t i = 0; i < m.known.bytes.size() && i < 256; i++) {
            snprintf(b, sizeof b, "%02x", m.known.bytes[i]);
            s += b;
        }
    }
    return s;
}
inline std::string missLines(const std::string& who, const Miss& m, double ms) {
    const std::string t = missText(m, ms);
    std::string out;
    for (size_t at = 0, piece = 0; at < t.size(); piece++) {
        const size_t take = std::min(t.size() - at, size_t(900));
        out += who + ": stage miss: ";
        if (piece != 0) out += "#" + std::to_string(m.number) + " continued: ";
        out += t.substr(at, take);
        if (at + take < t.size()) out += " ...";
        out += "\n";
        at += take;
    }
    return out;
}

// --- the bitcode the host has, by SHA-256 -------------------------------------------------------------------------------
inline bool readFile(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    out->clear();
    uint8_t chunk[65536];
    for (size_t n; (n = fread(chunk, 1, sizeof chunk, f)) > 0;) out->insert(out->end(), chunk, chunk + n);
    fclose(f);
    return true;
}

class BitcodeIndex {
public:
    struct Entry {
        std::vector<uint8_t> bytes;
        std::string origin;
    };
    // true when the bytes were new
    bool add(const uint8_t* p, size_t n, const std::string& origin) {
        if (n == 0) return false;
        const std::string h = sha256Hex(p, n);
        if (byHash_.count(h) != 0) return false;
        byHash_[h] = Entry{std::vector<uint8_t>(p, p + n), origin};
        return true;
    }
    const Entry* find(const std::string& sha) const {
        auto it = byHash_.find(sha);
        return it == byHash_.end() ? nullptr : &it->second;
    }
    size_t size() const { return byHash_.size(); }

    // every *.bc of DIR (the driver's fixture kernels, out/host/kernels); the number of files read
    unsigned addBcDir(const std::string& dir) {
        unsigned n = 0;
        std::vector<std::string> names;
        if (DIR* d = opendir(dir.c_str())) {
            while (dirent* e = readdir(d)) {
                const size_t l = strlen(e->d_name);
                if (l > 3 && strcmp(e->d_name + l - 3, ".bc") == 0) names.push_back(e->d_name);
            }
            closedir(d);
        }
        for (const std::string& name : names) {
            std::vector<uint8_t> b;
            if (readFile(dir + "/" + name, &b)) {
                add(b.data(), b.size(), "fixture " + name);
                n++;
            }
        }
        return n;
    }
    // one file's whole bytes
    bool addFile(const std::string& path) {
        std::vector<uint8_t> b;
        if (!readFile(path, &b)) return false;
        add(b.data(), b.size(), path);
        return true;
    }
    // a .metallib's functions (metallib.hpp's reader): each module as it lies in the file (MDSZ bytes at its offset,
    // what MTLFunction's bitcodeData is taken to be) and without its bitcode wrapper; -1 when the file is not one
    int addMetallib(const std::string& path, std::string* why) {
        auto keep = std::make_shared<std::vector<uint8_t>>();
        if (!readFile(path, keep.get())) {
            *why = "cannot read " + path;
            return -1;
        }
        MetalLibrary lib;
        if (!lib.parse(keep->data(), keep->size())) {
            *why = path + ": " + lib.why();
            return -1;
        }
        const bool thin = keep->size() >= 4 && memcmp(keep->data(), "MTLB", 4) == 0;
        int n = 0;
        for (const MetalFunction& f : lib.functions()) {
            if (thin && f.moduleOffset <= keep->size() && f.moduleSize <= keep->size() - f.moduleOffset) {
                add(keep->data() + f.moduleOffset, size_t(f.moduleSize), "ca_metallib " + f.name);
            }
            const uint8_t* bc = nullptr;
            uint64_t len = 0;
            if (lib.bitcode(f, &bc, &len)) add(bc, size_t(len), "ca_metallib " + f.name + " (unwrapped)");
            n++;
        }
        return n;
    }
    // QuartzCore's default.pipelinelib (a directory with its "functions" file and, for the labels, "pipelines.desc"), cut
    // as the plug-in's hm_ca_load and hm_ca_module cut it: the functions file's single-function metallibs one after the
    // other, of each the bitcode at the +0x48 section plus the function's OFFT, MDSZ long. The modules found; -1 without
    // a functions file.
    int addPipelinelib(const std::string& dir, std::string* why) {
        std::vector<uint8_t> f, d;
        if (!readFile(dir + "/functions", &f)) {
            *why = "cannot read " + dir + "/functions";
            return -1;
        }
        std::vector<std::string> labels;
        if (readFile(dir + "/pipelines.desc", &d)) labels = descLabels(d);
        int n = 0;
        size_t at = 0;
        for (size_t i = 0;; i++) {
            if (!labels.empty() && i >= labels.size()) break;
            const uint8_t* m = at < f.size() ? static_cast<const uint8_t*>(memmem(f.data() + at, f.size() - at, "MTLB", 4)) : nullptr;
            if (m == nullptr || size_t(m - f.data()) + 0x18 > f.size()) break;
            uint64_t total;
            memcpy(&total, m + 0x10, 8);
            if (total > f.size() - size_t(m - f.data())) break;
            const uint8_t* bc = nullptr;
            size_t len = 0;
            if (moduleOf(m, size_t(total), &bc, &len)) {
                add(bc, len, "pipelinelib " + (i < labels.size() ? labels[i] : "module #" + std::to_string(i)));
                n++;
            }
            at = size_t(m - f.data()) + size_t(total);
        }
        return n;
    }

private:
    // hm_ca_module, as it is
    static bool moduleOf(const uint8_t* lib, size_t size, const uint8_t** bc, size_t* len) {
        if (size < 0x58 || memcmp(lib, "MTLB", 4) != 0) return false;
        uint64_t funcs, base;
        uint32_t entry;
        memcpy(&funcs, lib + 0x18, 8);
        memcpy(&base, lib + 0x48, 8);
        if (funcs + 8 > size) return false;
        memcpy(&entry, lib + funcs + 4, 4);
        for (uint64_t q = funcs + 8, end = funcs + 4 + entry, offt = ~0ull, mdsz = 0; q + 6 <= end && q + 6 <= size;) {
            uint16_t l;
            if (memcmp(lib + q, "ENDT", 4) == 0) break;
            memcpy(&l, lib + q + 4, 2);
            if (memcmp(lib + q, "OFFT", 4) == 0 && l == 24 && q + 6 + 24 <= size) {
                memcpy(&offt, lib + q + 6 + 16, 8);
            } else if (memcmp(lib + q, "MDSZ", 4) == 0 && l == 8 && q + 6 + 8 <= size) {
                memcpy(&mdsz, lib + q + 6, 8);
            }
            q += 6 + l;
            if (offt != ~0ull && mdsz != 0 && base + offt + mdsz <= size) {
                *bc = lib + base + offt;
                *len = size_t(mdsz);
                return true;
            }
        }
        return false;
    }
    // pipelines.desc section 1: the functions' labels in the order of the functions file (hm_ca_load)
    static std::vector<std::string> descLabels(const std::vector<uint8_t>& d) {
        std::vector<std::string> labels;
        uint32_t sections, off[16], n;
        if (d.size() < 8) return labels;
        memcpy(&sections, d.data(), 4);
        if (sections > 16 || 4 + 4 * size_t(sections) > d.size()) return labels;
        memcpy(off, d.data() + 4, 4 * sections);
        if (sections <= 7 || size_t(off[1]) + 4 > d.size()) return labels;
        memcpy(&n, d.data() + off[1], 4);
        for (uint32_t i = 0; i < n && size_t(off[1]) + 8 + 4 * size_t(i) <= d.size(); i++) {
            uint32_t r;
            memcpy(&r, d.data() + off[1] + 4 + 4 * i, 4);
            if (size_t(off[1]) + r + 16 < d.size()) {
                const char* label = reinterpret_cast<const char*>(d.data()) + off[1] + r + 16;
                const size_t l = strnlen(label, d.size() - off[1] - r - 16);
                labels.push_back(l < 200 ? std::string(label, l) : std::string());
            }
        }
        return labels;
    }
    std::map<std::string, Entry> byHash_;
};

// --- the seed ----------------------------------------------------------------------------------------------------------
struct Report {
    unsigned misses = 0;          // misses read (after duplicates)
    unsigned distinct = 0;        // distinct requests among them (bitcode, constants, IBO base, known buffer)
    unsigned found = 0;           // bitcode found by its SHA-256
    unsigned notFound = 0;
    unsigned compiled = 0;
    unsigned refused = 0;         // the host compiler refused it
    unsigned equal = 0;           // the phone's digest and words (and cache name): kept in the seed
    unsigned different = 0;       // another digest, word count or cache name: left out
    unsigned verified = 0;        // kept, and served back from the seed directory word for word
    uint64_t bytes = 0;           // the kept files' total size
    std::vector<std::string> lines;   // one per distinct request: what came of it
};

namespace detail {
inline std::string requestKey(const Miss& m) {
    std::string k = m.sha256 + "|" + std::to_string(m.iboBase) + "|";
    for (const FunctionConstant& c : m.constants) k += std::to_string(c.index) + "=" + std::to_string(c.bits) + ",";
    if (m.hasKnown) {
        k += "|" + std::to_string(m.known.binding) + ":";
        for (uint8_t b : m.known.bytes) k += char('a' + (b >> 4)), k += char('a' + (b & 15));
    }
    return k;
}
inline void setOrUnset(const char* name, const std::string& v) {
    if (v.empty()) {
        unsetenv(name);
    } else {
        setenv(name, v.c_str(), 1);
    }
}
inline void removeDir(const std::string& d) {
    if (DIR* dir = opendir(d.c_str())) {
        while (dirent* e = readdir(dir)) {
            if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) remove((d + "/" + e->d_name).c_str());
        }
        closedir(dir);
    }
    rmdir(d.c_str());
}
inline uint64_t fileSize(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0;
}
}  // namespace detail

// Compile every distinct miss of LOG whose bitcode INDEX has, for CHIP_ID, and keep the digest-equal ones in OUT_DIR
// (an existing directory; a file already there under the same name is replaced). The process's HAKIOS_METAL_CACHE and
// HAKIOS_METAL_CACHE_SEED are used for the scratch cache and put back as they were. *WHY and false: no scratch directory.
inline bool seedStages(const Log& log, const BitcodeIndex& index, const std::string& outDir, uint32_t chipId, Report* r, std::string* why) {
    const char* oldRam = getenv("HAKIOS_METAL_CACHE");
    const char* oldSeed = getenv("HAKIOS_METAL_CACHE_SEED");
    const std::string keepRam = oldRam != nullptr ? oldRam : "", keepSeed = oldSeed != nullptr ? oldSeed : "";
    std::string work = outDir + "/.hm-seed-XXXXXX", check = outDir + "/.hm-seed-check-XXXXXX";
    if (mkdtemp(&work[0]) == nullptr || mkdtemp(&check[0]) == nullptr) {
        *why = "cannot make a scratch directory in " + outDir;
        detail::removeDir(work);
        return false;
    }
    std::set<std::string> seen;
    for (const Miss& m : log.misses) {
        r->misses++;
        if (!seen.insert(detail::requestKey(m)).second) continue;
        r->distinct++;
        char head[160];
        snprintf(head, sizeof head, "%s #%u %s digest %016llx: ", m.who.empty() ? "?" : m.who.c_str(), m.number, m.fragment ? "fs" : "vs",
                 (unsigned long long)m.digest);
        const BitcodeIndex::Entry* e = index.find(m.sha256);
        if (e == nullptr) {
            r->notFound++;
            r->lines.push_back(std::string(head) + "bitcode not found (sha256 " + m.sha256 + ", " + std::to_string(m.size) + " bytes)");
            continue;
        }
        r->found++;
        // the scratch cache: writable, empty of this request, no seed
        detail::setOrUnset("HAKIOS_METAL_CACHE", work);
        detail::setOrUnset("HAKIOS_METAL_CACHE_SEED", "");
        std::string whyNot;
        std::unique_ptr<CompiledStage> s = compileStageForChip(e->bytes.data(), e->bytes.size(), chipId, &whyNot, m.constants, m.iboBase,
                                                               m.hasKnown ? &m.known : nullptr);
        if (s == nullptr) {
            r->refused++;
            r->lines.push_back(std::string(head) + "refused on the host: " + whyNot + " (" + e->origin + ")");
            continue;
        }
        r->compiled++;
        const std::string name = s->cacheName;
        const std::string scratch = name.empty() ? std::string() : work + name;
        std::string differs;
        if (name.empty()) differs = "no cache name on the host";
        else if (m.key != "-" && m.key != name) differs = "cache name " + name + ", the phone's " + m.key;
        else if (s->digest != m.digest) {
            char b[96];
            snprintf(b, sizeof b, "digest %016llx on the host", (unsigned long long)s->digest);
            differs = b;
        } else if (s->words.size() != m.words) {
            differs = std::to_string(s->words.size()) + " words on the host, " + std::to_string(m.words) + " on the phone";
        } else if (detail::fileSize(scratch) == 0) {
            differs = "no cache file written";
        }
        if (!differs.empty()) {
            r->different++;
            if (!scratch.empty()) remove(scratch.c_str());
            r->lines.push_back(std::string(head) + "left out: " + differs + " (" + e->origin + ")");
            continue;
        }
        const std::string kept = outDir + name;
        if (rename(scratch.c_str(), kept.c_str()) != 0) {
            r->different++;
            remove(scratch.c_str());
            r->lines.push_back(std::string(head) + "left out: cannot move the file into " + outDir);
            continue;
        }
        r->equal++;
        r->bytes += detail::fileSize(kept);
        // served back from the seed as the phone would be: a new empty RAM cache, OUT_DIR the seed
        detail::setOrUnset("HAKIOS_METAL_CACHE", check);
        detail::setOrUnset("HAKIOS_METAL_CACHE_SEED", outDir);
        std::unique_ptr<CompiledStage> back = compileStageForChip(e->bytes.data(), e->bytes.size(), chipId, &whyNot, m.constants, m.iboBase,
                                                                  m.hasKnown ? &m.known : nullptr);
        const bool ok = back != nullptr && back->cacheSource == 2 && back->words == s->words && back->digest == s->digest;
        r->verified += ok;
        r->lines.push_back(std::string(head) + "seeded " + name.substr(1) + (ok ? "" : " (NOT served back from the seed)") + " (" + e->origin + ")");
    }
    detail::setOrUnset("HAKIOS_METAL_CACHE", keepRam);
    detail::setOrUnset("HAKIOS_METAL_CACHE_SEED", keepSeed);
    detail::removeDir(work);
    detail::removeDir(check);
    return true;
}

}  // namespace hakios::metal::seed
