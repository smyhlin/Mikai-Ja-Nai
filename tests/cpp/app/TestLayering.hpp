#pragma once

#include <QtCore/QObject>
#include <QtTest/QtTest>

// Layering invariants for the C++ modules:
// - src/backend/ must stay GUI-free and independent of src/player/ and src/app/
// - src/player/native/ and core transport (MpvApi, MpvHandle, PlaybackDiagnostics)
//   must stay GUI-free and independent of src/app/
// - src/player/ must never depend upward on src/app/
class TestLayering : public QObject {
  Q_OBJECT
 private slots:
  void theDomainLayerHasSourceFiles();
  void theDomainLayerIncludesNoGuiHeaders();
  void theDomainLayerDoesNotReachIntoTheAppLayer();
  void theDomainLayerDoesNotNameRawNumbersWhereTokensBelong();
  void theNativeSupportLayerIncludesNoGuiOrAppHeaders();
  void thePlayerLayerDoesNotReachIntoTheAppLayer();
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
