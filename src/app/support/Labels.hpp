#pragma once

#include <backend/Models.hpp>

#include <QString>
#include <QStringList>

namespace mikai::app::labels {

/// A title in the language the viewer chose for content titles. Falls back to
/// the other forms, and to "?" only when the payload carried no title at all.
QString titleName(const backend::Titles& titles);

/// Translated label for the API's raw status enum, empty when unknown.
QString status(const QString& raw);

/// Translated label for a release kind (`voice` / `sub`), raw text when unknown.
QString kind(const QString& raw);

/// Translated label for a stream provider key, capitalised when unknown.
QString provider(const QString& raw);

/// Translated label for a title format, raw text when unknown.
QString format(const QString& raw);

/// Translated label for a relation kind, with a readable fallback.
QString relation(const QString& raw);

/// Translated label for a source medium, raw text when unknown.
QString source(const QString& raw);

/// Translated label for a country code, raw text when unknown.
QString country(const QString& raw);

/// Translated label for an age rating, raw text when unknown.
QString ageRating(const QString& raw);

/// Translated season name plus year for an ISO start date, e.g. "Spring 2024".
/// Empty when the date does not parse.
QString season(const QString& startDate);

/// "1.2M" / "13.5K" / "940" for a score count.
QString compactNumber(qint64 value);

/// A QVariant holding an int, double or string, rendered for display.
/// Null renders empty.
QString variant(const QVariant& value);

}  // namespace mikai::app::labels
