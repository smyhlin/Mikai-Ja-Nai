#include "TestJson.hpp"

#include <backend/Json.hpp>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

#include <cmath>
#include <limits>

using mikai::backend::json::arrayOrEmpty;
using mikai::backend::json::objectOrEmpty;
using mikai::backend::json::optDouble;
using mikai::backend::json::optInt;
using mikai::backend::json::optInt64;
using mikai::backend::json::text;

namespace {

// QCOMPARE cannot print std::optional, so assert through has_value()/value().
void checkInt(const QJsonValue& value, int expected) {
    const std::optional<int> actual = optInt(value);
    QVERIFY(actual.has_value());
    QCOMPARE(*actual, expected);
}

void checkInt64(const QJsonValue& value, qint64 expected) {
    const std::optional<qint64> actual = optInt64(value);
    QVERIFY(actual.has_value());
    QCOMPARE(*actual, expected);
}

void checkDouble(const QJsonValue& value, double expected) {
    const std::optional<double> actual = optDouble(value);
    QVERIFY(actual.has_value());
    QCOMPARE(*actual, expected);
}

}  // namespace

void TestJson::optIntAcceptsInRangeDoubles() {
    checkInt(QJsonValue(0), 0);
    checkInt(QJsonValue(42), 42);
    checkInt(QJsonValue(-7), -7);
    // Truncation, not rounding: the helper is a cast, and the cast is only
    // reached after the range check.
    checkInt(QJsonValue(1.5), 1);
    checkInt(QJsonValue(-1.5), -1);
    checkInt(QJsonValue(std::numeric_limits<int>::max()),
             std::numeric_limits<int>::max());
    checkInt(QJsonValue(std::numeric_limits<int>::min()),
             std::numeric_limits<int>::min());
}

void TestJson::optIntRejectsOutOfRangeDoubles() {
    // P1-07: the §5 case — {"total": 1e30} must fall back, not UB.
    QVERIFY(!optInt(QJsonValue(1e30)).has_value());
    QVERIFY(!optInt(QJsonValue(-1e30)).has_value());
    QVERIFY(!optInt(QJsonValue(2147483648.0)).has_value());
    QVERIFY(!optInt(QJsonValue(-2147483649.0)).has_value());
    QVERIFY(!optInt(QJsonValue(std::numeric_limits<double>::quiet_NaN())).has_value());
    QVERIFY(!optInt(QJsonValue(std::numeric_limits<double>::infinity())).has_value());
    QVERIFY(!optInt(QJsonValue(-std::numeric_limits<double>::infinity())).has_value());
}

void TestJson::optIntParsesNumericStrings() {
    checkInt(QJsonValue(QStringLiteral("42")), 42);
    checkInt(QJsonValue(QStringLiteral("-3")), -3);
    QVERIFY(!optInt(QJsonValue(QStringLiteral("abc"))).has_value());
    QVERIFY(!optInt(QJsonValue(QStringLiteral(""))).has_value());
    // A string outside int range is a parse failure, not a fallback value.
    QVERIFY(!optInt(QJsonValue(QStringLiteral("99999999999999999999"))).has_value());
}

void TestJson::optIntRejectsWrongTypes() {
    QVERIFY(!optInt(QJsonValue(true)).has_value());
    QVERIFY(!optInt(QJsonValue()).has_value());
    QVERIFY(!optInt(QJsonValue(QJsonArray{1})).has_value());
    QVERIFY(!optInt(QJsonValue(QJsonObject{{QStringLiteral("x"), 1}})).has_value());
}

void TestJson::optInt64AcceptsInRangeDoubles() {
    checkInt64(QJsonValue(0), 0);
    checkInt64(QJsonValue(4611686018427387904.0), 4611686018427387904LL);
    checkInt64(QJsonValue(-4611686018427387904.0), -4611686018427387904LL);
}

void TestJson::optInt64RejectsOutOfRangeDoubles() {
    QVERIFY(!optInt64(QJsonValue(1e30)).has_value());
    QVERIFY(!optInt64(QJsonValue(std::numeric_limits<double>::quiet_NaN())).has_value());
    QVERIFY(!optInt64(QJsonValue(std::numeric_limits<double>::infinity())).has_value());
}

void TestJson::optDoubleReadsDoublesAndStrings() {
    checkDouble(QJsonValue(1.5), 1.5);
    checkDouble(QJsonValue(QStringLiteral("2.5")), 2.5);
    QVERIFY(!optDouble(QJsonValue(QStringLiteral("abc"))).has_value());
    QVERIFY(!optDouble(QJsonValue(true)).has_value());
    QVERIFY(!optDouble(QJsonValue()).has_value());
}

void TestJson::textReadsStrings() {
    QCOMPARE(text(QJsonValue(QStringLiteral("abc"))), QStringLiteral("abc"));
    QCOMPARE(text(QJsonValue(QStringLiteral(""))), QStringLiteral(""));
}

void TestJson::textCollapsesMissingAndWrongTypesToEmpty() {
    // A missing key, a null, an object and an array all collapse to "".
    QCOMPARE(text(QJsonValue()), QStringLiteral(""));
    QCOMPARE(text(QJsonValue(QJsonObject{{QStringLiteral("x"), 1}})), QStringLiteral(""));
    QCOMPARE(text(QJsonValue(QJsonArray{1})), QStringLiteral(""));
}

void TestJson::textStringifiesNumbersAndBools() {
    QCOMPARE(text(QJsonValue(1.5)), QStringLiteral("1.5"));
    QCOMPARE(text(QJsonValue(true)), QStringLiteral("true"));
    QCOMPARE(text(QJsonValue(false)), QStringLiteral("false"));
}

void TestJson::objectOrEmptyReturnsObjectOrEmpty() {
    const QJsonObject object{{QStringLiteral("x"), 1}};
    QCOMPARE(objectOrEmpty(QJsonValue(object)), object);
    QVERIFY(objectOrEmpty(QJsonValue(QJsonArray{1})).isEmpty());
    QVERIFY(objectOrEmpty(QJsonValue(QStringLiteral("s"))).isEmpty());
    QVERIFY(objectOrEmpty(QJsonValue()).isEmpty());
}

void TestJson::arrayOrEmptyReturnsArrayOrEmpty() {
    // The §5 case: a non-array value yields an empty array, never a crash.
    const QJsonArray array{1, 2};
    QCOMPARE(arrayOrEmpty(QJsonValue(array)), array);
    QVERIFY(arrayOrEmpty(QJsonValue(QJsonObject{{QStringLiteral("x"), 1}})).isEmpty());
    QVERIFY(arrayOrEmpty(QJsonValue(QStringLiteral("s"))).isEmpty());
    QVERIFY(arrayOrEmpty(QJsonValue(7)).isEmpty());
    QVERIFY(arrayOrEmpty(QJsonValue()).isEmpty());
}
