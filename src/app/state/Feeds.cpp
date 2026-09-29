#include "state/Feeds.hpp"

namespace mikai::app::feeds {

const QStringList& all() {
    static const QStringList kAll = {
        QStringLiteral("ongoing"),
        QStringLiteral("popular"),
        QStringLiteral("updates"),
        QStringLiteral("schedule"),
    };
    return kAll;
}

QString label(const QString& key) {
    if (key == QLatin1String("ongoing")) return QStringLiteral("Зараз виходять");
    if (key == QLatin1String("popular")) return QStringLiteral("Популярні на Mikai");
    if (key == QLatin1String("updates")) return QStringLiteral("Останні оновлення");
    if (key == QLatin1String("schedule")) return QStringLiteral("Розклад виходу");
    return {};
}

QSet<QString> enabled(const QString& serialized) {
    // A store that never saw the key means every feed is on.
    if (serialized.isNull()) {
        return QSet<QString>(all().begin(), all().end());
    }
    if (serialized.isEmpty()) {
        return {};
    }
    QSet<QString> result;
    const QStringList parts = serialized.split(QLatin1Char(','));
    for (const QString& part : parts) {
        const QString trimmed = part.trimmed();
        if (all().contains(trimmed)) {
            result.insert(trimmed);
        }
    }
    return result;
}

QString serialize(const QSet<QString>& feeds) {
    QStringList ordered;
    for (const QString& key : all()) {
        if (feeds.contains(key)) {
            ordered << key;
        }
    }
    return ordered.join(QLatin1Char(','));
}

QString settingsKey() { return QStringLiteral("home/feeds"); }

}  // namespace mikai::app::feeds
