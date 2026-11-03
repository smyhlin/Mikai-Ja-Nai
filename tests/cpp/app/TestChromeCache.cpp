#include "TestChromeCache.hpp"

#include <player/shell/ShellWidgets.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QEvent>
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
using mplayer::shell::chromeRepaintEvent;
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

  bool eventFilter(QObject* watched, QEvent* event) override {
    chromeRepaintEvent(this, watched, event);
    return QWidget::eventFilter(watched, event);
  }

  void chromeDirty(QWidget* widget, const QRect&) override { chrome.invalidate(widget); }
  void chromeDirtyAll() override { chrome.invalidateAll(); }
  /// A move damages the old and new box and keeps the pixels - the tests paint
  /// with an explicit damage rect, so nothing has to be requested here.
  void chromeMoved(QWidget*) override {}

  ChromeList chrome;
};

void QuietChrome::paintChrome(QPainter& painter) { painter.fillRect(rect(), fill_); }
/// Paints on or off colour from the widget's own enabled state: an EnabledChange
/// is a content change that must invalidate, and the image shows whether it did.
class EnabledChrome : public QuietChrome {
 public:
  explicit EnabledChrome(QWidget* parent) : QuietChrome(parent) {}

  void paintChrome(QPainter& painter) override {
    painter.fillRect(rect(), isEnabled() ? QColor(20, 200, 90, 255)
                                         : QColor(200, 40, 40, 255));
  }
};

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

/// The budget fallback is a shipping path, not an off switch: a control too big
/// for the 8 MiB budget is painted directly every pass, and it still has to leave
/// no band behind when it moves. The vacated rect can only come from the list's
/// paint history, because such an entry is never cache-valid.
void TestChromeCache::anOverBudgetControlLeavesNoBandBehindWhenItMoves() {
  // 2049x1024 = 2 098 176 px, just over the 2 097 152 px (8 MiB) budget, so this
  // control can never be cached and every pass rasterises it directly.
  const QSize controlSize(2049, 1024);
  const QColor frame(18, 18, 24, 255);
  const QColor fill(20, 200, 90, 255);
  const int shift = 20;

  auto* root = new QWidget;
  root->setAttribute(Qt::WA_DontShowOnScreen, true);
  root->resize(controlSize.width() + shift, controlSize.height());
  root->show();

  auto* control = new QuietChrome(root);
  control->resize(controlSize);
  control->move(0, 0);
  control->setFillQuietly(fill);
  QVERIFY(presented(control));

  ChromeList chrome;
  chrome.adopt(root, nullptr);

  QImage scratch = painted(chrome, root, 1.0, frame);
  QVERIFY(near(scratch.pixelColor(5, 500), fill));
  QVERIFY(near(scratch.pixelColor(controlSize.width() - 1, 500), fill));

  // Move it, then repaint ONLY the damage the move causes: the box it was
  // painted into, united with where it is now. Nothing else is repainted, so a
  // band left at the old position stays there.
  control->move(shift, 0);
  const QRect vacated = chrome.invalidate(control);
  QVERIFY2(!vacated.isEmpty(),
           "an over-budget control's last painted box is paint history even though "
           "the entry is not cache-valid");
  const QRect damage = vacated.united(QRect(control->pos(), controlSize));
  {
    QPainter painter(&scratch);
    // The frame here is the shell's picture, which paintGL repaints before the
    // chrome: the assertion is that the damage rect tells it to repaint the band
    // the control vacated, not that the chrome draws background itself.
    painter.fillRect(damage, frame);
    chrome.paint(painter, root, damage);
  }

  QVERIFY2(near(scratch.pixelColor(5, 500), frame),
           qPrintable(QStringLiteral("the vacated band is still painted: %1")
                          .arg(scratch.pixelColor(5, 500).name(QColor::HexArgb))));
  QVERIFY(near(scratch.pixelColor(shift + controlSize.width() - 1, 500), fill));

  chrome.invalidateAll();
  delete root;
}

/// A control can keep its area and change its shape (40x20 -> 20x40): reusing the
/// old image because the byte count matched clips everything outside it. The
/// device size, not the area, decides whether the buffer can be repainted in
/// place.
void TestChromeCache::aSameAreaReshapeRepaintsTheNewBox() {
  const QColor frame(18, 18, 24, 255);
  const QColor fill(20, 200, 90, 255);

  auto* root = new QWidget;
  root->setAttribute(Qt::WA_DontShowOnScreen, true);
  root->resize(40, 40);
  root->show();

  auto* control = new QuietChrome(root);
  control->resize(16, 12);  // 192 px
  control->move(0, 0);
  control->setFillQuietly(fill);
  QVERIFY(presented(control));

  ChromeList chrome;
  chrome.adopt(root, nullptr);

  QImage scratch = painted(chrome, root, 1.0, frame);
  QVERIFY(near(scratch.pixelColor(15, 11), fill));

  control->resize(12, 16);  // same area, different shape
  const QRect vacated = chrome.invalidate(control);
  const QRect damage = vacated.united(QRect(control->pos(), control->size()));
  {
    QPainter painter(&scratch);
    painter.fillRect(damage, frame);
    chrome.paint(painter, root, damage);
  }

  QVERIFY2(near(scratch.pixelColor(11, 15), fill),
           qPrintable(QStringLiteral("the taller box was clipped: %1")
                          .arg(scratch.pixelColor(11, 15).name(QColor::HexArgb))));
  QVERIFY(near(scratch.pixelColor(14, 2), frame));

  chrome.invalidateAll();
  delete root;
}

/// A move is not a content change: the pixels are reused and only the two boxes
/// are repainted. The moves are deliberately non-monotonic and diagonal, because
/// a reuse that forgets where it last blitted leaves the intermediate position
/// painted - an island the next damage rect does not cover.
void TestChromeCache::aMoveReusesThePixelsItAlreadyHolds() {
  const QColor frame(18, 18, 24, 255);
  const QColor fill(20, 200, 90, 255);

  auto* root = new QWidget;
  root->setAttribute(Qt::WA_DontShowOnScreen, true);
  root->resize(80, 60);
  root->show();

  auto* control = new QuietChrome(root);
  control->resize(16, 12);
  control->move(0, 0);
  control->setFillQuietly(fill);
  QVERIFY(presented(control));

  ChromeList chrome;
  chrome.adopt(root, nullptr);

  QImage scratch = painted(chrome, root, 1.0, frame);
  QVERIFY(near(scratch.pixelColor(8, 6), fill));

  const auto moveToAndRepaint = [&](const QPoint& target) {
    const QRect damage = chrome.damage(control).united(QRect(target, control->size()));
    control->move(target);
    {
      QPainter painter(&scratch);
      painter.fillRect(damage, frame);
      chrome.paint(painter, root, damage);
    }
    return damage;
  };

  // 0,0 -> 30,20 : the first move is a miss and renders the buffer.
  moveToAndRepaint(QPoint(30, 20));
  QVERIFY(near(scratch.pixelColor(30 + 8, 20 + 6), fill));
  QVERIFY(near(scratch.pixelColor(8, 6), frame));

  // 30,20 -> 0,30 : same size and ratio, so this one is a reuse. Its damage must
  // include 30,20 or the old position survives.
  moveToAndRepaint(QPoint(0, 30));
  QVERIFY2(near(scratch.pixelColor(0 + 8, 30 + 6), fill),
           qPrintable(QStringLiteral("the reused control did not draw at 0,30: %1")
                          .arg(scratch.pixelColor(8, 36).name(QColor::HexArgb))));
  QVERIFY2(near(scratch.pixelColor(30 + 8, 20 + 6), frame),
           qPrintable(QStringLiteral("the intermediate position is still painted: %1")
                          .arg(scratch.pixelColor(38, 26).name(QColor::HexArgb))));

  chrome.invalidateAll();
  delete root;
}

/// An ancestor that clips the control changes the part of it that is visible, so
/// the same widget position is a different cache entry: the widened part has to
/// be rendered, not served from the old clip.
void TestChromeCache::anAncestorClipChangeReRendersTheVisibleRect() {
  const QColor frame(18, 18, 24, 255);
  const QColor fill(20, 200, 90, 255);

  auto* root = new QWidget;
  root->setAttribute(Qt::WA_DontShowOnScreen, true);
  root->resize(60, 40);
  root->show();
  auto* clip = new QWidget(root);
  clip->setAttribute(Qt::WA_DontShowOnScreen, true);
  clip->resize(8, 12);
  clip->move(0, 0);
  clip->show();

  auto* control = new QuietChrome(clip);
  control->resize(16, 12);
  control->move(0, 0);
  control->setFillQuietly(fill);
  QVERIFY(presented(control));

  ChromeList chrome;
  chrome.adopt(root, nullptr);

  QImage scratch = painted(chrome, root, 1.0, frame);
  QVERIFY(near(scratch.pixelColor(6, 6), fill));
  QVERIFY(near(scratch.pixelColor(12, 6), frame));  // clipped away

  clip->resize(16, 12);
  chrome.invalidate(control);
  {
    QPainter painter(&scratch);
    painter.fillRect(QRect(0, 0, 16, 12), frame);
    chrome.paint(painter, root, QRect(0, 0, 16, 12));
  }
  QVERIFY2(near(scratch.pixelColor(12, 6), fill),
           qPrintable(QStringLiteral("the widened clip was served from the old rect: %1")
                          .arg(scratch.pixelColor(12, 6).name(QColor::HexArgb))));

  chrome.invalidateAll();
  delete root;
}

/// After a move, a real content change must still be re-rendered: reuse is keyed
/// on the local rect, not on "nothing happened".
void TestChromeCache::aContentChangeAfterAMoveIsNotServedStale() {
  const QColor frame(18, 18, 24, 255);
  const QColor first(20, 200, 90, 255);
  const QColor second(200, 40, 40, 255);

  auto* root = new QWidget;
  root->setAttribute(Qt::WA_DontShowOnScreen, true);
  root->resize(60, 40);
  root->show();

  auto* control = new QuietChrome(root);
  control->resize(16, 12);
  control->move(0, 0);
  control->setFillQuietly(first);
  QVERIFY(presented(control));

  ChromeList chrome;
  chrome.adopt(root, nullptr);

  QImage scratch = painted(chrome, root, 1.0, frame);
  control->move(20, 0);
  {
    QPainter painter(&scratch);
    const QRect damage =
        chrome.damage(control).united(QRect(control->pos(), control->size()));
    painter.fillRect(damage, frame);
    chrome.paint(painter, root, damage);
  }
  QVERIFY(near(scratch.pixelColor(20 + 8, 6), first));

  control->setFillQuietly(second);
  chrome.invalidate(control);
  {
    QPainter painter(&scratch);
    const QRect damage =
        chrome.damage(control).united(QRect(control->pos(), control->size()));
    painter.fillRect(damage, frame);
    chrome.paint(painter, root, damage);
  }
  QVERIFY2(near(scratch.pixelColor(20 + 8, 6), second),
           qPrintable(QStringLiteral("the changed content was served stale: %1")
                          .arg(scratch.pixelColor(20 + 8, 6).name(QColor::HexArgb))));

  chrome.invalidateAll();
  delete root;
}

/// A content event that arrives through the event filter must invalidate, and it
/// must keep doing so after the control has moved: the move path and the content
/// path are different, and sharing one of them leaves a disabled control looking
/// enabled. Driven by a real QEvent, read from the image.
void TestChromeCache::anEnabledChangeInvalidatesEvenAfterAMove() {
  const QColor frame(18, 18, 24, 255);
  const QColor enabled(20, 200, 90, 255);
  const QColor disabled(200, 40, 40, 255);

  auto* root = new QWidget;
  root->setAttribute(Qt::WA_DontShowOnScreen, true);
  root->resize(60, 40);
  root->show();

  auto* control = new EnabledChrome(root);
  control->resize(16, 12);
  control->move(0, 0);
  control->setAttribute(Qt::WA_DontShowOnScreen, true);
  control->show();
  QVERIFY(control->isVisible());

  ChromeList chrome;
  // The watcher is the host itself, exactly as the shell installs it, so a real
  // widget event reaches chromeRepaintEvent.
  chrome.adopt(root, root);

  QImage scratch = painted(chrome, root, 1.0, frame);
  QVERIFY(near(scratch.pixelColor(8, 6), enabled));

  control->move(20, 0);
  {
    QPainter painter(&scratch);
    const QRect damage =
        chrome.damage(control).united(QRect(control->pos(), control->size()));
    painter.fillRect(damage, frame);
    chrome.paint(painter, root, damage);
  }
  QVERIFY(near(scratch.pixelColor(28, 6), enabled));

  control->setEnabled(false);  // EnabledChange through the filter
  {
    QPainter painter(&scratch);
    const QRect damage =
        chrome.damage(control).united(QRect(control->pos(), control->size()));
    painter.fillRect(damage, frame);
    chrome.paint(painter, root, damage);
  }
  QVERIFY2(near(scratch.pixelColor(28, 6), disabled),
           qPrintable(QStringLiteral("the disabled control was served its cached enabled "
                                     "pixels: %1")
                          .arg(scratch.pixelColor(28, 6).name(QColor::HexArgb))));

  chrome.invalidateAll();
  delete root;
}
