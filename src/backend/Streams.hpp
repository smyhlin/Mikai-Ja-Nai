#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

class QNetworkAccessManager;

#include <stdexcept>

// Stream resolution mirrors mikai_backend/streams.py: the embed page is
// fetched, its obfuscated master playlist url is extracted, and the master is
// parsed into per-height variants.
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

// Blocking GET honouring kTimeoutMs and kFetchRetries on 429/5xx. Throws
// StreamError once the retries are exhausted.
QString fetchText(QNetworkAccessManager* manager, const QString& url,
                  const QMap<QString, QString>& headers = {});

QString ashdiMaster(QNetworkAccessManager* manager, const QString& pageUrl);
QString moonMaster(QNetworkAccessManager* manager, const QString& pageUrl);
QString tortugaDecode(const QString& token);
QString tortugaMaster(QNetworkAccessManager* manager, const QString& pageUrl);
QString extractMaster(QNetworkAccessManager* manager, const QString& pageUrl);

QVector<QualityVariant> parseVariants(const QString& masterText, const QString& baseUrl = {});

QString qualityUrl(const QVector<QualityVariant>& variants, int height);

QStringList listSegments(QNetworkAccessManager* manager, const QString& qualityUrl,
                         const QMap<QString, QString>& headers = {});

QMap<int, QString> qualityStreams(const QString& pageUrl);

}  // namespace mikai::backend
