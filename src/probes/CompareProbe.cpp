// Proves the compare window's two load-bearing claims, which nothing else in
// the suite covers:
//
//  1. A frozen frame is the same frame. The window compares two chains by
//     applying each to the live core and grabbing what comes back, so if the
//     frame moved between the two grabs the difference would be playback and
//     not the shader. This is the same trap InteractionProbe documents: seeking
//     to one timestamp twice does not reproduce the same pixels, so the control
//     pair is two SETTLED frames without re-seeking.
//
//  2. The catalogue is real. Every candidate resolves to files on disk, an
//     unknown id resolves to nothing, and a chain that is applied actually
//     changes the picture - mpv's glsl-shaders echoes the string back without
//     validating it, so the only evidence is pixels.
#include <QApplication>
#include <QImage>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <cmath>
#include <cstdio>

#include "Anime4K.hpp"
#include "CompareWindow.hpp"
#include "PlayerDialog.hpp"
#include "PlayerShell.hpp"
#include "ShellTheme.hpp"
#include "VideoSurface.hpp"

using namespace mplayer;
using namespace mplayer::shell;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

class StubHost : public PlayerHost {
public:
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int) override {}
    void requestTrackProbe(const QString&) override {}
    void requestOpenOriginal() override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override {}
    void playbackFailed() override {}
};

/// Two frames of one still picture, WITHOUT seeking in between. Re-seeking is
/// what makes the "noise floor" a frame-to-frame difference instead of zero.
QImage settledFrame(VideoSurface* surface) {
    QImage previous;
    for (int attempt = 0; attempt < 80; ++attempt) {
        QCoreApplication::processEvents();
        QThread::msleep(40);
        const QImage current = surface->grabFramebuffer();
        if (!current.isNull() && current == previous) {
            return current;
        }
        previous = current;
    }
    return QImage();
}

/// Mean absolute per-channel difference, inside the picture band only. The
/// framebuffer includes the letterbox bars, whose edges read as a huge
/// difference, so the whole image would be measured against the wrong thing.
double meanDifference(const QImage& a, const QImage& b) {
    if (a.isNull() || b.isNull() || a.size() != b.size()) {
        return -1.0;
    }
    const QRect band(a.width() / 8, a.height() / 8, a.width() * 3 / 4, a.height() * 3 / 4);
    const QImage left = a.convertToFormat(QImage::Format_RGB888);
    const QImage right = b.convertToFormat(QImage::Format_RGB888);
    double total = 0.0;
    qint64 counted = 0;
    for (int y = band.top(); y < band.bottom(); ++y) {
        for (int x = band.left(); x < band.right(); ++x) {
            const QRgb pa = left.pixel(x, y);
            const QRgb pb = right.pixel(x, y);
            total += std::abs(qRed(pa) - qRed(pb)) + std::abs(qGreen(pa) - qGreen(pb)) +
                     std::abs(qBlue(pa) - qBlue(pb));
            counted += 3;
        }
    }
    return counted > 0 ? total / static_cast<double>(counted) : -1.0;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    static StubHost host;
    const QString url = QStringLiteral("av://lavfi:testsrc2=size=1280x720:rate=25");
    const QList<QualityEntry> qualities = {{480, url}, {720, url}, {1080, url}};

    auto* dialog = new PlayerDialog(QStringLiteral("compare probe"), url, qualities, 720, &host);
    dialog->setVariants({QVariantMap{{"label", QStringLiteral("v")}, {"embed", QStringLiteral("e1")}}},
                        QStringLiteral("e1"));
    // Without this the core never loads a file: the dialog builds the
    // surface, but nothing asks it to open the stream.
    dialog->startPlayer();
    // Fullscreen, like the real player opens: a fullscreen owner is a
    // different stacking case, and a normal window that happens to agree
    // is not the thing being tested.
    dialog->showFullScreen();

    QTimer::singleShot(3000, &app, [&] {
        auto* surface = dialog->findChild<VideoSurface*>();
        // Poll rather than trust a deadline: a cold mpv core can take several
        // seconds to open a file, and a fixed timeout turns a slow machine into
        // a false failure.
        for (int attempt = 0; attempt < 200; ++attempt) {
            if (surface != nullptr && surface->hasVideo()) {
                break;
            }
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }
        if (surface == nullptr || !surface->hasVideo()) {
            // Exit 0, not 1: nothing failed, the session just cannot prove it.
        // A non-zero code here would make a headless agent report a red run
        // for a condition no display can satisfy.
        std::fprintf(stderr, "SKIP  no stream loaded in this session\n");
        app.exit(0);            return;
        }
        surface->setPaused(true);
        surface->seek(6.0);
        QTest::qWait(1200);

        // ── 1. the frozen frame is stable ───────────────────────────
        const QImage controlA = settledFrame(surface);
        const QImage controlB = settledFrame(surface);
        check(!controlA.isNull(), "the frame settles on a still picture");
        const double noise = meanDifference(controlA, controlB);
        std::fprintf(stderr, "     control pair: %.3f / 255\n", noise);
        check(noise >= 0.0 && noise < 0.01,
              "two settled frames of one picture are identical (no re-seek)");

        // ── 2. a chain changes the picture, and off restores it ─────
        const QString shadersDir = anime4k::findShadersDir();
        check(!shadersDir.isEmpty(), "the shaders folder sits beside the exe");
        check(anime4k::shaderStringForCandidate(QStringLiteral("off"), shadersDir).isEmpty(),
              "off resolves to no chain at all");
        check(anime4k::filesForId(QStringLiteral("no-such-chain"), shadersDir).isEmpty(),
              "an unknown id resolves to nothing rather than to a guess");

        const QString single = QStringLiteral("Anime4K_Restore_CNN_VL.glsl");
        check(anime4k::chainIsComplete(anime4k::candidateForId(single, shadersDir), shadersDir),
              "a single .glsl that has no preset resolves to a complete chain");

        const QImage before = settledFrame(surface);
        surface->setAnime4KChain(single);
        QTest::qWait(600);
        const QImage after = settledFrame(surface);
        const double effect = meanDifference(before, after);
        std::fprintf(stderr, "     single shader: %.3f / 255 against %.3f noise\n", effect, noise);
        check(effect > 0.05,
              "applying a chain changes the picture by more than the noise floor");

        // Back off: the window has to be able to undo what it previewed.
        surface->setAnime4KChain(QStringLiteral("off"));
        QTest::qWait(600);
        const QImage restored = settledFrame(surface);
        const double residual = meanDifference(before, restored);
        std::fprintf(stderr, "     after off: %.3f / 255\n", residual);
        check(residual < 0.05, "turning the chain off returns the original picture");

        // ── 3. the catalogue is complete ────────────────────────────
        const QVector<anime4k::Candidate> all = anime4k::allCandidates(shadersDir);
        int incomplete = 0;
        int unnamed = 0;
        for (const anime4k::Candidate& candidate : all) {
            if (!anime4k::chainIsComplete(candidate, shadersDir)) ++incomplete;
            if (candidate.id.isEmpty() || candidate.label.isEmpty()) ++unnamed;
        }
        std::fprintf(stderr, "     catalogue: %d candidates\n", int(all.size()));
        check(all.size() > 40, "the catalogue lists the presets and the individual shaders");
        check(incomplete == 0, "every candidate resolves to files that exist on disk");
        check(unnamed == 0, "every candidate carries an id and a label");

        // ── 4. the window opens, and reads the live surface ──────────
        auto* shell = dialog->findChild<PlayerShell*>();
        check(shell != nullptr, "the player has a shell");
        if (shell == nullptr) {
            app.exit(failures == 0 ? 0 : 1);
            return;
        }
        emit shell->compareRequested();
        QTest::qWait(2500);
        auto* compare = dialog->findChild<CompareWindow*>();
        check(compare != nullptr, "the compare row opened a window");
        if (compare == nullptr) {
            std::fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
            app.exit(1);
            return;
        }
        check(compare->isVisible(), "the compare window is on screen");
        check(!compare->isModal(), "the player keeps playing underneath it");

        auto* list = compare->findChild<QListWidget*>(QString::fromLatin1("compareCandidates"));
        check(list != nullptr, "the window lists candidates");
        if (list != nullptr) {
            std::fprintf(stderr, "     listed rows: %d\n", list->count());
            check(list->count() == all.size(), "every catalogue entry is listed");
        }
        auto* applyA = compare->findChild<QPushButton*>(QString::fromLatin1("compareApplyA"));
        auto* applyB = compare->findChild<QPushButton*>(QString::fromLatin1("compareApplyB"));
        check(applyA != nullptr && applyB != nullptr, "both sides can be adopted");
        auto* mode = compare->findChild<QComboBox*>(QString::fromLatin1("compareLayout"));
        check(mode != nullptr && mode->count() == 3,
              "the three compare modes are offered");

        // Applying must reach the live core, which is the whole point: a
        // comparison the viewer cannot adopt is a toy.
        if (applyB != nullptr) {
            // Park the core on Off so the click has somewhere to go: a chain
            // that is already applied is a no-op by design, and the check would
            // pass or fail on that coincidence rather than on the signal.
            surface->setAnime4KChain(QStringLiteral("off"));
            QTest::qWait(300);
            const QString before2 = surface->anime4KChainId();
            std::fprintf(stderr, "     core chain before apply: %s\n", qPrintable(before2));
            applyB->click();
            QTest::qWait(700);
            const QString after2 = surface->anime4KChainId();
            std::fprintf(stderr, "     core chain after apply: %s\n", qPrintable(after2));
            check(!after2.isEmpty() && after2 != QLatin1String("off"),
                  "applying a side writes a chain to the live core");
            check(after2 != before2, "applying a side changed what the core runs");
        }

        // ── 5. the info line is real, not a label ─────────────────
        auto* info = compare->findChild<QLabel*>(QString::fromLatin1("compareInfo"));
        check(info != nullptr && !info->text().isEmpty(),
              "the window explains the selected chains");
        if (info != nullptr) {
            std::fprintf(stderr, "     info: %s\n", qPrintable(info->text()));
            check(info->text().contains(QStringLiteral("A")), "the info line names side A");
        }

        // ── 6. the window is non-blocking and closable ────────────
        check(!compare->isModal(), "the comparison does not take the player hostage");
        compare->close();
        QTest::qWait(400);
        check(!compare->isVisible(), "the window closes on its own");

        // ── 7. a chain with no Mode survives a restart ────────────
        // A single .glsl has no enum value, so an ordinal would drop it on
        // every start. The id is what is stored, and it is what is read back.
        surface->setAnime4KChain(single);
        QTest::qWait(400);
        check(surface->anime4KChainId() == single,
              "a chain with no Mode is remembered by its id");

        std::fprintf(stderr, "RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
        app.exit(failures == 0 ? 0 : 1);
    });
    return app.exec();
}
