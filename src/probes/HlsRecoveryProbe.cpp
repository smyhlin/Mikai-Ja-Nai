// The expired signed-HLS recovery probe.
//
// A viewer holding a cached signed HLS URL whose signature has expired gets a
// 403, and the app recovers by re-extracting the master and playing the fresh
// signed URL. This probe drives that exact path through the public
// PlayerDialog/PlayerHost bridge against the hermetic loopback fixture:
//
//   seed: fetch the default /hls/master.m3u8 (whose v360 rung is signed with an
//   expiry one hour in the past) and cache that map, as a session that browsed
//   earlier would have;
//   fail: play the stale 360 URL. mpv's real 403 surfaces as an engine error,
//   the dialog shows its visible retry/status, and it calls
//   PlayerHost::playbackFailed() - the same public bridge TitlePage::Host uses;
//   recover: the host forgets the dead cache entry, re-fetches
//   /hls/master.m3u8?fresh=1 on the real Runner with a worker-local
//   QNetworkAccessManager, remembers the refreshed map and hands it back through
//   applyRefreshedStreams. mpv then plays the fresh 360 rung and a real colored
//   frame is read off the surface;
//   prove: the fixture's recorded header facts show the 403 and the later 200
//   for the same variant, both carrying Accept-Language.
//
// Two further independent scenarios keep the failure policy honest: a
// re-extraction that itself fails must leave a visible error up, and a
// processing failure (the public reportNativeProcessingFailure) must offer no
// retry and must never ask the host to re-extract. No native producer is
// claimed and the private TitlePage host is not reached.
//
// The base URL argument must be loopback: this probe never opens an outbound
// connection.

#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QMap>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>
#include <cstdio>
#include <functional>
#include <utility>

#include <backend/Streams.hpp>
#include "CrashLog.hpp"
#include "PlayerDialog.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSurface.hpp"
#include "state/QualityCache.hpp"
#include "theme/Theme.hpp"
#include "workers/Runner.hpp"

using mplayer::shell::PlayerDialog;
using mplayer::shell::PlayerHost;
using mplayer::shell::QualityEntry;

namespace {

/// The rung the fixture deliberately signs with a stale expiry.
constexpr int kTargetHeight = 360;
/// How long the visible failure is left up before the host's asynchronous
/// re-extraction lands. A real host re-extracts off-thread; pacing it keeps the
/// failure on screen long enough for the probe (and a screenshot) to observe it.
constexpr int kRecoveryDelayMs = 700;

int failures = 0;

void check(bool condition, const char* description) {
    std::fprintf(stderr, "%s %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) ++failures;
}

bool until(const std::function<bool()>& predicate, int timeout = 10000) {
    for (int elapsed = 0; elapsed < timeout; elapsed += 50) {
        if (predicate()) return true;
        QTest::qWait(50);
    }
    return predicate();
}

/// The probe is a hermetic fixture client: every URL must name the loopback
/// interface, so a mistaken argument cannot turn a QA run into an outbound
/// request against a real host.
bool loopbackUrl(const QUrl& url) {
    if (!url.isValid() || (url.scheme() != QLatin1String("http") &&
                           url.scheme() != QLatin1String("https"))) {
        return false;
    }
    const QString host = url.host();
    return host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost") ||
           host == QLatin1String("::1");
}

/// The one header the fixture insists on; the extraction path sends the same
/// locale, and mpv's own http-header-fields sends its comma-free twin.
QMap<QString, QString> fixtureHeaders() {
    QMap<QString, QString> headers;
    headers.insert(QStringLiteral("Accept-Language"), QStringLiteral("uk-UA"));
    return headers;
}

bool retryVisible(PlayerDialog* player) {
    if (player == nullptr) return false;
    auto* retry = player->findChild<QPushButton*>(QStringLiteral("playerRetryButton"));
    return retry != nullptr && retry->isVisible();
}

bool statusPanelVisible(PlayerDialog* player) {
    if (player == nullptr) return false;
    auto* panel = player->findChild<QWidget*>(QStringLiteral("playerStatusPanel"));
    return panel != nullptr && panel->isVisible();
}

QString statusText(PlayerDialog* player) {
    if (player == nullptr) return {};
    auto* text = player->findChild<QLabel*>(QStringLiteral("playerStatusText"));
    return text != nullptr ? text->text() : QString();
}

/// A frame from a real decoding surface carries saturated pixels; a blank or
/// failed one does not.
bool colorful(const QImage& image) {
    int hits = 0;
    for (int y = image.height() / 4; y < image.height() * 3 / 4; y += 8) {
        for (int x = image.width() / 4; x < image.width() * 3 / 4; x += 8) {
            const QColor pixel = image.pixelColor(x, y);
            if (qMax(pixel.red(), qMax(pixel.green(), pixel.blue())) -
                qMin(pixel.red(), qMin(pixel.green(), pixel.blue())) > 30) {
                ++hits;
            }
        }
    }
    return hits > 100;
}

/// The public host bridge, exercised exactly as TitlePage::Host drives it: a
/// media failure forgets the dead signed URL, re-extracts the fresh master on
/// the real Runner, remembers the refreshed map and hands it back. Non-owning
/// and outliving the dialog, like the real host.
class RecoveryHost : public QObject, public PlayerHost {
public:
    RecoveryHost(mikai::app::Runner& runner, mikai::app::QualityCache& cache, QString embed,
                 QString freshMasterUrl)
        : runner_(runner), cache_(cache), embed_(std::move(embed)),
          freshMasterUrl_(std::move(freshMasterUrl)) {}

    void attach(PlayerDialog* dialog) { dialog_ = dialog; }

    int failed() const { return failed_; }
    bool recovered() const { return recovered_; }
    bool closed() const { return closed_; }
    bool forgotEmbed() const { return forgotEmbed_; }
    bool failureRetryVisible() const { return failureRetryVisible_; }
    QString failureStatusText() const { return failureStatusText_; }
    QString recoveryError() const { return error_; }
    QMap<int, QString> refreshedMap() const { return refreshed_; }

    // PlayerHost ────────────────────────────────────────────────────────────
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int) override {}
    void requestTrackProbe(const QString&) override {}
    void requestOpenOriginal() override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override { closed_ = true; }

    /// The stream could not be played. Record the visible failure while it is
    /// still on screen, drop the dead cache entry, then re-extract off-thread.
    void playbackFailed() override {
        ++failed_;
        if (recovering_) return;  // one re-extraction per failed stream
        recovering_ = true;
        if (dialog_ != nullptr) {
            failureRetryVisible_ = retryVisible(dialog_);
            failureStatusText_ = statusText(dialog_);
        }
        cache_.forget(embed_);
        forgotEmbed_ = cache_.qualities(embed_).isEmpty();
        const QPointer<RecoveryHost> self(this);
        QTimer::singleShot(kRecoveryDelayMs, this, [self] {
            if (self.isNull() || !self->recovering_) return;
            self->startFetch();
        });
    }

private:
    void startFetch() {
        const QPointer<RecoveryHost> self(this);
        const QString url = freshMasterUrl_;
        runner_.runStream(
            [url] {
                // Worker-local: one manager for this fetch, torn down with the
                // task, so no HTTP object crosses threads.
                QNetworkAccessManager manager;
                const QString text = mikai::backend::fetchText(&manager, url, fixtureHeaders());
                QMap<int, QString> map;
                for (const mikai::backend::QualityVariant& variant :
                     mikai::backend::parseVariants(text, url)) {
                    if (variant.height > 0 && !variant.url.isEmpty()) {
                        map.insert(variant.height, variant.url);
                    }
                }
                return QVariant::fromValue(map);
            },
            [self](const QVariant& result) {
                if (!self.isNull()) self->finishRecovery(result.value<QMap<int, QString>>());
            },
            [self](const QString& message) {
                if (!self.isNull()) self->failRecovery(message);
            });
    }

    void finishRecovery(const QMap<int, QString>& map) {
        if (map.isEmpty()) {
            failRecovery(QStringLiteral("the fresh master carried no variants"));
            return;
        }
        cache_.remember(embed_, map);
        refreshed_ = map;
        recovered_ = true;
        if (dialog_ == nullptr) return;
        QList<QualityEntry> qualities;
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            QualityEntry entry;
            entry.height = it.key();
            entry.url = it.value();
            qualities.append(entry);
        }
        dialog_->applyRefreshedStreams(qualities, kTargetHeight);
    }

    void failRecovery(const QString& message) {
        error_ = message;
        // A failed re-extraction leaves the visible error up; nothing pretends
        // the stream recovered.
        if (dialog_ != nullptr) dialog_->showError(message);
    }

    mikai::app::Runner& runner_;
    mikai::app::QualityCache& cache_;
    QString embed_;
    QString freshMasterUrl_;
    QPointer<PlayerDialog> dialog_;
    QMap<int, QString> refreshed_;
    QString error_;
    QString failureStatusText_;
    int failed_ = 0;
    bool recovered_ = false;
    bool recovering_ = false;
    bool forgotEmbed_ = false;
    bool failureRetryVisible_ = false;
    bool closed_ = false;
};

QList<QualityEntry> toQualities(const QMap<int, QString>& map) {
    QList<QualityEntry> qualities;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        QualityEntry entry;
        entry.height = it.key();
        entry.url = it.value();
        qualities.append(entry);
    }
    return qualities;
}

/// Reads /_facts over real HTTP and reports what the fixture recorded: the
/// stale 403, the later 200 for the same variant, both master fetches, and the
/// Accept-Language header on every v360 playlist request.
struct FactReading {
    QString error;
    bool readable = false;
    bool stale403 = false;
    bool fresh200 = false;
    bool staleBeforeFresh = false;
    bool masterFetched = false;
    bool freshMasterFetched = false;
    bool sawV360 = false;
    bool acceptLanguageOnV360 = true;
};

FactReading readFacts(const QUrl& factsUrl) {
    FactReading reading;
    QJsonObject root;
    {
        QNetworkAccessManager manager;
        try {
            const QString body =
                mikai::backend::fetchText(&manager, factsUrl.toString(), fixtureHeaders());
            QJsonParseError parse{};
            const QJsonDocument document = QJsonDocument::fromJson(body.toUtf8(), &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject()) {
                reading.error = QStringLiteral("facts are not a JSON object");
                return reading;
            }
            root = document.object();
        } catch (const std::exception& exception) {
            reading.error = QString::fromUtf8(exception.what());
            return reading;
        }
    }
    reading.readable = true;
    int staleSeq = -1;
    int freshSeq = -1;
    const QJsonArray requests = root.value(QStringLiteral("requests")).toArray();
    for (const QJsonValue& value : requests) {
        const QJsonObject fact = value.toObject();
        const QString path = fact.value(QStringLiteral("path")).toString();
        const int status = fact.value(QStringLiteral("status")).toInt();
        const int seq = fact.value(QStringLiteral("seq")).toInt();
        bool hasFreshKey = false;
        for (const QJsonValue& key : fact.value(QStringLiteral("queryKeys")).toArray()) {
            if (key.toString() == QLatin1String("fresh")) hasFreshKey = true;
        }
        if (path == QLatin1String("/hls/master.m3u8")) {
            if (hasFreshKey) {
                reading.freshMasterFetched = true;
            } else {
                reading.masterFetched = true;
            }
        }
        if (path == QLatin1String("/hls/v360/playlist.m3u8")) {
            reading.sawV360 = true;
            if (status == 403) {
                reading.stale403 = true;
                staleSeq = seq;
            }
            // The fixture serves the playlist with a ranged 206, like the real
            // CDN serves ranged media: either success state proves the fresh
            // signature verifies.
            if (status == 200 || status == 206) {
                reading.fresh200 = true;
                freshSeq = seq;
            }
            if (!fact.value(QStringLiteral("acceptLanguage"))
                     .toObject()
                     .value(QStringLiteral("present"))
                     .toBool()) {
                reading.acceptLanguageOnV360 = false;
            }
        }
    }
    reading.staleBeforeFresh = staleSeq >= 0 && freshSeq >= 0 && staleSeq < freshSeq;
    return reading;
}

}  // namespace

int main(int argc, char** argv) {
    mikai::app::installCrashLog();
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    if (argc != 2) {
        std::fprintf(stderr,
                     "usage: hls_recovery_probe <fixture base URL>\n"
                     "The URL must be loopback (127.0.0.1/localhost/::1); the probe derives\n"
                     "/hls/master.m3u8, /hls/master.m3u8?fresh=1 and /_facts from it and never\n"
                     "opens an outbound connection.\n");
        return 2;
    }
    const QUrl base(QString::fromLocal8Bit(argv[1]));
    if (!loopbackUrl(base)) {
        std::fprintf(stderr, "FAIL fixture base URL is not loopback: %s\n", argv[1]);
        return 2;
    }
    const QUrl masterUrl = base.resolved(QUrl(QStringLiteral("/hls/master.m3u8")));
    QUrl freshMasterUrl = masterUrl;
    freshMasterUrl.setQuery(QStringLiteral("fresh=1"));
    const QUrl factsUrl = base.resolved(QUrl(QStringLiteral("/_facts")));

    // The dialog reads player/anime4k from settings, so both scopes are
    // redirected into a directory that dies with this process before the first
    // one is built. The probe must not inherit - or overwrite - a real session.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("HlsRecovery"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    if (!mplayer::probes::selectStandardRuntime()) {
        return 2;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("player/anime4k"), QStringLiteral("off"));
    settings.setValue(QStringLiteral("home/feeds"), QStringLiteral(""));
    app.setStyleSheet(mikai::app::theme::buildStylesheet());

    // Seed the cache exactly as a browsing session would: fetch the real default
    // master and keep its ladder, whose v360 rung is signed with a stale expiry.
    QMap<int, QString> stale;
    QString seedError;
    {
        QNetworkAccessManager manager;
        try {
            const QString text =
                mikai::backend::fetchText(&manager, masterUrl.toString(), fixtureHeaders());
            for (const mikai::backend::QualityVariant& variant :
                 mikai::backend::parseVariants(text, masterUrl.toString())) {
                if (variant.height > 0 && !variant.url.isEmpty()) {
                    stale.insert(variant.height, variant.url);
                }
            }
        } catch (const std::exception& exception) {
            seedError = QString::fromUtf8(exception.what());
        }
    }
    check(seedError.isEmpty() && stale.contains(kTargetHeight),
          "the fixture master serves a 360 rung to seed the stale cache");
    if (!stale.contains(kTargetHeight)) {
        std::fprintf(stderr, "RESULT: FAIL\n");
        return 1;
    }
    bool staleSignatureExpired = false;
    {
        const QUrl seeded(stale.value(kTargetHeight));
        const QUrlQuery query(seeded);
        bool ok = false;
        const qint64 expiry = query.queryItemValue(QStringLiteral("exp")).toLongLong(&ok);
        staleSignatureExpired = ok && expiry <= QDateTime::currentSecsSinceEpoch();
    }
    check(staleSignatureExpired, "the seeded 360 URL carries a signature that already expired");

    const QString embed = masterUrl.toString();  // cache key; never handed to detectSite()
    mikai::app::QualityCache cache;
    cache.load();
    cache.remember(embed, stale);

    mikai::app::Runner runner;

    // ── scenario 1: expiry → visible failure → fresh stream recovery ─────────
    RecoveryHost host(runner, cache, embed, freshMasterUrl.toString());
    auto* player = new PlayerDialog(QStringLiteral("HLS recovery"), stale.value(kTargetHeight),
                                    toQualities(stale), kTargetHeight, &host, nullptr);
    host.attach(player);
    player->show();
    QTest::qWait(250);
    player->startPlayer();
    QPointer<PlayerDialog> dialog(player);

    check(until([&host] { return host.failed() > 0; }, 30000),
          "the stale signed URL fails through the public PlayerHost bridge");
    check(host.failureRetryVisible() && !host.failureStatusText().isEmpty(),
          "the expired stream leaves a visible retry chip and status");
    check(host.forgotEmbed(), "the host forgets the dead signed URL before re-extracting");
    if (dialog) dialog->grab().save(QStringLiteral("hls-expired-failure.png"));

    // The 403 is proven from the fixture's own record before any recovery is
    // claimed, so a later success can never stand in for a missing failure.
    const FactReading staleFacts = readFacts(factsUrl);
    check(staleFacts.readable && staleFacts.error.isEmpty(),
          "the fixture header facts are readable");
    check(staleFacts.stale403 && staleFacts.masterFetched,
          "the fixture recorded the stale variant's 403 after the default master");

    check(until([&host] { return host.recovered() || !host.recoveryError().isEmpty(); }, 30000),
          "the host re-extracts a fresh master asynchronously");
    check(host.recoveryError().isEmpty() && host.recovered(),
          "the fresh master yields a refreshed quality map");

    bool rendered = false;
    if (dialog) {
        auto* surface = dialog->findChild<mplayer::VideoSurface*>();
        rendered = surface != nullptr &&
                   until([surface] { return surface->hasVideo() && surface->duration() > 3.0; }, 30000);
        if (rendered) {
            surface->setPaused(true);
            QTest::qWait(300);
            const QImage frame = surface->grabFramebuffer();
            rendered = !frame.isNull() && surface->videoWidth() == 640 &&
                       surface->videoHeight() == 360 && colorful(frame);
            dialog->grab().save(QStringLiteral("hls-recovered-stream.png"));
        }
    }
    check(rendered, "the refreshed 360 stream renders a real 640x360 colored frame");
    check(dialog && !retryVisible(dialog), "recovery clears the retry chip");
    check(host.recovered() &&
              host.refreshedMap().value(kTargetHeight) != stale.value(kTargetHeight),
          "the refreshed map holds a new 360 URL");
    check(cache.qualities(embed).value(kTargetHeight) ==
              host.refreshedMap().value(kTargetHeight),
          "the quality cache remembers the refreshed map");

    const FactReading freshFacts = readFacts(factsUrl);
    check(freshFacts.fresh200 && freshFacts.freshMasterFetched && freshFacts.staleBeforeFresh,
          "the fresh master serves the same variant 200 only after the 403");
    check(freshFacts.sawV360 && freshFacts.acceptLanguageOnV360,
          "every v360 playlist request carried an Accept-Language header");

    if (dialog) {
        dialog->close();
        check(until([&dialog] { return dialog.isNull(); }),
              "the recovered player closes safely");
        check(host.closed(), "closing the player notifies the host");
    }

    // ── scenario 2: the re-extraction itself fails → visible error stays ─────
    const QUrl missingMaster = base.resolved(QUrl(QStringLiteral("/hls/missing.m3u8")));
    RecoveryHost brokenHost(runner, cache, embed, missingMaster.toString());
    auto* broken = new PlayerDialog(QStringLiteral("HLS recovery (failed refresh)"),
                                    stale.value(kTargetHeight), toQualities(stale), kTargetHeight,
                                    &brokenHost, nullptr);
    brokenHost.attach(broken);
    broken->show();
    QTest::qWait(250);
    broken->startPlayer();
    QPointer<PlayerDialog> brokenPointer(broken);

    check(until([&brokenHost] { return brokenHost.failed() > 0; }, 30000),
          "the second stale stream also fails through the host bridge");
    check(until([&brokenHost] { return !brokenHost.recoveryError().isEmpty(); }, 30000),
          "a failed re-extraction reports an error through the host");
    check(!brokenHost.recovered() && retryVisible(brokenPointer) &&
              !statusText(brokenPointer).isEmpty(),
          "a failed re-extraction leaves the visible error up");
    if (brokenPointer) brokenPointer->grab().save(QStringLiteral("hls-refresh-failed.png"));
    if (brokenPointer) {
        brokenPointer->close();
        check(until([&brokenPointer] { return brokenPointer.isNull(); }),
              "the failed-recovery player closes safely");
    }

    // ── scenario 3: processing failure policy ───────────────────────────────
    // A processing failure is not a media failure: it shows its reason but
    // offers no retry and never asks the host to re-extract. This makes no claim
    // about a real native producer; it only exercises the public report.
    RecoveryHost quietHost(runner, cache, embed, freshMasterUrl.toString());
    auto* quiet = new PlayerDialog(QStringLiteral("HLS recovery (processing policy)"),
                                   stale.value(kTargetHeight), toQualities(stale), kTargetHeight,
                                   &quietHost, nullptr);
    quietHost.attach(quiet);
    quiet->show();
    QTest::qWait(250);
    quiet->reportNativeProcessingFailure(QStringLiteral("fixture processing failure"));
    QTest::qWait(200);
    QPointer<PlayerDialog> quietPointer(quiet);
    check(!retryVisible(quietPointer) && quietHost.failed() == 0 &&
              statusPanelVisible(quietPointer) && !statusText(quietPointer).isEmpty(),
          "a processing failure shows its reason but offers no retry or re-extraction");
    if (quietPointer) {
        quietPointer->close();
        check(until([&quietPointer] { return quietPointer.isNull(); }),
              "the processing-policy player closes safely");
    }

    std::fprintf(stderr, "RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
