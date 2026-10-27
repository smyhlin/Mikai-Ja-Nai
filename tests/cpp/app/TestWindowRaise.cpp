#include "TestWindowRaise.hpp"

#include "WindowRaisePolicy.hpp"
#include "PipFloat.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QSignalSpy>
#include <QDialog>
#include <QWidget>

#include <QtCore/QEventLoop>
#include <QtGui/QGuiApplication>

namespace {

QWidget* g_shell = nullptr;

/// Drives the queued activation the policy asks for. `show()` posts the raise
/// and the activation, and the platform answers them on the event loop, so the
/// observation has to be taken after a turn rather than immediately.
void pump() {
    for (int turn = 0; turn < 8; ++turn) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
}

QWidget* activeWidget() { return QApplication::activeWindow(); }

// Offscreen/minimal activate every shown window and warn `does not support
// raise()`, so the no-activate half is unobservable there: offscreen proves
// the exemption classification and visibility, a real platform proves the
// activation itself.
bool activationIsObservable() {
    const QString platform = QGuiApplication::platformName();
    return platform != QStringLiteral("offscreen") && platform != QStringLiteral("minimal");
}

}  // namespace

void TestWindowRaise::initTestCase() {
    // The suite owns the shell for the whole run: the policy raises whatever
    // window is shown, so a window left open by a previous test would also be
    // raised and could answer for the one under test.
    QVERIFY2(activeWidget() == nullptr || activeWidget() == g_shell,
             "another test left a window active; the policy is app-wide and this "
             "suite must own the stacking order");
    QVERIFY2(g_shell == nullptr, "initTestCase ran twice");
    g_shell = new QWidget;
    g_shell->setObjectName(QStringLiteral("testShell"));
    g_shell->show();
    mikai::app::WindowRaisePolicy::install();
    pump();
}

void TestWindowRaise::cleanupTestCase() {
    delete g_shell;
    g_shell = nullptr;
    pump();
}

void TestWindowRaise::pipWindowManagerCloseRequestsPlayerTeardown() {
    mplayer::shell::PipFloat pip;
    QSignalSpy closeRequests(&pip, &mplayer::shell::PipFloat::closeRequested);
    QObject::connect(&pip, &mplayer::shell::PipFloat::closeRequested, &pip, [&pip] {
        QCloseEvent reentrant;
        QCoreApplication::sendEvent(&pip, &reentrant);
    });
    QCloseEvent event;
    QCoreApplication::sendEvent(&pip, &event);
    QCOMPARE(closeRequests.count(), 1);
    QVERIFY(!event.isAccepted());
}

void TestWindowRaise::aNewWindowRaisesOverAnExistingOneAndTakesActivation() {
    // The app window is already up and active; the newly shown one must arrive
    // above it and take activation rather than hiding behind the shell.
    QWidget* later = new QWidget;
    later->setObjectName(QStringLiteral("raisedWindow"));
    later->show();

    pump();
    QVERIFY2(later->isVisible(), "the new window was not shown at all");
    QVERIFY2(QApplication::topLevelWidgets().contains(later),
             "the new window is not a top-level window");
    QVERIFY2(activeWidget() == later,
             "a newly shown top-level window did not take activation; the app's "
             "other window kept it instead");
    delete later;
    pump();
}

void TestWindowRaise::aWindowShownLaterStillRaisesOverTheEarlierOne() {
    // Stacking order, not just the first show: two new windows in a row, and the
    // second must be the active one even though the first just took it.
    QWidget* first = new QWidget;
    first->setObjectName(QStringLiteral("firstWindow"));
    first->show();
    pump();
    QCOMPARE(activeWidget(), first);

    QWidget* second = new QWidget;
    second->setObjectName(QStringLiteral("secondWindow"));
    second->show();
    pump();
    QVERIFY2(activeWidget() == second,
             "the window shown last did not take activation back from the one "
             "shown first");

    delete first;
    delete second;
    pump();
}

void TestWindowRaise::theShellWindowDoesNotStealActivationForItsOwnShow() {
    // The app's own shell is a window too, and re-showing a window that is
    // already visible is not a new window: the policy must not yank activation
    // away from a dialog the viewer is working in.
    QWidget* dialog = new QWidget;
    dialog->setObjectName(QStringLiteral("workingDialog"));
    dialog->show();
    pump();
    QCOMPARE(activeWidget(), dialog);

    g_shell->show();
    pump();
    QVERIFY2(activeWidget() == dialog,
             "re-showing the shell took activation away from the window the "
             "viewer was working in");

    delete dialog;
    pump();
}

void TestWindowRaise::aSecondTopLevelShownUnderAnotherDoesNotLeaveTheFirstActive() {
    QWidget* a = new QWidget;
    a->setObjectName(QStringLiteral("leftWindow"));
    a->show();
    pump();

    QWidget* b = new QWidget;
    b->setObjectName(QStringLiteral("rightWindow"));
    b->show();
    pump();

    QVERIFY2(activeWidget() == b,
             "one of two sibling windows stayed active after the other was "
             "shown on top of it");

    delete a;
    delete b;
    pump();
}

void TestWindowRaise::theShowWithoutActivatingAttributeKeepsItsWindowInactive() {
    // What ProcessingOverlay and the parked PiP float rely on: the attribute
    // means the window must be visible without taking activation, and the
    // app-wide policy must not carry it.
    QWidget* owner = new QWidget;
    owner->setObjectName(QStringLiteral("overlayOwner"));
    owner->show();
    pump();
    QCOMPARE(activeWidget(), owner);

    QWidget* overlay = new QWidget(owner);
    overlay->setObjectName(QStringLiteral("processingOverlay"));
    overlay->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    overlay->setAttribute(Qt::WA_ShowWithoutActivating);
    overlay->show();

    pump();
    QVERIFY2(overlay->isVisible(), "an exempt window was not shown");
    QVERIFY(mikai::app::WindowRaisePolicy::isExempt(overlay));
    if (activationIsObservable()) {
        QVERIFY2(activeWidget() == owner,
                 "the policy activated a window that asked not to be activated");
        QVERIFY2(!overlay->isActiveWindow(),
                 "the no-activate window became the active window anyway");
    }

    delete owner;
    pump();
}

void TestWindowRaise::theStayOnTopFlagKeepsItsWindowInactive() {
    QWidget* owner = new QWidget;
    owner->setObjectName(QStringLiteral("topOwner"));
    owner->show();
    pump();
    QCOMPARE(activeWidget(), owner);

    QWidget* top = new QWidget(owner);
    top->setObjectName(QStringLiteral("parkedFloat"));
    top->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    top->setAttribute(Qt::WA_ShowWithoutActivating);
    top->show();

    pump();
    QVERIFY2(top->isVisible(), "a parked always-on-top window was not shown");
    QVERIFY(mikai::app::WindowRaisePolicy::isExempt(top));
    if (activationIsObservable()) {
        QVERIFY2(activeWidget() == owner,
                 "the policy activated an always-on-top window that must stay quiet");
    }

    delete owner;
    pump();
}

void TestWindowRaise::theNoActivatePropertyKeepsItsWindowInactive() {
    // The escape hatch is for a window that cannot carry the attribute.
    QWidget* owner = new QWidget;
    owner->setObjectName(QStringLiteral("propOwner"));
    owner->show();
    pump();
    QCOMPARE(activeWidget(), owner);

    QWidget* quiet = new QWidget(owner);
    quiet->setObjectName(QStringLiteral("quietWindow"));
    quiet->setWindowFlag(Qt::Tool);
    quiet->setProperty("mikaiNoActivateOnShow", true);
    QCOMPARE(mikai::app::WindowRaisePolicy::isExempt(quiet), true);
    quiet->show();

    pump();
    QVERIFY2(quiet->isVisible(), "a property-exempt window was not shown");
    if (activationIsObservable()) {
        QVERIFY2(activeWidget() == owner,
                 "the property exemption did not keep the window inactive");
    }

    delete owner;
    pump();
}

void TestWindowRaise::anExemptWindowStillRaisesWhenAskedToExplicitly() {
    // The exemption only covers the automatic policy: a window that needs focus
    // can still ask for it, and that request must reach the platform.
    QWidget* owner = new QWidget;
    owner->setObjectName(QStringLiteral("explicitOwner"));
    owner->show();
    pump();

    QWidget* overlay = new QWidget(owner);
    overlay->setObjectName(QStringLiteral("askedForFocus"));
    overlay->setWindowFlag(Qt::Tool);
    overlay->setAttribute(Qt::WA_ShowWithoutActivating);
    overlay->show();
    pump();
    if (activationIsObservable()) {
        QCOMPARE(activeWidget(), owner);
    }

    overlay->raise();
    overlay->activateWindow();
    pump();
    if (activationIsObservable()) {
        QVERIFY2(activeWidget() == overlay,
                 "an explicit raise+activate on an exempt window had no effect");
    } else {
        QVERIFY(overlay->isVisible());
    }
    delete owner;
    pump();
}

void TestWindowRaise::aChildWidgetShowDoesNotMoveTheActiveWindow() {
    // A child becoming visible is a layout fact, not a window one: the policy
    // must not raise or activate on behalf of every widget in a list.
    QWidget* window = new QWidget;
    window->setObjectName(QStringLiteral("listWindow"));
    window->show();
    pump();
    QCOMPARE(activeWidget(), window);

    QWidget* child = new QWidget(window);
    child->setObjectName(QStringLiteral("listChild"));
    child->hide();
    child->show();
    pump();
    QVERIFY2(activeWidget() == window,
             "showing a child widget moved the active window");

    delete window;
    pump();
}

void TestWindowRaise::installingThePolicyTwiceRaisesOncePerShow() {
    // Installation is idempotent: a second install must not stack a second
    // filter that processes the same Show twice.
    QWidget* window = new QWidget;
    window->setObjectName(QStringLiteral("idempotentWindow"));
    window->hide();
    mikai::app::WindowRaisePolicy::install();
    window->show();
    mikai::app::WindowRaisePolicy::install();
    pump();
    QCOMPARE(activeWidget(), window);
    QVERIFY(window->isVisible());
    delete window;
    pump();
}

void TestWindowRaise::theExemptionIsVisibleBeforeTheWindowIsShown() {
    // A window can consult the policy before showing, so its own show path does
    // not have to guess what will happen to it.
    QWidget* ordinary = new QWidget;
    ordinary->setObjectName(QStringLiteral("ordinaryWindow"));
    QCOMPARE(mikai::app::WindowRaisePolicy::isExempt(ordinary), false);
    delete ordinary;

    QWidget* quiet = new QWidget;
    quiet->setObjectName(QStringLiteral("consultedQuiet"));
    quiet->setAttribute(Qt::WA_ShowWithoutActivating);
    QCOMPARE(mikai::app::WindowRaisePolicy::isExempt(quiet), true);
    delete quiet;

    QWidget* top = new QWidget;
    top->setObjectName(QStringLiteral("consultedTop"));
    top->setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint);
    QCOMPARE(mikai::app::WindowRaisePolicy::isExempt(top), true);
    delete top;

    QWidget* tagged = new QWidget;
    tagged->setObjectName(QStringLiteral("consultedTagged"));
    tagged->setProperty("mikaiNoActivateOnShow", true);
    QCOMPARE(mikai::app::WindowRaisePolicy::isExempt(tagged), true);
    delete tagged;
    pump();
}

void TestWindowRaise::thePolicyAppliesToADialogShownOnTopOfAWindow() {
    // The real shapes: a modal-less dialog raised over the shell, which is what
    // the settings surface and the cover viewer are.
    QWidget* shell = new QWidget;
    shell->setObjectName(QStringLiteral("dialogOwner"));
    shell->show();
    pump();
    QCOMPARE(activeWidget(), shell);

    QDialog* dialog = new QDialog(shell);
    dialog->setObjectName(QStringLiteral("settingsDialog"));
    dialog->show();
    pump();
    QVERIFY2(dialog->isVisible(), "the dialog was not shown on top of the shell");
    QVERIFY2(activeWidget() == dialog,
             "a dialog shown over an existing app window did not take activation");

    delete dialog;
    pump();
    delete shell;
    pump();
}
