#include "Win32Support.hpp"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <wincrypt.h>

namespace mikai::gui::win32 {

const wchar_t* wide(const QString& value) {
    return reinterpret_cast<const wchar_t*>(value.utf16());
}

bool missing(LSTATUS status) {
    return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

QString systemMessage(DWORD status) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, status, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    const QString reason =
        length != 0 ? QString::fromWCharArray(buffer, length).trimmed() : QString{};
    if (buffer != nullptr) LocalFree(buffer);
    return reason;
}

// P0-03: the ~40-line DPAPI helper. CryptProtectData ties the blob to the
// current user on this machine; CRYPTPROTECT_UI_FORBIDDEN keeps a background
// save from ever prompting. The blob is opaque bytes — the caller decides
// where they live (the app-data file in this unit).
bool protectCredential(const QString& plaintext, QByteArray* blob) {
    if (plaintext.isEmpty() || blob == nullptr) return false;
    const QByteArray plain = plaintext.toUtf8();
    DATA_BLOB input{static_cast<DWORD>(plain.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()))};
    DATA_BLOB output{};
    if (CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr,
                         CRYPTPROTECT_UI_FORBIDDEN, &output) == FALSE) {
        return false;
    }
    *blob = QByteArray(reinterpret_cast<const char*>(output.pbData),
                       static_cast<int>(output.cbData));
    LocalFree(output.pbData);
    return true;
}

bool unprotectCredential(const QByteArray& blob, QString* plaintext) {
    if (blob.isEmpty() || plaintext == nullptr) return false;
    DATA_BLOB input{static_cast<DWORD>(blob.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(blob.constData()))};
    DATA_BLOB output{};
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &output) == FALSE) {
        return false;
    }
    *plaintext = QString::fromUtf8(reinterpret_cast<const char*>(output.pbData),
                                   static_cast<int>(output.cbData));
    LocalFree(output.pbData);
    return true;
}

}  // namespace mikai::gui::win32
#endif

namespace mikai::gui {

QString openSubtitlesApiKeyFilePath() {
    const QString appData =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(appData).filePath(QStringLiteral("opensubtitles-api-key.bin"));
}

namespace {

// The bytes the file holds: the DPAPI blob, base64, on Windows; the raw key
// elsewhere. A file the other platform wrote is a corrupt store, not a key —
// both readers fail closed and return empty.
QByteArray encodeStoredKey(const QString& key, bool* ok) {
#ifdef Q_OS_WIN
    QByteArray blob;
    *ok = win32::protectCredential(key, &blob);
    return blob.toBase64();
#else
    *ok = true;
    return key.toUtf8();
#endif
}

QString decodeStoredKey(const QByteArray& stored, bool* ok) {
#ifdef Q_OS_WIN
    const QByteArray blob = QByteArray::fromBase64(stored.trimmed());
    QString key;
    *ok = win32::unprotectCredential(blob, &key);
    return key;
#else
    *ok = true;
    return QString::fromUtf8(stored).trimmed();
#endif
}

}  // namespace

QString loadOpenSubtitlesApiKey() {
    const QString path = openSubtitlesApiKeyFilePath();
    QFile file(path);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning("mikai: could not read the stored OpenSubtitles API key");
            return {};
        }
        bool ok = false;
        const QString key = decodeStoredKey(file.readAll(), &ok);
        if (!ok) {
            qWarning("mikai: the stored OpenSubtitles API key could not be decrypted");
            return {};
        }
        return key;
    }
    // One-time migration: the legacy plaintext QSettings value moves to the
    // new store and the QSettings key is deleted. If the move fails the legacy
    // value is kept (and logged) rather than silently dropping the key.
    QSettings settings;
    const QString legacy =
        settings.value(QStringLiteral("access/openSubtitlesApiKey")).toString();
    if (legacy.isEmpty()) return {};
    QString reason;
    if (!saveOpenSubtitlesApiKey(legacy, &reason)) {
        qWarning().noquote() << "mikai: could not migrate the OpenSubtitles API key:"
                             << reason;
        return legacy;
    }
    settings.remove(QStringLiteral("access/openSubtitlesApiKey"));
    qInfo("mikai: migrated the OpenSubtitles API key out of QSettings");
    return legacy;
}

bool saveOpenSubtitlesApiKey(const QString& key, QString* reason) {
    const QString path = openSubtitlesApiKeyFilePath();
    QSettings settings;
    if (key.trimmed().isEmpty()) {
        // Clearing the key removes both stores: nothing secret is left behind
        // in either place.
        QFile::remove(path);
        settings.remove(QStringLiteral("access/openSubtitlesApiKey"));
        return true;
    }
    bool ok = false;
    const QByteArray stored = encodeStoredKey(key.trimmed(), &ok);
    if (!ok) {
        if (reason != nullptr) {
            *reason = QStringLiteral("could not protect the OpenSubtitles API key");
        }
        return false;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (reason != nullptr) {
            *reason = QStringLiteral("could not create %1").arg(QFileInfo(path).absolutePath());
        }
        return false;
    }
    // QSaveFile so a partial write never lands; 0600 so only this user can
    // read the file on platforms where the file itself is the protection.
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (reason != nullptr) {
            *reason = QStringLiteral("could not write %1: %2").arg(path, file.errorString());
        }
        return false;
    }
#ifndef Q_OS_WIN
    if (!file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
        if (reason != nullptr) {
            *reason = QStringLiteral("could not protect %1: %2").arg(path, file.errorString());
        }
        return false;
    }
#endif
    if (file.write(stored) != stored.size() || !file.commit()) {
        if (reason != nullptr) {
            *reason = QStringLiteral("could not save %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    // The new store is the only store: a stale legacy value must not survive
    // a save and resurrect the plaintext copy on the next load.
    settings.remove(QStringLiteral("access/openSubtitlesApiKey"));
    return true;
}

}  // namespace mikai::gui
