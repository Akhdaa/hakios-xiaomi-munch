// hm-seed: the A650's compiled-stage seed, built on the host from a phone run's log (metal-ca-perf, design 1.5).
//
//   make out/host/hm-seed MESA=1 MESA_BUILD=<host Mesa build-native> CACHE_BUILD=<the phone's "stage cache:" build>
//   out/host/hm-seed -o SEED_DIR [-p PIPELINELIB_DIR] [-m LIBRARY.metallib]... [-b BC_DIR]... [--max-mib N] LOG...
//
//   LOG              a run's log (union.txt): its "stage cache:" line (build and layout, which must be this binary's) and
//                    its "stage miss:" lines (with their "stage miss: #N continued:" pieces)
//   -o SEED_DIR      an existing directory: each stage whose host digest equals the phone's goes in as <32 hex>.stage,
//                    to be put at /redwood/share/hakios-metal-cache (HAKIOS_METAL_CACHE_SEED) in the payload's small APFS
//   -p DIR           QuartzCore's default.pipelinelib (its "functions" and "pipelines.desc", taken from the resident
//                    system image with tools/apfs/apfs.py cat)
//   -m FILE          a .metallib whose functions the plug-in compiles (the merged QuartzCore-default.metallib)
//   -b DIR           a directory of .bc modules (the driver's fixture kernels, out/host/kernels)
//   --max-mib N      refuse (exit 3) when the seed comes to more than N MiB (default 32)
//
// Prints what came of each distinct request and the counts: found, compiled, equal (seeded), different, not found, and
// the seed's size. Exit 0; 1 a refusal (another build or layout, no "stage cache:" line, unreadable input); 2 usage;
// 3 over the size limit (the files are left for a look; the caller removes them).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hm_seed.hpp"

using namespace hakios::metal;

namespace {
int usage() {
    fprintf(stderr, "usage: hm-seed -o SEED_DIR [-p PIPELINELIB_DIR] [-m LIBRARY.metallib]... [-b BC_DIR]... [--max-mib N] LOG...\n");
    return 2;
}
}  // namespace

int main(int argc, char** argv) {
    std::string out, pipelinelib;
    std::vector<std::string> metallibs, bcDirs, logs;
    unsigned long maxMib = 32;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "-o" && (v = value())) out = v;
        else if (a == "-p" && (v = value())) pipelinelib = v;
        else if (a == "-m" && (v = value())) metallibs.push_back(v);
        else if (a == "-b" && (v = value())) bcDirs.push_back(v);
        else if (a == "--max-mib" && (v = value())) maxMib = strtoul(v, nullptr, 10);
        else if (!a.empty() && a[0] == '-') return usage();
        else logs.push_back(a);
    }
    if (out.empty() || logs.empty()) return usage();
    while (out.size() > 1 && out.back() == '/') out.pop_back();

    std::string text;
    for (const std::string& path : logs) {
        std::vector<uint8_t> b;
        if (!seed::readFile(path, &b)) {
            fprintf(stderr, "hm-seed: cannot read %s\n", path.c_str());
            return 1;
        }
        text.append(reinterpret_cast<const char*>(b.data()), b.size());
        text += '\n';
    }
    const seed::Log log = seed::parseLog(text);
    std::string why;
    printf("hm-seed: this binary's cache build %s; the log: %zu \"stage cache:\" line(s), %zu misses (%u duplicates, %u unreadable, %u unfinished)\n",
           stageCacheBuild(), log.caches.size(), log.misses.size(), log.duplicates, log.unreadable, log.unfinished);
    for (const seed::CacheLine& c : log.caches) printf("  stage cache: %s build %s layout %s\n", c.who.c_str(), c.build.c_str(), c.layout.c_str());
    for (const std::string& n : log.notes) printf("  %s\n", n.c_str());
    if (!seed::checkBuild(log, stageCacheBuild(), &why)) {
        fprintf(stderr, "hm-seed: refused: %s\n", why.c_str());
        return 1;
    }

    seed::BitcodeIndex index;
    if (!pipelinelib.empty()) {
        const int n = index.addPipelinelib(pipelinelib, &why);
        if (n < 0) {
            fprintf(stderr, "hm-seed: %s\n", why.c_str());
            return 1;
        }
        printf("  bitcode: %d modules of the pipeline library %s\n", n, pipelinelib.c_str());
    }
    for (const std::string& m : metallibs) {
        const int n = index.addMetallib(m, &why);
        if (n < 0) {
            fprintf(stderr, "hm-seed: %s\n", why.c_str());
            return 1;
        }
        printf("  bitcode: %d functions of %s\n", n, m.c_str());
    }
    for (const std::string& d : bcDirs) printf("  bitcode: %u modules in %s\n", index.addBcDir(d), d.c_str());
    printf("  bitcode: %zu distinct by SHA-256\n", index.size());

    seed::Report r;
    if (!seed::seedStages(log, index, out, a6xx::kA650ChipId, &r, &why)) {
        fprintf(stderr, "hm-seed: %s\n", why.c_str());
        return 1;
    }
    for (const std::string& l : r.lines) printf("  %s\n", l.c_str());
    printf("hm-seed: %u misses, %u distinct requests: %u bitcode found, %u not found; %u compiled (%u refused); %u digest-equal (seeded, %u served back "
           "from the seed), %u different (left out); seed %llu bytes (%.2f MiB) in %s\n",
           r.misses, r.distinct, r.found, r.notFound, r.compiled, r.refused, r.equal, r.verified, r.different, (unsigned long long)r.bytes,
           r.bytes / 1048576.0, out.c_str());
    if (r.bytes > uint64_t(maxMib) * 1048576) {
        fprintf(stderr, "hm-seed: the seed is %llu bytes, more than %lu MiB\n", (unsigned long long)r.bytes, maxMib);
        return 3;
    }
    return r.verified == r.equal ? 0 : 1;
}
