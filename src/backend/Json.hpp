// Shared checked JSON accessors for the backend (P2-01).
//
// These are the helpers that used to live in Models.cpp's anonymous
// namespace: every backend TU that reads API JSON should use them instead
// of bare .toInt()/.toString()/.toBool() defaults or an unchecked
// static_cast<int>(value.toDouble()). The numeric ones are range-checked
// (P1-07): a double outside the target range, NaN or inf falls back to
// nullopt with one qWarning instead of undefined behaviour.
#pragma once

#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <cmath>
#include <limits>
#include <optional>

namespace mikai::backend::json {

// Python's `data.get("x") or ""` collapses missing keys, nulls and wrong
// types into one empty fallback, which is what an empty QString carries here.
inline QString text(const QJsonValue& value) {
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble(), 'g', 17);
    }
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    return {};
}

inline std::optional<int> optInt(const QJsonValue& value) {
    if (value.isDouble()) {
        // P1-07: a double outside int range (or NaN/inf) makes the cast
        // below undefined behaviour, so check first and fall back.
        const double d = value.toDouble();
        if (!std::isfinite(d) || d < static_cast<double>(std::numeric_limits<int>::min()) ||
            d >= static_cast<double>(std::numeric_limits<int>::max()) + 1.0) {
            qWarning("Json: optInt: JSON number out of int range; using default");
            return std::nullopt;
        }
        return static_cast<int>(d);
    }
    if (value.isString()) {
        bool ok = false;
        const int parsed = value.toString().toInt(&ok);
        if (ok) {
            return parsed;
        }
    }
    return std::nullopt;
}

inline std::optional<qint64> optInt64(const QJsonValue& value) {
    if (value.isDouble()) {
        // P1-07: same range check as optInt (see above); a double >= 2^63
        // or < -2^63 cannot be a qint64.
        const double d = value.toDouble();
        if (!std::isfinite(d) || d < static_cast<double>(std::numeric_limits<qint64>::min()) ||
            d >= static_cast<double>(std::numeric_limits<qint64>::max())) {
            qWarning("Json: optInt64: JSON number out of qint64 range; using default");
            return std::nullopt;
        }
        return static_cast<qint64>(d);
    }
    if (value.isString()) {
        bool ok = false;
        const qint64 parsed = value.toString().toLongLong(&ok);
        if (ok) return parsed;
    }
    return std::nullopt;
}

inline std::optional<double> optDouble(const QJsonValue& value) {
    if (value.isDouble()) {
        return value.toDouble();
    }
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().toDouble(&ok);
        if (ok) {
            return parsed;
        }
    }
    return std::nullopt;
}

inline QJsonObject objectOrEmpty(const QJsonValue& value) {
    return value.isObject() ? value.toObject() : QJsonObject{};
}

inline QJsonArray arrayOrEmpty(const QJsonValue& value) {
    return value.isArray() ? value.toArray() : QJsonArray{};
}

}  // namespace mikai::backend::json
