#pragma once

#include <QtTest/QTest>

class TestBenchmarkReport : public QObject {
  Q_OBJECT

 private slots:
  void completedRunWritesOneParseableJsonWithTypedResults();
  void secondRunDoesNotOverwriteTheFirstReport();
  void unwritableDirectoryNamesTheCauseAndKeepsNothingBehind();
};
