#include "UpdateCheck.hpp"

#include "HttpTransport.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>
#include <limits>

namespace mikai::backend {
namespace {

constexpr auto kLatestReleaseEndpoint = "https://api.github.com/repos/smyhlin/Mikai-Ja-Nai/releases/latest";
constexpr int kTimeoutMs = 10000;

bool parseVersion(QString value, int (&parts)[3], QString* canonical = nullptr, bool requireV = false) {
    if (value.startsWith(QLatin1Char('v'))) {
        value.remove(0, 1);
    } else if (requireV) {
        return false;
    }
    static const QRegularExpression pattern(QStringLiteral("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$"));
    const auto match = pattern.match(value);
    if (!match.hasMatch()) return false;
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        const qulonglong number = match.captured(i + 1).toULongLong(&ok);
        if (!ok || number > static_cast<qulonglong>(std::numeric_limits<int>::max())) return false;
        parts[i] = static_cast<int>(number);
    }
    if (canonical) *canonical = QStringLiteral("%1.%2.%3").arg(parts[0]).arg(parts[1]).arg(parts[2]);
    return true;
}

bool newer(const int (&a)[3], const int (&b)[3]) {
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

}  // namespace

UpdateCheckResult parseLatestReleaseResponse(const QByteArray& response, const QString& currentVersion) {
    int current[3]{};
    if (!parseVersion(currentVersion, current)) return {};
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(response, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return {};
    const QJsonObject release = document.object();
    if (!release.value(QStringLiteral("draft")).isBool() ||
        !release.value(QStringLiteral("prerelease")).isBool() ||
        !release.value(QStringLiteral("tag_name")).isString() ||
        !release.value(QStringLiteral("html_url")).isString()) return {};
    if (release.value(QStringLiteral("draft")).toBool() || release.value(QStringLiteral("prerelease")).toBool()) return {};

    int latest[3]{};
    QString version;
    if (!parseVersion(release.value(QStringLiteral("tag_name")).toString(), latest, &version, true)) return {};
    const QUrl releaseUrl(release.value(QStringLiteral("html_url")).toString(), QUrl::StrictMode);
    const QString expectedPath = QStringLiteral("/smyhlin/Mikai-Ja-Nai/releases/tag/v%1").arg(version);
    if (!releaseUrl.isValid() || releaseUrl.scheme() != QLatin1String("https") ||
        releaseUrl.host() != QLatin1String("github.com") || releaseUrl.port() != -1 ||
        !releaseUrl.userName().isEmpty() || !releaseUrl.password().isEmpty() ||
        !releaseUrl.query().isEmpty() || !releaseUrl.fragment().isEmpty() ||
        releaseUrl.path() != expectedPath) return {};

    return {newer(latest, current) ? UpdateCheckStatus::UpdateAvailable : UpdateCheckStatus::UpToDate,
            version, releaseUrl};
}

UpdateCheckResult checkForUpdates(const QString& currentVersion) {
    int parsed[3]{};
    if (!parseVersion(currentVersion, parsed)) return {};
    QNetworkRequest request{QUrl(QString::fromLatin1(kLatestReleaseEndpoint))};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mikai-Ja-Nai update checker"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(kTimeoutMs);
    HttpFetchOptions options;
    options.timeoutMs = kTimeoutMs;
    options.maxResponseBytes = 256 * 1024;
    const HttpFetchResult response = runBlockingFetch(transportManagerForThread(), request, options);
    if (response.cancelled || response.timedOut || response.responseTooLarge || response.status != 200) return {};
    return parseLatestReleaseResponse(response.body, currentVersion);
}

}  // namespace mikai::backend
