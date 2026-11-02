#pragma once

#include <QtTest/QTest>

class TestThemeIcons : public QObject {
    Q_OBJECT

 private slots:
  void requestedSizeAndDevicePixelRatioAreRendered();
  void defaultGlyphTracksPaletteChanges();
  void customColorAlphaIsPreservedInCacheKey();
  void glyphBoxIsExactAtEveryScale();
  void trailingPadStaysOutsideTheGlyph();
  void glyphEdgesStayAntialiasedAtFractionalRatio();
  void noTornColumnsInsideGlyphInk();
  void chevronKeepsEveryStrokeRowInked();
  void paintPathsDoNotMultiplyTheDevicePixelRatio();
  void toggleAndSwatchPaintOnceAtEveryRatio();
  void shellGlyphJoinsAreRound();
  void shellAtmosBadgeStaysInsideItsBox();
};
