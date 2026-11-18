// metal-ca-perf, Property 1 (Bug Condition), C1: on the A650 (chip 0x06050002) a stage whose key was compiled earlier
// in the boot, or is in the seed built from an earlier boot's files, is served from the RAM cache (HAKIOS_METAL_CACHE,
// cacheSource 1) or from the seed (HAKIOS_METAL_CACHE_SEED, cacheSource 2) -- not compiled -- and its words are the
// words the compiler gave the first time.
//
// The stages: tri_flat_vs and tri_flat_fs (a VS/FS pair) and tri_cflat_fs specialised for a 52-byte known buffer 0
// (CoreAnimation's uber shader is specialised for the 52 bytes of its FragmentShaderConfig), each compiled with
// compileStageForChip(..., a6xx::kA650ChipId, ...). "Boot 1" compiles them into an empty RAM-cache directory; "boot 2"
// has a new empty RAM cache and boot 1's files as its seed (what the host seed build puts into the small APFS).
//
// On the code before the fix (the seam: stageCacheName(..., chipId) is "" for every chip but a6xx::kChipId) nothing is
// filed or found for the A650, so this property FAILS: that is the bug. Without the compiler in the build (no MESA=1)
// there is nothing to compile and the tests say so.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/compiler.hpp"
#include "test.hpp"

using namespace hakios::metal;

namespace {
std::vector<uint8_t> stageBitcode(const char* name) {
    std::vector<uint8_t> bytes;
    const std::string path = std::string("out/host/kernels/") + name + ".bc";
    if (FILE* f = fopen(path.c_str(), "rb")) {
        uint8_t chunk[4096];
        for (size_t n; (n = fread(chunk, 1, sizeof chunk, f)) > 0;) bytes.insert(bytes.end(), chunk, chunk + n);
        fclose(f);
    }
    return bytes;
}
std::vector<std::string> stageFiles(const std::string& d) {
    std::vector<std::string> out;
    if (DIR* dir = opendir(d.c_str())) {
        while (dirent* e = readdir(dir)) {
            const size_t n = strlen(e->d_name);
            if (n > 6 && strcmp(e->d_name + n - 6, ".stage") == 0) out.push_back(e->d_name);
        }
        closedir(dir);
    }
    return out;
}
void removeTree(const std::string& d) {
    if (DIR* dir = opendir(d.c_str())) {
        while (dirent* e = readdir(dir)) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            const std::string p = d + "/" + e->d_name;
            struct stat st{};
            if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                removeTree(p);
            } else {
                remove(p.c_str());
            }
        }
        closedir(dir);
    }
    rmdir(d.c_str());
}
std::string makeDir(const std::string& base, const char* name) {
    const std::string d = base + "/" + name;
    mkdir(d.c_str(), 0755);
    return d;
}
// a "boot": the RAM cache RAM (writable), the seeds SEED ("" none)
void boot(const std::string& ram, const std::string& seed) {
    if (ram.empty()) {
        unsetenv("HAKIOS_METAL_CACHE");
    } else {
        setenv("HAKIOS_METAL_CACHE", ram.c_str(), 1);
    }
    if (seed.empty()) {
        unsetenv("HAKIOS_METAL_CACHE_SEED");
    } else {
        setenv("HAKIOS_METAL_CACHE_SEED", seed.c_str(), 1);
    }
}
void copyFile(const std::string& from, const std::string& to) {
    FILE* in = fopen(from.c_str(), "rb");
    FILE* out = fopen(to.c_str(), "wb");
    if (in != nullptr && out != nullptr) {
        char chunk[65536];
        for (size_t n; (n = fread(chunk, 1, sizeof chunk, in)) > 0;) fwrite(chunk, 1, n, out);
    }
    if (in != nullptr) fclose(in);
    if (out != nullptr) fclose(out);
}

// one stage request of the plug-in: a bitcode and, for a specialisation, its known buffer
struct StageAsk {
    const char* name;
    std::vector<uint8_t> bitcode;
    bool specialised = false;
    KnownBuffer known;
};
std::vector<StageAsk> requests() {
    std::vector<StageAsk> r(3);
    r[0].name = "tri_flat_vs";
    r[1].name = "tri_flat_fs";
    r[2].name = "tri_cflat_fs (52-byte known buffer 0)";
    r[0].bitcode = stageBitcode("tri_flat_vs");
    r[1].bitcode = stageBitcode("tri_flat_fs");
    r[2].bitcode = stageBitcode("tri_cflat_fs");
    r[2].specialised = true;
    r[2].known.binding = 0;
    const float c[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    r[2].known.bytes.assign(reinterpret_cast<const uint8_t*>(c), reinterpret_cast<const uint8_t*>(c) + 16);
    for (unsigned i = 16; i < 52; i++) r[2].known.bytes.push_back(uint8_t(i * 5 + 1));
    return r;
}
std::unique_ptr<CompiledStage> compileA650(const StageAsk& q, std::string* why) {
    return compileStageForChip(q.bitcode.data(), q.bitcode.size(), a6xx::kA650ChipId, why, {}, 0, q.specialised ? &q.known : nullptr);
}
const char* sourceName(uint32_t s) { return s == 0 ? "compiled" : s == 1 ? "the RAM cache" : s == 2 ? "the seed" : "?"; }
}  // namespace

// The scoped cases: boot 1 into an empty RAM cache, the same stages again in boot 1, then boot 2 with a new empty RAM
// cache and boot 1's files as the seed.
// **Validates: Requirements 1.2, 2.2, 2.3**
TEST(stage_cache_property_1_bug_condition_a650_stage_not_compiled_twice) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: the A650 stage cache did not run)\n");
        return;
    }
    std::vector<StageAsk> rq = requests();
    for (const StageAsk& q : rq) CHECK(!q.bitcode.empty());
    if (rq[0].bitcode.empty() || rq[1].bitcode.empty() || rq[2].bitcode.empty()) return;
    char base[] = "/tmp/hakios-metal-stage-cache-XXXXXX";
    CHECK(mkdtemp(base) != nullptr);
    const std::string ram1 = makeDir(base, "ram1"), ram2 = makeDir(base, "ram2"), seed = makeDir(base, "seed");
    std::string why;

    // boot 1: first-ever compiles, into an empty RAM cache
    boot(ram1, "");
    std::vector<std::unique_ptr<CompiledStage>> first;
    for (const StageAsk& q : rq) {
        first.push_back(compileA650(q, &why));
        CHECK(first.back() != nullptr);
        if (first.back() == nullptr) std::fprintf(stderr, "  %s: %s\n", q.name, why.c_str());
    }
    const std::vector<std::string> filed = stageFiles(ram1);
    if (filed.size() != rq.size()) {
        std::fprintf(stderr, "  counterexample (boot 1): %zu A650 .stage file(s) in the RAM cache after compiling %zu stages\n", filed.size(),
                     rq.size());
    }
    CHECK_EQ(filed.size(), rq.size());

    // boot 1 again: the same stages a second time (a second pipeline object) come from the RAM cache
    for (size_t i = 0; i < rq.size(); i++) {
        std::unique_ptr<CompiledStage> again = compileA650(rq[i], &why);
        CHECK(again != nullptr);
        if (again == nullptr || first[i] == nullptr) continue;
        if (again->cacheSource == 0) {
            std::fprintf(stderr, "  counterexample (boot 1, second request): %s %s again (cache name \"%s\", %llu us)\n", rq[i].name,
                         sourceName(again->cacheSource), again->cacheName.c_str(), (unsigned long long)again->compileUs);
        }
        CHECK(again->cacheSource != 0);
        CHECK(again->words == first[i]->words);
        CHECK_EQ(again->digest, first[i]->digest);
    }

    // boot 2: a new empty RAM cache, boot 1's files as the seed
    for (const std::string& f : filed) copyFile(ram1 + "/" + f, seed + "/" + f);
    boot(ram2, seed);
    for (size_t i = 0; i < rq.size(); i++) {
        std::unique_ptr<CompiledStage> s = compileA650(rq[i], &why);
        CHECK(s != nullptr);
        if (s == nullptr || first[i] == nullptr) continue;
        if (s->cacheSource == 0) {
            std::fprintf(stderr, "  counterexample (boot 2, seed of %zu file(s)): %s %s again (cache name \"%s\", %llu us)\n", filed.size(),
                         rq[i].name, sourceName(s->cacheSource), s->cacheName.c_str(), (unsigned long long)s->compileUs);
        }
        CHECK(s->cacheSource != 0);
        CHECK(s->words == first[i]->words);
        CHECK_EQ(s->digest, first[i]->digest);
    }
    boot("", "");
    removeTree(base);
}

// Generated: random request sequences over the three stages across two boots (boot 2 seeded with boot 1's RAM cache,
// or not), as many pipeline objects ask for the same stages. Property: no key is compiled twice -- a request is
// compiled only the first time its stage is asked for in boot 1, or in boot 2 when boot 1 never asked for it -- and
// every stage served has the words of its first compile.
// **Validates: Requirements 1.2, 2.2, 2.3**
TEST(stage_cache_property_1_generated_requests_over_two_boots) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: the A650 stage cache did not run)\n");
        return;
    }
    std::vector<StageAsk> rq = requests();
    if (rq[0].bitcode.empty() || rq[1].bitcode.empty() || rq[2].bitcode.empty()) {
        CHECK(false);
        return;
    }
    std::mt19937 rng(0xa650cac0u);
    int requestsMade = 0, bugCases = 0, compiledAgain = 0, wrongWords = 0, printed = 0;
    std::string why;
    for (int round = 0; round < 6; round++) {
        char base[] = "/tmp/hakios-metal-stage-cache-XXXXXX";
        CHECK(mkdtemp(base) != nullptr);
        const std::string ram1 = makeDir(base, "ram1"), ram2 = makeDir(base, "ram2"), seed = makeDir(base, "seed");
        const bool seeded = rng() % 3 != 0;
        std::map<size_t, std::vector<uint32_t>> words;   // the first compile's words of each stage
        std::map<size_t, bool> askedInBoot1;
        for (int b = 0; b < 2; b++) {
            if (b == 0) {
                boot(ram1, "");
            } else {
                for (const std::string& f : stageFiles(ram1)) copyFile(ram1 + "/" + f, seed + "/" + f);
                boot(ram2, seeded ? seed : "");
            }
            std::map<size_t, bool> askedThisBoot;
            const unsigned n = 2 + rng() % 4;
            for (unsigned k = 0; k < n; k++) {
                const size_t i = rng() % rq.size();
                std::unique_ptr<CompiledStage> s = compileA650(rq[i], &why);
                requestsMade++;
                CHECK(s != nullptr);
                if (s == nullptr) continue;
                // compiled before: earlier in this boot, or in boot 1 when boot 2 has boot 1's files as its seed
                const bool before = askedThisBoot[i] || (b == 1 && seeded && askedInBoot1[i]);
                bugCases += before;
                if (before && s->cacheSource == 0) {
                    compiledAgain++;
                    if (printed++ < 5) {
                        std::fprintf(stderr, "  counterexample (round %d, boot %d%s, request %u): %s asked for before, %s again\n", round, b + 1,
                                     b == 1 ? (seeded ? ", seeded" : ", no seed") : "", k + 1, rq[i].name, sourceName(s->cacheSource));
                    }
                }
                if (words.count(i) == 0) {
                    words[i] = s->words;
                } else if (s->words != words[i]) {
                    wrongWords++;
                }
                askedThisBoot[i] = true;
                if (b == 0) askedInBoot1[i] = true;
            }
        }
        boot("", "");
        removeTree(base);
    }
    std::fprintf(stderr, "  stage cache property 1: %d requests, %d of them for a stage asked for before, %d compiled again, %d with other words\n",
                 requestsMade, bugCases, compiledAgain, wrongWords);
    CHECK(bugCases > 0);   // the generator reaches the bug condition
    CHECK_EQ(compiledAgain, 0);
    CHECK_EQ(wrongWords, 0);
}

// --- metal-ca-perf, Property 2 (Preservation): the compiled-stage cache outside the A650's bug condition ---------------
// Observed on the code before the fix and written down here, to hold after it:
// - redwood (a6xx::kChipId): the cache file's name is today's hash, byte for byte -- FNV over the compiler build (the
//   part of stageCacheBuild() before the layout), the layout {sizeof(Stage), sizeof(Kernel), IBO base}, the bitcode,
//   the constants (index, bits) and the known buffer (binding, bytes), printed "/<16 hex><16 hex>.stage" -- for every
//   fixture vertex and fragment bitcode, plain and with generated constants, known buffers and IBO bases; the file is
//   written in the format stageCacheSave writes today (reproduced below from the stage compiled) and a second request
//   comes back from it word for word;
// - chips without a compiler (0, 0x06030501): refused by name, no cache file, no name;
// - the A650's first-ever compile of a stage (nothing filed yet, a cache directory configured or not): the words,
//   immediates, resources, varying names and digest of the compile without any cache -- and the digests observed for
//   the three exploration stages (tri_flat_vs, tri_flat_fs, tri_cflat_fs for its 52-byte buffer), golden below.
// These pass on the unfixed code and are to pass, unchanged, after task 5 (task 5.6).
namespace {
// today's stageCacheName (compiler.cpp), for a6xx::kChipId: "" when neither cache variable is set
std::string todaysCacheName(const std::vector<uint8_t>& bc, const FunctionConstants& constants, uint32_t iboBase, const KnownBuffer* known) {
    const char* dir = getenv("HAKIOS_METAL_CACHE");
    const char* seed = getenv("HAKIOS_METAL_CACHE_SEED");
    if ((dir == nullptr || dir[0] == 0) && (seed == nullptr || seed[0] == 0)) return {};
    const std::string all = stageCacheBuild();
    const std::string build = all.substr(0, all.rfind(' '));
    uint64_t h[2] = {0xcbf29ce484222325ull, 0x84222325cbf29ce4ull};
    auto mix = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t b = static_cast<const uint8_t*>(p)[i];
            h[0] = (h[0] ^ b) * 0x100000001b3ull;
            h[1] = (h[1] ^ b) * 0x1000193ull + (h[1] >> 29);
        }
    };
    const uint32_t layout[3] = {uint32_t(sizeof(Stage)), uint32_t(sizeof(Kernel)), iboBase};
    mix(build.data(), build.size());
    mix(layout, sizeof(layout));
    mix(bc.data(), bc.size());
    for (const FunctionConstant& c : constants) {
        mix(&c.index, sizeof(c.index));
        mix(&c.bits, sizeof(c.bits));
    }
    if (known != nullptr) {
        mix(&known->binding, sizeof(known->binding));
        mix(known->bytes.data(), known->bytes.size());
    }
    char name[64];
    snprintf(name, sizeof(name), "/%016llx%016llx.stage", (unsigned long long)h[0], (unsigned long long)h[1]);
    return name;
}
// today's cache file (stageCacheSave) of S, as compiled (its Stage as it lies in memory)
std::string todaysCacheFile(const CompiledStage& s) {
    std::string out;
    auto raw = [&](const void* p, size_t n) { out.append(static_cast<const char*>(p), n); };
    auto u32 = [&](uint32_t v) { raw(&v, 4); };
    auto str = [&](const std::string& v) {
        u32(uint32_t(v.size()));
        raw(v.data(), v.size());
    };
    u32(0x53434d48);   // "HMCS"
    raw(&s.stage, sizeof(s.stage));
    u32(uint32_t(s.words.size()));
    raw(s.words.data(), s.words.size() * 4);
    u32(uint32_t(s.immediates.size()));
    raw(s.immediates.data(), s.immediates.size() * 4);
    u32(uint32_t(s.resources.size()));
    for (const Resource& r : s.resources) {
        u32(uint32_t(r.kind));
        u32(r.index);
        u32(r.sizeBytes);
        const uint8_t flags[8] = {r.read, r.write, r.irWrite, r.textureDim, r.textureArray, r.element, r.nullTexture, r.inlineSampler};
        raw(flags, sizeof(flags));
        str(r.typeName);
        u32(r.argbufSet);
        u32(r.argbufOffset);
    }
    u32(uint32_t(s.varyingNames.size()));
    for (const std::string& v : s.varyingNames) str(v);
    u32(uint32_t(s.inlineSamplers.size()));
    for (const auto& is : s.inlineSamplers) {
        u32(is.first);
        raw(&is.second, sizeof(is.second));
    }
    u32(s.iboBase);
    u32(s.writesMemory ? 1 : 0);
    str(s.name);
    str(s.listing);
    return out;
}
std::string fileBytes(const std::string& path) {
    std::string data;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char chunk[65536];
        for (size_t n; (n = fread(chunk, 1, sizeof chunk, f)) > 0;) data.append(chunk, n);
        fclose(f);
    }
    return data;
}
// every fixture vertex and fragment function's bitcode (out/host/kernels/*_vs.bc, *_fs.bc), by name
std::vector<std::pair<std::string, std::vector<uint8_t>>> stageFixtures() {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
    std::vector<std::string> names;
    if (DIR* dir = opendir("out/host/kernels")) {
        while (dirent* e = readdir(dir)) {
            const std::string n = e->d_name;
            if (n.size() > 6 && (n.compare(n.size() - 6, 6, "_vs.bc") == 0 || n.compare(n.size() - 6, 6, "_fs.bc") == 0)) names.push_back(n.substr(0, n.size() - 3));
        }
        closedir(dir);
    }
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) out.push_back({n, stageBitcode(n.c_str())});
    return out;
}
// one generated variant of a stage request: constants, a known buffer, an IBO base (any of them, or none)
struct Variant {
    FunctionConstants constants;
    uint32_t iboBase = 0;
    bool hasKnown = false;
    KnownBuffer known;
};
Variant randomVariant(std::mt19937& rng) {
    Variant v;
    const unsigned kind = rng() % 4;
    if (kind == 0 || kind == 3) {
        const unsigned n = 1 + rng() % 3;
        for (unsigned i = 0; i < n; i++) v.constants.push_back({uint32_t(rng() % 8), (uint64_t(rng()) << 32) | rng()});
    }
    if (kind == 1 || kind == 3) {
        v.hasKnown = true;
        v.known.binding = rng() % 2;
        const unsigned n = 4 * (1 + rng() % 16);
        for (unsigned i = 0; i < n; i++) v.known.bytes.push_back(uint8_t(rng()));
    }
    if (kind == 2 || kind == 3) v.iboBase = 4 * (rng() % 3);
    return v;
}
// the A650 digests observed on the code before the fix (first-ever compiles of the exploration stages)
struct GoldenDigest {
    const char* name;
    uint64_t digest;
};
const GoldenDigest kA650Digests[] = {
    {"tri_flat_vs", 0xb3c170f786424dbcull},                              // 64 words
    {"tri_flat_fs", 0xd20624b23231bf05ull},                              // 32 words
    {"tri_cflat_fs (52-byte known buffer 0)", 0x29515c8ff1f29e3bull},    // 32 words
};
}  // namespace

// Redwood's cache names byte for byte today's, its files in today's format, served again word for word; chips without
// a compiler get no name and no file. **Validates: Requirements 3.2, 3.6**
TEST(stage_cache_property_2_preservation_redwood_names_and_file_format) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: the redwood stage cache did not run)\n");
        return;
    }
    const std::string all = stageCacheBuild();
    const std::string layout = std::to_string(sizeof(Stage)) + "/" + std::to_string(sizeof(Kernel));
    CHECK(all.size() > layout.size() && all.compare(all.size() - layout.size(), layout.size(), layout) == 0);
    const auto fixtures = stageFixtures();
    CHECK(fixtures.size() >= 40);
    char base[] = "/tmp/hakios-metal-stage-cache-XXXXXX";
    CHECK(mkdtemp(base) != nullptr);
    const std::string ram = makeDir(base, "ram"), other = makeDir(base, "other");
    std::mt19937 rng(0x06030500u);
    int compiled = 0, refused = 0, nameWrong = 0, fileWrong = 0, againWrong = 0, printed = 0;
    std::string why;
    // no cache configured: no name (today's "" before the hash)
    boot("", "");
    if (!fixtures.empty()) {
        std::unique_ptr<CompiledStage> s = compileStageForChip(fixtures[0].second.data(), fixtures[0].second.size(), a6xx::kChipId, &why);
        CHECK(s == nullptr || s->cacheName.empty());
    }
    boot(ram, "");
    for (const auto& fx : fixtures) {
        for (int variant = 0; variant < 2; variant++) {
            const Variant v = variant == 0 ? Variant{} : randomVariant(rng);
            const KnownBuffer* known = v.hasKnown ? &v.known : nullptr;
            std::unique_ptr<CompiledStage> s =
                compileStageForChip(fx.second.data(), fx.second.size(), a6xx::kChipId, &why, v.constants, v.iboBase, known);
            if (s == nullptr) {
                refused++;
                continue;
            }
            compiled++;
            const std::string want = todaysCacheName(fx.second, v.constants, v.iboBase, known);
            if (s->cacheName != want) {
                nameWrong++;
                if (printed++ < 5)
                    std::fprintf(stderr, "  counterexample (%s, variant %d): cache name \"%s\", today's \"%s\"\n", fx.first.c_str(), variant,
                                 s->cacheName.c_str(), want.c_str());
                continue;
            }
            // filed in today's format (a stage compiled now; one served from the file has its own pointers)
            if (s->cacheSource == 0 && fileBytes(ram + want) != todaysCacheFile(*s)) {
                fileWrong++;
                if (printed++ < 5) std::fprintf(stderr, "  counterexample (%s, variant %d): the file %s is not today's format\n", fx.first.c_str(), variant, want.c_str());
            }
            std::unique_ptr<CompiledStage> again =
                compileStageForChip(fx.second.data(), fx.second.size(), a6xx::kChipId, &why, v.constants, v.iboBase, known);
            if (again == nullptr || again->cacheSource != 1 || again->words != s->words || again->immediates != s->immediates ||
                again->varyingNames != s->varyingNames || again->cacheName != want) {
                againWrong++;
                if (printed++ < 5)
                    std::fprintf(stderr, "  counterexample (%s, variant %d): asked for again, not today's file word for word\n", fx.first.c_str(), variant);
            }
        }
    }
    // chips with no compiler: refused by name, nothing filed
    const size_t filedBefore = stageFiles(ram).size();
    boot(other, "");
    for (uint32_t chip : {0u, 0x06030501u}) {
        for (size_t i = 0; i < fixtures.size(); i += 7) {
            std::unique_ptr<CompiledStage> s = compileStageForChip(fixtures[i].second.data(), fixtures[i].second.size(), chip, &why);
            CHECK(s == nullptr || s->cacheName.empty());
            if (s == nullptr) CHECK(why == "ir3 has no compiler for the requested chip");
        }
    }
    CHECK(stageFiles(other).empty());
    std::fprintf(stderr, "  stage cache property 2 (redwood): %d stages compiled (%d refused) from %zu fixtures, %zu files; %d names not today's, "
                         "%d files not today's format, %d not served again word for word\n",
                 compiled, refused, fixtures.size(), filedBefore, nameWrong, fileWrong, againWrong);
    CHECK(compiled >= 80);
    CHECK_EQ(nameWrong, 0);
    CHECK_EQ(fileWrong, 0);
    CHECK_EQ(againWrong, 0);
    boot("", "");
    removeTree(base);
}

// The A650's first-ever compiles: with a cache directory configured (empty) the stage is the one compiled without any
// cache, word for word, and the exploration stages' digests are the ones observed. **Validates: Requirements 3.2, 3.6**
TEST(stage_cache_property_2_preservation_a650_first_ever_compiles) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: the A650 first compiles did not run)\n");
        return;
    }
    const auto fixtures = stageFixtures();
    std::mt19937 rng(0x06050002u);
    int compared = 0, refusedBoth = 0, differ = 0, printed = 0;
    std::string why, whyCached;
    for (const auto& fx : fixtures) {
        const Variant v = rng() % 2 == 0 ? Variant{} : randomVariant(rng);
        const KnownBuffer* known = v.hasKnown ? &v.known : nullptr;
        boot("", "");
        std::unique_ptr<CompiledStage> plain =
            compileStageForChip(fx.second.data(), fx.second.size(), a6xx::kA650ChipId, &why, v.constants, v.iboBase, known);
        char base[] = "/tmp/hakios-metal-stage-cache-XXXXXX";
        CHECK(mkdtemp(base) != nullptr);
        const std::string ram = makeDir(base, "ram"), seed = makeDir(base, "seed");
        boot(ram, seed);
        std::unique_ptr<CompiledStage> first =
            compileStageForChip(fx.second.data(), fx.second.size(), a6xx::kA650ChipId, &whyCached, v.constants, v.iboBase, known);
        boot("", "");
        removeTree(base);
        if (plain == nullptr && first == nullptr) {
            refusedBoth++;
            CHECK(why == whyCached);
            continue;
        }
        compared++;
        const bool same = plain != nullptr && first != nullptr && first->cacheSource == 0 && plain->words == first->words &&
                          plain->immediates == first->immediates && plain->varyingNames == first->varyingNames &&
                          plain->resources.size() == first->resources.size() && plain->iboBase == first->iboBase &&
                          plain->writesMemory == first->writesMemory && plain->digest == first->digest && plain->chipId == first->chipId;
        if (!same) {
            differ++;
            if (printed++ < 5) std::fprintf(stderr, "  counterexample (%s): the first-ever A650 compile with a cache configured differs from the one without\n", fx.first.c_str());
        }
    }
    // the exploration stages, against the digests observed on the code before the fix
    std::vector<StageAsk> rq = requests();
    int goldenWrong = 0;
    for (size_t i = 0; i < rq.size(); i++) {
        boot("", "");
        std::unique_ptr<CompiledStage> s = compileA650(rq[i], &why);
        CHECK(s != nullptr);
        if (s == nullptr) continue;
        std::fprintf(stderr, "  observed A650 digest: %s 0x%016llx (%zu words)\n", rq[i].name, (unsigned long long)s->digest, s->words.size());
        CHECK(strcmp(kA650Digests[i].name, rq[i].name) == 0);
        if (kA650Digests[i].digest != 0 && s->digest != kA650Digests[i].digest) {
            goldenWrong++;
            std::fprintf(stderr, "  counterexample (%s): digest 0x%016llx, observed before the fix 0x%016llx\n", rq[i].name, (unsigned long long)s->digest,
                         (unsigned long long)kA650Digests[i].digest);
        }
    }
    std::fprintf(stderr, "  stage cache property 2 (A650 first compiles): %d compared (%d refused on both), %d differ, %d digests not the observed\n",
                 compared, refusedBoth, differ, goldenWrong);
    CHECK(compared >= 40);
    CHECK_EQ(differ, 0);
    CHECK_EQ(goldenWrong, 0);
}
