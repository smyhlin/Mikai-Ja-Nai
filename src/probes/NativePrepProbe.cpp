#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QStringList>
#include <cstdio>
#include <iterator>

namespace {

int failures = 0;

void check(bool passed, const char* behavior) {
  std::fprintf(stderr, "PREP %s %s\n", passed ? "ok" : "FAIL", behavior);
  if (!passed) ++failures;
}

bool writeFile(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool finish(QProcess& process, int timeout) {
  if (!process.waitForStarted(5000)) {
    std::fprintf(stderr, "PREP process start: %s\n", process.errorString().toUtf8().constData());
    return false;
  }
  if (!process.waitForFinished(timeout)) {
    process.kill();
    process.waitForFinished(5000);
    std::fprintf(stderr, "PREP process exceeded deadline\n");
    return false;
  }
  const QByteArray diagnostics = process.readAllStandardError() + process.readAllStandardOutput();
  std::fwrite(diagnostics.constData(), 1, static_cast<size_t>(diagnostics.size()), stderr);
  return process.exitStatus() == QProcess::NormalExit;
}

QStringList helperArguments(const QString& graph, const QString& outEngine, const QString& uuid, const QString& shapes) {
  QStringList args;
  args << QStringLiteral("--onnx") << graph;
  args << QStringLiteral("--out") << outEngine;
  args << QStringLiteral("--shapes") << shapes;
  if (!uuid.isEmpty()) {
    args << QStringLiteral("--device-uuid") << uuid;
  }
  return args;
}

QJsonObject jsonEvent(const QByteArray& data, const QString& name) {
  const char* begin = data.constData();
  const char* end = begin + data.size();
  const char* lineStart = begin;
  while (lineStart < end) {
    const char* lineEnd = lineStart;
    while (lineEnd < end && *lineEnd != '\n' && *lineEnd != '\r') {
      ++lineEnd;
    }
    const QByteArray line(lineStart, static_cast<int>(lineEnd - lineStart));
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (document.isObject() &&
        document.object().value(QStringLiteral("event")).toString() == name) {
      return document.object();
    }
    if (lineEnd < end) {
      if (*lineEnd == '\r' && lineEnd + 1 < end && *(lineEnd + 1) == '\n') {
        lineStart = lineEnd + 2;
      } else {
        lineStart = lineEnd + 1;
      }
    } else {
      break;
    }
  }
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  const QStringList args = app.arguments();
  if (args.size() != 5) {
    std::fprintf(stderr, "Usage: native_prep_probe <helper.exe> <graph.onnx> <GPU-uuid> <named-input-shapes>\n");
    return 2;
  }
  const QString helper = QFileInfo(args[1]).canonicalFilePath();
  const QString graph = QFileInfo(args[2]).canonicalFilePath();
  const QString uuid = args[3];
  const QString shapes = args[4];
  if (helper.isEmpty() || graph.isEmpty()) {
    std::fprintf(stderr, "RESULT: FAIL helper and graph must exist\n");
    return 2;
  }
  QTemporaryDir scratch(QDir::tempPath() + QStringLiteral("/mikai prep Ж-XXXXXX"));
  if (!scratch.isValid()) {
    std::fprintf(stderr, "RESULT: FAIL scratch creation failed\n");
    return 2;
  }

  const QString standingEngine = scratch.filePath(QStringLiteral("previous.engine"));
  const QByteArray standingBytes("previous artifact must survive a second prepare");
  if (!writeFile(standingEngine, standingBytes)) {
    std::fprintf(stderr, "RESULT: FAIL previous-artifact fixture creation failed\n");
    return 2;
  }
  QProcess overwrite;
  overwrite.start(helper, helperArguments(graph, standingEngine, uuid, shapes));
  const bool overwriteFinished = finish(overwrite, 10000);
  check(overwriteFinished && overwrite.exitCode() != 0 && readFile(standingEngine) == standingBytes,
        "preparing over an existing artifact refuses without changing its bytes");

  const QString invalidGraph = scratch.filePath(QStringLiteral("invalid.onnx"));
  if (!writeFile(invalidGraph, QByteArray("not an ONNX graph"))) {
    std::fprintf(stderr, "RESULT: FAIL corrupt-graph fixture creation failed\n");
    return 2;
  }
  const QString failedEngine = scratch.filePath(QStringLiteral("failed.engine"));
  QProcess failure;
  failure.start(helper, helperArguments(invalidGraph, failedEngine, uuid, shapes));
  const bool failureFinished = finish(failure, 60000);
  check(failureFinished && failure.exitCode() == 5 && !QFileInfo::exists(failedEngine),
        "a real builder failure leaves no usable-looking engine");

  const QString wrongDeviceEngine = scratch.filePath(QStringLiteral("wrong-device.engine"));
  QProcess wrongDevice;
  wrongDevice.start(helper, helperArguments(graph, wrongDeviceEngine,
                                             QStringLiteral("GPU-00000000-0000-0000-0000-000000000000"), shapes));
  const bool deviceRefused = finish(wrongDevice, 10000);
  check(deviceRefused && wrongDevice.exitCode() == 3 && !QFileInfo::exists(wrongDeviceEngine),
        "a missing GPU UUID refuses rather than building on another adapter");

  const QString cancelledEngine = scratch.filePath(QStringLiteral("cancelled.engine"));
  QProcess cancellation;
  cancellation.start(helper, helperArguments(graph, cancelledEngine, uuid, shapes));
  const bool started = cancellation.waitForStarted(5000);
  QElapsedTimer clock;
  clock.start();
  QByteArray accumulated;
  bool sawBuildStarted = false;
  while (started && cancellation.state() != QProcess::NotRunning && clock.elapsed() < 10000) {
    if (cancellation.waitForReadyRead(20)) {
      accumulated += cancellation.readAll();
    }
    if (!sawBuildStarted && !jsonEvent(accumulated, QStringLiteral("build-started")).isEmpty()) {
      sawBuildStarted = true;
      break;
    }
    QCoreApplication::processEvents();
  }
  check(sawBuildStarted, "preparation emits build-started before build proceeds");
  const QJsonObject build = jsonEvent(accumulated, QStringLiteral("build-started"));
  const QJsonValue tf32 = build.value(QStringLiteral("tf32Allowed"));
  const QJsonValue stronglyTyped = build.value(QStringLiteral("stronglyTyped"));
  check(tf32.isBool() && !tf32.toBool() && stronglyTyped.isBool() && stronglyTyped.toBool(),
        "preparation disables TF32 without weakening the graph's strongly typed precision");
  cancellation.kill();
  const bool helperStopped = cancellation.waitForFinished(5000);
  check(helperStopped && !QFileInfo::exists(cancelledEngine),
        "cancelling preparation stops its GPU builder and publishes no engine");
  check(readFile(standingEngine) == standingBytes && readFile(invalidGraph) == QByteArray("not an ONNX graph"),
        "failure and cancellation leave previous artifacts and source bytes unchanged");

  std::fprintf(stderr, "RESULT: %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
