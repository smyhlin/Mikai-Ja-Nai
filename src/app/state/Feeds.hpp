#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

namespace mikai::app {

/// Which home-page feed blocks are shown.
///
/// Every enabled feed costs API calls on each home load, so a user on a metered
/// or slow connection can switch off the ones they do not read. A disabled
/// block is never requested and never rendered.
namespace feeds {

/// The four home feed blocks, in the order the settings dialog lists them.
const QStringList& all();

/// Ukrainian heading for a feed key, empty when the key is unknown.
QString label(const QString& key);

/// A store that never saw the key means every feed is on.
QSet<QString> enabled(const QString& serialized);

/// Serialize back to the comma-joined form the settings dialog persists,
/// always in `all()` order so the value does not churn between saves.
QString serialize(const QSet<QString>& feeds);

/// QSettings key the selection lives under.
QString settingsKey();

}  // namespace feeds
}  // namespace mikai::app
