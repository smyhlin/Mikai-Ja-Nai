#pragma once

#include <QtTest/QTest>

class TestThemeIcons : public QObject {
    Q_OBJECT

 private slots:
  void requestedSizeAndDevicePixelRatioAreRendered();
  void defaultGlyphTracksPaletteChanges();
  void customColorAlphaIsPreservedInCacheKey();
  void glyphBoxIsPhysicalAtEveryScale();
  void trailingPadStaysOutsideTheGlyph();
  void glyphEdgesStayAntialiasedAtFractionalRatio();
  void noTornColumnsInsideGlyphInk();
  void chevronKeepsEveryStrokeRowInked();
  void toggleAndSwatchPaintOnceAtEveryRatio();
  void artworkFrameIsAllocatedAtTheDeviceSize();
  void shellGlyphJoinsAreRound();
  void shellAtmosBadgeStaysInsideItsBox();
  void nativeWindowPaintStaysAffineAtFractionalDpr();
};
