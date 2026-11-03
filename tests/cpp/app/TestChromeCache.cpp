#include "TestChromeCache.hpp"

#include <player/shell/ShellWidgets.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QtGlobal>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtWidgets/QWidget>

#include <cmath>

namespace {

using mplayer::shell::ChromeHost;
using mplayer::shell::ChromeList;
using mplayer::shell::ChromePaint;
using mplayer::shell::chromeUpdate;

/// A control whose pixels are its own fill. `setFillQuietly` moves that state
/// and tells nobody, so a paint that still shows the old colour is a paint that
/// came out of the cache - the property under test, read from the image instead
/// of from a call.
class QuietChrome : public QWidget, public ChromePaint {
 public:
  explicit QuietChrome(QWidget* parent) : QWidget(parent) {
    resize(12, 12);
    setAttribute(Qt::WA_TranslucentBackground, true);
  }

  void paintChrome(QPainter& painter) override;
  void setFillQuietly(const QColor& color) { fill_ = color; }

 private:
  QColor fill_ = QColor(20, 200, 90, 255);
};

/// The same control, but drawn as a translucent rounded card inset by a pixel:
/// the corner of its box is outside the shape, which is where a blit that
/// carried a black backing would show.
class VeilChrome : public QWidget, public ChromePaint {
 public:
  explicit VeilChrome(QWidget* parent) : QWidget(parent) {
    resize(16, 16);
    setAttribute(Qt::WA_TranslucentBackground, true);
  }

  void paintChrome(QPainter& painter) override;

  static QColor veil() { return QColor(200, 40, 40, 128); }
};

/// The smallest host that has a ChromeList: the invalidation route a control
/// takes is `chromeUpdate()` -> this, exactly as the shell wires it.
class Host : public QWidget, public ChromeHost {
 public:
  Host() { resize(40, 40); }

  void chromeDirty(QWidget* widget, const QRect&) override { chrome.invalidate(widget); }
  void chromeDirtyAll() override { chrome.invalidateAll(); }

  ChromeList chrome;
};

void QuietChrome::paintChrome(QPainter& painter) { painter.fillRect(rect(), fill_); }

void VeilChrome::paintChrome(QPainter& painter) {
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(Qt::NoPen);
  painter.setBrush(veil());
  painter.drawRoundedRect(QRectF(1.0, 1.0, width() - 2.0, height() - 2.0), 5.0, 5.0);
}

/// Paints the host's adopted chrome into an image at `ratio` over `frame`, the
/// way the shell's painter does - the frame stays visible wherever a control
/// does not cover it.
QImage painted(ChromeList& chrome, QWidget* root, qreal ratio, const QColor& frame) {
  const QSize size = root->size();
  QImage image(QSize(qRound(size.width() * ratio), qRound(size.height() * ratio)),
               QImage::Format_ARGB32_Premultiplied);
  image.setDevicePixelRatio(ratio);
  image.fill(frame);
  QPainter painter(&image);
  chrome.paint(painter, root, QRect(QPoint(0, 0), size));
  painter.end();
  return image;
}

/// `fill` composited over `below` at its own alpha, so an expected pixel is
/// computed rather than copied from whatever the paint happened to produce.
QColor blendOver(const QColor& fill, const QColor& below) {
  const double alpha = fill.alphaF();
  const auto mix = [alpha](int top, int bottom) {
    return qRound(top * alpha + bottom * (1.0 - alpha));
  };
  return QColor(mix(fill.red(), below.red()), mix(fill.green(), below.green()),
                mix(fill.blue(), below.blue()));
}

bool near(const QColor& got, const QColor& want, int tolerance = 2) {
  return std::abs(got.red() - want.red()) <= tolerance &&
         std::abs(got.green() - want.green()) <= tolerance &&
         std::abs(got.blue() - want.blue()) <= tolerance;
}

/// Accepts the widget for a run: shown without a window, because an adopted
/// control that is not visible is skipped (correctly) by the cache walk.
bool presented(QWidget* widget) {
  widget->setAttribute(Qt::WA_DontShowOnScreen, true);
  widget->show();
  return widget->isVisible();
}

}  // namespace

void TestChromeCache::aSecondPaintServesTheCachedPixels() {
  Host host;
  auto* control = new QuietChrome(&host);
  control->move(4, 5);
  host.chrome.adopt(&host, nullptr);
  QVERIFY(presented(&host));

  const QColor first = QColor(20, 200, 90, 255);
  const QColor second = QColor(220, 30, 40, 255);

  // The control stands at (4,5) and is 12x12, so (9,10) is inside it.
  const QImage one = painted(host.chrome, &host, 1.0, Qt::black);
  QCOMPARE(one.pixelColor(9, 10), first);

  // The state moves with no invalidation routed: the pixels must not.
  control->setFillQuietly(second);
  const QImage two = painted(host.chrome, &host, 1.0, Qt::black);
  QCOMPARE(two.pixelColor(9, 10), first);

  // The one control's own route, exactly what a real host is handed.
  chromeUpdate(control);
  const QImage three = painted(host.chrome, &host, 1.0, Qt::black);
  QCOMPARE(three.pixelColor(9, 10), second);

  // And the invalidation is all-or-nothing per control: a second paint after it
  // keeps the new pixels without re-rasterising them.
  control->setFillQuietly(first);
  const QImage four = painted(host.chrome, &host, 1.0, Qt::black);
  QCOMPARE(four.pixelColor(9, 10), second);
  host.hide();
}

void TestChromeCache::aDevicePixelRatioChangeReRendersAtTheNewRatio() {
  Host host;
  auto* control = new QuietChrome(&host);
  control->move(4, 5);
  host.chrome.adopt(&host, nullptr);
  QVERIFY(presented(&host));

  const QColor second = QColor(220, 30, 40, 255);
  const QImage one = painted(host.chrome, &host, 1.0, Qt::black);
  QCOMPARE(one.pixelColor(9, 10), QColor(20, 200, 90, 255));

  // A cache rendered at 1.0 cannot be blitted into a 2.0 device: the state
  // moved quietly, so only a re-rasterisation at the new ratio shows it.
  control->setFillQuietly(second);
  const QImage two = painted(host.chrome, &host, 2.0, Qt::black);
  QCOMPARE(two.width(), 80);
  // The control's device rect is x 8..32, y 10..34 at ratio 2.
  QCOMPARE(two.pixelColor(20, 20), second);
  QCOMPARE(two.pixelColor(31, 33), second);
  QCOMPARE(two.pixelColor(7, 9), QColor(Qt::black));
  QCOMPARE(two.pixelColor(32, 34), QColor(Qt::black));
  host.hide();
}

void TestChromeCache::aTranslucentControlLeavesNoBlackFringe() {
  Host host;
  auto* control = new VeilChrome(&host);
  control->move(6, 6);
  host.chrome.adopt(&host, nullptr);
  QVERIFY(presented(&host));

  const QColor bright(255, 255, 255);
  const QColor dark(0, 0, 0);
  const QImage overBright = painted(host.chrome, &host, 1.0, bright);
  const QImage overDark = painted(host.chrome, &host, 1.0, dark);

  // The corner of the control's box is outside the rounded shape, so the frame
  // itself must still be there: a cached image that carried a black backing
  // store would leave a black corner on the bright frame.
  QCOMPARE(overBright.pixelColor(6, 6), bright);
  QCOMPARE(overDark.pixelColor(6, 6), dark);

  // Its centre is the veil composited under the frame it was blitted onto,
  // rather than a colour with the frame already baked in.
  const QColor centre = VeilChrome::veil();
  QVERIFY(near(overBright.pixelColor(13, 13), blendOver(centre, bright)));
  QVERIFY(near(overDark.pixelColor(13, 13), blendOver(centre, dark)));

  // Nothing inside the control's box is darker than the bright frame allows:
  // the whole failure a black fringe names.
  const int floorGreen = blendOver(centre, bright).green();
  for (int y = 6; y < 22; ++y) {
    for (int x = 6; x < 22; ++x) {
      const QColor pixel = overBright.pixelColor(x, y);
      QVERIFY2(pixel.green() >= floorGreen,
               qPrintable(QStringLiteral("black fringe at %1,%2: %3")
                              .arg(x)
                              .arg(y)
                              .arg(pixel.name(QColor::HexArgb))));
    }
  }
  host.hide();
}
