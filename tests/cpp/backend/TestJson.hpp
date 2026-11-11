// Checked JSON accessors: the backend/Json.hpp helpers.
//
// These exist because the wire is untrusted: a JSON number outside the
// target range makes an unchecked cast undefined behaviour (P1-07), so
// every numeric read goes through a range-checked helper that falls back
// to nullopt with one qWarning instead. The out-of-range cases below
// deliberately emit that warning; it is the expected fallback signal,
// not test noise.

#pragma once

#include <QtTest/QTest>

class TestJson : public QObject {
  Q_OBJECT

 private slots:
  // optInt.
  void optIntAcceptsInRangeDoubles();
  void optIntRejectsOutOfRangeDoubles();
  void optIntParsesNumericStrings();
  void optIntRejectsWrongTypes();

  // optInt64 / optDouble.
  void optInt64AcceptsInRangeDoubles();
  void optInt64RejectsOutOfRangeDoubles();
  void optDoubleReadsDoublesAndStrings();

  // text.
  void textReadsStrings();
  void textCollapsesMissingAndWrongTypesToEmpty();
  void textStringifiesNumbersAndBools();

  // objectOrEmpty / arrayOrEmpty.
  void objectOrEmptyReturnsObjectOrEmpty();
  void arrayOrEmptyReturnsArrayOrEmpty();
};
