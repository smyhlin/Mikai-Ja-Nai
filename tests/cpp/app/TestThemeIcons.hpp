#pragma once

#include <QtTest/QTest>

class TestThemeIcons : public QObject {
    Q_OBJECT

 private slots:
  void requestedSizeAndDevicePixelRatioAreRendered();
  void defaultGlyphTracksPaletteChanges();
  void customColorAlphaIsVisible();
  void quickGlyphUsesThemeTintAndDevicePixelRatio();
  void quickGlyphRequestTracksTintAndDprChanges();
  void glyphBoxIsPhysicalAtEveryScale();
  void trailingPadStaysOutsideTheGlyph();
  void glyphEdgesStayAntialiasedAtFractionalRatio();
  void headerGlyphsShareTheirLeadingInkEdge_data();
  void headerGlyphsShareTheirLeadingInkEdge();
  void noTornColumnsInsideGlyphInk();
  void toggleAndSwatchPaintOnceAtEveryRatio();
  void artworkFrameIsAllocatedAtTheDeviceSize();
  void nativeWindowPaintStaysAffineAtFractionalDpr();
};
