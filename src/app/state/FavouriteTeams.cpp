#include "app/state/FavouriteTeams.hpp"

#include "app/state/SettingsKeys.hpp"
#include "app/support/Labels.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace mikai::app {
namespace {

QJsonObject loadAll(const QString& key) {
    QSettings settings;
    const QByteArray raw = settings.value(key).toByteArray();
    if (raw.isEmpty()) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    return doc.isObject() ? doc.object() : QJsonObject{};
}

void saveAll(const QString& key, const QJsonObject& obj) {
    QSettings settings;
    settings.setValue(key, QJsonDocument(obj).toJson(QJsonDocument::Compact));
    settings.sync();
}

}  // namespace

FavouriteTeams::FavouriteTeams(QString settingsKey)
    : key_(settingsKey.isEmpty() ? QString::fromUtf8(kFavouriteTeamsKey) : std::move(settingsKey)) {}

std::optional<FavouriteTeam> FavouriteTeams::favourite(const QString& ref) const {
    const QString trimmedRef = ref.trimmed();
    if (trimmedRef.isEmpty()) return std::nullopt;
    const QJsonObject root = loadAll(key_);
    const QJsonValue val = root.value(trimmedRef);
    if (!val.isObject()) return std::nullopt;
    const QJsonObject entry = val.toObject();
    const QString provider = labels::providerKey(entry.value(QStringLiteral("provider")).toString());
    const QString team = entry.value(QStringLiteral("team")).toString().trimmed();
    if (provider.isEmpty() || team.isEmpty()) return std::nullopt;
    return FavouriteTeam{provider, team, entry.value(QStringLiteral("release")).toString().trimmed()};
}

bool FavouriteTeams::isFavourite(const QString& ref, const QString& provider,
                                 const QString& teamSlug) const {
    const auto fav = favourite(ref);
    if (!fav.has_value()) return false;
    const QString wantedProv = labels::providerKey(provider);
    const auto isMoon = [](const QString& k) {
        return k == QLatin1String("moon") || k == QLatin1String("moonanime");
    };
    const bool provMatch =
        fav->provider == wantedProv || (isMoon(fav->provider) && isMoon(wantedProv));
    return provMatch &&
           fav->teamSlug.compare(teamSlug.trimmed(), Qt::CaseInsensitive) == 0;
}

void FavouriteTeams::setFavourite(const QString& ref, const QString& provider,
                                  const QString& teamSlug, const QString& releaseId) {
    const QString trimmedRef = ref.trimmed();
    const QString normProv = labels::providerKey(provider);
    const QString trimmedTeam = teamSlug.trimmed();
    if (trimmedRef.isEmpty() || normProv.isEmpty() || trimmedTeam.isEmpty()) return;

    QJsonObject root = loadAll(key_);
    QJsonObject entry;
    entry.insert(QStringLiteral("provider"), normProv);
    entry.insert(QStringLiteral("team"), trimmedTeam);
    if (!releaseId.trimmed().isEmpty()) {
        entry.insert(QStringLiteral("release"), releaseId.trimmed());
    }
    root.insert(trimmedRef, entry);
    saveAll(key_, root);
}

void FavouriteTeams::clearFavourite(const QString& ref) {
    const QString trimmedRef = ref.trimmed();
    if (trimmedRef.isEmpty()) return;
    QJsonObject root = loadAll(key_);
    if (!root.contains(trimmedRef)) return;
    root.remove(trimmedRef);
    saveAll(key_, root);
}

}  // namespace mikai::app
