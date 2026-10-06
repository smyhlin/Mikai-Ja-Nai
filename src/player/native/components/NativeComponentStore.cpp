#include "native/components/NativeComponentStore.hpp"

#include "native/bundle/NativePaths.hpp"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <optional>

namespace mplayer::native {
namespace {

const char* kSchema = "mikai.native.components/1";
const char* kRecordFile = "component.json";
const char* kExtractionSubdir = "animejanai/inference";

QString translated(const char* message) {
    return QCoreApplication::translate("NativeComponentStore", message);
}

bool isSha256Hex(const QString& value) {
    if (value.size() != 64) return false;
    for (const QChar& c : value) {
        if (!c.isDigit() && (c < QLatin1Char('a') || c > QLatin1Char('f'))) return false;
    }
    return true;
}

struct FileRecord {
    QString path;
    qint64 bytes = 0;
    QString sha256;
};

struct Record {
    QString release;
    QVector<ComponentPack> packs;
    QVector<FileRecord> files;
};

bool parsePackArray(const QJsonArray& array, QVector<ComponentPack>* packs, QString* error) {
    const QStringList packKeys = {QStringLiteral("name"), QStringLiteral("asset"),
                                  QStringLiteral("assetBytes"), QStringLiteral("assetSha256"),
                                  QStringLiteral("url")};
    for (const QJsonValue& value : array) {
        if (!value.isObject()) {
            *error = QStringLiteral("a pack entry is not an object");
            return false;
        }
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (!packKeys.contains(it.key())) {
                *error = QStringLiteral("a pack entry carries an unknown key '%1'").arg(it.key());
                return false;
            }
        }
        ComponentPack pack;
        pack.name = object.value(QStringLiteral("name")).toString();
        pack.asset = object.value(QStringLiteral("asset")).toString();
        pack.assetBytes = static_cast<qint64>(object.value(QStringLiteral("assetBytes")).toDouble());
        pack.assetSha256 = object.value(QStringLiteral("assetSha256")).toString();
        pack.url = object.value(QStringLiteral("url")).toString();
        if (pack.name.isEmpty() || !isSha256Hex(pack.assetSha256) || pack.assetBytes <= 0) {
            *error = QStringLiteral("a pack entry is incomplete");
            return false;
        }
        packs->append(pack);
    }
    return true;
}

bool parseFileArray(const QJsonArray& array, QVector<FileRecord>* files, QString* error) {
    const QStringList fileKeys = {QStringLiteral("path"), QStringLiteral("bytes"),
                                  QStringLiteral("sha256")};
    for (const QJsonValue& value : array) {
        if (!value.isObject()) {
            *error = QStringLiteral("a file entry is not an object");
            return false;
        }
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (!fileKeys.contains(it.key())) {
                *error = QStringLiteral("a file entry carries an unknown key '%1'").arg(it.key());
                return false;
            }
        }
        FileRecord file;
        file.path = object.value(QStringLiteral("path")).toString();
        file.bytes = static_cast<qint64>(object.value(QStringLiteral("bytes")).toDouble());
        file.sha256 = object.value(QStringLiteral("sha256")).toString();
        QString pathReason;
        if (safeRelativePath(file.path, &pathReason).isEmpty() ||
            safeRelativePath(file.path, &pathReason).contains(QLatin1Char('/'))) {
            *error = QStringLiteral("file path '%1' is not a flat safe name").arg(file.path);
            return false;
        }
        if (!isSha256Hex(file.sha256) || file.bytes <= 0) {
            *error = QStringLiteral("file '%1' has an unusable digest or size").arg(file.path);
            return false;
        }
        files->append(file);
    }
    return true;
}

std::optional<Record> readRecord(const QString& directory, QString* error) {
    const QString path = QDir(directory).filePath(QString::fromLatin1(kRecordFile));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("%1 cannot be read").arg(path);
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("%1 is not valid JSON").arg(path);
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    const QStringList recordKeys = {QStringLiteral("schema"), QStringLiteral("release"),
                                    QStringLiteral("packs"), QStringLiteral("files")};
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!recordKeys.contains(it.key())) {
            *error = QStringLiteral("%1 carries an unknown key '%2'").arg(path, it.key());
            return std::nullopt;
        }
    }
    if (object.value(QStringLiteral("schema")).toString() != QLatin1String(kSchema)) {
        *error = QStringLiteral("%1 has the wrong schema").arg(path);
        return std::nullopt;
    }
    Record record;
    record.release = object.value(QStringLiteral("release")).toString();
    if (!parsePackArray(object.value(QStringLiteral("packs")).toArray(), &record.packs, error))
        return std::nullopt;
    if (!parseFileArray(object.value(QStringLiteral("files")).toArray(), &record.files, error))
        return std::nullopt;
    return record;
}

bool writeRecord(const QString& directory, const Record& record, QString* error) {
    QJsonArray packs;
    for (const ComponentPack& pack : record.packs) {
        QJsonObject object;
        object.insert(QStringLiteral("name"), pack.name);
        object.insert(QStringLiteral("asset"), pack.asset);
        object.insert(QStringLiteral("assetBytes"), pack.assetBytes);
        object.insert(QStringLiteral("assetSha256"), pack.assetSha256);
        object.insert(QStringLiteral("url"), pack.url);
        packs.append(object);
    }
    QJsonArray files;
    for (const FileRecord& file : record.files) {
        QJsonObject object;
        object.insert(QStringLiteral("path"), file.path);
        object.insert(QStringLiteral("bytes"), file.bytes);
        object.insert(QStringLiteral("sha256"), file.sha256);
        files.append(object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("schema"), QString::fromLatin1(kSchema));
    root.insert(QStringLiteral("release"), record.release);
    root.insert(QStringLiteral("packs"), packs);
    root.insert(QStringLiteral("files"), files);

    QDir().mkpath(directory);
    QSaveFile save(QDir(directory).filePath(QString::fromLatin1(kRecordFile)));
    if (!save.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("component.json could not be written: %1").arg(save.errorString());
        return false;
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (save.write(payload) != payload.size() || !save.commit()) {
        *error = QStringLiteral("component.json could not be committed");
        return false;
    }
    return true;
}

std::optional<QPair<qint64, QString>> measureFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty()) break;
        hash.addData(chunk);
    }
    return qMakePair(file.size(), QString::fromLatin1(hash.result().toHex()));
}

}  // namespace

ComponentStore componentStore() {
    return {nativeSubdirectory(QStringLiteral("components"))};
}

QString componentReleaseDirectory(const ComponentStore& store) {
    return QDir(store.root).filePath(
        QStringLiteral("tensorrt/%1").arg(componentReleaseTag()));
}

StoredPack storedPack(const ComponentStore& store, const ComponentPack& pack) {
    StoredPack result;
    result.pack = pack.name;
    result.directory = componentReleaseDirectory(store);
    if (store.root.isEmpty()) {
        result.reasonKey = QStringLiteral("native.components.missing");
        result.reason = translated("The component store root is not available");
        return result;
    }
    QString error;
    const std::optional<Record> record = readRecord(result.directory, &error);
    if (!record.has_value()) {
        result.reasonKey = QStringLiteral("native.components.record");
        result.reason = error;
        return result;
    }
    const ComponentPack* recorded = nullptr;
    for (const ComponentPack& candidate : record->packs) {
        if (candidate.name == pack.name) {
            recorded = &candidate;
            break;
        }
    }
    if (recorded == nullptr) {
        result.reasonKey = QStringLiteral("native.components.missing");
        result.reason = QStringLiteral("%1 is not installed").arg(pack.name);
        return result;
    }
    for (const QString& fileName : pack.files) {
        const FileRecord* fileRecord = nullptr;
        for (const FileRecord& candidate : record->files) {
            if (candidate.path == fileName) {
                fileRecord = &candidate;
                break;
            }
        }
        if (fileRecord == nullptr) {
            result.reasonKey = QStringLiteral("native.components.missing");
            result.reason = QStringLiteral("%1 does not record %2").arg(pack.name, fileName);
            return result;
        }
        const QFileInfo info(QDir(result.directory).filePath(fileName));
        if (!info.exists() || info.size() != fileRecord->bytes) {
            result.reasonKey = QStringLiteral("native.components.missing");
            result.reason = QStringLiteral("%1 is absent or the wrong size").arg(fileName);
            return result;
        }
        result.bytes += fileRecord->bytes;
    }
    result.installed = true;
    return result;
}

QStringList installedPackNames(const ComponentStore& store) {
    if (store.root.isEmpty()) return {};
    QString error;
    const std::optional<Record> record = readRecord(componentReleaseDirectory(store), &error);
    if (!record.has_value()) return {};
    QStringList names;
    for (const ComponentPack& pack : record->packs) names.append(pack.name);
    return names;
}

StoredPack verifyPack(const ComponentStore& store, const ComponentPack& pack) {
    StoredPack result = storedPack(store, pack);
    if (!result.installed) return result;

    QString error;
    const std::optional<Record> record = readRecord(result.directory, &error);
    if (!record.has_value()) {
        result.installed = false;
        result.reasonKey = QStringLiteral("native.components.record");
        result.reason = error;
        return result;
    }
    for (const QString& fileName : pack.files) {
        const FileRecord* recorded = nullptr;
        for (const FileRecord& candidate : record->files) {
            if (candidate.path == fileName) {
                recorded = &candidate;
                break;
            }
        }
        if (recorded == nullptr) {
            result.installed = false;
            result.reasonKey = QStringLiteral("native.components.missing");
            result.reason = QStringLiteral("%1 does not record %2").arg(pack.name, fileName);
            return result;
        }
        const std::optional<QPair<qint64, QString>> measured =
            measureFile(QDir(result.directory).filePath(fileName));
        if (!measured.has_value()) {
            result.installed = false;
            result.reasonKey = QStringLiteral("native.components.missing");
            result.reason = QStringLiteral("%1 could not be read").arg(fileName);
            return result;
        }
        if (measured->first != recorded->bytes ||
            measured->second.compare(recorded->sha256, Qt::CaseInsensitive) != 0) {
            result.installed = false;
            result.reasonKey = QStringLiteral("native.components.digest");
            result.reason = QStringLiteral("%1 no longer matches the recorded bytes").arg(fileName);
            return result;
        }
    }
    result.reasonKey.clear();
    result.reason.clear();
    return result;
}

bool installPack(const ComponentStore& store, const ComponentPack& pack,
                 const QString& extractionRoot, QString* reasonKey, QString* reason) {
    auto fail = [&](const QString& key, const QString& text) {
        if (reasonKey) *reasonKey = key;
        if (reason) *reason = text;
        return false;
    };
    if (store.root.isEmpty()) {
        return fail(QStringLiteral("native.components.install"),
                    QStringLiteral("the component store root is not available"));
    }
    const QString directory = componentReleaseDirectory(store);
    if (!QDir().mkpath(directory)) {
        return fail(QStringLiteral("native.components.install"),
                    QStringLiteral("%1 could not be created").arg(directory));
    }

    Record record{componentReleaseTag(), {}, {}};
    QString error;
    const QString recordPath = QDir(directory).filePath(QString::fromLatin1(kRecordFile));
    if (QFileInfo::exists(recordPath)) {
        const std::optional<Record> existing = readRecord(directory, &error);
        if (!existing.has_value()) {
            return fail(QStringLiteral("native.components.record"), error);
        }
        record = *existing;
    }

    const QString sourceDir =
        QDir(extractionRoot).filePath(QString::fromLatin1(kExtractionSubdir));

    QVector<FileRecord> newFiles;
    for (const QString& fileName : pack.files) {
        const QString source = QDir(sourceDir).filePath(fileName);
        if (!QFileInfo::exists(source)) {
            return fail(QStringLiteral("native.components.extract"),
                        QStringLiteral("%1 is not in the archive").arg(fileName));
        }
        QFile in(source);
        if (!in.open(QIODevice::ReadOnly)) {
            return fail(QStringLiteral("native.components.extract"),
                        QStringLiteral("%1 could not be read").arg(source));
        }
        const std::optional<QPair<qint64, QString>> measured = measureFile(source);
        if (!measured.has_value()) {
            return fail(QStringLiteral("native.components.extract"),
                        QStringLiteral("%1 could not be read").arg(source));
        }
        const QString target = QDir(directory).filePath(fileName);
        QFile::remove(target);
        QFile out(target);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return fail(QStringLiteral("native.components.install"),
                        QStringLiteral("%1 could not be copied into the store").arg(fileName));
        }
        // The copy is hashed as it streams, so the record can only ever hold
        // bytes that were compared against the source instead of trusting that
        // a blind copy landed whole.
        QCryptographicHash hash(QCryptographicHash::Sha256);
        char buffer[64 * 1024];
        qint64 copied = 0;
        bool streamed = true;
        for (;;) {
            const qint64 n = in.read(buffer, sizeof buffer);
            if (n == 0) {
                break;
            }
            if (n < 0 || out.write(buffer, n) != n) {
                streamed = false;
                break;
            }
            hash.addData(QByteArrayView(buffer, static_cast<qsizetype>(n)));
            copied += n;
        }
        out.close();
        in.close();
        const QString digest = QString::fromLatin1(hash.result().toHex());
        if (!streamed || copied != measured->first ||
            digest.compare(measured->second, Qt::CaseInsensitive) != 0) {
            QFile::remove(target);
            return fail(QStringLiteral("native.components.install"),
                        QStringLiteral("%1 did not copy correctly").arg(fileName));
        }
        newFiles.append({fileName, copied, digest});
    }

    bool packRecorded = false;
    for (ComponentPack& existing : record.packs) {
        if (existing.name == pack.name) {
            existing = pack;
            packRecorded = true;
            break;
        }
    }
    if (!packRecorded) record.packs.append(pack);

    for (const FileRecord& file : newFiles) {
        bool replaced = false;
        for (FileRecord& existing : record.files) {
            if (existing.path == file.path) {
                existing = file;
                replaced = true;
                break;
            }
        }
        if (!replaced) record.files.append(file);
    }

    if (!writeRecord(directory, record, &error)) {
        return fail(QStringLiteral("native.components.record"), error);
    }
    if (reasonKey) reasonKey->clear();
    if (reason) reason->clear();
    return true;
}

}  // namespace mplayer::native
