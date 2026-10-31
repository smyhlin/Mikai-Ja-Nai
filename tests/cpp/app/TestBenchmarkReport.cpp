#include "TestBenchmarkReport.hpp"

#include "dialogs/settings/BenchmarkPlayback.hpp"
#include "dialogs/settings/BenchmarkReport.hpp"

#include <QtCore/QByteArray>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTimeZone>
#include <QtCore/QSize>
#include <QtCore/QTemporaryDir>

namespace {

// A clip stands in for the packaged recording: a short distinct payload makes a
// test prove which copy was opened rather than merely that a path matched.
bool writeClip(const QString& path, const QByteArray& payload) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  return file.write(payload) == payload.size();
}

QByteArray readClip(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return file.readAll();
}

// The Linux install tree is configurable, so the packaged location is derived
// from the same macro the application is compiled with rather than a hardcoded
// data directory.
#ifndef MIKAI_BENCHMARK_INSTALL_RELATIVE
#define MIKAI_BENCHMARK_INSTALL_RELATIVE "../share/mikai/benchmarks"
#endif

QString installedClipPath(const QString& applicationDir, const QString& name) {
  return QDir::cleanPath(applicationDir + QLatin1Char('/') +
                         QString::fromUtf8(MIKAI_BENCHMARK_INSTALL_RELATIVE) +
                         QLatin1Char('/') + name);
}

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

void TestBenchmarkReport::clipResolvesFromInstalledSharePrefix() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  // One level below the temporary root keeps every probed location inside the
  // sandbox.
  const QDir prefix(QDir(directory.path()).absoluteFilePath(QStringLiteral("prefix")));
  const QString applicationDir = prefix.absoluteFilePath(QStringLiteral("bin"));
  QVERIFY(QDir().mkpath(applicationDir));
  // A Linux package installs the binary as <prefix>/bin/mikai.
  const QString packaged =
      installedClipPath(applicationDir, QStringLiteral("1920x1080.mp4"));
  QVERIFY(writeClip(packaged, QByteArrayLiteral("clip")));

  QCOMPARE(mikai::app::locateBenchmarkClip(QStringLiteral("1920x1080.mp4"), applicationDir),
           packaged);
  // An unshipped clip stays missing rather than matching a neighbour by name.
  QVERIFY(mikai::app::locateBenchmarkClip(QStringLiteral("1280x720.mp4"), applicationDir)
              .isEmpty());
}

void TestBenchmarkReport::adjacentClipFolderBeatsTheSharePrefix() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QDir prefix(QDir(directory.path()).absoluteFilePath(QStringLiteral("prefix")));
  const QString applicationDir = prefix.absoluteFilePath(QStringLiteral("bin"));
  QVERIFY(QDir().mkpath(applicationDir));
  // Windows installer and portable payload put clips beside the binary.
  const QString adjacent =
      prefix.absoluteFilePath(QStringLiteral("bin/benchmarks/480x360.mp4"));
  const QString shared = installedClipPath(applicationDir, QStringLiteral("480x360.mp4"));
  QVERIFY(writeClip(adjacent, QByteArrayLiteral("portable-payload")));
  QVERIFY(writeClip(shared, QByteArrayLiteral("installed-prefix")));

  const QString located =
      mikai::app::locateBenchmarkClip(QStringLiteral("480x360.mp4"), applicationDir);
  QCOMPARE(located, adjacent);
  // The adjacent copy is the one opened, not the share copy.
  QCOMPARE(readClip(located), QByteArrayLiteral("portable-payload"));
}

void TestBenchmarkReport::missingClipResolvesToNothingWithoutASubstitute() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QDir prefix(QDir(directory.path()).absoluteFilePath(QStringLiteral("prefix")));
  const QString applicationDir = prefix.absoluteFilePath(QStringLiteral("bin"));
  QVERIFY(QDir().mkpath(applicationDir));
  QVERIFY(writeClip(installedClipPath(applicationDir, QStringLiteral("480x360.mp4")),
                    QByteArrayLiteral("neighbour")));
  QVERIFY(QDir().mkpath(
      installedClipPath(applicationDir, QStringLiteral("1920x1080.mp4"))));

  QVERIFY(mikai::app::locateBenchmarkClip(QStringLiteral("1920x1080.mp4"), applicationDir)
              .isEmpty());
}

void TestBenchmarkReport::developmentBuildReadsSourceResourcesBenchmarks() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QDir root(directory.path());
  const QString sourceClip =
      root.absoluteFilePath(QStringLiteral("resources/benchmarks/768x576.mp4"));
  QVERIFY(writeClip(sourceClip, QByteArrayLiteral("development-source")));

  // Nested build directory, the layout of every configured preset.
  const QString nested = root.absoluteFilePath(QStringLiteral("build/release"));
  QVERIFY(QDir().mkpath(nested));
  QCOMPARE(mikai::app::locateBenchmarkClip(QStringLiteral("768x576.mp4"), nested), sourceClip);

  // A single-level build directory resolves the same source tree.
  const QString flat = root.absoluteFilePath(QStringLiteral("build"));
  QVERIFY(QDir().mkpath(flat));
  QCOMPARE(mikai::app::locateBenchmarkClip(QStringLiteral("768x576.mp4"), flat), sourceClip);
}
