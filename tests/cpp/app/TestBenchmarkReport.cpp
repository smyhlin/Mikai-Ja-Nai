#include "TestBenchmarkReport.hpp"

#include "dialogs/settings/BenchmarkReport.hpp"

#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTimeZone>
#include <QtCore/QSize>
#include <QtCore/QTemporaryDir>

namespace {

mikai::app::BenchmarkReport sampleReport() {
  mikai::app::BenchmarkReport report;
  report.startedAt = QDateTime(QDate(2026, 10, 18), QTime(10, 0, 0), QTimeZone::UTC);
  report.finishedAt = QDateTime(QDate(2026, 10, 18), QTime(10, 0, 40), QTimeZone::UTC);
  report.runtimeMetadata = {
      {QStringLiteral("gpu"), QStringLiteral("Test GPU")},
      {QStringLiteral("runtime"), QStringLiteral("tensorrt")}};

  mikai::app::BenchmarkReportResult measured;
  measured.modelId = QStringLiteral("builtin1001");
  measured.modelLabel = QStringLiteral("Quality");
  measured.clip = QStringLiteral("480x360.mp4");
  measured.sourcePath = QStringLiteral("/media/480x360.mp4");
  measured.sourceSize = {480, 360};
  measured.targetSize = {960, 720};
  measured.fps = 123.5;
  measured.backend = QStringLiteral("tensorrt");
  measured.enginePrepMs = 4210;
  measured.startedAt = report.startedAt;
  measured.finishedAt = report.finishedAt;
  report.results.append(measured);

  mikai::app::BenchmarkReportResult failed;
  failed.modelId = QStringLiteral("spatial:custom:missing");
  failed.modelLabel = QStringLiteral("Custom");
  failed.clip = QStringLiteral("480x360.mp4");
  failed.sourcePath = QStringLiteral("/media/480x360.mp4");
  failed.sourceSize = {480, 360};
  failed.targetSize = {960, 720};
  failed.fps = -1.0;
  failed.failureReason = QStringLiteral("the selected spatial model is not installed");
  failed.backend = QStringLiteral("tensorrt");
  failed.startedAt = report.startedAt;
  failed.finishedAt = report.finishedAt;
  report.results.append(failed);
  return report;
}

}  // namespace

void TestBenchmarkReport::completedRunWritesOneParseableJsonWithTypedResults() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());

  const auto written = mikai::app::writeBenchmarkReport(directory.path(), sampleReport());
  QVERIFY2(written.success, qPrintable(written.error));
  QCOMPARE(QFileInfo(written.path).absolutePath(),
           QDir(directory.path()).absoluteFilePath(QStringLiteral("bench")));

  QFile reportFile(written.path);
  QVERIFY2(reportFile.open(QIODevice::ReadOnly), qPrintable(reportFile.errorString()));
  const QJsonDocument document = QJsonDocument::fromJson(reportFile.readAll());
  QVERIFY(document.isObject());
  const QJsonObject root = document.object();
  QCOMPARE(root.value(QStringLiteral("schemaVersion")).toInt(), 1);
  QCOMPARE(root.value(QStringLiteral("startedAt")).toString(),
           QStringLiteral("2026-10-18T10:00:00.000Z"));
  QCOMPARE(root.value(QStringLiteral("finishedAt")).toString(),
           QStringLiteral("2026-10-18T10:00:40.000Z"));
  QCOMPARE(root.value(QStringLiteral("runtime")).toObject()
               .value(QStringLiteral("gpu")).toString(),
           QStringLiteral("Test GPU"));

  const QJsonArray results = root.value(QStringLiteral("results")).toArray();
  QCOMPARE(results.size(), 2);

  const QJsonObject measured = results.at(0).toObject();
  QCOMPARE(measured.value(QStringLiteral("modelId")).toString(), QStringLiteral("builtin1001"));
  QCOMPARE(measured.value(QStringLiteral("fps")).toDouble(), 123.5);
  QVERIFY(!measured.contains(QStringLiteral("failureReason")) ||
          measured.value(QStringLiteral("failureReason")).toString().isEmpty());
  QCOMPARE(measured.value(QStringLiteral("sourceGeometry")).toObject()
               .value(QStringLiteral("width")).toInt(), 480);
  QCOMPARE(measured.value(QStringLiteral("targetGeometry")).toObject()
               .value(QStringLiteral("height")).toInt(), 720);
  QCOMPARE(measured.value(QStringLiteral("enginePreparationMs")).toInt(), 4210);

  const QJsonObject failed = results.at(1).toObject();
  QVERIFY(failed.value(QStringLiteral("fps")).isNull());
  QCOMPARE(failed.value(QStringLiteral("failureReason")).toString(),
           QStringLiteral("the selected spatial model is not installed"));
}

void TestBenchmarkReport::secondRunDoesNotOverwriteTheFirstReport() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());

  const auto first = mikai::app::writeBenchmarkReport(directory.path(), sampleReport());
  QVERIFY2(first.success, qPrintable(first.error));
  const auto second = mikai::app::writeBenchmarkReport(directory.path(), sampleReport());
  QVERIFY2(second.success, qPrintable(second.error));
  QVERIFY(first.path != second.path);
  QVERIFY(QFileInfo(first.path).isFile());
  QVERIFY(QFileInfo(second.path).isFile());
}

void TestBenchmarkReport::unwritableDirectoryNamesTheCauseAndKeepsNothingBehind() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  QFile blocked(QDir(directory.path()).filePath(QStringLiteral("bench")));
  QVERIFY(blocked.open(QIODevice::WriteOnly));
  blocked.close();
  const auto written = mikai::app::writeBenchmarkReport(directory.path(), sampleReport());
  QVERIFY(!written.success);
  QVERIFY(!written.error.isEmpty());
  QVERIFY(written.path.isEmpty());
}
