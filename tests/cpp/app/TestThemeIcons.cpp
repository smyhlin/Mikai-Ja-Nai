#include "TestThemeIcons.hpp"

#include "state/AppearanceStore.hpp"
#include "theme/Icons.hpp"
#include "theme/Metrics.hpp"
#include "theme/Palette.hpp"
#include "widgets/SwatchButton.hpp"
#include "widgets/TitleCard.hpp"
#include "widgets/ToggleSwitch.hpp"

#include <player/quick/PlayerIconProvider.hpp>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QPair>
#include <QPalette>
#include <QPixmap>
#include <QRect>
#include <QRegion>
#include <QStringList>
#include <QTransform>
#include <QVector>
#include <QWindow>
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
///
/// Without an explicit flag set, QWidget::render() also paints the window
/// background: the widget's whole region is source-filled with the opaque
/// QPalette::Window brush (QWidgetPrivate::paintBackground), which would make
/// every "this pixel is clear" assertion below a statement about the palette
/// rather than about the control.
QImage renderAt(QWidget& widget, qreal ratio) {
  const QSize logical = widget.size();
  QImage image(QSize(qMax(1, qRound(logical.width() * ratio)),
                     qMax(1, qRound(logical.height() * ratio))),
               QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  image.setDevicePixelRatio(ratio);
  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing, true);
  widget.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
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

/// The bounds of the pixels an opaque shape filled with `color` covers, or an
/// invalid rect when the colour is absent. Unlike inkBounds this ignores the
/// antialiased edge and any other shape in the same raster.
QRect colorBounds(const QImage& image, const QColor& color) {
  int left = image.width();
  int top = image.height();
  int right = -1;
  int bottom = -1;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor pixel = image.pixelColor(x, y);
      if (pixel.alpha() < 250) continue;
      if (qAbs(pixel.red() - color.red()) > 2 || qAbs(pixel.green() - color.green()) > 2 ||
          qAbs(pixel.blue() - color.blue()) > 2) {
        continue;
      }
      left = qMin(left, x);
      top = qMin(top, y);
      right = qMax(right, x);
      bottom = qMax(bottom, y);
    }
  }
  if (right < left || bottom < top) return {};
  return QRect(QPoint(left, top), QPoint(right, bottom));
}

/// Records what a widget's own paint code sends to a painter, without drawing it.
/// QPainter resolves QPaintDevice::redirected() before it ever asks for a paint
/// engine, and QWidget::render(QPaintDevice*) points that redirect at the target,
/// so a widget's real QPainter commands and transform updates land here.
class TraceEngine : public QPaintEngine {
 public:
  // A recorder takes every command verbatim, so it accepts the three things
  // QPainter otherwise emulates: an affine painter transform
  // (PrimitiveTransform - the traced device carries the window's ratio), a
  // non-solid pen such as the toggle's Qt::NoPen (BrushStroke), and a
  // translucent fill or border (AlphaBlend). Without these QPainter reroutes
  // each shape through QPainterPrivate::draw_helper, which rasterises it into a
  // QImage and emits one drawImage, so the widget's own vector geometry would
  // never reach the trace.
  TraceEngine()
      : QPaintEngine(QPaintEngine::PrimitiveTransform | QPaintEngine::BrushStroke |
                     QPaintEngine::AlphaBlend) {}

  bool begin(QPaintDevice*) override { return true; }
  bool end() override { return true; }

  void updateState(const QPaintEngineState& state) override {
    if (!(state.state() & QPaintEngine::DirtyTransform)) return;
    ++transformUpdates_;
    const QTransform transform = state.transform();
    if (transform.type() > worstType_) worstType_ = transform.type();
    if (!qFuzzyCompare(transform.m33(), qreal(1.0))) unitM33_ = false;
  }

  void drawRects(const QRectF*, int) override { record(QStringLiteral("rects")); }
  void drawLines(const QLineF*, int) override { record(QStringLiteral("lines")); }
  void drawEllipse(const QRectF&) override { record(QStringLiteral("ellipse")); }
  void drawPath(const QPainterPath&) override { record(QStringLiteral("path")); }
  void drawPolygon(const QPointF*, int, PolygonDrawMode) override {
    record(QStringLiteral("polygon"));
  }
  void drawPixmap(const QRectF&, const QPixmap&, const QRectF&) override {
    record(QStringLiteral("pixmap"));
  }
  void drawImage(const QRectF&, const QImage&, const QRectF&, Qt::ImageConversionFlags) override {
    record(QStringLiteral("image"));
  }

  Type type() const override { return QPaintEngine::User; }

  void reset() {
    commands_ = 0;
    transformUpdates_ = 0;
    worstType_ = QTransform::TxNone;
    unitM33_ = true;
    kinds_.clear();
  }
  int commands() const { return commands_; }
  int transformUpdates() const { return transformUpdates_; }
  QTransform::TransformationType worstType() const { return worstType_; }
  bool unitM33() const { return unitM33_; }
  bool saw(const QString& kind) const { return kinds_.contains(kind); }
  QStringList kinds() const { return kinds_; }

 private:
  void record(const QString& kind) {
    ++commands_;
    if (!kinds_.contains(kind)) kinds_.append(kind);
  }

  int commands_ = 0;
  int transformUpdates_ = 0;
  QTransform::TransformationType worstType_ = QTransform::TxNone;
  bool unitM33_ = true;
  QStringList kinds_;
};

/// A paint device with the widget's own ratio, so the engine sees the transform
/// a real backing store would carry.
class TraceDevice : public QPaintDevice {
 public:
  TraceDevice(const QSize& size, qreal ratio) : size_(size), ratio_(ratio > 0.0 ? ratio : 1.0) {}

  QPaintEngine* paintEngine() const override { return &engine_; }
  TraceEngine& engine() { return engine_; }

  int metric(PaintDeviceMetric metricId) const override {
    switch (metricId) {
      case PdmWidth:
        return size_.width();
      case PdmHeight:
        return size_.height();
      case PdmWidthMM:
        return size_.width();
      case PdmHeightMM:
        return size_.height();
      case PdmNumColors:
        return 0;
      case PdmDepth:
        return 32;
      case PdmDpiX:
      case PdmDpiY:
      case PdmPhysicalDpiX:
      case PdmPhysicalDpiY:
        return 96;
      case PdmDevicePixelRatio:
        return qMax(1, int(ratio_));
      case PdmDevicePixelRatioScaled:
        return qRound(ratio_ * QPaintDevice::devicePixelRatioFScale());
      default:
        return 0;
    }
  }

 private:
  QSize size_;
  qreal ratio_;
  mutable TraceEngine engine_;
};

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

void TestThemeIcons::customColorAlphaIsVisible() {
  const QColor lowAlpha(37, 89, 143, 72);
  const QColor highAlpha(37, 89, 143, 196);
  const QPixmap low = mikai::app::theme::gearIcon(25, lowAlpha.name(QColor::HexArgb))
                          .pixmap(QSize(25, 25));
  const QPixmap high = mikai::app::theme::gearIcon(25, highAlpha.name(QColor::HexArgb))
                           .pixmap(QSize(25, 25));
  QVERIFY(containsColorAtAlpha(low, lowAlpha));
  QVERIFY(containsColorAtAlpha(high, highAlpha));
}

void TestThemeIcons::glyphBoxIsPhysicalAtEveryScale() {
  // The contract the user approved: the icon box is rasterised at the screen's
  // own ratio, in exactly qRound(logical * dpr) device pixels, tagged with that
  // same ratio. The glyph inside it is square in physical pixels, so no ratio can
  // stretch it, and the pad is the only extra width.
  const QList<qreal> scales = {1.0, 1.25, 1.5, 1.75, 2.0};
  const QColor ink(QStringLiteral("#ffffff"));
  const int pad = mikai::app::theme::kIconGlyphPad;
  const QList<QPair<QSize, int>> requests = {
      {QSize(mikai::app::theme::kIconSize + pad, mikai::app::theme::kIconSize), pad},
      {QSize(mikai::app::theme::kIconSize, mikai::app::theme::kIconSize), 0},
  };

  for (const qreal scale : scales) {
    for (const QPair<QSize, int>& request : requests) {
      const QSize box = request.first;
      const int gap = request.second;
      const QIcon icon =
          mikai::app::theme::searchIcon(mikai::app::theme::kIconSize, ink.name(), gap);
      QCOMPARE(icon.actualSize(box), box);

      const QPixmap pixmap = icon.pixmap(box, scale);
      QVERIFY2(!pixmap.isNull(), "no raster for a requested box");
      const QSize expected(qRound(box.width() * scale), qRound(box.height() * scale));
      QVERIFY2(pixmap.size() == expected,
               qPrintable(QStringLiteral("%1x%2 device px, expected %3x%4 at %5")
                              .arg(pixmap.width())
                              .arg(pixmap.height())
                              .arg(expected.width())
                              .arg(expected.height())
                              .arg(scale)));
      QVERIFY2(qFuzzyCompare(pixmap.devicePixelRatio(), scale),
               qPrintable(QStringLiteral("tagged %1, screen asks %2")
                              .arg(pixmap.devicePixelRatio())
                              .arg(scale)));

      const QRect bounds = inkBounds(pixmap.toImage());
      QVERIFY2(bounds.isValid(), "the glyph left no ink");
      const int padColumns = expected.width() - qRound(box.height() * scale);
      QVERIFY2(padColumns >= 0, "the box is narrower than its glyph");
      QVERIFY2(bounds.right() < expected.width() - padColumns,
               "glyph ink reached into the trailing pad");
      // Square in physical pixels: the glyph is never stretched to fill the pad.
      QVERIFY2(qAbs(bounds.width() - bounds.height()) <= 2,
               qPrintable(QStringLiteral("ink %1x%2 is not square at %3")
                              .arg(bounds.width())
                              .arg(bounds.height())
                              .arg(scale)));
    }
  }
}

void TestThemeIcons::trailingPadStaysOutsideTheGlyph() {
  const int gap = mikai::app::theme::kIconGlyphPad;
  const QSize box(mikai::app::theme::kIconSize + gap, mikai::app::theme::kIconSize);
  const QIcon icon = mikai::app::theme::searchIcon(
      mikai::app::theme::kIconSize, QStringLiteral("#ffffff"), gap);

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
      mikai::app::theme::playIcon(mikai::app::theme::kIconSize, QStringLiteral("#ffffff"));
  const QSize box(mikai::app::theme::kIconSize, mikai::app::theme::kIconSize);
  for (const qreal scale : {1.25, 1.75}) {
    const QImage image = icon.pixmap(box, scale).toImage();
    QVERIFY2(intermediateAlphaCount(image) > 4,
             qPrintable(QStringLiteral("no antialiased edge at %1").arg(scale)));
  }
}

void TestThemeIcons::headerGlyphsShareTheirLeadingInkEdge_data() {
  QTest::addColumn<qreal>("ratio");
  QTest::newRow("100-percent") << qreal(1.0);
  QTest::newRow("125-percent") << qreal(1.25);
}

void TestThemeIcons::headerGlyphsShareTheirLeadingInkEdge() {
  QFETCH(qreal, ratio);
  const int size = mikai::app::theme::kIconSize;
  const QString color = QStringLiteral("#ffffff");
  const QList<QIcon> icons{
      mikai::app::theme::playIcon(size, color),
      mikai::app::theme::filterIcon(size, color),
      mikai::app::theme::teamsIcon(size, color),
      mikai::app::theme::searchIcon(size, color)};
  int leftmost = std::numeric_limits<int>::max();
  int rightmost = 0;
  for (const QIcon& icon : icons) {
    const QRect bounds = inkBounds(icon.pixmap(QSize(size, size), ratio).toImage());
    QVERIFY2(!bounds.isEmpty(), "A header glyph has no visible ink");
    leftmost = qMin(leftmost, bounds.left());
    rightmost = qMax(rightmost, bounds.left());
  }
  QVERIFY2(rightmost - leftmost <= 1,
           qPrintable(QStringLiteral("DPR %1: leading ink spans device pixels %2..%3")
                          .arg(ratio)
                          .arg(leftmost)
                          .arg(rightmost)));
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

void TestThemeIcons::toggleAndSwatchPaintOnceAtEveryRatio() {
  const mikai::app::theme::Palette palette =
      mikai::app::theme::paletteFor(mikai::app::currentAppearance());

  for (const qreal ratio : {1.0, 1.25, 1.5, 1.75, 2.0}) {
    mikai::app::ToggleSwitch toggle;
    // Polishing is what QStyleSheetStyle uses to raise a widget's minimum size
    // from the ambient sheet, and it happens on first paint, so a size pinned
    // before it can be overridden afterwards. Pin after the polish and assert
    // the box, so the fixture measures the control and not the sheet. Focus is
    // cleared explicitly: a focused control paints an extra ring, which is not
    // the body geometry this case compares.
    toggle.clearFocus();
    toggle.ensurePolished();
    toggle.setFixedSize(mikai::app::theme::kSettingsSwitchWidth,
                        mikai::app::theme::kSettingsSwitchHeight);
    QCOMPARE(toggle.size(), QSize(mikai::app::theme::kSettingsSwitchWidth,
                                  mikai::app::theme::kSettingsSwitchHeight));
    QVERIFY2(!toggle.hasFocus(), "the toggle fixture must not carry focus");
    const QImage track = renderAt(toggle, ratio);
    // The track is a rounded rect inset by half a logical pixel, so its widest
    // row covers half of the boundary pixel at worst. Anything that draws the
    // track again past the widget (a painter scaled by the ratio) clips it into
    // a solid boundary column instead.
    for (int y = 0; y < track.height(); ++y) {
      QVERIFY2(alphaAt(track, track.width() - 1, y) < 250,
               qPrintable(QStringLiteral("toggle clipped at %1 on the right").arg(ratio)));
    }
    // The knob is drawn once, at its logical position and its logical diameter:
    // an extra scale would both move its centre and grow it by that scale.
    const QRect knob = colorBounds(track, QColor(palette.muted));
    QVERIFY2(knob.isValid(), "the knob was not painted");
    const qreal knobX = centroidX(track, QColor(palette.muted));
    const qreal expected = 10.5 * ratio;
    QVERIFY2(qAbs(knobX - expected) <= 2.5,
             qPrintable(QStringLiteral("knob at %1, expected %2 at %3")
                            .arg(knobX)
                            .arg(expected)
                            .arg(ratio)));
    // The knob is height - 6 logical pixels across; a disc's fully covered
    // interior is ~1 px smaller than its bounding box on each side.
    QVERIFY2(qAbs(knob.width() - (mikai::app::theme::kSettingsSwitchHeight - 6) * ratio) <= 3.0,
             qPrintable(QStringLiteral("knob spans %1 px at %2")
                            .arg(knob.width())
                            .arg(ratio)));
    QVERIFY2(qAbs(knob.left() - 2.5 * ratio) <= 2.0,
             qPrintable(QStringLiteral("knob starts at %1 instead of %2")
                            .arg(knob.left())
                            .arg(2.5 * ratio)));

    // The settings tab fixed-sizes every swatch: a plain resize() would be
    // clamped to QPushButton's sizeHint (its minimumSizeHint), which is wider
    // than the chip and would measure a shape the application never shows.
    //
    // The other clamp is the ambient sheet: QStyleSheetStyle applies a rule's
    // min-height plus padding plus border as the widget's minimum size while it
    // polishes, and the application's QPushButton rule asks for more height than
    // this chip. Polishing happens on first paint, after a size pinned at
    // construction, which is why another suite setting the sheet made the body
    // two logical pixels taller. Polish first, pin second, and clear focus -
    // SwatchButton paints a selection ring when focused, which is not the body
    // geometry measured here.
    mikai::app::SwatchButton swatch(QColor(200, 40, 90));
    swatch.clearFocus();
    swatch.ensurePolished();
    swatch.setFixedSize(mikai::app::theme::kSettingsSwatchSize,
                        mikai::app::theme::kSettingsSwatchSize);
    QCOMPARE(swatch.size(), QSize(mikai::app::theme::kSettingsSwatchSize,
                                  mikai::app::theme::kSettingsSwatchSize));
    QVERIFY2(!swatch.hasFocus(), "the swatch fixture must not carry focus");
    const QImage chip = renderAt(swatch, ratio);
    for (int y = 0; y < chip.height(); ++y) {
      QVERIFY2(alphaAt(chip, 0, y) == 0,
               qPrintable(QStringLiteral("swatch clipped on the left at %1").arg(ratio)));
      QVERIFY2(alphaAt(chip, chip.width() - 1, y) == 0,
               qPrintable(QStringLiteral("swatch clipped on the right at %1").arg(ratio)));
    }
    // SwatchButton paints the body as the widget inset by 3 logical pixels with a
    // 1 px border centred on it, so the chip's silhouette is (size - 6) + 1
    // logical pixels across and its leftmost ink sits at 3 - 0.5. Both numbers
    // are read off the painted output: a second scale would move the edge in and
    // overshoot the widget instead.
    const QRect chipInk = inkBounds(chip);
    QVERIFY2(chipInk.isValid(), "the swatch was not painted");
    const qreal silhouette = (mikai::app::theme::kSettingsSwatchSize - 6.0 + 1.0) * ratio;
    QVERIFY2(qAbs(chipInk.width() - silhouette) <= 2.0,
             qPrintable(QStringLiteral("swatch spans %1 px at %2, body plus border is %3")
                            .arg(chipInk.width())
                            .arg(ratio)
                            .arg(silhouette)));
    QVERIFY2(qAbs(chipInk.height() - silhouette) <= 2.0,
             qPrintable(QStringLiteral("swatch is %1 px tall at %2, body plus border is %3")
                            .arg(chipInk.height())
                            .arg(ratio)
                            .arg(silhouette)));
    QVERIFY2(qAbs(chipInk.left() - 2.5 * ratio) <= 1.5,
             qPrintable(QStringLiteral("swatch starts at %1 instead of %2")
                            .arg(chipInk.left())
                            .arg(2.5 * ratio)));
  }
}

void TestThemeIcons::artworkFrameIsAllocatedAtTheDeviceSize() {
  // The home card poster: an 80x114 slot must own a 160x228 raster at 200 %,
  // not an 80x114 one that Qt upscales. The frame is also painted from logical
  // coordinates, which is what keeps the corner radius and the shade readable.
  QImage source(320, 456, QImage::Format_ARGB32_Premultiplied);
  source.fill(QColor(30, 90, 200));
  const QPixmap pixmap = QPixmap::fromImage(source);
  const QSize logical(mikai::app::theme::kHomePosterWidth,
                      mikai::app::theme::kHomePosterHeight);

  for (const qreal ratio : {1.0, 1.25, 1.5, 1.75, 2.0}) {
    // The home card's poster carries the radius its own rule declares, and the
    // cover shade its surface asks for.
    const QPixmap frame = mikai::app::TitleCard::rounded(
        pixmap, logical, ratio,
        mikai::app::theme::radiusSmall(mikai::app::currentAppearance()), true);
    QVERIFY2(!frame.isNull(), "no card frame was painted");
    QVERIFY2(qFuzzyCompare(frame.devicePixelRatio(), ratio), "the frame carries another ratio");
    QCOMPARE(frame.size(), QSize(qRound(logical.width() * ratio),
                                qRound(logical.height() * ratio)));
    const QImage image = frame.toImage();
    // The rounded corners are composited away, so the outermost pixel is clear
    // while the middle of the artwork is solid: the mask ran in logical
    // coordinates against a device-resolution raster.
    QVERIFY2(alphaAt(image, 0, 0) == 0, "the frame's corner is not rounded");
    QVERIFY2(alphaAt(image, image.width() / 2, image.height() / 4) > 200,
             "the frame's artwork is missing");
  }
}

void TestThemeIcons::quickGlyphUsesThemeTintAndDevicePixelRatio() {
  mplayer::quick::PlayerIconProvider provider;
  const QColor tint(37, 89, 143, 196);
  const QString request =
      QStringLiteral("play?size=24&dpr=1.5&color=") +
      QString::fromLatin1(tint.name(QColor::HexArgb).toUtf8().toPercentEncoding());
  QSize reported;
  const QImage image = provider.requestImage(request, &reported, QSize());

  QVERIFY(!image.isNull());
  QCOMPARE(image.size(), QSize(36, 36));
  QCOMPARE(reported, image.size());
  QCOMPARE(image.devicePixelRatio(), 1.5);
  bool visibleTint = false;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor pixel = image.pixelColor(x, y);
      if (pixel.red() == tint.red() && pixel.green() == tint.green() &&
          pixel.blue() == tint.blue() && pixel.alpha() == tint.alpha()) {
        visibleTint = true;
        break;
      }
    }
    if (visibleTint) break;
  }
  QVERIFY2(visibleTint, "the Quick glyph must visibly use the requested theme tint");
}
void TestThemeIcons::quickGlyphRequestTracksTintAndDprChanges() {
  mplayer::quick::PlayerIconProvider provider;
  const QColor firstTint(29, 67, 223, 196);
  const QColor secondTint(211, 31, 47, 112);
  const auto request = [](const QColor& tint, qreal dpr) {
    return QStringLiteral("play?size=24&dpr=%1&color=%2")
        .arg(QString::number(dpr, 'f', 3),
             QString::fromLatin1(tint.name(QColor::HexArgb).toUtf8().toPercentEncoding()));
  };

  QSize firstSize;
  const QImage first = provider.requestImage(request(firstTint, 1.0), &firstSize, QSize());
  QVERIFY(!first.isNull());
  QCOMPARE(first.size(), QSize(24, 24));
  QCOMPARE(firstSize, first.size());
  QCOMPARE(first.devicePixelRatio(), 1.0);
  QVERIFY(containsColorAtAlpha(QPixmap::fromImage(first), firstTint));

  // This is the URL change PlayerIcon.qml makes when its bound theme tint or
  // Window.devicePixelRatio changes. A fresh provider request must not reuse
  // the prior themed raster.
  QSize changedSize;
  const QImage changed = provider.requestImage(request(secondTint, 1.5), &changedSize, QSize());
  QVERIFY(!changed.isNull());
  QCOMPARE(changed.size(), QSize(36, 36));
  QCOMPARE(changedSize, changed.size());
  QCOMPARE(changed.devicePixelRatio(), 1.5);
  QVERIFY(containsColorAtAlpha(QPixmap::fromImage(changed), secondTint));
  QVERIFY(!containsColorAtAlpha(QPixmap::fromImage(changed), firstTint));
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

void TestThemeIcons::nativeWindowPaintStaysAffineAtFractionalDpr() {
  // The removed double-DPR blocks only ran when the widget had its own window
  // handle, and they multiplied the painter's world transform by that window's
  // ratio - scaling all nine entries, m33 included, which makes the transform
  // projective. A shown top-level widget is the only way to reach that state,
  // and painting it into a tracing device shows what it really emits.
  mikai::app::ToggleSwitch toggle;
  toggle.show();
  mikai::app::SwatchButton swatch(QColor(200, 40, 90));
  swatch.setFixedSize(mikai::app::theme::kSettingsSwatchSize,
                      mikai::app::theme::kSettingsSwatchSize);
  swatch.show();
  QCoreApplication::processEvents();

  const QVector<QPair<QString, QWidget*>> targets = {
      {QStringLiteral("ToggleSwitch"), &toggle},
      {QStringLiteral("SwatchButton"), &swatch},
  };

  QWindow* handle = toggle.windowHandle();
  if (handle == nullptr) {
    QSKIP("no native window handle: the removed block could not have run either");
  }
  // The block multiplied by the window's ratio, so a 1.0 window cannot show it.
  const qreal ratio = handle->devicePixelRatio();
  if (ratio <= 1.0) {
    QSKIP("window ratio is 1.0: the TestThemeIcons CTest environment sets "
          "QT_SCALE_FACTOR=1.25; run this suite standalone to reach it");
  }

  for (const QPair<QString, QWidget*>& target : targets) {
    QWidget* widget = target.second;
    // A paint device with the widget's own ratio, the way its backing store has.
    TraceDevice device(QSize(widget->width() * 2, widget->height() * 2), ratio);
    widget->render(&device, QPoint(), QRegion(), QWidget::DrawChildren);
    const TraceEngine& trace = device.engine();

    const QString kinds =
        trace.kinds().isEmpty() ? QStringLiteral("none") : trace.kinds().join(QStringLiteral(", "));

    QVERIFY2(widget->windowHandle() != nullptr,
             qPrintable(QStringLiteral("%1 is not a shown native window").arg(target.first)));
    QVERIFY2(trace.commands() > 0,
             qPrintable(QStringLiteral("%1 emitted no paint commands").arg(target.first)));
    // Non-vacuity: the control's own vector geometry reached the engine, so the
    // transform checked below belongs to real drawing rather than to an empty
    // stream. The primitive kind itself is not pinned - QPainter may hand a
    // rounded rect or an ellipse over as a QPainterPath - only that some shape
    // was emitted instead of nothing but a blit.
    const QStringList vectorKinds = {QStringLiteral("path"), QStringLiteral("rects"),
                                     QStringLiteral("ellipse"), QStringLiteral("polygon"),
                                     QStringLiteral("lines")};
    bool vectorGeometry = false;
    for (const QString& kind : vectorKinds) {
      if (trace.saw(kind)) {
        vectorGeometry = true;
        break;
      }
    }
    QVERIFY2(vectorGeometry,
             qPrintable(QStringLiteral("%1 emitted no vector geometry (kinds: %2)")
                            .arg(target.first, kinds)));
    // The painter state is flushed before the first command, so a transform was
    // really observed. A DPR-multiplied world transform shows up as TxProject.
    QVERIFY2(trace.transformUpdates() > 0,
             qPrintable(QStringLiteral("%1 emitted no transform update (kinds: %2)")
                            .arg(target.first, kinds)));
    QVERIFY2(trace.worstType() < QTransform::TxProject,
             qPrintable(QStringLiteral("%1 painted through a projective transform at %2 "
                                       "(%3 updates, kinds: %4)")
                            .arg(target.first)
                            .arg(ratio)
                            .arg(trace.transformUpdates())
                            .arg(kinds)));
    QVERIFY2(trace.unitM33(),
             qPrintable(QStringLiteral("%1 scaled the matrix' m33 at %2 (kinds: %3)")
                            .arg(target.first)
                            .arg(ratio)
                            .arg(kinds)));
  }
}
