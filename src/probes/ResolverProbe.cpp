// Throwaway: does the candidate resolver see every shader, and does every chain
// resolve to files that exist? Nothing else in the suite covers the catalogue.
#include <QCoreApplication>
#include <QFileInfo>
#include <QTextStream>
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

    int presets = 0, singles = 0, incomplete = 0, emptyId = 0;
    for (const Candidate& c : all) {
        if (c.kind == Kind::Preset) { ++presets; } else { ++singles; }
        if (c.id.isEmpty()) { ++emptyId; }
        if (!chainIsComplete(c, dir)) {
            ++incomplete;
            std::fprintf(stderr, "  INCOMPLETE %s -> %s\n", qPrintable(c.id),
                         qPrintable(c.files.join(QLatin1Char(','))));
        }
    }
    std::fprintf(stderr, "presets=%d singles=%d incomplete=%d emptyId=%d\n", presets, singles,
                 incomplete, emptyId);

    // round trip: every id must resolve back to itself
    int roundTripFail = 0;
    for (const Candidate& c : all) {
        const QStringList back = filesForId(c.id, dir);
        if (c.id != QLatin1String("off") && back.isEmpty()) {
            ++roundTripFail;
            std::fprintf(stderr, "  NO RESOLVE %s\n", qPrintable(c.id));
        }
    }
    std::fprintf(stderr, "roundTripFail=%d\n", roundTripFail);

    // the chain string must be non-empty for a real candidate
    const QString chain = shaderStringForCandidate(QStringLiteral("hq_ca"), dir);
    std::fprintf(stderr, "hq_ca chain parts: %d\n", chain.split(QLatin1Char(';')).size());

    // a single shader resolves to itself
    const QString one = shaderStringForCandidate(QStringLiteral("Anime4K_Restore_GAN_UUL.glsl"), dir);
    std::fprintf(stderr, "single chain: %s\n", qPrintable(one));

    const bool ok = incomplete == 0 && emptyId == 0 && roundTripFail == 0 &&
                    presets == 10 && singles > 30 && !chain.isEmpty() && !one.isEmpty();
    std::fprintf(stderr, "RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
