#pragma once

#include <QtTest/QTest>

class TestBenchmarkReport : public QObject {
  Q_OBJECT

 private slots:
  void completedRunWritesOneParseableJsonWithTypedResults();
  void secondRunDoesNotOverwriteTheFirstReport();
  void unwritableDirectoryNamesTheCauseAndKeepsNothingBehind();
  void clipResolvesFromInstalledSharePrefix();
  void adjacentClipFolderBeatsTheSharePrefix();
  void missingClipResolvesToNothingWithoutASubstitute();
  void developmentBuildReadsSourceResourcesBenchmarks();
};
