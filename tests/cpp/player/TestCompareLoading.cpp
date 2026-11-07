#include "TestCompareLoading.hpp"

#include <player/compare/ComparePane.hpp>
#include <player/shell/PlayerShell.hpp>
#include <player/shell/ShellTheme.hpp>
#include <player/shell/ShellWidgets.hpp>

#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QToolButton>
#include <QWidget>

namespace {

QImage renderCenterGlyph(mplayer::shell::CenterGlyph& center) {
    QImage image(center.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    center.paintChrome(painter);
    return image;
}

}  // namespace

void TestCompareLoading::centeredIndicatorRendersAndIsAccessible() {
    mplayer::shell::PlayerShell player;
    player.resize(900, 560);
    player.setMedia(QStringLiteral("comparison-loading-test.mkv"));
    player.setPlaying(false);
    player.show();
    QCoreApplication::processEvents();

    auto* center = player.findChild<mplayer::shell::CenterGlyph*>();
    QVERIFY(center != nullptr);
    QVERIFY(center->isVisible());
    const QImage pausedGlyph = renderCenterGlyph(*center);

    player.setCompareLoading(true);
    QVERIFY(center->isVisible());
    QVERIFY(center->testAttribute(Qt::WA_TransparentForMouseEvents));
    QVERIFY(!center->accessibleName().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(renderCenterGlyph(*center) != pausedGlyph, 1000);

    player.setPlaying(false);
    player.setCompareLoading(false);
    QVERIFY(center->isVisible());
    QVERIFY(center->accessibleName().isEmpty());
    QVERIFY(renderCenterGlyph(*center) == pausedGlyph);
}

void TestCompareLoading::loadingSurvivesHudPlaybackAndMediaChangesThenRestoresLatestGlyph() {
    mplayer::shell::PlayerShell player;
    player.resize(900, 560);
    player.setMedia(QStringLiteral("comparison-loading-test.mkv"));
    player.setPlaying(false);
    player.show();
    QCoreApplication::processEvents();

    auto* center = player.findChild<mplayer::shell::CenterGlyph*>();
    QVERIFY(center != nullptr);
    const QImage pausedGlyph = renderCenterGlyph(*center);
    player.setCompareLoading(true);
    QTRY_VERIFY_WITH_TIMEOUT(renderCenterGlyph(*center) != pausedGlyph, 1000);
    const QImage initialSpinner = renderCenterGlyph(*center);

    player.setPlaying(true);
    QVERIFY(center->isVisible());
    player.setMedia(QStringLiteral("replacement-loading-test.mkv"));
    QVERIFY(center->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(renderCenterGlyph(*center) != initialSpinner, 1000);

    player.enterFullscreen();
    QTRY_VERIFY_WITH_TIMEOUT(
        player.state() == mplayer::shell::PlayerShell::State::FullscreenAutoHidden,
        mplayer::shell::kAutohideMs + 500);
    QVERIFY(center->isVisible());
    const QImage hiddenHudSpinner = renderCenterGlyph(*center);
    QTRY_VERIFY_WITH_TIMEOUT(renderCenterGlyph(*center) != hiddenHudSpinner, 1000);

    player.setCompareLoading(false);
    QVERIFY(!center->isVisible());
    player.setPlaying(false);
    QVERIFY(!center->isVisible());
    player.wake();
    QVERIFY(center->isVisible());
}

void TestCompareLoading::selectorLabelRefitsAfterBadgeResize() {
    mplayer::shell::ComparePane pane;
    pane.resize(70, 160);
    pane.setContent(QStringLiteral("test"), QImage(1, 1, QImage::Format_ARGB32), QStringLiteral("Short"));
    pane.show();
    QCoreApplication::processEvents();

    auto* badge = pane.findChild<QToolButton*>();
    QVERIFY(badge != nullptr);
    const QString fullLabel = QStringLiteral("A · Short  ▾");
    QVERIFY(badge->width() <= pane.width());
    QVERIFY(badge->fontMetrics().horizontalAdvance(badge->text()) <= badge->width());
    QVERIFY(badge->text() != fullLabel);

    pane.resize(500, 160);
    QCoreApplication::processEvents();

    QVERIFY(badge->width() > 70);
    QCOMPARE(badge->text(), fullLabel);
}
