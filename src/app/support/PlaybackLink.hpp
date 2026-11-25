#pragma once

#include <QString>

#include <optional>

namespace mikai::app {

/// Versioned deep link for opening a specific title/provider/team/episode/timecode:
///   mikai-janai://play?ref=<ref>&provider=<providerKey>&team=<teamSlug>&episode=<ep>[&t=<sec>]
/// Format v1 has no explicit `v=` query parameter; future breaking changes should
/// add `&v=2` while keeping v1 parsing intact.
struct PlaybackLink {
    QString ref;
    QString provider;  // Normalised provider key (labels::providerKey)
    QString teamSlug;
    QString episode;   // String episode number ("3", "12.5", "OVA")
    std::optional<int> timeSec;

    bool hasPlaybackTarget() const {
        return !provider.isEmpty() && !episode.isEmpty();
    }
};

/// Formats a PlaybackLink into a canonical mikai-janai://play?... URI.
/// Returns an empty string if `ref` is empty or unsafe.
QString formatPlaybackLink(const PlaybackLink& link);

/// Parses and strictly validates a mikai-janai://play?... URI.
/// Returns std::nullopt with a human-readable diagnostic in `error` if invalid.
std::optional<PlaybackLink> parsePlaybackLink(const QString& raw, QString* error = nullptr);

/// True when `raw` begins with the mikai-janai:// scheme (case-insensitive).
bool isPlaybackLinkUrl(const QString& raw);

}  // namespace mikai::app
