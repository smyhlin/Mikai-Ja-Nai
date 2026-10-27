#include "TestThemeIcons.hpp"

#include "theme/Icons.hpp"

#include <QApplication>
#include <QImage>
#include <QPalette>
#include <QPixmap>

namespace {

struct PaletteRestore {
  QPalette original = QApplication::palette();
  ~PaletteRestore() { QApplication::setPalette(original); }
};

bool containsColor(const QPixmap& pixmap, const QColor& color) {
  const QImage image = pixmap.toImage();
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (image.pixelColor(x, y) == color) return true;
    }
  }
  return false;
}

bool containsColorAtAlpha(const QPixmap& pixmap, const QColor& color) {
  const QImage image = pixmap.toImage();
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor pixel = image.pixelColor(x, y);
      if (pixel.alpha() == color.alpha() &&
          qAbs(pixel.red() - color.red()) <= 2 &&
          qAbs(pixel.green() - color.green()) <= 2 &&
          qAbs(pixel.blue() - color.blue()) <= 2) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

void TestThemeIcons::requestedSizeAndDevicePixelRatioAreRendered() {
  const QIcon icon = mikai::app::theme::chevronLeftIcon(19);
  const QPixmap regular = icon.pixmap(QSize(19, 19), 1.0);
  const QPixmap hidpi = icon.pixmap(QSize(19, 19), 2.0);
  QCOMPARE(regular.size(), QSize(19, 19));
  QCOMPARE(regular.devicePixelRatio(), 1.0);
  QCOMPARE(hidpi.size(), QSize(38, 38));
  QCOMPARE(hidpi.devicePixelRatio(), 2.0);
  const QIcon large = mikai::app::theme::gearIcon(37);
  QCOMPARE(large.actualSize(QSize(37, 37)), QSize(37, 37));
}

void TestThemeIcons::customColorAlphaIsPreservedInCacheKey() {
  const QColor lowAlpha(37, 89, 143, 72);
  const QColor highAlpha(37, 89, 143, 196);
  const QPixmap low = mikai::app::theme::gearIcon(25, lowAlpha.name(QColor::HexArgb))
                          .pixmap(QSize(25, 25));
  const QPixmap high = mikai::app::theme::gearIcon(25, highAlpha.name(QColor::HexArgb))
                           .pixmap(QSize(25, 25));
  QVERIFY(containsColorAtAlpha(low, lowAlpha));
  QVERIFY(containsColorAtAlpha(high, highAlpha));
}

void TestThemeIcons::defaultGlyphTracksPaletteChanges() {
  const PaletteRestore restore;
  QPalette palette = restore.original;
  const QColor first(211, 31, 47);
  const QColor second(29, 67, 223);

  palette.setColor(QPalette::Text, first);
  QApplication::setPalette(palette);
  const QPixmap firstRender = mikai::app::theme::gearIcon(21).pixmap(QSize(21, 21));
  QVERIFY(containsColor(firstRender, first));

  palette.setColor(QPalette::Text, second);
  QApplication::setPalette(palette);
  const QPixmap secondRender = mikai::app::theme::gearIcon(21).pixmap(QSize(21, 21));
  QVERIFY(containsColor(secondRender, second));
  QVERIFY(!containsColor(secondRender, first));
}
