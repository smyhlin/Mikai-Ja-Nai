#include <utility>
#include "pages/TitlePage.hpp"

#include "pages/EpisodeGrid.hpp"

#include "gui/Clipboard.hpp"
#include "Labels.hpp"
#include "dialogs/CoverViewer.hpp"
#include "state/NativePlaybackAssembly.hpp"
#include "state/QualityCache.hpp"
#include "theme/Colors.hpp"
#include "theme/Icons.hpp"
#include "theme/Metrics.hpp"
#include "widgets/FailureView.hpp"
#include "widgets/FlowLayout.hpp"
#include "widgets/RangePager.hpp"
#include "widgets/TitleCard.hpp"
#include "widgets/RateLimitLabel.hpp"
#include "workers/Runner.hpp"

#include <PlayerDialog.hpp>
#include <VideoSurface.hpp>
#include <backend/HttpTransport.hpp>
#include <backend/MikaiClient.hpp>
#include <backend/Releases.hpp>
#include <backend/Streams.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>

#include <QAbstractButton>
#include <QAccessible>
#include <QButtonGroup>
#include <QtCore/QDebug>
#include <QEvent>
#include <QKeyEvent>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStyle>
#include <QPointer>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace mikai::app {
namespace {

QList<mplayer::shell::QualityEntry> toQualities(const QMap<int, QString>& map) {
    QList<mplayer::shell::QualityEntry> list;
    list.reserve(map.size());
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (!it.value().trimmed().isEmpty()) {
            list.append({it.key(), it.value()});
        }
    }
    return list;
}

/// The highest usable height in a quality map, 0 when there is none.
int maxQuality(const QMap<int, QString>& map) {
    int best = 0;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (it.key() > 0 && !it.value().trimmed().isEmpty()) {
            best = qMax(best, it.key());
        }
    }
    return best;
}


}  // namespace

/// Everything the player cannot do for itself, answered by the page that opened
/// it. Non-owning: the session is freed when the dialog reports its close.
class TitlePage::Host : public mplayer::shell::PlayerHost {
public:
    Host(TitlePage* page, PlayerSession* session) : page_(page), session_(session) {}

    void requestVariant(const QString& embed, const QString& label) override {
        page_->switchPlayerVariant(session_, embed, label);
    }
    void requestEpisode(int index) override {
        if (index < 0 || index >= session_->playlist.size()) {
            return;
        }
        page_->playEpisodeFrom(session_, session_->playlist.at(index), index);
    }
    void requestTrackProbe(const QString& embed) override {
        page_->probeTrackQuality(session_, embed);
    }
    void requestCopyStreamUrl() override {
        if (session_->entry != nullptr && !mikai::gui::copyText(session_->entry->embedUrl)) {
            showPlayerWarning(QCoreApplication::translate("TitlePage", "Failed to copy link"));
        }
    }
    void playerClosed() override { page_->closeSession(session_->id); }
    void playbackFailed() override { page_->recoverStream(session_); }

private:
    /// The named failure the dialog's clipboard action could not
    /// complete. Parented to the player so the warning sits above the thing that
    /// raised it and is reachable by a screen reader, with the page's status line
    /// as the only fallback when there is no live dialog.
    ///
    /// The warning spins its own nested event loop, and everything that can
    /// delete this page or this player inside it - a window close, a navigation,
    /// the dialog's own close - has to be able to happen. Both pointers are
    /// therefore read once, weakly, before the loop, and nothing is dereferenced
    /// after it: the dialog's own close path is what disposes a session, and it
    /// does that while the session is still registered.
    void showPlayerWarning(const QString& message) {
        // Read weakly, once, before the nested loop: a dialog already on its way
        // to being deleted must not be handed a message box as its parent.
        const QPointer<mplayer::shell::PlayerDialog> dialog = session_->dialog;
        if (dialog.isNull()) {
            const QPointer<TitlePage> page = page_;
            if (!page.isNull()) {
                page->flashStatus(message, true);
            }
            return;
        }
        const QString title = dialog->windowTitle();
        QMessageBox::warning(dialog, title, message);
    }

    TitlePage* page_;
    PlayerSession* session_;
};

// Defined here, where Host is complete: destroying a session destroys its
// host bridge, with no manual delete anywhere.
TitlePage::PlayerSession::~PlayerSession() = default;

TitlePage::PlayerSession* TitlePage::sessionById(quint64 id) {
    const auto it = std::find_if(sessions_.cbegin(), sessions_.cend(),
                                 [id](const std::unique_ptr<PlayerSession>& held) {
                                     return held != nullptr && held->id == id;
                                 });
    return it == sessions_.cend() ? nullptr : it->get();
}

bool TitlePage::isLiveSession(PlayerSession* session) {
    return session != nullptr && sessionById(session->id) == session;
}


void TitlePage::openPlayer(EpisodeEntry* entry, const QMap<int, QString>& qualities) {
    const int height = maxQuality(qualities);
    if (height <= 0) {
        flashStatus(tr("Could not find a stream for this episode"), true);
        return;
    }
    auto owned = std::make_unique<PlayerSession>();
    owned->id = nextSessionId_++;
    // Borrowed for the rest of this call only: the vector owns the session
    // from the append below, and every queued callback re-resolves by id.
    PlayerSession* session = owned.get();
    session->host = std::make_unique<Host>(this, session);
    session->entry = entry;
    int currentIndex = 0;
    session->playlist = tabPlaylist(entry->stateIndex, entry->number, &currentIndex);
    const quint64 sessionId = session->id;
    sessions_.push_back(std::move(owned));

    session->dialog = new mplayer::shell::PlayerDialog(
        QStringLiteral("%1 · %2p").arg(entry->label).arg(height), qualities.value(height),
        toQualities(qualities), height, session->host.get(), this);
    // Core creation can report failure synchronously from startPlayer().
    connect(session->dialog, &mplayer::shell::PlayerDialog::runtimeRequested, this,
            [this, sessionId] {
                if (PlayerSession* live = sessionById(sessionId)) requestRuntime(live);
            });
    // A processing-failure remedy needs the application's Settings surface, which
    // only the window owns. The page re-emits it with the dialog that raised it and
    // nothing else: no URL work, no quality probe, no episode or hostless control
    // is added here. The session registration is checked on every use, so a
    // remedy from a player this page has already disposed is dropped instead of
    // being forwarded for a core that no longer exists.
    connect(session->dialog, &mplayer::shell::PlayerDialog::processingSettingsRequested, this,
            [this, sessionId](mplayer::shell::ProcessingSettingsIntent intent) {
                PlayerSession* session = sessionById(sessionId);
                if (session == nullptr || session->dialog == nullptr) {
                    return;
                }
                emit playerProcessingSettingsRequested(session->dialog, intent);
            });
    connect(session->dialog, &mplayer::shell::PlayerDialog::componentSetupRequested,
            this, [this, sessionId] {
                PlayerSession* session = sessionById(sessionId);
                if (session != nullptr && session->dialog != nullptr) {
                    emit playerComponentSetupRequested();
                }
            });
    // A provider switch can replace the grid, so resolve the session's own episode identity.
    connect(session->dialog, &mplayer::shell::PlayerDialog::episodePlaybackStarted, this,
            [this, sessionId](int playlistIndex) {
                PlayerSession* session = sessionById(sessionId);
                if (session == nullptr || session->dialog == nullptr) {
                    return;
                }
                const QString number = session->playlist.value(playlistIndex)
                                           .value(QStringLiteral("number"))
                                           .toString();
                if (!number.isEmpty()) {
                    noteEpisodeStarted(number);
                }
            });
    connect(session->dialog, &mplayer::shell::PlayerDialog::episodeProgress, this,
            [this, sessionId](int playlistIndex, double position, double duration) {
                PlayerSession* session = sessionById(sessionId);
                if (session == nullptr || session->dialog == nullptr) {
                    return;
                }
                const QString number = session->playlist.value(playlistIndex)
                                           .value(QStringLiteral("number"))
                                           .toString();
                if (!number.isEmpty()) {
                    noteEpisodeProgress(number, position, duration);
                }
            });
    connect(session->dialog, &mplayer::shell::PlayerDialog::episodeResumeDeclined, this,
            [this, sessionId](int playlistIndex) {
                PlayerSession* session = sessionById(sessionId);
                if (session == nullptr || session->dialog == nullptr) {
                    return;
                }
                const QString number = session->playlist.value(playlistIndex)
                                           .value(QStringLiteral("number"))
                                           .toString();
                if (!number.isEmpty()) {
                    // "Start over" means the stored position is not a resume
                    // point any more, and the indicator must stop claiming it.
                    watchHistory_.clearOffset(ref_, number);
                    updateWatchPanel();
                }
            });
    connect(session->dialog, &mplayer::shell::PlayerDialog::episodePlaybackFinished, this,
            [this, sessionId](int playlistIndex) {
                PlayerSession* session = sessionById(sessionId);
                if (session == nullptr || session->dialog == nullptr) {
                    return;
                }
                const QString number = session->playlist.value(playlistIndex)
                                           .value(QStringLiteral("number"))
                                           .toString();
                if (!number.isEmpty()) {
                    setEpisodeWatched(number, true);
                }
                // P0-08: host-driven auto-advance. PlayerDialog owns neither
                // the playlist nor the decision: it only dedupes and emits.
                // Advance only when the finished episode is still the one
                // playing, a next episode exists, and the viewer has not
                // disabled auto-advance (one bool, default ON). The step goes
                // through the existing manual episode-stepping path, so the
                // last episode simply stops: no loop, no replay.
                const bool autoAdvance = QSettings().value(
                    QString::fromLatin1(mplayer::VideoSurface::kAutoAdvanceSettingKey),
                    true).toBool();
                if (!autoAdvance || playlistIndex != session->dialog->currentEpisodeIndex() ||
                    playlistIndex + 1 >= session->playlist.size()) {
                    return;
                }
                session->dialog->stepEpisode(1);
            });
    // The app-owned assembly answers this episode player's native requests: it
    // reads the owned model store, the requested recipe and the measured machine
    // identity off the GUI thread and submits the whole request back through the
    // dialog. Every episode player gets its own binding, exactly as the
    // standalone media player does.
    bindNativePlaybackAssembly(runner_, *session->dialog);
    session->dialog->setPlaylist(session->playlist, currentIndex);
    session->dialog->setVariants(episodeVariants(entry->number), entry->embedUrl);
    requestSkipSegments(session);
    // The episode the player opens first is not applied through applyEpisode, so
    // its resume point is handed over before startPlayer().
    session->dialog->setEpisodeResumePoint(watchHistory_.offset(ref_, entry->number));
    session->dialog->show();
    session->dialog->startPlayer();
}

bool TitlePage::closeSession(quint64 sessionId) {
    PlayerSession* session = sessionById(sessionId);
    if (session == nullptr) {
        return true;
    }
    // The player has to acknowledge its own stop first: a refused close means
    // native work is still stopping, so the session stays exactly as it is and
    // the removal can be retried.
    if (session->dialog != nullptr && !session->dialog->close()) {
        qWarning().noquote() << "mikai: session removal refused (native stop unsettled)";
        return false;
    }
    // close() spins the event loop while it settles, so the session may
    // already be gone (its own playerClosed() disposed it); disposeSession
    // re-resolves by id and is a no-op then.
    disposeSession(sessionId, false);
    return true;
}

void TitlePage::disposeSession(quint64 sessionId, bool forced) {
    // The player's own playerClosed() may already have disposed this session
    // while close() was running, so resolve by id: the id can never name a
    // replacement session, where a pointer comparison could.
    PlayerSession* session = sessionById(sessionId);
    if (session == nullptr) {
        return;
    }
    auto* dialog = session->dialog;
    for (auto waiting = trackWaiters_.begin(); waiting != trackWaiters_.end();) {
        waiting->removeAll(session);
        if (waiting->isEmpty()) {
            waiting = trackWaiters_.erase(waiting);
        } else {
            ++waiting;
        }
    }
    session->dialog = nullptr;
    // The epoch this session was opened under goes with it, so a runtime reply
    // that is still queued for the disposed player is dropped rather than
    // starting a player that no longer exists. The resolution it may have started
    // is released here, which is what keeps a closed player from holding the one
    // process resolution a later request would need.
    runtimeSeq_ += 1;
    session->runtimePending = false;
    releaseSessionResolution(session);
    if (forced && dialog != nullptr) {
        // A refused close leaves the host bridge live until the core is destroyed.
        // Verified precondition (PlayerDialog.cpp:376): the dialog sets
        // WA_DeleteOnClose, so only an *accepted* close schedules its
        // deleteLater; this path runs only after close() was refused, so no
        // deletion is pending and the direct delete cannot double-delete.
        const QPointer<mplayer::shell::PlayerDialog> target(dialog);
        target->hide();
        delete target.data();
    }
    // Erasing the owning pointer destroys the session and its host bridge;
    // nothing below may touch `session` again.
    const auto it = std::find_if(sessions_.cbegin(), sessions_.cend(),
                                 [session](const std::unique_ptr<PlayerSession>& held) {
                                     return held.get() == session;
                                 });
    if (it != sessions_.cend()) sessions_.erase(it);
}

void TitlePage::requestRuntime(PlayerSession* session) {
    if (closed_ || !isLiveSession(session) || session->dialog == nullptr) {
        return;
    }
    auto* dialog = session->dialog;
    // The episode the viewer asked for is shown first, whatever happens below:
    // a startup that is still validating must not look like a refusal to open.
    dialog->show();
    if (mplayer::native::runtimeSelected()) {
        // A module is pinned already, so this dialog has no core for the same
        // reason the dialog itself retries.
        return;
    }
    if (session->runtimePending) {
        // This player is already waiting. The dialog can ask more than once (an
        // open that failed while the wait holds, then a retry chip), so the ask
        // is answered with the wait's own status, never a second resolution.
        dialog->showRuntimePendingStatus();
        return;
    }
    // One resolution per process. The app's own startup validation holds it while
    // it hashes the bundle: an episode opened during that window joins it, so
    // the player shows the resolution that is running and plays as soon as the
    // module is pinned, instead of being told the runtime is missing one turn
    // before it loads.
    if (mplayer::native::runtimeResolutionPending()) {
        session->runtimePending = true;
        session->runtimeResolutionHeld = false;
        session->runtimeResolution = mplayer::native::ResolutionLease();
        dialog->showRuntimePendingStatus();
        answerSessionWhenSettled(session);
        return;
    }
    // Nothing is running yet: this episode was opened before the startup
    // validation had taken its resolution. Taking it is the one way to answer the
    // request at all, and holding the lease is what lets the reply - or the
    // disposal of this player - hand the resolution back without guessing who
    // owns it.
    session->runtimeResolution = mplayer::native::beginResolution();
    if (!session->runtimeResolution) {
        // The one process resolution was taken between the check above and here,
        // so this request joins whatever is running now rather than competing
        // with it.
        session->runtimePending = true;
        session->runtimeResolutionHeld = false;
        dialog->showRuntimePendingStatus();
        answerSessionWhenSettled(session);
        return;
    }
    session->runtimeResolutionHeld = true;
    session->runtimePending = true;
    runSessionRuntimeResolution(session);
}

void TitlePage::runSessionRuntimeResolution(PlayerSession* session) {
    if (!isLiveSession(session) || session->dialog == nullptr) {
        return;
    }
    const int seq = ++runtimeSeq_;
    const int sourceSeq = session->sourceSequence;
    const EpisodeEntry* entry = session->entry;
    const quint64 sessionId = session->id;
    const QPointer<TitlePage> self(this);
    const QPointer<mplayer::shell::PlayerDialog> target(session->dialog);
    // Validate the bundle off-thread; load the process-pinned module on the GUI thread.
    runner_.run(
        [] {
            return QVariant::fromValue(mplayer::native::resolveStoredRuntime());
        },
        [self, target, sessionId, sourceSeq, entry, seq](const QVariant& result) {
            if (self.isNull()) {
                return;
            }
            // Publish the terminal state before the session's wait is released,
            // so a player that opens the moment the module is pinned sees a
            // module rather than a resolution that just ended.
            const mplayer::native::RuntimeSelection selection = mplayer::native::commitRuntime(
                result.value<mplayer::native::RuntimeSelection>());
            // Resolve by id: a disposed session's lease was already released
            // by its disposal, so there is nothing left to release here (the
            // old code released through the freed pointer).
            PlayerSession* session = self->sessionById(sessionId);
            if (session == nullptr) {
                return;
            }
            if (self->closed_ || seq != self->runtimeSeq_ || target.isNull() ||
                session->dialog != target.data() ||
                sourceSeq != session->sourceSequence || session->entry != entry) {
                // A superseded player: its episode, its dialog or the whole page
                // is gone. The resolution it started is already recorded, so the
                // lease goes back without touching the player that replaced it.
                self->releaseSessionResolution(session);
                return;
            }
            self->answerSessionResolved(session, selection);
        },
        [self, target, sessionId, sourceSeq, entry, seq](const backend::Failure& failure) {
            if (self.isNull()) {
                return;
            }
            PlayerSession* session = self->sessionById(sessionId);
            if (session == nullptr) {
                return;
            }
            if (self->closed_ || seq != self->runtimeSeq_ || target.isNull() ||
                session->dialog != target.data() ||
                sourceSeq != session->sourceSequence || session->entry != entry) {
                self->releaseSessionResolution(session);
                return;
            }
            self->answerSessionResolved(session, mplayer::native::RuntimeSelection(),
                                        backend::failureLine(failure));
        });
}

void TitlePage::answerSessionWhenSettled(PlayerSession* session) {
    const QPointer<TitlePage> self(this);
    const quint64 sessionId = session->id;
    mplayer::native::whenRuntimeResolutionFinishes(self.data(), [self, sessionId] {
        // The wait is answered once, and only while the page and the player that
        // asked are still the ones on screen: a page that closed, or a source the
        // viewer switched away from, is answered with nothing.
        if (self.isNull() || self->closed_) {
            return;
        }
        PlayerSession* session = self->sessionById(sessionId);
        if (session == nullptr) {
            return;
        }
        if (mplayer::native::runtimeSelected()) {
            self->answerSessionResolved(session, mplayer::native::currentSelection());
            return;
        }
        self->answerSessionResolved(session, mplayer::native::currentSelection(),
                                   mplayer::native::currentSelection().reason);
    });
}

void TitlePage::answerSessionResolved(PlayerSession* session,
                                      const mplayer::native::RuntimeSelection& selection,
                                      const QString& reason) {
    if (closed_ || !isLiveSession(session) || session->dialog == nullptr) {
        return;
    }
    if (!session->runtimePending) {
        // The wait was already answered - a closed or replaced player, or an
        // earlier terminal answer. Nothing is replayed.
        return;
    }
    session->runtimePending = false;
    const bool loaded = selection.loaded || mplayer::native::runtimeSelected();
    if (!loaded) {
        const QString detail =
            reason.isEmpty() ? mplayer::native::currentSelection().reason : reason;
        const QString text = mplayer::native::runtimeUnavailablePlaybackText(detail);
        releaseSessionResolution(session);
        session->dialog->showError(text);
        return;
    }
    releaseSessionResolution(session);
    // The episode this session is still on opens now that the module exists. It is
    // the same startPlayer() the dialog itself retries, so the source that was
    // asked for is the one that plays.
    session->dialog->startPlayer();
}

void TitlePage::releaseSessionResolution(PlayerSession* session) {
    if (session == nullptr || !session->runtimeResolutionHeld) {
        return;
    }
    const mplayer::native::ResolutionLease lease = session->runtimeResolution;
    session->runtimeResolutionHeld = false;
    session->runtimeResolution = mplayer::native::ResolutionLease();
    mplayer::native::endResolution(lease);
}

void TitlePage::requestSkipSegments(PlayerSession* session) {
    if (closed_ || !isLiveSession(session) ||
        session->entry == nullptr || session->dialog == nullptr) {
        return;
    }

    const QString embed = session->entry->embedUrl;
    session->dialog->setSkipSegments({});
    if (!embed.contains(QStringLiteral("moonanime.art/iframe/"))) {
        return;
    }

    const QPointer<TitlePage> self(this);
    const QPointer<mplayer::shell::PlayerDialog> target(session->dialog);
    const int seq = session->sourceSequence;
    const quint64 sessionId = session->id;
    runner_.runStream(
        [embed] {
            return QVariant::fromValue(
                backend::moonSkipSegments(backend::transportManagerForThread(), embed));
        },
        [self, target, sessionId, embed, seq](const QVariant& result) {
            if (self.isNull() || self->closed_ || target.isNull()) {
                return;
            }
            PlayerSession* session = self->sessionById(sessionId);
            if (session == nullptr || session->dialog != target.data() ||
                seq != session->sourceSequence || target->currentEmbed() != embed) {
                return;
            }
            target->setSkipSegments(result.value<QVector<QVariantMap>>());
        },
        [self, target, sessionId, embed, seq](const backend::Failure& failure) {
            if (self.isNull() || self->closed_ || target.isNull()) {
                return;
            }
            PlayerSession* session = self->sessionById(sessionId);
            if (session == nullptr || session->dialog != target.data() ||
                seq != session->sourceSequence || target->currentEmbed() != embed) {
                return;
            }
            qWarning().noquote() << "mikai: could not load skip segments:" << failure.message;
        });
}

void TitlePage::recoverStream(PlayerSession* session) {
    // Signed stream URLs expire, so re-extract once before reporting failure.
    // The re-extraction targets whatever track the dialog is on RIGHT NOW: a
    // dub/sub switch replaces the record, so recovery must not rewind the
    // episode to the variant it opened with.
    if (closed_ || !isLiveSession(session) ||
        session->entry == nullptr || session->dialog == nullptr) {
        return;
    }
    const EpisodeEntry* failedEntry = session->entry;
    const QString embed = failedEntry->embedUrl;
    // The cached URL is the one that just failed; keeping it would replay the
    // same dead link on the next click instead of extracting again.
    qualityCache_.forget(embed);
    flashStatus(tr("Stream expired — refreshing URL…"), true);
    const QPointer<TitlePage> self(this);
    const QPointer<mplayer::shell::PlayerDialog> target(session->dialog);
    const int seq = session->sourceSequence;
    const quint64 sessionId = session->id;
    runner_.runStream(
        [embed] { return QVariant::fromValue(backend::qualityStreams(embed)); },
        [self, target, sessionId, failedEntry, embed, seq](const QVariant& result) {
            const QMap<int, QString> qualities = result.value<QMap<int, QString>>();
            if (self.isNull() || self->closed_ || qualities.isEmpty() || target.isNull()) {
                return;
            }
            PlayerSession* session = self->sessionById(sessionId);
            if (session == nullptr || session->dialog != target.data() ||
                seq != session->sourceSequence || session->entry != failedEntry) {
                return;
            }
            self->qualityCache_.remember(embed, qualities);
            target->applyRefreshedStreams(toQualities(qualities), target->currentHeight());
        },
        [self, target, sessionId, failedEntry, seq](const backend::Failure& failure) {
            if (self.isNull() || self->closed_ || target.isNull()) {
                return;
            }
            PlayerSession* session = self->sessionById(sessionId);
            if (session == nullptr || session->dialog != target.data() ||
                seq != session->sourceSequence || session->entry != failedEntry) {
                return;
            }
            const QString line =
                failure.message.contains(QStringLiteral("no_source"), Qt::CaseInsensitive)
                    ? QCoreApplication::translate("TitlePage",
                                                  "This player has no video any more.")
                    : QCoreApplication::translate("TitlePage",
                                                  "Could not refresh the stream link.");
            const QString code = backend::failureCodeLabel(failure);
            session->dialog->showError(code.isEmpty() ? line
                                                      : QStringLiteral("%1 · %2").arg(line, code));
        });
}

void TitlePage::switchPlayerVariant(PlayerSession* session, const QString& embed,
                                    const QString& label) {
    if (closed_ || !isLiveSession(session) || session->dialog == nullptr) {
        return;
    }
    auto* entry = new EpisodeEntry;
    entry->stateIndex = session->entry != nullptr ? session->entry->stateIndex : -1;
    entry->number = session->entry != nullptr && !session->entry->number.isEmpty()
                        ? session->entry->number
                        : label;
    entry->embedUrl = embed;
    entry->label = session->entry != nullptr && !session->entry->label.isEmpty()
                       ? session->entry->label
                       : label;
    ownedEntries_.append(entry);
    const QPointer<TitlePage> self(this);
    const QPointer<mplayer::shell::PlayerDialog> target(session->dialog);
    const int seq = ++session->sourceSequence;
    const quint64 sessionId = session->id;
    ensureQualities(
        entry, [self, target, sessionId, seq](EpisodeEntry* loaded,
                                           const QMap<int, QString>& qualities) {
            if (self.isNull() || self->closed_ || target.isNull()) {
                return;
            }
            PlayerSession* session = self->sessionById(sessionId);
            if (session == nullptr || session->dialog != target.data() ||
                seq != session->sourceSequence) {
                return;
            }
            session->entry = loaded;
            session->dialog->applyVariant(toQualities(qualities), loaded->label,
                                          loaded->embedUrl);
            self->requestSkipSegments(session);
        });
}

void TitlePage::probeTrackQuality(PlayerSession* session, const QString& embed) {
    // Answer the player's request for one track's best quality, sharing the
    // per-embed cache so a track already probed costs nothing.
    if (closed_ || !isLiveSession(session) || session->dialog == nullptr) {
        return;
    }
    const QMap<int, QString> cached = qualityCache_.qualities(embed);
    if (!cached.isEmpty()) {
        session->dialog->setTrackQuality(embed, maxQuality(cached));
        return;
    }
    if (trackWaiters_[embed].contains(session)) {
        return;
    }
    trackWaiters_[embed].append(session);
    const QPointer<TitlePage> self(this);
    const QPointer<mplayer::shell::PlayerDialog> target(session->dialog);
    const quint64 sessionId = session->id;
    runner_.runStream(
        [embed] { return QVariant::fromValue(backend::qualityStreams(embed)); },
        [self, target, sessionId, embed](const QVariant& result) {
            if (self.isNull() || target.isNull()) {
                return;
            }
            PlayerSession* session = self->sessionById(sessionId);
            if (session == nullptr || session->dialog != target.data()) {
                return;
            }
            const auto waiting = self->trackWaiters_.find(embed);
            if (waiting != self->trackWaiters_.end()) {
                waiting->removeAll(session);
                if (waiting->isEmpty()) {
                    self->trackWaiters_.erase(waiting);
                }
            }
            const QMap<int, QString> qualities = result.value<QMap<int, QString>>();
            if (self->closed_ || qualities.isEmpty()) {
                return;
            }
            self->qualityCache_.remember(embed, qualities);
            session->dialog->setTrackQuality(embed, maxQuality(qualities));
        },
        [self, target, sessionId, embed](const backend::Failure&) {
            if (self.isNull() || target.isNull()) {
                return;
            }
            PlayerSession* session = self->sessionById(sessionId);
            if (session == nullptr || session->dialog != target.data()) {
                return;
            }
            const auto waiting = self->trackWaiters_.find(embed);
            if (waiting != self->trackWaiters_.end() && waiting->removeAll(session) > 0 &&
                waiting->isEmpty()) {
                self->trackWaiters_.erase(waiting);
            }
        });
}

void TitlePage::playEpisodeFrom(PlayerSession* session, const QVariantMap& item, int index) {
    if (closed_ || !isLiveSession(session) || session->dialog == nullptr) {
        return;
    }
    const QString label = item.value(QStringLiteral("label")).toString();
    QString number = item.value(QStringLiteral("number")).toString().trimmed();
    if (number.isEmpty()) {
        static const QString prefix = QStringLiteral("Episode ");
        const QString localizedPrefix = tr("Episode %1").arg(QString());
        if (!localizedPrefix.isEmpty() && label.startsWith(localizedPrefix)) {
            number = label.mid(localizedPrefix.size()).trimmed();
        } else {
            number = label.startsWith(prefix) ? label.mid(prefix.size()) : label;
        }
    }
    auto* entry = new EpisodeEntry;
    entry->number = number;
    entry->embedUrl = item.value(QStringLiteral("embed")).toString();
    entry->label = label;
    ownedEntries_.append(entry);
    const QVector<QVariantMap> variants = episodeVariants(number);
    const QPointer<TitlePage> self(this);
    const QPointer<mplayer::shell::PlayerDialog> target(session->dialog);
    const int seq = ++session->sourceSequence;
    const quint64 sessionId = session->id;
    ensureQualities(
        entry, [self, target, sessionId, index, variants, seq](EpisodeEntry* loaded,
                                                          const QMap<int, QString>& qualities) {
            if (self.isNull() || self->closed_ || target.isNull()) {
                return;
            }
            PlayerSession* session = self->sessionById(sessionId);
            if (session == nullptr || session->dialog != target.data() ||
                seq != session->sourceSequence) {
                return;
            }
            session->entry = loaded;
            // Continue a half-watched episode instead of restarting it.
            target->setEpisodeResumePoint(self->watchHistory_.offset(self->ref_, loaded->number));
            session->dialog->applyEpisode(loaded->label, loaded->embedUrl, index,
                                          toQualities(qualities), variants);
            self->requestSkipSegments(session);
        });
}


bool TitlePage::shutdown(bool forced) {
    if (closed_) {
        return true;
    }
    // Every player acknowledges its own stop before anything is disposed. A
    // refusal keeps the page, its sessions and its dialogs exactly as they are,
    // so the shutdown can be retried; only the destructor, which cannot refuse
    // destruction, disposes a refused session anyway - and even then each
    // widget's destructor releases what it held only after the core has died.
    // Ids are collected first: disposing a session erases it from the vector.
    QVector<quint64> open;
    open.reserve(sessions_.size());
    for (const auto& held : sessions_) open.append(held->id);
    for (quint64 sessionId : open) {
        if (sessionById(sessionId) == nullptr) {
            // The player's own playerClosed() already disposed this one while an
            // earlier close was running.
            continue;
        }
        if (closeSession(sessionId)) {
            continue;
        }
        if (!forced) {
            qWarning().noquote() << "mikai: title shutdown refused (native stop unsettled)";
            return false;
        }
        // Destruction cannot be refused, so the forced stop is named rather than
        // probed; each dialog's destructor still releases what it held only after
        // its core has synchronously died.
        qWarning().noquote()
            << "mikai: forced title teardown (destruction cannot be refused; native claims are "
               "released only after the core is destroyed)";
        disposeSession(sessionId, true);
    }
    closed_ = true;
    fetchWaiters_.clear();
    entryCallbacks_.clear();
    trackWaiters_.clear();
    // Bumping the sequence makes any release probe still in flight a no-op.
    releaseProbeSeq_ += 1;
    // The last runtime wait - a player that was already disposed, or one whose
    // resolution is still queued - is released rather than left owned by a page
    // that is gone, so a later request is not locked out of the one process
    // resolution by a player nobody is watching any more.
    runtimeSeq_ += 1;
    for (const auto& held : sessions_) {
        held->runtimePending = false;
        releaseSessionResolution(held.get());
    }
    qualityCache_.flushNow();
    if (coverViewer_ != nullptr) {
        // close() destroys it via WA_DeleteOnClose, which fires destroyed and
        // clears the pointer. A deleteLater() on top of that is a double free.
        CoverViewer* viewer = coverViewer_;
        coverViewer_ = nullptr;
        viewer->close();
    }
    qDeleteAll(ownedEntries_);
    ownedEntries_.clear();
    return true;
}

}  // namespace mikai::app
