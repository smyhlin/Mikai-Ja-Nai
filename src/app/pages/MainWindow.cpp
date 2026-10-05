#include "pages/MainWindow.hpp"

#include "dialogs/SettingsDialog.hpp"
#include "pages/SearchPage.hpp"
#include "pages/TitlePage.hpp"
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
    setWindowTitle(tr("Mikai Wrapper"));
    setMinimumSize(860, 620);
    resize(1080, 760);

    runner_ = new Runner(this);
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
    // The dialog is the only place an mpv failure surfaces, so the connect
    // precedes startPlayer(): "runtimeRequested" means the core could not be
    // created at all, and the host provisions and retries for real.
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
        // A refused close keeps the player, so nothing about it is retired yet:
        // its runtime sequence and pending flag still belong to it.
        if (!mediaPlayer_->close() && !forced) {
            return false;
        }
        // A runtime reply for this dialog must not reach the next one: bump the
        // sequence and drop the pending flag so the replacement can ask anew.
        ++runtimeSeq_;
        runtimePending_ = false;
        // Deleted, not merely closed: the dialog carries WA_DeleteOnClose, so
        // close() only hides it, and a hidden window still answers
        // findChildren and still holds its mpv core. The next open must find
        // no player at all, and a failed stream's parked core must not decode
        // behind the stream that replaced it.
        //
        // Taken through its own close path first, though: closeEvent stops the
        // core and hides the GL surface, so the widget is deleted hidden.
        // Deleting a still-visible QOpenGLWidget destroys its context while the
        // driver is presenting it - the DrvPresentBuffers fault the PiP pool
        // already guards against - and a poisoned context can leave the next
        // dialog's render context uncreated, which reads as a second player
        // that never plays. The explicit delete still runs, so no hidden window
        // or parked core survives it.
        //
        // Only the destructor, which cannot refuse destruction, deletes through
        // a refusal - and even then the widget's own destructor releases what it
        // held only after the core has synchronously died. The forced stop is
        // named rather than probed: nothing here may claim an acknowledgement.
        if (forced) {
            qWarning().noquote()
                << "mikai: forced media teardown (destruction cannot be refused; native claims "
                   "are released only after the core is destroyed)";
        }
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
    dialog->show();
    if (mplayer::native::runtimeSelected()) {
        // A module is already pinned, so startPlayer() is the only thing that
        // may run again. It emits `runtimeRequested` synchronously when the
        // core still cannot be created, so the second entry from inside it is
        // dropped rather than retried for ever.
        if (runtimeRetrying_) {
            return;
        }
        runtimeRetrying_ = true;
        dialog->startPlayer();
        runtimeRetrying_ = false;
        return;
    }
    if (runtimePending_ || !mplayer::native::beginRecovery()) {
        return;
    }
    runtimePending_ = true;
    const int seq = ++runtimeSeq_;
    const QPointer<MainWindow> self(this);
    const QPointer<mplayer::shell::PlayerDialog> target(dialog);
    // Resolution may download, so it runs on the pool; the load happens back on
    // the GUI thread, where a core is about to be created.
    runner_->run(
        [] {
            return QVariant::fromValue(
                mplayer::native::resolveRuntime(mplayer::native::storedRuntimeChoice()));
        },
        [self, target, seq](const QVariant& result) {
            mplayer::native::endRecovery();
            if (self.isNull() || seq != self->runtimeSeq_ || target.isNull()) {
                return;
            }
            self->runtimePending_ = false;
            const mplayer::native::RuntimeSelection selection = mplayer::native::commitRuntime(
                result.value<mplayer::native::RuntimeSelection>());
            if (!selection.loaded) {
                target->showError(mplayer::native::runtimeUnavailablePlaybackText(selection.reason));
                return;
            }
            target->startPlayer();
        },
        [self, target, seq](const QString& reason) {
            mplayer::native::endRecovery();
            if (self.isNull() || seq != self->runtimeSeq_ || target.isNull()) {
                return;
            }
            self->runtimePending_ = false;
            target->showError(mplayer::native::runtimeUnavailablePlaybackText(reason));
        });
}

void MainWindow::openSettings() {
    openSettingsImpl(std::nullopt, {});
}

void MainWindow::openSettingsForIntent(mplayer::shell::ProcessingSettingsIntent intent,
                                       QPointer<mplayer::shell::PlayerDialog> core) {
    openSettingsImpl(intent, core);
}

void MainWindow::openSettingsImpl(std::optional<mplayer::shell::ProcessingSettingsIntent> intent,
                                  QPointer<mplayer::shell::PlayerDialog> preferredCore) {
    SettingsDialog dialog(client_, *runner_, this);

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
        // Every accepted Save has to reach the running core, not only a remedy's:
        // the dialog writes the same native request and the same shortcut bindings
        // either way, and only a live core can pick them up. The originating core
        // wins when a remedy raised this dialog, otherwise the core on screen is
        // used, and either way only while it is alive and still owns a stream.
        // The refresh is idempotent - it dedups the request announcement and
        // reinstalls bindings - so a Save that changed nothing native announces
        // nothing. It never reopens the URL, never calls the API and never touches
        // the quality cache or the plan already running.
        if (mplayer::shell::PlayerDialog* core = owningCore(); core != nullptr) {
            core->refreshStoredSettings();
        }
    }
    // Opening the settings surface must not arm anything by itself: the
    // preparation panel arms the probe only on an explicit Prepare click, and
    // every handler above is only reachable from that click.
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
