// The public crash-log contract in src/app/support/CrashLog: install,
// session-log path, and append — through the public header only.
//
// What this suite deliberately does NOT do (read before extending it):
//   * The 24 KiB report block (kReportCapacityBytes, CrashLog.cpp:46) is
//     allocated once by installCrashLog() (CrashLog.cpp:943) and is
//     intentionally never freed — a crash-handler buffer (v1 §1.7). The
//     pointer lives in an anonymous namespace, so no test can observe it
//     directly; the observable proxy is that installCrashLog() is
//     idempotent (see below). This test must NOT free the block.
//   * The X-C6 cases — rotation of a > 2 MiB mikai.log to its sibling
//     mikai.log.1, the crash-filename stamp shape, and zero-padded
//     writeLocalTime/writeUtcTime — belong to task 10 (tasks/10 X-C6 §5:
//     "extend it here, do not create a second suite"). They are not here
//     because the fixes have not landed: rotation is broken in this tree
//     (rotateIfLarge builds the sibling with QDir(path).filePath(), which
//     yields mikai.log/mikai.log.1), the Windows stamp duplicates the "20"
//     prefix, and neither writer zero-pads single-digit fields yet. The
//     writers and formatStampNarrow also live in an anonymous namespace
//     (and are split Win32/POSIX), so there is no test seam for them in
//     this tree without a production change, which this task does not make.

#pragma once

#include <QtTest/QTest>

class TestCrashLog : public QObject {
  Q_OBJECT

private slots:
  void theLogPathIsEmptyUntilInstallRuns();
  void writeLogLineBeforeInstallIsANoOp();
  void installCrashLogSetsTheLogPathAndWritesTheBanner();
  void installCrashLogIsIdempotent();
  void writeLogLineAppendsToTheSessionLog();
};
