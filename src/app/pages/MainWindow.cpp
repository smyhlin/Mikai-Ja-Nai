#include "pages/MainWindow.hpp"

#include "dialogs/SettingsDialog.hpp"
#include "dialogs/ComponentStoreTab.hpp"
#include "pages/SearchPage.hpp"
#include "pages/TitlePage.hpp"
#include "state/NativeComponentProvisioner.hpp"

#include <native/components/NativeComponentCatalog.hpp>
#include <native/components/NativeGpuProbe.hpp>
#include "state/NativePlaybackAssembly.hpp"
#include "state/QualityCache.hpp"
#include "workers/Runner.hpp"

#include <backend/MikaiClient.hpp>
#include <native/prepare/NativePreparationContext.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>

#include <PlayerDialog.hpp>
#include <memory>
#include <optional>
#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>
#include <QtCore/QMetaObject>
#include <QtCore/QTimer>
#include <QtCore/QVector>
#include <native/prepare/NativeFrameFixture.hpp>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QUrl>
#include <QVariant>
#include <QCloseEvent>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace mikai::app {

namespace {

/// The player that currently owns playback: the one on the page on screen when it
/// has an active source, otherwise the standalone media player. Entering PiP
/// hides the main dialog while it still owns the pooled float, so this asks the
/// dialog's own liveness (`hasActiveSource()`) rather than visibility, and never
/// retains a pointer: the answer is re-resolved on every call.
mplayer::shell::PlayerDialog* owningPlaybackPlayer(QWidget* page,
                                                   mplayer::shell::PlayerDialog* local) {
    if (page != nullptr) {
        mplayer::shell::PlayerDialog* found = nullptr;
        const QList<mplayer::shell::PlayerDialog*> players =
            page->findChildren<mplayer::shell::PlayerDialog*>();
        for (mplayer::shell::PlayerDialog* candidate : players) {
            if (candidate != nullptr && candidate->hasActiveSource()) {
                found = candidate;
            }
        }
        if (found != nullptr) {
            return found;
        }
    }
    return local != nullptr && local->hasActiveSource() ? local : nullptr;
}

/// One settings dialog's live preparation job. The owning core, the job
/// generation and the capture's position provenance all move while the settings
/// dialog is open, so they live in one shared value rather than in a captured
/// pointer or a widget's label.
struct SettingsNativeJob {
    QPointer<mplayer::shell::PlayerDialog> core;
    quint64 generation = 0;
    int frameIndex = -1;
    QElapsedTimer deadline;
    QVector<QMetaObject::Connection> frameConnections;
    quint64 lastPositionEpoch = 0;
    double lastPosition = -1.0;
    bool havePosition = false;
};

/// How long an explicit Prepare click waits for the owning core to report the
/// source and device its probe observed. A deadline, not a retry: the refusal is
/// named and nothing is guessed.
constexpr int kNativeContextWaitMs = 8000;

}  // namespace

MainWindow::MainWindow(backend::MikaiClient& client, QWidget* parent)
    : QWidget(parent), client_(client) {
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(tr("Mikai Wrapper"));
    setMinimumSize(860, 620);
    resize(1080, 760);

    runner_ = new Runner(this);
    componentProvisioner_ = new NativeComponentProvisioner(*runner_, this);
    qualityCache_ = new QualityCache(this);
    qualityCache_->load();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(this);
    root->addWidget(stack_);

    search_ = new SearchPage(*runner_, client_, stack_);
    connect(search_, &SearchPage::openRequested, this,
            [this](const QString& ref, const QString& poster) { openTitle(ref, poster); });
    connect(search_, &SearchPage::settingsRequested, this, &MainWindow::openSettings);
    connect(search_, &SearchPage::mediaFileRequested, this, &MainWindow::chooseMediaFile);
    connect(search_, &SearchPage::mediaUrlRequested, this, &MainWindow::chooseMediaUrl);
    stack_->addWidget(search_);
}

MainWindow::~MainWindow() {
    // Destruction cannot be refused: the player is closed anyway, with the
    // forced stop named and its own close still run first so the core dies
    // before the widget releases what its filter was holding.
    closeMedia(/*forced=*/true);
    // The forced close already released this window's resolution and dropped the
    // pending flag; the guard makes destruction safe even when the close was
    // refused, so no wait outlives the window that started it.
    runtimeSeq_ += 1;
    runtimePending_ = false;
    releaseRuntimeResolution();
    // The close event covers the ordinary path, but a session that ends any
    // other way (a quit() that never delivers one) would otherwise leave the
    // entries unwritten. flushNow() is a no-op once the cache is clean, so
    // running it twice costs nothing.
    qualityCache_->flushNow();
}

mplayer::shell::PlayerDialog* MainWindow::currentMediaPlayer() const {
    return mediaPlayer_.data();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // A player that has not acknowledged its own native stop refuses its close,
    // and the shell must not go while a core is still stopping work: the close is
    // ignored and the window stays usable for a retry. The same holds for a
    // reentrant close arriving through the player's own settling pump, which
    // finds the same refusal and returns without recursing.
    if (!closeMedia() || !disposeTitle()) {
        event->ignore();
        return;
    }
    // The title page flushes when it shuts down, but a session that only browsed
    // the home page never reaches that path. Persist here so those sessions
    // keep their extracted streams, and use the forced variant so the debounce
    // interval cannot swallow the last write.
    qualityCache_->flushNow();
    QWidget::closeEvent(event);
}

void MainWindow::chooseMediaFile() {
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Open video file"), {},
        tr("Videos (*.mkv *.mp4 *.webm *.avi *.mov *.m4v *.ts *.m2ts *.mpeg *.mpg);;"
           "All files (*)"));
    if (!file.isEmpty()) {
        openMedia(file);
    }
}

void MainWindow::chooseMediaUrl() {
    bool accepted = false;
    const QString source = QInputDialog::getText(
        this, tr("Open video by URL"), tr("Video or stream URL:"), QLineEdit::Normal, {},
        &accepted).trimmed();
    if (accepted && !source.isEmpty()) {
        openMedia(source);
    }
}

bool MainWindow::openMedia(const QString& source) {
    const QUrl url = QUrl::fromUserInput(source, {}, QUrl::AssumeLocalFile);
    const QString media = url.isLocalFile() ? url.toLocalFile() : url.toString(QUrl::FullyEncoded);
    QString caption = url.fileName();
    if (caption.isEmpty()) {
        caption = url.host().isEmpty() ? tr("Video") : url.host();
    }
    if (!closeMedia()) {
        // The player being replaced is still stopping native work, so replacing
        // it would destroy that stop; the open is refused and the current player
        // stays exactly as it is.
        qWarning().noquote() << "mikai: media open refused (native stop unsettled)";
        return false;
    }
    // closeMedia() deleted the replaced player synchronously: with mpv a
    // closed dialog's core would otherwise keep its failed end-file event and
    // its retry chip alive until the deferred delete, and any later lookup
    // reads the old window's state instead of the new stream's.
    mediaPlayer_ = new mplayer::shell::PlayerDialog(caption, media, this);
    // Core creation can report failure synchronously from startPlayer().
    connect(mediaPlayer_, &mplayer::shell::PlayerDialog::runtimeRequested, this,
            [this] { requestMediaRuntime(); });
    // A processing-failure remedy from the standalone player opens this app's
    // existing Processing surface, aimed at this dialog while it still owns a
    // stream. The pointer is guarded on every use, so a replaced player is never
    // asked for anything.
    connect(mediaPlayer_, &mplayer::shell::PlayerDialog::processingSettingsRequested, this,
            [this](mplayer::shell::ProcessingSettingsIntent intent) {
                openSettingsForIntent(intent, mediaPlayer_);
            });
    connect(mediaPlayer_, &mplayer::shell::PlayerDialog::componentSetupRequested,
            this, [this] { offerAnimeJanaiSetup(); });
    // The app-owned assembly answers this dialog's native requests: it reads the
    // owned model store, the requested recipe and the measured machine identity
    // off the GUI thread and submits the whole request back through the dialog.
    bindNativePlaybackAssembly(*runner_, *mediaPlayer_);
    mediaPlayer_->show();
    mediaPlayer_->startPlayer();
    return true;
}

bool MainWindow::closeMedia(bool forced) {
    if (mediaPlayer_) {
        const bool accepted = mediaPlayer_->close();
        if (!accepted && !forced) {
            return false;
        }
        // A runtime reply for this dialog must not reach the next one: bump the
        // sequence and drop the pending flag so the replacement can ask anew.
        // A resolution this window started is released here rather than at the
        // reply, so closing the initiating player cannot leave the one process
        // resolution held by a wait nobody is waiting for any more.
        ++runtimeSeq_;
        runtimePending_ = false;
        releaseRuntimeResolution();
        if (!accepted) {
            // Visible GL teardown has faulted the driver; forced destruction must hide first.
            if (mediaPlayer_) mediaPlayer_->hide();
            qWarning().noquote()
                << "mikai: forced media teardown (destruction cannot be refused; native claims "
                   "are released only after the core is destroyed)";
        }
        // Release the old core before another player is created.
        delete mediaPlayer_.data();
        mediaPlayer_.clear();
    }
    return true;
}

void MainWindow::requestMediaRuntime() {
    if (!mediaPlayer_) {
        return;
    }
    auto* dialog = mediaPlayer_.data();
    // The requested source is shown first, whatever happens below: the viewer
    // asked for this file and the window must not look empty while the runtime
    // is being settled.
    dialog->show();
    if (mplayer::native::runtimeSelected()) {
        // A module is pinned already, so the only reason this player has no core
        // is the same open the dialog will retry itself.
        return;
    }
    if (runtimePending_) {
        // This dialog is already waiting. The dialog can ask more than once (an
        // open that failed while the wait holds, then a retry chip), so the ask
        // is answered with the wait's own status instead of a second resolution.
        dialog->showRuntimePendingStatus();
        return;
    }
    // One resolution per process. The app's own startup validation holds it
    // while it hashes the bundle, and a request that arrives during that window
    // joins it: the dialog shows what the resolution is doing and plays as soon
    // as the module is pinned, instead of being told the runtime is missing one
    // turn before it loads.
    if (mplayer::native::runtimeResolutionPending()) {
        runtimePending_ = true;
        runtimeResolutionHeld_ = false;
        runtimeResolution_ = mplayer::native::ResolutionLease();
        answerRuntimeWhenSettled();
        return;
    }
    // Nothing is running yet: this player asked before the startup validation had
    // taken its resolution. Taking it is the one way to answer the request at
    // all, and holding the lease is what lets the reply - or the disposal of this
    // player - hand the resolution back without guessing who owns it.
    runtimeResolution_ = mplayer::native::beginResolution();
    if (!runtimeResolution_) {
        // The one process resolution was taken between the check above and here,
        // so this request joins whatever is running now rather than competing
        // with it.
        runtimePending_ = true;
        runtimeResolutionHeld_ = false;
        answerRuntimeWhenSettled();
        return;
    }
    runtimeResolutionHeld_ = true;
    runtimePending_ = true;
    runMediaRuntimeResolution();
}

void MainWindow::runMediaRuntimeResolution() {
    const int seq = ++runtimeSeq_;
    const QPointer<MainWindow> self(this);
    const QPointer<mplayer::shell::PlayerDialog> target(mediaPlayer_.data());
    // Validate the bundle off-thread; load the process-pinned module on the GUI thread.
    runner_->run(
        [] {
            return QVariant::fromValue(
                mplayer::native::resolveRuntime(mplayer::native::storedRuntimeChoice()));
        },
        [self, target, seq](const QVariant& result) {
            if (self.isNull()) {
                return;
            }
            // The terminal state is published before this window's wait is
            // released, so a request that arrives the moment the module is
            // pinned sees a module, not a resolution that just ended.
            const mplayer::native::RuntimeSelection selection = mplayer::native::commitRuntime(
                result.value<mplayer::native::RuntimeSelection>());
            if (seq != self->runtimeSeq_ || target.isNull()) {
                // A superseded dialog: the resolution it started is already
                // recorded, so this window's lease - if it still owns one - is
                // released without touching the dialog that replaced it.
                self->releaseRuntimeResolution();
                return;
            }
            self->runtimePending_ = false;
            if (!selection.loaded) {
                const QString text =
                    mplayer::native::runtimeUnavailablePlaybackText(selection.reason);
                self->releaseRuntimeResolution();
                target->showError(text);
                return;
            }
            self->releaseRuntimeResolution();
            // The source this dialog actually asked for is the one that plays,
            // and it opens only after the module is pinned.
            target->startPlayer();
        },
        [self, target, seq](const QString& reason) {
            if (self.isNull()) {
                return;
            }
            if (seq != self->runtimeSeq_ || target.isNull()) {
                self->releaseRuntimeResolution();
                return;
            }
            self->runtimePending_ = false;
            const QString text = mplayer::native::runtimeUnavailablePlaybackText(reason);
            self->releaseRuntimeResolution();
            target->showError(text);
        });
}

void MainWindow::answerRuntimeWhenSettled() {
    // The dialog is shown with the wait already on screen: a viewer who asked for
    // playback during startup validation must see the resolution that is running,
    // never a runtime reported as permanently missing.
    if (mediaPlayer_) {
        mediaPlayer_->showRuntimePendingStatus();
    }
    mplayer::native::whenRuntimeResolutionFinishes(this, [this] {
        answerMediaRuntimeResolved();
    });
}

void MainWindow::releaseRuntimeResolution() {
    if (runtimeResolutionHeld_) {
        const mplayer::native::ResolutionLease lease = runtimeResolution_;
        runtimeResolutionHeld_ = false;
        runtimeResolution_ = mplayer::native::ResolutionLease();
        mplayer::native::endResolution(lease);
    }
}

void MainWindow::answerMediaRuntimeResolved() {
    // The wait is answered exactly once and only for the dialog that is still
    // open: the request sequence is bumped whenever the player is replaced or
    // closed, so a completion belonging to a source the viewer moved on from
    // plays nothing and says nothing.
    if (!runtimePending_) {
        return;
    }
    runtimePending_ = false;
    const QPointer<mplayer::shell::PlayerDialog> dialog = mediaPlayer_;
    if (dialog.isNull()) {
        return;
    }
    if (mplayer::native::runtimeSelected()) {
        // The requested source opens now that the module exists: startPlayer()
        // is the same call the dialog itself would retry, so the source that was
        // asked for is the one that plays.
        dialog->startPlayer();
        return;
    }
    // No module: the recorded terminal state names what was actually wrong, so
    // the failure is the one truth and never a generic one.
    dialog->showError(mplayer::native::runtimeUnavailablePlaybackText(
        mplayer::native::currentSelection().reason));
}

void MainWindow::ensureNativeComponents() {
    // The device is observed before the runtime check, so the one line naming
    // what this machine actually has is written even when the selected runtime
    // is Standard and the component check returns immediately. Without it, a
    // "no GPU" report has nothing behind it in the log.
    const mplayer::native::GpuComputeCapability& gpu = mplayer::native::observeComputeCapability();
    // The pack this device resolves to is what the Components tab lists; naming
    // it here is what makes the selection checkable without opening the dialog.
    const mplayer::native::ComponentPack* resource =
        mplayer::native::tensorRtResourcePackFor(gpu.smMajor, gpu.smMinor);
    qInfo().noquote() << "mikai: tensorrt component" << (resource == nullptr
                                                             ? QStringLiteral("none")
                                                             : resource->name);
    if (componentProvisioner_ == nullptr) return;
    // The prompt is the gate: ensure() may submit work only after the viewer
    // chose Download. Running it before the question made a Native startup
    // pull hundreds of megabytes before the first paint - the prompt then
    // appeared beside a download already in flight, or never appeared once
    // the store had filled.
    if (mplayer::native::nativeComponentsPromptDismissed()) {
        // "Skip and never ask" bought this viewer silence: no question and no
        // download. ensure() is deliberately not called - even for its log
        // line - so a muted prompt can never be the branch that starts work.
        qInfo() << "mikai: native components prompt muted by the viewer";
        return;
    }
    QString missing;
    const bool needs = componentProvisioner_->needsDownload(&missing);
    if (!needs) {
        // Nothing to ask, but the startup check stays diagnosable: this call
        // is what writes "notNative"/"satisfied"/"noGpu"/"noBundle" into
        // mikai.log. Every path that answers "no download needed" is a path
        // that refuses before the submit, so it is log-only in practice too.
        QString reason;
        if (!componentProvisioner_->ensure(&reason)) {
            qInfo().noquote() << "mikai: native components:" << reason;
        }
        return;
    }
    // The one warning the viewer asked for at startup: when the GPU's own packs
    // are missing from the store, offer to fetch them, this run only, or stay
    // quiet forever. Only the muted dismissal is persisted; everything else is
    // re-decided on the next launch from the store's actual contents.
    QMessageBox prompt(QMessageBox::Warning, tr("GPU components missing"),
                       tr("The TensorRT packs this GPU needs are not installed:\n%1\n\n"
                          "Without them, preparing a TensorRT engine fails.")
                           .arg(missing),
                       QMessageBox::NoButton, this);
    QPushButton* download = prompt.addButton(tr("Download"), QMessageBox::AcceptRole);
    prompt.addButton(tr("Skip for now"), QMessageBox::RejectRole);
    prompt.addButton(tr("Skip and never ask"), QMessageBox::DestructiveRole);
    prompt.setDefaultButton(download);
    prompt.exec();
    // Roles, not pointers: only QAbstractButton is declared here, and the role
    // is exactly the viewer's choice rather than which widget instance it was.
    switch (prompt.buttonRole(prompt.clickedButton())) {
        case QMessageBox::DestructiveRole:
            mplayer::native::dismissNativeComponentsPrompt();
            qInfo() << "mikai: native components prompt dismissed by the viewer";
            break;
        case QMessageBox::AcceptRole: {
            // The one call that may submit work: the viewer just chose it.
            QString downloadReason;
            if (!componentProvisioner_->ensure(&downloadReason)) {
                qInfo().noquote() << "mikai: native components:" << downloadReason;
            }
            openComponentSettings();
            break;
        }
        default:
            qInfo() << "mikai: native components download skipped for this run";
            break;
    }
}

void MainWindow::openSettings() {
    openSettingsImpl(std::nullopt, {});
}

void MainWindow::openComponentSettings() {
    openSettingsImpl(std::nullopt, {}, true);
}

void MainWindow::openSettingsForIntent(mplayer::shell::ProcessingSettingsIntent intent,
                                       QPointer<mplayer::shell::PlayerDialog> core) {
    openSettingsImpl(intent, core);
}

void MainWindow::offerAnimeJanaiSetup(SettingsDialog* settings) {
    if (setupDeclined_ || setupPromptOffered_) return;
    const QString promptBody = mikai::app::animeJanaiSetupPrompt();
    if (promptBody.isEmpty()) return;

    QWidget* parent = QApplication::activeWindow();
    if (parent == nullptr) parent = this;

    QDialog prompt(parent);
    prompt.setWindowTitle(tr("Set up AnimeJaNai"));
    prompt.setModal(true);

    auto* layout = new QVBoxLayout(&prompt);
    auto* heading = new QLabel(tr("Set up AnimeJaNai"), &prompt);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    layout->addWidget(heading);

    auto* body = new QLabel(promptBody, &prompt);
    body->setTextFormat(Qt::RichText);
    body->setWordWrap(true);
    layout->addWidget(body);

    auto* buttons = new QDialogButtonBox(&prompt);
    auto* install = buttons->addButton(tr("Install"), QDialogButtonBox::AcceptRole);
    install->setObjectName(QStringLiteral("primary"));
    auto* notNow = buttons->addButton(tr("Not now"), QDialogButtonBox::RejectRole);
    install->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    notNow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(buttons);
    install->setDefault(true);

    setupPromptOffered_ = true;
    bool installChosen = false;
    bool notNowChosen = false;
    connect(install, &QPushButton::clicked, &prompt, [&prompt, &installChosen] {
        installChosen = true;
        prompt.accept();
    });
    connect(notNow, &QPushButton::clicked, &prompt, [&prompt, &notNowChosen] {
        notNowChosen = true;
        prompt.reject();
    });
    prompt.exec();
    if (installChosen) {
        if (settings != nullptr) {
            settings->showComponentsTab();
            settings->installRecommended();
        } else {
            openSettingsImpl(std::nullopt, {}, true, true);
        }
    } else if (notNowChosen) {
        setupDeclined_ = true;
    }
}

void MainWindow::openSettingsImpl(std::optional<mplayer::shell::ProcessingSettingsIntent> intent,
                                  QPointer<mplayer::shell::PlayerDialog> preferredCore,
                                  bool showComponentsFirst,
                                  bool installAnimeJanaiRecommended) {
    SettingsDialog dialog(client_, *runner_, this);
    dialog.setComponentProvisioner(componentProvisioner_);
    connect(&dialog, &SettingsDialog::componentsTabOpened, this,
            [this](SettingsDialog* settings) { offerAnimeJanaiSetup(settings); });
    if (showComponentsFirst) {
        dialog.showComponentsTab();
    }
    if (installAnimeJanaiRecommended) {
        dialog.installRecommended();
    }

    // Every preparation fact is resolved live at the moment it is asked for. A
    // captured pointer, a visibility test or a cached context would answer for
    // the wrong core as soon as PiP takes the float over, so the owner is a
    // callable rather than a value.
    const QPointer<SettingsDialog> settings(&dialog);
    auto job = std::make_shared<SettingsNativeJob>();
    const auto owningCore = [this, preferredCore, intent]() -> mplayer::shell::PlayerDialog* {
        // The dialog that raised the remedy answers for itself, while it still
        // owns a stream.
        if (!preferredCore.isNull() && preferredCore->hasActiveSource()) {
            return preferredCore.data();
        }
        if (intent.has_value()) {
            // No fallback for an explicit remedy. The dialog that asked for it is
            // gone or no longer playing, and preparing some other core instead
            // would start work on a movie the viewer did not ask about. Every
            // handler below turns this null into a named "no core owns the source"
            // refusal, which is the honest answer.
            return nullptr;
        }
        // Plain settings keep the existing convention: whatever owns playback on
        // screen, the page's dialog first.
        return owningPlaybackPlayer(stack_ != nullptr ? stack_->currentWidget() : nullptr,
                                    mediaPlayer_.data());
    };

    // The backend an explicit preparation should align the core to, read live at
    // the moment it is asked for: the settings panel's UNSAVED provider choice
    // when one is selected, otherwise the stored preference. Never the effective
    // plan's provider - that may differ from what the viewer just asked for.
    const auto desiredBackend = [settings]() -> mplayer::native::Backend {
        // The dialog already resolves the fallback itself - the panel's unsaved
        // choice when one is made, the stored preference otherwise - so this asks
        // once and takes the answer, rather than re-deriving a default here and
        // risking a second one that disagrees with the dialog's.
        return settings->nativePreparationDesiredBackend();
    };

    // Readiness: a real playing or paused source exists, so Prepare may be armed
    // before any device fact has been observed. This arms nothing.
    dialog.setNativePreparationSourcePresence([owningCore](QString* reasonKey, QString* reason) {
        mplayer::shell::PlayerDialog* player = owningCore();
        if (player != nullptr && player->hasActiveSource()) {
            return true;
        }
        if (reasonKey != nullptr) *reasonKey = QStringLiteral("native.prepare.source");
        if (reason != nullptr) {
            *reason = tr("No playback core has an active source to prepare from.");
        }
        return false;
    });

    // A context already observed is pulled straight from the owning core; this is
    // never a cached value and never a guess.
    dialog.setNativePreparationContextProvider([owningCore]() {
        mplayer::shell::PlayerDialog* player = owningCore();
        return player != nullptr
                   ? player->nativePreparationContext()
                   : std::optional<mplayer::native::NativePreparationContext>{};
    });

    // The explicit Prepare click arms the owning core's probe and waits, bounded,
    // for the first context that core actually observes. Opening the settings
    // page never reaches this path.
    auto contextTimer = std::make_shared<QTimer>();
    contextTimer->setInterval(100);
    QObject::connect(contextTimer.get(), &QTimer::timeout, &dialog,
                     [job, owningCore, contextTimer, settings, desiredBackend]() {
        if (settings.isNull()) {
            contextTimer->stop();
            return;
        }
        mplayer::shell::PlayerDialog* player = owningCore();
        if (player == nullptr || !player->hasActiveSource()) {
            contextTimer->stop();
            settings->failNativePreparationContext(
                QStringLiteral("native.prepare.core"),
                tr("No playback core owns the current source any more."));
            return;
        }
        if (job->core != player) {
            // The owning core changed (PiP, or another episode): arm the one that
            // owns playback now rather than waiting on the previous core.
            job->core = player;
            // The probe is aligned to the DESIRED backend - the panel's unsaved
            // provider choice when one is selected, otherwise the stored
            // preference - never to whatever plan happens to be admitted. A
            // TensorRT request therefore aligns the core to a real CUDA decoder
            // (or refuses by name) before the filter observes anything.
            player->armNativeProbe(desiredBackend());
        }
        const std::optional<mplayer::native::NativePreparationContext> context =
            player->nativePreparationContext();
        if (context.has_value()) {
            contextTimer->stop();
            settings->deliverNativePreparationContext(*context);
            return;
        }
        if (job->deadline.elapsed() >= kNativeContextWaitMs) {
            contextTimer->stop();
            settings->failNativePreparationContext(
                QStringLiteral("native.prepare.contextTimeout"),
                tr("The playback core did not report its source and device in time."));
        }
    });
    dialog.setNativePreparationContextRequest([job, contextTimer](quint64 generation) {
        job->generation = generation;
        job->deadline.restart();
        job->core.clear();
        contextTimer->start();
    });

    // One capture per request, through the dialog's own forward so it follows the
    // owning core across PiP. An interpolation stage's second frame must be a
    // different observation of the SAME source: an unchanged position is a
    // duplicate and a changed source epoch is a different episode of the same
    // geometry, so both are refused by name instead of delivered as a pair.
    dialog.setNativePreparationFrameRequest(
        [job, owningCore, settings](int frameIndex, quint64 generation) {
            if (settings.isNull()) {
                return;
            }
            for (const QMetaObject::Connection& connection : job->frameConnections) {
                QObject::disconnect(connection);
            }
            job->frameConnections.clear();
            mplayer::shell::PlayerDialog* player = owningCore();
            if (player == nullptr || !player->hasActiveSource()) {
                settings->failNativePreparationFrame(
                    frameIndex, QStringLiteral("native.reference.noCore"),
                    tr("No playback core owns the current source any more."));
                return;
            }
            job->frameIndex = frameIndex;
            job->generation = generation;
            job->core = player;
            const QPointer<mplayer::shell::PlayerDialog> core(player);
            job->frameConnections.append(QObject::connect(
                player, &mplayer::shell::PlayerDialog::nativeReferenceFrameReady, settings.data(),
                [job, settings, core, frameIndex](const mplayer::native::FrameFixture& frame,
                                                  quint64 sourceEpoch, double framePosition,
                                                  int reportedIndex,
                                                  std::optional<mplayer::native::Rational> frameFps) {
                    if (settings.isNull() || core != job->core || frameIndex != job->frameIndex) {
                        return;
                    }
                    // The index is echoed back; a mismatch means this answer belongs
                    // to a request this job already moved past.
                    if (reportedIndex != job->frameIndex) {
                        return;
                    }
                    if (frameIndex > 0 && job->havePosition) {
                        // A different source epoch is a different source, so two
                        // frames from two episodes of the same geometry are not a
                        // pair even though the size matches.
                        if (sourceEpoch != job->lastPositionEpoch) {
                            settings->failNativePreparationFrame(
                                frameIndex, QStringLiteral("native.reference.sourceChanged"),
                                tr("The source changed between an interpolation stage's two "
                                   "frames, so they are not a pair."));
                            return;
                        }
                        if (framePosition == job->lastPosition) {
                            settings->failNativePreparationFrame(
                                frameIndex, QStringLiteral("native.reference.duplicate"),
                                tr("The core returned the same observation twice for an "
                                   "interpolation stage's second frame."));
                            return;
                        }
                    }
                    job->lastPositionEpoch = sourceEpoch;
                    job->lastPosition = framePosition;
                    job->havePosition = true;
                    settings->deliverNativePreparationFrame(frameIndex, frame, sourceEpoch,
                                                            framePosition);
                }));
            job->frameConnections.append(QObject::connect(
                player, &mplayer::shell::PlayerDialog::nativeReferenceFrameFailed, settings.data(),
                [job, settings, core, frameIndex](const QString& reasonKey, const QString& reason,
                                                  int reportedIndex) {
                    if (settings.isNull() || core != job->core || frameIndex != job->frameIndex) {
                        return;
                    }
                    if (reportedIndex != job->frameIndex) {
                        return;
                    }
                    settings->failNativePreparationFrame(frameIndex, reasonKey, reason);
                }));
            // The index is the request: it is what makes an interpolation stage's
            // second capture an adjacent frame instead of a second copy of the
            // first.
            player->requestNativeReferenceFrame(frameIndex);
        });

    // A TensorRT build runs for minutes inside this modal dialog, so its progress
    // is mirrored into the player's own always-on-top preparation window: the
    // viewer keeps seeing the picture and the work at the same time, and the
    // window outlives the dialog because the core it belongs to does.
    //
    // The core is resolved at each event rather than captured once: the dialog is
    // modal, but PiP can hand the float the stream while it is open, and the
    // window must follow whichever surface owns playback.
    connect(&dialog, &SettingsDialog::nativePreparationStarted, this, [owningCore] {
        if (mplayer::shell::PlayerDialog* core = owningCore(); core != nullptr) {
            core->beginEnginePreparation(
                QCoreApplication::translate("MainWindow", "Preparing TensorRT engine"));
        }
    });
    connect(&dialog, &SettingsDialog::nativePreparationProgress, this,
            [owningCore](int stageIndex, int stageCount, const QString& phase,
                         const QString& detail) {
                if (mplayer::shell::PlayerDialog* core = owningCore(); core != nullptr) {
                    core->updateEnginePreparation(stageIndex, stageCount, phase, detail);
                }
            });
    connect(&dialog, &SettingsDialog::nativePreparationFinished, this, [owningCore](bool ok) {
        if (mplayer::shell::PlayerDialog* core = owningCore(); core != nullptr) {
            core->endEnginePreparation(
                ok, ok ? QCoreApplication::translate("MainWindow", "The engine is ready.")
                       : QCoreApplication::translate(
                             "MainWindow", "The build stopped; the Processing tab names the reason."));
        }
    });

    // The window's Cancel reaches the build's own cancellation gate through the
    // dialog that owns it. The core on screen is the one that raised it.
    if (mplayer::shell::PlayerDialog* core = owningCore(); core != nullptr) {
        connect(core, &mplayer::shell::PlayerDialog::enginePreparationCancelRequested, &dialog,
                &SettingsDialog::cancelNativePreparation, Qt::UniqueConnection);
    }

    // A remedy is dispatched only after every provider above is installed, so the
    // existing preparation command runs with the live context callbacks already
    // answering, and only for a remedy the viewer actually pressed. Opening the
    // settings surface on its own reaches nothing below.
    if (intent.has_value()) {
        const mplayer::shell::ProcessingSettingsIntent requested = *intent;
        const QPointer<SettingsDialog> target(&dialog);
        // Deferred one turn so the dialog is on screen with its providers already
        // connected when the command runs.
        QTimer::singleShot(0, &dialog, [target, requested] {
            if (!target.isNull()) {
                target->applyProcessingIntent(requested);
            }
        });
    }

    if (dialog.exec() == QDialog::Accepted) {
        search_->refreshFeedSettings();
    }
    // Components publish installs even when preference edits are cancelled.
    if (mplayer::shell::PlayerDialog* core = owningCore(); core != nullptr) {
        core->refreshStoredSettings();
    }
}

bool MainWindow::openTitle(const QString& ref, const QString& poster, bool preserveHistory) {
    if (ref.isEmpty()) {
        return false;
    }
    // The page being replaced has to acknowledge the close of every player it
    // owns first; a refusal keeps that page and cancels this navigation, so the
    // history is left exactly as it was.
    if (!disposeTitle()) {
        qWarning().noquote()
            << "mikai: title navigation refused (previous page's native stop unsettled)";
        return false;
    }
    if (!preserveHistory) {
        titleHistory_.clear();
    }
    titleRef_ = ref;
    titlePoster_ = poster;
    // The card's detail lookup usually already landed for this ref, so hand it
    // over and skip the redundant getAnime inside the title page.
    const backend::AnimeDetail* hint = search_->cachedDetail(ref);
    title_ = new TitlePage(*runner_, client_, *qualityCache_, ref, poster,
                           hint != nullptr ? *hint : backend::AnimeDetail(), stack_);
    connect(title_, &TitlePage::backRequested, this, &MainWindow::goSearch);
    connect(title_, &TitlePage::settingsRequested, this, &MainWindow::openSettings);
    // An episode player's processing-failure remedy reaches the same Processing
    // surface as the standalone player's, aimed at the dialog that raised it.
    // The pointer is only used as a QPointer, so a closed or replaced player is
    // never asked for preparation facts.
    connect(title_, &TitlePage::playerProcessingSettingsRequested, this,
            [this](mplayer::shell::PlayerDialog* player,
                   mplayer::shell::ProcessingSettingsIntent intent) {
                openSettingsForIntent(intent, QPointer<mplayer::shell::PlayerDialog>(player));
            });
    connect(title_, &TitlePage::playerComponentSetupRequested,
            this, [this] { offerAnimeJanaiSetup(); });
    connect(title_, &TitlePage::relatedOpenRequested, this,
            [this](const QString& target, const QString& targetPoster) {
                openRelated(target, targetPoster);
            });
    connect(title_, &TitlePage::recentTitleLoaded, search_, &SearchPage::recordRecentTitle);
    stack_->addWidget(title_);
    stack_->setCurrentWidget(title_);
    return true;
}

void MainWindow::openRelated(const QString& ref, const QString& poster) {
    const bool pushed = !titleRef_.isEmpty();
    if (pushed) {
        titleHistory_.append({titleRef_, titlePoster_});
    }
    // A refused navigation must not leave the push behind, or "back" would walk
    // onto the page it never left.
    if (!openTitle(ref, poster, true) && pushed) {
        titleHistory_.removeLast();
    }
}

void MainWindow::goSearch() {
    if (!titleHistory_.isEmpty()) {
        // The entry is consumed only once the page it names really opened, so a
        // refused navigation leaves "back" able to try again.
        const auto previous = titleHistory_.last();
        if (openTitle(previous.first, previous.second, true)) {
            titleHistory_.removeLast();
        }
        return;
    }
    if (!disposeTitle()) {
        return;
    }
    titleRef_.clear();
    titlePoster_.clear();
    stack_->setCurrentWidget(search_);
}

bool MainWindow::disposeTitle() {
    if (title_ == nullptr) {
        return true;
    }
    // Shutdown saves the quality cache and closes every player before the widget
    // goes, so switching titles never strands a window - unless a player refuses
    // its close, in which case the page and every session in it are kept intact.
    if (!title_->shutdown()) {
        return false;
    }
    stack_->removeWidget(title_);
    title_->deleteLater();
    title_ = nullptr;
    return true;
}

}  // namespace mikai::app
