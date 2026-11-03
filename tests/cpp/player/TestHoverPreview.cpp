#include "TestHoverPreview.hpp"

#include <player/shell/FramePreviewService.hpp>
#include <player/shell/HoverPreview.hpp>
#include <player/shell/OpeningOverlay.hpp>
#include <player/shell/PlayerShell.hpp>
#include <player/shell/ShellSliders.hpp>
#include <player/shell/ShellTheme.hpp>

#include "MpvApi.hpp"

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
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
using mplayer::shell::FramePreviewStats;
using mplayer::shell::hoverCardKeepsPointer;
using mplayer::shell::HoverPreview;
using mplayer::shell::PreviewFrame;
using mplayer::shell::PreviewFrameCache;
using mplayer::shell::previewCardRect;
using mplayer::shell::previewFrameFromRaw;
using mplayer::shell::previewReplyMatches;

/// A 2x2 picture, distinct per call, so a stored frame can be told from another.
QImage storedImage(int shade) {
    QImage image(2, 2, QImage::Format_RGBA8888);
    image.fill(QColor(shade % 256, (shade * 7) % 256, (shade * 13) % 256));
    return image;
}

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

/// Pixels that read as a bright line: the headline is white on the card's dark
/// panel, so a pixel that is both opaque enough and bright enough is a letter
/// (or its antialiased edge), and the dimmer detail line is not.
int brightInk(const QImage& image) {
    if (image.isNull()) return 0;
    const QImage rgba = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int ink = 0;
    for (int y = 0; y < rgba.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(rgba.constScanLine(y));
        for (int x = 0; x < rgba.width(); ++x) {
            const QRgb pixel = row[x];
            if (qAlpha(pixel) < 128) continue;
            if (qGray(pixel) < 190) continue;
            ++ink;
        }
    }
    return ink;
}

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
    /// The instant the request named.
    double requestedSeconds = 0.0;
    /// The timestamp the decoder reported for `image`.
    double decodedSeconds = -1.0;
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
    card.setSource(clip, 1);
    const QRect bar(120, 500, 760, 18);
    const QPoint anchor(520, bar.center().y());

    card.present(1.0, host.mapToGlobal(anchor), QRect(host.mapToGlobal(bar.topLeft()), bar.size()),
                 QString(), /*allowFrame=*/true);
    QVERIFY2(settleUntil([&] { return card.hasFrame(); }, 20000),
             "the card never received the frame of the hovered instant");
    QVERIFY2(inside(card.geometry(), host.rect()), "the card stays inside the host");

    // The frame described the source it was decoded from. A new epoch is a new
    // source even when the URL is the one the card already holds.
    card.setSource(absentClip(), 2);
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
                     [&frames](quint64 token, double requestedSeconds, double decodedSeconds,
                               const QImage& image) {
                         frames.append(FrameReply{token, requestedSeconds, decodedSeconds, image});
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

    service.setSource(clip, 1);

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
    QCOMPARE(first.requestedSeconds, 1.0);
    // The identity is the request's instant; the picture's own timestamp is the
    // decoder's reading of where it landed, which is what a caption may print.
    QVERIFY2(std::isfinite(first.decodedSeconds) && first.decodedSeconds >= 0.0,
             "the reply carries the decoder's own timestamp");
    QVERIFY2(std::abs(first.decodedSeconds - 1.0) < 0.25,
             "an exact seek lands within a frame or two of the asked instant");
    QVERIFY2(!isUniform(first.image), "a decoded frame is picture, not a blank buffer");

    // A second, distinct instant on the same source: no reload, one seek.
    service.requestFrame(2, 6.0);
    QVERIFY2(settleUntil([&] { return replied(2); }, 20000),
             "the decoder never answered the second hover");
    const FrameReply second = frameFor(2);
    QVERIFY2(!second.image.isNull(), "the second hover must decode a real frame");
    QCOMPARE(second.requestedSeconds, 6.0);
    QVERIFY2(second.image != first.image, "1 s and 6 s must not decode to the same picture");

    // Two hovers of instants the store cannot answer, issued before the worker
    // can serve either: the first is obsolete and must never surface.
    const qsizetype before = frames.size();
    service.requestFrame(3, 2.0);
    service.requestFrame(4, 4.0);
    QVERIFY2(settleUntil([&] { return replied(4); }, 20000),
             "the newest hover never answered");
    QVERIFY2(!replied(3), "an obsolete request surfaced");
    QCOMPARE(frames.size(), before + 1);
    QCOMPARE(frameFor(4).requestedSeconds, 4.0);

    // A hover racing a media change: the frame belongs to the source the card
    // has left, so it must not reach it at all. The instant is one the store
    // cannot answer - 1.0 was decoded by the first hover and would be published
    // inside the request itself, before the media change exists.
    const qsizetype settled = frames.size();
    service.requestFrame(5, 9.0);
    service.setSource(absentClip(), 2);
    QVERIFY2(!settleUntil([&] { return frames.size() > settled; }, 2000),
             "a frame of the replaced source survived the media change");
    QVERIFY(!replied(5));

    // After shutdown nothing decodes and nothing is answered.
    service.shutdown();
    service.requestFrame(6, 1.0);
    QVERIFY(!settleUntil([&] { return replied(6); }, 500));
}

void TestHoverPreview::theStoreKeepsSixtyFourInstantsAndEvictsTheOldest() {
    PreviewFrameCache store;
    for (int index = 0; index < PreviewFrameCache::kCapacity; ++index) {
        store.insert(1, PreviewFrame{double(index) * 0.5, double(index) * 0.5,
                                     storedImage(index)});
    }
    QCOMPARE(store.size(), PreviewFrameCache::kCapacity);
    QCOMPARE(store.evictions(), quint64(0));

    // Use the oldest entry: it is now the newest, so it must survive the pass
    // that drops its neighbours.
    QVERIFY(store.lookup(1, 0.0).has_value());

    store.insert(1, PreviewFrame{0.5 * PreviewFrameCache::kCapacity,
                                 0.5 * PreviewFrameCache::kCapacity,
                                 storedImage(PreviewFrameCache::kCapacity)});
    QCOMPARE(store.size(), PreviewFrameCache::kCapacity);
    QCOMPARE(store.evictions(), quint64(1));
    QVERIFY2(store.lookup(1, 0.0).has_value(), "a used entry was evicted before an idle one");
    QVERIFY2(!store.lookup(1, 0.5).has_value(), "the least recently used instant is the one dropped");
    QVERIFY(store.lookup(1, 0.5 * PreviewFrameCache::kCapacity).has_value());

    // Sixty-four is the bound: a longer sweep keeps the newest work and drops the
    // oldest at the same rate.
    for (int index = 0; index < 10; ++index) {
        store.insert(1, PreviewFrame{100.0 + index, 100.0 + index, storedImage(index)});
    }
    QCOMPARE(store.size(), PreviewFrameCache::kCapacity);
    QCOMPARE(store.evictions(), quint64(11));
}

void TestHoverPreview::aStoredInstantIsBucketedByHalfSecondsAndBySourceEpoch() {
    QCOMPARE(PreviewFrameCache::bucketOf(12.0), qint64(24));
    QCOMPARE(PreviewFrameCache::bucketOf(12.2), qint64(24));
    QCOMPARE(PreviewFrameCache::bucketOf(12.3), qint64(25));
    QCOMPARE(PreviewFrameCache::bucketOf(-1.0), qint64(-1));

    PreviewFrameCache store;
    store.insert(7, PreviewFrame{12.0, 12.041, storedImage(1)});
    const auto sameBucket = store.lookup(7, 12.2);
    QVERIFY2(sameBucket.has_value(), "a hover inside the decoded half second is the same frame");
    QCOMPARE(sameBucket->decodedSeconds, 12.041);
    QVERIFY2(!store.lookup(8, 12.0).has_value(),
             "another source epoch must not inherit a frame keyed to the previous one");
    QVERIFY(!store.lookup(7, 12.6).has_value());

    // A store with no image is not a frame a card could show.
    store.insert(7, PreviewFrame{13.0, 13.0, QImage()});
    QVERIFY(!store.lookup(7, 13.0).has_value());
}

void TestHoverPreview::aRepeatedInstantIsServedWithoutAskingTheDecoder() {
    if (!requireRuntime()) return;
    const QString clip = fixtureClip();
    QVERIFY2(QFileInfo::exists(clip), qPrintable(clip));

    QList<FrameReply> frames;
    FramePreviewService service;
    QObject::connect(&service, &FramePreviewService::frameReady, &service,
                     [&frames](quint64 token, double requestedSeconds, double decodedSeconds,
                               const QImage& image) {
                         frames.append(FrameReply{token, requestedSeconds, decodedSeconds, image});
                     });
    const auto replyFor = [&frames](quint64 token) {
        for (const FrameReply& reply : frames) {
            if (reply.token == token) return reply;
        }
        return FrameReply{};
    };

    service.setSource(clip, 1);
    service.requestFrame(1, 3.0);
    QVERIFY2(settleUntil([&] { return replyFor(1).token == 1; }, 20000),
             "the decoder never answered the first hover");
    const FramePreviewStats afterMiss = service.stats();
    QCOMPARE(afterMiss.workerRequests, quint64(1));
    QCOMPARE(afterMiss.cacheHits, quint64(0));

    // The same instant again: answered from the store, inside the call, with no
    // worker round trip and no seek.
    QElapsedTimer clock;
    clock.start();
    service.requestFrame(2, 3.0);
    const qint64 servedUs = clock.nsecsElapsed() / 1000;
    const FramePreviewStats afterHit = service.stats();
    QCOMPARE(replyFor(2).token, quint64(2));
    QCOMPARE(afterHit.cacheHits, quint64(1));
    QCOMPARE(afterHit.workerRequests, afterMiss.workerRequests);
    QVERIFY2(afterHit.lastHitLatencyUs >= 0 && afterHit.lastHitLatencyUs < 5000,
             "a cache hit is a lookup and a signal, not a decode");
    QVERIFY2(servedUs < 5000, "the repeated instant was answered without an event loop turn");
    // The stored picture keeps the timestamp it was decoded at, not the instant
    // the pointer happens to name now.
    QCOMPARE(replyFor(2).decodedSeconds, replyFor(1).decodedSeconds);
    QCOMPARE(replyFor(2).image, replyFor(1).image);

    service.shutdown();
}

void TestHoverPreview::aFinishedDecodeIsNeverThrownAwayWhenThePointerMovesOn() {
    if (!requireRuntime()) return;
    const QString clip = fixtureClip();
    QVERIFY2(QFileInfo::exists(clip), qPrintable(clip));

    QList<FrameReply> frames;
    FramePreviewService service;
    QObject::connect(&service, &FramePreviewService::frameReady, &service,
                     [&frames](quint64 token, double requestedSeconds, double decodedSeconds,
                               const QImage& image) {
                         frames.append(FrameReply{token, requestedSeconds, decodedSeconds, image});
                     });
    const auto replyFor = [&frames](quint64 token) {
        for (const FrameReply& reply : frames) {
            if (reply.token == token) return reply;
        }
        return FrameReply{};
    };

    service.setSource(clip, 1);
    service.requestFrame(1, 1.0);
    QVERIFY2(settleUntil([&] { return replyFor(1).token == 1; }, 20000),
             "the decoder never answered the first hover");

    // A miss leaves the worker, and a return to an instant already stored is
    // answered inside the call - so token 2 necessarily finishes after the
    // pointer has moved on to token 3.
    service.requestFrame(2, 8.0);
    service.requestFrame(3, 1.0);
    QCOMPARE(replyFor(3).token, quint64(3));
    QVERIFY2(replyFor(2).token == 0, "the swept instant had already finished");
    QVERIFY2(settleUntil([&] { return replyFor(2).token == 2; }, 20000),
             "a decode that finished after the pointer moved on was thrown away");
    QVERIFY2(!replyFor(2).image.isNull(), "the late reply carries a real frame");

    // And it is stored under its own instant, so returning to it is a hit.
    const FramePreviewStats before = service.stats();
    service.requestFrame(4, 8.0);
    QCOMPARE(replyFor(4).token, quint64(4));
    QCOMPARE(service.stats().cacheHits, before.cacheHits + 1);

    service.shutdown();
}

void TestHoverPreview::theCardKeepsAnOlderPictureAsExplicitlyStale() {
    if (!requireRuntime()) return;
    const QString clip = fixtureClip();
    QVERIFY2(QFileInfo::exists(clip), qPrintable(clip));

    QWidget host;
    host.resize(1000, 620);
    host.show();
    QCoreApplication::processEvents();

    HoverPreview card(&host);
    card.setSource(clip, 1);
    const QRect bar(120, 500, 760, 18);
    const QPoint anchor(520, bar.center().y());
    const QRect barGlobal(host.mapToGlobal(bar.topLeft()), bar.size());

    card.present(1.0, host.mapToGlobal(anchor), barGlobal, QString(), /*allowFrame=*/true);
    QVERIFY2(settleUntil([&] { return card.hasFrame() && card.statusText().isEmpty(); }, 20000),
             "the card never settled on the first instant");
    const double pictureTime = card.frameSeconds();
    QVERIFY2(std::abs(pictureTime - 1.0) < 0.25, "the card prints the picture's own instant");

    // The pointer jumps far away. The previous picture must stay, marked stale,
    // and must keep its own timestamp rather than borrowing the new one.
    card.present(9.0, host.mapToGlobal(anchor), barGlobal, QString(), /*allowFrame=*/true);
    QVERIFY2(card.hasFrame(), "the card dropped its picture for a grey void");
    QVERIFY2(card.isFrameStale(), "a picture of another instant must be marked stale");
    QVERIFY2(!card.statusText().isEmpty(), "a stale card says what it is waiting for");
    QCOMPARE(card.frameSeconds(), pictureTime);
    QVERIFY2(std::abs(card.frameSeconds() - 9.0) > 1.0,
             "the stale picture must not be relabelled as the pointer's instant");

    QVERIFY2(settleUntil([&] { return !card.isFrameStale(); }, 20000),
             "the card never received the frame of the new instant");
    QVERIFY2(std::abs(card.frameSeconds() - 9.0) < 0.3, "the new picture names its own instant");

    card.dismiss();
}

void TestHoverPreview::aDecodingCardNamesTheWaitAndARefusalStaysNamed() {
    if (!requireRuntime()) return;
    const QString clip = fixtureClip();
    QVERIFY2(QFileInfo::exists(clip), qPrintable(clip));

    QWidget host;
    host.resize(1000, 620);
    host.show();
    QCoreApplication::processEvents();

    HoverPreview card(&host);
    card.setSource(clip, 1);
    const QRect bar(120, 500, 760, 18);
    const QPoint anchor(520, bar.center().y());

    // Asked for an instant it does not hold yet: no picture, and the wait is
    // stated instead of being left as an empty card.
    card.present(4.0, host.mapToGlobal(anchor), QRect(host.mapToGlobal(bar.topLeft()), bar.size()),
                 QString(), /*allowFrame=*/true);
    QVERIFY(!card.hasFrame());
    QVERIFY2(!card.statusText().isEmpty(), "a decoding card names what it is waiting for");
    QVERIFY2(card.statusDetail().isEmpty(), "a wait is not a refusal and carries no reason");

    QVERIFY2(settleUntil([&] { return card.hasFrame(); }, 20000), "the card never decoded");

    // A source that can never answer: the refusal is named, once, and no frame
    // is left behind claiming to be the instant.
    card.setSource(absentClip(), 2);
    card.present(4.0, host.mapToGlobal(anchor), QRect(host.mapToGlobal(bar.topLeft()), bar.size()),
                 QString(), /*allowFrame=*/true);
    // The refusal, not the wait: a card with no picture already names its wait,
    // so the state to wait for is the one that carries the decoder's own reason.
    QVERIFY2(settleUntil([&] { return !card.statusDetail().isEmpty(); }, 20000),
             "a refused preview must say so instead of staying blank");
    QVERIFY2(!card.statusText().isEmpty(), "the refusal is a named state, not a blank card");
    QVERIFY2(!card.hasFrame(), "a refusal leaves no picture behind as if it were the instant");
    QVERIFY2(!card.isFrameStale(), "a refusal is not a stale picture");
    card.dismiss();
}

void TestHoverPreview::aSameUrlReloadInvalidatesTheStoredFrames() {
    if (!requireRuntime()) return;
    const QString clip = fixtureClip();
    QVERIFY2(QFileInfo::exists(clip), qPrintable(clip));

    QList<FrameReply> frames;
    FramePreviewService service;
    QObject::connect(&service, &FramePreviewService::frameReady, &service,
                     [&frames](quint64 token, double requestedSeconds, double decodedSeconds,
                               const QImage& image) {
                         frames.append(FrameReply{token, requestedSeconds, decodedSeconds, image});
                     });
    service.setSource(clip, 1);
    service.requestFrame(1, 5.0);
    QVERIFY2(settleUntil([&] { return !frames.isEmpty(); }, 20000),
             "the decoder never answered the first hover");
    const quint64 requestsAfterFirst = service.stats().workerRequests;

    // The same URL, a new source epoch: the stored picture describes a source
    // that is no longer loaded and must not answer for this one.
    service.setSource(clip, 2);
    service.requestFrame(2, 5.0);
    QVERIFY2(frames.size() == 1, "a frame of the replaced epoch answered the new request");
    QVERIFY2(settleUntil([&] { return frames.size() == 2; }, 20000),
             "the new epoch never decoded its own frame");
    QCOMPARE(service.stats().cacheHits, quint64(0));
    QCOMPARE(service.stats().workerRequests, requestsAfterFirst + 1);

    // The card drops its picture when the epoch moves, even under one URL.
    QWidget host;
    host.resize(800, 500);
    host.show();
    QCoreApplication::processEvents();
    HoverPreview card(&host);
    card.setSource(clip, 1);
    const QRect bar(120, 400, 560, 18);
    card.present(5.0, host.mapToGlobal(QPoint(400, 400)),
                 QRect(host.mapToGlobal(bar.topLeft()), bar.size()), QString(), true);
    QVERIFY2(settleUntil([&] { return card.hasFrame(); }, 20000), "the card never decoded");
    card.setSource(clip, 2);
    QVERIFY2(!card.hasFrame(), "a same-URL reload kept the previous source's picture");
    card.dismiss();
    service.shutdown();
}

void TestHoverPreview::aPointerBetweenTheBarAndTheCardKeepsIt() {
    const QRect bar(100, 600, 800, 18);
    const QRect card(300, 470, 340, 120);
    QVERIFY2(hoverCardKeepsPointer(bar, card, QPoint(500, 609)),
             "a pointer on the bar belongs to the card the bar opened");
    QVERIFY2(hoverCardKeepsPointer(bar, card, QPoint(500, 530)),
             "a pointer on the card belongs to it");
    QVERIFY2(hoverCardKeepsPointer(bar, card, QPoint(500, 590)),
             "a pointer crossing the gap on its way to the card must not end it");
    QVERIFY(!hoverCardKeepsPointer(bar, card, QPoint(50, 590)));
    QVERIFY(!hoverCardKeepsPointer(bar, card, QPoint(1150, 700)));
    QVERIFY2(!hoverCardKeepsPointer(QRect(), card, QPoint(500, 530)),
             "a hidden bar leaves no band for a pointer to be in");
}

void TestHoverPreview::aParkedTimelineHoverKeepsTheChromeAwake() {
    mplayer::shell::PlayerShell shell;
    shell.resize(1000, 620);
    shell.setMedia(QStringLiteral("parked-hover-fixture.mp4"));
    shell.setDuration(120.0);
    shell.show();
    shell.enterFullscreen();
    QCoreApplication::processEvents();

    auto* bar = shell.findChild<mplayer::shell::ScrubBar*>(QStringLiteral("playerScrubBar"));
    auto* card = shell.findChild<HoverPreview*>(QStringLiteral("playerHoverPreview"));
    QVERIFY(bar != nullptr && card != nullptr);
    // A pointer resting on the bar with no button held: the move is what a
    // viewer's hand produces, and it is consumed by the bar itself.
    QMouseEvent move(QEvent::MouseMove, QPointF(bar->rect().center()),
                     QPointF(bar->mapToGlobal(bar->rect().center())), Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(bar, &move);
    QVERIFY(bar->isHovering());
    QVERIFY(card->isVisible());

    // Longer than kAutohideMs: the chrome under a pointer that never left must
    // not hide and take the card with it.
    for (int waited = 0; waited < 3200; waited += 100) {
        QCoreApplication::processEvents();
        QThread::msleep(100);
    }
    QVERIFY2(shell.stateName() != QLatin1String("FULLSCREEN_AUTO_HIDDEN"),
             "the chrome auto-hid underneath a pointer parked on the timeline");
    QVERIFY2(card->isVisible(), "the hover card vanished under a resting pointer");
}

void TestHoverPreview::aVanishedTimelineEndsTheCardWithoutGrace() {
    mplayer::shell::PlayerShell shell;
    shell.resize(1000, 620);
    shell.setMedia(QStringLiteral("vanished-hover-fixture.mp4"));
    shell.setDuration(120.0);
    shell.show();
    QCoreApplication::processEvents();

    auto* bar = shell.findChild<mplayer::shell::ScrubBar*>(QStringLiteral("playerScrubBar"));
    auto* card = shell.findChild<HoverPreview*>(QStringLiteral("playerHoverPreview"));
    QVERIFY(bar != nullptr && card != nullptr);
    QMouseEvent move(QEvent::MouseMove, QPointF(bar->rect().center()),
                     QPointF(bar->mapToGlobal(bar->rect().center())), Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(bar, &move);
    QVERIFY(card->isVisible());

    // The control the card describes is gone: the card goes with it, not after a
    // grace period that only makes sense for a journey to somewhere.
    bar->hide();
    QCoreApplication::processEvents();
    QVERIFY2(!card->isVisible(), "the card outlived the timeline it belongs to");
}

void TestHoverPreview::aCachedAnswerForTheNewestRequestSettlesTheCard() {
    if (!requireRuntime()) return;
    const QString clip = fixtureClip();
    QVERIFY2(QFileInfo::exists(clip), qPrintable(clip));

    QWidget host;
    host.resize(1000, 620);
    host.show();
    QCoreApplication::processEvents();

    HoverPreview card(&host);
    card.setSource(clip, 1);
    const QRect bar(120, 500, 760, 18);
    const QPoint anchor(520, bar.center().y());
    const QRect barGlobal(host.mapToGlobal(bar.topLeft()), bar.size());

    card.present(1.0, host.mapToGlobal(anchor), barGlobal, QString(), /*allowFrame=*/true);
    QVERIFY2(settleUntil([&] { return card.hasFrame() && card.statusText().isEmpty(); }, 20000),
             "the card never settled on the first instant");
    const double pictureSeconds = card.frameSeconds();

    // One pixel further on the same timeline: still inside the decoded half
    // second, and more than a frame away from the picture's own instant. The
    // store answers this at once, so the answer is this request's answer and the
    // card must stop claiming it is waiting for one.
    card.present(1.2, host.mapToGlobal(anchor), barGlobal, QString(), /*allowFrame=*/true);
    QVERIFY2(card.hasFrame(), "the card lost the picture it already had");
    QVERIFY2(card.statusText().isEmpty(),
             "a card that already holds this bucket's picture still claims to be decoding");
    QVERIFY2(!card.isFrameStale(), "a stored answer is not a stale picture");
    QCOMPARE(card.frameSeconds(), pictureSeconds);
    card.dismiss();
}

void TestHoverPreview::aCardRescuedInTheBandIsDismissedWhenThePointerLeavesIt() {
    mplayer::shell::PlayerShell shell;
    shell.resize(1000, 620);
    shell.setMedia(QStringLiteral("band-exit-fixture.mp4"));
    shell.setDuration(120.0);
    shell.show();
    QCoreApplication::processEvents();

    auto* bar = shell.findChild<mplayer::shell::ScrubBar*>(QStringLiteral("playerScrubBar"));
    auto* card = shell.findChild<HoverPreview*>(QStringLiteral("playerHoverPreview"));
    QVERIFY(bar != nullptr && card != nullptr);

    // A pointer on the bar, the way a viewer reaches the card.
    const QPoint onBar = bar->rect().center();
    QMouseEvent hoverBar(QEvent::MouseMove, QPointF(onBar), QPointF(bar->mapToGlobal(onBar)),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(bar, &hoverBar);
    QVERIFY2(card->isVisible(), "the hover card did not appear for a pointer on the timeline");

    // On the card itself: inside the band the bar and the card span. A rescue
    // here is correct - the journey is not an absence - but it must not be the
    // last word on a pointer that keeps moving.
    const QPoint onCard = card->mapTo(&shell, card->rect().center());
    QMouseEvent hoverCard(QEvent::MouseMove, QPointF(onCard),
                          QPointF(shell.mapToGlobal(onCard)), Qt::NoButton, Qt::NoButton,
                          Qt::NoModifier);
    QCoreApplication::sendEvent(&shell, &hoverCard);
    for (int waited = 0; waited < 400; waited += 50) {
        QCoreApplication::processEvents();
        QThread::msleep(50);
    }
    QVERIFY2(card->isVisible(), "the card did not survive the pointer resting in its band");

    // Away from both: the picture, nowhere near the timeline.
    const QPoint away(8, 8);
    QMouseEvent leaveBand(QEvent::MouseMove, QPointF(away), QPointF(shell.mapToGlobal(away)),
                          Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&shell, &leaveBand);
    for (int waited = 0; waited < 600; waited += 50) {
        QCoreApplication::processEvents();
        QThread::msleep(50);
    }
    QVERIFY2(!card->isVisible(), "the card outlived the pointer leaving the bar-and-card band");
}

void TestHoverPreview::aCacheHitSpendsNoSeekAndADecodeCountsItsOwn() {
    if (!requireRuntime()) return;
    const QString clip = fixtureClip();
    QVERIFY2(QFileInfo::exists(clip), qPrintable(clip));

    QList<FrameReply> frames;
    FramePreviewService service;
    QObject::connect(&service, &FramePreviewService::frameReady, &service,
                     [&frames](quint64 token, double requestedSeconds, double decodedSeconds,
                               const QImage& image) {
                         frames.append(FrameReply{token, requestedSeconds, decodedSeconds, image});
                     });
    service.setSource(clip, 1);
    service.requestFrame(1, 3.0);
    QVERIFY2(settleUntil([&] { return frames.size() == 1; }, 20000),
             "the decoder never answered the first hover");

    // A decoded instant paid for its seeks: at most a keyframe pass and the exact
    // seek that refines it, and at least one of them. A request is not a seek.
    const FramePreviewStats decoded = service.stats();
    QCOMPARE(decoded.workerRequests, quint64(1));
    QVERIFY2(decoded.seeks >= 1, "a decoded instant accepted a seek");
    QVERIFY2(decoded.seeks <= 2, "a far jump is one keyframe pass and one exact seek");

    // The store answers the same instant without spending another one.
    service.requestFrame(2, 3.0);
    QCOMPARE(frames.size(), 2);
    const FramePreviewStats hit = service.stats();
    QCOMPARE(hit.cacheHits, decoded.cacheHits + 1);
    QCOMPARE(hit.seeks, decoded.seeks);
    QVERIFY2(hit.lastHitLatencyUs >= 0 && hit.lastHitLatencyUs < 5000,
             "a stored answer is a lookup and a signal, not a decode");

    service.shutdown();
}

void TestHoverPreview::theOpeningCardPaintsEveryLetterOfItsWords() {
    QWidget host;
    host.resize(1280, 720);
    mplayer::shell::OpeningOverlay overlay(&host);
    overlay.setGeometry(QRect(QPoint(0, 0), host.size()));
    host.show();
    QCoreApplication::processEvents();

    // The wait's own sentences, English and Ukrainian: the card is measured from
    // whatever words it is handed, so both have to reach it whole.
    struct Sentence {
        QString headline;
        QString detail;
    };
    const Sentence sentences[] = {
        {QStringLiteral("Opening stream…"), QStringLiteral("probe opening")},
        {QStringLiteral("Відкриття потоку…"), QStringLiteral("Серія 1 · 720p")},
    };
    const QFont headlineFont = mplayer::shell::uiFont(12, true);
    const QFontMetrics metrics(headlineFont);
    for (const Sentence& sentence : sentences) {
        overlay.showOpening(sentence.headline, sentence.detail);
        QCoreApplication::processEvents();

        // The same words drawn by Qt at their own natural width, in the card's own
        // font and colour: that is the ink the card has to carry. Nothing here
        // knows the card's geometry, so a cut headline shows up as ink that never
        // reached it rather than as a number copied from the layout.
        const int naturalWidth = metrics.horizontalAdvance(sentence.headline) + 8;
        QImage reference(naturalWidth, metrics.height() + 4, QImage::Format_ARGB32_Premultiplied);
        reference.fill(Qt::transparent);
        {
            QPainter painter(&reference);
            painter.setFont(headlineFont);
            painter.setPen(mplayer::shell::white(235));
            painter.drawText(QRect(0, 0, reference.width(), reference.height()),
                             Qt::AlignLeft | Qt::AlignVCenter, sentence.headline);
        }
        const int wanted = brightInk(reference);
        QVERIFY2(wanted > 100, "the reference words carry ink to compare against");

        const QImage card = overlay.grab().toImage();
        QVERIFY(!card.isNull());
        const int painted = brightInk(card);
        QVERIFY2(painted * 10 >= wanted * 8,
                 qPrintable(QStringLiteral("\"%1\" painted %2 of the %3 ink pixels it needs: "
                                           "the card cut the words")
                                .arg(sentence.headline)
                                .arg(painted)
                                .arg(wanted)));
        const QSize cardSize = overlay.sizeHint();
        QVERIFY2(cardSize.width() > 0 && cardSize.height() > 0,
                 "the card reports the size it paints");
        QVERIFY2(cardSize.width() <= host.width(),
                 "the card stays inside the window it stands in");
    }
    overlay.hideOpening();
}

void TestHoverPreview::shellActivityWithoutAHoverStartsNoPreviewDecoder() {
    mplayer::shell::PlayerShell shell;
    shell.resize(1000, 620);
    shell.setMedia(QStringLiteral("activity-without-hover-fixture.mp4"));
    shell.setDuration(120.0);
    shell.show();
    QCoreApplication::processEvents();

    auto* card = shell.findChild<HoverPreview*>(QStringLiteral("playerHoverPreview"));
    QVERIFY(card != nullptr);

    // Playback advancing, a pause and a pointer over the picture: activity a
    // viewer makes before ever touching the timeline. None of it may create the
    // second decoder or queue a frame request - keeping a warm core warm is the
    // only thing activity is allowed to do.
    for (int tick = 0; tick < 8; ++tick) {
        shell.setPosition(double(tick) * 0.5);
        shell.setPlaying(tick % 2 == 0);
        const QPoint spot = shell.rect().center();
        QMouseEvent move(QEvent::MouseMove, QPointF(spot), QPointF(shell.mapToGlobal(spot)),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&shell, &move);
        QCoreApplication::processEvents();
    }

    const FramePreviewStats stats = card->previewStats();
    QCOMPARE(stats.requests, quint64(0));
    QCOMPARE(stats.workerRequests, quint64(0));
    QCOMPARE(stats.seeks, quint64(0));
    QVERIFY(!card->isVisible());
}
