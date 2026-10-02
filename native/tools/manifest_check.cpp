#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

namespace {

bool readObject(const QString& path, QJsonObject& object, QTextStream& errors) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        errors << path << ": " << file.errorString() << '\n';
        return false;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        errors << path << ": invalid JSON object: " << error.errorString() << '\n';
        return false;
    }
    object = document.object();
    return true;
}

bool withinRoot(const QString& root, const QString& path) {
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return path.startsWith(root + QLatin1Char('/'), sensitivity);
}

bool verifyFile(const QString& sourceId, const QString& root, const QJsonObject& record,
                QTextStream& errors) {
    const QString name = record.value(QStringLiteral("file")).toString(
        record.value(QStringLiteral("name")).toString());
    const QString expectedHash = record.value(QStringLiteral("sha256")).toString();
    static const QRegularExpression hashPattern(QStringLiteral("^[0-9a-f]{64}$"));
    if (name.isEmpty() || QDir::isAbsolutePath(name) || name.split(QLatin1Char('/')).contains(QStringLiteral(".."))
        || name.contains(QLatin1Char('\\')) || !hashPattern.match(expectedHash).hasMatch()) {
        errors << sourceId << ": invalid file identity: " << name << '\n';
        return false;
    }
    const QFileInfo info(QDir(root).filePath(name));
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty() || !info.isFile() || !withinRoot(root, canonical)) {
        errors << sourceId << ": missing file or path escapes source root: " << name << '\n';
        return false;
    }
    if (record.contains(QStringLiteral("bytes"))) {
        const auto value = record.value(QStringLiteral("bytes"));
        const qint64 expectedBytes = value.toInteger(-1);
        if (!value.isDouble() || expectedBytes < 0 || info.size() != expectedBytes) {
            errors << sourceId << ": size mismatch: " << name << " (actual " << info.size() << ")\n";
            return false;
        }
    }
    QFile file(canonical);
    if (!file.open(QIODevice::ReadOnly)) {
        errors << sourceId << ": " << name << ": " << file.errorString() << '\n';
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || QString::fromLatin1(hash.result().toHex()) != expectedHash) {
        errors << sourceId << ": SHA-256 mismatch or read failure: " << name << '\n';
        return false;
    }
    return true;
}

}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mikai_manifest_check"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Check native source/component identities without loading any DLL."));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("sources"), QStringLiteral("Source identity manifest"), QStringLiteral("file")});
    parser.addOption({QStringLiteral("components"), QStringLiteral("Component rights manifest"), QStringLiteral("file")});
    parser.addOption({QStringLiteral("source-root"), QStringLiteral("Explicit read-only source root, repeatable"), QStringLiteral("id=directory")});
    parser.process(application);
    QTextStream errors(stderr);
    QTextStream output(stdout);
    QJsonObject sources;
    QJsonObject components;
    if (!readObject(parser.value(QStringLiteral("sources")), sources, errors)
        || !readObject(parser.value(QStringLiteral("components")), components, errors)) {
        return 1;
    }
    if (sources.value(QStringLiteral("schema")).toString() != QStringLiteral("mikai.native.sources/1")
        || components.value(QStringLiteral("schema")).toString() != QStringLiteral("mikai.native.components/1")
        || !sources.value(QStringLiteral("sources")).isArray()
        || sources.value(QStringLiteral("sources")).toArray().isEmpty()
        || !components.value(QStringLiteral("components")).isArray()
        || components.value(QStringLiteral("components")).toArray().isEmpty()) {
        errors << "Unsupported source/component manifest schema\n";
        return 1;
    }
    QMap<QString, QString> roots;
    for (const auto& entry : parser.values(QStringLiteral("source-root"))) {
        const auto separator = entry.indexOf(QLatin1Char('='));
        const QString id = entry.left(separator);
        const QFileInfo info(entry.mid(separator + 1));
        const QString root = info.canonicalFilePath();
        if (separator <= 0 || roots.contains(id) || root.isEmpty() || !info.isDir()) {
            errors << "Invalid or duplicate source root: " << entry << '\n';
            return 1;
        }
        roots.insert(id, root);
    }
    QSet<QString> sourceIds;
    bool invalid = false;
    bool incomplete = false;
    bool blocked = false;
    qsizetype verified = 0;
    for (const auto& value : sources.value(QStringLiteral("sources")).toArray()) {
        const auto source = value.toObject();
        const QString id = source.value(QStringLiteral("id")).toString();
        const QString kind = source.value(QStringLiteral("pinKind")).toString();
        if (id.isEmpty() || sourceIds.contains(id)
            || (kind != QStringLiteral("releasePin") && kind != QStringLiteral("referencePin"))) {
            errors << "Invalid source identity/pin kind: " << id << '\n';
            invalid = true;
            continue;
        }
        sourceIds.insert(id);
        QJsonArray files = source.value(QStringLiteral("sourceDigestsMeasuredLocally")).toArray();
        for (const auto& model : source.value(QStringLiteral("models")).toArray()) files.append(model);
        for (const auto& file : source.value(QStringLiteral("files")).toArray()) files.append(file);
        if (files.isEmpty()) {
            errors << id << ": file integrity not established (no file digest inventory)\n";
            incomplete = true;
            continue;
        }
        if (!roots.contains(id)) {
            errors << id << ": file integrity not established (no --source-root)\n";
            incomplete = true;
            continue;
        }
        for (const auto& file : files) {
            if (!file.isObject() || !verifyFile(id, roots.value(id), file.toObject(), errors)) {
                invalid = true;
                continue;
            }
            ++verified;
        }
    }
    for (auto it = roots.cbegin(); it != roots.cend(); ++it) {
        if (!sourceIds.contains(it.key())) {
            errors << "Unknown source root id: " << it.key() << '\n';
            invalid = true;
        }
    }
    QSet<QString> componentIds;
    for (const auto& value : components.value(QStringLiteral("components")).toArray()) {
        const auto component = value.toObject();
        const QString id = component.value(QStringLiteral("id")).toString();
        const auto identity = component.value(QStringLiteral("sourceIdentity")).toObject();
        const QString sourceId = identity.value(QStringLiteral("sourceId")).toString();
        const QString kind = identity.value(QStringLiteral("pinKind")).toString();
        const QString rights = component.value(QStringLiteral("redistribution")).toString();
        if (id.isEmpty() || componentIds.contains(id) || !sourceIds.contains(sourceId)
            || (kind != QStringLiteral("releasePin") && kind != QStringLiteral("referencePin"))
            || (rights != QStringLiteral("cleared") && rights != QStringLiteral("blocked") && rights != QStringLiteral("external"))) {
            errors << "Invalid component/source/rights identity: " << id << '\n';
            invalid = true;
        }
        componentIds.insert(id);
        if (rights == QStringLiteral("blocked")) {
            errors << id << ": redistribution blocked: " << component.value(QStringLiteral("blockerRef")).toString() << '\n';
            blocked = true;
        }
    }
    output << "Files with matching recorded digests: " << verified << '\n';
    if (invalid) return 1;
    if (blocked) return 3;
    if (incomplete) return 2;
    output << "Source file integrity and component identity checks passed; this is not bundle loadability or runtime proof.\n";
    return 0;
}
