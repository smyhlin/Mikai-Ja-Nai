#include "BenchmarkReport.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace mikai::app {
namespace {

QJsonObject sizeObject(const QSize& size) {
    return {{QStringLiteral("width"), size.width()},
            {QStringLiteral("height"), size.height()}};
}

QString uniquePath(const QString& directory) {
    for (;;) {
        const QString name = QStringLiteral("benchmark-%1-%2.json")
                                 .arg(QDateTime::currentDateTimeUtc().toString(
                                     QStringLiteral("yyyyMMdd'T'HHmmsszzz'Z'")),
                                      QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QString path = QDir(directory).filePath(name);
        if (!QFileInfo::exists(path)) return path;
    }
}

}  // namespace

QString benchmarkReportBaseDirectory() {
#ifdef Q_OS_LINUX
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
#else
    return QCoreApplication::applicationDirPath();
#endif
}

BenchmarkReportWriteResult writeBenchmarkReport(const QString& applicationDir,
                                                const BenchmarkReport& report) {
    const QString directory = QDir(applicationDir).filePath(QStringLiteral("bench"));
    if (!QDir().mkpath(directory)) {
        return {false, {}, QStringLiteral("Could not create report directory '%1'.").arg(directory)};
    }

    const QString path = uniquePath(directory);
    QJsonArray results;
    for (const BenchmarkReportResult& result : report.results) {
        QJsonObject row{{QStringLiteral("modelId"), result.modelId},
                        {QStringLiteral("modelLabel"), result.modelLabel},
                        {QStringLiteral("clip"), result.clip},
                        {QStringLiteral("sourcePath"), result.sourcePath},
                        {QStringLiteral("sourceGeometry"), sizeObject(result.sourceSize)},
                        {QStringLiteral("targetGeometry"), sizeObject(result.targetSize)},
                        {QStringLiteral("backend"), result.backend},
                        {QStringLiteral("enginePreparationMs"), result.enginePrepMs},
                        {QStringLiteral("startedAt"), result.startedAt.toUTC().toString(Qt::ISODateWithMs)},
                        {QStringLiteral("finishedAt"), result.finishedAt.toUTC().toString(Qt::ISODateWithMs)}};
        if (result.fps >= 0.0) row.insert(QStringLiteral("fps"), result.fps);
        else row.insert(QStringLiteral("fps"), QJsonValue(QJsonValue::Null));
        row.insert(QStringLiteral("failureReason"), result.failureReason);
        results.append(row);
    }
    QJsonObject root{{QStringLiteral("schemaVersion"), 1},
                     {QStringLiteral("startedAt"), report.startedAt.toUTC().toString(Qt::ISODateWithMs)},
                     {QStringLiteral("finishedAt"), report.finishedAt.toUTC().toString(Qt::ISODateWithMs)},
                     {QStringLiteral("runtime"), QJsonObject::fromVariantMap(report.runtimeMetadata)},
                     {QStringLiteral("results"), results}};
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {false, path, QStringLiteral("Could not open '%1': %2").arg(path, file.errorString())};
    }
    if (file.write(payload) != payload.size()) {
        const QString error = file.errorString();
        file.cancelWriting();
        return {false, path, QStringLiteral("Could not write '%1' completely: %2").arg(path, error)};
    }
    if (!file.commit()) {
        return {false, path, QStringLiteral("Could not commit '%1': %2").arg(path, file.errorString())};
    }
    return {true, path, {}};
}

}  // namespace mikai::app
