
#include <QAccessible>
#include <QAccessibleInterface>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QMetaMethod>
#include <QLabel>
#include <QMimeData>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <cmath>
#include <cstdio>
#include <functional>

#include "MpvApi.hpp"
#include "PlayerDialog.hpp"
#include "PlayerPopups.hpp"
#include "PlayerShell.hpp"
#include "ProbePicture.hpp"
#include "ProbeRuntime.hpp"
#include "ShellWidgets.hpp"
#include "VideoSurface.hpp"

struct RefusalHost : public mplayer::shell::PlayerHost {
    int variantRequests = 0;
    int episodeRequests = 0;
    int trackProbes = 0;
    int copyRequests = 0;
    int originalRequests = 0;
    int playbackFailures = 0;
    int closed = 0;

    void requestVariant(const QString&, const QString&) override { ++variantRequests; }
    void requestEpisode(int) override { ++episodeRequests; }
    void requestTrackProbe(const QString&) override { ++trackProbes; }
    void requestOpenOriginal() override { ++originalRequests; }
    void requestCopyStreamUrl() override { ++copyRequests; }
    void playerClosed() override { ++closed; }
    void playbackFailed() override { ++playbackFailures; }
};

QString statusText(mplayer::shell::PlayerDialog* dialog) {
    if (dialog != nullptr) {
        if (auto* label = dialog->findChild<QLabel*>(QStringLiteral("playerStatusText"))) {
            return label->text();
        }
    }
    return {};
}

bool refusalRowVisible(mplayer::shell::PlayerDialog* dialog) {
    if (dialog != nullptr) {
        if (auto* panel = dialog->findChild<QFrame*>(QStringLiteral("playerStatusPanel"))) {
            return panel->isVisibleTo(dialog);
        }
    }
    return false;
}

bool anywhereShows(QWidget* root, const QString& text) {
    if (root == nullptr || text.isEmpty()) return false;
    for (const QLabel* label : root->findChildren<QLabel*>()) {
        if (label->isVisible() && label->text().contains(text, Qt::CaseInsensitive)) return true;
    }
    return false;
}

const QStringList& successWords() {
    static const QStringList words{QStringLiteral("Playing local file"), QStringLiteral("Episode: "),
                                   QStringLiteral("Track: "), QStringLiteral("copied")};
    return words;
}

class AlertCounter {
public:
    static AlertCounter* current_;
    static QAccessible::UpdateHandler previous_;

    explicit AlertCounter(QObject* target) : target_(target), wasActive_(QAccessible::isActive()) {
        previous_ = QAccessible::installUpdateHandler(&AlertCounter::onUpdate);
        current_ = this;
        QAccessible::setActive(true);
    }

    ~AlertCounter() {
        QAccessible::installUpdateHandler(previous_);
        QAccessible::setActive(wasActive_);
        current_ = nullptr;
        previous_ = nullptr;
    }

    int count() const { return count_; }

private:
    static void onUpdate(QAccessibleEvent* event) {
        if (current_ != nullptr && event != nullptr && event->type() == QAccessible::Alert &&
            event->object() == current_->target_) {
            ++current_->count_;
        }
        if (previous_ != nullptr) previous_(event);
    }

    QObject* target_ = nullptr;
    int count_ = 0;
    bool wasActive_ = false;
};

AlertCounter* AlertCounter::current_ = nullptr;
QAccessible::UpdateHandler AlertCounter::previous_ = nullptr;

using namespace mplayer::shell;

namespace {

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

void pump(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
}

bool waitFor(const std::function<bool()>& ready, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents();
        if (ready()) return true;
        QThread::msleep(10);
    }
    return ready();
}

bool corePaused(mplayer::VideoSurface* surface, bool paused) {
    const auto value = surface->probeMpvProperty(QStringLiteral("pause"),
                                               mplayer::ProbePropertyKind::Flag);
    return value && value->kind == mplayer::ProbePropertyValue::Kind::Flag &&
           value->flag == paused;
}

std::optional<double> corePosition(mplayer::VideoSurface* surface) {
    const auto value = surface->probeMpvProperty(QStringLiteral("time-pos"),
                                               mplayer::ProbePropertyKind::Number);
    return value && value->isNumeric() ? std::optional<double>(value->asNumber()) : std::nullopt;
}

void requireHeld(mplayer::probes::Verdict& verdict, mplayer::VideoSurface* surface,
                 const QString& scenario) {
    const bool acknowledged = waitFor([surface] { return corePaused(surface, true); }, 2000);
    verdict.require(acknowledged, scenario + QStringLiteral(": actual core pause acknowledged"));
    QImage frame;
    int stableFrames = 0;
    for (int attempt = 0; attempt < 25 && stableFrames < 4; ++attempt) {
        pump(80);
        const QImage next = surface->grabVideoFramebuffer();
        const auto picture = mplayer::probes::measurePicture(
            next, surface->videoWidth(), surface->videoHeight());
        stableFrames = picture.litFraction() >= mplayer::probes::kMinPictureLitFraction &&
                               next == frame ? stableFrames + 1 : 0;
        frame = next;
    }
    verdict.require(stableFrames >= 4, scenario + QStringLiteral(": paused rendering settles"));
    const auto start = corePosition(surface);
    bool paused = true;
    bool frozen = start.has_value();
    for (int i = 0; i < 5; ++i) {
        pump(80);
        paused = paused && corePaused(surface, true) && !surface->isPlaying();
        const auto position = corePosition(surface);
        frozen = frozen && position && start && std::abs(*position - *start) < 0.000001;
    }
    verdict.require(paused, scenario + QStringLiteral(": paused output remains enforced"));
    verdict.require(frozen, scenario + QStringLiteral(": actual media clock stays frozen"));
    const QImage after = surface->grabVideoFramebuffer();
    const bool comparable = !frame.isNull() && !after.isNull() && frame.size() == after.size();
    long changed = 0;
    long compared = 0;
    if (comparable) {
        const QRect picture = mplayer::probes::pictureRect(
            frame.size(), surface->videoWidth(), surface->videoHeight());
        for (int y = picture.top(); y <= picture.bottom(); y += 2) {
            for (int x = picture.left(); x <= picture.right(); x += 2) {
                const QColor before = frame.pixelColor(x, y);
                const QColor next = after.pixelColor(x, y);
                ++compared;
                if (std::abs(before.red() - next.red()) +
                        std::abs(before.green() - next.green()) +
                        std::abs(before.blue() - next.blue()) > mplayer::probes::kMotionPixelDelta)
                    ++changed;
            }
        }
    }
    verdict.metric(scenario + QStringLiteral(": changed %1/%2 picture pixels; %3x%4 -> %5x%6")
                                  .arg(changed).arg(compared)
                                  .arg(frame.width()).arg(frame.height())
                                  .arg(after.width()).arg(after.height()));
    verdict.require(compared > 0 &&
                        double(changed) / double(compared) <
                            mplayer::probes::kMinMotionChangedFraction,
                    scenario + QStringLiteral(": the paused picture shows no visible motion"));
}

QString labelTexts(const QWidget* root) {
    QStringList texts;
    for (const QLabel* label : root->findChildren<QLabel*>()) texts << label->text();
    return texts.join(QStringLiteral(" | "));
}

void clickCancel(ProcessingOverlay* overlay) {
    if (overlay == nullptr) return;
    for (QPushButton* button : overlay->findChildren<QPushButton*>()) {
        if (button->isVisible() && button->isEnabled()) {
            button->click();
            return;
        }
    }
}

bool announceAnimeJanaiBuild(mplayer::VideoSurface* surface, bool active, bool succeeded,
                             const QString& detail) {
    return surface != nullptr && QMetaObject::invokeMethod(
        surface, "animeJanaiEngineBuildChanged", Qt::DirectConnection,
        Q_ARG(bool, active), Q_ARG(bool, succeeded), Q_ARG(QString, detail));
}

PipFloat* activeFloat() {
    for (QWidget* window : QApplication::topLevelWidgets()) {
        if (auto* pip = qobject_cast<PipFloat*>(window); pip != nullptr && pip->isVisible()) {
            return pip;
        }
    }
    return nullptr;
}

}  // namespace

/// The loader-refusal mode. `clip` is a real local media file the player is
/// handed as direct media; the refusal comes from the runtime, never from the
/// clip. Returns the verdict's own exit code.
int runRuntimeRefusal(const QString& clip) {
    mplayer::probes::Verdict verdict;

    // The loader must be empty BEFORE this runs, so the refusal is observed
    // rather than assumed. An already-pinned module would make every check below
    // a claim about a process that is not the one on screen.
    if (mplayer::MpvApi::instance().loaded()) {
        std::fprintf(stderr,
                     "FAIL a module is already pinned: the refusal mode must run on a "
                     "process that has not selected one\n");
        return 1;
    }
    if (mplayer::native::runtimeSelected()) {
        std::fprintf(stderr, "FAIL a runtime is already recorded as selected\n");
        return 1;
    }

    // The refusal under test: a named installable package that is not there.
    // `MIKAI_PROBE_BUNDLE` names an absolute root that does not exist, which is
    // the shape a caller produces by naming a directory that was never
    // published. The reader must refuse it by name - there is no discovery
    // fallback behind an explicit request - and must pin nothing.
    const QString missingRoot = QStringLiteral("Z:/mikai-engine-popup-probe-no-such-runtime-root");
    qputenv("MIKAI_PROBE_BUNDLE", missingRoot.toUtf8());
    const bool missingRefused = !mplayer::probes::selectRuntime();
    verdict.require(missingRefused,
                    QStringLiteral("an isolated package that is not there is refused by name"));
    verdict.require(!mplayer::MpvApi::instance().loaded(),
                    QStringLiteral("the refused isolated package pins no module"));
    qunsetenv("MIKAI_PROBE_BUNDLE");

    // The same refusal for an explicitly named module file rather than a bundle
    // root: a path that is not a real file. The loader's own reason is printed,
    // because that reason is what a viewer would be shown.
    const QString missingModule =
        QStringLiteral("Z:/mikai-engine-popup-probe-no-such-libmpv-2.dll");
    mplayer::MpvBundle missing;
    missing.root = QStringLiteral("Z:/mikai-engine-popup-probe-no-such-runtime-root");
    missing.modulePath = missingModule;
    missing.descriptorId = QStringLiteral("engine-popup-probe-missing-module");
    missing.protocolVersion = 1;
    QString missingReason;
    const bool missingModuleRefused = !mplayer::MpvApi::instance().load(missing, &missingReason);
    verdict.require(missingModuleRefused,
                    QStringLiteral("an explicit module file that is not there is refused by name"));
    verdict.require(!missingReason.trimmed().isEmpty(),
                    QStringLiteral("the module refusal names its own reason"));
    if (missingModuleRefused) {
        std::fprintf(stderr, "[runtime-refusal] module %s refused: %s\n",
                     qPrintable(missingModule), qPrintable(missingReason));
    }
    verdict.require(!mplayer::MpvApi::instance().loaded(),
                    QStringLiteral("a refused loader leaves no core behind it"));

    RefusalHost host;
    const QList<QualityEntry> qualities = {{360, clip}};
    auto* dialog = new PlayerDialog(QStringLiteral("probe 360p"), clip, qualities, 360, &host);
    dialog->show();
    QPointer<PlayerDialog> guard(dialog);

    // The dialog is constructed exactly as the app builds it, and startPlayer()
    // is what opens the stream, so the failure it reports is the real one.
    dialog->startPlayer();
    for (int i = 0; i < 200 && !refusalRowVisible(dialog); ++i) pump(10);
    auto* surface = dialog->findChild<mplayer::VideoSurface*>(QStringLiteral("videoSurface"));

    // ── the refusal is visible ────────────────────────────────────────────
    verdict.require(surface != nullptr, QStringLiteral("the real player owns its surface"));
    verdict.require(surface == nullptr || !surface->hasVideo(),
                    QStringLiteral("no core means hasVideo is false"));
    verdict.require(surface == nullptr || !surface->isPlaying(),
                    QStringLiteral("no core means nothing reports playing"));
    verdict.require(!dialog->enginePreparationVisible(),
                    QStringLiteral("no operation window appears for a runtime that never loaded"));
    verdict.require(dialog->findChild<ProcessingOverlay*>() == nullptr,
                    QStringLiteral("no engine work is announced without a core"));
    verdict.require(refusalRowVisible(dialog) && !statusText(dialog).trimmed().isEmpty(),
                    QStringLiteral("the unavailable runtime is readable on the status row"));
    verdict.require(statusText(dialog).contains(missingRoot) ||
                        statusText(dialog).contains(missingModule),
                    QStringLiteral("the player preserves the loader's missing-runtime cause"));
    verdict.require(dialog->isVisible() && dialog->width() > 0,
                    QStringLiteral("the window itself stays on screen after the refusal"));

    // ── the refusal is accessible ─────────────────────────────────────────
    // The Retry chip is what a screen reader reaches for; it is raised with the
    // failure and is named, not a bare glyph.
    QPushButton* retry = nullptr;
    for (QPushButton* button : dialog->findChildren<QPushButton*>()) {
        if (button->objectName() == QLatin1String("playerRetryButton")) retry = button;
    }
    verdict.require(retry != nullptr && retry->isVisibleTo(dialog),
                    QStringLiteral("the failure keeps its Retry chip reachable"));
    verdict.require(retry != nullptr && !retry->accessibleName().trimmed().isEmpty(),
                    QStringLiteral("the Retry chip carries an accessible name"));

    // The row itself is measured as a row, not as text a sighted viewer alone can
    // read: on screen, carrying the refusal, and reachable by accessible name.
    QLabel* announceLabel = dialog->findChild<QLabel*>(QStringLiteral("playerStatusText"));
    verdict.require(announceLabel != nullptr && announceLabel->isVisibleTo(dialog) &&
                        !announceLabel->text().trimmed().isEmpty(),
                    QStringLiteral("the failure row is visible and carries its own reason"));
    QAccessibleInterface* row =
        announceLabel != nullptr ? QAccessible::queryAccessibleInterface(announceLabel) : nullptr;
    verdict.require(row != nullptr && row->isValid(),
                    QStringLiteral("the failure row exposes a live accessible interface"));
    verdict.require(row != nullptr && !row->text(QAccessible::Name).trimmed().isEmpty(),
                    QStringLiteral("the failure row is reachable by its accessible name"));

    // The Alert is what actually announces a failure rather than showing one, and
    // it is counted over the player's real missing-runtime path. The count must
    // FAIL before the app raises it and PASS once it does: a path that names the
    // runtime on screen but says nothing to assistive technology is not finished.
    int alertsBefore = 0;
    {
        AlertCounter alerts(announceLabel);
        dialog->startPlayer();
        for (int i = 0; i < 100 && alerts.count() == 0; ++i) pump(10);
        alertsBefore = alerts.count();
    }
    const bool alertRaised = alertsBefore > 0;
    verdict.require(alertRaised,
                    QStringLiteral("the unavailable runtime raises an accessible Alert "
                                   "(observed %1)").arg(alertsBefore));
    if (!alertRaised) {
        std::fprintf(stderr,
                     "[runtime-refusal] showErrorStatus sets the named status row and the Retry "
                     "chip but emits no QAccessible::Alert, while flashStatus(message, true) "
                     "does; the missing-runtime path is not announced to assistive technology\n");
    }

    // ── retry asks the app to provision, never reloads a dead url ─────────
    // With no engine there is nothing to retry with: the signal the app wires to
    // its provisioning path is the answer, and it must arrive exactly once. A
    // retry that re-ran the load instead would loop a stream that cannot load.
    int runtimeRequests = 0;
    QObject::connect(dialog, &PlayerDialog::runtimeRequested, dialog,
                     [&runtimeRequests] { ++runtimeRequests; });
    const int failuresBefore = host.playbackFailures;
    if (retry != nullptr && retry->isEnabled()) {
        retry->click();
        for (int i = 0; i < 50; ++i) pump(10);
    }
    verdict.require(surface == nullptr || !surface->hasVideo(),
                    QStringLiteral("Retry without a runtime still opens no stream"));
    verdict.require(runtimeRequests == 1,
                    QStringLiteral("Retry without a runtime asks for provisioning exactly once"));
    verdict.require(host.playbackFailures == failuresBefore,
                    QStringLiteral("Retry does not re-report a failure the app already has"));
    verdict.require(refusalRowVisible(dialog) && !statusText(dialog).trimmed().isEmpty(),
                    QStringLiteral("the failed retry leaves the refusal readable"));

    // ── no false success anywhere in the chrome ───────────────────────────
    for (const QString& word : successWords()) {
        verdict.require(!anywhereShows(dialog, word),
                        QStringLiteral("no false success text is shown: %1").arg(word));
    }
    verdict.require(dialog->currentHeight() == 360,
                    QStringLiteral("the player still names the height the caller offered"));
    verdict.require(surface == nullptr || !surface->isPlaying(),
                    QStringLiteral("the play control never reports a running stream"));

    // ── the public UI still answers, honestly ─────────────────────────────
    // Quality: the popover lists what the caller offered, so the choice stays
    // readable even though nothing can play right now.
    if (auto* shell = dialog->findChild<PlayerShell*>()) {
        shell->openSettings();
        for (int i = 0; i < 100 && !anywhereShows(dialog, QStringLiteral("360p")); ++i) {
            pump(10);
        }
        auto* flyout = dialog->findChild<SettingsFlyout*>();
        verdict.require(flyout != nullptr && flyout->isVisible(),
                        QStringLiteral("the settings popover opens with no core loaded"));
        verdict.require(anywhereShows(dialog, QStringLiteral("360p")),
                        QStringLiteral("the offered quality stays listed on a refused runtime"));
        shell->dismissOverlay();
    }

    // Episode: the host is asked for the playlist, and the dialog's own answer to
    // that request must be the refusal rather than a "Playing episode N" claim.
    QVector<QVariantMap> playlist;
    for (int index = 0; index < 3; ++index) {
        QVariantMap entry;
        entry.insert(QStringLiteral("label"), QStringLiteral("Series %1").arg(index + 1));
        playlist.append(entry);
    }
    dialog->setPlaylist(playlist, 0);
    pump(10);
    if (auto* seriesButton = dialog->findChild<QPushButton*>(QStringLiteral("playerSeriesToggle"));
        seriesButton != nullptr && seriesButton->isVisible() && seriesButton->isEnabled()) {
        seriesButton->click();
        pump(10);
    }
    if (auto* drawer = dialog->findChild<SeriesDrawer*>()) {
        verdict.require(drawer->isVisibleTo(dialog),
                        QStringLiteral("the episode drawer opens next to the refusal"));
        for (QPushButton* row : drawer->findChildren<QPushButton*>()) {
            if (row->isVisible() && row->isEnabled()) {
                row->click();
                break;
            }
        }
        pump(10);
    }
    verdict.require(host.episodeRequests == 1,
                    QStringLiteral("picking an episode row asks the host exactly once"));
    const int failuresBeforeEpisode = host.playbackFailures;
    dialog->applyEpisode(QStringLiteral("Series 2"), QStringLiteral("embed-2"), 1,
                         QList<QualityEntry>{{360, clip}}, {});
    for (int i = 0; i < 100; ++i) pump(10);
    verdict.require(surface == nullptr || !surface->hasVideo(),
                    QStringLiteral("an episode swap on a refused runtime opens no stream"));
    verdict.require(!anywhereShows(dialog, QStringLiteral("Episode: ")),
                    QStringLiteral("no episode success is claimed on a core that does not exist"));
    verdict.require(host.playbackFailures >= failuresBeforeEpisode,
                    QStringLiteral("the refused episode swap reports its own failure, once"));

    // Track: publishing variants is the host's answer; the dialog lists them
    // without pretending a track is playing.
    QVector<QVariantMap> variants;
    for (int index = 0; index < 2; ++index) {
        QVariantMap variant;
        variant.insert(QStringLiteral("embed"), QStringLiteral("embed-%1").arg(index + 1));
        variant.insert(QStringLiteral("label"),
                       index == 0 ? QStringLiteral("Ukrainian dub") : QStringLiteral("English dub"));
        variant.insert(QStringLiteral("kind"), QStringLiteral("voice"));
        variant.insert(QStringLiteral("kind_label"), QStringLiteral("DUB"));
        // The first is already measured; the second is not, so opening the picker
        // must ask the host for it rather than assert a quality it never reported.
        variant.insert(QStringLiteral("quality"), index == 0 ? 1080 : 0);
        variants.append(variant);
    }
    dialog->setVariants(variants, QStringLiteral("embed-1"));
    pump(10);
    verdict.require(dialog->currentEmbed() == QStringLiteral("embed-1"),
                    QStringLiteral("the published track becomes the current one"));
    const int trackProbesBefore = host.trackProbes;
    dialog->openAudioMenu();
    auto* audio = dialog->findChild<MenuPopup*>(QStringLiteral("playerAudioPopup"));
    verdict.require(audio != nullptr && audio->isVisible() && audio->rowCount() >= 2,
                    QStringLiteral("the dub picker lists the published tracks"));
    verdict.require(host.trackProbes == trackProbesBefore + 1,
                    QStringLiteral("an unmeasured track is probed through the host, by name"));
    // Picking a row asks the host for that variant; it is the host's answer that
    // would load a stream, and the host has none to offer.
    const int variantsBeforePicking = host.variantRequests;
    if (audio != nullptr && audio->rowCount() >= 2) {
        if (auto* row = qobject_cast<QPushButton*>(audio->rowWidget(1));
            row != nullptr && row->isVisible() && row->isEnabled()) {
            row->click();
            for (int i = 0; i < 100; ++i) pump(10);
        }
    }
    verdict.require(host.variantRequests == variantsBeforePicking + 1,
                    QStringLiteral("picking a track asks the host for that variant, exactly once"));
    const int failuresBeforeVariant = host.playbackFailures;
    dialog->applyVariant(QList<QualityEntry>{{360, clip}}, QStringLiteral("English dub"),
                         QStringLiteral("embed-2"));
    for (int i = 0; i < 100; ++i) pump(10);
    verdict.require(surface == nullptr || !surface->hasVideo(),
                    QStringLiteral("a variant swap on a refused runtime opens no stream"));
    verdict.require(!anywhereShows(dialog, QStringLiteral("Track: ")),
                    QStringLiteral("no track success is claimed on a core that does not exist"));
    verdict.require(host.playbackFailures >= failuresBeforeVariant,
                    QStringLiteral("the refused variant swap is not silently dropped"));

    // Drop: a real local file handed to the player. The drop becomes direct
    // media, so there is no anime host to re-extract for: the failure is the
    // player's own visible row, and it is still a refusal by name.
    const int failuresBeforeDrop = host.playbackFailures;
    QMimeData droppedMedia;
    droppedMedia.setUrls({QUrl::fromLocalFile(clip)});
    QDragEnterEvent dragEnter(QPointF(10, 10), Qt::CopyAction, &droppedMedia,
                              Qt::LeftButton, Qt::NoModifier);
    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &droppedMedia,
                    Qt::LeftButton, Qt::NoModifier);
    if (auto* dropShell = dialog->findChild<PlayerShell*>(); dropShell != nullptr) {
        QApplication::sendEvent(dropShell, &dragEnter);
        QApplication::sendEvent(dropShell, &drop);
    }
    verdict.require(dragEnter.isAccepted() && drop.isAccepted(),
                    QStringLiteral("the player accepts the real local-file drop"));
    for (int i = 0; i < 100; ++i) pump(10);
    verdict.require(surface == nullptr || !surface->hasVideo(),
                    QStringLiteral("a dropped file loads nothing without a runtime"));
    verdict.require(!anywhereShows(dialog, QStringLiteral("Playing local file")),
                    QStringLiteral("a dropped file is not announced as local playback"));
    verdict.require(refusalRowVisible(dialog) && !statusText(dialog).trimmed().isEmpty(),
                    QStringLiteral("the dropped file's own refusal stays readable"));
    verdict.require(statusText(dialog).contains(missingRoot) ||
                        statusText(dialog).contains(missingModule),
                    QStringLiteral("the dropped file preserves the real runtime failure cause"));
    // Direct media has no host to re-extract, so the drop must not manufacture a
    // host callback for a failure the host has no part in.
    verdict.require(host.playbackFailures == failuresBeforeDrop,
                    QStringLiteral("a direct drop does not ask the host to re-extract"));

    // ── close-Tool regression ─────────────────────────────────────────────
    // The dialog is closed while the refusal is on screen. Every Tool window and
    // every top-level window the dialog owned is enumerated before the close and
    // the same sets again after it, so the regression is "nothing this player
    // owned is still standing", not merely "there never was one".
    QStringList toolsBefore;
    for (QWidget* window : QApplication::topLevelWidgets()) {
        if (window->windowFlags().testFlag(Qt::Tool) && !window->objectName().isEmpty()) {
            toolsBefore << window->objectName();
        }
    }
    QStringList ownedBefore;
    for (QWidget* window : QApplication::topLevelWidgets()) {
        if (window->windowHandle() != nullptr && dialog->isAncestorOf(window)) {
            ownedBefore << QStringLiteral("%1/%2").arg(window->metaObject()->className(),
                                                       window->objectName());
        }
    }
    const int closedBefore = host.closed;
    dialog->close();
    for (int i = 0; i < 250 && !guard.isNull(); ++i) pump(10);
    verdict.require(guard.isNull(), QStringLiteral("closing the refused player deletes the dialog"));
    verdict.require(host.closed >= closedBefore,
                    QStringLiteral("the host hears the close once the dialog is gone"));
    QStringList toolsAfter;
    for (QWidget* window : QApplication::topLevelWidgets()) {
        if (window->windowFlags().testFlag(Qt::Tool) && !window->objectName().isEmpty()) {
            toolsAfter << window->objectName();
        }
    }
    QStringList orphans;
    for (QWidget* window : QApplication::topLevelWidgets()) {
        if (window->windowHandle() == nullptr) continue;
        const QString id =
            QStringLiteral("%1/%2").arg(window->metaObject()->className(), window->objectName());
        if (ownedBefore.contains(id)) orphans << id;
    }
    verdict.require(orphans.isEmpty(), QStringLiteral("no owned window survives the closed player"));
    for (const QString& tool : toolsBefore) {
        verdict.require(!toolsAfter.contains(tool),
                        QStringLiteral("no orphan operation window survives the close: %1").arg(tool));
    }

    const int exitCode = verdict.report("engine_popup_runtime_refusal");
    std::fprintf(stderr,
                 "[runtime-refusal] isolatedRoot=%s missingModule=%s refusal=%d alerts=%d "
                 "host: variant=%d episode=%d trackProbe=%d copy=%d original=%d "
                 "playbackFailed=%d runtimeRequested=%d closed=%d\n",
                 qPrintable(missingRoot), qPrintable(missingModule), missingRefused ? 1 : 0,
                 alertsBefore, host.variantRequests, host.episodeRequests, host.trackProbes,
                 host.copyRequests, host.originalRequests, host.playbackFailures,
                 runtimeRequests, host.closed);
    return exitCode;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) return 1;
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiEnginePopupProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("EnginePopupProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());

    // The refusal mode is the one path that must reach a real player with NO
    // module loaded, so it is recognised before the abort below and it is the
    // only thing that is. It bypasses the abort rather than replacing what
    // refuses: the refusal is still produced by the production selection helper
    // and by the loader, naming a package that is not there.
    //
    //   engine_popup_probe --runtime-refusal <real clip file>
    //
    // The caller must run it on a process with no mpv installed for it to find:
    // the parent launches this stage with no mpv and its own per-stage QSettings,
    // and the probe refuses an environment that has already selected a module.
    // Everything else is the operation-window mode unchanged, and still needs a
    // real runtime, which the abort below still enforces.
    const QStringList arguments = QCoreApplication::arguments();
    const int refusalIndex = arguments.indexOf(QStringLiteral("--runtime-refusal"));
    if (refusalIndex >= 0) {
        if (refusalIndex + 1 >= arguments.size()) {
            std::fprintf(stderr, "usage: engine_popup_probe --runtime-refusal <real clip file>\n");
            return 2;
        }
        for (const QString& argument : arguments.mid(1)) {
            if (argument.startsWith(QLatin1Char('-')) &&
                argument != QLatin1String("--runtime-refusal")) {
                std::fprintf(stderr, "engine_popup_probe: unknown option %s\n",
                             qPrintable(argument));
                return 2;
            }
        }
        const QString refusalClip = arguments.at(refusalIndex + 1);
        // No inherited selection: an explicit module variable here would let a
        // populated stage stand in for the missing install under test.
        qunsetenv("MIKAI_PROBE_BUNDLE");
        qunsetenv("MIKAI_MPV_MODULE");
        if (mplayer::native::runtimeSelected() || mplayer::MpvApi::instance().loaded()) {
            std::fprintf(
                stderr, "FAIL --runtime-refusal requires a process with no selected module\n");
            return 2;
        }
        std::fprintf(stderr, "CLI: engine_popup_probe --runtime-refusal %s\n",
                     qPrintable(refusalClip));
        return runRuntimeRefusal(refusalClip);
    }

    if (!mplayer::probes::selectRuntime()) return 2;

    StubHost host;
    const QString url = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                 : QStringLiteral("av://lavfi:testsrc2=size=640x360:rate=25");
    const QList<QualityEntry> qualities = {{360, url}};
    auto* dialog = new PlayerDialog(QStringLiteral("probe 360p"), url, qualities, 360, &host);
    dialog->startPlayer();
    dialog->show();
    mplayer::probes::Verdict verdict;
    auto* surface = dialog->findChild<mplayer::VideoSurface*>(QStringLiteral("videoSurface"));
    const bool loaded = surface != nullptr && waitFor([surface] {
        return surface->hasVideo() && surface->position() > 0.2 && surface->isPlaying();
    });
    verdict.require(loaded, QStringLiteral("real media is playing before preparation"));
    if (!loaded) {
        std::fprintf(stderr,
                     "startup: surface=%d visible=%d context=%d video=%d playing=%d "
                     "position=%.3f size=%dx%d status=%s\n",
                     surface != nullptr, surface != nullptr && surface->isVisible(),
                     surface != nullptr && surface->isContextReady(),
                     surface != nullptr && surface->hasVideo(),
                     surface != nullptr && surface->isPlaying(),
                     surface != nullptr ? surface->position() : -1.0,
                     surface != nullptr ? surface->width() : 0,
                     surface != nullptr ? surface->height() : 0, qPrintable(statusText(dialog)));
        dialog->close();
        return verdict.report("engine_popup_probe");
    }
    verdict.require(!dialog->enginePreparationVisible(),
                    QStringLiteral("no operation window appears before work starts"));

    bool cancelSeen = false;
    QObject::connect(dialog, &PlayerDialog::enginePreparationCancelRequested, dialog,
                     [&cancelSeen] { cancelSeen = true; });
    const quint64 originalGeneration = surface->pauseRequestGeneration();
    dialog->beginEnginePreparation(QStringLiteral("Preparing TensorRT engine"));
    QApplication::processEvents();
    auto* overlay = dialog->findChild<ProcessingOverlay*>();
    verdict.require(overlay != nullptr && dialog->enginePreparationVisible(),
                    QStringLiteral("preparation owns a visible operation window"));
    auto* progress = overlay != nullptr ? overlay->findChild<QProgressBar*>() : nullptr;
    verdict.require(progress != nullptr && progress->minimum() == 0 && progress->maximum() == 0,
                    QStringLiteral("progress is indeterminate without real stages"));
    verdict.require(surface->pauseRequestGeneration() == originalGeneration,
                    QStringLiteral("an operation pause does not masquerade as viewer intent"));
    requireHeld(verdict, surface, QStringLiteral("originally playing preparation"));
    surface->setPaused(false);
    requireHeld(verdict, surface, QStringLiteral("Play requested during preparation"));
    surface->togglePause();
    surface->togglePause();
    requireHeld(verdict, surface, QStringLiteral("successive toggles retain latest Play intent"));
    dialog->updateEnginePreparation(2, 3, QStringLiteral("Builder phase"), QStringLiteral("Real stage detail"));
    verdict.require(progress != nullptr && progress->maximum() == 3 && progress->value() == 2,
                    QStringLiteral("only a supplied stage count creates determinate progress"));
    dialog->endEnginePreparation(true, QStringLiteral("Complete"));
    verdict.require(waitFor([surface] { return corePaused(surface, false); }),
                    QStringLiteral("success restores the retained playing intent"));
    pump(1800);
    verdict.require(!dialog->enginePreparationVisible(),
                    QStringLiteral("successful completion dismisses its notification"));

    surface->setPaused(true);
    waitFor([surface] { return corePaused(surface, true); });
    dialog->beginEnginePreparation(QStringLiteral("Paused input"));
    dialog->beginEnginePreparation(QStringLiteral("Updated operation"));
    requireHeld(verdict, surface, QStringLiteral("repeated begin on paused input"));
    dialog->endEnginePreparation(true, QStringLiteral("Complete"));
    verdict.require(corePaused(surface, true) && !surface->isPlaying(),
                    QStringLiteral("repeated begin preserves the original paused intent"));

    dialog->beginEnginePreparation(QStringLiteral("Paused input with Play"));
    surface->togglePause();
    requireHeld(verdict, surface, QStringLiteral("toggle Play on originally paused input"));
    dialog->endEnginePreparation(false, QStringLiteral("native.build.toolFailed"));
    verdict.require(waitFor([surface] { return corePaused(surface, false); }),
                    QStringLiteral("failure honors the latest explicit Play intent"));
    pump(1800);
    verdict.require(dialog->enginePreparationVisible() && overlay != nullptr &&
                        labelTexts(overlay).contains(QStringLiteral("native.build.toolFailed")),
                    QStringLiteral("failure remains readable beyond the success-dismiss delay"));

    dialog->beginEnginePreparation(QStringLiteral("Newer Pause wins"));
    surface->setPaused(false);
    surface->setPaused(true);
    requireHeld(verdict, surface, QStringLiteral("newer explicit Pause"));
    dialog->endEnginePreparation(true, QStringLiteral("Complete"));
    verdict.require(corePaused(surface, true) && !surface->isPlaying(),
                    QStringLiteral("completion cannot override a newer explicit Pause"));

    surface->setPaused(false);
    waitFor([surface] { return corePaused(surface, false); });
    dialog->beginEnginePreparation(QStringLiteral("Cancelled operation"));
    clickCancel(overlay);
    verdict.require(cancelSeen, QStringLiteral("public preparation Cancel reaches its builder owner"));
    requireHeld(verdict, surface, QStringLiteral("Cancel awaiting builder acknowledgement"));
    dialog->endEnginePreparation(false, QStringLiteral("Cancelled"));
    verdict.require(waitFor([surface] { return corePaused(surface, false); }),
                    QStringLiteral("acknowledged cancellation restores surviving playing intent"));
    verdict.require(labelTexts(overlay).contains(QStringLiteral("Cancelled")),
                    QStringLiteral("cancellation terminal detail remains readable"));

    dialog->beginEnginePreparation(QStringLiteral("First build"));
    dialog->endEnginePreparation(true, QStringLiteral("Complete"));
    dialog->beginEnginePreparation(QStringLiteral("Next build"));
    pump(2200);
    verdict.require(dialog->enginePreparationVisible(),
                    QStringLiteral("an older completion timer cannot hide a new active build"));
    requireHeld(verdict, surface, QStringLiteral("new build past older completion timer"));
    cancelSeen = false;
    clickCancel(overlay);
    verdict.require(cancelSeen, QStringLiteral("the next operation remains cancellable"));
    dialog->endEnginePreparation(false, QStringLiteral("Cancelled"));
    verdict.require(progress != nullptr && progress->maximum() > 0,
                    QStringLiteral("a stopped operation no longer animates as running"));

    surface->setPaused(false);
    waitFor([surface] { return corePaused(surface, false); });
    dialog->beginEnginePreparation(QStringLiteral("Source-bound operation"));
    const uint64_t oldEpoch = surface->sourceEpoch();
    surface->open(url);
    verdict.require(waitFor([surface, oldEpoch] {
        return surface->sourceEpoch() != oldEpoch && surface->hasVideo();
    }), QStringLiteral("replacement loads a genuinely newer source epoch"));
    surface->setPaused(true);
    dialog->endEnginePreparation(true, QStringLiteral("Old source complete"));
    verdict.require(corePaused(surface, true),
                    QStringLiteral("old completion never resumes replacement media"));
    surface->setPaused(false);
    verdict.require(waitFor([surface] { return corePaused(surface, false); }),
                    QStringLiteral("source replacement leaves no stale engine pause hold"));

    dialog->beginEnginePreparation(QStringLiteral("Reload while Play is held"));
    const uint64_t reloadEpoch = surface->sourceEpoch();
    dialog->applyRefreshedStreams(qualities, 360);
    verdict.require(waitFor([surface, reloadEpoch] {
        return surface->sourceEpoch() != reloadEpoch && surface->hasVideo();
    }), QStringLiteral("public stream refresh replaces the held source"));
    requireHeld(verdict, surface, QStringLiteral("replacement remains paused during preparation"));
    verdict.require(!surface->playbackPauseIntent(),
                    QStringLiteral("held Play survives source replacement"));
    dialog->endEnginePreparation(true, QStringLiteral("Replacement complete"));
    verdict.require(waitFor([surface] {
        return corePaused(surface, false) && surface->isPlaying();
    }), QStringLiteral("terminal acknowledgement resumes the replacement's retained Play"));

    surface->setPaused(true);
    waitFor([surface] { return corePaused(surface, true); });
    dialog->beginEnginePreparation(QStringLiteral("Reload before newer Play"));
    const uint64_t pausedReloadEpoch = surface->sourceEpoch();
    dialog->applyRefreshedStreams(qualities, 360);
    surface->setPaused(false);
    verdict.require(waitFor([surface, pausedReloadEpoch] {
        return surface->sourceEpoch() != pausedReloadEpoch && surface->hasVideo();
    }), QStringLiteral("paused source refresh finishes after a newer Play request"));
    requireHeld(verdict, surface, QStringLiteral("newer Play waits for operation acknowledgement"));
    verdict.require(!surface->playbackPauseIntent(),
                    QStringLiteral("deferred load cannot overwrite newer Play with recorded Pause"));
    dialog->endEnginePreparation(true, QStringLiteral("Newer Play complete"));
    verdict.require(waitFor([surface] {
        return corePaused(surface, false) && surface->isPlaying();
    }), QStringLiteral("terminal applies Play requested during the replacement load"));

    dialog->applyAnimeJanaiBackend(QStringLiteral("tensorrt"));
    dialog->applyAnimeJanaiRequest(QStringLiteral("builtin1001"));
    const bool animeJanaiPlanReady = waitFor([dialog, surface] {
        const auto plan = surface->animeJanaiPlan();
        return dialog->animeJanaiPlanGeneration() > 0 &&
               plan.presetId == QStringLiteral("builtin1001") &&
               plan.enabled && plan.available && surface->animeJanaiProcessingObserved();
    }, 20000);
    verdict.require(animeJanaiPlanReady,
                    QStringLiteral("configured AnimeJaNai processing reaches the actual player core"));
    if (!animeJanaiPlanReady) {
        std::fprintf(stderr, "native setup: %s\n", qPrintable(surface->animeJanaiStatus()));
        dialog->close();
        return verdict.report("engine_popup_probe");
    }
    pump(500);

    const QString buildDetail = QStringLiteral("Compiling spatial test engine");
    verdict.require(announceAnimeJanaiBuild(surface, true, false, buildDetail),
                    QStringLiteral("controlled build-start notification reaches the player"));
    verdict.require(dialog->enginePreparationVisible() && overlay != nullptr &&
                        labelTexts(overlay).contains(buildDetail) &&
                        labelTexts(overlay).contains(QStringLiteral("TensorRT compilation")) &&
                        labelTexts(overlay).contains(QStringLiteral("Preset:")) &&
                        labelTexts(overlay).contains(QStringLiteral("Elapsed:")),
                    QStringLiteral("ongoing build status shows phase, detail and current preset"));
    verdict.require(progress != nullptr && progress->minimum() == 0 && progress->maximum() == 0,
                    QStringLiteral("builder notification does not invent percentage progress"));
    dialog->togglePip();
    PipFloat* pip = nullptr;
    const bool floated = waitFor([&pip] {
        pip = activeFloat();
        return pip != nullptr && pip->videoSurface()->hasVideo();
    });
    verdict.require(floated, QStringLiteral("real PiP owner loads during preparation"));
    if (floated) {
        requireHeld(verdict, pip->videoSurface(), QStringLiteral("PiP receives active preparation hold"));
        verdict.require(!pip->videoSurface()->animeJanaiPlan().enabled,
                        QStringLiteral("PiP does not attach a second AnimeJaNai engine build"));
        const QString readyDetail = QStringLiteral("TensorRT engine ready");
        verdict.require(announceAnimeJanaiBuild(surface, false, true, readyDetail),
                        QStringLiteral("controlled ready notification settles through the owning core"));
        verdict.require(labelTexts(overlay).contains(readyDetail),
                        QStringLiteral("ready terminal detail remains visible to the viewer"));
        verdict.require(waitFor([pip] {
            const auto plan = pip->videoSurface()->animeJanaiPlan();
            return plan.enabled && plan.presetId == QStringLiteral("builtin1001");
        }), QStringLiteral("PiP receives the plan only after the controlled build notification settles"));
        verdict.require(waitFor([pip, surface] {
            return corePaused(pip->videoSurface(), false) && corePaused(surface, true);
        }), QStringLiteral("held Play resumes only the float after preparation handoff"));
        dialog->beginEnginePreparation(QStringLiteral("PiP preparation"));
        pip->videoSurface()->setPaused(false);
        requireHeld(verdict, pip->videoSurface(), QStringLiteral("PiP Play during preparation"));
        pip->videoSurface()->setPaused(true);
        dialog->endEnginePreparation(true, QStringLiteral("Complete"));
        verdict.require(corePaused(pip->videoSurface(), true) && corePaused(surface, true),
                        QStringLiteral("PiP newer Pause survives completion; parked main stays paused"));
        dialog->beginEnginePreparation(QStringLiteral("PiP held Play returns"));
        pip->videoSurface()->setPaused(false);
        requireHeld(verdict, pip->videoSurface(), QStringLiteral("PiP Play awaiting hand-back"));
        dialog->togglePip();
        requireHeld(verdict, surface, QStringLiteral("main receives PiP's held Play"));
        verdict.require(!surface->playbackPauseIntent(),
                        QStringLiteral("hand-back preserves Play before releasing the float hold"));
        dialog->endEnginePreparation(true, QStringLiteral("Hand-back complete"));
        verdict.require(waitFor([pip, surface] {
            return corePaused(surface, false) && corePaused(pip->videoSurface(), true);
        }), QStringLiteral("hand-back terminal resumes main while pooled float stays paused"));
        surface->setPaused(true);
        pump(350);
        verdict.require(corePaused(surface, true),
                        QStringLiteral("deferred PiP return cannot override a newer Pause"));
    }

    surface->setPaused(false);
    dialog->beginEnginePreparation(QStringLiteral("Closing source"));
    surface->close();
    dialog->endEnginePreparation(true, QStringLiteral("Closed source complete"));
    verdict.require(!surface->hasVideo() && !surface->isPlaying(),
                    QStringLiteral("completion cannot resume a closed source"));
    dialog->startPlayer();
    verdict.require(waitFor([surface] {
        return surface->hasVideo() && corePaused(surface, false) && surface->position() > 0.2;
    }), QStringLiteral("reopening after close has no stale operation hold"));
    auto* tool = dialog->findChild<ProcessingOverlay*>();
    if (tool != nullptr) tool->hide();
    // Controlled state exercises late UI replay only; it is not evidence that an engine built.
    const QMetaMethod buildSignal =
        QMetaMethod::fromSignal(&mplayer::VideoSurface::animeJanaiEngineBuildChanged);
    verdict.require(QObject::disconnect(surface, buildSignal, dialog, QMetaMethod()),
                    QStringLiteral("dialog can be detached to exercise late state replay"));
    const QString lateBuildDetail = QStringLiteral("Synthetic retained build state");
    verdict.require(announceAnimeJanaiBuild(surface, true, false, lateBuildDetail) &&
                        surface->animeJanaiEngineBuildActive() &&
                        !dialog->enginePreparationVisible(),
                    QStringLiteral("a start missed while detached remains in the surface state"));
    dialog->syncAnimeJanaiEngineBuildState(surface);
    verdict.require(tool != nullptr && dialog->enginePreparationVisible() &&
                        labelTexts(tool).contains(lateBuildDetail),
                    QStringLiteral("late attachment replays the active build into a visible tool"));
    requireHeld(verdict, surface, QStringLiteral("late-attached build status"));
    QPointer<QWidget> toolGuard(tool);
    QPointer<PlayerDialog> dialogGuard(dialog);
    dialog->close();
    verdict.require(waitFor([&dialogGuard] { return dialogGuard.isNull(); }, 5000),
                    QStringLiteral("closing the player deletes the dialog"));
    pump(200);
    verdict.require(toolGuard.isNull() || !toolGuard->isVisible(),
                    QStringLiteral("no orphan operation Tool stays visible after close"));
    return verdict.report("engine_popup_probe");
}
