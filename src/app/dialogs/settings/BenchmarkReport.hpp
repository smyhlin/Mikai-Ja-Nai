#pragma once

#include <QDateTime>
#include <QSize>
#include <QString>
#include <QVariantMap>
#include <QVector>

namespace mikai::app {

struct BenchmarkReportResult {
    QString modelId;
    QString modelLabel;
    QString clip;
    QString sourcePath;
    QSize sourceSize;
    QSize targetSize;
    double fps = -1.0;
    QString failureReason;
    QString backend;
    qint64 enginePrepMs = -1;
    QDateTime startedAt;
    QDateTime finishedAt;
};

struct BenchmarkReport {
    QDateTime startedAt;
    QDateTime finishedAt;
    QVariantMap runtimeMetadata;
    QVector<BenchmarkReportResult> results;
};

struct BenchmarkReportWriteResult {
    bool success = false;
    QString path;
    QString error;
};

/// Base directory for automatically saved reports.
QString benchmarkReportBaseDirectory();

/// Atomically writes one uniquely named JSON report beneath applicationDir/bench.
BenchmarkReportWriteResult writeBenchmarkReport(const QString& applicationDir,
                                               const BenchmarkReport& report);

}  // namespace mikai::app
