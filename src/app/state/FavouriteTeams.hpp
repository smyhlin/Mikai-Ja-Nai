#pragma once

#include <QString>

#include <optional>

namespace mikai::app {

struct FavouriteTeam {
    QString provider;   // Normalised via labels::providerKey()
    QString teamSlug;   // Release-local team slug
    QString releaseId;  // Diagnostic hint only; not part of lookup key
};

/// Persists per-title favourite (provider, teamSlug) under QSettings key
/// `title/favouriteTeams` (SettingsKeys.hpp).
class FavouriteTeams {
public:
    explicit FavouriteTeams(QString settingsKey = {});

    std::optional<FavouriteTeam> favourite(const QString& ref) const;
    bool isFavourite(const QString& ref, const QString& provider, const QString& teamSlug) const;
    void setFavourite(const QString& ref, const QString& provider, const QString& teamSlug,
                      const QString& releaseId = {});
    void clearFavourite(const QString& ref);

private:
    QString key_;
};

}  // namespace mikai::app
