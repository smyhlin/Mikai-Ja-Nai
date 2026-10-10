// Layer guards, plus the one shared-service behaviour the status line depends on.
//
// The guards are the C++ restatement of two Python tests that no longer have a
// Python runtime to run in: the domain library must not include Qt GUI headers,
// and it must not name the app layer. In C++ the same mistake shows up as a
// link-time dependency on Qt6Widgets, so the checks are made on the sources.

#pragma once

#include <QtTest/QTest>

class TestLayering : public QObject {
  Q_OBJECT

 private slots:
  void theDomainLayerHasSourceFiles();
  void theDomainLayerIncludesNoGuiHeaders();
  void theDomainLayerDoesNotReachIntoTheAppLayer();
  void theDomainLayerDoesNotNameRawNumbersWhereTokensBelong();
  void theStreamErrorsShareOneHierarchy();
};

class TestClipboard : public QObject {
  Q_OBJECT

 private slots:
  void emptyImagesLeaveExistingClipboardContentUnchanged();
  void copiedImagesRetainTheirPixels();
  void copyingReportsWhetherTheBoardTookTheText();
  void copyingTheSameTextTwiceIsStable();
  void copyingTheSameImageReplacesUnrelatedFormats();
  void emptyTextReplacesPreviousText();
};
