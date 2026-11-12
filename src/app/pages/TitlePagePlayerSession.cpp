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

#include <player/quick/PlayerController.hpp>
#include <player/core/VideoSession.hpp>
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
/// it. Non-owning: the session is freed when the controller reports its close.
class TitlePage::Host : public mplayer::shell::PlayerHost {
public:
    Host(TitlePage* page, PlayerSession* session)
        : page_(page), sessionId_(session->id) {}

    void requestVariant(const QString& embed, const QString& label) override {
        if (PlayerSession* session = liveSession()) {
            page_->switchPlayerVariant(session, embed, label);
        }
    }
    void requestEpisode(int index) override {
        PlayerSession* session = liveSession();
        if (session == nullptr || index < 0 || index >= session->playlist.size()) {
            return;
        }
        page_->playEpisodeFrom(session, session->playlist.at(index), index);
    }
    void requestTrackProbe(const QString& embed) override {
        if (PlayerSession* session = liveSession()) {
            page_->probeTrackQuality(session, embed);
        }
    }
    void requestCopyStreamUrl() override {
        PlayerSession* session = liveSession();
        if (session != nullptr && session->entry != nullptr &&
            !mikai::gui::copyText(session->entry->embedUrl)) {
            reportCopyFailure(session, QCoreApplication::translate("TitlePage", "Failed to copy link"));
        }
    }
    void playerClosed() override {
        if (page_.isNull()) return;
        const QPointer<TitlePage> page = page_;
        const quint64 sessionId = sessionId_;
        // The QML close handler must return before its window is destroyed.
        QTimer::singleShot(0, page.data(), [page, sessionId] {
            if (page) page->closeSession(sessionId);
        });
    }
    void playbackFailed() override {
        if (PlayerSession* session = liveSession()) page_->recoverStream(session);
    }

private:
    PlayerSession* liveSession() const {
        return page_.isNull() ? nullptr : page_->sessionById(sessionId_);
    }

    /// Report clipboard refusal through the live player when possible, so its
    /// notification and accessibility alert stay attached to the player window.
    void reportCopyFailure(PlayerSession* session, const QString& message) {
        const QPointer<mplayer::quick::PlayerController> controller = session->controller;
        if (!controller.isNull()) {
            controller->reportActionFailure(message);
            return;
        }
        if (!page_.isNull()) {
            page_->flashStatus(message, true);
        }
    }

    QPointer<TitlePage> page_;
    const quint64 sessionId_;
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
    if (qualities.isEmpty()) {
        flashStatus(tr("Could not find a stream for this episode"), true);
        return;
    }
    int height = maxQuality(qualities);
    QString url = qualities.value(height);
    if (url.trimmed().isEmpty()) {
        for (auto it = qualities.constBegin(); it != qualities.constEnd(); ++it) {
            if (!it.value().trimmed().isEmpty()) {
                height = it.key();
                url = it.value();
                break;
            }
        }
    }
    if (url.trimmed().isEmpty()) {
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

    const QString caption = height > 0
                                ? QStringLiteral("%1 · %2p").arg(entry->label).arg(height)
                                : entry->label;
    session->controller = new mplayer::quick::PlayerController(
        caption, url, toQualities(qualities), height, session->host.get(), this);
    // Core creation can report failure synchronously from startPlayer().
    connect(session->controller, &mplayer::quick::PlayerController::runtimeRequested, this,
            [this, sessionId] {
                if (PlayerSession* live = sessionById(sessionId)) requestRuntime(live);
            });
    // A processing-failure remedy needs the application's Settings surface, which
    // only the window owns. The page re-emits it with the controller that raised it and
    // nothing else: no URL work, no quality probe, no episode or hostless control
    // is added here. The session registration is checked on every use, so a
    // remedy from a player this page has already disposed is dropped instead of
    // being forwarded for a core that no longer exists.
    connect(session->controller, &mplayer::quick::PlayerController::processingSettingsRequested, this,
            [this, sessionId](mplayer::shell::ProcessingSettingsIntent intent) {
                PlayerSession* session = sessionById(sessionId);
                if (session == nullptr || session->controller == nullptr) {
                    return;
                }
                emit playerProcessingSettingsRequested(session->controller, intent);
            });
    connect(session->controller, &mplayer::quick::PlayerController::componentSetupRequested,
            this, [this, sessionId] {
                PlayerSession* session = sessionById(sessionId);
                if (session != nullptr && session->controller != nullptr) {
                    emit playerComponentSetupRequested();
                }
            });
    // A provider switch can replace the grid, so resolve the session's own episode identity.
    connect(session->controller, &mplayer::quick::PlayerController::episodePlaybackStarted, this,
            [this, sessionId](int playlistIndex) {
                PlayerSession* session = sessionById(sessionId);
                if (session == nullptr || session->controller == nullptr) {
                    return;
                }
                const QString number = session->playlist.value(playlistIndex)
                                           .value(QStringLiteral("number"))
                                           .toString();
                if (!number.isEmpty()) {
                    noteEpisodeStarted(number);
                }
            });
    connect(session->controller, &mplayer::quick::PlayerController::episodeProgress, this,
            [this, sessionId](int playlistIndex, double position, double duration) {
                PlayerSession* session = sessionById(sessionId);
                if (session == nullptr || session->controller == nullptr) {
                    return;
                }
                const QString number = session->playlist.value(playlistIndex)
                                           .value(QStringLiteral("number"))
                                           .toString();
                if (!number.isEmpty()) {
                    noteEpisodeProgress(number, position, duration);
                }
            });
    connect(session->controller, &mplayer::quick::PlayerController::episodeResumeDeclined, this,
            [this, sessionId](int playlistIndex) {
                PlayerSession* session = sessionById(sessionId);
                if (session == nullptr || session->controller == nullptr) {
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
    connect(session->controller, &mplayer::quick::PlayerController::episodePlaybackFinished, this,
            [this, sessionId](int playlistIndex) {
                PlayerSession* session = sessionById(sessionId);
                if (session == nullptr || session->controller == nullptr) {
                    return;
                }
                const QString number = session->playlist.value(playlistIndex)
                                           .value(QStringLiteral("number"))
                                           .toString();
                if (!number.isEmpty()) {
                    setEpisodeWatched(number, true);
                }
                // P0-08: host-driven auto-advance. PlayerController owns neither
                // the playlist nor the decision: it only dedupes and emits.
                // Advance only when the finished episode is still the one
                // playing, a next episode exists, and the viewer has not
                // disabled auto-advance (one bool, default ON). The step goes
                // through the existing manual episode-stepping path, so the
                // last episode simply stops: no loop, no replay.
                const bool autoAdvance = QSettings().value(
                    QString::fromLatin1(mplayer::VideoSession::kAutoAdvanceSettingKey),
                    true).toBool();
                if (!autoAdvance || playlistIndex != session->controller->currentEpisodeIndex() ||
                    playlistIndex + 1 >= session->playlist.size()) {
                    return;
                }
                session->controller->stepEpisode(1);
            });
    // The app-owned assembly answers this episode player's native requests: it
    // reads the owned model store, the requested recipe and the measured machine
    // identity off the GUI thread and submits the whole request back through the
    // controller. Every episode player gets its own binding, exactly as the
    // standalone media player does.
    bindNativePlaybackAssembly(runner_, *session->controller);
    session->controller->setPlaylist(session->playlist, currentIndex);
    session->controller->setVariants(episodeVariants(entry->number), entry->embedUrl);
    requestSkipSegments(session);
    // The episode the player opens first is not applied through applyEpisode, so
    // its resume point is handed over before startPlayer().
    session->controller->setEpisodeResumePoint(watchHistory_.offset(ref_, entry->number));
    session->controller->showWindow();
    session->controller->startPlayer();
}

bool TitlePage::closeSession(quint64 sessionId) {
    PlayerSession* session = sessionById(sessionId);
    if (session == nullptr) {
        return true;
    }
    // The player has to acknowledge its own stop first: a refused close means
    // native work is still stopping, so the session stays exactly as it is and
    // the removal can be retried.
    if (session->controller != nullptr && !session->controller->requestClose()) {
        qWarning().noquote() << "mikai: session removal refused (native stop unsettled)";
        return false;
    }
    // requestClose() may have called playerClosed() synchronously, disposing
    // this session already; disposeSession re-resolves by id and is then a no-op.
    disposeSession(sessionId, false);
    return true;
}

void TitlePage::disposeSession(quint64 sessionId, bool forced) {
    // The player's own playerClosed() may already have disposed this session
    // while requestClose() was running, so resolve by id: the id can never name a
    // replacement session, where a pointer comparison could.
    PlayerSession* session = sessionById(sessionId);
    if (session == nullptr) {
        return;
    }
    auto* controller = session->controller.data();
    for (auto waiting = trackWaiters_.begin(); waiting != trackWaiters_.end();) {
        auto& waiters = waiting.value();
        waiters.erase(std::remove_if(waiters.begin(), waiters.end(),
                                     [sessionId](const TrackWaiter& waiter) {
                                         return waiter.sessionId == sessionId;
                                     }),
                      waiters.end());
        if (waiters.isEmpty()) {
            waiting = trackWaiters_.erase(waiting);
        } else {
            ++waiting;
        }
    }
    session->controller = nullptr;
    ++session->runtimeSequence;
    session->runtimePending = false;
    releaseSessionResolution(session);

    // Unregister before any controller teardown can call Host::playerClosed()
    // again. Keep the bridge alive until the controller is destroyed.
    std::unique_ptr<Host> host = std::move(session->host);
    const auto it = std::find_if(sessions_.cbegin(), sessions_.cend(),
                                 [session](const std::unique_ptr<PlayerSession>& held) {
                                     return held.get() == session;
                                 });
    if (it != sessions_.cend()) sessions_.erase(it);

    if (controller != nullptr) {
        const QPointer<mplayer::quick::PlayerController> target(controller);
        if (forced) target->hideWindow();
        delete target.data();
    }
    host.reset();
}

void TitlePage::requestRuntime(PlayerSession* session) {
    if (closed_ || !isLiveSession(session) || session->controller == nullptr) {
        return;
    }
    auto* controller = session->controller.data();
    // The episode the viewer asked for is shown first, whatever happens below:
    // a startup that is still validating must not look like a refusal to open.
    controller->showWindow();
    if (mplayer::native::runtimeSelected()) {
        // A module is pinned already, so the controller can retry immediately.
        return;
    }
    if (session->runtimePending) {
        // This player is already waiting. The controller can ask more than once (an
        // open that failed while the wait holds, then a retry chip), so the ask
        // is answered with the wait's own status, never a second resolution.
        controller->showRuntimePendingStatus();
        return;
    }
    // One resolution per process. The app's own startup validation holds it while
    // it hashes the bundle: an episode opened during that window joins it, so
    // the player shows the resolution that is running and plays as soon as the
    // module is pinned, instead of being told the runtime is missing one turn
    // before it loads.
    if (mplayer::native::runtimeResolutionPending()) {
        ++session->runtimeSequence;
        session->runtimePending = true;
        session->runtimeResolutionHeld = false;
        session->runtimeResolution = mplayer::native::ResolutionLease();
        controller->showRuntimePendingStatus();
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
        ++session->runtimeSequence;
        session->runtimePending = true;
        session->runtimeResolutionHeld = false;
        controller->showRuntimePendingStatus();
        answerSessionWhenSettled(session);
        return;
    }
    session->runtimeResolutionHeld = true;
    session->runtimePending = true;
    runSessionRuntimeResolution(session);
}

void TitlePage::runSessionRuntimeResolution(PlayerSession* session) {
    if (!isLiveSession(session) || session->controller == nullptr) {
        return;
    }
    const int seq = ++session->runtimeSequence;
    const int sourceSeq = session->sourceSequence;
    const EpisodeEntry* entry = session->entry;
    const quint64 sessionId = session->id;
    const QPointer<TitlePage> self(this);
    const QPointer<mplayer::quick::PlayerController> target(session->controller);
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
            PlayerSession* session = self->sessionById(sessionId);
            if (session == nullptr) {
                return;
            }
            if (seq != session->runtimeSequence) {
                return;
            }
            if (self->closed_ || target.isNull() || session->controller != target.data() ||
                sourceSeq != session->sourceSequence || session->entry != entry) {
                ++session->runtimeSequence;
                session->runtimePending = false;
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
            if (seq != session->runtimeSequence) {
                return;
            }
            if (self->closed_ || target.isNull() || session->controller != target.data() ||
                sourceSeq != session->sourceSequence || session->entry != entry) {
                ++session->runtimeSequence;
                session->runtimePending = false;
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
    const int sourceSequence = session->sourceSequence;
    const EpisodeEntry* entry = session->entry;
    const int runtimeSequence = session->runtimeSequence;
    mplayer::native::whenRuntimeResolutionFinishes(
        self.data(), [self, sessionId, sourceSequence, entry, runtimeSequence] {
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
        if (session->runtimeSequence != runtimeSequence) {
            return;
        }
        if (session->sourceSequence != sourceSequence || session->entry != entry) {
            ++session->runtimeSequence;
            session->runtimePending = false;
            self->releaseSessionResolution(session);
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
    if (closed_ || !isLiveSession(session) || session->controller == nullptr) {
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
        session->controller->showError(text);
        return;
    }
    releaseSessionResolution(session);
    // The episode this session is still on opens now that the module exists. It is
    // the same startPlayer() the controller itself retries, so the source that was
    // asked for is the one that plays.
    session->controller->startPlayer();
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
        session->entry == nullptr || session->controller == nullptr) {
        return;
    }

    const EpisodeEntry* entry = session->entry;
    const QString embed = entry->embedUrl;
    session->controller->setSkipSegments({});
    if (!embed.contains(QStringLiteral("moonanime.art/iframe/"))) {
        return;
    }

    const QPointer<TitlePage> self(this);
    const QPointer<mplayer::quick::PlayerController> target(session->controller);
    const int seq = session->sourceSequence;
    const quint64 sessionId = session->id;
    runner_.runStream(
        [embed] {
            return QVariant::fromValue(
                backend::moonSkipSegments(backend::transportManagerForThread(), embed));
        },
        [self, target, sessionId, embed, entry, seq](const QVariant& result) {
            if (self.isNull() || self->closed_ || target.isNull()) {
                return;
            }
            PlayerSession* session = self->sessionById(sessionId);
            if (session == nullptr || session->controller != target.data() ||
                seq != session->sourceSequence || session->entry != entry ||
                entry->embedUrl != embed || target->currentEmbed() != embed) {
                return;
            }
            target->setSkipSegments(result.value<QVector<QVariantMap>>());
        },
        [self, target, sessionId, embed, entry, seq](const backend::Failure& failure) {
            if (self.isNull() || self->closed_ || target.isNull()) {
                return;
            }
            PlayerSession* session = self->sessionById(sessionId);
            if (session == nullptr || session->controller != target.data() ||
                seq != session->sourceSequence || session->entry != entry ||
                entry->embedUrl != embed || target->currentEmbed() != embed) {
                return;
            }
            qWarning().noquote() << "mikai: could not load skip segments:" << failure.message;
        });
}

void TitlePage::recoverStream(PlayerSession* session) {
    // Signed stream URLs expire, so re-extract once before reporting failure.
    // The re-extraction targets whatever track the controller is on RIGHT NOW: a
    // dub/sub switch replaces the record, so recovery must not rewind the
    // episode to the variant it opened with.
    if (closed_ || !isLiveSession(session) ||
        session->entry == nullptr || session->controller == nullptr ||
        session->controller->currentEmbed() != session->entry->embedUrl) {
        return;
    }
    const EpisodeEntry* failedEntry = session->entry;
    const QString embed = failedEntry->embedUrl;
    // The cached URL is the one that just failed; keeping it would replay the
    // same dead link on the next click instead of extracting again.
    qualityCache_.forget(embed);
    flashStatus(tr("Stream expired — refreshing URL…"), true);
    const QPointer<TitlePage> self(this);
    const QPointer<mplayer::quick::PlayerController> target(session->controller);
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
            if (session == nullptr || session->controller != target.data() ||
                seq != session->sourceSequence || session->entry != failedEntry ||
                failedEntry->embedUrl != embed || target->currentEmbed() != embed) {
                return;
            }
            self->qualityCache_.remember(embed, qualities);
            target->applyRefreshedStreams(toQualities(qualities), target->currentHeight());
        },
        [self, target, sessionId, failedEntry, embed, seq](const backend::Failure& failure) {
            if (self.isNull() || self->closed_ || target.isNull()) {
                return;
            }
            PlayerSession* session = self->sessionById(sessionId);
            if (session == nullptr || session->controller != target.data() ||
                seq != session->sourceSequence || session->entry != failedEntry ||
                failedEntry->embedUrl != embed || target->currentEmbed() != embed) {
                return;
            }
            const QString line =
                failure.message.contains(QStringLiteral("no_source"), Qt::CaseInsensitive)
                    ? QCoreApplication::translate("TitlePage",
                                                  "This player has no video any more.")
                    : QCoreApplication::translate("TitlePage",
                                                  "Could not refresh the stream link.");
            const QString code = backend::failureCodeLabel(failure);
            session->controller->showError(code.isEmpty() ? line
                                                      : QStringLiteral("%1 · %2").arg(line, code));
        });
}

void TitlePage::switchPlayerVariant(PlayerSession* session, const QString& embed,
                                    const QString& label) {
    if (closed_ || !isLiveSession(session) || session->controller == nullptr ||
        session->entry == nullptr) {
        return;
    }
    EpisodeEntry* const previousEntry = session->entry;
    const QString previousEmbed = previousEntry->embedUrl;
    auto* entry = new EpisodeEntry;
    entry->stateIndex = previousEntry->stateIndex;
    entry->number = !previousEntry->number.isEmpty() ? previousEntry->number : label;
    entry->embedUrl = embed;
    entry->label = !previousEntry->label.isEmpty() ? previousEntry->label : label;
    ownedEntries_.append(entry);
    const QPointer<TitlePage> self(this);
    const QPointer<mplayer::quick::PlayerController> target(session->controller);
    const int seq = ++session->sourceSequence;
    const quint64 sessionId = session->id;
    ensureQualities(
        entry, [self, target, sessionId, seq, previousEntry, previousEmbed, embed](
                   EpisodeEntry* loaded, const QMap<int, QString>& qualities) {
            if (self.isNull() || self->closed_ || target.isNull() ||
                loaded == nullptr || loaded->embedUrl != embed) {
                return;
            }
            PlayerSession* session = self->sessionById(sessionId);
            if (session == nullptr || session->controller != target.data() ||
                seq != session->sourceSequence || session->entry != previousEntry ||
                previousEntry->embedUrl != previousEmbed ||
                target->currentEmbed() != previousEmbed) {
                return;
            }
            session->entry = loaded;
            target->applyVariant(toQualities(qualities), loaded->label, loaded->embedUrl);
            self->requestSkipSegments(session);
        });
}

void TitlePage::probeTrackQuality(PlayerSession* session, const QString& embed) {
    // Answer a track's quality request, sharing both the cache and any in-flight
    // extraction across every player session.
    if (closed_ || !isLiveSession(session) || session->controller == nullptr ||
        session->entry == nullptr) {
        return;
    }
    const QMap<int, QString> cached = qualityCache_.qualities(embed);
    if (!cached.isEmpty()) {
        session->controller->setTrackQuality(embed, maxQuality(cached));
        return;
    }

    const QString sourceEmbed = session->entry->embedUrl;
    if (sourceEmbed.isEmpty() || session->controller->currentEmbed() != sourceEmbed) {
        return;
    }
    const int sourceSequence = session->sourceSequence;
    const quint64 sessionId = session->id;
    auto& waiters = trackWaiters_[embed];
    for (TrackWaiter& waiter : waiters) {
        if (waiter.sessionId != sessionId) {
            continue;
        }
        if (waiter.sourceSequence == sourceSequence && waiter.entry == session->entry &&
            waiter.sourceEmbed == sourceEmbed) {
            return;
        }
        waiter.sourceSequence = sourceSequence;
        waiter.entry = session->entry;
        waiter.sourceEmbed = sourceEmbed;
        return;
    }

    const bool startFetch = !trackProbesInFlight_.contains(embed);
    waiters.append({sessionId, sourceSequence, session->entry, sourceEmbed});
    if (!startFetch) {
        return;
    }
    trackProbesInFlight_.insert(embed);

    const QPointer<TitlePage> self(this);
    runner_.runStream(
        [embed] { return QVariant::fromValue(backend::qualityStreams(embed)); },
        [self, embed](const QVariant& result) {
            if (self.isNull()) {
                return;
            }
            self->trackProbesInFlight_.remove(embed);
            if (self->closed_) {
                self->trackWaiters_.remove(embed);
                return;
            }
            const QVector<TrackWaiter> waiters = self->trackWaiters_.take(embed);
            const QMap<int, QString> qualities = result.value<QMap<int, QString>>();
            QVector<QPointer<mplayer::quick::PlayerController>> recipients;
            if (!qualities.isEmpty()) {
                for (const TrackWaiter& waiter : waiters) {
                    PlayerSession* session = self->sessionById(waiter.sessionId);
                    if (session == nullptr || session->controller == nullptr ||
                        session->sourceSequence != waiter.sourceSequence ||
                        session->entry != waiter.entry || waiter.entry == nullptr ||
                        waiter.entry->embedUrl != waiter.sourceEmbed ||
                        session->controller->currentEmbed() != waiter.sourceEmbed) {
                        continue;
                    }
                    recipients.append(session->controller);
                }
            }
            if (recipients.isEmpty()) {
                return;
            }
            self->qualityCache_.remember(embed, qualities);
            const int height = maxQuality(qualities);
            for (const QPointer<mplayer::quick::PlayerController>& controller : recipients) {
                if (!controller.isNull()) {
                    controller->setTrackQuality(embed, height);
                }
            }
        },
        [self, embed](const backend::Failure&) {
            if (self.isNull()) {
                return;
            }
            self->trackProbesInFlight_.remove(embed);
            self->trackWaiters_.remove(embed);
        });
}

void TitlePage::playEpisodeFrom(PlayerSession* session, const QVariantMap& item, int index) {
    if (closed_ || !isLiveSession(session) || session->controller == nullptr) {
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
    const QPointer<mplayer::quick::PlayerController> target(session->controller);
    const EpisodeEntry* const previousEntry = session->entry;
    const QString previousEmbed = previousEntry != nullptr ? previousEntry->embedUrl : QString();
    const QString requestedEmbed = entry->embedUrl;
    const int seq = ++session->sourceSequence;
    const quint64 sessionId = session->id;
    ensureQualities(
        entry, [self, target, sessionId, index, variants, seq, previousEntry, previousEmbed,
                requestedEmbed](EpisodeEntry* loaded,
                                                          const QMap<int, QString>& qualities) {
            if (self.isNull() || self->closed_ || target.isNull()) {
                return;
            }
            PlayerSession* session = self->sessionById(sessionId);
            if (session == nullptr || session->controller != target.data() ||
                seq != session->sourceSequence || session->entry != previousEntry ||
                loaded == nullptr || loaded->embedUrl != requestedEmbed ||
                (!previousEmbed.isEmpty() && target->currentEmbed() != previousEmbed)) {
                return;
            }
            session->entry = loaded;
            // Continue a half-watched episode instead of restarting it.
            target->setEpisodeResumePoint(self->watchHistory_.offset(self->ref_, loaded->number));
            session->controller->applyEpisode(loaded->label, loaded->embedUrl, index,
                                          toQualities(qualities), variants);
            self->requestSkipSegments(session);
        });
}


bool TitlePage::shutdown(bool forced) {
    if (closed_) {
        return true;
    }
    // Every player acknowledges its own stop before anything is disposed. A
    // refusal keeps the page, its sessions and their controllers exactly as they are,
    // so the shutdown can be retried; only the destructor, which cannot refuse
    // destruction, disposes a refused session anyway - and even then each
    // controller's destructor releases its session only after the core has died.
    // Ids are collected first: disposing a session erases it from the vector.
    QVector<quint64> open;
    open.reserve(sessions_.size());
    for (const auto& held : sessions_) open.append(held->id);
    bool refused = false;
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
            refused = true;
            qWarning().noquote() << "mikai: title shutdown refused (native stop unsettled)";
            continue;
        }
        // Destruction cannot be refused, so the forced stop is named rather than
        // probed; each controller's destructor still releases what it held only after
        // its core has synchronously died.
        qWarning().noquote()
            << "mikai: forced title teardown (destruction cannot be refused; native claims are "
               "released only after the core is destroyed)";
        disposeSession(sessionId, true);
    }
    if (refused) {
        return false;
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
    for (const auto& held : sessions_) {
        ++held->runtimeSequence;
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
