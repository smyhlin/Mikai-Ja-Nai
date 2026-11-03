#pragma once

#include <QtTest/QTest>

// Pixels-only contract for the chrome raster cache (T-E7): one device-pixel
// image per adopted control, reused until that control's own invalidation
// route says otherwise. Every assertion reads the image a real ChromeList
// painted, and the control's state is moved without routing an invalidation
// where the point is that the cache must hold - so nothing here watches a call.
class TestChromeCache : public QObject {
    Q_OBJECT

 private slots:
  void aSecondPaintServesTheCachedPixels();
  void aDevicePixelRatioChangeReRendersAtTheNewRatio();
  void aTranslucentControlLeavesNoBlackFringe();
  void anOverBudgetControlLeavesNoBandBehindWhenItMoves();
  void aSameAreaReshapeRepaintsTheNewBox();
  void aMoveReusesThePixelsItAlreadyHolds();
  void anAncestorClipChangeReRendersTheVisibleRect();
  void aContentChangeAfterAMoveIsNotServedStale();
  void anEnabledChangeInvalidatesEvenAfterAMove();
  void adjacentControlsShareASheetWithoutBleeding();
};
