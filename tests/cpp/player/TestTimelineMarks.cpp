#include "TestTimelineMarks.hpp"

#include <player/shell/ShellSliders.hpp>
#include <player/shell/ShellTheme.hpp>

#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPoint>
#include <QStringList>
#include <QVariantMap>
#include <QtTest/QSignalSpy>

namespace {

using mplayer::shell::ScrubBar;

constexpr int kWidth = 200;
constexpr int kHeight = 22;
constexpr double kMargin = 7.0;

QVariantMap segment(const char* type, double start, double end) {
    return QVariantMap{{QStringLiteral("type"), QString::fromLatin1(type)},
                       {QStringLiteral("start"), start},
                       {QStringLiteral("end"), end}};
}

QString named(const char* source) {
    return QCoreApplication::translate("mplayer::shell::ScrubBar", source);
}

int xFor(double seconds, double duration) {
    const double span = kWidth - 2.0 * kMargin;
    const double fraction = duration > 0.0 ? qBound(0.0, seconds / duration, 1.0) : 0.0;
    return qRound(kMargin + span * fraction);
}

constexpr int kBandRow = 6;

QImage paintBar(ScrubBar& bar) {
    bar.resize(kWidth, kHeight);
    QImage image(kWidth, kHeight, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    bar.paintChrome(painter);
    painter.end();
    return image;
}

int alphaAt(const QImage& image, int x) {
    if (x < 0 || x >= image.width()) {
        return -1;
    }
    return qAlpha(image.pixel(x, kBandRow));
}

void sendMouse(ScrubBar& bar, QEvent::Type type, int x, Qt::MouseButton button,
               Qt::MouseButtons buttons) {
    const QPoint local(x, kHeight / 2);
    QMouseEvent event(type, QPointF(local), QPointF(bar.mapToGlobal(local)), button, buttons,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&bar, &event);
}

void moveTo(ScrubBar& bar, int x) {
    sendMouse(bar, QEvent::MouseMove, x, Qt::NoButton, Qt::NoButton);
}

void pressAt(ScrubBar& bar, int x) {
    sendMouse(bar, QEvent::MouseButtonPress, x, Qt::LeftButton, Qt::LeftButton);
}

void leaveBar(ScrubBar& bar) {
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&bar, &leave);
}

}  // namespace

void TestTimelineMarks::theTintFollowsTheSegmentTimesAndTheKnownDuration() {
    ScrubBar bar;
    bar.setDuration(600.0);
    bar.setSkipSegments({segment("opening", 60.0, 90.0)});

    const QImage image = paintBar(bar);

    QVERIFY2(alphaAt(image, xFor(75.0, 600.0)) > 40, "the opening is tinted");
    QCOMPARE(alphaAt(image, xFor(30.0, 600.0)), 0);
    QCOMPARE(alphaAt(image, xFor(200.0, 600.0)), 0);

    const QColor accent = mplayer::shell::accent();
    const QColor painted = image.pixelColor(xFor(75.0, 600.0), kBandRow);
    QVERIFY(qAbs(painted.red() - accent.red()) <= 2);
    QVERIFY(qAbs(painted.green() - accent.green()) <= 2);
    QVERIFY(qAbs(painted.blue() - accent.blue()) <= 2);
}

void TestTimelineMarks::anOverlapIsOneTintAndNotTwo() {
    ScrubBar bar;
    bar.setDuration(600.0);
    bar.setSkipSegments({segment("opening", 60.0, 180.0), segment("ending", 120.0, 240.0)});

    const QImage image = paintBar(bar);

    const int one = alphaAt(image, xFor(90.0, 600.0));
    const int both = alphaAt(image, xFor(150.0, 600.0));
    QVERIFY2(one > 40, "the first range is tinted");
    QCOMPARE(both, one);
    QVERIFY(alphaAt(image, xFor(119.0, 600.0)) > 40);
    QVERIFY(alphaAt(image, xFor(121.0, 600.0)) > 40);
}

void TestTimelineMarks::aRangePastTheKnownEndIsClippedToTheTimeline() {
    ScrubBar clipped;
    clipped.setDuration(100.0);
    clipped.setSkipSegments({segment("ending", 90.0, 200.0)});
    const QImage image = paintBar(clipped);
    QVERIFY(alphaAt(image, xFor(95.0, 100.0)) > 40);
    QVERIFY2(alphaAt(image, xFor(99.0, 100.0)) > 40, "the tint reaches the right edge");

    ScrubBar beyond;
    beyond.setDuration(100.0);
    beyond.setSkipSegments({segment("ending", 140.0, 200.0)});
    const QImage empty = paintBar(beyond);
    QCOMPARE(alphaAt(empty, xFor(99.0, 100.0)), 0);
    QCOMPARE(alphaAt(empty, xFor(50.0, 100.0)), 0);
}

void TestTimelineMarks::aRangeAppearsWhenTheLengthBecomesKnown() {
    ScrubBar bar;
    bar.setDuration(0.0);
    bar.setSkipSegments({segment("opening", 0.0, 90.0)});
    const QImage image = paintBar(bar);
    for (int x = 20; x < kWidth - 20; x += 10) {
        QCOMPARE(alphaAt(image, x), 0);
    }
    bar.setDuration(100.0);
    const QImage measured = paintBar(bar);
    QVERIFY(alphaAt(measured, xFor(45.0, 100.0)) > 40);
}

void TestTimelineMarks::labelsNameEverySegmentUnderTheInstant() {
    ScrubBar bar;
    bar.setDuration(600.0);
    bar.setSkipSegments({segment("opening", 60.0, 90.0), segment("recap", 70.0, 80.0),
                         segment("ending", 300.0, 330.0)});

    QCOMPARE(bar.segmentLabelAt(69.0), named("Opening"));
    QCOMPARE(bar.segmentLabelAt(75.0),
             (QStringList{named("Opening"), named("Recap")}.join(QStringLiteral(", "))));
    QCOMPARE(bar.segmentLabelAt(310.0), named("Ending"));
    QCOMPARE(bar.segmentLabelAt(0.0), QString());
    QCOMPARE(bar.segmentLabelAt(100.0), QString());
}

void TestTimelineMarks::aBoundaryBelongsToTheRangeThatStartsThere() {
    ScrubBar bar;
    bar.setDuration(600.0);
    bar.setSkipSegments({segment("opening", 60.0, 90.0), segment("ending", 90.0, 120.0)});

    QCOMPARE(bar.segmentLabelAt(60.0), named("Opening"));
    QCOMPARE(bar.segmentLabelAt(85.0), named("Opening"));
    QCOMPARE(bar.segmentLabelAt(90.0), named("Ending"));
    QCOMPARE(bar.segmentLabelAt(120.0), QString());
}

void TestTimelineMarks::anUnknownTypeReadsAsTheProvidersOwnWord() {
    QCOMPARE(ScrubBar::segmentTypeLabel(QStringLiteral("mixed_ending")), named("Mixed ending"));
    QCOMPARE(ScrubBar::segmentTypeLabel(QStringLiteral("recap")), named("Recap"));
    QCOMPARE(ScrubBar::segmentTypeLabel(QStringLiteral("bonus_scene")),
             QStringLiteral("Bonus scene"));
    QCOMPARE(ScrubBar::segmentTypeLabel(QString()), named("Segment"));
}

void TestTimelineMarks::theHoverReportsTheTimeUnderThePointer() {
    ScrubBar bar;
    bar.resize(kWidth, kHeight);
    bar.setDuration(600.0);
    QSignalSpy hover(&bar, &ScrubBar::hoverTimeChanged);
    QVERIFY(hover.isValid());

    const QPoint local(xFor(150.0, 600.0), kHeight / 2);
    moveTo(bar, local.x());

    QCOMPARE(hover.count(), 1);
    const QList<QVariant> report = hover.takeFirst();
    QVERIFY(qAbs(report.at(0).toDouble() - 150.0) < 3.0);
    QCOMPARE(report.at(1).toPoint(), bar.mapToGlobal(local));
    QVERIFY(bar.isHovering());
    QVERIFY(qAbs(bar.hoverTime() - 150.0) < 3.0);
}

void TestTimelineMarks::leavingTheBarEndsTheHover() {
    ScrubBar bar;
    bar.resize(kWidth, kHeight);
    bar.setDuration(600.0);
    QSignalSpy hover(&bar, &ScrubBar::hoverTimeChanged);
    QSignalSpy left(&bar, &ScrubBar::hoverLeft);

    moveTo(bar, xFor(150.0, 600.0));
    QCOMPARE(hover.count(), 1);

    leaveBar(bar);

    QCOMPARE(left.count(), 1);
    QVERIFY(!bar.isHovering());
    QCOMPARE(bar.hoverTime(), -1.0);
}

void TestTimelineMarks::hidingTheBarEndsTheHoverToo() {
    ScrubBar bar;
    bar.resize(kWidth, kHeight);
    bar.show();
    QCoreApplication::processEvents();
    bar.setDuration(600.0);
    QSignalSpy left(&bar, &ScrubBar::hoverLeft);

    moveTo(bar, xFor(150.0, 600.0));
    bar.hide();
    QCoreApplication::processEvents();

    QVERIFY2(left.count() >= 1, "hiding the bar ends the hover");
    QVERIFY(!bar.isHovering());
    QCOMPARE(bar.hoverTime(), -1.0);
}

void TestTimelineMarks::noLengthMeansNoTimeButStillAnEnd() {
    ScrubBar bar;
    QSignalSpy hover(&bar, &ScrubBar::hoverTimeChanged);
    QSignalSpy left(&bar, &ScrubBar::hoverLeft);

    moveTo(bar, xFor(150.0, 600.0));

    QCOMPARE(hover.count(), 0);
    QVERIFY(!bar.isHovering());
    QCOMPARE(bar.hoverTime(), -1.0);

    leaveBar(bar);
    QCOMPARE(left.count(), 1);
}

void TestTimelineMarks::aNewLengthReprojectsTheHover() {
    ScrubBar bar;
    bar.resize(kWidth, kHeight);
    bar.setDuration(600.0);
    moveTo(bar, xFor(150.0, 600.0));

    QSignalSpy hover(&bar, &ScrubBar::hoverTimeChanged);
    bar.setDuration(1200.0);

    QCOMPARE(hover.count(), 1);
    QVERIFY(qAbs(hover.at(0).at(0).toDouble() - 300.0) < 6.0);
}

void TestTimelineMarks::aPressStillSeeksAndReportsTheHover() {
    ScrubBar bar;
    bar.resize(kWidth, kHeight);
    bar.setDuration(600.0);
    QSignalSpy values(&bar, &mplayer::shell::RangeSlider::valueChanged);
    QSignalSpy hover(&bar, &ScrubBar::hoverTimeChanged);

    pressAt(bar, xFor(150.0, 600.0));

    QCOMPARE(values.count(), 1);
    QVERIFY(qAbs(values.at(0).at(0).toDouble() - 0.25) < 0.02);
    QCOMPARE(hover.count(), 1);
    QVERIFY(qAbs(hover.at(0).at(0).toDouble() - 150.0) < 3.0);
    QVERIFY(bar.isDragging());

    sendMouse(bar, QEvent::MouseButtonRelease, xFor(150.0, 600.0), Qt::LeftButton, Qt::NoButton);
    QVERIFY(!bar.isDragging());
}
