#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

class QNetworkAccessManager;

#include <stdexcept>

// Stream resolution mirrors mikai_backend/streams.py: the embed page is
// fetched and its obfuscated source is extracted. Ashdi and Tortuga publish an
// HLS master playlist, which is parsed into per-height variants; MoonAnime
// publishes a labelled list of direct media URLs instead.
namespace mikai::backend {

inline const QString kUserAgent =
    QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                   "Chrome/142.0.0.0 Safari/537.36");
inline const int kTimeoutMs = 20000;
inline const int kFetchRetries = 2;

class StreamError : public std::runtime_error {
public:
    explicit StreamError(const QString& message)
        : std::runtime_error(message.toStdString()), message(message) {}

    QString message;
};

// Raised when the site resolved fine but the page carries no video file at
// all. The UI shows this differently from a network failure, so it must be
// caught before StreamError.
class NoSourceError : public StreamError {
public:
    explicit NoSourceError(const QString& message) : StreamError(message) {}
};

struct QualityVariant {
    int height = 0;
    QString url;
};

QString detectSite(const QString& pageUrl);  // throws std::invalid_argument

// P0-02: http/https only (host must be non-empty). The one URL-allowlist rule:
// every OS URL dispatch gates on this, including TitlePage::openEmbedInBrowser.
bool isSafeStreamUrl(const QString& rawUrl);

// Blocking GET honouring kTimeoutMs and kFetchRetries. Transport failures -
// timeout, remote close, a refused connection - retry under the same budget
// as the 429/5xx statuses; a cancellation and a definite non-retryable 4xx
// answer are final. Throws StreamError once the retries are exhausted.
QString fetchText(QNetworkAccessManager* manager, const QString& url,
                  const QMap<QString, QString>& headers = {});

QString ashdiMaster(QNetworkAccessManager* manager, const QString& pageUrl);

// The decoded MoonAnime source: "[720p]https://…,[1080p]https://…". A payload
// that carries a single master URL instead (the older shape) yields no entries.
QVector<QualityVariant> parseMoonSourceList(const QString& decoded);
QVector<QualityVariant> moonVariants(QNetworkAccessManager* manager, const QString& pageUrl);

// Maps contain "type", "start" and "end" in seconds; provider order and overlaps are preserved.
QVector<QVariantMap> moonSkipSegments(QNetworkAccessManager* manager, const QString& pageUrl);

QVector<QVariantMap> parseMoonSkipSegments(const QString& html, const QString& pageUrl = {});
QString tortugaDecode(const QString& token);
QString tortugaMaster(QNetworkAccessManager* manager, const QString& pageUrl);

QVector<QualityVariant> parseVariants(const QString& masterText, const QString& baseUrl = {});

QString qualityUrl(const QVector<QualityVariant>& variants, int height);

QStringList listSegments(QNetworkAccessManager* manager, const QString& qualityUrl,
                         const QMap<QString, QString>& headers = {});

QMap<int, QString> qualityStreams(const QString& pageUrl);

}  // namespace mikai::backend
