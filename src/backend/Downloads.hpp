#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QUrl>

#include <functional>

namespace mikai::backend {

QByteArray fetchUrlBytes(const QUrl& url, const QMap<QString, QString>& headers, int timeoutMs);

bool downloadToFile(const QUrl& url, const QString& destination, qint64 expectedBytes,
                    const QString& expectedSha256,
                    const std::function<void(qint64 received, qint64 total)>& progress,
                    const std::function<bool()>& cancelled, QString* reason);

QString ensureSevenZip();

bool extractArchive(const QString& sevenZip, const QString& archive, const QString& destination,
                    QString* reason);

}  // namespace mikai::backend
