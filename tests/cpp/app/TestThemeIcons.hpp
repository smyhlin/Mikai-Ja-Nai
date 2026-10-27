#pragma once

#include <QtTest/QTest>

class TestThemeIcons : public QObject {
  Q_OBJECT

 private slots:
  void requestedSizeAndDevicePixelRatioAreRendered();
  void defaultGlyphTracksPaletteChanges();
  void customColorAlphaIsPreservedInCacheKey();
};
