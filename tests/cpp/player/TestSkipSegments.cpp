#include "TestSkipSegments.hpp"

#include <player/shell/PlayerShell.hpp>
#include <player/shell/ShellSliders.hpp>
#include <player/shell/ShellTheme.hpp>
#include <player/shell/ShellWidgets.hpp>

#include <QCoreApplication>
#include <QStringList>
#include <QVariantMap>
#include <QtTest/QSignalSpy>

#include <algorithm>
#include <limits>

namespace {
using mplayer::shell::PlayerShell;
using mplayer::shell::ScrubBar;
using mplayer::shell::SkipButton;

QVariantMap segment(const char* type, double start, double end) {
    return {{QStringLiteral("type"), QString::fromLatin1(type)},
            {QStringLiteral("start"), start}, {QStringLiteral("end"), end}};
}

QVector<SkipButton*> visibleOffers(PlayerShell& shell) {
    QVector<SkipButton*> visible;
    for (SkipButton* button : shell.findChildren<SkipButton*>()) {
        if (button->isVisible()) visible.append(button);
    }
    std::sort(visible.begin(), visible.end(), [](const SkipButton* a, const SkipButton* b) {
        return a->y() > b->y();
    });
    return visible;
}

QStringList offerLabels(PlayerShell& shell) {
    QStringList labels;
    for (const SkipButton* button : visibleOffers(shell)) labels.append(button->label());
    return labels;
}

QString skipLabel(const char* type) {
    return QCoreApplication::translate("mplayer::shell::PlayerShell", "Skip %1")
        .arg(ScrubBar::segmentTypeLabel(QString::fromLatin1(type)));
}

void showShell(PlayerShell& shell) {
    shell.resize(1000, 620);
    shell.setMedia(QStringLiteral("skip-segments-test.mkv"));
    shell.show();
    QCoreApplication::processEvents();
}
}  // namespace

void TestSkipSegments::thePositionDecidesWhichOverlappingSegmentsAreOffered() {
    PlayerShell shell;
    showShell(shell);
    shell.setSkipSegments({segment("opening", 10.0, 40.0),
                           segment("mixed_opening", 20.0, 60.0),
                           segment("recap", 30.0, 50.0),
                           segment("ending", 100.0, 120.0)});
    shell.setPosition(5.0);
    QVERIFY(visibleOffers(shell).isEmpty());
    shell.setPosition(15.0);
    QCOMPARE(offerLabels(shell), QStringList{skipLabel("opening")});
    shell.setPosition(35.0);
    QCOMPARE(offerLabels(shell), (QStringList{skipLabel("opening"), skipLabel("mixed_opening"),
                                               skipLabel("recap")}));
    shell.setPosition(50.0);
    QCOMPARE(offerLabels(shell), QStringList{skipLabel("mixed_opening")});
    shell.setPosition(60.0);
    QVERIFY(visibleOffers(shell).isEmpty());
    shell.setPosition(110.0);
    QCOMPARE(offerLabels(shell), QStringList{skipLabel("ending")});
    shell.setPosition(35.0);
    QCOMPARE(visibleOffers(shell).size(), 3);
    shell.setSkipSegments({});
    QVERIFY(visibleOffers(shell).isEmpty());
}

void TestSkipSegments::eachOverlappingOfferIsActionableAndSeeksToItsOwnEnd() {
    PlayerShell shell;
    showShell(shell);
    QSignalSpy seek(&shell, &PlayerShell::seekRequested);
    shell.setSkipSegments({segment("mixed_opening", 20.0, 60.0),
                           segment("recap", 30.0, 50.0)});
    shell.setPosition(35.0);
    const auto offers = visibleOffers(shell);
    QCOMPARE(offers.size(), 2);
    QCOMPARE(offers.at(0)->label(), skipLabel("mixed_opening"));
    QCOMPARE(offers.at(1)->label(), skipLabel("recap"));
    QVERIFY(!offers.at(0)->accessibleName().isEmpty());
    QVERIFY(!offers.at(1)->accessibleName().isEmpty());
    offers.at(1)->click();
    QCOMPARE(seek.count(), 1);
    QCOMPARE(seek.at(0).at(0).toDouble(), 50.0);
    QCOMPARE(offerLabels(shell), QStringList{skipLabel("mixed_opening")});
    visibleOffers(shell).at(0)->click();
    QCOMPARE(seek.count(), 2);
    QCOMPARE(seek.at(1).at(0).toDouble(), 60.0);
    QVERIFY(visibleOffers(shell).isEmpty());
}

void TestSkipSegments::aListThatArrivesLateIsOfferedAtTheCurrentPosition() {
    PlayerShell shell;
    showShell(shell);
    QSignalSpy seek(&shell, &PlayerShell::seekRequested);
    shell.setPosition(35.0);
    QVERIFY(visibleOffers(shell).isEmpty());
    shell.setSkipSegments({segment("recap", 30.0, 50.0)});
    QCOMPARE(offerLabels(shell), QStringList{skipLabel("recap")});
    visibleOffers(shell).at(0)->click();
    QCOMPARE(seek.count(), 1);
    QCOMPARE(seek.at(0).at(0).toDouble(), 50.0);
}

void TestSkipSegments::offersSurviveAnAutoHiddenHudAndStayAboveTheDock() {
    PlayerShell shell;
    showShell(shell);
    shell.setSkipSegments({segment("opening", 10.0, 40.0),
                           segment("mixed_opening", 20.0, 60.0)});
    shell.setPosition(30.0);
    shell.enterFullscreen();
    QTRY_VERIFY_WITH_TIMEOUT(shell.state() == PlayerShell::State::FullscreenAutoHidden,
                             mplayer::shell::kAutohideMs + 500);
    const auto offers = visibleOffers(shell);
    QCOMPARE(offers.size(), 2);
    const int dockWidth = std::min(mplayer::shell::kDockMaxWidth,
                                   shell.width() - 2 * mplayer::shell::kDockHorizontalInset);
    const int dockRight = (shell.width() + dockWidth) / 2;
    const int dockTop = shell.height() - mplayer::shell::kDockBottomInset -
                        mplayer::shell::kDockHeight;
    QVERIFY(offers.at(0)->geometry().bottom() < dockTop);
    QVERIFY(offers.at(0)->geometry().right() <= dockRight);
    QVERIFY(offers.at(1)->geometry().bottom() < offers.at(0)->geometry().top());
    QSignalSpy seek(&shell, &PlayerShell::seekRequested);
    offers.at(0)->click();
    QCOMPARE(seek.count(), 1);
    QCOMPARE(seek.at(0).at(0).toDouble(), 40.0);
    QVERIFY(shell.state() != PlayerShell::State::FullscreenAutoHidden);
}

void TestSkipSegments::aMalformedSegmentIsNotAnOffer() {
    PlayerShell shell;
    showShell(shell);
    shell.setPosition(35.0);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    shell.setSkipSegments({segment("ending", 50.0, 20.0), segment("recap", 30.0, 30.0),
                           segment("opening", nan, 60.0), segment("mixed_opening", 20.0, inf),
                           segment("recap", 30.0, 50.0), segment("recap", 30.0, 50.0)});
    QCOMPARE(offerLabels(shell), QStringList{skipLabel("recap")});
}
