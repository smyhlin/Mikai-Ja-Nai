// Consumer-protocol regressions for the raw playback diagnostics.
//
// The two parsers are what turn a core's NODE replies into PlaybackDiagnostics,
// so their two failure modes are exactly the two bugs the diagnostics exist to
// avoid: a 64-bit counter read through a double, and a vf chain this build could
// not read being reported as "native filter absent". Every case below builds a
// reply by hand - no core, no surface - so the answer is the parser's alone.

#pragma once

#include <QtTest/QTest>

class TestPlaybackDiagnostics : public QObject {
  Q_OBJECT

 private slots:
  void anIntegerBeyondTwoToTheFiftyThirdIsRetainedExactly();
  void aCounterThatIsUnavailableOrNotAnIntegerIsUnknownNeverZero();
  void anUnreadableVfReplyIsNeverProvenAbsent();
  void anEmptyChainAndAnOrdinaryFilterProveNativeAbsent();
  void theNativeFilterNameOrLabelProvesPresence();
};
