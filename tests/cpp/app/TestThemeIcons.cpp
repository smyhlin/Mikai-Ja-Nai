#include "TestThemeIcons.hpp"

#include "state/AppearanceStore.hpp"
#include "theme/Icons.hpp"
#include "theme/Metrics.hpp"
#include "theme/Palette.hpp"
#include "widgets/SwatchButton.hpp"
#include "widgets/ToggleSwitch.hpp"

#include <player/shell/ShellTheme.hpp>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPair>
#include <QPalette>
#include <QPixmap>
#include <QRect>
#include <QStringList>
#include <QtMath>

#include <cmath>
#include <limits>

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

int alphaAt(const QImage& image, int x, int y) {
  if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) return 0;
  return image.pixelColor(x, y).alpha();
}

/// Pixels the raster has touched, or an invalid rect when it is empty.
QRect inkBounds(const QImage& image) {
  int left = image.width();
  int top = image.height();
  int right = -1;
  int bottom = -1;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (image.pixelColor(x, y).alpha() == 0) continue;
      left = qMin(left, x);
      top = qMin(top, y);
      right = qMax(right, x);
      bottom = qMax(bottom, y);
    }
  }
  if (right < left || bottom < top) return {};
  return QRect(QPoint(left, top), QPoint(right, bottom));
}

/// Pixels an antialiased edge leaves between clear and solid. A nearest-neighbour
/// resample of a vector glyph leaves almost none.
int intermediateAlphaCount(const QImage& image) {
  int count = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const int alpha = image.pixelColor(x, y).alpha();
      if (alpha > 12 && alpha < 243) ++count;
    }
  }
  return count;
}

/// Renders a widget into a device-sized image tagged with `ratio`, which is how
/// a HiDPI screen sees it.
QImage renderAt(QWidget& widget, qreal ratio) {
  const QSize logical = widget.size();
  QImage image(QSize(qMax(1, qRound(logical.width() * ratio)),
                     qMax(1, qRound(logical.height() * ratio))),
               QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  image.setDevicePixelRatio(ratio);
  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing, true);
  widget.render(&painter);
  painter.end();
  return image;
}

/// The mean x of the pixels that are exactly `color`: where a shape was drawn.
qreal centroidX(const QImage& image, const QColor& color) {
  qreal sum = 0.0;
  int count = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor pixel = image.pixelColor(x, y);
      if (pixel.alpha() != 255) continue;
      if (qAbs(pixel.red() - color.red()) > 2 || qAbs(pixel.green() - color.green()) > 2 ||
          qAbs(pixel.blue() - color.blue()) > 2) {
        continue;
      }
      sum += x;
      ++count;
    }
  }
  return count == 0 ? -1.0 : sum / count;
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

void TestThemeIcons::glyphBoxIsExactAtEveryScale() {
  const QList<qreal> scales = {1.0, 1.25, 1.5, 1.75, 2.0};
  const QColor ink(QStringLiteral("#ffffff"));
  const int pad = mikai::app::theme::kSearchIconGlyphPad;
  // The boxes the application really asks for: the padded header action, the
  // unpadded square, and the menu chevron.
  const QList<QPair<QSize, int>> requests = {
      {QSize(mikai::app::theme::kSearchIconSize + pad, mikai::app::theme::kSearchIconSize), pad},
      {QSize(mikai::app::theme::kSearchIconSize, mikai::app::theme::kSearchIconSize), 0},
      {QSize(mikai::app::theme::kSearchMenuChevronSize,
             mikai::app::theme::kSearchMenuChevronSize),
       0},
  };

  for (const qreal scale : scales) {
    for (const QPair<QSize, int>& request : requests) {
      const QSize box = request.first;
      const int gap = request.second;
      {
        const QIcon icon = mikai::app::theme::searchIcon(
            mikai::app::theme::kSearchIconSize, ink.name(), gap);
        const QPixmap pixmap = icon.pixmap(box, scale);
        QVERIFY2(!pixmap.isNull(), "no raster for a requested box");
        const qreal tagged = pixmap.devicePixelRatio();
        QVERIFY2(tagged > 0.0, "raster carries no device pixel ratio");
        QVERIFY2(tagged + 1e-9 >= scale, "raster is coarser than the screen asked for");
        QVERIFY2(qAbs(pixmap.width() / tagged - box.width()) < 1e-9,
                 qPrintable(QStringLiteral("logical width %1 for box %2 at %3")
                                .arg(pixmap.width() / tagged)
                                .arg(box.width())
                                .arg(scale)));
        QVERIFY2(qAbs(pixmap.height() / tagged - box.height()) < 1e-9,
                 qPrintable(QStringLiteral("logical height %1 for box %2 at %3")
                                .arg(pixmap.height() / tagged)
                                .arg(box.height())
                                .arg(scale)));
        QCOMPARE(icon.actualSize(box), box);
      }
    }
  }
}

void TestThemeIcons::trailingPadStaysOutsideTheGlyph() {
  const int gap = mikai::app::theme::kSearchIconGlyphPad;
  const QSize box(mikai::app::theme::kSearchIconSize + gap,
                  mikai::app::theme::kSearchIconSize);
  const QIcon icon = mikai::app::theme::searchIcon(
      mikai::app::theme::kSearchIconSize, QStringLiteral("#ffffff"), gap);

  for (const qreal scale : {1.0, 1.25, 1.5, 2.0}) {
    const QPixmap pixmap = icon.pixmap(box, scale);
    const QImage image = pixmap.toImage();
    const int padColumns = pixmap.width() - pixmap.height();
    QVERIFY2(padColumns > 0, "the trailing pad is missing from the raster");
    const QRect bounds = inkBounds(image);
    QVERIFY2(bounds.isValid(), "the glyph left no ink");
    QVERIFY2(bounds.right() < pixmap.width() - padColumns,
             "glyph ink reached into the trailing pad");
    // The glyph itself stays square: a wider box must not stretch it.
    QVERIFY2(qAbs(bounds.width() - bounds.height()) <= 2,
             qPrintable(QStringLiteral("ink %1x%2 is not square")
                            .arg(bounds.width())
                            .arg(bounds.height())));
  }
}

void TestThemeIcons::glyphEdgesStayAntialiasedAtFractionalRatio() {
  const QIcon icon =
      mikai::app::theme::playIcon(mikai::app::theme::kSearchIconSize, QStringLiteral("#ffffff"));
  const QSize box(mikai::app::theme::kSearchIconSize, mikai::app::theme::kSearchIconSize);
  for (const qreal scale : {1.25, 1.75}) {
    const QImage image = icon.pixmap(box, scale).toImage();
    QVERIFY2(intermediateAlphaCount(image) > 4,
             qPrintable(QStringLiteral("no antialiased edge at %1").arg(scale)));
  }
}

void TestThemeIcons::noTornColumnsInsideGlyphInk() {
  // The keyboard glyph is a closed outline, so every column and row between its
  // ink bounds carries ink. A resample that drops or duplicates columns shows up
  // as a hole there.
  const QIcon icon =
      mikai::app::theme::keyboardIcon(mikai::app::theme::kIconSize, QStringLiteral("#e8e8f0"));
  const qreal ratio = 1.25;
  QImage image(qRound(22 * ratio), qRound(18 * ratio), QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  image.setDevicePixelRatio(ratio);
  QPainter painter(&image);
  icon.paint(&painter, QRect(0, 0, 22, 18));
  painter.end();

  const QRect bounds = inkBounds(image);
  QVERIFY2(bounds.isValid(), "the glyph left no ink");
  for (int x = bounds.left(); x <= bounds.right(); ++x) {
    int peak = 0;
    for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
      peak = qMax(peak, alphaAt(image, x, y));
    }
    QVERIFY2(peak > 0, qPrintable(QStringLiteral("transparent column %1 inside the glyph").arg(x)));
  }
  for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
    int peak = 0;
    for (int x = bounds.left(); x <= bounds.right(); ++x) {
      peak = qMax(peak, alphaAt(image, x, y));
    }
    QVERIFY2(peak > 0, qPrintable(QStringLiteral("transparent row %1 inside the glyph").arg(y)));
  }
}

void TestThemeIcons::chevronKeepsEveryStrokeRowInked() {
  const int side = mikai::app::theme::kSearchMenuChevronSize;
  const QIcon icon = mikai::app::theme::chevronDownIcon(side, QStringLiteral("#c8c8d0"));
  for (const qreal ratio : {1.0, 1.25, 1.5}) {
    const QImage image = icon.pixmap(QSize(side, side), ratio).toImage();
    const QRect bounds = inkBounds(image);
    QVERIFY2(bounds.isValid(), "the chevron left no ink");
    // The V's two arms plus the apex: at 12 px that is four or five rows, and the
    // outermost ones are the round caps, which taper by design.
    QVERIFY(bounds.height() >= 4);
    for (int y = bounds.top() + 1; y < bounds.bottom(); ++y) {
      int peak = 0;
      for (int x = bounds.left(); x <= bounds.right(); ++x) {
        peak = qMax(peak, alphaAt(image, x, y));
      }
      QVERIFY2(peak >= 128,
               qPrintable(QStringLiteral("stroke row %1 at %2 is %.0f%% covered")
                              .arg(y)
                              .arg(ratio)
                              .arg(peak * 100.0 / 255.0)));
    }
  }
}

void TestThemeIcons::paintPathsDoNotMultiplyTheDevicePixelRatio() {
#ifdef MIKAI_SOURCE_DIR
  // A widget painter is already in logical coordinates. `worldTransform() * dpr`
  // scales all nine matrix entries, doubling the drawing and turning the
  // transform projective, so the pattern is banned outright.
  const QStringList files = {
      QStringLiteral("src/app/widgets/ToggleSwitch.cpp"),
      QStringLiteral("src/app/widgets/SwatchButton.cpp"),
      QStringLiteral("src/player/shell/ShellWidgets.cpp"),
  };
  for (const QString& relative : files) {
    QFile file(QDir(QStringLiteral(MIKAI_SOURCE_DIR)).filePath(relative));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(QStringLiteral("cannot read %1").arg(relative)));
    const QString body = QString::fromUtf8(file.readAll());
    QVERIFY2(!body.contains(QStringLiteral("worldTransform() *")),
             qPrintable(QStringLiteral("%1 multiplies the painter by the device pixel ratio")
                            .arg(relative)));
  }
#else
  QSKIP("MIKAI_SOURCE_DIR is not defined");
#endif
}

void TestThemeIcons::toggleAndSwatchPaintOnceAtEveryRatio() {
  const mikai::app::theme::Palette palette =
      mikai::app::theme::paletteFor(mikai::app::currentAppearance());

  for (const qreal ratio : {1.0, 1.25, 1.5, 2.0}) {
    mikai::app::ToggleSwitch toggle;
    toggle.resize(mikai::app::theme::kSettingsSwitchWidth,
                  mikai::app::theme::kSettingsSwitchHeight);
    const QImage track = renderAt(toggle, ratio);
    // A second DPR multiplication draws the track past the widget and clips it,
    // leaving a solid column on the boundary.
    for (int y = 0; y < track.height(); ++y) {
      QVERIFY2(alphaAt(track, track.width() - 1, y) < 250,
               qPrintable(QStringLiteral("toggle clipped at %1 on the right").arg(ratio)));
    }
    // The knob is drawn once, at its logical position.
    const qreal knobX = centroidX(track, QColor(palette.muted));
    QVERIFY2(knobX >= 0.0, "the knob was not painted");
    const qreal expected = 10.5 * ratio;
    QVERIFY2(qAbs(knobX - expected) <= 2.5,
             qPrintable(QStringLiteral("knob at %1, expected %2 at %3")
                            .arg(knobX)
                            .arg(expected)
                            .arg(ratio)));

    mikai::app::SwatchButton swatch(QColor(200, 40, 90));
    swatch.resize(mikai::app::theme::kSettingsSwatchSize,
                  mikai::app::theme::kSettingsSwatchSize);
    const QImage chip = renderAt(swatch, ratio);
    for (int y = 0; y < chip.height(); ++y) {
      QVERIFY2(alphaAt(chip, 0, y) == 0,
               qPrintable(QStringLiteral("swatch clipped on the left at %1").arg(ratio)));
      QVERIFY2(alphaAt(chip, chip.width() - 1, y) == 0,
               qPrintable(QStringLiteral("swatch clipped on the right at %1").arg(ratio)));
    }
    const QRect chipInk = inkBounds(chip);
    QVERIFY2(chipInk.isValid(), "the swatch was not painted");
    QVERIFY2(qAbs(chipInk.left() - 3.0 * ratio) <= 2.0,
             qPrintable(QStringLiteral("swatch body starts at %1 instead of %2")
                            .arg(chipInk.left())
                            .arg(3.0 * ratio)));
  }
}

void TestThemeIcons::shellGlyphJoinsAreRound() {
  // expand() is a polyline, so its corners are real joins. A miter reaches past
  // the stroke's half-width, so solid ink must never appear outside the round
  // envelope of the segments. (At the shell's constant 2.25 px stroke the excess
  // a miter adds is under one pixel of antialiasing, which is why the threshold
  // is a fully opaque one: this stays a real guard if a stroke grows.)
  const QList<QPointF> segments = {
      QPointF(15, 3),  QPointF(21, 3),    // polyline 1, arm 1
      QPointF(21, 3),  QPointF(21, 9),    // polyline 1, arm 2
      QPointF(9, 21),  QPointF(3, 21),    // polyline 2, arm 1
      QPointF(3, 21),  QPointF(3, 15),    // polyline 2, arm 2
      QPointF(21, 3),  QPointF(14, 10),   // diagonal
      QPointF(3, 21),  QPointF(10, 14),   // diagonal
  };
  for (const int side : {24, 30}) {
    QPixmap pixmap(side, side);
    pixmap.fill(Qt::transparent);
    {
      QPainter painter(&pixmap);
      painter.setRenderHint(QPainter::Antialiasing, true);
      mplayer::shell::IconPainter::expand(painter, 0, 0, side);
    }
    const QImage image = pixmap.toImage();
    const qreal k = side / 24.0;
    const qreal halfWidth = 2.25 / 2.0;
    int solid = 0;
    for (int y = 0; y < image.height(); ++y) {
      for (int x = 0; x < image.width(); ++x) {
        if (image.pixelColor(x, y).alpha() < 250) continue;
        ++solid;
        const QPointF at(x + 0.5, y + 0.5);
        qreal nearest = std::numeric_limits<qreal>::max();
        for (int i = 0; i + 1 < segments.size(); i += 2) {
          const QPointF a(segments.at(i).x() * k, segments.at(i).y() * k);
          const QPointF b(segments.at(i + 1).x() * k, segments.at(i + 1).y() * k);
          const QPointF ab = b - a;
          const qreal lengthSquared = ab.x() * ab.x() + ab.y() * ab.y();
          qreal t = lengthSquared > 0.0
                        ? ((at.x() - a.x()) * ab.x() + (at.y() - a.y()) * ab.y()) / lengthSquared
                        : 0.0;
          t = qBound(0.0, t, 1.0);
          const QPointF closest(a.x() + ab.x() * t, a.y() + ab.y() * t);
          const qreal dx = at.x() - closest.x();
          const qreal dy = at.y() - closest.y();
          nearest = qMin(nearest, std::sqrt(dx * dx + dy * dy));
        }
        QVERIFY2(nearest <= halfWidth + 0.75,
                 qPrintable(QStringLiteral("solid ink %1 px from a stroke at %2 px")
                                .arg(nearest)
                                .arg(side)));
      }
    }
    QVERIFY2(solid > 20, "the glyph did not paint");
  }
}

void TestThemeIcons::shellAtmosBadgeStaysInsideItsBox() {
  for (const int side : {24, 48}) {
    QPixmap pixmap(side, side);
    pixmap.fill(Qt::transparent);
    {
      QPainter painter(&pixmap);
      painter.setRenderHint(QPainter::Antialiasing, true);
      mplayer::shell::IconPainter::dolbyAtmos(painter, 0, 0, side);
    }
    const QImage image = pixmap.toImage();
    for (int i = 0; i < side; ++i) {
      QVERIFY2(alphaAt(image, 0, i) == 0,
               qPrintable(QStringLiteral("badge ink on the left edge at %1").arg(side)));
      QVERIFY2(alphaAt(image, side - 1, i) == 0,
               qPrintable(QStringLiteral("badge ink on the right edge at %1").arg(side)));
      QVERIFY2(alphaAt(image, i, 0) == 0,
               qPrintable(QStringLiteral("badge ink on the top edge at %1").arg(side)));
      QVERIFY2(alphaAt(image, i, side - 1) == 0,
               qPrintable(QStringLiteral("badge ink on the bottom edge at %1").arg(side)));
    }
    QVERIFY2(inkBounds(image).isValid(), "the badge did not paint");
  }
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
