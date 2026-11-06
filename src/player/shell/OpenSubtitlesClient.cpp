#include "OpenSubtitlesClient.hpp"

#include <backend/HttpTransport.hpp>

#include <QtCore/QEventLoop>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTimer>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace mplayer {
namespace shell {
namespace {

constexpr char kBaseUrl[] = "https://api.opensubtitles.com/api/v1";
constexpr char kUserAgent[] = "Mikai Ja Nai 0.2.0";

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

/// One POST on the calling thread with the same bounded wait the GET transport
/// uses; the download endpoint accepts no other verb.
QByteArray postJson(const QNetworkRequest& request, const QByteArray& body,
                    int timeoutMs, QString* error) {
    if (error != nullptr) error->clear();
    QNetworkAccessManager* manager = mikai::backend::transportManagerForThread();
    QNetworkRequest post = request;
    post.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = manager->post(post, body);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    QByteArray payload;
    if (reply->isRunning()) reply->abort();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    payload = reply->readAll();
    if (error != nullptr) {
        if (reply->error() != QNetworkReply::NoError && status == 0) {
            *error = reply->errorString();
        } else if (status < 200 || status >= 300) {
            *error = QStringLiteral("HTTP %1").arg(status);
        }
    }
    reply->deleteLater();
    return payload;
}

}  // namespace

OpenSubtitlesSearch searchOpenSubtitles(const QString& apiKey, const QString& query,
                                        const QString& languageCode, int timeoutMs) {
    OpenSubtitlesSearch result;
    const QString trimmedKey = apiKey.trimmed();
    if (trimmedKey.isEmpty()) {
        result.error = QStringLiteral("an API key is required");
        return result;
    }
    const QString language = languageCode.trimmed();
    const auto fillError = [&result](const QString& text) { result.error = text; };

    QUrl url(QString::fromLatin1(kBaseUrl) + QStringLiteral("/subtitles"));
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("query"), query.trimmed());
    if (!language.isEmpty()) params.addQueryItem(QStringLiteral("languages"), language);
    params.addQueryItem(QStringLiteral("order_by"), QStringLiteral("download_count"));
    params.addQueryItem(QStringLiteral("order_direction"), QStringLiteral("desc"));
    url.setQuery(params);

    QNetworkRequest request = makeRequest(QStringLiteral("/subtitles"), trimmedKey);
    request.setUrl(url);
    const mikai::backend::HttpFetchResult fetch = mikai::backend::runBlockingFetch(
        mikai::backend::transportManagerForThread(), request,
        mikai::backend::HttpFetchOptions{timeoutMs, false, {}});
    if (fetch.cancelled) {
        fillError(QStringLiteral("cancelled"));
        return result;
    }
    if (fetch.timedOut) {
        fillError(QStringLiteral("the request timed out"));
        return result;
    }
    if (fetch.status < 200 || fetch.status >= 300) {
        fillError(fetch.networkError.isEmpty()
                      ? QStringLiteral("HTTP %1").arg(fetch.status)
                      : fetch.networkError);
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(fetch.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        fillError(QStringLiteral("the reply was not readable"));
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
        postJson(makeRequest(QStringLiteral("/download"), apiKey), body, timeoutMs, &postError);
    if (!postError.isEmpty()) return fail(postError);
    const QJsonDocument document = QJsonDocument::fromJson(reply);
    const QString link = document.object().value(QStringLiteral("link")).toString();
    if (link.isEmpty()) return fail(QStringLiteral("the service offered no file"));

    QNetworkRequest fileRequest{QUrl(link)};
    fileRequest.setRawHeader("User-Agent", QByteArray(kUserAgent));
    fileRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
    const mikai::backend::HttpFetchResult fetch = mikai::backend::runBlockingFetch(
        mikai::backend::transportManagerForThread(), fileRequest,
        mikai::backend::HttpFetchOptions{timeoutMs, false, {}});
    if (fetch.cancelled) return fail(QStringLiteral("cancelled"));
    if (fetch.timedOut) return fail(QStringLiteral("the download timed out"));
    if (fetch.status < 200 || fetch.status >= 300 || fetch.body.isEmpty()) {
        return fail(fetch.networkError.isEmpty() ? QStringLiteral("the download failed")
                                                 : fetch.networkError);
    }
    return fetch.body;
}

}  // namespace shell
}  // namespace mplayer
