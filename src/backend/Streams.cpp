#include "Streams.hpp"

#include "Diagnostics.hpp"
#include "HttpTransport.hpp"
#include "Cancellation.hpp"

#include <QByteArray>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QWaitCondition>
#include <QUrl>

#include <algorithm>
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

// The bootstrap uses a rolling XOR seeded by byte zero and a 32-byte key.
QString decodeMoonPayload(const QString& html, const QString& pageUrl) {
    static const QRegularExpression blobPattern(QStringLiteral("atob\\(\"([A-Za-z0-9+/=]{100,})\"\\)"));
    const QRegularExpressionMatch blobMatch = blobPattern.match(html);
    if (!blobMatch.hasMatch()) {
        // P1-05: every URL in an exception is wrapped with redactDiagnostic()
        // at construction, so a signed URL (token in path and query) can never
        // reach logs or the UI through the exception chain.
        throw StreamError(redactDiagnostic(
            QStringLiteral("moonanime payload not found in %1").arg(pageUrl)));
    }

    const QByteArray cipher = QByteArray::fromBase64(blobMatch.captured(1).toLatin1());
    if (cipher.size() < 34) {
        throw StreamError(redactDiagnostic(
            QStringLiteral("moonanime payload too short in %1").arg(pageUrl)));
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
    return QString::fromUtf8(out);
}

bool knownMoonSegmentType(const QString& type) {
    return type == QLatin1String("opening") || type == QLatin1String("ending") ||
           type == QLatin1String("recap") || type == QLatin1String("mixed_opening") ||
           type == QLatin1String("mixed_ending");
}

}  // namespace

QString detectSite(const QString& pageUrl) {
    const QUrl url(pageUrl);
    const QString host = url.host().toLower();
    const QString path = url.path().toLower();
    const auto hostIsOrSubdomainOf = [&host](const QString& domain) {
        return host == domain || host.endsWith(QLatin1Char('.') + domain);
    };
    if (url.isValid() && !url.host().isEmpty() &&
        (url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0 ||
         url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0)) {
        if (hostIsOrSubdomainOf(QStringLiteral("ashdi.vip")) &&
            path.startsWith(QStringLiteral("/vod/")) && path.size() > 5) {
            return QStringLiteral("ashdi");
        }
        if (hostIsOrSubdomainOf(QStringLiteral("moonanime.art")) &&
            path.startsWith(QStringLiteral("/iframe/")) && path.size() > 8) {
            return QStringLiteral("moon");
        }
        if (hostIsOrSubdomainOf(QStringLiteral("tortuga.tw")) &&
            path.startsWith(QStringLiteral("/vod/")) && path.size() > 5) {
            return QStringLiteral("tortuga");
        }
        if (path.endsWith(QStringLiteral(".m3u8")) || path.endsWith(QStringLiteral(".mp4"))) {
            return QStringLiteral("direct");
        }
    }
    throw std::invalid_argument(
        redactDiagnostic(QStringLiteral("unsupported episode url: ") + pageUrl).toStdString());
}

QString fetchText(QNetworkAccessManager* manager, const QString& url,
                  const QMap<QString, QString>& headers) {
    for (int attempt = 0; attempt <= kFetchRetries; ++attempt) {
        QNetworkRequest request{QUrl(url)};
        request.setRawHeader("User-Agent", kUserAgent.toUtf8());
        for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
            request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
        }

        // T-403: the shared transport loop carries this scraper's timeout and
        // collects the raw headers the backoff below consults.
        const HttpFetchResult result =
            runBlockingFetch(manager, request, HttpFetchOptions{kTimeoutMs, true, {}});
        // T-305: transport failures - a timeout, a remote close, a refused
        // connection - retry with the same clamp-and-backoff the 429/5xx
        // path uses. Qt reports a definite HTTP error answer as a network
        // error too, so a bare error string is not enough to classify: a
        // status this scraper treats as final (403, 404, ...) fails at once
        // instead of burning the budget on an answer that cannot change.
        // Only a cancellation is final for the transport classes, because the
        // caller asked to stop. The budget is the loop counter itself, shared
        // with the HTTP status retries, so the total attempt count stays
        // bounded.
        const bool transportFailure =
            (result.timedOut || !result.networkError.isEmpty()) && result.status < 400;
        const bool retryable =
            !result.cancelled && (transportFailure || retryableStatus(result.status));
        if (result.cancelled) {
            throw StreamError(redactDiagnostic(
                QStringLiteral("GET failed for %1: the request was cancelled").arg(url)));
        }
        if (retryable) {
            if (attempt < kFetchRetries) {
                int remainingMs =
                    static_cast<int>(retryDelaySeconds(result.headers, attempt) * 1000.0);
                // P1-05 (rung 3): an interruptible QWaitCondition wait, not a
                // blocking sleep, on the 2-thread stream pool. The same
                // retryDelaySeconds() backoff (0.25-2.0 s) in 200 ms slices,
                // so cancellation is still observed at most a slice late.
                // No waker is wired to this wait yet: it times out per slice
                // and the cancellation check above runs each time round.
                QMutex backoffMutex;
                QWaitCondition backoffWait;
                QMutexLocker backoffLocker(&backoffMutex);
                while (remainingMs > 0) {
                    if (WorkCancellationScope::cancellationRequested()) {
                        throw StreamError(redactDiagnostic(
                            QStringLiteral("GET failed for %1: the request was cancelled")
                                .arg(url)));
                    }
                    const int sleepMs = std::min(200, remainingMs);
                    backoffWait.wait(&backoffMutex, sleepMs);
                    remainingMs -= sleepMs;
                }
                if (WorkCancellationScope::cancellationRequested()) {
                    throw StreamError(redactDiagnostic(
                        QStringLiteral("GET failed for %1: the request was cancelled")
                            .arg(url)));
                }
                continue;
            }
            // T-305: reachable now that transport classes joined the retry
            // budget - the last attempt failed in a retryable way and there
            // are no attempts left to spend on it, so name what it was.
            const QString detail =
                result.timedOut
                    ? QStringLiteral("timed out after %1 ms").arg(kTimeoutMs)
                    : result.status >= 400
                          ? QStringLiteral("HTTP %1").arg(result.status)
                          : result.networkError;
            throw StreamError(redactDiagnostic(
                QStringLiteral("GET failed for %1: exhausted retries (%2)").arg(url, detail)));
        }
        if (result.status >= 400) {
            throw StreamError(redactDiagnostic(
                QStringLiteral("GET failed for %1: HTTP %2").arg(url).arg(result.status)));
        }
        return QString::fromUtf8(result.body);
    }
    throw StreamError(redactDiagnostic(
        QStringLiteral("GET failed for %1: exhausted retries").arg(url)));
}

QString ashdiMaster(QNetworkAccessManager* manager, const QString& pageUrl) {
    const QString html = fetchText(manager, pageUrl, bareHeaderSet());
    static const QRegularExpression pattern(
        QStringLiteral("file\\s*:\\s*['\"](https?://[^'\"]+?\\.m3u8[^'\"]*)['\"]"));
    const QRegularExpressionMatch match = pattern.match(html);
    if (!match.hasMatch()) {
        throw StreamError(redactDiagnostic(
            QStringLiteral("ashdi master playlist not found in %1").arg(pageUrl)));
    }
    return match.captured(1);
}

namespace {
QString moonSourceText(QNetworkAccessManager* manager, const QString& pageUrl) {
    const QString html = fetchText(manager, pageUrl, pageHeaderSet());
    const QString js = decodeMoonPayload(html, pageUrl);

    static const QRegularExpression emptySource(
        QStringLiteral("var rawVideo\\s*=\\s*[\"']\\s*[\"']|file:\\s*[\"']\\s*[\"']"));
    if (emptySource.match(js).hasMatch()) {
        throw NoSourceError(redactDiagnostic(
            QStringLiteral("moonanime has no video source for %1 (no_source)").arg(pageUrl)));
    }

    static const QRegularExpression xkPattern(
        QStringLiteral("function _0xd\\(e\\)\\{var k=\"([^\"]+)\""));
    static const QRegularExpression rawPattern(
        QStringLiteral("var rawVideo = _0xd\\(\"([^\"]+)\"\\)"));
    const QRegularExpressionMatch xkMatch = xkPattern.match(js);
    const QRegularExpressionMatch rawMatch = rawPattern.match(js);
    if (!xkMatch.hasMatch() || !rawMatch.hasMatch()) {
        throw StreamError(redactDiagnostic(
            QStringLiteral("moonanime keys not found in %1").arg(pageUrl)));
    }

    const QByteArray xk = xkMatch.captured(1).toUtf8();
    const QByteArray raw = QByteArray::fromBase64(rawMatch.captured(1).toLatin1());
    if (xk.isEmpty()) {
        throw StreamError(redactDiagnostic(
            QStringLiteral("moonanime keys not found in %1").arg(pageUrl)));
    }
    QByteArray decoded(raw.size(), Qt::Uninitialized);
    for (int i = 0; i < raw.size(); ++i) {
        decoded[i] = static_cast<char>(static_cast<unsigned char>(raw.at(i)) ^
                                       static_cast<unsigned char>(xk.at(i % xk.size())));
    }
    return QString::fromUtf8(decoded);
}

QString legacyMoonMaster(const QString& text, const QString& pageUrl) {
    QString master = text.trimmed();
    while (master.endsWith(QLatin1Char('"')) || master.endsWith(QLatin1Char(' '))) {
        master.chop(1);
    }
    if (master.startsWith(QLatin1Char('"'))) {
        master.remove(0, 1);
    }
    if (!master.startsWith(QStringLiteral("http"))) {
        throw StreamError(redactDiagnostic(
            QStringLiteral("moonanime master not found in %1").arg(pageUrl)));
    }
    return master;
}
}  // namespace

QVector<QualityVariant> parseMoonSourceList(const QString& decoded) {
    static const QRegularExpression labelled(
        QStringLiteral("\\[(\\d{2,4})p[^\\]]*\\](https?://[^,\\[\\s]+)"));
    QVector<QualityVariant> variants;
    QRegularExpressionMatchIterator matches = labelled.globalMatch(decoded);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const int height = match.captured(1).toInt();
        const QString url = match.captured(2);
        bool known = false;
        for (const QualityVariant& variant : variants) {
            if (variant.height == height) {
                known = true;
                break;
            }
        }
        if (height > 0 && !known) {
            variants.append(QualityVariant{height, url});
        }
    }
    return variants;
}

QVector<QualityVariant> moonVariants(QNetworkAccessManager* manager, const QString& pageUrl) {
    const QString text = moonSourceText(manager, pageUrl);
    const QVector<QualityVariant> variants = parseMoonSourceList(text);
    if (!variants.isEmpty()) {
        return variants;
    }
    // Older payloads carried one HLS master URL where the labelled list is now.
    return {QualityVariant{0, legacyMoonMaster(text, pageUrl)}};
}

QVector<QVariantMap> parseMoonSkipSegments(const QString& html, const QString& pageUrl) {
    QString js;
    try {
        js = decodeMoonPayload(html, pageUrl);
    } catch (const StreamError& failure) {
        qWarning().noquote() << "mikai: moonanime skip metadata unavailable:" << failure.message;
        return {};
    }

    if (!js.contains(QStringLiteral("__skipSegments"))) {
        return {};
    }

    static const QRegularExpression skipPattern(
        QStringLiteral("__skipSegments[\\s\\S]{0,512}?JSON\\.parse\\(\\s*_0xd\\(\\s*[\"']([^\"']*)[\"']\\s*\\)\\s*\\)"));
    const QRegularExpressionMatch skipMatch = skipPattern.match(js);
    if (!skipMatch.hasMatch()) {
        qWarning().noquote()
            << "mikai: moonanime skip metadata pattern not found in" << pageUrl;
        return {};
    }

    static const QRegularExpression keyPattern(
        QStringLiteral("function\\s+_0xd\\s*\\(\\s*e\\s*\\)\\s*\\{\\s*var\\s+k\\s*=\\s*[\"']([^\"']+)[\"']"));
    const QRegularExpressionMatch keyMatch = keyPattern.match(js);
    if (!keyMatch.hasMatch() || keyMatch.captured(1).isEmpty()) {
        qWarning().noquote()
            << "mikai: moonanime skip metadata key not found in" << pageUrl;
        return {};
    }

    const QByteArray cipher = QByteArray::fromBase64(skipMatch.captured(1).toLatin1());
    const QByteArray key = keyMatch.captured(1).toUtf8();
    QByteArray decoded(cipher.size(), Qt::Uninitialized);
    for (int i = 0; i < cipher.size(); ++i) {
        decoded[i] = static_cast<char>(static_cast<unsigned char>(cipher.at(i)) ^
                                       static_cast<unsigned char>(key.at(i % key.size())));
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(decoded, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qWarning().noquote() << "mikai: moonanime skip metadata is not valid JSON in" << pageUrl
                             << "at byte" << parseError.offset << "-" << parseError.errorString();
        return {};
    }
    if (!document.isArray()) {
        qWarning().noquote()
            << "mikai: moonanime skip metadata is not a JSON array in" << pageUrl;
        return {};
    }

    QVector<QVariantMap> segments;
    int rejected = 0;
    const QJsonArray items = document.array();
    for (const QJsonValue& value : items) {
        if (!value.isObject()) {
            ++rejected;
            continue;
        }
        const QJsonObject item = value.toObject();
        const QString type = item.value(QStringLiteral("t")).toString();
        const QJsonValue startValue = item.value(QStringLiteral("s"));
        const QJsonValue endValue = item.value(QStringLiteral("e"));
        const double start = startValue.toDouble();
        const double end = endValue.toDouble();
        if (!knownMoonSegmentType(type) || !startValue.isDouble() || !endValue.isDouble() ||
            !std::isfinite(start) || !std::isfinite(end) || start < 0.0 || start >= end) {
            ++rejected;
            continue;
        }
        QVariantMap segment;
        segment.insert(QStringLiteral("type"), type);
        segment.insert(QStringLiteral("start"), start);
        segment.insert(QStringLiteral("end"), end);
        segments.append(segment);
    }
    if (rejected > 0) {
        qWarning().noquote() << "mikai: moonanime skip metadata ignored" << rejected
                             << "invalid segment(s) in" << pageUrl;
    }
    return segments;
}

QVector<QVariantMap> moonSkipSegments(QNetworkAccessManager* manager, const QString& pageUrl) {
    const QString html = fetchText(manager, pageUrl, pageHeaderSet());
    return parseMoonSkipSegments(html, pageUrl);
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
        throw StreamError(redactDiagnostic(
            QStringLiteral("tortuga file token not found in %1").arg(pageUrl)));
    }
    try {
        return tortugaDecode(match.captured(1));
    } catch (const std::exception& exc) {
        throw StreamError(redactDiagnostic(
            QStringLiteral("tortuga token decode failed for %1: %2")
                .arg(pageUrl, QString::fromUtf8(exc.what()))));
    }
}

// P0-02: http/https only, shared so callers outside this file (TitlePage)
// gate OS URL dispatch on the same rule. Exact semantics preserved from the
// anonymous-namespace version.
bool isSafeStreamUrl(const QString& rawUrl) {
    const QUrl parsed(rawUrl, QUrl::StrictMode);
    if (!parsed.isValid() || parsed.host().isEmpty()) {
        return false;
    }
    const QString scheme = parsed.scheme().toLower();
    return scheme == QLatin1String("https") || scheme == QLatin1String("http");
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

QMap<int, QString> qualityStreams(const QString& pageUrl) {
    const QString site = detectSite(pageUrl);
    if (site == QLatin1String("direct")) {
        return {{0, pageUrl}};
    }
    // T-403: the shared per-thread manager. One manager per worker thread
    // rather than one per call: a fresh manager means a fresh cookie jar and a
    // fresh TLS handshake, so a local one paid for the provider handshake
    // again on every episode click. Tied to the thread's lifetime so an
    // expiring pool thread leaves nothing behind.
    QNetworkAccessManager* manager = transportManagerForThread();
    QVector<QualityVariant> variants;
    if (site == QLatin1String("moon")) {
        variants = moonVariants(manager, pageUrl);
        // An unlabelled entry is the older single master playlist, which still
        // has to be read before its variants are known.
        if (variants.size() == 1 && variants.first().height == 0) {
            const QString master = variants.first().url;
            variants = parseVariants(fetchText(manager, master, mediaHeaderSet()), master);
        }
    } else {
        const QString master =
            site == QLatin1String("ashdi") ? ashdiMaster(manager, pageUrl)
                                           : tortugaMaster(manager, pageUrl);
        variants = parseVariants(fetchText(manager, master, bareHeaderSet()), master);
    }
    QMap<int, QString> streams;
    for (const QualityVariant& variant : variants) {
        if (!variant.url.isEmpty() && isSafeStreamUrl(variant.url)) {
            streams.insert(variant.height, variant.url);
        }
    }
    if (streams.isEmpty() && isSafeStreamUrl(pageUrl)) {
        streams.insert(0, pageUrl);
    }
    return streams;
}

}  // namespace mikai::backend
