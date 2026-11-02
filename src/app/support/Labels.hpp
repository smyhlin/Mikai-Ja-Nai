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

/// Normalised provider identity: lower-cased, trimmed, with spaces, underscores
/// and hyphens removed, so the API key `moon` and the display name `MoonAnime`
/// compare as the same family instead of as two strings.
QString providerKey(const QString& raw);

/// The provider serving a source: its embed URL first, then the API's own
/// provider field, `other` when neither names one.
QString providerFor(const backend::Source& source);

/// Ordering used to choose defaults: 0 for the preferred provider, 1 for Moon,
/// 2 for its `MoonAnime` spelling, 3 for anything else. An empty `preferred`
/// means Moon.
int preferredProviderRank(const QString& raw, const QString& preferred);

/// The provider keys in tab order: the preferred one first, then Moon, then
/// whatever order the caller supplied.
QStringList orderedProviders(const QStringList& providers, const QString& preferred);

/// The release to open by default: the one served by `preferred`, else the
/// first Moon release, else the first release. -1 for an empty list.
int preferredReleaseIndex(const QVector<backend::Release>& releases, const QString& preferred);

/// Persisted `playback/preferredProvider`, normalised; `moon` when unset.
QString preferredProvider();

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
