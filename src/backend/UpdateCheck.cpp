#include "UpdateCheck.hpp"

#include "Downloads.hpp"
#include "HttpTransport.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <limits>

namespace mikai::backend {
namespace {

constexpr auto kLatestReleaseEndpoint = "https://api.github.com/repos/smyhlin/Mikai-Ja-Nai/releases/latest";
constexpr auto kNightlyReleaseEndpoint = "https://api.github.com/repos/smyhlin/Mikai-Ja-Nai/releases/tags/nightly";
constexpr int kTimeoutMs = 10000;

bool isSafeAssetName(const QString& name) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._+-]*$"));
    return pattern.match(name).hasMatch();
}

bool parseVersion(QString value, int (&parts)[4], QString* canonical = nullptr, bool requireV = false) {
    if (value.startsWith(QLatin1Char('v'))) {
        value.remove(0, 1);
    } else if (requireV) {
        return false;
    }
    static const QRegularExpression pattern(
        QStringLiteral("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(?:\\.(0|[1-9][0-9]*))?$"));
    const auto match = pattern.match(value);
    if (!match.hasMatch()) return false;
    const bool hasFourth = !match.captured(4).isEmpty();
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        const qulonglong number = match.captured(i + 1).toULongLong(&ok);
        if (!ok || number > static_cast<qulonglong>(std::numeric_limits<int>::max())) return false;
        parts[i] = static_cast<int>(number);
    }
    if (hasFourth) {
        bool ok = false;
        const qulonglong number = match.captured(4).toULongLong(&ok);
        if (!ok || number > static_cast<qulonglong>(std::numeric_limits<int>::max())) return false;
        parts[3] = static_cast<int>(number);
    } else {
        parts[3] = 0;
    }
    if (canonical) {
        *canonical = hasFourth
                         ? QStringLiteral("%1.%2.%3.%4").arg(parts[0]).arg(parts[1]).arg(parts[2]).arg(parts[3])
                         : QStringLiteral("%1.%2.%3").arg(parts[0]).arg(parts[1]).arg(parts[2]);
    }
    return true;
}

bool newer(const int (&a)[4], const int (&b)[4]) {
    for (int i = 0; i < 4; ++i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

}  // namespace

UpdateCheckResult parseLatestReleaseResponse(const QByteArray& response, const QString& currentVersion) {
    int current[4]{};
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

    int latest[4]{};
    QString version;
    if (!parseVersion(release.value(QStringLiteral("tag_name")).toString(), latest, &version, true)) return {};
    const QUrl releaseUrl(release.value(QStringLiteral("html_url")).toString(), QUrl::StrictMode);
    const QString expectedPath = QStringLiteral("/smyhlin/Mikai-Ja-Nai/releases/tag/v%1").arg(version);
    if (!releaseUrl.isValid() || releaseUrl.scheme() != QLatin1String("https") ||
        releaseUrl.host() != QLatin1String("github.com") || releaseUrl.port() != -1 ||
        !releaseUrl.userName().isEmpty() || !releaseUrl.password().isEmpty() ||
        !releaseUrl.query().isEmpty() || !releaseUrl.fragment().isEmpty() ||
        releaseUrl.path() != expectedPath) return {};

    QVector<UpdateAsset> assets;
    const QJsonArray rawAssets = release.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue& item : rawAssets) {
        const QJsonObject obj = item.toObject();
        const QString name = obj.value(QStringLiteral("name")).toString().trimmed();
        const QUrl downloadUrl(
            obj.value(QStringLiteral("browser_download_url")).toString().trimmed(),
            QUrl::StrictMode);
        const qint64 size = obj.value(QStringLiteral("size")).toVariant().toLongLong();
        if (name.isEmpty() || !downloadUrl.isValid() ||
            downloadUrl.scheme() != QLatin1String("https") ||
            downloadUrl.host() != QLatin1String("github.com") ||
            !downloadUrl.path().startsWith(QStringLiteral("/smyhlin/Mikai-Ja-Nai/releases/download/"))) {
            continue;
        }
        assets.append(UpdateAsset{name, downloadUrl, size});
    }

    UpdateCheckResult out;
    out.status = newer(latest, current) ? UpdateCheckStatus::UpdateAvailable
                                        : UpdateCheckStatus::UpToDate;
    out.version = version;
    out.releaseUrl = releaseUrl;
    out.tagName = release.value(QStringLiteral("tag_name")).toString();
    out.assets = std::move(assets);
    return out;
}

UpdateChannel parseUpdateChannel(const QString& value) {
    return value.trimmed().compare(QLatin1String("nightly"), Qt::CaseInsensitive) == 0
               ? UpdateChannel::Nightly
               : UpdateChannel::Stable;
}

QString updateChannelName(UpdateChannel channel) {
    return channel == UpdateChannel::Nightly ? QStringLiteral("nightly")
                                             : QStringLiteral("stable");
}

UpdateCheckResult parseNightlyReleaseResponse(const QByteArray& response,
                                              const QString& currentBuildRevision,
                                              bool allowChannelSwitch) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(response, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return {};
    const QJsonObject release = document.object();
    if (!release.value(QStringLiteral("draft")).isBool() ||
        !release.value(QStringLiteral("prerelease")).isBool() ||
        !release.value(QStringLiteral("tag_name")).isString() ||
        !release.value(QStringLiteral("html_url")).isString()) {
        return {};
    }
    if (release.value(QStringLiteral("draft")).toBool() ||
        !release.value(QStringLiteral("prerelease")).toBool()) {
        return {};
    }
    const QString tag = release.value(QStringLiteral("tag_name")).toString().trimmed();
    if (tag != QLatin1String("nightly")) return {};

    const QUrl releaseUrl(release.value(QStringLiteral("html_url")).toString(), QUrl::StrictMode);
    if (!releaseUrl.isValid() || releaseUrl.scheme() != QLatin1String("https") ||
        releaseUrl.host() != QLatin1String("github.com") || releaseUrl.port() != -1 ||
        !releaseUrl.userName().isEmpty() || !releaseUrl.password().isEmpty() ||
        !releaseUrl.query().isEmpty() || !releaseUrl.fragment().isEmpty() ||
        releaseUrl.path() != QLatin1String("/smyhlin/Mikai-Ja-Nai/releases/tag/nightly")) {
        return {};
    }

    QVector<UpdateAsset> assets;
    const QJsonArray rawAssets = release.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue& item : rawAssets) {
        const QJsonObject obj = item.toObject();
        const QString name = obj.value(QStringLiteral("name")).toString().trimmed();
        const QUrl downloadUrl(
            obj.value(QStringLiteral("browser_download_url")).toString().trimmed(),
            QUrl::StrictMode);
        const qint64 size = obj.value(QStringLiteral("size")).toVariant().toLongLong();
        if (name.isEmpty() || !downloadUrl.isValid() ||
            downloadUrl.scheme() != QLatin1String("https") ||
            downloadUrl.host() != QLatin1String("github.com") ||
            !downloadUrl.path().startsWith(QStringLiteral("/smyhlin/Mikai-Ja-Nai/releases/download/nightly/"))) {
            continue;
        }
        assets.append(UpdateAsset{name, downloadUrl, size});
    }

    QString commitSha = release.value(QStringLiteral("target_commitish")).toString().trimmed();
    const QString title = release.value(QStringLiteral("name")).toString().trimmed();
    // Prefer the trailing SHA token (e.g. "Nightly 20261121-2cd1d64") so an
    // 8-digit YYYYMMDD date prefix is never mistaken for the commit hash.
    static const QRegularExpression trailingShaPattern(
        QStringLiteral("(?:^|[-_\\s])([0-9a-fA-F]{7,40})$"));
    static const QRegularExpression fallbackShaPattern(QStringLiteral("([0-9a-fA-F]{7,40})"));
    if (commitSha.isEmpty() || commitSha == QLatin1String("main")) {
        auto match = trailingShaPattern.match(title);
        if (!match.hasMatch()) match = fallbackShaPattern.match(title);
        if (match.hasMatch()) commitSha = match.captured(1);
    }
    const QString shortRemote = commitSha.left(7).toLower();
    const QString shortLocal = currentBuildRevision.trimmed().left(7).toLower();
    const bool sameCommit = !shortRemote.isEmpty() && !shortLocal.isEmpty() && shortRemote == shortLocal;

    UpdateCheckResult out;
    out.status = (allowChannelSwitch || !sameCommit) ? UpdateCheckStatus::UpdateAvailable
                                                     : UpdateCheckStatus::UpToDate;
    out.version = title.isEmpty()
                      ? (shortRemote.isEmpty() ? QStringLiteral("nightly")
                                               : QStringLiteral("nightly-%1").arg(shortRemote))
                      : title;
    out.releaseUrl = releaseUrl;
    out.tagName = tag;
    out.assets = std::move(assets);
    out.isPrerelease = true;
    out.commitSha = shortRemote;
    return out;
}

UpdateCheckResult checkForUpdates(const QString& currentVersion, UpdateChannel channel,
                                  const QString& currentBuildRevision, bool allowChannelSwitch) {
    if (channel == UpdateChannel::Stable) {
        int parsed[4]{};
        if (!parseVersion(currentVersion, parsed)) return {};
    }
    const char* endpoint = channel == UpdateChannel::Nightly ? kNightlyReleaseEndpoint
                                                             : kLatestReleaseEndpoint;
    QNetworkRequest request{QUrl(QString::fromLatin1(endpoint))};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mikai-Ja-Nai update checker"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(kTimeoutMs);
    HttpFetchOptions options;
    options.timeoutMs = kTimeoutMs;
    options.maxResponseBytes = 256 * 1024;
    const HttpFetchResult response = runBlockingFetch(transportManagerForThread(), request, options);
    if (response.cancelled || response.timedOut || response.responseTooLarge || response.status != 200) {
        UpdateCheckResult failed;
        failed.status = UpdateCheckStatus::Failed;
        if (response.timedOut) {
            failed.errorKind = UpdateCheckErrorKind::Timeout;
        } else if (response.status == 404) {
            failed.errorKind = UpdateCheckErrorKind::NoReleasePublished;
        } else if (response.status == 403 || response.status == 429) {
            failed.errorKind = UpdateCheckErrorKind::RateLimited;
        } else if (response.status == 0) {
            failed.errorKind = UpdateCheckErrorKind::Offline;
            failed.errorDetail = response.networkError;
        } else {
            failed.errorKind = UpdateCheckErrorKind::HttpError;
            failed.errorDetail = QStringLiteral("HTTP %1").arg(response.status);
        }
        return failed;
    }
    UpdateCheckResult parsed = channel == UpdateChannel::Nightly
                                   ? parseNightlyReleaseResponse(response.body, currentBuildRevision, allowChannelSwitch)
                                   : parseLatestReleaseResponse(response.body, currentVersion);
    if (parsed.status == UpdateCheckStatus::Failed && parsed.errorKind == UpdateCheckErrorKind::None) {
        parsed.errorKind = UpdateCheckErrorKind::InvalidResponse;
    }
    if (channel == UpdateChannel::Stable && allowChannelSwitch &&
        parsed.status == UpdateCheckStatus::UpToDate) {
        parsed.status = UpdateCheckStatus::UpdateAvailable;
    }
    return parsed;
}

InstallKind detectInstallKind(const QString& applicationFilePath, const QString& appImageEnv) {
    const QString exePath = applicationFilePath.isEmpty()
                                ? QCoreApplication::applicationFilePath()
                                : applicationFilePath;
#ifdef Q_OS_WIN
    Q_UNUSED(appImageEnv);
    const QFileInfo exeInfo(exePath);
    const QDir installDir = exeInfo.dir();
    if (QFileInfo::exists(installDir.filePath(QStringLiteral("unins000.exe")))) {
        return InstallKind::WindowsInstaller;
    }
    if (QFileInfo(installDir.absolutePath()).isWritable()) {
        return InstallKind::WindowsPortable;
    }
    return InstallKind::SystemPackage;
#elif defined(Q_OS_LINUX)
    const QString appImage = appImageEnv.isEmpty() ? qEnvironmentVariable("APPIMAGE") : appImageEnv;
    if (!appImage.isEmpty()) {
        const QFileInfo info(appImage);
        if (info.exists() && QFileInfo(info.absolutePath()).isWritable()) {
            return InstallKind::LinuxAppImage;
        }
        return InstallKind::SystemPackage;
    }
    const QString canonical = QFileInfo(exePath).absoluteFilePath();
    if (canonical.startsWith(QLatin1String("/usr/")) ||
        canonical.startsWith(QLatin1String("/opt/")) ||
        canonical.startsWith(QLatin1String("/var/")) ||
        canonical.startsWith(QLatin1String("/snap/")) ||
        canonical.startsWith(QLatin1String("/nix/"))) {
        return InstallKind::SystemPackage;
    }
    const QDir installDir = QFileInfo(canonical).dir();
    if (QFileInfo(installDir.absolutePath()).isWritable()) {
        return InstallKind::LinuxTarball;
    }
    return InstallKind::SystemPackage;
#else
    Q_UNUSED(exePath);
    Q_UNUSED(appImageEnv);
    return InstallKind::Unknown;
#endif
}

bool isSelfUpdatable(InstallKind kind) {
    return kind == InstallKind::WindowsInstaller || kind == InstallKind::WindowsPortable ||
           kind == InstallKind::LinuxAppImage || kind == InstallKind::LinuxTarball;
}

std::optional<UpdateAsset> selectUpdateAsset(const QVector<UpdateAsset>& assets, InstallKind kind) {
    for (const UpdateAsset& asset : assets) {
        if (!isSafeAssetName(asset.name)) continue;
        const QString lower = asset.name.toLower();
        switch (kind) {
            case InstallKind::LinuxAppImage:
                if (lower.endsWith(QLatin1String(".appimage"))) return asset;
                break;
            case InstallKind::LinuxTarball:
                if (lower.contains(QLatin1String("linux")) &&
                    (lower.endsWith(QLatin1String(".tar.gz")) || lower.endsWith(QLatin1String(".tgz")))) {
                    return asset;
                }
                break;
            case InstallKind::WindowsInstaller:
                if (lower.endsWith(QLatin1String(".exe")) &&
                    (lower.contains(QLatin1String("setup")) || lower.contains(QLatin1String("installer")))) {
                    return asset;
                }
                break;
            case InstallKind::WindowsPortable:
                if (lower.endsWith(QLatin1String(".zip")) &&
                    (lower.contains(QLatin1String("win")) || lower.contains(QLatin1String("portable")))) {
                    return asset;
                }
                break;
            case InstallKind::SystemPackage:
            case InstallKind::Unknown:
                return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<UpdateAsset> findChecksumsAsset(const QVector<UpdateAsset>& assets) {
    for (const UpdateAsset& asset : assets) {
        const QString upper = asset.name.toUpper();
        if (upper == QLatin1String("SHA256SUMS") || upper == QLatin1String("SHA256SUMS.TXT")) {
            return asset;
        }
    }
    return std::nullopt;
}

QString parseSha256Sums(const QByteArray& content, const QString& assetName) {
    static const QRegularExpression linePattern(
        QStringLiteral("^([0-9a-fA-F]{64})\\s+\\*?(.+)$"));
    const QString text = QString::fromUtf8(content);
    for (const QString& rawLine : text.split(QLatin1Char('\n'))) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const auto match = linePattern.match(line);
        if (!match.hasMatch()) continue;
        const QString candidate = QFileInfo(match.captured(2).trimmed()).fileName();
        if (candidate == assetName) {
            return match.captured(1).toLower();
        }
    }
    return {};
}

bool downloadAndVerifyUpdate(const UpdateCheckResult& release, InstallKind kind,
                             const std::function<void(qint64 received, qint64 total)>& progress,
                             StagedUpdate* out, QString* error) {
    auto fail = [error](const QString& msg) {
        if (error != nullptr) *error = msg;
        return false;
    };
    if (!isSelfUpdatable(kind)) {
        return fail(QStringLiteral("This installation is managed externally and cannot self-update."));
    }
    const auto asset = selectUpdateAsset(release.assets, kind);
    if (!asset) {
        return fail(QStringLiteral("No matching release package was found for this platform."));
    }
    const auto checksums = findChecksumsAsset(release.assets);
    if (!checksums) {
        return fail(QStringLiteral("Release checksums (SHA256SUMS) are missing; refusing unverified update."));
    }

    QNetworkRequest sumRequest(checksums->downloadUrl);
    sumRequest.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mikai-Ja-Nai updater"));
    sumRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                            QNetworkRequest::NoLessSafeRedirectPolicy);
    HttpFetchOptions sumOptions;
    sumOptions.timeoutMs = 15000;
    sumOptions.maxResponseBytes = 256 * 1024;
    const HttpFetchResult sumFetch =
        runBlockingFetch(transportManagerForThread(), sumRequest, sumOptions);
    if (sumFetch.cancelled) return fail(QStringLiteral("Update download was cancelled."));
    if (sumFetch.timedOut || sumFetch.status < 200 || sumFetch.status >= 300) {
        return fail(QStringLiteral("Could not download release SHA256SUMS."));
    }
    const QString expectedSha = parseSha256Sums(sumFetch.body, asset->name);
    if (expectedSha.isEmpty()) {
        return fail(QStringLiteral("SHA256SUMS does not contain a checksum for %1.").arg(asset->name));
    }

    const QString baseData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString safeVersion =
        QString(release.version).replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._+-]")), QStringLiteral("_"));
    const QString stagingDir = QDir(baseData).filePath(QStringLiteral("updates/%1").arg(safeVersion));
    if (!QDir().mkpath(stagingDir)) {
        return fail(QStringLiteral("Could not create update staging directory."));
    }
    const QString destPath = QDir(stagingDir).filePath(asset->name);
    if (QFileInfo::exists(destPath)) {
        QFile::remove(destPath);
    }

    QString dlReason;
    if (!downloadToFile(asset->downloadUrl, destPath, asset->size, expectedSha, progress, {}, &dlReason)) {
        return fail(dlReason.isEmpty() ? QStringLiteral("Failed to download or verify update package.")
                                       : dlReason);
    }

    if (out != nullptr) {
        out->stagedPath = destPath;
        out->stagingDir = stagingDir;
        out->assetName = asset->name;
        out->version = release.version;
        out->installKind = kind;
    }
    return true;
}

bool launchUpdateInstaller(const StagedUpdate& staged, qint64 currentPid, QString* error) {
    auto fail = [error](const QString& msg) {
        if (error != nullptr) *error = msg;
        return false;
    };
    if (!QFileInfo::exists(staged.stagedPath)) {
        return fail(QStringLiteral("Staged update file no longer exists."));
    }

#ifdef Q_OS_WIN
    if (staged.installKind == InstallKind::WindowsInstaller) {
        QProcess proc;
        proc.setProgram(staged.stagedPath);
        proc.setArguments({QStringLiteral("/SILENT"), QStringLiteral("/CLOSEAPPLICATIONS"),
                           QStringLiteral("/RESTARTAPPLICATIONS")});
        if (!proc.startDetached()) {
            return fail(QStringLiteral("Could not start the Windows update installer."));
        }
        return true;
    }
    if (staged.installKind == InstallKind::WindowsPortable) {
        const QString exePath = QCoreApplication::applicationFilePath();
        const QString installDir = QFileInfo(exePath).absolutePath();
        const QString scriptPath = QDir(staged.stagingDir).filePath(QStringLiteral("apply-update.ps1"));
        QFile script(scriptPath);
        if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            return fail(QStringLiteral("Could not write portable updater script."));
        }
        const QString ps = QStringLiteral(
            "param([int]$PidToWait, [string]$ZipPath, [string]$TargetDir, [string]$ExePath)\n"
            "try { Wait-Process -Id $PidToWait -Timeout 30 -ErrorAction SilentlyContinue } catch {}\n"
            "$backup = \"$TargetDir.bak\"\n"
            "if (Test-Path $backup) { Remove-Item -Recurse -Force $backup -ErrorAction SilentlyContinue }\n"
            "try {\n"
            "  Expand-Archive -Path $ZipPath -DestinationPath $TargetDir -Force\n"
            "  Start-Process -FilePath $ExePath\n"
            "} catch {\n"
            "  exit 1\n"
            "}\n");
        script.write(ps.toUtf8());
        script.close();
        QProcess proc;
        proc.setProgram(QStringLiteral("powershell.exe"));
        proc.setArguments({QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
                           QStringLiteral("Bypass"), QStringLiteral("-File"), scriptPath,
                           QString::number(currentPid), staged.stagedPath, installDir, exePath});
        if (!proc.startDetached()) {
            return fail(QStringLiteral("Could not launch portable updater helper."));
        }
        return true;
    }
    return fail(QStringLiteral("Unsupported Windows installation type for automatic updates."));
#elif defined(Q_OS_LINUX)
    if (staged.installKind == InstallKind::LinuxAppImage) {
        const QString targetAppImage = qEnvironmentVariable("APPIMAGE");
        if (targetAppImage.isEmpty()) {
            return fail(QStringLiteral("APPIMAGE path is unavailable."));
        }
        const QString scriptPath = QDir(staged.stagingDir).filePath(QStringLiteral("apply-update.sh"));
        QFile script(scriptPath);
        if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            return fail(QStringLiteral("Could not write AppImage updater script."));
        }
        const QByteArray sh =
            "#!/bin/sh\n"
            "set -eu\n"
            "PID_WAIT=\"$1\"; STAGED=\"$2\"; TARGET=\"$3\"\n"
            "i=0; while kill -0 \"$PID_WAIT\" 2>/dev/null && [ \"$i\" -lt 150 ]; do sleep 0.2; i=$((i+1)); done\n"
            "BACKUP=\"${TARGET}.bak\"\n"
            "cp -f \"$TARGET\" \"$BACKUP\"\n"
            "chmod +x \"$STAGED\"\n"
            "if mv -f \"$STAGED\" \"$TARGET\"; then\n"
            "  rm -f \"$BACKUP\"\n"
            "  exec \"$TARGET\" &\n"
            "else\n"
            "  mv -f \"$BACKUP\" \"$TARGET\" || true\n"
            "  exit 1\n"
            "fi\n";
        script.write(sh);
        script.close();
        QFile::setPermissions(scriptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        if (!startDetachedHostTool(QStringLiteral("/bin/sh"),
                                   {scriptPath, QString::number(currentPid), staged.stagedPath, targetAppImage})) {
            return fail(QStringLiteral("Could not launch AppImage updater helper."));
        }
        return true;
    }
    if (staged.installKind == InstallKind::LinuxTarball) {
        const QString exePath = QCoreApplication::applicationFilePath();
        const QString installDir = QFileInfo(exePath).absolutePath();
        const QString scriptPath = QDir(staged.stagingDir).filePath(QStringLiteral("apply-update.sh"));
        QFile script(scriptPath);
        if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            return fail(QStringLiteral("Could not write tarball updater script."));
        }
        const QByteArray sh =
            "#!/bin/sh\n"
            "set -eu\n"
            "PID_WAIT=\"$1\"; ARCHIVE=\"$2\"; TARGET_DIR=\"$3\"; EXE_PATH=\"$4\"\n"
            "i=0; while kill -0 \"$PID_WAIT\" 2>/dev/null && [ \"$i\" -lt 150 ]; do sleep 0.2; i=$((i+1)); done\n"
            "TMP_EXTRACT=\"${ARCHIVE}.extract\"\n"
            "rm -rf \"$TMP_EXTRACT\" && mkdir -p \"$TMP_EXTRACT\"\n"
            "tar -xzf \"$ARCHIVE\" -C \"$TMP_EXTRACT\"\n"
            "cp -a \"$TMP_EXTRACT\"/. \"$TARGET_DIR\"/\n"
            "rm -rf \"$TMP_EXTRACT\"\n"
            "exec \"$EXE_PATH\" &\n";
        script.write(sh);
        script.close();
        QFile::setPermissions(scriptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        if (!startDetachedHostTool(QStringLiteral("/bin/sh"),
                                   {scriptPath, QString::number(currentPid), staged.stagedPath, installDir, exePath})) {
            return fail(QStringLiteral("Could not launch tarball updater helper."));
        }
        return true;
    }
    return fail(QStringLiteral("This Linux installation cannot be self-updated; please update via your package manager or GitHub Releases."));
#else
    Q_UNUSED(currentPid);
    return fail(QStringLiteral("Automatic updates are not supported on this platform."));
#endif
}

}  // namespace mikai::backend
