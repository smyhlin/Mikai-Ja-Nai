#include "TestHoverPreview.hpp"

#include <player/shell/FramePreviewService.hpp>
#include <player/shell/HoverPreview.hpp>
#include <player/shell/PlayerShell.hpp>
#include <player/shell/ShellSliders.hpp>

#include "MpvApi.hpp"

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QList>
#include <QMouseEvent>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QString>
#include <QThread>
#include <QWidget>

#include <cmath>

namespace {

using mplayer::shell::FramePreviewService;
using mplayer::shell::HoverPreview;
using mplayer::shell::previewCardRect;
using mplayer::shell::previewFrameFromRaw;
using mplayer::shell::previewReplyMatches;

/// A host the size of a windowed player. The bar sits where the dock puts it.
QRect barIn(const QRect& host) { return QRect(host.left() + 160, host.bottom() - 90, 640, 18); }

/// Four distinct pixels with a row pitch larger than the row itself, which is
/// how mpv pads a capture: RGBA per pixel, then an unused tail per row.
QByteArray paddedCapture() {
    const unsigned char red[4] = {255, 0, 0, 255};
    const unsigned char green[4] = {0, 255, 0, 255};
    const unsigned char blue[4] = {0, 0, 255, 255};
    const unsigned char white[4] = {255, 255, 255, 255};
    const unsigned char pad[4] = {1, 2, 3, 4};
    QByteArray payload;
    payload.reserve(24);
    payload.append(reinterpret_cast<const char*>(red), 4);
    payload.append(reinterpret_cast<const char*>(green), 4);
    payload.append(reinterpret_cast<const char*>(pad), 4);
    payload.append(reinterpret_cast<const char*>(blue), 4);
    payload.append(reinterpret_cast<const char*>(white), 4);
    payload.append(reinterpret_cast<const char*>(pad), 4);
    return payload;
}

/// Answers the question so the caller asserts: a void helper would let a failed
/// QVERIFY continue the body after it.
bool inside(const QRect& card, const QRect& host) { return host.contains(card); }

/// A real decode of a real clip carries picture, not one repeated colour.
bool isUniform(const QImage& image) {
    if (image.isNull()) return true;
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    QSet<QRgb> colours;
    for (int x = 1; x <= 4; ++x) {
        for (int y = 1; y <= 4; ++y) {
            colours.insert(rgba.pixel(rgba.width() * x / 5, rgba.height() * y / 5));
        }
    }
    return colours.size() < 2;
}

// An explicit path, never a search: the tests must not provision a runtime.
mplayer::MpvBundle envBundle() {
    const QString module = qEnvironmentVariable("MIKAI_MPV_MODULE");
    if (module.isEmpty()) return mplayer::MpvBundle{};
    const QFileInfo info(module);
    mplayer::MpvBundle bundle;
    bundle.root = info.absolutePath();
    bundle.modulePath = info.absoluteFilePath();
    bundle.descriptorId = QStringLiteral("env-standard-runtime");
    bundle.protocolVersion = 1;
    return bundle;
}

/// The 12 s colourful clip the suite already ships: two instants on it decode to
/// two different pictures, which is what proves the seek reaches the source.
QString fixtureClip() {
    return QDir::toNativeSeparators(
        QStringLiteral(MIKAI_SOURCE_DIR "/tests/fixtures/media/motion_320x180_24fps_8bit_hevc.mp4"));
}

QString absentClip() {
    return QDir::toNativeSeparators(
        QStringLiteral(MIKAI_SOURCE_DIR "/tests/fixtures/media/absent_on_purpose.mp4"));
}

/// The decoder runs on its own thread; this one only pumps its queue until the
/// signal has been delivered or the deadline passes.
template <typename Predicate>
bool settleUntil(Predicate done, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return done();
}

struct FrameReply {
    quint64 token = 0;
    double seconds = 0.0;
    QImage image;
};

struct MissReply {
    quint64 token = 0;
    double seconds = 0.0;
    QString reason;
};

}  // namespace

void TestHoverPreview::initTestCase() {
    if (mplayer::MpvApi::instance().loaded()) return;
    const mplayer::MpvBundle bundle = envBundle();
    if (bundle.modulePath.isEmpty()) return;
    QString reason;
    if (!mplayer::MpvApi::instance().load(bundle, &reason)) selectionError_ = reason;
}

bool TestHoverPreview::requireRuntime() {
    if (mplayer::MpvApi::instance().loaded()) return true;
    if (!selectionError_.isEmpty()) {
        QTest::qFail(qPrintable(QStringLiteral("cannot select %1: %2")
                                    .arg(envBundle().modulePath, selectionError_)),
                     __FILE__, __LINE__);
        return false;
    }
    QTest::qSkip("set MIKAI_MPV_MODULE to the libmpv-2.dll of a real runtime to run this",
                 __FILE__, __LINE__);
    return false;
}

void TestHoverPreview::theCardStandsAboveTheBarAndInsideTheWindow() {
    const QRect host(0, 0, 1200, 700);
    const QRect bar = barIn(host);
    const QPoint anchor(600, bar.center().y());

    const QRect card = previewCardRect(host, anchor, bar, 16.0 / 9.0);

    QVERIFY(!card.isEmpty());
    QVERIFY2(inside(card, host), "the card must stay inside the host bounds");
    QVERIFY2(card.bottom() < bar.top(), "the card stands above the bar");
    QVERIFY(std::abs(card.center().x() - anchor.x()) <= 1);
    const double aspect = static_cast<double>(card.width()) / card.height();
    QVERIFY2(std::abs(aspect - 16.0 / 9.0) < 0.02, "the card takes the frame's aspect");
}

void TestHoverPreview::aPointerNearAnEdgeKeepsTheCardOnScreen() {
    const QRect host(0, 0, 1200, 700);
    const QRect bar = barIn(host);

    const QRect left = previewCardRect(host, QPoint(0, bar.center().y()), bar, 16.0 / 9.0);
    const QRect right =
        previewCardRect(host, QPoint(host.right(), bar.center().y()), bar, 16.0 / 9.0);

    QVERIFY2(inside(left, host), "the left-anchored card stays inside the window");
    QVERIFY2(inside(right, host), "the right-anchored card stays inside the window");
    QVERIFY2(left.left() > host.left(), "a card at the left edge keeps its margin");
    QVERIFY2(right.right() < host.right(), "a card at the right edge keeps its margin");
}

void TestHoverPreview::aCardWithNoRoomAboveDropsBelowTheBar() {
    const QRect host(0, 0, 800, 320);
    const QRect bar(host.left() + 80, host.top() + 40, 600, 16);
    const QRect card = previewCardRect(host, QPoint(400, bar.center().y()), bar, 16.0 / 9.0);

    QVERIFY2(inside(card, host), "the card stays inside the window");
    QVERIFY2(card.top() > bar.bottom(), "with no room above, the card goes below the bar");
}

void TestHoverPreview::aTallFrameIsFittedByHeightNotCropped() {
    // A portrait source: fitting by width would make it taller than the window.
    const QRect host(0, 0, 1200, 700);
    const QRect bar = barIn(host);
    const QRect card = previewCardRect(host, QPoint(600, bar.center().y()), bar, 9.0 / 16.0);

    QVERIFY2(inside(card, host), "a tall card stays inside the window");
    QVERIFY(card.height() <= host.height() * 55 / 100);
    const double aspect = static_cast<double>(card.width()) / card.height();
    QVERIFY2(std::abs(aspect - 9.0 / 16.0) < 0.03, "the fitted card keeps the frame's aspect");
}

void TestHoverPreview::anImplausibleAspectFallsBackToWidescreen() {
    const QRect host(0, 0, 1200, 700);
    const QRect bar = barIn(host);
    const QPoint anchor(600, bar.center().y());

    const QRect zero = previewCardRect(host, anchor, bar, 0.0);
    const QRect huge = previewCardRect(host, anchor, bar, 90.0);
    const QRect widescreen = previewCardRect(host, anchor, bar, 16.0 / 9.0);

    QCOMPARE(zero, widescreen);
    QCOMPARE(huge, widescreen);
}

void TestHoverPreview::aReplyMustNameTheNewestRequestAndTheSameInstant() {
    QVERIFY(previewReplyMatches(7, 7, 42.0, 42.0));
    // Within one frame time is the same frame: the pointer never lands twice on
    // exactly the same double.
    QVERIFY(previewReplyMatches(7, 7, 42.01, 41.99));
    QVERIFY(!previewReplyMatches(6, 7, 42.0, 42.0));
    // The newest request, but for another instant: the frame of the request
    // before it.
    QVERIFY(!previewReplyMatches(7, 7, 42.0, 50.0));
    // A card that never asked cannot accept a reply.
    QVERIFY(!previewReplyMatches(0, 0, 1.0, 1.0));
    QVERIFY(!previewReplyMatches(7, 7, std::nan(""), 42.0));
}

void TestHoverPreview::aCaptureWithRowPaddingKeepsItsPixels() {
    const QByteArray payload = paddedCapture();
    QString reason;
    const QImage frame = previewFrameFromRaw(QByteArrayLiteral("rgba"), 2, 2, 12, payload.data(),
                                             payload.size(), &reason);

    QVERIFY2(!frame.isNull(), qPrintable(reason));
    QCOMPARE(frame.size(), QSize(2, 2));
    // The padding after each row is dropped, not shifted into the next pixel: a
    // fixed pitch would swap these colours.
    QCOMPARE(frame.pixelColor(0, 0), QColor(255, 0, 0, 255));
    QCOMPARE(frame.pixelColor(1, 0), QColor(0, 255, 0, 255));
    QCOMPARE(frame.pixelColor(0, 1), QColor(0, 0, 255, 255));
    QCOMPARE(frame.pixelColor(1, 1), QColor(255, 255, 255, 255));
}

void TestHoverPreview::aMalformedCaptureIsRefusedWithAReason() {
    const QByteArray payload = paddedCapture();
    QString reason;

    // A channel order nobody asked for would swap red and blue silently.
    QVERIFY(previewFrameFromRaw(QByteArrayLiteral("bgra"), 2, 2, 12, payload.data(),
                                payload.size(), &reason)
                .isNull());
    QVERIFY(!reason.isEmpty());

    // A pitch that does not cover the declared width.
    QVERIFY(previewFrameFromRaw(QByteArrayLiteral("rgba"), 2, 2, 4, payload.data(), payload.size(),
                                &reason)
                .isNull());
    QVERIFY(!reason.isEmpty());

    // A payload shorter than the declared rows is refused instead of read past.
    QVERIFY(previewFrameFromRaw(QByteArrayLiteral("rgba"), 2, 2, 12, payload.data(), 20, &reason)
                .isNull());
    QVERIFY(!reason.isEmpty());

    // A geometry no real preview has must not choose the allocation.
    QVERIFY(previewFrameFromRaw(QByteArrayLiteral("rgba"), 100000, 2, 400000, payload.data(),
                                payload.size(), &reason)
                .isNull());
    QVERIFY(!reason.isEmpty());
    QVERIFY(previewFrameFromRaw(QByteArrayLiteral("rgba"), 0, 2, 12, payload.data(), payload.size(),
                                &reason)
                .isNull());
}

void TestHoverPreview::thePreviewIsPaintedOnTheVideoChrome() {
    QWidget host;
    host.resize(1000, 620);
    host.show();
    QCoreApplication::processEvents();

    HoverPreview card(&host);
    mplayer::shell::ChromeList chrome;
    chrome.adopt(&host, &host);
    const QRect bar(120, 500, 760, 18);
    const QPoint anchor(520, bar.center().y());
    card.present(96.0, host.mapToGlobal(anchor),
                 QRect(host.mapToGlobal(bar.topLeft()), bar.size()), QString(), false);

    const QColor background(10, 220, 20);
    QImage picture(host.size(), QImage::Format_ARGB32_Premultiplied);
    picture.fill(background);
    QPainter painter(&picture);
    chrome.paint(painter, &host, host.rect());
    painter.end();

    QVERIFY(card.isVisible());
    QVERIFY(picture.pixelColor(card.geometry().center()) != background);
}

void TestHoverPreview::thePreviewStaysAboveThePlayerControls() {
    mplayer::shell::PlayerShell shell;
    shell.resize(1000, 620);
    shell.setMedia(QStringLiteral("preview-fixture.mp4"));
    shell.setDuration(100.0);
    shell.show();
    QCoreApplication::processEvents();

    auto* bar = shell.findChild<mplayer::shell::ScrubBar*>(QStringLiteral("playerScrubBar"));
    auto* card = shell.findChild<HoverPreview*>(QStringLiteral("playerHoverPreview"));
    QVERIFY(bar != nullptr && card != nullptr);
    const QPoint point = bar->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(point),
                      QPointF(bar->mapToGlobal(point)), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(bar, &press);

    QVERIFY(card->isVisible());
    const QWidget* dock = bar->parentWidget();
    QVERIFY(dock != nullptr);
    QVERIFY2(card->geometry().bottom() < dock->geometry().top(),
             "the preview must not be covered by the transport controls");
}

void TestHoverPreview::aScrubKeepsTheCardToItsTimestampOnly() {
    QWidget host;
    host.resize(1000, 620);
    host.show();
    QCoreApplication::processEvents();

    HoverPreview card(&host);
    const QRect bar(120, 500, 760, 18);
    const QPoint anchor(520, bar.center().y());

    card.present(96.0, host.mapToGlobal(anchor), QRect(host.mapToGlobal(bar.topLeft()), bar.size()),
                 QStringLiteral("Ендінг"), /*allowFrame=*/false);

    QVERIFY(card.isVisible());
    // A scrub gesture asks for no decode, so no frame may appear on the card.
    QVERIFY(!card.hasFrame());
    QVERIFY2(inside(card.geometry(), host.rect()), "the card stays inside the host");
    QVERIFY2(card.geometry().bottom() < bar.top(), "the card stands above the bar");

    card.dismiss();
    QVERIFY(!card.isVisible());
}

void TestHoverPreview::anUnavailableSourceLeavesNoFrameOnTheCard() {
    QWidget host;
    host.resize(1000, 620);
    host.show();
    QCoreApplication::processEvents();

    HoverPreview card(&host);
    // No source was ever published, so the decoder has nothing to read and the
    // card must say so rather than invent a frame.
    const QRect bar(120, 500, 760, 18);
    const QPoint anchor(520, bar.center().y());

    card.present(12.5, host.mapToGlobal(anchor), QRect(host.mapToGlobal(bar.topLeft()), bar.size()),
                 QString(), /*allowFrame=*/true);
    QCoreApplication::processEvents();

    QVERIFY(card.isVisible());
    QVERIFY2(!card.hasFrame(), "an unavailable source must not leave a frame behind");
    QVERIFY2(!card.statusText().isEmpty(), "the card names why there is no frame");
    QVERIFY2(inside(card.geometry(), host.rect()), "the card stays inside the host");
}

void TestHoverPreview::aHoverCardShowsTheDecodedFrameOfItsInstant() {
    if (!requireRuntime()) return;
    const QString clip = fixtureClip();
    QVERIFY2(QFileInfo::exists(clip), qPrintable(clip));

    QWidget host;
    host.resize(1000, 620);
    host.show();
    QCoreApplication::processEvents();

    HoverPreview card(&host);
    card.setSource(clip);
    const QRect bar(120, 500, 760, 18);
    const QPoint anchor(520, bar.center().y());

    card.present(1.0, host.mapToGlobal(anchor), QRect(host.mapToGlobal(bar.topLeft()), bar.size()),
                 QString(), /*allowFrame=*/true);
    QVERIFY2(settleUntil([&] { return card.hasFrame(); }, 20000),
             "the card never received the frame of the hovered instant");
    QVERIFY2(inside(card.geometry(), host.rect()), "the card stays inside the host");

    // The frame described the source it was decoded from.
    card.setSource(absentClip());
    QVERIFY(!card.hasFrame());
}

void TestHoverPreview::aHoveredInstantIsDecodedAndOnlyTheNewestServes() {
    if (!requireRuntime()) return;
    const QString clip = fixtureClip();
    QVERIFY2(QFileInfo::exists(clip), qPrintable(clip));

    // The replies outlive the service, so the connections never outlive them.
    QList<FrameReply> frames;
    QList<MissReply> misses;
    FramePreviewService service;
    QObject::connect(&service, &FramePreviewService::frameReady, &service,
                     [&frames](quint64 token, double seconds, const QImage& image) {
                         frames.append(FrameReply{token, seconds, image});
                     });
    QObject::connect(&service, &FramePreviewService::frameUnavailable, &service,
                     [&misses](quint64 token, double seconds, const QString& reason) {
                         misses.append(MissReply{token, seconds, reason});
                     });
    const auto replied = [&frames, &misses](quint64 token) {
        for (const FrameReply& reply : frames) {
            if (reply.token == token) return true;
        }
        for (const MissReply& reply : misses) {
            if (reply.token == token) return true;
        }
        return false;
    };
    const auto frameFor = [&frames](quint64 token) {
        for (const FrameReply& reply : frames) {
            if (reply.token == token) return reply;
        }
        return FrameReply{};
    };

    service.setSource(clip);

    // The first hover of a fresh service: the core, its event pump and their
    // timer are created by this request, so a tick that never starts is silence
    // here rather than a slower card.
    service.requestFrame(1, 1.0);
    QVERIFY2(settleUntil([&] { return replied(1); }, 20000),
             "the decoder never answered the first hover");
    QVERIFY2(misses.isEmpty(), qPrintable(misses.value(0).reason));
    const FrameReply first = frameFor(1);
    QVERIFY2(!first.image.isNull(), "the first hover must decode a real frame");
    QVERIFY(std::abs(double(first.image.width()) / first.image.height() - 16.0 / 9.0) < 0.02);
    QCOMPARE(first.seconds, 1.0);
    QVERIFY2(!isUniform(first.image), "a decoded frame is picture, not a blank buffer");

    // A second, distinct instant on the same source: no reload, one seek.
    service.requestFrame(2, 6.0);
    QVERIFY2(settleUntil([&] { return replied(2); }, 20000),
             "the decoder never answered the second hover");
    const FrameReply second = frameFor(2);
    QVERIFY2(!second.image.isNull(), "the second hover must decode a real frame");
    QCOMPARE(second.seconds, 6.0);
    QVERIFY2(second.image != first.image, "1 s and 6 s must not decode to the same picture");

    // Two hovers before the worker can serve either: the first is obsolete and
    // must never surface.
    const qsizetype before = frames.size();
    service.requestFrame(3, 1.0);
    service.requestFrame(4, 6.0);
    QVERIFY2(settleUntil([&] { return replied(4); }, 20000),
             "the newest hover never answered");
    QVERIFY2(!replied(3), "an obsolete request surfaced");
    QCOMPARE(frames.size(), before + 1);
    QCOMPARE(frameFor(4).seconds, 6.0);

    // A hover racing a media change: the frame belongs to the source the card
    // has left, so it must not reach it at all.
    const qsizetype settled = frames.size();
    service.requestFrame(5, 1.0);
    service.setSource(absentClip());
    QVERIFY2(!settleUntil([&] { return frames.size() > settled; }, 2000),
             "a frame of the replaced source survived the media change");
    QVERIFY(!replied(5));

    // After shutdown nothing decodes and nothing is answered.
    service.shutdown();
    service.requestFrame(6, 1.0);
    QVERIFY(!settleUntil([&] { return replied(6); }, 500));
}
