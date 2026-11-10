// The Win32 registry/error plumbing in src/gui/Win32Support: RegistryKey RAII
// (open / missing / close, move-only), systemMessage(), wide().
//
// Windows-only suite: the helpers do not exist outside Q_OS_WIN
// (Win32Support.hpp is empty there), so the suite is registered only on
// Windows — see the if(WIN32) block in cmake/MikaiTests.cmake and the
// matching #ifdef Q_OS_WIN guards in tests/cpp/main.cpp. It is the first
// suite to touch Win32Support; the tree had 0 references in tests/ before it.

#pragma once

#include <QtTest/QTest>

class TestWin32Support : public QObject {
  Q_OBJECT

private slots:
  void anOpenedKeyIsValidAndItsMoveEmptiesTheSource();
  void aMissingKeyReportsAsMissing();
  void moveAssignmentEmptiesTheSourceAndClosesTheOldHandle();
  void systemMessageReturnsTextForAKnownError();
  void wideRoundTripsANonAsciiPath();
};
