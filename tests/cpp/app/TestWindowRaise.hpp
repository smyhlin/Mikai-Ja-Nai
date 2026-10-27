// The app-wide window stacking policy: a newly shown top-level window arrives
// above the app's other windows and takes activation, while the windows that
// deliberately show without activating keep their behaviour.
//
// The behaviour is asserted on real windows under the real policy rather than on
// a hand-written stub: what regresses in this area is a call site that forgot
// its raise, and only installing the production filter on real widgets shows
// that the policy - not the test - is what moves the window. Both platforms are
// honoured: the tests use QApplication::activeWindow(), which is where the
// request lands, so a window manager that refuses the request is not the thing
// under test, and the no-activate half is checked on the attribute itself.

#pragma once

#include <QtTest/QTest>

class TestWindowRaise : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void cleanupTestCase();

  void aNewWindowRaisesOverAnExistingOneAndTakesActivation();
  void aWindowShownLaterStillRaisesOverTheEarlierOne();
  void theShellWindowDoesNotStealActivationForItsOwnShow();
  void aSecondTopLevelShownUnderAnotherDoesNotLeaveTheFirstActive();
  void theShowWithoutActivatingAttributeKeepsItsWindowInactive();
  void theStayOnTopFlagKeepsItsWindowInactive();
  void theNoActivatePropertyKeepsItsWindowInactive();
  void anExemptWindowStillRaisesWhenAskedToExplicitly();
  void aChildWidgetShowDoesNotMoveTheActiveWindow();
  void installingThePolicyTwiceRaisesOncePerShow();
  void theExemptionIsVisibleBeforeTheWindowIsShown();
  void thePolicyAppliesToADialogShownOnTopOfAWindow();
  void pipWindowManagerCloseRequestsPlayerTeardown();
};
