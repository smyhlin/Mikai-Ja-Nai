#include "Streams.hpp"

#include "Cancellation.hpp"

#include <QByteArray>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <cmath>

namespace mikai::backend {
namespace {

QMap<QString, QString> siteHeaders(const QString& accept) {
    QMap<QString, QString> headers;
    headers.insert(QStringLiteral("User-Agent"), kUserAgent);
    headers.insert(QStringLiteral("Accept-Language"), QStringLiteral("uk-UA,uk;q=0.9,en;q=0.8"));
    if (!accept.isEmpty()) {
        headers.insert(QStringLiteral("Accept"), accept);
    }
    return headers;
}

QMap<QString, QString> pageHeaderSet() {
    return siteHeaders(
        QStringLiteral("text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"));
}

QMap<QString, QString> mediaHeaderSet() { return siteHeaders(QStringLiteral("*/*")); }

QMap<QString, QString> bareHeaderSet() {
    QMap<QString, QString> headers;
    headers.insert(QStringLiteral("User-Agent"), kUserAgent);
    return headers;
}

bool retryableStatus(int status) {
    return status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

// Retry-After is advisory and sometimes nonsense, so clamp it the way the
// Python client does; fall back to a growing linear backoff.
double retryDelaySeconds(const QMap<QString, QString>& headers, int attempt) {
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (it.key().compare(QLatin1String("Retry-After"), Qt::CaseInsensitive) == 0 &&
            !it.value().isEmpty()) {
            bool ok = false;
            const double seconds = it.value().toDouble(&ok);
            if (ok && std::isfinite(seconds)) {
                return std::min(2.0, std::max(0.25, seconds));
            }
        }
    }
    return 0.4 * (attempt + 1);
}

struct HttpResult {
    int status = 0;
    QByteArray body;
    QMap<QString, QString> headers;
    QString error;
};

// The one blocking primitive every other fetch builds on: run a request to
// completion on a private event loop so callers stay synchronous like the
// Python implementation.
HttpResult runBlocking(QNetworkAccessManager* manager, const QNetworkRequest& request) {
    HttpResult result;
    QNetworkReply* reply = manager->get(request);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QTimer cancelTimer;
    cancelTimer.setInterval(200);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&cancelTimer, &QTimer::timeout, &loop, [&]() {
        if (WorkCancellationScope::cancellationRequested()) {
            loop.quit();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(kTimeoutMs);
    cancelTimer.start();
    loop.exec();
    cancelTimer.stop();
    if (reply->isRunning()) {
        // The deadline fired first: abort so the reply cannot outlive us. The
        // drain is on the same timer rather than an unbounded wait, because an
        // abort can complete late and this thread belongs to a pool.
        reply->abort();
        timer.start(kTimeoutMs);
        loop.exec();
    }
    timer.stop();

    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto pairs = reply->rawHeaderPairs();
    for (const auto& pair : pairs) {
        result.headers.insert(QString::fromUtf8(pair.first), QString::fromUtf8(pair.second));
    }
    if (reply->error() != QNetworkReply::NoError) {
        result.error = reply->errorString();
    }
    result.body = reply->readAll();
    reply->deleteLater();
    return result;
}

}  // namespace

QString detectSite(const QString& pageUrl) {
    if (pageUrl.contains(QStringLiteral("ashdi.vip/vod/"))) {
        return QStringLiteral("ashdi");
    }
    if (pageUrl.contains(QStringLiteral("moonanime.art/iframe/"))) {
        return QStringLiteral("moon");
    }
    if (pageUrl.contains(QStringLiteral("tortuga.tw/vod/"))) {
        return QStringLiteral("tortuga");
    }
    throw std::invalid_argument("unsupported episode url: " + pageUrl.toStdString());
}

QString fetchText(QNetworkAccessManager* manager, const QString& url,
                  const QMap<QString, QString>& headers) {
    for (int attempt = 0; attempt <= kFetchRetries; ++attempt) {
        QNetworkRequest request{QUrl(url)};
        request.setRawHeader("User-Agent", kUserAgent.toUtf8());
        for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
            request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
        }

        const HttpResult result = runBlocking(manager, request);
        if (result.error.isEmpty()) {
            if (retryableStatus(result.status) && attempt < kFetchRetries) {
                QThread::msleep(
                    static_cast<unsigned long>(retryDelaySeconds(result.headers, attempt) * 1000.0));
                continue;
            }
            if (result.status >= 400) {
                throw StreamError(
                    QStringLiteral("GET failed for %1: HTTP %2").arg(url).arg(result.status));
            }
            return QString::fromUtf8(result.body);
        }
        throw StreamError(QStringLiteral("GET failed for %1: %2").arg(url, result.error));
    }
    throw StreamError(QStringLiteral("GET failed for %1: exhausted retries").arg(url));
}

QString ashdiMaster(QNetworkAccessManager* manager, const QString& pageUrl) {
    const QString html = fetchText(manager, pageUrl, bareHeaderSet());
    static const QRegularExpression pattern(
        QStringLiteral("file\\s*:\\s*['\"](https?://[^'\"]+?\\.m3u8[^'\"]*)['\"]"));
    const QRegularExpressionMatch match = pattern.match(html);
    if (!match.hasMatch()) {
        throw StreamError(QStringLiteral("ashdi master playlist not found in %1").arg(pageUrl));
    }
    return match.captured(1);
}

QString moonMaster(QNetworkAccessManager* manager, const QString& pageUrl) {
    const QString html = fetchText(manager, pageUrl, pageHeaderSet());
    static const QRegularExpression blobPattern(QStringLiteral("atob\\(\"([A-Za-z0-9+/=]{100,})\"\\)"));
    const QRegularExpressionMatch blobMatch = blobPattern.match(html);
    if (!blobMatch.hasMatch()) {
        throw StreamError(QStringLiteral("moonanime payload not found in %1").arg(pageUrl));
    }

    const QByteArray cipher = QByteArray::fromBase64(blobMatch.captured(1).toLatin1());
    if (cipher.size() < 34) {
        throw StreamError(QStringLiteral("moonanime payload too short in %1").arg(pageUrl));
    }
    const char* key = cipher.constData() + 1;
    const char* enc = cipher.constData() + 33;
    const int length = cipher.size() - 33;
    QByteArray out(length, Qt::Uninitialized);
    unsigned char k = static_cast<unsigned char>(cipher.at(0));
    for (int i = 0; i < length; ++i) {
        const unsigned char kb = static_cast<unsigned char>(key[i % 32]);
        const unsigned char e = static_cast<unsigned char>(enc[i]);
        out[i] = static_cast<char>(e ^ kb ^ k);
        k = static_cast<unsigned char>((e + kb) & 255);
    }
    const QString js = QString::fromUtf8(out);

    static const QRegularExpression emptySource(
        QStringLiteral("var rawVideo\\s*=\\s*[\"']\\s*[\"']|file:\\s*[\"']\\s*[\"']"));
    if (emptySource.match(js).hasMatch()) {
        throw NoSourceError(
            QStringLiteral("moonanime has no video source for %1 (no_source)").arg(pageUrl));
    }

    static const QRegularExpression xkPattern(
        QStringLiteral("function _0xd\\(e\\)\\{var k=\"([^\"]+)\""));
    static const QRegularExpression rawPattern(
        QStringLiteral("var rawVideo = _0xd\\(\"([^\"]+)\"\\)"));
    const QRegularExpressionMatch xkMatch = xkPattern.match(js);
    const QRegularExpressionMatch rawMatch = rawPattern.match(js);
    if (!xkMatch.hasMatch() || !rawMatch.hasMatch()) {
        throw StreamError(QStringLiteral("moonanime keys not found in %1").arg(pageUrl));
    }

    const QByteArray xk = xkMatch.captured(1).toUtf8();
    const QByteArray raw = QByteArray::fromBase64(rawMatch.captured(1).toLatin1());
    if (xk.isEmpty()) {
        throw StreamError(QStringLiteral("moonanime keys not found in %1").arg(pageUrl));
    }
    QByteArray decoded(raw.size(), Qt::Uninitialized);
    for (int i = 0; i < raw.size(); ++i) {
        decoded[i] = static_cast<char>(static_cast<unsigned char>(raw.at(i)) ^
                                       static_cast<unsigned char>(xk.at(i % xk.size())));
    }
    const QString text = QString::fromUtf8(decoded);

    static const QRegularExpression pairPattern(
        QStringLiteral("\\[([^\\]]+)\\]((?:https?://)[^,\\[\\s]+)"));
    QRegularExpressionMatchIterator pairs = pairPattern.globalMatch(text);
    if (pairs.hasNext()) {
        return pairs.next().captured(2);
    }
    QString master = text.trimmed();
    while (master.endsWith(QLatin1Char('"')) || master.endsWith(QLatin1Char(' '))) {
        master.chop(1);
    }
    if (master.startsWith(QLatin1Char('"'))) {
        master.remove(0, 1);
    }
    if (!master.startsWith(QStringLiteral("http"))) {
        throw StreamError(QStringLiteral("moonanime master not found in %1").arg(pageUrl));
    }
    return master;
}

QString tortugaDecode(const QString& token) {
    const QByteArray cipher = QByteArray::fromBase64(token.toLatin1());
    if (cipher.isEmpty()) {
        throw StreamError(QStringLiteral("tortuga token decode failed: empty payload"));
    }
    const unsigned char seed = static_cast<unsigned char>(cipher.at(0));
    QByteArray out(cipher.size() - 1, Qt::Uninitialized);
    for (int i = 0; i + 1 < cipher.size(); ++i) {
        const unsigned char mask = static_cast<unsigned char>((seed + 13 + 7 * i) % 256);
        out[i] = static_cast<char>(static_cast<unsigned char>(cipher.at(i + 1)) ^ mask);
    }
    return QString::fromUtf8(out);
}

QString tortugaMaster(QNetworkAccessManager* manager, const QString& pageUrl) {
    const QString html = fetchText(manager, pageUrl, bareHeaderSet());
    static const QRegularExpression pattern(QStringLiteral("file:\\s*\"([^\"]+)\""));
    const QRegularExpressionMatch match = pattern.match(html);
    if (!match.hasMatch()) {
        throw StreamError(QStringLiteral("tortuga file token not found in %1").arg(pageUrl));
    }
    try {
        return tortugaDecode(match.captured(1));
    } catch (const std::exception& exc) {
        throw StreamError(QStringLiteral("tortuga token decode failed for %1: %2")
                              .arg(pageUrl, QString::fromUtf8(exc.what())));
    }
}

QString extractMaster(QNetworkAccessManager* manager, const QString& pageUrl) {
    const QString site = detectSite(pageUrl);
    if (site == QLatin1String("ashdi")) {
        return ashdiMaster(manager, pageUrl);
    }
    if (site == QLatin1String("tortuga")) {
        return tortugaMaster(manager, pageUrl);
    }
    return moonMaster(manager, pageUrl);
}

QVector<QualityVariant> parseVariants(const QString& masterText, const QString& baseUrl) {
    QVector<QualityVariant> variants;
    bool pending = false;
    QualityVariant current;
    const QUrl base(baseUrl);

    const QStringList lines = masterText.split(QLatin1Char('\n'));
    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(QStringLiteral("#EXT-X-STREAM-INF"))) {
            static const QRegularExpression resolution(QStringLiteral("RESOLUTION=(\\d+)x(\\d+)"));
            const QRegularExpressionMatch match = resolution.match(line);
            current = QualityVariant{};
            if (match.hasMatch()) {
                current.height = match.captured(2).toInt();
            }
            pending = true;
        } else if (pending && !line.startsWith(QLatin1Char('#'))) {
            if (line.startsWith(QStringLiteral("http"))) {
                current.url = line;
            } else if (base.isValid() && !base.isEmpty()) {
                current.url = base.resolved(QUrl(line)).toString();
            } else {
                current.url = line;
            }
            variants.append(current);
            pending = false;
        }
    }
    return variants;
}

QString qualityUrl(const QVector<QualityVariant>& variants, int height) {
    if (variants.isEmpty()) {
        throw StreamError(QStringLiteral("master playlist has no variants"));
    }
    for (const QualityVariant& variant : variants) {
        if (variant.height == height) {
            return variant.url;
        }
    }
    const QualityVariant* bestLower = nullptr;
    for (const QualityVariant& variant : variants) {
        if (variant.height > 0 && variant.height < height &&
            (bestLower == nullptr || variant.height > bestLower->height)) {
            bestLower = &variant;
        }
    }
    if (bestLower != nullptr) {
        return bestLower->url;
    }
    const QualityVariant* bestKnown = nullptr;
    for (const QualityVariant& variant : variants) {
        if (variant.height > 0 && (bestKnown == nullptr || variant.height > bestKnown->height)) {
            bestKnown = &variant;
        }
    }
    if (bestKnown != nullptr) {
        return bestKnown->url;
    }
    return variants.first().url;
}

QStringList listSegments(QNetworkAccessManager* manager, const QString& qualityUrl,
                         const QMap<QString, QString>& headers) {
    const QUrl base(qualityUrl);
    QStringList urls;
    const QStringList lines = fetchText(manager, qualityUrl, headers).split(QLatin1Char('\n'));
    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        urls.append(line.startsWith(QStringLiteral("http"))
                        ? line
                        : base.resolved(QUrl(line)).toString());
    }
    return urls;
}

QMap<int, QString> qualityStreams(const QString& pageUrl) {
    const QString site = detectSite(pageUrl);
    // One manager per worker thread rather than one per call: a fresh manager
    // means a fresh cookie jar and a fresh TLS handshake, so a local one paid
    // for the provider handshake again on every episode click. Tied to the
    // thread's lifetime so an expiring pool thread leaves nothing behind.
    static thread_local QNetworkAccessManager* manager = [] {
        auto* created = new QNetworkAccessManager;
        QObject::connect(QThread::currentThread(), &QThread::finished, created,
                         &QObject::deleteLater);
        return created;
    }();
    const QString master = extractMaster(manager, pageUrl);
    const QMap<QString, QString> headers =
        (site == QLatin1String("ashdi") || site == QLatin1String("tortuga")) ? bareHeaderSet()
                                                                             : mediaHeaderSet();
    const QVector<QualityVariant> variants =
        parseVariants(fetchText(manager, master, headers), master);
    auto isSafeStreamUrl = [](const QString& rawUrl) -> bool {
        const QUrl parsed(rawUrl, QUrl::StrictMode);
        if (!parsed.isValid() || parsed.host().isEmpty()) {
            return false;
        }
        const QString scheme = parsed.scheme().toLower();
        return scheme == QLatin1String("https") || scheme == QLatin1String("http");
    };
    QMap<int, QString> streams;
    for (const QualityVariant& variant : variants) {
        if (variant.height > 0 && !variant.url.isEmpty() && isSafeStreamUrl(variant.url)) {
            streams.insert(variant.height, variant.url);
        }
    }
    return streams;
}

}  // namespace mikai::backend
