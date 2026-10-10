#include "native/components/NativeComponentStore.hpp"

#include "native/bundle/NativePaths.hpp"
#include "native/models/NativeRife.hpp"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <optional>

namespace mplayer::native {
namespace {

const char* kSchema = "mikai.native.components/1";
const char* kRecordFile = "component.json";
const char* kRifeRecordFile = "install.json";
const char* kRifeRecordSchema = "mikai.native.rife/1";
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
    qint64 hashed = 0;
    for (;;) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty()) {
            if (file.error() != QFile::NoError) return std::nullopt;
            break;
        }
        hash.addData(chunk);
        hashed += chunk.size();
    }
    if (hashed != file.size()) return std::nullopt;
    return qMakePair(hashed, QString::fromLatin1(hash.result().toHex()));
}

std::optional<QPair<qint64, QString>> copyHashed(const QString& source, const QString& target,
                                                 QString* error) {
    const qint64 expected = QFileInfo(source).size();
    const QString partial = target + QStringLiteral(".partial");
    QString lastError;
    for (int attempt = 0; attempt < 3; ++attempt) {
        QFile::remove(partial);
        QFile in(source);
        if (!in.open(QIODevice::ReadOnly)) {
            lastError = in.errorString();
            continue;
        }
        QFile out(partial);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            lastError = out.errorString();
            continue;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        char buffer[1024 * 1024];
        qint64 copied = 0;
        bool streamed = true;
        for (;;) {
            const qint64 n = in.read(buffer, sizeof buffer);
            if (n < 0) {
                streamed = false;
                lastError = in.errorString();
                break;
            }
            if (n == 0) break;
            qint64 written = 0;
            while (written < n) {
                const qint64 w = out.write(buffer + written, n - written);
                if (w <= 0) {
                    streamed = false;
                    lastError = out.errorString();
                    break;
                }
                written += w;
            }
            if (!streamed) break;
            hash.addData(QByteArrayView(buffer, static_cast<qsizetype>(n)));
            copied += n;
        }
        const bool committed = streamed && out.flush() && out.error() == QFile::NoError;
        out.close();
        in.close();
        if (!committed || copied != expected) {
            QFile::remove(partial);
            if (lastError.isEmpty()) lastError = QStringLiteral("the copy ended before the source");
            continue;
        }
        const QString digest = QString::fromLatin1(hash.result().toHex());
        QFile::remove(target);
        if (!QFile::rename(partial, target)) {
            QFile::remove(partial);
            lastError = QStringLiteral("the existing file is in use");
            continue;
        }
        const std::optional<QPair<qint64, QString>> landed = measureFile(target);
        if (!landed.has_value() || landed->first != expected ||
            landed->second.compare(digest, Qt::CaseInsensitive) != 0) {
            QFile::remove(target);
            lastError = QStringLiteral("the stored copy did not match the source");
            continue;
        }
        return landed;
    }
    if (error != nullptr) *error = lastError;
    return std::nullopt;
}
bool safeRifePins(const QVector<RifeModelPin>& pins, QString* reason) {
    if (pins.isEmpty()) {
        *reason = QStringLiteral("the RIFE graph inventory is empty");
        return false;
    }
    QSet<QString> names;
    for (const RifeModelPin& pin : pins) {
        QString pathReason;
        const QString safe = safeRelativePath(pin.fileName, &pathReason);
        const QString key = pin.fileName.toCaseFolded();
        if (safe.isEmpty() || safe.contains(QLatin1Char('/')) || pin.bytes <= 0 ||
            !isSha256Hex(pin.graphSha256) || names.contains(key)) {
            *reason = QStringLiteral("the RIFE graph inventory contains an invalid or duplicate pin");
            return false;
        }
        names.insert(key);
    }
    return true;
}

bool matchesCompiledRifePins(const QVector<RifeModelPin>& pins) {
    const QVector<RifeModelPin>& compiled = rifeModelPins();
    if (pins.size() != compiled.size()) return false;
    for (qsizetype i = 0; i < pins.size(); ++i) {
        if (pins.at(i).fileName != compiled.at(i).fileName ||
            pins.at(i).bytes != compiled.at(i).bytes ||
            pins.at(i).graphSha256 != compiled.at(i).graphSha256) {
            return false;
        }
    }
    return true;
}

bool copyPinnedGraph(const QString& source, const QString& destination,
                     const RifeModelPin& pin, const std::function<bool()>& cancelled,
                     QString* reason) {
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) {
        *reason = QStringLiteral("%1 could not be read").arg(source);
        return false;
    }
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)) {
        *reason = QStringLiteral("%1 could not be created").arg(destination);
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 copied = 0;
    char buffer[64 * 1024];
    for (;;) {
        if (cancelled && cancelled()) {
            output.cancelWriting();
            *reason = translated("The RIFE graph installation was cancelled.");
            return false;
        }
        const qint64 count = input.read(buffer, sizeof buffer);
        if (count == 0) break;
        if (count < 0 || output.write(buffer, count) != count) {
            output.cancelWriting();
            *reason = QStringLiteral("%1 could not be copied").arg(pin.fileName);
            return false;
        }
        hash.addData(QByteArrayView(buffer, static_cast<qsizetype>(count)));
        copied += count;
    }
    const QString digest = QString::fromLatin1(hash.result().toHex());
    if (copied != pin.bytes || digest.compare(pin.graphSha256, Qt::CaseInsensitive) != 0) {
        output.cancelWriting();
        *reason = QStringLiteral("%1 does not match its pinned size and SHA-256").arg(pin.fileName);
        return false;
    }
    if (!output.commit()) {
        *reason = QStringLiteral("%1 could not be committed").arg(pin.fileName);
        return false;
    }
    return true;
}
bool exactKeys(const QJsonObject& object, const QStringList& expected) {
    if (object.size() != expected.size()) return false;
    for (const QString& key : expected) {
        if (!object.contains(key)) return false;
    }
    return true;
}

bool rifeRecordMatches(const QString& directory, const QVector<RifeModelPin>& pins,
                       QString* reason) {
    const QString path = QDir(directory).filePath(QString::fromLatin1(kRifeRecordFile));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *reason = QStringLiteral("the RIFE install record is missing or unreadable");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *reason = QStringLiteral("the RIFE install record is not valid JSON");
        return false;
    }
    const QJsonObject root = document.object();
    if (!exactKeys(root, {QStringLiteral("schema"), QStringLiteral("release"),
                          QStringLiteral("archive"), QStringLiteral("files")}) ||
        root.value(QStringLiteral("schema")).toString() != QLatin1String(kRifeRecordSchema) ||
        root.value(QStringLiteral("release")).toString() != componentReleaseTag()) {
        *reason = QStringLiteral("the RIFE install record has an unknown schema or release");
        return false;
    }
    const RifeArchivePack& pack = rifeArchivePack();
    const QJsonValue archiveValue = root.value(QStringLiteral("archive"));
    if (!archiveValue.isObject()) {
        *reason = QStringLiteral("the RIFE install record has no archive identity");
        return false;
    }
    const QJsonObject archive = archiveValue.toObject();
    if (!exactKeys(archive, {QStringLiteral("name"), QStringLiteral("asset"),
                             QStringLiteral("url"), QStringLiteral("assetBytes"),
                             QStringLiteral("assetSha256")}) ||
        archive.value(QStringLiteral("name")).toString() != pack.name ||
        archive.value(QStringLiteral("asset")).toString() != pack.asset ||
        archive.value(QStringLiteral("url")).toString() != pack.url ||
        !archive.value(QStringLiteral("assetBytes")).isDouble() ||
        archive.value(QStringLiteral("assetBytes")).toDouble() != static_cast<double>(pack.assetBytes) ||
        archive.value(QStringLiteral("assetSha256")).toString() != pack.assetSha256) {
        *reason = QStringLiteral("the RIFE install record does not match the pinned archive");
        return false;
    }
    const QJsonValue filesValue = root.value(QStringLiteral("files"));
    if (!filesValue.isArray()) {
        *reason = QStringLiteral("the RIFE install record has no file inventory");
        return false;
    }
    const QJsonArray files = filesValue.toArray();
    if (files.size() != pins.size()) {
        *reason = QStringLiteral("the RIFE install record does not match the pinned graph inventory");
        return false;
    }
    for (qsizetype i = 0; i < files.size(); ++i) {
        const QJsonValue value = files.at(i);
        if (!value.isObject()) {
            *reason = QStringLiteral("the RIFE install record contains an invalid file identity");
            return false;
        }
        const QJsonObject item = value.toObject();
        const RifeModelPin& pin = pins.at(i);
        if (!exactKeys(item, {QStringLiteral("path"), QStringLiteral("bytes"),
                              QStringLiteral("sha256")}) ||
            item.value(QStringLiteral("path")).toString() != pin.fileName ||
            !item.value(QStringLiteral("bytes")).isDouble() ||
            item.value(QStringLiteral("bytes")).toDouble() != static_cast<double>(pin.bytes) ||
            item.value(QStringLiteral("sha256")).toString() != pin.graphSha256) {
            *reason = QStringLiteral("the RIFE install record does not match pinned file '%1'")
                          .arg(pin.fileName);
            return false;
        }
    }
    return true;
}

bool writeRifeRecord(const QString& directory, const QVector<RifeModelPin>& pins,
                     QString* reason) {
    const RifeArchivePack& pack = rifeArchivePack();
    QJsonObject archive;
    archive.insert(QStringLiteral("name"), pack.name);
    archive.insert(QStringLiteral("asset"), pack.asset);
    archive.insert(QStringLiteral("url"), pack.url);
    archive.insert(QStringLiteral("assetBytes"), pack.assetBytes);
    archive.insert(QStringLiteral("assetSha256"), pack.assetSha256);
    QJsonArray files;
    for (const RifeModelPin& pin : pins) {
        QJsonObject item;
        item.insert(QStringLiteral("path"), pin.fileName);
        item.insert(QStringLiteral("bytes"), pin.bytes);
        item.insert(QStringLiteral("sha256"), pin.graphSha256);
        files.append(item);
    }
    QJsonObject root;
    root.insert(QStringLiteral("schema"), QString::fromLatin1(kRifeRecordSchema));
    root.insert(QStringLiteral("release"), componentReleaseTag());
    root.insert(QStringLiteral("archive"), archive);
    root.insert(QStringLiteral("files"), files);
    QSaveFile save(QDir(directory).filePath(QString::fromLatin1(kRifeRecordFile)));
    if (!save.open(QIODevice::WriteOnly)) {
        if (reason) *reason = QStringLiteral("the RIFE install record could not be written");
        return false;
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (save.write(payload) != payload.size() || !save.commit()) {
        if (reason) *reason = QStringLiteral("the RIFE install record could not be committed");
        return false;
    }
    return true;
}

bool exactRifeDirectoryInventory(const QString& directory, const QVector<RifeModelPin>& pins,
                                 QString* reason) {
    QSet<QString> expected;
    expected.insert(QString::fromLatin1(kRifeRecordFile));
    for (const RifeModelPin& pin : pins) expected.insert(pin.fileName);
    QHash<QString, qint64> expectedBytes;
    for (const RifeModelPin& pin : pins) expectedBytes.insert(pin.fileName, pin.bytes);
    QSet<QString> found;
    QDirIterator iterator(directory, QDir::AllEntries | QDir::NoDotAndDotDot |
                                          QDir::Hidden | QDir::System);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QFileInfo info(path);
        const QString name = info.fileName();
        if (info.isSymLink() || !info.isFile() || !expected.contains(name) ||
            found.contains(name) ||
            (name != QString::fromLatin1(kRifeRecordFile) &&
             info.size() != expectedBytes.value(name))) {
            *reason = QStringLiteral("the RIFE graph store contains an unexpected entry");
            return false;
        }
        found.insert(name);
    }
    if (found != expected) {
        *reason = QStringLiteral("the RIFE graph store does not contain the exact pinned inventory");
        return false;
    }
    return true;
}

}  // namespace

ComponentStore componentStore() {
    return {nativeSubdirectory(QStringLiteral("components"))};
}
RifeArchiveStore rifeArchiveStore() {
    const ComponentStore components = componentStore();
    return {components.root.isEmpty()
                ? QString()
                : QDir(components.root).filePath(QStringLiteral("rife/3.7.0"))};
}

StoredRifeGraph storedRifeGraph(const RifeArchiveStore& store, const RifeModelPin& pin) {
    StoredRifeGraph result;
    result.fileName = pin.fileName;
    QString pathReason;
    if (safeRelativePath(pin.fileName, &pathReason).isEmpty() ||
        pin.fileName.contains(QLatin1Char('/'))) {
        result.reason = QStringLiteral("the RIFE graph pin does not name a safe file");
        return result;
    }
    result.path = QDir(store.root).filePath(pin.fileName);
    if (store.root.isEmpty()) {
        result.reason = translated("The RIFE archive store root is not available");
        return result;
    }
    const QVector<RifeModelPin>& compiled = rifeModelPins();
    bool knownPin = false;
    for (const RifeModelPin& candidate : compiled) {
        if (candidate.fileName == pin.fileName && candidate.bytes == pin.bytes &&
            candidate.graphSha256 == pin.graphSha256) {
            knownPin = true;
            break;
        }
    }
    QString recordReason;
    if (!knownPin || !rifeRecordMatches(store.root, compiled, &recordReason) ||
        !exactRifeDirectoryInventory(store.root, compiled, &recordReason)) {
        result.reason = knownPin ? recordReason
                                 : QStringLiteral("the requested graph is not a compiled RIFE pin");
        return result;
    }
    const QFileInfo info(result.path);
    if (!info.exists() || !info.isFile() || info.isSymLink() ||
        !pathHasNoReparsePoints(nativeRoot(), result.path)) {
        result.reason = QStringLiteral("%1 is not present as a regular graph file").arg(pin.fileName);
        return result;
    }
    result.installed = true;
    result.bytes = info.size();
    if (result.bytes != pin.bytes) {
        result.reason = QStringLiteral("%1 is present with an unexpected byte count").arg(pin.fileName);
    }
    return result;
}

StoredRifeGraph verifyRifeGraph(const RifeArchiveStore& store, const RifeModelPin& pin) {
    StoredRifeGraph result = storedRifeGraph(store, pin);
    if (!result.installed) return result;
    const std::optional<QPair<qint64, QString>> measured = measureFile(result.path);
    if (!measured.has_value() || measured->first != pin.bytes ||
        measured->second.compare(pin.graphSha256, Qt::CaseInsensitive) != 0) {
        result.installed = false;
        result.reason = QStringLiteral("%1 does not match its pinned size and SHA-256")
                            .arg(pin.fileName);
        return result;
    }
    return result;
}

bool installRifeGraphs(const RifeArchiveStore& store, const QVector<RifeModelPin>& pins,
                       const QString& extractionRoot, const std::function<bool()>& cancelled,
                       QString* reason) {
    auto fail = [reason](const QString& message) {
        if (reason) *reason = message;
        return false;
    };
    QString pinReason;
    if (store.root.isEmpty() || pins.size() != 47 || !safeRifePins(pins, &pinReason)) {
        return fail(store.root.isEmpty()
                        ? translated("The RIFE archive store root is not available")
                        : (pins.size() != 47
                               ? translated("The pinned RIFE graph inventory must contain 47 files.")
                               : pinReason));
    }
    const QString sourceRoot = QDir(extractionRoot).filePath(QStringLiteral("animejanai/rife"));
    QSet<QString> expectedNames;
    for (const RifeModelPin& pin : pins) expectedNames.insert(pin.fileName);
    QSet<QString> foundNames;
    QDirIterator iterator(extractionRoot, QDir::AllEntries | QDir::NoDotAndDotDot |
                                               QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QFileInfo info(path);
        const QString relative = QDir(extractionRoot).relativeFilePath(path);
        if (info.isSymLink()) return fail(QStringLiteral("%1 is a link").arg(relative));
        if (info.isDir()) {
            if (relative != QStringLiteral("animejanai") &&
                relative != QStringLiteral("animejanai/rife")) {
                return fail(QStringLiteral("unexpected RIFE archive directory: %1").arg(relative));
            }
            continue;
        }
        if (!relative.startsWith(QStringLiteral("animejanai/rife/"))) {
            return fail(QStringLiteral("unexpected RIFE archive file: %1").arg(relative));
        }
        const QString name = relative.mid(QStringLiteral("animejanai/rife/").size());
        if (!expectedNames.contains(name) || foundNames.contains(name)) {
            return fail(QStringLiteral("unexpected or duplicate RIFE archive graph: %1").arg(relative));
        }
        foundNames.insert(name);
    }
    if (foundNames != expectedNames || !matchesCompiledRifePins(pins)) {
        return fail(QStringLiteral("the extracted RIFE archive does not contain the exact pinned inventory"));
    }
    if (cancelled && cancelled()) return fail(translated("The RIFE graph installation was cancelled."));

    const QString parent = QFileInfo(store.root).absolutePath();
    if (!pathWithinRoot(nativeRoot(), store.root) ||
        !pathHasNoReparsePoints(nativeRoot(), parent)) {
        return fail(QStringLiteral("the RIFE graph store path is not a safe app-owned path"));
    }
    if (!QDir().mkpath(parent) || !pathHasNoReparsePoints(nativeRoot(), parent)) {
        return fail(QStringLiteral("%1 could not be created as a safe app-owned path").arg(parent));
    }
    const QFileInfo previous(store.root);
    if (previous.exists() &&
        (!previous.isDir() || !pathHasNoReparsePoints(nativeRoot(), store.root))) {
        return fail(QStringLiteral("the existing RIFE graph store is not a safe directory"));
    }
    const QString stage = store.root + QStringLiteral(".staging-") +
                          QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString backup = store.root + QStringLiteral(".previous-") +
                           QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(stage)) {
        QDir(stage).removeRecursively();
        return fail(QStringLiteral("%1 could not be created").arg(stage));
    }
    auto removeStage = [&stage] { QDir(stage).removeRecursively(); };
    for (const RifeModelPin& pin : pins) {
        if (!copyPinnedGraph(QDir(sourceRoot).filePath(pin.fileName),
                             QDir(stage).filePath(pin.fileName), pin, cancelled, reason)) {
            removeStage();
            return false;
        }
    }
    if (!writeRifeRecord(stage, pins, reason)) {
        removeStage();
        return false;
    }
    if (cancelled && cancelled()) {
        removeStage();
        return fail(translated("The RIFE graph installation was cancelled."));
    }

    const bool hadPrevious = QFileInfo::exists(store.root);
    if (hadPrevious && !QDir().rename(store.root, backup)) {
        removeStage();
        return fail(QStringLiteral("the previous RIFE graph store could not be staged for replacement"));
    }
    if (!QDir().rename(stage, store.root)) {
        if (hadPrevious) QDir().rename(backup, store.root);
        removeStage();
        return fail(QStringLiteral("the complete RIFE graph inventory could not be published"));
    }
    if (hadPrevious) QDir(backup).removeRecursively();
    if (reason) reason->clear();
    return true;
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
        const QString target = QDir(directory).filePath(fileName);
        QString copyError;
        const std::optional<QPair<qint64, QString>> copied = copyHashed(source, target, &copyError);
        if (!copied.has_value()) {
            return fail(QStringLiteral("native.components.install"),
                        QStringLiteral("%1 did not copy correctly: %2").arg(fileName, copyError));
        }
        newFiles.append({fileName, copied->first, copied->second});
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
