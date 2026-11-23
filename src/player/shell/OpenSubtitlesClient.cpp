#include "OpenSubtitlesClient.hpp"

#include <backend/HttpTransport.hpp>
#include <backend/Cancellation.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTimer>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace mplayer {
namespace shell {
namespace {

constexpr char kBaseUrl[] = "https://api.opensubtitles.com/api/v1";
#ifdef MIKAI_PROJECT_VERSION
constexpr char kUserAgent[] = "Mikai Ja Nai " MIKAI_PROJECT_VERSION;
#else
constexpr char kUserAgent[] = "Mikai Ja Nai";
#endif
constexpr qint64 kMaxJsonResponseBytes = 2 * 1024 * 1024;

QString computeOpenSubtitlesMovieHash(const QString& localPath) {
    QFile file(localPath);
    const qint64 size = file.size();
    constexpr qint64 kChunk = 65536;
    if (size < kChunk * 2 || !file.open(QIODevice::ReadOnly)) return {};
    quint64 hash = static_cast<quint64>(size);
    const auto accumulate = [&hash](const QByteArray& bytes) {
        const auto* ptr = reinterpret_cast<const unsigned char*>(bytes.constData());
        for (qsizetype i = 0; i + 8 <= bytes.size(); i += 8) {
            quint64 word = 0;
            for (int b = 0; b < 8; ++b) word |= static_cast<quint64>(ptr[i + b]) << (b * 8);
            hash += word;
        }
    };
    const QByteArray head = file.read(kChunk);
    if (head.size() != kChunk) return {};
    accumulate(head);
    if (!file.seek(size - kChunk)) return {};
    const QByteArray tail = file.read(kChunk);
    if (tail.size() != kChunk) return {};
    accumulate(tail);
    return QStringLiteral("%1").arg(hash, 16, 16, QLatin1Char('0'));
}
constexpr qint64 kMaxSubtitleResponseBytes = 10 * 1024 * 1024;

bool isSafeOpenSubtitlesDownloadUrl(const QUrl& url) {
    if (!url.isValid() || url.isRelative()) return false;
    if (url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0) return false;
    if (!url.userInfo().isEmpty()) return false;
    const QString host = url.host().trimmed().toLower();
    if (host.isEmpty()) return false;
    if (host == QLatin1String("localhost") || host.endsWith(QLatin1String(".localhost")) ||
        host.endsWith(QLatin1String(".local")) || host.endsWith(QLatin1String(".internal"))) {
        return false;
    }
    QHostAddress addr;
    if (addr.setAddress(host)) {
        if (addr.isNull() || addr.isLoopback() || addr.isLinkLocal() || addr.isPrivateUse() ||
            addr.isUniqueLocalUnicast() || addr.isMulticast() || addr.isBroadcast() ||
            addr == QHostAddress::Any || addr == QHostAddress::AnyIPv4 ||
            addr == QHostAddress::AnyIPv6) {
            return false;
        }
    }
    return true;
}

QNetworkRequest makeRequest(const QString& path, const QString& apiKey) {
    QNetworkRequest request{QUrl(QString::fromLatin1(kBaseUrl) + path)};
    // The service rejects a request without these two; the key is per account.
    request.setRawHeader("Api-Key", apiKey.toUtf8());
    request.setRawHeader("User-Agent", QByteArray(kUserAgent));
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    return request;
}

/// One POST on the calling thread with bounded size, timeout, and cancellation;
/// the download endpoint accepts no other verb.
QByteArray postJson(const QNetworkRequest& request, const QByteArray& body,
                    int timeoutMs, qint64 maxBytes, QString* error) {
    if (error != nullptr) error->clear();
    if (mikai::backend::WorkCancellationScope::cancellationRequested()) {
        if (error != nullptr) *error = QStringLiteral("cancelled");
        return {};
    }
    QNetworkAccessManager* manager = mikai::backend::transportManagerForThread();
    QNetworkRequest post = request;
    post.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = manager->post(post, body);
    QByteArray payload;
    bool timedOut = false;
    bool cancelled = false;
    bool tooLarge = false;

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QTimer cancelPoll;
    cancelPoll.setInterval(100);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        if (reply->isRunning()) reply->abort();
        loop.quit();
    });
    QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&] {
        if (mikai::backend::WorkCancellationScope::cancellationRequested()) {
            cancelled = true;
            if (reply->isRunning()) reply->abort();
            loop.quit();
        }
    });
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        const QByteArray chunk = reply->readAll();
        if (maxBytes > 0 && static_cast<qint64>(payload.size()) + chunk.size() > maxBytes) {
            tooLarge = true;
            payload.clear();
            if (reply->isRunning()) reply->abort();
            loop.quit();
            return;
        }
        payload.append(chunk);
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished()) {
        timer.start(timeoutMs);
        cancelPoll.start();
        loop.exec();
        timer.stop();
        cancelPoll.stop();
    }

    if (reply->isRunning()) {
        reply->abort();
        QEventLoop drain;
        QTimer drainTimer;
        drainTimer.setSingleShot(true);
        QObject::connect(reply, &QNetworkReply::finished, &drain, &QEventLoop::quit);
        QObject::connect(&drainTimer, &QTimer::timeout, &drain, &QEventLoop::quit);
        drainTimer.start(1000);
        if (!reply->isFinished()) drain.exec();
    }
    if (!tooLarge && !cancelled && !timedOut) {
        const QByteArray rest = reply->readAll();
        if (maxBytes > 0 && static_cast<qint64>(payload.size()) + rest.size() > maxBytes) {
            tooLarge = true;
            payload.clear();
        } else {
            payload.append(rest);
        }
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (error != nullptr) {
        if (cancelled) {
            *error = QCoreApplication::translate("OpenSubtitlesClient", "Search cancelled.");
        } else if (timedOut) {
            *error = QCoreApplication::translate("OpenSubtitlesClient", "The request timed out.");
        } else if (tooLarge) {
            *error = QCoreApplication::translate("OpenSubtitlesClient", "The reply exceeded the size limit.");
        } else if (reply->error() != QNetworkReply::NoError && status == 0) {
            *error = reply->errorString();
        } else if (status < 200 || status >= 300) {
            *error = QCoreApplication::translate("OpenSubtitlesClient", "HTTP %1").arg(status);
        }
    }
    reply->deleteLater();
    return (cancelled || timedOut || tooLarge) ? QByteArray() : payload;
}

}  // namespace

OpenSubtitlesSearch searchOpenSubtitles(const QString& apiKey, const QString& query,
                                        const QString& languageCode, int timeoutMs) {
    OpenSubtitlesSearch result;
    const QString trimmedKey = apiKey.trimmed();
    if (trimmedKey.isEmpty()) {
        result.error = QCoreApplication::translate("OpenSubtitlesClient", "An API key is required.");
        return result;
    }
    const QString language = languageCode.trimmed();
    const auto fillError = [&result](const QString& text) { result.error = text; };

    QUrl url(QString::fromLatin1(kBaseUrl) + QStringLiteral("/subtitles"));
    QUrlQuery params;
    const QString trimmedQuery = query.trimmed();
    if (QFileInfo::exists(trimmedQuery) && QFileInfo(trimmedQuery).isFile()) {
        const QString movieHash = computeOpenSubtitlesMovieHash(trimmedQuery);
        if (!movieHash.isEmpty()) {
            params.addQueryItem(QStringLiteral("moviehash"), movieHash);
        }
        params.addQueryItem(QStringLiteral("query"), QFileInfo(trimmedQuery).completeBaseName());
    } else {
        params.addQueryItem(QStringLiteral("query"), trimmedQuery);
    }
    if (!language.isEmpty()) params.addQueryItem(QStringLiteral("languages"), language);
    params.addQueryItem(QStringLiteral("order_by"), QStringLiteral("download_count"));
    params.addQueryItem(QStringLiteral("order_direction"), QStringLiteral("desc"));
    url.setQuery(params);

    QNetworkRequest request = makeRequest(QStringLiteral("/subtitles"), trimmedKey);
    request.setUrl(url);
    const mikai::backend::HttpFetchResult fetch = mikai::backend::runBlockingFetch(
        mikai::backend::transportManagerForThread(), request,
        mikai::backend::HttpFetchOptions{timeoutMs, false, {}, kMaxJsonResponseBytes});
    if (fetch.cancelled) {
        fillError(QCoreApplication::translate("OpenSubtitlesClient", "Search cancelled."));
        return result;
    }
    if (fetch.timedOut) {
        fillError(QCoreApplication::translate("OpenSubtitlesClient", "The request timed out."));
        return result;
    }
    if (fetch.responseTooLarge) {
        fillError(QCoreApplication::translate("OpenSubtitlesClient", "The reply exceeded the size limit."));
        return result;
    }
    if (fetch.status < 200 || fetch.status >= 300) {
        fillError(fetch.networkError.isEmpty()
                      ? QCoreApplication::translate("OpenSubtitlesClient", "HTTP %1").arg(fetch.status)
                      : fetch.networkError);
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(fetch.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        fillError(QCoreApplication::translate("OpenSubtitlesClient", "The reply was not readable."));
        return result;
    }
    const QJsonArray data = document.object().value(QStringLiteral("data")).toArray();
    for (const QJsonValue& entry : data) {
        const QJsonObject attributes = entry.toObject().value(QStringLiteral("attributes")).toObject();
        const QJsonArray files = attributes.value(QStringLiteral("files")).toArray();
        const QString language = attributes.value(QStringLiteral("language")).toString();
        for (const QJsonValue& file : files) {
            const QJsonObject object = file.toObject();
            OpenSubtitlesHit hit;
            hit.fileId = object.value(QStringLiteral("file_id")).toInt();
            hit.language = language;
            hit.releaseName = object.value(QStringLiteral("file_name")).toString();
            hit.hearingImpaired =
                attributes.value(QStringLiteral("hearing_impaired")).toBool()
                    ? QStringLiteral("HI")
                    : QString();
            hit.downloads = attributes.value(QStringLiteral("download_count")).toInt();
            if (hit.fileId > 0) result.hits.append(hit);
        }
    }
    return result;
}

QByteArray downloadOpenSubtitles(const QString& apiKey, int fileId, QString* error,
                                 int timeoutMs) {
    if (error != nullptr) error->clear();
    const auto fail = [error](const QString& text) {
        if (error != nullptr) *error = text;
        return QByteArray();
    };
    if (apiKey.trimmed().isEmpty()) return fail(QStringLiteral("an API key is required"));
    if (fileId <= 0) return fail(QStringLiteral("no subtitle was selected"));

    QString postError;
    const QByteArray body =
        QStringLiteral("{\"file_id\":%1}").arg(fileId).toUtf8();
    const QByteArray reply =
        postJson(makeRequest(QStringLiteral("/download"), apiKey), body, timeoutMs,
                 kMaxJsonResponseBytes, &postError);
    if (!postError.isEmpty()) return fail(postError);
    const QJsonDocument document = QJsonDocument::fromJson(reply);
    const QString link = document.object().value(QStringLiteral("link")).toString().trimmed();
    if (link.isEmpty()) return fail(QStringLiteral("the service offered no file"));

    const QUrl downloadUrl(link, QUrl::StrictMode);
    if (!isSafeOpenSubtitlesDownloadUrl(downloadUrl)) {
        return fail(QStringLiteral("the service offered an unsafe download URL"));
    }

    QNetworkRequest fileRequest{downloadUrl};
    fileRequest.setRawHeader("User-Agent", QByteArray(kUserAgent));
    fileRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
    const mikai::backend::HttpFetchResult fetch = mikai::backend::runBlockingFetch(
        mikai::backend::transportManagerForThread(), fileRequest,
        mikai::backend::HttpFetchOptions{timeoutMs, false, {}, kMaxSubtitleResponseBytes});
    if (fetch.cancelled) return fail(QStringLiteral("cancelled"));
    if (fetch.timedOut) return fail(QStringLiteral("the download timed out"));
    if (fetch.responseTooLarge) return fail(QStringLiteral("the subtitle file exceeded the size limit"));
    if (fetch.status < 200 || fetch.status >= 300 || fetch.body.isEmpty()) {
        return fail(fetch.networkError.isEmpty() ? QStringLiteral("the download failed")
                                                 : fetch.networkError);
    }
    return fetch.body;
}

}  // namespace shell
}  // namespace mplayer
