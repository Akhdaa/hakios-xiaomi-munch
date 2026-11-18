// metal-ca-perf task 5.2: hm-seed (tools/hm_seed.hpp, tools/hm-seed.cpp), the A650's compiled-stage seed built on the
// host from a phone run's "stage cache:" and "stage miss:" lines. A synthetic run log made from the fixture kernels
// stands for the phone: its stages compiled here for the A650 (the "phone" side), written as the plug-in's
// hm_perf_stage writes them (one miss long enough for "stage miss: #N continued:" pieces, one from another process
// interleaved, a line seen twice), then seeded, and the seed served back as the phone's next boot would be served.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../tools/hm_seed.hpp"
#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/compiler.hpp"
#include "test.hpp"

using namespace hakios::metal;

namespace {
std::vector<uint8_t> fixtureBitcode(const char* name) {
    std::vector<uint8_t> b;
    seed::readFile(std::string("out/host/kernels/") + name + ".bc", &b);
    return b;
}
std::vector<std::string> dirEntries(const std::string& d) {
    std::vector<std::string> out;
    if (DIR* dir = opendir(d.c_str())) {
        while (dirent* e = readdir(dir)) {
            if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) out.push_back(e->d_name);
        }
        closedir(dir);
    }
    return out;
}
void removeAll(const std::string& d) {
    for (const std::string& e : dirEntries(d)) {
        const std::string p = d + "/" + e;
        struct stat st{};
        if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            removeAll(p);
        } else {
            remove(p.c_str());
        }
    }
    rmdir(d.c_str());
}
void cacheDirs(const std::string& ram, const std::string& seedDir) {
    if (ram.empty()) unsetenv("HAKIOS_METAL_CACHE");
    else setenv("HAKIOS_METAL_CACHE", ram.c_str(), 1);
    if (seedDir.empty()) unsetenv("HAKIOS_METAL_CACHE_SEED");
    else setenv("HAKIOS_METAL_CACHE_SEED", seedDir.c_str(), 1);
}
}  // namespace

TEST(hm_seed_sha256_vectors) {
    CHECK(seed::sha256Hex(nullptr, 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(seed::sha256Hex(reinterpret_cast<const uint8_t*>("abc"), 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";   // 56 bytes: the padding takes a second block
    CHECK(seed::sha256Hex(reinterpret_cast<const uint8_t*>(two), strlen(two)) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    std::vector<uint8_t> million(1000000, 'a');
    CHECK(seed::sha256Hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// The plug-in's lines as they are in a run's union: the once-line, misses of two processes, a long miss in pieces with
// another process's line between them, a line seen twice, a piece whose start is not in the log.
TEST(hm_seed_parses_the_plugin_lines) {
    const std::string sha(64, 'a');
    std::string text =
        "HakiosMetal[backboardd 14]: stage cache: build 50c9ecf8c8b9d6669cdf88c3, layout 1448/848, off (metal_cache=0)\n"
        "[   12.345678] adreno: dcvs: something else\n"
        "HakiosMetal[backboardd 14]: stage miss: vs #1 key - words 64 digest b3c170f786424dbc in 812.4 ms; bitcode sha256 " + sha +
        " (4312 bytes); constants none; ibo 0; known none\n";
    seed::Miss longOne;
    longOne.fragment = true;
    longOne.number = 2;
    longOne.key = "/0123456789abcdef0123456789abcdef.stage";
    longOne.words = 96;
    longOne.digest = 0x29515c8ff1f29e3bull;
    longOne.sha256 = std::string(64, 'b');
    longOne.size = 123456;
    for (unsigned i = 0; i < 30; i++) longOne.constants.push_back({i, i == 0 ? 0ull : 0x8000000000000000ull | i});
    longOne.iboBase = 4;
    longOne.hasKnown = true;
    longOne.known.binding = 0;
    for (unsigned i = 0; i < 256; i++) longOne.known.bytes.push_back(uint8_t(i * 7));
    const std::string pieces = seed::missLines("HakiosMetal[backboardd 14]", longOne, 1628.0);
    CHECK(pieces.find("stage miss: #2 continued: ") != std::string::npos);
    // the first piece, a line of another process, then the rest
    const size_t firstEnd = pieces.find('\n') + 1;
    text += pieces.substr(0, firstEnd);
    text += "HakiosMetal[SpringBoard 40]: stage miss: fs #1 key /ffffffffffffffffffffffffffffffff.stage words 32 digest 0000000000000001 in 3.0 ms; "
            "bitcode sha256 " + std::string(64, 'c') + " (100 bytes); constants 3=0x1 7=0; ibo 2; known binding 1 bytes 00ff\n";
    text += pieces.substr(firstEnd);
    // seen again (an overlapping snapshot), and a piece without its start
    text += "HakiosMetal[backboardd 14]: stage miss: vs #1 key - words 64 digest b3c170f786424dbc in 812.4 ms; bitcode sha256 " + sha +
            " (4312 bytes); constants none; ibo 0; known none\n";
    text += "HakiosMetal[backboardd 14]: stage miss: #9 continued: 0011\n";

    const seed::Log log = seed::parseLog(text);
    CHECK_EQ(log.caches.size(), size_t(1));
    if (!log.caches.empty()) {
        CHECK(log.caches[0].who == "HakiosMetal[backboardd 14]");
        CHECK(log.caches[0].build == "50c9ecf8c8b9d6669cdf88c3");
        CHECK(log.caches[0].layout == "1448/848");
    }
    CHECK_EQ(log.misses.size(), size_t(3));
    CHECK_EQ(log.duplicates, 1u);
    CHECK_EQ(log.unreadable, 1u);
    CHECK_EQ(log.unfinished, 0u);
    if (log.misses.size() != 3) return;
    const seed::Miss& a = log.misses[0];
    CHECK(!a.fragment && a.number == 1 && a.key == "-" && a.words == 64 && a.digest == 0xb3c170f786424dbcull && a.sha256 == sha);
    CHECK(a.size == 4312 && a.constants.empty() && a.iboBase == 0 && !a.hasKnown);
    const seed::Miss& b = log.misses[1];   // SpringBoard's, complete before backboardd's long one
    CHECK(b.who == "HakiosMetal[SpringBoard 40]" && b.fragment && b.iboBase == 2 && b.hasKnown && b.known.binding == 1);
    CHECK(b.constants.size() == 2 && b.constants[0].index == 3 && b.constants[0].bits == 1 && b.constants[1].index == 7 && b.constants[1].bits == 0);
    CHECK(b.known.bytes == std::vector<uint8_t>({0x00, 0xff}));
    const seed::Miss& c = log.misses[2];
    CHECK(c.who == "HakiosMetal[backboardd 14]" && c.fragment && c.number == 2 && c.key == longOne.key && c.words == 96);
    CHECK(c.digest == longOne.digest && c.sha256 == longOne.sha256 && c.size == longOne.size && c.iboBase == 4);
    CHECK_EQ(c.constants.size(), longOne.constants.size());
    for (size_t i = 0; i < c.constants.size() && i < longOne.constants.size(); i++) {
        CHECK(c.constants[i].index == longOne.constants[i].index && c.constants[i].bits == longOne.constants[i].bits);
    }
    CHECK(c.hasKnown && c.known.binding == 0 && c.known.bytes == longOne.known.bytes);
    // a miss whose last piece never came is unfinished, not a miss
    const seed::Log cut = seed::parseLog(pieces.substr(0, firstEnd));
    CHECK(cut.misses.empty() && cut.unfinished == 1);
}

TEST(hm_seed_refuses_another_build_or_layout) {
    std::string why;
    const std::string host = "50c9ecf8c8b9d6669cdf88c3 1448/848";
    auto logOf = [](const char* line) { return seed::parseLog(line); };
    CHECK(seed::checkBuild(logOf("HakiosMetal[backboardd 14]: stage cache: build 50c9ecf8c8b9d6669cdf88c3, layout 1448/848, off (metal_cache=0)\n"), host, &why));
    CHECK(!seed::checkBuild(logOf("HakiosMetal[backboardd 14]: stage cache: build 0123456789abcdef01234567, layout 1448/848, off\n"), host, &why));
    CHECK(why.find("compiler build") != std::string::npos);
    CHECK(!seed::checkBuild(logOf("HakiosMetal[backboardd 14]: stage cache: build 50c9ecf8c8b9d6669cdf88c3, layout 1448/840, off\n"), host, &why));
    CHECK(why.find("layout") != std::string::npos);
    CHECK(!seed::checkBuild(logOf("HakiosMetal[backboardd 14]: stage miss: vs #1 ...\n"), host, &why));
    CHECK(why.find("no \"stage cache:\" line") != std::string::npos);
    // two processes, one of another build: refused
    CHECK(!seed::checkBuild(logOf("HakiosMetal[a 1]: stage cache: build 50c9ecf8c8b9d6669cdf88c3, layout 1448/848, x\n"
                                  "HakiosMetal[b 2]: stage cache: build 50c9ecf8c8b9d6669cdf88c4, layout 1448/848, x\n"),
                            host, &why));
    CHECK(!seed::checkBuild(logOf("HakiosMetal[a 1]: stage cache: build x, layout 1448/848, x\n"), "", &why));
}

// The fixture kernels as the phone's stages: compiled for the A650 (the "phone"), logged, seeded on the "host" (this
// process, the same compiler build), then the next boot served from the seed. Kept: the digest-equal ones only; a
// logged digest the host does not reproduce, a cache name not the host's, a bitcode the host does not have: left out.
// **Validates: Requirements 2.2, 3.6, 3.11**
TEST(hm_seed_seeds_the_digest_equal_stages_only) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: hm-seed's compile and seed did not run)\n");
        return;
    }
    const std::vector<uint8_t> vs = fixtureBitcode("tri_flat_vs"), fs = fixtureBitcode("tri_flat_fs"), cfs = fixtureBitcode("tri_cflat_fs");
    CHECK(!vs.empty() && !fs.empty() && !cfs.empty());
    if (vs.empty() || fs.empty() || cfs.empty()) return;
    char base[] = "/tmp/hakios-metal-hm-seed-XXXXXX";
    CHECK(mkdtemp(base) != nullptr);
    const std::string phoneRam = std::string(base) + "/phone-ram", out = std::string(base) + "/seed", next = std::string(base) + "/next-ram";
    mkdir(phoneRam.c_str(), 0755);
    mkdir(out.c_str(), 0755);
    mkdir(next.c_str(), 0755);

    // the phone's requests
    struct Ask {
        const std::vector<uint8_t>* bc;
        FunctionConstants constants;
        uint32_t ibo = 0;
        bool hasKnown = false;
        KnownBuffer known;
        bool loggedWithoutKey = false;   // metal_cache=0: "key -"
        int spoil = 0;                   // 1: the logged digest is not the phone's; 2: the logged key is not the phone's
        bool keep = false;               // expected in the seed
    };
    std::vector<Ask> asks(6);
    asks[0].bc = &vs, asks[0].loggedWithoutKey = true, asks[0].keep = true;
    asks[1].bc = &fs, asks[1].keep = true;
    for (unsigned i = 0; i < 40; i++) asks[1].constants.push_back({i + 16, 0x0123456789abcdefull + i});   // a miss in pieces
    asks[2].bc = &cfs, asks[2].hasKnown = true, asks[2].keep = true;
    for (unsigned i = 0; i < 52; i++) asks[2].known.bytes.push_back(uint8_t(i * 5 + 1));
    asks[3].bc = &vs, asks[3].constants = {{1, 5}}, asks[3].spoil = 1;
    asks[4].bc = &fs, asks[4].ibo = 4, asks[4].spoil = 2;
    asks[5].bc = &cfs, asks[5].constants = {{2, 1}};   // its bitcode replaced below by bytes the host has not
    const std::string who = "HakiosMetal[backboardd 14]";
    const std::string all = stageCacheBuild();
    std::string text = who + ": stage cache: build " + all.substr(0, all.rfind(' ')) + ", layout " + all.substr(all.rfind(' ') + 1) +
                       ", RAM cache " + phoneRam + " (writable: yes), seed none (0 stages)\n";
    std::vector<std::unique_ptr<CompiledStage>> phone;
    std::string why;
    cacheDirs(phoneRam, "");
    for (size_t i = 0; i < asks.size(); i++) {
        const Ask& a = asks[i];
        phone.push_back(compileStageForChip(a.bc->data(), a.bc->size(), a6xx::kA650ChipId, &why, a.constants, a.ibo, a.hasKnown ? &a.known : nullptr));
        CHECK(phone.back() != nullptr && phone.back()->cacheSource == 0 && !phone.back()->cacheName.empty());
        if (phone.back() == nullptr) {
            std::fprintf(stderr, "  ask %zu: %s\n", i, why.c_str());
            cacheDirs("", "");
            removeAll(base);
            return;
        }
        seed::Miss m;
        m.number = unsigned(i + 1);
        m.fragment = a.bc != &vs;
        m.key = a.loggedWithoutKey ? "-" : phone.back()->cacheName;
        if (a.spoil == 2) m.key = "/00000000000000000000000000000000.stage";
        m.words = uint32_t(phone.back()->words.size());
        m.digest = phone.back()->digest ^ (a.spoil == 1 ? 1 : 0);
        m.sha256 = seed::sha256Hex(*a.bc);
        if (i == 5) m.sha256 = seed::sha256Hex(reinterpret_cast<const uint8_t*>("not a module the host has"), 25);
        m.size = a.bc->size();
        m.constants = a.constants;
        m.iboBase = a.ibo;
        m.hasKnown = a.hasKnown;
        m.known = a.known;
        text += seed::missLines(who, m, 812.5);
        if (i == 0) text += seed::missLines(who, m, 812.5);   // the same line twice
        if (i == 1) CHECK(seed::missLines(who, m, 1.0).find("stage miss: #2 continued: ") != std::string::npos);
    }

    // the host: the log, the fixture kernels' bitcode, the seed
    const seed::Log log = seed::parseLog(text);
    CHECK_EQ(log.misses.size(), asks.size());
    CHECK_EQ(log.duplicates, 1u);
    CHECK(seed::checkBuild(log, stageCacheBuild(), &why));
    seed::BitcodeIndex index;
    CHECK(index.addBcDir("out/host/kernels") >= 40);
    seed::Report r;
    CHECK(seed::seedStages(log, index, out, a6xx::kA650ChipId, &r, &why));
    for (const std::string& l : r.lines) std::fprintf(stderr, "  hm-seed: %s\n", l.c_str());
    CHECK_EQ(r.misses, 6u);
    CHECK_EQ(r.distinct, 6u);
    CHECK_EQ(r.found, 5u);
    CHECK_EQ(r.notFound, 1u);
    CHECK_EQ(r.compiled, 5u);
    CHECK_EQ(r.equal, 3u);
    CHECK_EQ(r.verified, 3u);
    CHECK_EQ(r.different, 2u);
    CHECK(r.bytes > 0);
    // the seed holds the kept stages under the phone's names, and nothing else (no scratch left)
    std::vector<std::string> files = dirEntries(out);
    CHECK_EQ(files.size(), size_t(3));
    for (size_t i = 0; i < asks.size(); i++) {
        struct stat st{};
        const bool there = stat((out + phone[i]->cacheName).c_str(), &st) == 0;
        CHECK(there == asks[i].keep);
    }

    // the phone's next boot: an empty RAM cache, the seed read-only: the kept stages served from it word for word, the
    // ones left out compiled as before
    cacheDirs(next, out);
    for (size_t i = 0; i < asks.size(); i++) {
        const Ask& a = asks[i];
        std::unique_ptr<CompiledStage> s =
            compileStageForChip(a.bc->data(), a.bc->size(), a6xx::kA650ChipId, &why, a.constants, a.ibo, a.hasKnown ? &a.known : nullptr);
        CHECK(s != nullptr);
        if (s == nullptr) continue;
        CHECK_EQ(s->cacheSource, a.keep ? 2u : 0u);
        CHECK(s->words == phone[i]->words);
        CHECK_EQ(s->digest, phone[i]->digest);
        CHECK(s->varyingNames == phone[i]->varyingNames);
        CHECK(s->cacheName == phone[i]->cacheName);
    }
    cacheDirs("", "");
    removeAll(base);
}
