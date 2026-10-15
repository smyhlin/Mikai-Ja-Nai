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
#include <QJsonValue>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTextStream>

#include <utility>

// mikai_manifest_check — recomputes every digest the manifests pin, and checks
// the manifests against each other. It loads no DLL and proves nothing about
// run-time behaviour.
//
// Exit codes:
//   0  clean
//   1  invalid identity, digest mismatch, or a cross-manifest inconsistency
//   2  a digest inventory or source root was not established
//   3  a component is redistribution 'blocked'

namespace {

constexpr auto kSourcesSchema = "mikai.native.sources/1";
constexpr auto kComponentsSchema = "mikai.native.components/1";

const QRegularExpression &sha256Pattern()
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern;
}

const QRegularExpression &gitShaPattern()
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{40}$"));
    return pattern;
}

bool isSha256(const QString &value)
{
    return sha256Pattern().match(value).hasMatch();
}

bool isGitSha(const QString &value)
{
    return gitShaPattern().match(value).hasMatch();
}

bool readObject(const QString &path, QJsonObject &object, QTextStream &errors) {
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

bool withinRoot(const QString &root, const QString &path) {
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return path.startsWith(root + QLatin1Char('/'), sensitivity);
}

// Collects every per-file digest record a source publishes. The key set is the
// contract between sources.json and this tool: a new digest inventory must be
// added here as well, or it is silently never verified.
QJsonArray digestInventory(const QJsonObject &source)
{
    QJsonArray files = source.value(QStringLiteral("sourceDigestsMeasuredLocally")).toArray();
    for (const auto &model : source.value(QStringLiteral("models")).toArray()) files.append(model);
    for (const auto &file : source.value(QStringLiteral("files")).toArray()) files.append(file);
    const auto packageFiles = source.value(QStringLiteral("licence")).toObject()
                                  .value(QStringLiteral("filesInPackage")).toArray();
    for (const auto &file : packageFiles) files.append(file);
    const QJsonObject binary = source.value(QStringLiteral("binaryIdentity")).toObject();
    if (!binary.isEmpty()) files.append(binary);

    // The same file can be reachable through more than one key (DirectML.dll is
    // both a binaryIdentity and a package file). Verify and count it once, so
    // the reported total reflects distinct files.
    QJsonArray unique;
    QSet<QString> seen;
    for (const auto &value : std::as_const(files)) {
        const auto record = value.toObject();
        const QString key = record.value(QStringLiteral("file")).toString(
            record.value(QStringLiteral("name")).toString())
            + QLatin1Char('|') + record.value(QStringLiteral("sha256")).toString();
        if (seen.contains(key)) continue;
        seen.insert(key);
        unique.append(value);
    }
    return unique;
}

bool verifyFile(const QString &sourceId, const QString &root, const QJsonObject &record,
                QTextStream &errors) {
    const QString name = record.value(QStringLiteral("file")).toString(
        record.value(QStringLiteral("name")).toString());
    const QString expectedHash = record.value(QStringLiteral("sha256")).toString();
    if (name.isEmpty() || QDir::isAbsolutePath(name) || name.split(QLatin1Char('/')).contains(QStringLiteral(".."))
        || name.contains(QLatin1Char('\\')) || !isSha256(expectedHash)) {
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

// Observation digests are format-checked and never recomputed: they describe
// files this repository did not build, so re-hashing them would assert an
// integrity claim that does not hold. A null digest is the recorded meaning
// "not computed" and is accepted; any other non-digest string is an error.
int scanObservations(const QJsonValue &value, QSet<QString> &digests, QTextStream &errors,
                     int &checked)
{
    int invalid = 0;
    if (value.isObject()) {
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (it.key() == QLatin1String("sha256")) {
                if (it.value().isNull()) continue;
                if (!it.value().isString() || !isSha256(it.value().toString())) {
                    errors << "observations: malformed sha256 under " << it.key() << '\n';
                    ++invalid;
                } else {
                    digests.insert(it.value().toString());
                    ++checked;
                }
                continue;
            }
            invalid += scanObservations(it.value(), digests, errors, checked);
        }
    } else if (value.isArray()) {
        for (const auto &item : value.toArray())
            invalid += scanObservations(item, digests, errors, checked);
    }
    return invalid;
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
    if (sources.value(QStringLiteral("schema")).toString() != QLatin1String(kSourcesSchema)
        || components.value(QStringLiteral("schema")).toString() != QLatin1String(kComponentsSchema)
        || !sources.value(QStringLiteral("sources")).isArray()
        || sources.value(QStringLiteral("sources")).toArray().isEmpty()
        || !components.value(QStringLiteral("components")).isArray()
        || components.value(QStringLiteral("components")).toArray().isEmpty()) {
        errors << "Unsupported source/component manifest schema\n";
        return 1;
    }
    QMap<QString, QString> roots;
    for (const auto &entry : parser.values(QStringLiteral("source-root"))) {
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
    QMap<QString, QString> sourcePinKinds;
    QMap<QString, QString> sourceCommits;
    QSet<QString> sourceIds;
    QSet<QString> pinnedDigests;
    bool invalid = false;
    bool incomplete = false;
    bool blocked = false;
    qsizetype verified = 0;

    for (const auto &value : sources.value(QStringLiteral("sources")).toArray()) {
        const auto source = value.toObject();
        const QString id = source.value(QStringLiteral("id")).toString();
        const QString kind = source.value(QStringLiteral("pinKind")).toString();
        const QString integrity = source.value(QStringLiteral("fileIntegrity")).toString();
        if (id.isEmpty() || sourceIds.contains(id)
            || (kind != QStringLiteral("releasePin") && kind != QStringLiteral("referencePin"))
            || (integrity != QStringLiteral("digestInventory")
                && integrity != QStringLiteral("commitIdentity")
                && integrity != QStringLiteral("documentationOnly"))) {
            errors << "Invalid source identity/pin kind/fileIntegrity: " << id << '\n';
            invalid = true;
            continue;
        }
        if (source.value(QStringLiteral("fileIntegrityReason")).toString().isEmpty()) {
            errors << id << ": fileIntegrity is declared without a reason\n";
            invalid = true;
        }
        sourceIds.insert(id);
        sourcePinKinds.insert(id, kind);
        sourceCommits.insert(id, source.value(QStringLiteral("ref")).toObject()
                                   .value(QStringLiteral("sha")).toString());

        const QJsonArray files = digestInventory(source);
        for (const auto &file : files) {
            if (!file.isObject() || !isSha256(file.toObject().value(QStringLiteral("sha256")).toString())) {
                errors << id << ": malformed entry in the digest inventory\n";
                invalid = true;
                continue;
            }
            pinnedDigests.insert(file.toObject().value(QStringLiteral("sha256")).toString());
        }

        if (integrity == QLatin1String("digestInventory")) {
            // A source that claims per-file digests must actually publish them,
            // and each one must be recomputable from a supplied root.
            if (files.isEmpty()) {
                errors << id << ": digestInventory declared but no digests are recorded\n";
                invalid = true;
                continue;
            }
            if (!roots.contains(id)) {
                errors << id << ": file integrity not established (no --source-root)\n";
                incomplete = true;
                continue;
            }
            for (const auto &file : files) {
                if (!file.isObject() || !verifyFile(id, roots.value(id), file.toObject(), errors)) {
                    invalid = true;
                    continue;
                }
                ++verified;
            }
            continue;
        }

        if (integrity == QLatin1String("commitIdentity")) {
            // Identity is the revision, so the revision must be well formed. A
            // recorded archive digest is checked for shape and recomputed when a
            // root was supplied; a null digest is the recorded "not computed".
            const auto ref = source.value(QStringLiteral("ref")).toObject();
            if (!ref.isEmpty()) {
                const QString sha = ref.value(QStringLiteral("sha")).toString();
                if (!isGitSha(sha)) {
                    errors << id << ": commit pin without a well-formed 40-hex sha\n";
                    invalid = true;
                }
                const QString tree = ref.value(QStringLiteral("tree")).toString();
                if (!tree.isEmpty() && !isGitSha(tree)) {
                    errors << id << ": git tree is not a well-formed 40-hex sha\n";
                    invalid = true;
                }
            }
            if (files.isEmpty() && !roots.contains(id)) continue;
        }
        // documentationOnly: nothing is shipped, so no digest is required.
    }

    for (auto it = roots.cbegin(); it != roots.cend(); ++it) {
        if (!sourceIds.contains(it.key())) {
            errors << "Unknown source root id: " << it.key() << '\n';
            invalid = true;
        }
    }

    // Observations: format-checked, never recomputed. Their digests must not
    // also appear as a pin, or an observation has been promoted without a build.
    // Collected only so the count can be reported; the set itself is deliberately
    // not used to veto pins, because a digest may be both observed and
    // independently pinned (see the four-entry check below).
    QSet<QString> observationDigests;
    int observationCount = 0;
    if (!sources.contains(QStringLiteral("observations"))) {
        errors << "sources.json: no observations block\n";
        invalid = true;
    } else if (scanObservations(sources.value(QStringLiteral("observations")), observationDigests,
                                errors, observationCount) != 0) {
        invalid = true;
    }
    const QJsonObject observed = sources.value(QStringLiteral("observations")).toObject()
                                     .value(QStringLiteral("observedBinaries")).toObject();
    const QJsonArray observedEntries = observed.value(QStringLiteral("entries")).toArray();
    const QStringList requiredLabels = {QStringLiteral("stock core"), QStringLiteral("native fork core"),
                                        QStringLiteral("aji dispatcher"), QStringLiteral("aji TensorRT backend")};
    if (observedEntries.size() != requiredLabels.size()) {
        errors << "observations.observedBinaries: expected 4 entries, found "
               << observedEntries.size() << '\n';
        invalid = true;
    }
    for (const auto &label : requiredLabels) {
        bool found = false;
        for (const auto &entry : observedEntries) {
            const auto record = entry.toObject();
            if (record.value(QStringLiteral("label")).toString() != label) continue;
            found = true;
            if (record.value(QStringLiteral("producedByThisRepository")).toBool()) {
                errors << "observations: " << label << " is marked as built by this repository\n";
                invalid = true;
            }
            if (!isSha256(record.value(QStringLiteral("sha256")).toString())) {
                errors << "observations: " << label << " has a malformed digest\n";
                invalid = true;
            }
            const auto bytes = record.value(QStringLiteral("bytes"));
            if (!bytes.isDouble() || bytes.toInteger(-1) < 0) {
                errors << "observations: " << label << " has a malformed byte count\n";
                invalid = true;
            }
        }
        if (!found) {
            errors << "observations.observedBinaries: missing the '" << label << "' entry\n";
            invalid = true;
        }
    }
    // The four plan hashes must never be referenced as a release pin. Note that
    // this is scoped to those four, not to the whole observations block: a
    // measured digest may legitimately also appear as a pin when it was
    // independently established from upstream (DirectML.dll is the case in
    // point - its pin comes from the NuGet package, and the observation that the
    // reference package's copy is byte-identical merely corroborates it).
    QSet<QString> descriptiveDigests;
    for (const auto &entry : observedEntries)
        descriptiveDigests.insert(entry.toObject().value(QStringLiteral("sha256")).toString());
    for (const auto &digest : descriptiveDigests) {
        if (pinnedDigests.contains(digest)) {
            errors << "sources.json: a descriptive observation digest is also used as a release pin\n";
            invalid = true;
        }
    }

    // ABI contract: the declared count must equal the list, and the required
    // plus optional split must account for every export exactly once.
    const QJsonObject abi = sources.value(QStringLiteral("abi")).toObject();
    const QJsonArray exports = abi.value(QStringLiteral("exports")).toArray();
    if (exports.isEmpty() || abi.value(QStringLiteral("exportCount")).toInt(-1) != exports.size()) {
        errors << "sources.json: abi.exportCount does not match abi.exports\n";
        invalid = true;
    }
    qsizetype requiredCount = 0;
    qsizetype optionalCount = 0;
    QSet<QString> symbols;
    for (const auto &value : exports) {
        const auto record = value.toObject();
        const QString symbol = record.value(QStringLiteral("symbol")).toString();
        const QString declaredIn = record.value(QStringLiteral("declaredIn")).toString();
        if (symbol.isEmpty() || symbols.contains(symbol)) {
            errors << "sources.json: empty or duplicate ABI symbol: " << symbol << '\n';
            invalid = true;
            continue;
        }
        symbols.insert(symbol);
        if (record.value(QStringLiteral("required")).toBool()) ++requiredCount; else ++optionalCount;
        if (declaredIn != QLatin1String("both") && declaredIn != QLatin1String("inferenceOnly")
            && declaredIn != QLatin1String("forkOnly")) {
            errors << "sources.json: ABI symbol " << symbol << " has no declaredIn\n";
            invalid = true;
        }
    }
    if (abi.value(QStringLiteral("loadGateCount")).toInt(-1) != requiredCount
        || abi.value(QStringLiteral("optionalCount")).toInt(-1) != optionalCount) {
        errors << "sources.json: abi load gate / optional counts disagree with abi.exports\n";
        invalid = true;
    }
    for (const auto &header : abi.value(QStringLiteral("headerPair")).toArray()) {
        const auto record = header.toObject();
        if (!isSha256(record.value(QStringLiteral("sha256")).toString())) {
            errors << "sources.json: ABI header digest is not a well-formed sha256: "
                   << record.value(QStringLiteral("path")).toString() << '\n';
            invalid = true;
        }
    }

    // Build configuration pins: the recorded values are load-bearing, so the
    // ones that change a link or a kernel are checked against their own restatement.
    const QJsonObject build = sources.value(QStringLiteral("buildConfigurationPins")).toObject();
    const QString nvinfer = build.value(QStringLiteral("AJI_NVINFER")).toObject()
                                .value(QStringLiteral("valueUsedHere")).toString();
    if (nvinfer != QLatin1String("nvinfer_11")) {
        errors << "sources.json: AJI_NVINFER must be pinned to nvinfer_11, found: " << nvinfer << '\n';
        invalid = true;
    }
    const QJsonObject archPin = build.value(QStringLiteral("CMAKE_CUDA_ARCHITECTURES")).toObject();
    const QJsonArray archList = archPin.value(QStringLiteral("asList")).toArray();
    const QStringList archFromValue = archPin.value(QStringLiteral("value")).toString().split(QLatin1Char(';'));
    bool architecturesMatch = !archList.isEmpty() && archList.size() == archFromValue.size();
    for (qsizetype index = 0; architecturesMatch && index < archList.size(); ++index) {
        architecturesMatch = archList.at(index).toString() == archFromValue.at(index);
    }
    if (!architecturesMatch) {
        errors << "sources.json: CMAKE_CUDA_ARCHITECTURES asList does not match its recorded value\n";
        invalid = true;
    }
    if (!archPin.value(QStringLiteral("value")).toString().contains(QLatin1String("120-virtual"))) {
        errors << "sources.json: CMAKE_CUDA_ARCHITECTURES must keep a -virtual entry for forward JIT\n";
        invalid = true;
    }
    const QJsonObject patchSeries = sources.value(QStringLiteral("patchSeries")).toObject();
    const QJsonObject mpvForkSeries =
        patchSeries.value(QStringLiteral("mpvForkPatchSeries")).toObject();
    const QString patchBase = !mpvForkSeries.isEmpty()
                                  ? mpvForkSeries.value(QStringLiteral("baseCommit")).toString()
                                  : patchSeries.value(QStringLiteral("baseCommit")).toString();
    QString forkCommit;
    for (auto it = sourcePinKinds.cbegin(); it != sourcePinKinds.cend(); ++it) {
        if (it.key() != QLatin1String("mpv-fork")) continue;
        forkCommit = sourceCommits.value(it.key());
    }
    if (!mpvForkSeries.isEmpty() && !patchSeries.value(QStringLiteral("baseCommit")).isNull()) {
        errors << "sources.json: patchSeries.baseCommit must be null when mpvForkPatchSeries carries the fork commit pin\n";
        invalid = true;
    }
    if (patchBase.isEmpty() || !isGitSha(patchBase)
        || (!forkCommit.isEmpty() && patchBase != forkCommit)) {
        errors << "sources.json: mpvForkPatchSeries.baseCommit does not match the mpv-fork commit pin\n";
        invalid = true;
    }

    // Components: identity, licence, rights decision, and agreement with the
    // source they claim to come from.
    QMap<QString, QString> componentBundles;
    QSet<QString> componentIds;
    for (const auto &value : components.value(QStringLiteral("components")).toArray()) {
        const auto component = value.toObject();
        const QString id = component.value(QStringLiteral("id")).toString();
        const auto identity = component.value(QStringLiteral("sourceIdentity")).toObject();
        const QString sourceId = identity.value(QStringLiteral("sourceId")).toString();
        const QString kind = identity.value(QStringLiteral("pinKind")).toString();
        const QString rights = component.value(QStringLiteral("redistribution")).toString();
        if (id.isEmpty() || componentIds.contains(id) || !sourceIds.contains(sourceId)
            || (kind != QStringLiteral("releasePin") && kind != QStringLiteral("referencePin"))
            || (rights != QStringLiteral("cleared") && rights != QStringLiteral("blocked")
                && rights != QStringLiteral("external"))) {
            errors << "Invalid component/source/rights identity: " << id << '\n';
            invalid = true;
            continue;
        }
        componentIds.insert(id);
        componentBundles.insert(id, component.value(QStringLiteral("bundle")).toString());

        // A component must not claim a pin kind its source does not have: that
        // would assert a byte-level identity the source only documents.
        if (sourcePinKinds.value(sourceId) != kind) {
            errors << id << ": pinKind " << kind << " contradicts source " << sourceId << '\n';
            invalid = true;
        }
        // A component may only ship a digest that is a real pin. Being merely
        // observed is not enough; being independently pinned upstream while also
        // having been observed is fine, which is why descriptiveDigests (the four
        // plan hashes) is the set checked here rather than all observations.
        const QJsonArray componentFiles = identity.value(QStringLiteral("files")).toArray();
        for (const auto &file : componentFiles) {
            const QString digest = file.toObject().value(QStringLiteral("sha256")).toString();
            if (!isSha256(digest)) {
                errors << id << ": malformed component file digest\n";
                invalid = true;
            } else if (descriptiveDigests.contains(digest)) {
                errors << id << ": a component ships a descriptive observation digest as a pin\n";
                invalid = true;
            }
        }
        if (rights == QLatin1String("cleared") && !component.value(QStringLiteral("inBundle")).toBool()) {
            errors << id << ": cleared for redistribution but not in any bundle\n";
            invalid = true;
        }
        if (component.value(QStringLiteral("licence")).toObject().isEmpty()) {
            errors << id << ": no licence recorded\n";
            invalid = true;
        }
        if (rights == QLatin1String("blocked")) {
            errors << id << ": redistribution blocked: "
                   << component.value(QStringLiteral("blockerRef")).toString() << '\n';
            blocked = true;
        }
    }

    // Every component belongs to exactly one declared bundle, and every bundle
    // entry resolves, so a component cannot be silently dropped from a manifest.
    QMap<QString, QStringList> bundleMembers;
    for (const auto &value : components.value(QStringLiteral("bundles")).toArray()) {
        const auto bundle = value.toObject();
        const QString bundleId = bundle.value(QStringLiteral("id")).toString();
        const QString decision = bundle.value(QStringLiteral("shippingDecision")).toString();
        if (bundleId.isEmpty() || bundleMembers.contains(bundleId)) {
            errors << "Invalid or duplicate bundle id: " << bundleId << '\n';
            invalid = true;
            continue;
        }
        if (decision != QLatin1String("cleared") && decision != QLatin1String("external")
            && decision != QLatin1String("blocked") && decision != QLatin1String("privateLocal")) {
            errors << "Invalid bundle shippingDecision: " << bundleId << '\n';
            invalid = true;
        }
        for (const auto &member : bundle.value(QStringLiteral("components")).toArray()) {
            const QString memberId = member.toString();
            if (!componentIds.contains(memberId)) {
                errors << "bundle " << bundleId << ": undeclared component " << memberId << '\n';
                invalid = true;
                continue;
            }
            if (componentBundles.value(memberId) != bundleId) {
                errors << memberId << ": listed under bundle " << bundleId
                       << " but declares bundle " << componentBundles.value(memberId) << '\n';
                invalid = true;
            }
            bundleMembers[bundleId].append(memberId);
        }
    }
    // Every component must appear in the manifest of the bundle it declares, or
    // it could be shipped without a shipping decision ever being checked.
    for (auto it = bundleMembers.cbegin(); it != bundleMembers.cend(); ++it) {
        for (const auto &memberId : it.value()) componentBundles.remove(memberId);
    }
    for (auto it = componentBundles.cbegin(); it != componentBundles.cend(); ++it) {
        errors << it.key() << ": declares bundle " << it.value() << " but is not listed in it\n";
        invalid = true;
    }

    output << "Files with matching recorded digests: " << verified << '\n';
    output << "Observation digests format-checked (never recomputed): " << observationCount << '\n';
    if (invalid) return 1;
    if (blocked) return 3;
    if (incomplete) return 2;
    output << "Source file integrity and component identity checks passed; this is not bundle loadability or runtime proof.\n";
    return 0;
}