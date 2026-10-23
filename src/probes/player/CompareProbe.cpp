// Compares canonical raw input at the held timestamp while preserving exact
// processed live pixels; differences exclude letterboxing.
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QSpinBox>
#include <QWheelEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>

#include "Anime4K.hpp"
#include "CompareCanvas.hpp"
#include "ComparePane.hpp"
#include "CompareWindow.hpp"
#include "PlayerDialog.hpp"
#include "PlayerShell.hpp"
#include "ShellTheme.hpp"
#include "VideoSurface.hpp"
#include "native/components/AnimeJanaiConfig.hpp"
#include "ProbeRuntime.hpp"
#include "ProbePicture.hpp"

using namespace mplayer;
using namespace mplayer::shell;

namespace {

/// Diagnostics only. When enabled, every async playback-timestamp sample taken
/// from the raw/active Aji control section is mirrored to stderr together with
/// the requested epoch, the epoch observed at request time, whether the async
/// request was valid, whether the callback ran, and whether a timestamp value
/// arrived. Reporting never changes a check outcome.
bool timestampTrace = false;

unsigned timestampAttempt = 0;
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
        const QImage current = surface->grabVideoFramebuffer();
        if (!current.isNull() && current == previous) {
            return current;
        }
        previous = current;
    }
    return QImage();
}

struct PlaybackTimestampSample {
    uint64_t sourceEpoch = 0;
    std::optional<double> timestamp;
    bool completed = false;
};

bool requestPlaybackTimestamp(VideoSurface* surface, PlaybackTimestampSample* sample,
                              int timeoutMs = 5000) {
    if (surface == nullptr || sample == nullptr) return false;
    auto result = std::make_shared<PlaybackTimestampSample>();
    QEventLoop loop;
    QPointer<QEventLoop> loopGuard(&loop);
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    const uint64_t requestedEpoch = surface->sourceEpoch();
    const uint64_t epochAtRequest = surface->sourceEpoch();
    const AsyncRequestId request = surface->requestPlaybackTimestamp(
        [result, loopGuard](uint64_t sourceEpoch, std::optional<double> timestamp) {
            result->sourceEpoch = sourceEpoch;
            result->timestamp = timestamp;
            result->completed = true;
            if (loopGuard != nullptr) loopGuard->quit();
        });
    const bool requestValid = request.valid();
    if (!requestValid) {
        if (timestampTrace) {
            std::fprintf(stderr,
                         "TRACE timestamp attempt=%u requestedEpoch=%llu epochAtRequest=%llu "
                         "requestValid=0 callbackCompleted=0 hasTimestamp=0 timestamp=(none)\n",
                         ++timestampAttempt,
                         static_cast<unsigned long long>(requestedEpoch),
                         static_cast<unsigned long long>(epochAtRequest));
        }
        return false;
    }
    if (!result->completed) {
        timeout.start(timeoutMs);
        loop.exec();
    }
    *sample = *result;
    if (timestampTrace) {
        std::fprintf(stderr,
                     "TRACE timestamp attempt=%u requestedEpoch=%llu epochAtRequest=%llu "
                     "requestValid=1 callbackCompleted=%d hasTimestamp=%d timestamp=%s "
                     "replyEpoch=%llu epochNow=%llu\n",
                     ++timestampAttempt,
                     static_cast<unsigned long long>(requestedEpoch),
                     static_cast<unsigned long long>(epochAtRequest),
                     sample->completed ? 1 : 0, sample->timestamp.has_value() ? 1 : 0,
                     sample->timestamp.has_value()
                         ? qPrintable(QString::number(*sample->timestamp, 'f', 6))
                         : "(none)",
                     static_cast<unsigned long long>(sample->sourceEpoch),
                     static_cast<unsigned long long>(surface->sourceEpoch()));
    }
    return sample->completed && sample->timestamp.has_value();
}

bool playbackTimestampMatches(VideoSurface* surface, uint64_t sourceEpoch,
                              const std::optional<double>& expectedTimestamp) {
    PlaybackTimestampSample current;
    return expectedTimestamp.has_value() && requestPlaybackTimestamp(surface, &current) &&
           current.sourceEpoch == sourceEpoch &&
           std::abs(*current.timestamp - *expectedTimestamp) <= 1e-6;
}
bool waitForStablePlaybackTimestamp(VideoSurface* surface, uint64_t sourceEpoch,
                                    PlaybackTimestampSample* stable, int timeoutMs = 10000) {
    if (surface == nullptr || stable == nullptr) return false;
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        PlaybackTimestampSample first;
        if (!requestPlaybackTimestamp(surface, &first, 1000) ||
            first.sourceEpoch != sourceEpoch) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            continue;
        }
        QThread::msleep(150);
        PlaybackTimestampSample second;
        if (requestPlaybackTimestamp(surface, &second, 1000) &&
            second.sourceEpoch == sourceEpoch &&
            std::abs(*second.timestamp - *first.timestamp) <= 1e-6) {
            *stable = second;
            if (timestampTrace) {
                std::fprintf(stderr,
                             "TRACE stable-ok requestedEpoch=%llu epochNow=%llu timestamp=%.6f\n",
                             static_cast<unsigned long long>(sourceEpoch),
                             static_cast<unsigned long long>(surface->sourceEpoch()),
                             *second.timestamp);
            }
            return true;
        }
        if (timestampTrace) {
            std::fprintf(stderr,
                         "TRACE stable-retry requestedEpoch=%llu epochNow=%llu "
                         "firstEpoch=%llu firstTs=%s secondEpoch=%llu secondTs=%s\n",
                         static_cast<unsigned long long>(sourceEpoch),
                         static_cast<unsigned long long>(surface->sourceEpoch()),
                         static_cast<unsigned long long>(first.sourceEpoch),
                         qPrintable(first.timestamp.has_value()
                                        ? QString::number(*first.timestamp, 'f', 6)
                                        : QStringLiteral("(none)")),
                         static_cast<unsigned long long>(second.sourceEpoch),
                         qPrintable(second.timestamp.has_value()
                                        ? QString::number(*second.timestamp, 'f', 6)
                                        : QStringLiteral("(none)")));
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    if (timestampTrace) {
        std::fprintf(stderr,
                     "TRACE stable-timeout requestedEpoch=%llu epochNow=%llu\n",
                     static_cast<unsigned long long>(sourceEpoch),
                     static_cast<unsigned long long>(surface->sourceEpoch()));
    }
    return false;
}
bool waitForPlaybackProgress(VideoSurface* surface, uint64_t sourceEpoch,
                             const std::optional<double>& startingTimestamp,
                             PlaybackTimestampSample* progressed, int timeoutMs = 10000) {
    if (surface == nullptr || !startingTimestamp.has_value() || progressed == nullptr) {
        return false;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        PlaybackTimestampSample current;
        if (requestPlaybackTimestamp(surface, &current, 1000) &&
            current.sourceEpoch == sourceEpoch && current.timestamp.has_value() &&
            std::abs(*current.timestamp - *startingTimestamp) > 0.01) {
            *progressed = current;
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(50);
    }
    return false;
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

struct LiveShaderSnapshot {
    PlaybackTimestampSample playback;
    bool timestampAvailable = false;
    bool paused = false;
    bool playing = false;
    uint64_t outputFrames = 0;
    QString requestedId;
    QString effectiveChain;
    std::optional<ProbePropertyValue> coreChain;
};

LiveShaderSnapshot sampleLiveShaderState(VideoSurface* surface) {
    LiveShaderSnapshot sample;
    sample.timestampAvailable = requestPlaybackTimestamp(surface, &sample.playback);
    sample.paused = surface->pauseObserved();
    sample.playing = surface->isPlaying();
    sample.outputFrames = surface->playbackDiagnostics().renderedOutputFrames;
    sample.requestedId = surface->anime4KChainId();
    sample.effectiveChain = surface->effectiveShaderChain();
    sample.coreChain = surface->probeMpvProperty(QStringLiteral("glsl-shaders"),
                                               ProbePropertyKind::Text);
    return sample;
}

void reportLiveShaderSnapshot(const char* phase, const LiveShaderSnapshot& sample,
                              const QImage& image) {
    std::fprintf(stderr,
                 "TRACE live-shader phase=%s epoch=%llu timestamp=%s paused=%d playing=%d "
                 "framebuffer=%dx%d outputFrames=%llu requested=%s effective=%s core=%s\n",
                 phase, static_cast<unsigned long long>(sample.playback.sourceEpoch),
                 sample.timestampAvailable
                     ? qPrintable(QString::number(*sample.playback.timestamp, 'f', 6))
                     : "(unavailable)",
                 sample.paused, sample.playing, image.width(), image.height(),
                 static_cast<unsigned long long>(sample.outputFrames),
                 qPrintable(sample.requestedId), qPrintable(sample.effectiveChain),
                 sample.coreChain.has_value() ? qPrintable(sample.coreChain->text)
                                              : "(unavailable)");
}

bool sameLiveShaderTimestamp(const LiveShaderSnapshot& a, const LiveShaderSnapshot& b) {
    return a.timestampAvailable && b.timestampAvailable && a.paused && b.paused &&
           !a.playing && !b.playing && a.playback.sourceEpoch == b.playback.sourceEpoch &&
           std::abs(*a.playback.timestamp - *b.playback.timestamp) <= 1e-6;
}

/// The label path a catalogue category id reads as: one label per level of the
/// id, taken from the catalogue itself. The chooser is checked against the
/// category it claims rather than against a copy of its wording.
QStringList labelPath(const QString& categoryId) {
    QStringList path;
    QString prefix;
    for (const QString& part : categoryId.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        prefix = prefix.isEmpty() ? part : prefix + QLatin1Char('/') + part;
        const QString label = anime4k::categoryLabel(prefix);
        if (!label.isEmpty()) {
            path.append(label);
        }
    }
    return path;
}

bool triggerChoice(CompareWindow* compare, int side, const QString& id,
                  const std::function<void()>& beforeTrigger = {}) {
    int badgeIndex = 0;
    QToolButton* targetBadge = nullptr;
    for (QToolButton* badge : compare->findChildren<QToolButton*>()) {
        if (!badge->isVisible() || badge->menu() == nullptr) {
            continue;
        }
        if (badgeIndex++ == side) {
            targetBadge = badge;
            break;
        }
    }
    if (targetBadge == nullptr || targetBadge->menu() == nullptr) {
        return false;
    }

    // Emulate actual user mouse click on the badge button to open its menu
    QTest::mouseClick(targetBadge, Qt::LeftButton, Qt::NoModifier, targetBadge->rect().center());
    QCoreApplication::processEvents();
    if (!targetBadge->menu()->isVisible()) {
        targetBadge->menu()->popup(targetBadge->mapToGlobal(QPoint(0, targetBadge->height())));
        QCoreApplication::processEvents();
    }

    std::function<bool(QMenu*)> trigger = [&](QMenu* menu) {
        for (QAction* action : menu->actions()) {
            if (QMenu* submenu = action->menu()) {
                const QRect actGeom = menu->actionGeometry(action);
                QTest::mouseMove(menu, actGeom.center());
                action->hover();
                QCoreApplication::processEvents();
                if (!submenu->isVisible()) {
                    submenu->popup(menu->mapToGlobal(QPoint(actGeom.right(), actGeom.top())));
                    QCoreApplication::processEvents();
                }
                if (trigger(submenu)) return true;
            } else if (action->data().toString() == id) {
                if (beforeTrigger) beforeTrigger();
                QTimer renderWatchdog;
                if (id.startsWith(QLatin1String("spatial:"))) {
                    renderWatchdog.setSingleShot(true);
                    QObject::connect(&renderWatchdog, &QTimer::timeout, compare, [compare, id] {
                        std::fprintf(stderr,
                                     "FAIL %s render watchdog: still pending after 600000 ms\n",
                                     qPrintable(id));
                        compare->close();
                    });
                    renderWatchdog.start(600000);
                }
                QPointer<QToolButton> badgeGuard(targetBadge);
                if (!menu->isVisible()) {
                    menu->popup(targetBadge->mapToGlobal(QPoint(0, targetBadge->height())));
                    QCoreApplication::processEvents();
                }
                const QRect geom = menu->actionGeometry(action);
                std::fprintf(stderr, "MOUSE_CLICK: action %s at (%d, %d)\n",
                             qPrintable(id), geom.center().x(), geom.center().y());
                QTest::mouseMove(menu, geom.center());
                QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, geom.center());
                renderWatchdog.stop();
                if (!badgeGuard.isNull()) {
                    if (QMenu* badgeMenu = badgeGuard->menu()) {
                        badgeMenu->hide();
                    }
                    QCoreApplication::processEvents();
                }
                return true;
            }
        }
        return false;
    };
    QPointer<QToolButton> selectedBadge(targetBadge);
    QMenu* selectedMenu = targetBadge->menu();
    const bool selected = trigger(selectedMenu);
    if (!selectedBadge.isNull() && selectedBadge->menu() != nullptr) {
        selectedBadge->menu()->hide();
        QCoreApplication::processEvents();
    }
    return selected;
}

/// How many side panes the side-by-side layout currently has. A refused
/// capture leaves none, which is why a selection afterwards cannot be made:
/// the badge chooser lives in the pane.
qsizetype visibleComparePanes(CompareWindow* compare) {
    qsizetype visible = 0;
    for (ComparePane* pane : compare->findChildren<ComparePane*>()) {
        if (pane->isVisible()) ++visible;
    }
    return visible;
}

bool visibleSideMatches(CompareWindow* compare, int side, const QString& id,
                        const QImage& output, VideoSurface* surface) {
    QList<ComparePane*> visible;
    for (ComparePane* pane : compare->findChildren<ComparePane*>()) {
        if (pane->isVisible()) visible.append(pane);
    }
    if (side < 0 || side >= visible.size() || output.isNull()) return false;
    ComparePane* pane = visible.at(side);
    bool exactPixmap = false;
    for (QGraphicsItem* item : pane->scene()->items()) {
        if (auto* pixmap = dynamic_cast<QGraphicsPixmapItem*>(item))
            exactPixmap |= pixmap->pixmap().toImage().convertToFormat(QImage::Format_RGBA8888) ==
                           output.convertToFormat(QImage::Format_RGBA8888);
    }
    bool checkedId = false;
    std::function<void(QMenu*)> scan = [&](QMenu* menu) {
        for (QAction* action : menu->actions()) {
            if (action->menu()) scan(action->menu());
            else if (action->isChecked() && action->data().toString() == id) checkedId = true;
        }
    };
    for (QToolButton* badge : pane->findChildren<QToolButton*>()) {
        if (badge->isVisible() && badge->menu()) scan(badge->menu());
    }
    const auto picture = mplayer::probes::measurePicture(
        pane->viewport()->grab().toImage(), surface->videoWidth(), surface->videoHeight());
    return pane->key() == id && exactPixmap && checkedId && !picture.isMostlyBlack();
}

bool waitForRenderedSide(CompareWindow* compare, const QString& id, int side,
                         QImage* result, QString* failure) {
    // A cold TensorRT engine may take minutes. Pump the GUI event loop during
    // preparation, but make lack of forward progress a named hard failure.
    constexpr int watchdogMs = 600000;
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < watchdogMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (!compare->candidateError(id).isEmpty()) {
            if (failure != nullptr) *failure = compare->candidateError(id);
            return false;
        }
        if (!compare->isVisible()) {
            if (failure != nullptr) *failure = QStringLiteral("comparison closed before output");
            return false;
        }
        const QImage image = side == 0 ? compare->renderedSideA() : compare->renderedSideB();
        if (!compare->comparisonPending()) {
            if (!image.isNull()) {
                if (result != nullptr) *result = image;
                return true;
            }
            if (failure != nullptr) {
                *failure = QStringLiteral("candidate completed without a rendered side image");
            }
            return false;
        }
        QThread::msleep(20);
    }
    if (failure != nullptr) {
        *failure = compare->comparisonPending()
                       ? QStringLiteral("still pending after 600000 ms")
                       : QStringLiteral("no rendered side image after 600000 ms");
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // Both scopes are redirected into a directory that dies with this process
    // before the first PlayerDialog, whose constructor reads player/anime4k and
    // player/presetShortcuts. Without it a probe inherits a real session.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiCompareProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("CompareProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }

    timestampTrace = !qEnvironmentVariableIsEmpty("COMPARE_PROBE_TRACE_TIMESTAMPS");
    if (timestampTrace) {
        std::fprintf(stderr, "TRACE timestamp diagnostics enabled\n");
    }
    const bool ajiMode = argc > 1 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--aji");
    if (ajiMode && argc != 3) {
        std::fprintf(stderr, "usage: compare_probe.exe --aji <video>\n");
        return 2;
    }
    if (ajiMode) {
        std::fprintf(stderr, "CLI: compare_probe --aji <video>\n");
    }
    static StubHost host;
    const QString url = ajiMode ? QString::fromLocal8Bit(argv[2])
                                : (argc > 1 ? QString::fromLocal8Bit(argv[1])
                                            : QStringLiteral("av://lavfi:testsrc2=size=1280x720:rate=25"));
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
        const int sourceWatchdogMs = ajiMode ? 600000 : 10000;
        for (int waited = 0; waited < sourceWatchdogMs; waited += 50) {
            if (surface != nullptr && surface->hasVideo()) {
                break;
            }
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }
        if (surface == nullptr || !surface->hasVideo()) {
            if (ajiMode) {
                std::fprintf(stderr,
                             "FAIL --aji source did not load within %d ms: video=%s, surface=%d\n",
                             sourceWatchdogMs, qPrintable(url), surface != nullptr);
                app.exit(1);
                return;
            }
            // Exit 0, not 1: nothing failed, the session just cannot prove it.
            // A non-zero code here would make a headless agent report a red run
            // for a condition no display can satisfy.
            std::fprintf(stderr, "SKIP  no stream loaded in this session\n");
            app.exit(0);
            return;
        }
        surface->setPaused(true);
        surface->seek(6.0);
        QTest::qWait(1200);

        QStringList publicSpatialIds;
        if (ajiMode) {
            PlaybackTimestampSample initialStableTimestamp;
            const bool initiallyStable = waitForStablePlaybackTimestamp(
                surface, surface->sourceEpoch(), &initialStableTimestamp);
            if (!initiallyStable || surface->isPlaying()) {
                PlaybackTimestampSample latest;
                const bool latestAvailable = requestPlaybackTimestamp(surface, &latest);
                std::fprintf(stderr,
                             "FAIL --aji initial pause did not settle: epoch=%llu, timestamp=%s, "
                             "playingFlag=%d, position=%.6f, hasVideo=%d, status=%s\n",
                             static_cast<unsigned long long>(surface->sourceEpoch()),
                             latestAvailable ? qPrintable(QString::number(*latest.timestamp, 'f', 6))
                                             : "(unavailable)",
                             surface->isPlaying(), surface->position(), surface->hasVideo(),
                             qPrintable(surface->animeJanaiStatus()));
                app.exit(1);
                return;
            }
        }
        native::AnimeJanaiPlan originalAjiPlan;
        QString originalAjiId;
        QImage ajiSourceBaseline;
        QString shaderBeforeAji;
        const native::Backend ajiBackend = native::Backend::TensorRT;

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
        // Everything the probe runs is read out of the catalogue: the presets,
        // the id that disables the chain, and the loose shader the live checks
        // apply. A name typed here would be a second copy of that contract - the
        // copy that goes stale the moment the catalogue changes.
        const QVector<anime4k::ModeInfo> presets = anime4k::availableModes();
        QStringList presetIds;
        QStringList selectablePresetIds;
        presetIds.reserve(presets.size());
        for (const anime4k::ModeInfo& preset : presets) {
            presetIds.append(preset.id);
            if (anime4k::chainIsComplete(anime4k::candidateForId(preset.id, shadersDir),
                                         shadersDir)) {
                selectablePresetIds.append(preset.id);
            }
        }
        const QVector<anime4k::Candidate> all = anime4k::allCandidates(shadersDir);
        const QString offId = anime4k::modeId(anime4k::Mode::Off);

        check(anime4k::shaderStringForCandidate(offId, shadersDir).isEmpty(),
              "off resolves to no chain at all");
        check(anime4k::filesForId(QStringLiteral("no-such-chain"), shadersDir).isEmpty(),
              "an unknown id resolves to nothing rather than to a guess");

        // A loose restoration shader: one the catalogue offers as a file of its
        // own rather than inside a preset chain. Its id is the file name, and
        // that is the value which has to reach the player and come back below.
        QString single;
        for (const anime4k::Candidate& candidate : all) {
            if (presetIds.contains(candidate.id) || candidate.kind != anime4k::Kind::Restore) {
                continue;
            }
            if (anime4k::chainIsComplete(candidate, shadersDir)) {
                single = candidate.id;
                break;
            }
        }
        check(!single.isEmpty(),
              "the catalogue offers a loose restoration shader that has no preset");

        const QImage before = settledFrame(surface);
        const LiveShaderSnapshot beforeState = sampleLiveShaderState(surface);
        const QImage beforeConfirmed = timestampTrace ? settledFrame(surface) : QImage();
        surface->setAnime4KChain(single);
        QTest::qWait(600);
        const QImage after = settledFrame(surface);
        const LiveShaderSnapshot afterState = sampleLiveShaderState(surface);
        const double effect = meanDifference(before, after);
        std::fprintf(stderr, "     single shader: %.3f / 255 against %.3f noise\n", effect, noise);
        check(effect > 0.05 && sameLiveShaderTimestamp(beforeState, afterState),
              "applying a chain changes the picture by more than the noise floor");

        surface->setAnime4KChain(offId);
        QTest::qWait(600);
        const QImage restored = settledFrame(surface);
        const LiveShaderSnapshot restoredState = sampleLiveShaderState(surface);
        const double residual = meanDifference(before, restored);
        std::fprintf(stderr, "     after off: %.3f / 255\n", residual);
        const bool restoresOriginal = residual >= 0.0 && residual < 0.05 &&
                                      sameLiveShaderTimestamp(beforeState, restoredState);
        check(restoresOriginal, "turning the chain off returns the original picture");
        if (timestampTrace || !restoresOriginal) {
            reportLiveShaderSnapshot("before", beforeState, before);
            reportLiveShaderSnapshot("active", afterState, after);
            reportLiveShaderSnapshot("off", restoredState, restored);
            if (!beforeConfirmed.isNull()) {
                std::fprintf(stderr, "TRACE live-shader initial-to-confirmed=%.6f / 255\n",
                             meanDifference(before, beforeConfirmed));
            }
        }
        if (!restoresOriginal) {
            // Keep the first failure; a later image diagnoses it, never replaces it.
            QTest::qWait(1200);
            const QImage delayedOff = settledFrame(surface);
            const LiveShaderSnapshot delayedState = sampleLiveShaderState(surface);
            reportLiveShaderSnapshot("off-delayed", delayedState, delayedOff);
            std::fprintf(stderr, "TRACE live-shader initial-to-delayed=%.6f "
                                 "off-to-delayed=%.6f / 255\n",
                         meanDifference(before, delayedOff), meanDifference(restored, delayedOff));
            before.save(QStringLiteral(".probe-out/compare-live-before.png"));
            after.save(QStringLiteral(".probe-out/compare-live-active.png"));
            restored.save(QStringLiteral(".probe-out/compare-live-off.png"));
            delayedOff.save(QStringLiteral(".probe-out/compare-live-off-delayed.png"));

            surface->setAnime4KChain(single);
            QTest::qWait(600);
            const QImage repeatedActive = settledFrame(surface);
            const LiveShaderSnapshot repeatedActiveState = sampleLiveShaderState(surface);
            surface->setAnime4KChain(offId);
            QTest::qWait(600);
            const QImage repeatedOff = settledFrame(surface);
            const LiveShaderSnapshot repeatedOffState = sampleLiveShaderState(surface);
            reportLiveShaderSnapshot("active-repeat", repeatedActiveState, repeatedActive);
            reportLiveShaderSnapshot("off-repeat", repeatedOffState, repeatedOff);
            std::fprintf(stderr, "TRACE live-shader repeated-effect=%.6f "
                                 "repeated-residual=%.6f / 255 sameTimestamp=%d\n",
                         meanDifference(delayedOff, repeatedActive),
                         meanDifference(delayedOff, repeatedOff),
                         sameLiveShaderTimestamp(delayedState, repeatedOffState));
        }

        QStringList incompleteCandidates;
        for (const anime4k::Candidate& candidate : all) {
            if (!anime4k::chainIsComplete(candidate, shadersDir)) {
                incompleteCandidates.append(candidate.id);
            }
        }
        std::fprintf(stderr, "     catalogue: %d candidates, %d incomplete\n", int(all.size()),
                     int(incompleteCandidates.size()));
        for (const QString& id : incompleteCandidates) {
            std::fprintf(stderr, "     unavailable shader candidate: %s\n", qPrintable(id));
        }

        // ── 4. the window opens, and reads the live surface ──────────
        auto* shell = dialog->findChild<PlayerShell*>();
        check(shell != nullptr, "the player has a shell");
        if (shell == nullptr) {
            app.exit(failures == 0 ? 0 : 1);
            return;
        }
        if (ajiMode) {
            QString originalShaderForOffIntent;
            for (const anime4k::ModeInfo& preset : presets) {
                if (preset.mode != anime4k::Mode::Off &&
                    anime4k::chainIsComplete(anime4k::candidateForId(preset.id, shadersDir),
                                             shadersDir)) {
                    originalShaderForOffIntent = preset.id;
                    break;
                }
            }
            check(!originalShaderForOffIntent.isEmpty(),
                  "the Aji Off-intent regression has a complete non-Off shader preset");
            surface->setAnime4KChain(originalShaderForOffIntent);
            shaderBeforeAji = surface->anime4KChainId();
            check(shaderBeforeAji == originalShaderForOffIntent && shaderBeforeAji != offId &&
                      !surface->effectiveShaderChain().isEmpty(),
                  "the live Aji test begins with an observable active shader chain");
            surface->setAnime4KChain(offId);
            surface->setAnimeJanaiPlan(native::AnimeJanaiPlan{});
            bool baseOff = false;
            for (int waited = 0; waited < 600000; waited += 50) {
                QCoreApplication::processEvents();
                surface->update();
                if (surface->animeJanaiStatus() == QLatin1String("Off")) {
                    baseOff = true;
                    break;
                }
                QThread::msleep(50);
            }
            check(baseOff, "the source baseline is captured with Aji off");
            ajiSourceBaseline = settledFrame(surface);
            check(!ajiSourceBaseline.isNull(), "the unprocessed source frame is captured");
            surface->setAnime4KChain(shaderBeforeAji);
            for (const native::AnimeJanaiPreset& preset : native::animeJanaiPresets()) {
                if (!preset.rife && preset.id.startsWith(QLatin1String("spatial:"))) {
                    publicSpatialIds.append(preset.id);
                }
            }
            check(publicSpatialIds.size() == 5, "--aji sees all five public spatial models");
            if (publicSpatialIds.size() != 5) {
                app.exit(1);
                return;
            }
            originalAjiId = publicSpatialIds.first();
            originalAjiPlan = native::prepareAnimeJanaiPlan(originalAjiId, ajiBackend);
            check(originalAjiPlan.available,
                  "the staged backend and public spatial model prepare a real Aji plan");
            if (!originalAjiPlan.available) {
                std::fprintf(stderr, "     Aji preparation: %s\n",
                             qPrintable(originalAjiPlan.reason));
                app.exit(1);
                return;
            }
            QString ajiFailure;
            QObject::connect(surface, &VideoSurface::animeJanaiFailure, surface,
                             [&ajiFailure](const QString& reason) { ajiFailure = reason; });
            surface->setAnimeJanaiPlan(originalAjiPlan);
            const bool observed = [&] {
                for (int waited = 0; waited < 600000; waited += 50) {
                    QCoreApplication::processEvents();
                    surface->update();
                    if (surface->animeJanaiProcessingObserved()) return true;
                    if (!ajiFailure.isEmpty()) return false;
                    QThread::msleep(50);
                }
                return false;
            }();
            check(observed, "the active baseline Aji backend reports spatial processing");
            if (!observed) {
                std::fprintf(stderr, "     Aji baseline: %s\n",
                             qPrintable(ajiFailure.isEmpty() ? surface->animeJanaiStatus()
                                                            : ajiFailure));
                app.exit(1);
                return;
            }
        }
        if (ajiMode) {
            surface->setPaused(false);
            PlaybackTimestampSample beforePlayback;
            PlaybackTimestampSample playbackAdvanced;
            const bool actuallyStarted = requestPlaybackTimestamp(surface, &beforePlayback) &&
                                         waitForPlaybackProgress(
                                             surface, beforePlayback.sourceEpoch,
                                             beforePlayback.timestamp, &playbackAdvanced);
            check(actuallyStarted,
                  "the Aji probe begins Compare with observable active playback");
            if (!actuallyStarted) {
                std::fprintf(stderr,
                             "FAIL --aji playback start: epoch=%llu, timestamp=%s, "
                             "playingFlag=%d, status=%s\n",
                             static_cast<unsigned long long>(beforePlayback.sourceEpoch),
                             beforePlayback.timestamp.has_value()
                                 ? qPrintable(QString::number(*beforePlayback.timestamp, 'f', 6))
                                 : "(unavailable)",
                             surface->isPlaying(), qPrintable(surface->animeJanaiStatus()));
                app.exit(1);
                return;
            }
        }
        const uint64_t comparisonSourceEpoch = surface->sourceEpoch();
        const uint64_t pauseGenerationBeforeCompare = surface->pauseRequestGeneration();
        PlaybackTimestampSample preCaptureTimestamp;
        const bool preCaptureTimestampAvailable =
            requestPlaybackTimestamp(surface, &preCaptureTimestamp);
        emit shell->compareRequested();
        QTest::qWait(2500);
        const uint64_t pauseGenerationAfterCompare = surface->pauseRequestGeneration();
        auto* compare = dialog->findChild<CompareWindow*>();
        std::fprintf(stderr,
                     "DIAG --aji Compare open pause generation: before=%llu after=%llu, "
                     "playing=%d, pending=%d\n",
                     static_cast<unsigned long long>(pauseGenerationBeforeCompare),
                     static_cast<unsigned long long>(pauseGenerationAfterCompare),
                     surface->isPlaying(), compare != nullptr && compare->comparisonPending());
        check(compare != nullptr, "the compare row opened a window");
        if (compare == nullptr) {
            std::fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
            app.exit(1);
            return;
        }
        check(compare->isVisible(), "the compare window is on screen");
        check(surface->sourceEpoch() == comparisonSourceEpoch,
              "opening Compare preserves the source epoch");
        const std::optional<double> frozenTimestamp = compare->frozenPlaybackTimestamp();
        PlaybackTimestampSample settledAfterCapture;
        const bool captureTimestampStable =
            waitForStablePlaybackTimestamp(surface, comparisonSourceEpoch, &settledAfterCapture);
        check(compare->frozenSourceEpoch() == comparisonSourceEpoch &&
                  frozenTimestamp.has_value() && captureTimestampStable &&
                  !surface->isPlaying() &&
                  settledAfterCapture.timestamp.has_value() &&
                  std::abs(*frozenTimestamp - *settledAfterCapture.timestamp) <= 1e-6 &&
                  playbackTimestampMatches(surface, comparisonSourceEpoch, frozenTimestamp),
              "Compare freezes the current stable playback timestamp and source epoch");
        const QString comparisonPng = QStringLiteral(".probe-out/compare-window.png");
        const QImage fullscreenFrozenFrame = compare->frozenFrame();
        const QString fullscreenSelectionA = compare->selectedCandidateId(0);
        const QString fullscreenSelectionB = compare->selectedCandidateId(1);
        auto* fullscreenButton =
            compare->findChild<QToolButton*>(QStringLiteral("compareFullscreen"));
        auto* pinControl = compare->findChild<QPushButton*>(QStringLiteral("comparePinBtn"));
        const auto fullscreenStatePreserved = [&] {
            return compare->frozenFrame() == fullscreenFrozenFrame &&
                   compare->frozenSourceEpoch() == comparisonSourceEpoch &&
                   compare->frozenPlaybackTimestamp() == frozenTimestamp &&
                   compare->selectedCandidateId(0) == fullscreenSelectionA &&
                   compare->selectedCandidateId(1) == fullscreenSelectionB &&
                   compare->playbackHeld() && !surface->isPlaying();
        };
        const QRect initialNormalGeometry = compare->geometry();
        check(fullscreenButton != nullptr && pinControl != nullptr &&
                  fullscreenStatePreserved(),
              "fullscreen regression starts from a frozen held comparison");
        if (fullscreenButton != nullptr && pinControl != nullptr) {
            QTest::mouseClick(fullscreenButton, Qt::LeftButton);
            QCoreApplication::processEvents();
            check(compare->isFullScreen() && fullscreenButton->isChecked() &&
                      fullscreenStatePreserved(),
                  "toolbar fullscreen entry preserves frozen frame and playback hold");
            const Qt::FocusPolicy originalPinFocusPolicy = pinControl->focusPolicy();
            pinControl->setFocusPolicy(Qt::StrongFocus);
            pinControl->setFocus();
            QCoreApplication::processEvents();
            const bool childHasFocus = QApplication::focusWidget() == pinControl;
            QTest::keyClick(pinControl, Qt::Key_F11);
            QCoreApplication::processEvents();
            check(childHasFocus && !compare->isFullScreen() && !fullscreenButton->isChecked() &&
                      fullscreenStatePreserved(),
                  "F11 exits fullscreen with a child control focused");
            pinControl->setFocus();
            QTest::keyClick(pinControl, Qt::Key_F11);
            QCoreApplication::processEvents();
            QTest::keyClick(compare, Qt::Key_Escape);
            QCoreApplication::processEvents();
            check(!compare->isFullScreen() && compare->isVisible() &&
                      fullscreenStatePreserved(),
                  "Escape exits fullscreen without closing or releasing the comparison");
            compare->showMaximized();
            QCoreApplication::processEvents();
            QTest::keyClick(pinControl, Qt::Key_F11);
            QCoreApplication::processEvents();
            QTest::keyClick(pinControl, Qt::Key_F11);
            QCoreApplication::processEvents();
            check(compare->isMaximized() && fullscreenStatePreserved(),
                  "fullscreen restores the prior maximized state");
            compare->showNormal();
            compare->setGeometry(initialNormalGeometry);
            QCoreApplication::processEvents();
            QTest::keyClick(pinControl, Qt::Key_F11);
            QCoreApplication::processEvents();
            QTest::mouseClick(pinControl, Qt::LeftButton);
            QCoreApplication::processEvents();
            check(compare->isFullScreen() && compare->isPinned() &&
                      fullscreenStatePreserved(),
                  "pinning in fullscreen preserves fullscreen and playback state");
            QTest::mouseClick(pinControl, Qt::LeftButton);
            QCoreApplication::processEvents();
            check(compare->isFullScreen() && !compare->isPinned() &&
                      fullscreenStatePreserved(),
                  "unpinning in fullscreen preserves fullscreen and playback state");
            QTest::keyClick(pinControl, Qt::Key_F11);
            QCoreApplication::processEvents();
            check(!compare->isFullScreen() && compare->geometry() == initialNormalGeometry &&
                      fullscreenStatePreserved(),
                  "fullscreen restores normal geometry and keeps the original hold");
            pinControl->setFocusPolicy(originalPinFocusPolicy);
        }
        check(QDir().mkpath(QStringLiteral(".probe-out")) &&
                  compare->grab().save(comparisonPng),
              "the visible comparison window is captured as a PNG");

        // Every public shader preset and spatial model must be selectable.
        if (ajiMode &&
            (!frozenTimestamp.has_value() || !captureTimestampStable ||
             surface->isPlaying() ||
             !settledAfterCapture.timestamp.has_value() ||
             !compare->captureError().isEmpty() || compare->frozenFrame().isNull() ||
             compare->frozenSourceEpoch() != comparisonSourceEpoch ||
             std::abs(*frozenTimestamp - *settledAfterCapture.timestamp) > 1e-6 ||
             !playbackTimestampMatches(surface, comparisonSourceEpoch, frozenTimestamp))) {
            PlaybackTimestampSample latestAfterCapture;
            const bool latestAvailable = requestPlaybackTimestamp(surface, &latestAfterCapture);
            const QString beforeTimestamp =
                preCaptureTimestampAvailable && preCaptureTimestamp.timestamp.has_value()
                    ? QString::number(*preCaptureTimestamp.timestamp, 'f', 6)
                    : QStringLiteral("(unavailable)");
            const QString frozenTimestampText =
                frozenTimestamp.has_value() ? QString::number(*frozenTimestamp, 'f', 6)
                                            : QStringLiteral("(unavailable)");
            const QString currentTimestamp =
                latestAvailable && latestAfterCapture.timestamp.has_value()
                    ? QString::number(*latestAfterCapture.timestamp, 'f', 6)
                    : QStringLiteral("(unavailable)");
            std::fprintf(stderr,
                         "FAIL --aji capture prerequisite: error=%s, preEpoch=%llu, "
                         "preTimestamp=%s, frozenEpoch=%llu, frozenTimestamp=%s, "
                         "currentEpoch=%llu, currentTimestamp=%s, stable=%d, "
                         "playingFlag=%d, position=%.6f, pauseGeneration=%llu->%llu, "
                         "ajiStatus=%s\n",
                         qPrintable(compare->captureError()),
                         static_cast<unsigned long long>(preCaptureTimestamp.sourceEpoch),
                         qPrintable(beforeTimestamp),
                         static_cast<unsigned long long>(compare->frozenSourceEpoch()),
                         qPrintable(frozenTimestampText),
                         static_cast<unsigned long long>(latestAfterCapture.sourceEpoch),
                         qPrintable(currentTimestamp), captureTimestampStable,
                         surface->isPlaying(), surface->position(),
                         static_cast<unsigned long long>(pauseGenerationBeforeCompare),
                         static_cast<unsigned long long>(pauseGenerationAfterCompare),
                         qPrintable(surface->animeJanaiStatus()));
            app.exit(1);
            return;
        }
        struct Offered {
            QString id;
            QString label;
            QStringList path;
            bool enabled = true;
            QString tooltip;
        };
        QVector<Offered> expected;
        QStringList unavailablePresetIds;
        QHash<QString, QStringList> missingPresetFiles;
        for (const anime4k::ModeInfo& preset : presets) {
            const anime4k::Candidate candidate = anime4k::candidateForId(preset.id, shadersDir);
            if (anime4k::chainIsComplete(candidate, shadersDir)) {
                expected.append({preset.id, preset.label, labelPath(preset.category), true, {}});
            } else {
                unavailablePresetIds.append(preset.id);
                QStringList missingFiles;
                for (const QString& file : candidate.files) {
                    if (!QFileInfo(QDir(shadersDir).filePath(file)).exists()) {
                        missingFiles.append(QFileInfo(file).fileName());
                    }
                }
                missingPresetFiles.insert(preset.id, missingFiles);
            }
        }
        const QStringList spatialPath{QStringLiteral("AnimeJaNai"),
                                      QStringLiteral("Spatial models")};
        for (const native::AnimeJanaiPreset& preset : native::animeJanaiPresets()) {
            if (preset.rife || !preset.id.startsWith(QLatin1String("spatial:"))) {
                continue;
            }
            expected.append({preset.id, preset.label, spatialPath, true, {}});
        }
        std::function<void(QMenu*, QStringList&, QVector<Offered>&)> collect =
            [&collect](QMenu* menu, QStringList& path, QVector<Offered>& out) {
                for (QAction* action : menu->actions()) {
                    if (QMenu* submenu = action->menu()) {
                        path.append(action->text());
                        collect(submenu, path, out);
                        path.removeLast();
                    } else if (!action->data().toString().isEmpty()) {
                        out.append({action->data().toString(), action->text(), path,
                                    action->isEnabled() && menu->isEnabled(),
                                    action->toolTip()});
                    }
                }
            };
        int chipMenus = 0;
        QStringList unoffered;   // available catalog entries missing from a badge
        QStringList repeated;    // registry entries offered more than once
        QStringList foreign;     // choices not present in either public catalog
        QStringList misplaced;   // choices not under their category's own labels
        QStringList mislabeled;  // choices whose leaf text differs from its catalog label
        QStringList disabledAvailable;
        QStringList selectableMissingAssets;
        QStringList untruthfulMissingAssetLabels;
        for (QToolButton* chip : compare->findChildren<QToolButton*>()) {
            if (!chip->isVisible() || chip->menu() == nullptr) {
                continue;
            }
            ++chipMenus;
            QStringList path;
            QVector<Offered> offered;
            collect(chip->menu(), path, offered);
            for (const Offered& entry : expected) {
                const auto seen = std::count_if(
                    offered.cbegin(), offered.cend(),
                    [&entry](const Offered& choice) { return choice.id == entry.id; });
                if (seen == 0 && !unoffered.contains(entry.id)) {
                    unoffered.append(entry.id);
                }
                const auto found = std::find_if(
                    offered.cbegin(), offered.cend(),
                    [&entry](const Offered& choice) { return choice.id == entry.id; });
                if (found != offered.cend() && !found->enabled &&
                    !disabledAvailable.contains(entry.id)) {
                    disabledAvailable.append(entry.id);
                }
                if (seen > 1 && !repeated.contains(entry.id)) {
                    repeated.append(entry.id);
                }
            }
            for (const Offered& choice : offered) {
                const auto expectedChoice = std::find_if(
                    expected.cbegin(), expected.cend(), [&choice](const Offered& entry) {
                        return entry.id == choice.id;
                    });
                if (expectedChoice == expected.cend()) {
                    if (unavailablePresetIds.contains(choice.id)) {
                        const auto seenUnavailable = std::count_if(
                            offered.cbegin(), offered.cend(),
                            [&choice](const Offered& candidate) {
                                return candidate.id == choice.id;
                            });
                        if (seenUnavailable > 1 && !repeated.contains(choice.id)) {
                            repeated.append(choice.id);
                        }
                        const auto preset = std::find_if(
                            presets.cbegin(), presets.cend(),
                            [&choice](const anime4k::ModeInfo& candidate) {
                                return candidate.id == choice.id;
                            });
                        if (preset != presets.cend()) {
                            if (choice.path != labelPath(preset->category) &&
                                !misplaced.contains(choice.id)) {
                                misplaced.append(choice.id);
                            }
                            if (choice.label != preset->label && !mislabeled.contains(choice.id)) {
                                mislabeled.append(choice.id);
                            }
                        }
                        if (choice.enabled && !selectableMissingAssets.contains(choice.id)) {
                            selectableMissingAssets.append(choice.id);
                        }
                        const QStringList missingFiles = missingPresetFiles.value(choice.id);
                        const bool namesMissingFile = std::any_of(
                            missingFiles.cbegin(), missingFiles.cend(),
                            [&choice](const QString& file) {
                                return !file.isEmpty() &&
                                       choice.tooltip.contains(file, Qt::CaseInsensitive);
                            });
                        if (!choice.enabled && !namesMissingFile &&
                            !untruthfulMissingAssetLabels.contains(choice.id)) {
                            untruthfulMissingAssetLabels.append(choice.id);
                        }
                        continue;
                    }
                    if (!foreign.contains(choice.id)) {
                        foreign.append(choice.id);
                    }
                } else {
                    if (choice.path != expectedChoice->path && !misplaced.contains(choice.id)) {
                        misplaced.append(choice.id);
                    }
                    if (choice.label != expectedChoice->label && !mislabeled.contains(choice.id)) {
                        mislabeled.append(choice.id);
                    }
                }
            }
        }
        std::fprintf(stderr, "     chip menus: %d, expected registered choices: %d\n", chipMenus,
                     int(expected.size()));
        for (const QString& id : unoffered) {
            std::fprintf(stderr, "     not selectable: %s\n", qPrintable(id));
        }
        for (const QString& id : repeated) {
            std::fprintf(stderr, "     offered twice: %s\n", qPrintable(id));
        }
        for (const QString& id : foreign) {
            std::fprintf(stderr, "     not a registered upscale candidate or preset: %s\n",
                         qPrintable(id));
        }
        for (const QString& id : misplaced) {
            std::fprintf(stderr, "     under the wrong group: %s\n", qPrintable(id));
        }
        for (const QString& id : mislabeled) {
            std::fprintf(stderr, "     spelled differently: %s\n", qPrintable(id));
        }
        for (const QString& id : selectableMissingAssets) {
            std::fprintf(stderr, "     missing shader assets remain selectable: %s\n",
                         qPrintable(id));
        }
        for (const QString& id : untruthfulMissingAssetLabels) {
            std::fprintf(stderr, "     missing shader row lacks its missing-file explanation: %s\n",
                         qPrintable(id));
        }
        check(chipMenus > 0, "the compare badges carry a candidate chooser menu");
        check(unoffered.isEmpty(),
              "the chooser offers every preset and public spatial model from the catalogs");
        check(repeated.isEmpty(), "no registered compare candidate is offered twice in one badge menu");
        check(foreign.isEmpty(), "no RIFE, private or loose shader candidate is selectable");
        check(misplaced.isEmpty(), "each choice sits under its registry category labels");
        check(mislabeled.isEmpty(), "each choice uses its registry label");
        check(disabledAvailable.isEmpty(),
              "available catalog candidates are not incorrectly disabled");
        check(selectableMissingAssets.isEmpty(),
              "shader presets with missing assets cannot be selected");
        check(untruthfulMissingAssetLabels.isEmpty(),
              "disabled missing-asset shader rows explain the actual missing file");
        // The chain internals: files a preset runs inside its chain but that the
        // catalogue does not list as candidates of their own (the AutoDownscale
        // steps). Which files those are is read from the catalogue rather than
        // named here, so the set follows the chains instead of the probe.
        QStringList offeredAlone;
        for (const anime4k::Candidate& candidate : all) {
            if (!presetIds.contains(candidate.id)) {
                offeredAlone.append(candidate.id);
            }
        }
        QStringList internals;
        for (const anime4k::ModeInfo& preset : presets) {
            for (const QString& file : preset.files) {
                if (!offeredAlone.contains(file) && !internals.contains(file)) {
                    internals.append(file);
                }
            }
        }
        bool offeredInternal = false;
        for (QToolButton* chip : compare->findChildren<QToolButton*>()) {
            if (!chip->isVisible() || chip->menu() == nullptr) {
                continue;
            }
            std::function<void(QMenu*)> scan = [&](QMenu* menu) {
                for (QAction* action : menu->actions()) {
                    if (QMenu* submenu = action->menu()) {
                        scan(submenu);
                    } else if (internals.contains(action->data().toString())) {
                        offeredInternal = true;
                    }
                }
            };
            scan(chip->menu());
        }
        check(!offeredInternal, "the chain-internal downscale steps are not offered standalone");
        check(chipMenus == 2, "each visible image has exactly one corner model selector");
        auto* modeBox = compare->findChild<QComboBox*>(QStringLiteral("compareMode"));
        auto* splitKnob = compare->findChild<QDoubleSpinBox*>(QStringLiteral("compareSplit"));
        auto* flickerKnob = compare->findChild<QSpinBox*>(QStringLiteral("compareFlicker"));
        auto* gainKnob = compare->findChild<QSpinBox*>(QStringLiteral("compareGain"));
        auto* fitButton = compare->findChild<QToolButton*>(QStringLiteral("compareFit"));
        check(modeBox && splitKnob && flickerKnob && gainKnob && fitButton &&
                  !splitKnob->isVisible() && !flickerKnob->isVisible() && !gainKnob->isVisible(),
              "the initially visible two-pane layout hides all irrelevant controls");
        if (!modeBox || !splitKnob || !flickerKnob || !gainKnob || !fitButton) {
            app.exit(1);
            return;
        }
        for (CompareMode mode : {CompareMode::SplitH, CompareMode::SplitV,
                                 CompareMode::Flicker, CompareMode::Difference,
                                 CompareMode::SideBySide}) {
            modeBox->setCurrentIndex(modeBox->findData(QString::fromLatin1(modeKey(mode))));
            QCoreApplication::processEvents();
            check(splitKnob->isVisible() == (mode == CompareMode::SplitH || mode == CompareMode::SplitV) &&
                      flickerKnob->isVisible() == (mode == CompareMode::Flicker) &&
                      gainKnob->isVisible() == (mode == CompareMode::Difference),
                  "layout switching exposes only the control that actually applies");
        }
        QList<ComparePane*> panes;
        for (ComparePane* pane : compare->findChildren<ComparePane*>())
            if (pane->isVisible()) panes.append(pane);
        check(panes.size() == 2, "side-by-side exposes two actual image panes");
        if (panes.size() == 2) {
            const QPoint local = panes.first()->viewport()->rect().center();
            QWheelEvent zoomEvent(QPointF(local), QPointF(panes.first()->viewport()->mapToGlobal(local)),
                                  QPoint(), QPoint(0, 240), Qt::NoButton, Qt::NoModifier,
                                  Qt::NoScrollPhase, false);
            QApplication::sendEvent(panes.first()->viewport(), &zoomEvent);
            QCoreApplication::processEvents();
            check(panes.first()->zoom() > 1.0 && panes.last()->zoom() > 1.0,
                  "an actual wheel gesture zooms both active panes");
            QTest::mouseClick(fitButton, Qt::LeftButton);
            QCoreApplication::processEvents();
            check(panes.first()->zoom() == 1.0 && panes.last()->zoom() == 1.0,
                  "Fit resets both active side-by-side pane zooms");
        }
        const QImage immutableShaderInput = compare->frozenFrame();
        const QImage liveBeforeModelChanges = settledFrame(surface);
        // One switch test using actual mouse click emulation:
        QString testPresetId;
        for (const anime4k::ModeInfo& preset : presets) {
            if (preset.mode != anime4k::Mode::Off &&
                anime4k::chainIsComplete(anime4k::candidateForId(preset.id, shadersDir), shadersDir)) {
                testPresetId = preset.id;
                break;
            }
        }
        check(!testPresetId.isEmpty(), "found a complete Anime4K preset to switch to");

        const QImage unchangedSide = compare->renderedSideB();
        check(triggerChoice(compare, 0, testPresetId),
              "a corner selector switches to an available Anime4K preset via mouse click");

        QImage output;
        QString failure;
        const bool completed = waitForRenderedSide(compare, testPresetId, 0, &output, &failure);
        const auto picture = mplayer::probes::measurePicture(
            output, surface->videoWidth(), surface->videoHeight());
        check(completed && !picture.isMostlyBlack() &&
                  visibleSideMatches(compare, 0, testPresetId, output, surface),
              "changing the Anime4K model leaves the correctly labelled actual visible image lit");
        check(compare->renderedSideB() == unchangedSide,
              "changing one model preserves the other side's pixels");
        check(compare->frozenFrame() == immutableShaderInput &&
                  playbackTimestampMatches(surface, comparisonSourceEpoch, frozenTimestamp) &&
                  settledFrame(surface) == liveBeforeModelChanges,
              "shader comparisons preserve frozen input and exact live pixels and timestamp");

        bool menuVisible = false;
        for (QMenu* menu : compare->findChildren<QMenu*>()) menuVisible |= menu->isVisible();
        check(!menuVisible, "the chooser closes before the new output is presented");
        if (!completed || picture.isMostlyBlack()) {
            std::fprintf(stderr, "FAIL shader %s side 0: %s\n",
                         qPrintable(testPresetId), qPrintable(failure));
            app.exit(1);
            return;
        }

        check(triggerChoice(compare, 0, QStringLiteral("off")) &&
                  compare->renderedSideA() == immutableShaderInput &&
                  compare->renderedSideB() == unchangedSide,
              "corner selector returns to the exact immutable raw Original after shader change");

        if (ajiMode) {
            const QString savedPreset =
                QSettings().value(QStringLiteral("player/anime4k")).toString();
            const QString liveChain = surface->anime4KChainId();
            const QString liveShader = surface->effectiveShaderChain();
            const QImage liveControl = settledFrame(surface);
            const QImage immutableOriginal = compare->frozenFrame();
            const auto unchangedLivePlayback = [&] {
                return !surface->isPlaying() &&
                       playbackTimestampMatches(surface, comparisonSourceEpoch,
                                                frozenTimestamp) &&
                       surface->animeJanaiPlan().presetId == originalAjiId &&
                       surface->animeJanaiPlan().backend == ajiBackend &&
                       surface->animeJanaiProcessingObserved() &&
                       surface->anime4KChainId() == liveChain &&
                       surface->effectiveShaderChain() == liveShader &&
                       settledFrame(surface) == liveControl;
            };
            check(!immutableOriginal.isNull() && !liveControl.isNull(),
                  "the isolated source and the held player's output are both captured");
            check(unchangedLivePlayback(),
                  "opening the isolated comparison preserves exact live timestamp and pixels");
            if (immutableOriginal.isNull() || liveControl.isNull() ||
                !unchangedLivePlayback()) {
                app.exit(1);
                return;
            }

            auto* operation = compare->findChild<ProcessingOverlay*>();
            bool cancelledRealBuild = false;
            bool retryBuildObserved = false;
            bool honestBuildUi = false;
            bool elapsedAdvanced = false;
            QElapsedTimer buildUiElapsed;
            QStringList initialOperationLabels;
            const QImage otherBeforeBuildCancellation = compare->renderedSideB();
            QTimer cancelRealBuild;
            cancelRealBuild.setInterval(40);
            QObject::connect(&cancelRealBuild, &QTimer::timeout, compare, [&] {
                if (!operation || !operation->isVisible() || !compare->comparisonPending()) return;
                auto* progress = operation->findChild<QProgressBar*>();
                auto* cancel = operation->findChild<QPushButton*>();
                if (!progress || progress->minimum() != 0 || progress->maximum() != 0 ||
                    !cancel || !cancel->isEnabled()) return;
                bool popupOpen = false;
                for (QMenu* menu : compare->findChildren<QMenu*>()) popupOpen |= menu->isVisible();
                const bool honestNow = !popupOpen && !surface->isPlaying() && surface->pauseObserved() &&
                                       operation->isWindow() &&
                                       operation->windowFlags().testFlag(Qt::WindowStaysOnTopHint);
                QStringList labels;
                for (QLabel* label : operation->findChildren<QLabel*>()) labels.append(label->text());
                if (!buildUiElapsed.isValid()) {
                    buildUiElapsed.start();
                    initialOperationLabels = labels;
                    honestBuildUi = honestNow;
                    return;
                }
                honestBuildUi &= honestNow;
                if (buildUiElapsed.elapsed() < 1200) return;
                elapsedAdvanced = labels != initialOperationLabels;
                cancelRealBuild.stop();
                cancelledRealBuild = true;
                QTest::mouseClick(cancel, Qt::LeftButton);
            });
            cancelRealBuild.start();
            const QString cancellableModel = publicSpatialIds.last();
            check(triggerChoice(compare, 0, cancellableModel),
                  "a real spatial choice exercises cached-or-building engine ownership");
            cancelRealBuild.stop();
            if (cancelledRealBuild) {
                check(honestBuildUi && elapsedAdvanced && !compare->comparisonPending() &&
                          compare->renderedSideA().isNull() &&
                          !compare->candidateError(cancellableModel).isEmpty() &&
                          compare->renderedSideB() == otherBeforeBuildCancellation &&
                          unchangedLivePlayback(),
                      "actual build Cancel stops owned work after advancing elapsed time, with indeterminate progress and exact live preservation");
                QTimer observeRetryBuild;
                observeRetryBuild.setInterval(40);
                QObject::connect(&observeRetryBuild, &QTimer::timeout, compare, [&] {
                    auto* progress = operation->findChild<QProgressBar*>();
                    retryBuildObserved |= operation->isVisible() && compare->comparisonPending() &&
                                          progress && progress->maximum() == 0;
                });
                observeRetryBuild.start();
                check(triggerChoice(compare, 0, cancellableModel),
                      "the real model can be selected again after cancelling its owned builder");
                observeRetryBuild.stop();
            }
            QImage preparedOutput;
            QString prepareFailure;
            check(waitForRenderedSide(compare, cancellableModel, 0,
                                      &preparedOutput, &prepareFailure) &&
                      visibleSideMatches(compare, 0, cancellableModel, preparedOutput, surface),
                  "the actual model succeeds after either cached selection or cancelled-build retry");
            if (retryBuildObserved && operation) {
                auto* progress = operation->findChild<QProgressBar*>();
                auto* cancel = operation->findChild<QPushButton*>();
                check(operation->isVisible() && progress && progress->maximum() == 1 &&
                          progress->value() == 1 && cancel && !cancel->isEnabled(),
                      "a genuinely completed retry presents visible settled success before dismissing");
            }
            check(triggerChoice(compare, 0, offId),
                  "the build-lifecycle case returns to exact raw input before the model sweep");

            for (qsizetype index = 0; index < publicSpatialIds.size(); ++index) {
                const QString& id = publicSpatialIds.at(index);
                check(triggerChoice(compare, 0, id),
                      "the visible side-A menu selects a public spatial model");
                QImage a;
                QString failure;
                const bool renderedA = waitForRenderedSide(compare, id, 0, &a, &failure);
                check(renderedA && !a.isNull() &&
                          !mplayer::probes::measurePicture(a, surface->videoWidth(),
                                                          surface->videoHeight()).isMostlyBlack() &&
                          visibleSideMatches(compare, 0, id, a, surface),
                      "the selected spatial model produces lit correctly labelled visible side-A output");
                if (!renderedA || a.isNull()) {
                    std::fprintf(stderr, "FAIL model %s: %s\n", qPrintable(id),
                                 qPrintable(failure));
                    app.exit(1);
                    return;
                }
                const double effect = meanDifference(immutableOriginal, a);
                std::fprintf(stderr, "     spatial %s effect %.6f / 255\n",
                             qPrintable(id), effect);
                check(effect > 0.01,
                      "the model output differs from the immutable Original pixels");
                check(triggerChoice(compare, 1, id),
                      "the visible side-B menu selects the same spatial model");
                QImage b;
                const bool renderedB = waitForRenderedSide(compare, id, 1, &b, &failure);
                check(renderedB && !b.isNull() && visibleSideMatches(compare, 1, id, b, surface),
                      "the same spatial model produces lit correctly labelled visible side-B output");
                if (!renderedB || b.isNull()) {
                    std::fprintf(stderr, "FAIL side-B model %s: %s; visible=%d capture=%s\n",
                                 qPrintable(id), qPrintable(failure), compare->isVisible(),
                                 qPrintable(compare->captureError()));
                    app.exit(1);
                    return;
                }
                const double noise = meanDifference(a, b);
                std::fprintf(stderr, "     same-input same-model noise %.6f / 255\n", noise);
                check(renderedB && a == b && noise == 0.0,
                      "both sides of one model use exactly the same frozen input");
                check(compare->frozenFrame() == immutableOriginal && unchangedLivePlayback(),
                      "each model preserves frozen Original and pixel-identical live playback");
                check(a.save(QStringLiteral(".probe-out/compare-aji-%1.png").arg(index)),
                      "the actual selected-model output is saved as PNG");
            }

            check(triggerChoice(compare, 0, offId),
                  "the visible menu selects an unprocessed Original side");
            QImage rawControl;
            QString rawFailure;
            const bool rawRendered =
                waitForRenderedSide(compare, offId, 0, &rawControl, &rawFailure);
            check(rawRendered && rawControl == immutableOriginal,
                  "Off reproduces the pixel-identical immutable Original after all models");
            check(triggerChoice(compare, 1, offId),
                  "the other visible menu selects the same Original side");
            check(compare->renderedSideA() == compare->renderedSideB(),
                  "two unprocessed sides have zero pixel difference");
            check(unchangedLivePlayback(),
                  "Original rendering leaves the player's active Aji and shader output untouched");

            QString crossFamilyShader;
            for (const anime4k::ModeInfo& preset : presets) {
                if (preset.id != offId &&
                    anime4k::chainIsComplete(anime4k::candidateForId(preset.id), shadersDir)) {
                    crossFamilyShader = preset.id;
                    break;
                }
            }
            check(!crossFamilyShader.isEmpty(), "a real shader is available for cross-family switching");
            for (const QString& spatialId : publicSpatialIds) {
                for (int side = 0; side < 2; ++side) {
                    const QImage otherSide = side == 0 ? compare->renderedSideB()
                                                        : compare->renderedSideA();
                    for (const QString& id : {crossFamilyShader, spatialId, crossFamilyShader}) {
                        check(triggerChoice(compare, side, id),
                              "a visible corner switches across the shader/spatial boundary");
                        QImage output;
                        QString failure;
                        const bool complete = waitForRenderedSide(compare, id, side, &output, &failure);
                        check(complete && visibleSideMatches(compare, side, id, output, surface),
                              "cross-family switching presents actual lit pixels with the selected model");
                        check((side == 0 ? compare->renderedSideB() : compare->renderedSideA()) == otherSide &&
                                  compare->frozenFrame() == immutableOriginal && unchangedLivePlayback(),
                              "cross-family changes preserve the other side, raw input and exact live state");
                        if (!complete) {
                            std::fprintf(stderr, "FAIL cross-family %s side %d: %s\\n",
                                         qPrintable(id), side, qPrintable(failure));
                            app.exit(1);
                            return;
                        }
                    }
                }
            }
            check(triggerChoice(compare, 0, offId) && triggerChoice(compare, 1, offId) &&
                      compare->renderedSideA() == immutableOriginal &&
                      compare->renderedSideB() == immutableOriginal,
                  "both sides return to exact raw pixels after all cross-family transitions");

            const native::AnimeJanaiConfig mixedConfig = native::AnimeJanaiConfigStore::load();
            const native::AnimeJanaiSourceContext sourceContext =
                surface->animeJanaiSourceContext();
            QString mixedPresetId;
            native::AnimeJanaiChain mixedChain;
            const qint64 sourceArea = qint64(sourceContext.width) * sourceContext.height;
            for (const native::AnimeJanaiSlot& slot : mixedConfig.customSlots) {
                QVector<native::AnimeJanaiChain> chains = slot.chains;
                std::sort(chains.begin(), chains.end(),
                          [](const native::AnimeJanaiChain& left,
                             const native::AnimeJanaiChain& right) {
                              return left.number < right.number;
                          });
                for (const native::AnimeJanaiChain& chain : chains) {
                    bool hasAjiStep = false;
                    bool hasAnime4KStep = false;
                    for (const native::AnimeJanaiChainStep& step : chain.steps) {
                        hasAjiStep |= step.kind == native::AnimeJanaiStepKind::OnnxModel;
                        hasAnime4KStep |= step.kind == native::AnimeJanaiStepKind::Anime4K;
                    }
                    const auto areaBound = [](const QString& text) {
                        if (text.compare(QStringLiteral("infxinf"), Qt::CaseInsensitive) == 0)
                            return qint64(0);
                        const qsizetype x = text.indexOf(QLatin1Char('x'));
                        if (x <= 0) return qint64(-1);
                        bool widthOk = false;
                        bool heightOk = false;
                        const qint64 width = text.left(x).toLongLong(&widthOk);
                        const qint64 height = text.mid(x + 1).toLongLong(&heightOk);
                        if (!widthOk || !heightOk || width == 0 || height == 0)
                            return qint64(0);
                        return width * height;
                    };
                    const qint64 minimumArea = areaBound(chain.minResolution);
                    const qint64 maximumArea = areaBound(chain.maxResolution);
                    const bool sourceMatches =
                        sourceContext.width > 0 && sourceContext.height > 0 &&
                        std::isfinite(sourceContext.fps) && sourceContext.fps > 0.0 &&
                        minimumArea >= 0 && maximumArea >= 0 &&
                        sourceArea >= minimumArea &&
                        (maximumArea == 0 || sourceArea <= maximumArea) &&
                        sourceContext.fps >= chain.minFps &&
                        (chain.maxFps <= 0 || sourceContext.fps <= chain.maxFps);
                    if (sourceMatches) {
                        if (hasAjiStep && hasAnime4KStep) {
                            mixedPresetId = QStringLiteral("builtin_slot:%1").arg(slot.number);
                            mixedChain = chain;
                        }
                        break;
                    }
                }
                if (!mixedPresetId.isEmpty()) break;
            }
            if (mixedPresetId.isEmpty()) {
                std::fprintf(stderr,
                             "SKIP configured mixed Compare scenario: no mixed ONNX/Anime4K slot exists in the read-only native config; fixture requirement is one slot with both step kinds and installed referenced assets.\\n");
            } else {
                const native::Anime4KShaderProgramResolver shaderResolver =
                    [](const QString& presetId, QString* reason) {
                        return anime4k::filterShaderProgramForCandidate(
                            presetId, anime4k::findShadersDir(), reason);
                    };
                const native::AnimeJanaiPlan mixedPlan = native::prepareAnimeJanaiPlan(
                    mixedPresetId, ajiBackend, {}, sourceContext, shaderResolver);
                if (!mixedPlan.available) {
                    std::fprintf(stderr,
                                 "SKIP configured mixed Compare scenario: fixture assets or source activation are unavailable: %s\\n",
                                 qPrintable(mixedPlan.reason));
                } else {
                    native::AnimeJanaiSourceContext unknownCadence = sourceContext;
                    unknownCadence.fps = 0.0;
                    const native::AnimeJanaiPlan deferredPlan =
                        native::prepareAnimeJanaiPlan(mixedPresetId, ajiBackend, {},
                                                       unknownCadence, {});
                    check(deferredPlan.needsSourceContext && !deferredPlan.available,
                          "unknown observed FPS defers mixed-chain selection without a guessed cadence or fallback");
                    check(mixedPlan.selectedChainNumber == mixedChain.number,
                          "mixed Compare selects the configured chain using original source dimensions and observed FPS");
                    QVector<QPair<native::AnimeJanaiPlanStage::Kind, QPair<int, int>>> expectedStages;
                    for (const native::AnimeJanaiChainStep& step : mixedChain.steps) {
                        if (step.kind == native::AnimeJanaiStepKind::OnnxModel &&
                            !expectedStages.isEmpty() &&
                            expectedStages.last().first == native::AnimeJanaiPlanStage::Kind::Aji) {
                            expectedStages.last().second.second = step.number;
                        } else {
                            expectedStages.append({
                                step.kind == native::AnimeJanaiStepKind::Anime4K
                                    ? native::AnimeJanaiPlanStage::Kind::Anime4K
                                    : native::AnimeJanaiPlanStage::Kind::Aji,
                                {step.number, step.number}});
                        }
                    }
                    QVector<QPair<native::AnimeJanaiPlanStage::Kind, QPair<int, int>>> actualStages;
                    for (const native::AnimeJanaiPlanStage& stage : mixedPlan.stages) {
                        if (stage.firstStepNumber > 0) {
                            actualStages.append({stage.kind,
                                                 {stage.firstStepNumber,
                                                  stage.lastStepNumber}});
                        }
                    }
                    const bool ordered = actualStages == expectedStages;
                    int ajiStages = 0;
                    int shaderStages = 0;
                    for (const native::AnimeJanaiPlanStage& stage : mixedPlan.stages) {
                        if (stage.kind == native::AnimeJanaiPlanStage::Kind::Aji) ++ajiStages;
                        else if (stage.kind == native::AnimeJanaiPlanStage::Kind::Anime4K) ++shaderStages;
                    }
                    check(ordered && ajiStages > 0 && shaderStages > 0,
                          "mixed Compare plan preserves configured A4K/AJI stage order");
                    bool scaleMatches = true;
                    for (const native::AnimeJanaiPlanStage& stage : mixedPlan.stages) {
                        if (stage.kind != native::AnimeJanaiPlanStage::Kind::Anime4K) continue;
                        const auto configured = std::find_if(
                            mixedChain.steps.cbegin(), mixedChain.steps.cend(),
                            [&stage](const native::AnimeJanaiChainStep& step) {
                                return step.number == stage.firstStepNumber &&
                                       step.kind == native::AnimeJanaiStepKind::Anime4K;
                            });
                        scaleMatches &= configured != mixedChain.steps.cend() &&
                                        stage.scaleFactor == configured->scaleFactor;
                    }
                    check(scaleMatches,
                          "each comparison shader stage uses its configured output scale");
                    check(triggerChoice(compare, 0, mixedPresetId),
                          "Compare accepts the actual configured mixed profile");
                    QImage mixedOutput;
                    QString mixedFailure;
                    const bool mixedRendered = waitForRenderedSide(
                        compare, mixedPresetId, 0, &mixedOutput, &mixedFailure);
                    check(mixedRendered && !mixedOutput.isNull() &&
                              visibleSideMatches(compare, 0, mixedPresetId, mixedOutput, surface) &&
                              mixedOutput != immutableOriginal,
                          "configured mixed chain renders through isolated Compare in its declared order and scale");
                    if (!mixedRendered) {
                        std::fprintf(stderr, "FAIL mixed Compare %s: %s\\n",
                                     qPrintable(mixedPresetId), qPrintable(mixedFailure));
                        app.exit(1);
                        return;
                    }
                    check(triggerChoice(compare, 0, offId) &&
                              compare->renderedSideA() == immutableOriginal &&
                              unchangedLivePlayback(),
                          "switching away from mixed Compare restores the exact frozen and unrelated live output");
                }
            }

            bool supersededWhilePending = false;
            QTimer rapidSelection;
            rapidSelection.setInterval(1);
            QObject::connect(&rapidSelection, &QTimer::timeout, compare, [&] {
                if (!compare->comparisonPending()) return;
                rapidSelection.stop();
                supersededWhilePending = triggerChoice(compare, 0, crossFamilyShader);
            });
            rapidSelection.start();
            check(triggerChoice(compare, 0, publicSpatialIds.first()),
                  "a real spatial selection starts the rapid-selection lifecycle");
            rapidSelection.stop();
            QImage finalRapidOutput;
            QString rapidFailure;
            check(supersededWhilePending &&
                      waitForRenderedSide(compare, crossFamilyShader, 0,
                                          &finalRapidOutput, &rapidFailure) &&
                      visibleSideMatches(compare, 0, crossFamilyShader, finalRapidOutput, surface) &&
                      compare->renderedSideB() == immutableOriginal && unchangedLivePlayback(),
                  "a newer selection wins without publishing the superseded model or touching live playback");
            check(triggerChoice(compare, 0, offId),
                  "Original remains usable after rapid selection cancellation");

            const QImage unaffectedBeforeFailure = compare->renderedSideB();
            compare->setAjiBackend(native::Backend::DirectML);
            const QString missingId = publicSpatialIds.last();
            check(triggerChoice(compare, 0, missingId),
                  "a spatial choice keeps the explicitly selected unavailable backend");
            QImage missingOutput;
            QString missingFailure;
            const bool substituted =
                waitForRenderedSide(compare, missingId, 0, &missingOutput, &missingFailure);
            check(!substituted && missingOutput.isNull() &&
                      compare->renderedSideA().isNull() &&
                      !compare->candidateError(missingId).isEmpty(),
                  "an unavailable backend names the failure without a fallback or stale image");
            check(compare->renderedSideB() == unaffectedBeforeFailure,
                  "a named backend failure preserves the unaffected comparison side exactly");
            check(unchangedLivePlayback(),
                  "an unavailable comparison backend leaves live processing and timestamp intact");
            compare->setAjiBackend(ajiBackend);
            check(triggerChoice(compare, 0, offId),
                  "Original remains usable after a failed backend selection");
            check(compare->renderedSideA() == immutableOriginal,
                  "the named backend failure does not damage the frozen input");
            check(QSettings().value(QStringLiteral("player/anime4k")).toString() == savedPreset,
                  "comparison choices leave saved player preferences unchanged");

            compare->close();
            PlaybackTimestampSample resumed;
            check(!compare->isVisible() &&
                      waitForPlaybackProgress(surface, comparisonSourceEpoch,
                                              frozenTimestamp, &resumed),
                  "closing Compare resumes playback that was playing when it opened");
            check(surface->animeJanaiPlan().presetId == originalAjiId &&
                      surface->anime4KChainId() == liveChain,
                  "normal close preserves the player's processing choices");

            const auto reopen = [&] {
                const quint64 pauseBefore = surface->pauseRequestGeneration();
                emit shell->compareRequested();
                QCoreApplication::processEvents();
                std::fprintf(stderr,
                             "     reopen capture readiness: inProgress=%d held=%d "
                             "visible=%d pending=%d frozenNull=%d\n",
                             compare->captureInProgress() ? 1 : 0,
                             compare->playbackHeld() ? 1 : 0, compare->isVisible() ? 1 : 0,
                             compare->comparisonPending() ? 1 : 0,
                             compare->frozenFrame().isNull() ? 1 : 0);
                const bool captured = compare->isVisible() && compare->captureError().isEmpty() &&
                                      !compare->frozenFrame().isNull();
                if (!captured) {
                    PlaybackTimestampSample current;
                    const bool timestampAvailable = requestPlaybackTimestamp(surface, &current);
                    const auto frozen = compare->frozenPlaybackTimestamp();
                    std::fprintf(stderr,
                                 "TRACE Compare reopen refused: visible=%d pending=%d frozenNull=%d "
                                 "capture=%s lock=%s inProgress=%d held=%d visiblePanes=%d "
                                 "liveEpoch=%llu frozenEpoch=%llu "
                                 "timestamp=%s frozenTimestamp=%s paused=%d playing=%d "
                                 "pauseGeneration=%llu->%llu\n",
                                 compare->isVisible(), compare->comparisonPending(),
                                 compare->frozenFrame().isNull(), qPrintable(compare->captureError()),
                                 qPrintable(compare->lockError()),
                                 compare->captureInProgress() ? 1 : 0,
                                 compare->playbackHeld() ? 1 : 0,
                                 static_cast<int>(visibleComparePanes(compare)),
                                 static_cast<unsigned long long>(surface->sourceEpoch()),
                                 static_cast<unsigned long long>(compare->frozenSourceEpoch()),
                                 timestampAvailable
                                     ? qPrintable(QString::number(*current.timestamp, 'f', 6))
                                     : "(unavailable)",
                                 frozen.has_value() ? qPrintable(QString::number(*frozen, 'f', 6))
                                                    : "(unavailable)",
                                 surface->pauseObserved(), surface->isPlaying(),
                                 static_cast<unsigned long long>(pauseBefore),
                                 static_cast<unsigned long long>(surface->pauseRequestGeneration()));
                }
                return captured;
            };
            surface->setPaused(true);
            PlaybackTimestampSample pausedBeforeOpen;
            check(waitForStablePlaybackTimestamp(surface, comparisonSourceEpoch,
                                                 &pausedBeforeOpen),
                  "the paused-close case starts from an observed stable timestamp");
            check(reopen(), "Compare captures an originally paused source again");
            compare->close();
            check(!surface->isPlaying() &&
                      playbackTimestampMatches(surface, comparisonSourceEpoch,
                                               pausedBeforeOpen.timestamp),
                  "closing an originally paused Compare preserves pause and timestamp");

            surface->setPaused(false);
            PlaybackTimestampSample moving;
            check(waitForPlaybackProgress(surface, comparisonSourceEpoch,
                                          pausedBeforeOpen.timestamp, &moving),
                  "the explicit-pause case starts from real moving playback");
            check(reopen(), "Compare opens before a newer same-value pause request");
            const auto heldTimestamp = compare->frozenPlaybackTimestamp();
            surface->setPaused(true);
            compare->close();
            check(!surface->isPlaying() &&
                      playbackTimestampMatches(surface, comparisonSourceEpoch, heldTimestamp),
                  "a newer explicit pause wins over Compare's original playing intent");

            check(reopen(), "Compare opens before a newer explicit Play request");
            const auto beforePlay = compare->frozenPlaybackTimestamp();
            surface->setPaused(false);
            PlaybackTimestampSample advanced;
            check(waitForPlaybackProgress(surface, comparisonSourceEpoch, beforePlay, &advanced),
                  "explicit Play advances the live player while Compare is visible");
            check(compare->frozenFrame().isNull() &&
                      compare->renderedSideA().isNull() &&
                      !compare->captureError().isEmpty(),
                  "resumed playback visibly invalidates the old comparison pair");
            compare->close();
            PlaybackTimestampSample stillPlaying;
            check(waitForPlaybackProgress(surface, comparisonSourceEpoch,
                                          advanced.timestamp, &stillPlaying),
                  "closing Compare preserves a newer explicit Play request");

            check(reopen(), "Compare reopens before a source replacement");
            const uint64_t oldEpoch = surface->sourceEpoch();
            check(surface->open(url), "the player accepts a source replacement");
            QTest::qWait(1500);
            check(surface->sourceEpoch() != oldEpoch && compare->frozenFrame().isNull() &&
                      !compare->captureError().isEmpty(),
                  "a new source epoch clears the old frozen input even for the same URL");
            compare->close();
            surface->setPaused(true);
            surface->seek(6.0);
            PlaybackTimestampSample replacementHeld;
            check(waitForStablePlaybackTimestamp(surface, surface->sourceEpoch(),
                                                 &replacementHeld),
                  "the replacement source settles before a new comparison");
            check(reopen() && compare->frozenSourceEpoch() == surface->sourceEpoch(),
                  "reopening Compare captures the replacement source instead of stale pixels");
            compare->close();

            surface->setPaused(false);
            PlaybackTimestampSample pendingMoving;
            check(waitForPlaybackProgress(surface, surface->sourceEpoch(),
                                          replacementHeld.timestamp, &pendingMoving),
                  "the pending-close case starts from moving playback");
            check(reopen(), "Compare opens before pending spatial work");
            const uint64_t pendingEpoch = surface->sourceEpoch();
            const auto pendingTimestamp = compare->frozenPlaybackTimestamp();
            bool closedPending = false;
            QTimer closeTimer;
            closeTimer.setInterval(1);
            QObject::connect(&closeTimer, &QTimer::timeout, compare, [&] {
                if (!compare->comparisonPending()) return;
                closedPending = true;
                closeTimer.stop();
                compare->close();
            });
            closeTimer.start();
            const bool pendingSelected =
                triggerChoice(compare, 0, publicSpatialIds.first());
            closeTimer.stop();
            check(pendingSelected,
                  "the visible menu starts a cancellable spatial comparison");
            if (!pendingSelected) {
                // A refusal upstream leaves the window with no published pane,
                // so there is no side-0 badge to choose from: name that state
                // instead of leaving the pending checks unexplained.
                std::fprintf(stderr,
                             "     pending selection unavailable: panes=%d frozenNull=%d "
                             "capture=%s lock=%s pending=%d held=%d\n",
                             static_cast<int>(visibleComparePanes(compare)),
                             compare->frozenFrame().isNull() ? 1 : 0,
                             qPrintable(compare->captureError()),
                             qPrintable(compare->lockError()),
                             compare->comparisonPending() ? 1 : 0,
                             compare->playbackHeld() ? 1 : 0);
            }
            PlaybackTimestampSample resumedPending;
            check(closedPending && !compare->isVisible() &&
                      waitForPlaybackProgress(surface, pendingEpoch, pendingTimestamp,
                                              &resumedPending),
                  "closing during pending work releases the hold and resumes the live player");
            check(surface->animeJanaiPlan().presetId == originalAjiId &&
                      surface->animeJanaiPlan().backend == ajiBackend &&
                      surface->anime4KChainId() == liveChain,
                  "pending comparison cancellation never replaces live processing");
            surface->setPaused(true);
            check(reopen(), "Compare reopens for source replacement during pending work");
            const uint64_t pendingSourceEpoch = surface->sourceEpoch();
            bool sourceReplacedPending = false;
            QTimer replacePendingSource;
            replacePendingSource.setInterval(1);
            QObject::connect(&replacePendingSource, &QTimer::timeout, compare, [&] {
                if (!compare->comparisonPending()) return;
                replacePendingSource.stop();
                sourceReplacedPending = true;
                surface->open(url);
            });
            replacePendingSource.start();
            check(triggerChoice(compare, 0, publicSpatialIds.last()),
                  "a real spatial selection begins before pending source replacement");
            replacePendingSource.stop();
            QTest::qWait(1000);
            check(sourceReplacedPending && surface->sourceEpoch() != pendingSourceEpoch &&
                      !compare->comparisonPending() && compare->frozenFrame().isNull() &&
                      compare->renderedSideA().isNull() && compare->renderedSideB().isNull() &&
                      !compare->captureError().isEmpty(),
                  "source replacement cancels owned work and publishes no old-source pair");
            compare->close();

            for (int attempt = 0; attempt < 2; ++attempt) {
                const bool escape = attempt == 1;
                surface->setPaused(true);
                surface->seek(6.0);
                PlaybackTimestampSample heldBeforeClose;
                const bool heldSettled = waitForStablePlaybackTimestamp(
                    surface, surface->sourceEpoch(), &heldBeforeClose);
                check(reopen(),
                      escape ? "the window reopened for the Escape close case"
                            : "the window reopened for the programmatic close case");
                if (escape) {
                    QTest::keyClick(compare, Qt::Key_Escape);
                } else {
                    compare->close();
                }
                const bool closedByAffordance =
                    !compare->isVisible() && heldSettled && !surface->isPlaying() &&
                    playbackTimestampMatches(surface, surface->sourceEpoch(),
                                             heldBeforeClose.timestamp) &&
                    !surface->nativePlanLocked() && compare->frozenFrame().isNull();
                check(closedByAffordance,
                      escape
                          ? "Escape closes the window and releases the hold"
                          : "closing the window releases the hold it took for its frozen frame");
                if (!closedByAffordance) {
                    std::fprintf(stderr,
                                 "     %s close: visible=%d heldSettled=%d playing=%d "
                                 "planLocked=%d frozenNull=%d capture=%s\n",
                                 escape ? "escape" : "close", compare->isVisible() ? 1 : 0,
                                 heldSettled ? 1 : 0, surface->isPlaying() ? 1 : 0,
                                 surface->nativePlanLocked() ? 1 : 0,
                                 compare->frozenFrame().isNull() ? 1 : 0,
                                 qPrintable(compare->captureError()));
                }
            }

            dialog->close();
            std::fprintf(stderr, "RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
            app.exit(failures == 0 ? 0 : 1);
            return;
        }

        if (auto* info = compare->findChild<QLabel*>(QString::fromLatin1("compareInfo"))) {
            std::fprintf(stderr, "     info: %s\n", qPrintable(info->text()));
        }

        // ── 6. a second capture is the new frame, not the old one ──
        // The failure this covers is not hypothetical: the window used to
        // re-seek the live player to the position it was already at and then
        // keep whatever the framebuffer happened to hold, so rewinding and
        // capturing again could hand back the frame the viewer had left. The
        // core is parked Off here so the frozen frame is the plain picture and
        surface->setAnime4KChain(offId);
        QTest::qWait(500);

        // Two positions far enough apart that the picture cannot be confused:
        // testsrc2 advances its counters and its colour bars every frame.
        const double firstAt = 3.0;
        const double secondAt = 11.0;
        surface->setPaused(true);

        // Capture once at the first position. The window is closed first
        // because showing an ALREADY-shown window does not re-fire showEvent,
        // and re-opening the kept window is the path under test here.
        compare->close();
        surface->seek(firstAt);
        QTest::qWait(1500);
        emit shell->compareRequested();
        QTest::qWait(3000);
        const QImage firstCapture = compare->frozenFrame();
        check(!firstCapture.isNull(), "the first capture froze a frame");
        check(compare->captureError().isEmpty(), "the first capture reported no error");

        // Now move the player and capture again on the SAME window. If the
        // window were caching, the second capture would equal the first.
        surface->seek(secondAt);
        QTest::qWait(1500);
        const QImage referenceAtSecond = settledFrame(surface);
        check(!referenceAtSecond.isNull(),
              "the player settled on a frame at the second position");
        compare->captureNow();
        QTest::qWait(2500);
        const QImage secondCapture = compare->frozenFrame();
        check(!secondCapture.isNull(), "the second capture froze a frame");
        check(compare->captureError().isEmpty(), "the second capture reported no error");

        // The three-way check: the two captures differ from each other, and
        // the second one matches the picture the player is actually showing.
        const double capturesDiffer = meanDifference(firstCapture, secondCapture);
        const double secondVsReference = meanDifference(secondCapture, referenceAtSecond);
        const double secondVsFirst = meanDifference(secondCapture, firstCapture);
        std::fprintf(stderr, "     first vs second: %.3f / 255\n", capturesDiffer);
        std::fprintf(stderr, "     second vs player: %.3f / 255\n", secondVsReference);
        std::fprintf(stderr, "     second vs first:  %.3f / 255\n", secondVsFirst);
        check(capturesDiffer > 0.05,
              "two captures at two positions are two different frames");
        check(secondVsFirst > 0.05, "the second capture is not the first frame again");
        check(secondVsReference < secondVsFirst,
              "the second capture is closer to the new position than to the old one");

        // Both sides come from one frozen source, so every capture of the same
        // stream has to be the same source size.
        check(!firstCapture.isNull() && firstCapture.size() == secondCapture.size(),
              "both captures are the same source size");

        // ── 7. a chain with no Mode survives a restart ────────────
        // A single .glsl has no enum value, so an ordinal would drop it on
        // every start. The id is what is stored, and it is what is read back.
        surface->setAnime4KChain(single);
        QTest::qWait(400);
        check(surface->anime4KChainId() == single,
              "a chain with no Mode is remembered by its id");

        // ── 8. Compare leaves the player as found ─────────────────
        // Isolation must preserve even a live loose shader absent from the chooser.
        compare->close();
        QTest::qWait(200);
        check(surface->anime4KChainId() == single,
              "closing the window leaves the player's chain unchanged");

        emit shell->compareRequested();
        QTest::qWait(3000);
        check(compare->isVisible(), "the kept window reopens");
        check(!compare->frozenFrame().isNull(), "the reopened window captured a frame");
        check(compare->captureError().isEmpty(), "the reopened capture reported no error");
        check(surface->anime4KChainId() == single,
              "a reopened isolated capture leaves the player's chain unchanged");

        compare->captureNow();
        QTest::qWait(2500);
        check(compare->captureError().isEmpty(), "the repeated capture reported no error");
        check(surface->anime4KChainId() == single,
              "capturing again restores the player's chain");

        check(!compare->isModal(), "the comparison does not take the player hostage");
        compare->close();
        QTest::qWait(400);
        check(!compare->isVisible(), "the window closes on its own");
        check(surface->anime4KChainId() == single,
              "the chain is still the player's after the second close");

        // ── 9. polling is left at its PRIOR value, not switched on ─
        // The invariant is the value the surface actually had, not a fixed
        // true: this source never enabled native status polling, so it has to
        // be found with polling off after a successful capture, a reopen and a
        // close. Switching it on is exactly the failure this checks for.
        surface->setNativeStatusPolling(false);
        check(!surface->nativeStatusPolling(), "native polling is explicitly off on this source");
        compare->captureNow();
        QTest::qWait(2500);
        check(compare->captureError().isEmpty(), "the off-prior capture succeeded");
        check(!surface->nativeStatusPolling(), "an off-prior capture leaves polling off");
        emit shell->compareRequested();
        QTest::qWait(3000);
        check(compare->isVisible(), "the off-prior compare reopens");
        check(!surface->nativeStatusPolling(), "an off-prior reopen leaves polling off");
        compare->close();
        QTest::qWait(300);
        check(!surface->nativeStatusPolling(), "an off-prior close leaves polling off");

        // ── 9b. the native plan is really locked for the window's lifetime ─
        // Pausing the status reader is not locking. The real scoped lock is what
        // freezes planning: it is held while the window is open and released the
        // moment it closes, and a window that cannot hold it names why rather
        // than freezing a plan that is about to move.
        surface->setNativeStatusPolling(true);
        emit shell->compareRequested();
        QTest::qWait(3000);
        check(compare->isVisible(), "the compare window is open for the lock check");
        check(surface->nativePlanLocked() || !compare->lockError().isEmpty() ||
                  !compare->captureError().isEmpty(),
              "an open window holds the plan lock or names why no native base is frozen");
        const QImage lockedFrame = compare->frozenFrame();
        auto* caption = compare->findChild<QLabel*>(QStringLiteral("compareInfo"));
        QString queuedCaption;
        if (surface->nativePlanLocked() && caption != nullptr) {
            // Consumer state transition, not a wording check: the visible
            // caption must change the moment a native request is queued, while
            // the frozen frame keeps its epoch, and it must clear on release.
            const QString before = caption->text();
            surface->turnNativeOff();
            QTest::qWait(150);
            queuedCaption = caption->text();
            std::fprintf(stderr, "     compare caption before: %s\n", qPrintable(before));
            std::fprintf(stderr, "     compare caption queued: %s\n", qPrintable(queuedCaption));
            check(surface->queuedNativeTransitions() == 1,
                  "a native request while the plan is held is queued, not applied");
            check(compare->frozenFrame() == lockedFrame,
                  "the queued native request leaves the frozen frame untouched");
            check(caption->isVisible() && queuedCaption != before,
                  "the visible caption changes the moment a native change is queued");
        }
        compare->close();
        QTest::qWait(300);
        std::fprintf(stderr, "     compare caption released: %s\n",
                     caption != nullptr ? qPrintable(caption->text()) : "(no caption)");
        check(!surface->nativePlanLocked(),
              "closing the window releases the held native plan lock");
        if (!queuedCaption.isEmpty()) {
            check(surface->queuedNativeTransitions() == 0,
                  "closing the window submits the queued native transition");
            check(caption->text() != queuedCaption,
                  "releasing the lock clears the queued state from the visible caption");
        }
        surface->setNativeStatusPolling(false);

        // ── 10. an unsettled capture fails and still restores the surface ──
        // Hidden-window cleanup discards even a successful capture, so keep this refusal visible.
        compare->show();
        QTest::qWait(3000);
        surface->setNativeStatusPolling(true);
        check(surface->nativeStatusPolling(), "native polling is on before the forced failure");
        QTimer churn;
        bool seekFlip = false;
        QObject::connect(&churn, &QTimer::timeout, [&] {
            seekFlip = !seekFlip;
            surface->seek(seekFlip ? 4.0 : 9.0);
        });
        churn.start(20);
        compare->captureNow();
        churn.stop();
        QTest::qWait(200);
        std::fprintf(stderr, "     unsettled refusal: capture=%s lock=%s\n",
                     qPrintable(compare->captureError()), qPrintable(compare->lockError()));
        check(!compare->captureError().isEmpty(), "an unsettled capture refuses by name");
        check(compare->frozenFrame().isNull(), "the unsettled capture froze no frame");
        check(surface->nativeStatusPolling(),
              "a failed capture restores the native status polling it paused");
        check(surface->anime4KChainId() == single, "a failed capture leaves the chain alone");
        surface->setNativeStatusPolling(false);

        // ── 10b. the stored preset is not the live chain, and stays that way ──
        // Saved selection and live loose shader are distinct; preserve both.
        const QString storedKey = QStringLiteral("player/anime4k");
        QSettings persisted;
        persisted.setValue(storedKey, offId);
        persisted.sync();
        check(surface->anime4KChainId() == single && single != offId,
              "the live chain is not the stored preset before the render");
        check(QSettings().value(storedKey).toString() == offId,
              "the stored preset is the off id, not the live chain");
        const QString chainBefore = surface->anime4KChainId();
        const QString propertyBefore = surface->effectiveShaderChain();
        const auto pickPreset = [&](int chipIndex, const QString& id) {
            int seen = -1;
            for (QToolButton* chip : compare->findChildren<QToolButton*>()) {
                if (chip->menu() == nullptr) {
                    continue;
                }
                if (++seen != chipIndex) {
                    continue;
                }
                std::function<bool(QMenu*)> pick = [&](QMenu* menu) -> bool {
                    for (QAction* action : menu->actions()) {
                        if (QMenu* submenu = action->menu()) {
                            if (pick(submenu)) {
                                return true;
                            }
                        } else if (action->data().toString() == id) {
                            action->trigger();
                            return true;
                        }
                    }
                    return false;
                };
                return pick(chip->menu());
            }
            return false;
        };
        check(pickPreset(0, selectablePresetIds.value(0)),
              "the first badge accepts an available preset pick");
        check(surface->anime4KChainId() == chainBefore,
              "picking side A leaves the live chain id unchanged");
        check(surface->effectiveShaderChain() == propertyBefore,
              "picking side A leaves glsl-shaders as found");
        check(pickPreset(1, selectablePresetIds.value(1)),
              "the second badge accepts another available preset pick");
        check(surface->anime4KChainId() == chainBefore,
              "picking side B leaves the live chain id unchanged");
        check(surface->effectiveShaderChain() == propertyBefore,
              "picking side B leaves glsl-shaders as found");
        compare->close();
        QTest::qWait(200);
        check(surface->anime4KChainId() == chainBefore,
              "closing after both picks leaves the live chain id unchanged");
        check(surface->effectiveShaderChain() == propertyBefore,
              "closing after both picks leaves glsl-shaders as found");
        check(QSettings().value(storedKey).toString() == offId,
              "compare never rewrites the stored preset");

        // ── 10c. a failed capture hands playback back as it found it ──
        compare->show();
        QTest::qWait(3000);
        surface->setNativeStatusPolling(true);
        surface->setPaused(false);
        QTest::qWait(200);
        check(surface->isPlaying(), "the surface is playing before the failed capture");
        check(surface->nativeStatusPolling(), "native polling is on before the failed capture");
        QTimer churnPlayback;
        const double repeatedSeekTarget = surface->position();
        QObject::connect(&churnPlayback, &QTimer::timeout, [&] {
            surface->seek(repeatedSeekTarget);
        });
        churnPlayback.start(20);
        compare->captureNow();
        churnPlayback.stop();
        QTest::qWait(200);
        std::fprintf(stderr, "     playback refusal: capture=%s lock=%s\n",
                     qPrintable(compare->captureError()), qPrintable(compare->lockError()));
        check(!compare->captureError().isEmpty(), "a playback capture that cannot settle refuses");
        check(compare->frozenFrame().isNull(), "the playback refusal froze no frame");
        check(surface->isPlaying(), "a failed capture hands playback back playing");
        check(surface->anime4KChainId() == single, "the playback refusal leaves the chain alone");
        check(surface->nativeStatusPolling(), "the playback refusal leaves polling as found");
        surface->setPaused(true);
        surface->setNativeStatusPolling(false);

        // ── 11. a closed surface is refused by name ───────────────
        // Closing the borrowed surface is a REAL capture failure: hasVideo()
        // goes false while the widget lives on. The window must name the
        // refusal rather than freeze an old or empty picture, and it must not
        // touch the chain it borrowed. This goes through the window's own
        // public capture path, not the dialog's open guard.
        surface->close();
        QTest::qWait(250);
        check(!surface->hasVideo(), "the player surface reports no video after close");
        compare->captureNow();
        QTest::qWait(250);
        std::fprintf(stderr, "     closed-surface refusal: %s\n",
                     qPrintable(compare->captureError()));
        check(!compare->captureError().isEmpty(),
              "a capture on a closed surface refuses instead of freezing");
        check(compare->frozenFrame().isNull(), "the refused capture froze no frame");
        check(surface->anime4KChainId() == single,
              "the closed surface keeps the chain it was left on");
        check(!surface->nativeStatusPolling(),
              "a failed capture leaves polling at its prior off, not switched on");

        // ── 12. a surface with no video is refused by name ────────
        // A second, never-opened surface proves the refusal does not depend on
        // the live player: the window borrows whatever surface it is handed,
        // and one with no file loaded must refuse rather than freeze.
        VideoSurface idleSurface;
        idleSurface.setAnime4KChain(single);
        check(!idleSurface.hasVideo(), "a fresh surface reports no video");
        CompareWindow idleCompare(&idleSurface, idleSurface.anime4KChainId());
        idleCompare.captureNow();
        QTest::qWait(200);
        std::fprintf(stderr, "     no-video refusal: %s\n",
                     qPrintable(idleCompare.captureError()));
        check(!idleCompare.captureError().isEmpty(),
              "a capture on a surface with no video refuses by name");
        check(idleCompare.frozenFrame().isNull(), "the no-video capture froze no frame");
        check(idleSurface.anime4KChainId() == single,
              "the no-video surface keeps the chain it was handed");

        // The degenerate case: no surface at all. It has to name the missing
        // surface rather than dereference it.
        CompareWindow noSurface(nullptr, selectablePresetIds.value(0));
        noSurface.captureNow();
        QTest::qWait(50);
        std::fprintf(stderr, "     no-surface refusal: %s\n",
                     qPrintable(noSurface.captureError()));
        check(!noSurface.captureError().isEmpty(), "a capture with no surface refuses by name");
        check(noSurface.frozenFrame().isNull(), "the no-surface capture froze no frame");

        // ── 13. a surface destroyed mid-grab is refused, not dereferenced ──
        // The borrowed surface can die while a capture is pumping the event
        // loop. The QPointer guard has to turn that into a named refusal rather
        // than a read of freed memory. This surface really has video - it is
        // made to freeze a working frame first - and is then deleted from a
        // timer while the NEXT grab is still waiting for it to settle.
        VideoSurface* borrowed = new VideoSurface();
        borrowed->resize(320, 240);
        const bool borrowedOpened = borrowed->open(url);
        for (int attempt = 0; borrowedOpened && !borrowed->hasVideo() && attempt < 200; ++attempt) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }
        if (borrowedOpened && borrowed->hasVideo()) {
            auto* guardWindow = new CompareWindow(borrowed, selectablePresetIds.value(0));
            guardWindow->captureNow();
            check(!guardWindow->captureError().isEmpty() || !guardWindow->frozenFrame().isNull(),
                  "the second surface produced a real frame before the guard check");
            QTimer guardChurn;
            bool guardFlip = false;
            QObject::connect(&guardChurn, &QTimer::timeout, [&] {
                guardFlip = !guardFlip;
                borrowed->seek(guardFlip ? 4.0 : 9.0);
            });
            guardChurn.start(20);
            QTimer guardKill;
            guardKill.setSingleShot(true);
            QObject::connect(&guardKill, &QTimer::timeout, [&] {
                guardChurn.stop();
                delete borrowed;
                borrowed = nullptr;
            });
            guardKill.start(150);
            guardWindow->captureNow();
            guardChurn.stop();
            if (borrowed != nullptr) {
                delete borrowed;
                borrowed = nullptr;
            }
            std::fprintf(stderr, "     destroyed-surface refusal: %s\n",
                         qPrintable(guardWindow->captureError()));
            check(!guardWindow->captureError().isEmpty(),
                  "a surface destroyed mid-grab is refused by name");
            check(guardWindow->frozenFrame().isNull(),
                  "a surface destroyed mid-grab kept no frame");
            delete guardWindow;
        } else {
            std::fprintf(stderr,
                         "SKIP  no second surface for the destroyed-mid-grab guard check\n");
            delete borrowed;
        }

        std::fprintf(stderr, "RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
        app.exit(failures == 0 ? 0 : 1);
    });
    return app.exec();
}
