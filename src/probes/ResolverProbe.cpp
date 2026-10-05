#include <QCoreApplication>
#include <cstdio>

#include "Anime4K.hpp"

using namespace mplayer::anime4k;

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString dir = findShadersDir();
    std::fprintf(stderr, "shaders dir: %s\n", qPrintable(dir));
    if (dir.isEmpty()) {
        std::fprintf(stderr, "RESULT: FAIL (no shaders dir)\n");
        return 1;
    }
    const QVector<Candidate> all = allCandidates();
    std::fprintf(stderr, "candidates: %d\n", int(all.size()));

    int presets = 0, singles = 0, incomplete = 0, emptyId = 0, internals = 0;
    for (const Candidate& c : all) {
        if (c.kind == Kind::Preset) { ++presets; } else { ++singles; }
        if (c.id.isEmpty()) { ++emptyId; }
        if (c.id.contains(QLatin1String("AutoDownscalePre"))) {
            ++internals;
            std::fprintf(stderr, "  STANDALONE INTERNAL %s\n", qPrintable(c.id));
        }
        if (!chainIsComplete(c, dir)) {
            ++incomplete;
            std::fprintf(stderr, "  INCOMPLETE %s -> %s\n", qPrintable(c.id),
                         qPrintable(c.files.join(QLatin1Char(','))));
        }
    }
    std::fprintf(stderr, "presets=%d singles=%d incomplete=%d emptyId=%d\n", presets, singles,
                 incomplete, emptyId);
    std::fprintf(stderr, "standalone internals=%d\n", internals);
    // Persisted preset ids must remain selectable when the catalogue grows.
    int missingBuiltins = 0;
    for (const char* id : {"off", "fast_a", "fast_b", "fast_c", "hq_a", "hq_b", "hq_c", "hq_aa",
                           "hq_bb", "hq_ca"}) {
        const QString key = QString::fromLatin1(id);
        const bool known = candidateForId(key, dir).id == key;
        if (!known) {
            ++missingBuiltins;
            std::fprintf(stderr, "  MISSING BUILTIN %s\n", id);
        }
    }
    std::fprintf(stderr, "missingBuiltins=%d\n", missingBuiltins);

    const bool ok = incomplete == 0 && emptyId == 0 && internals == 0 && missingBuiltins == 0;
    std::fprintf(stderr, "RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
