#include "NativeUserRecipes.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QSaveFile>
#include <QtCore/QStringList>

#include "NativePaths.hpp"

namespace mplayer {
namespace native {

namespace {

constexpr const char* kRecipeIdPrefix = "recipe:";
constexpr const char* kContentIdPrefix = "sha256:";
constexpr const char* kRecipeDocumentSuffix = ".json";
/// A recipe document is a handful of fields; anything at this size is not one
/// this build wrote, and is refused rather than read whole.
constexpr qint64 kMaxRecipeDocumentBytes = 64 * 1024;
/// A bound on how many documents one directory may contribute, so a directory
/// that grew without the app cannot make a status tick unbounded.
constexpr int kMaxStoredRecipes = 256;

/// The one lookup with this file's context. Every literal it is handed must be
/// wrapped in QT_TRANSLATE_NOOP: that macro is what lupdate reads, and at run
/// time it is the literal itself.
QString translated(const char* message) {
    return QCoreApplication::translate("NativeUserRecipes", message);
}

void fail(QString* reasonKey, QString* reason, const QString& key, const QString& text) {
    if (reasonKey != nullptr) *reasonKey = key;
    if (reason != nullptr) *reason = text;
}

void clear(QString* reasonKey, QString* reason) {
    if (reasonKey != nullptr) reasonKey->clear();
    if (reason != nullptr) reason->clear();
}

/// The canonical body of a recipe: everything that decides what runs, in the
/// exact order it runs. `recipeToJson` is the one definition of that body, so a
/// stored document and the fingerprint in its own name can never drift apart;
/// `id` and `label` are the two members that do not change what runs, and both
/// are removed before hashing. Every writer and reader builds this object
/// through that one function, so the compact document it serializes to is the
/// same bytes on both sides.
QString bodyHashOf(const Recipe& recipe) {
    QJsonObject body = recipeToJson(recipe);
    body.remove(QStringLiteral("id"));
    body.remove(QStringLiteral("label"));
    const QByteArray canonical = QJsonDocument(body).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

/// The one definition of the one-stage spatial recipe a row and a backend mean,
/// with no id and no label. `spatialRecipeIdFor` hashes exactly this body and
/// `addSpatialModel` persists exactly this body, so the two can never disagree.
Recipe spatialRecipeFor(const ModelRecord& model, Backend backend) {
    Recipe recipe;
    recipe.backend = backend;
    SpatialStage stage;
    stage.modelId = model.id;
    recipe.spatial = {stage};
    return recipe;
}

/// The 64 lowercase hex characters of a content id ("sha256:<hex>").
QString contentHex(const QString& contentId) {
    const QString prefix = QString::fromLatin1(kContentIdPrefix);
    if (!contentId.startsWith(prefix)) return {};
    const QString digest = contentId.mid(prefix.size());
    return isSha256Hex(digest) ? digest : QString{};
}

/// The id a stored recipe whose body hashes to `hex` carries on `backend`. The
/// id is the document's own key as well as its name, and neither is an ordinal.
QString recipeIdFor(const QString& hex, Backend backend) {
    return QStringLiteral("recipe:sha256:%1:%2").arg(hex, stableName(backend));
}

/// The document name for a recipe whose body hashes to `hex`, on `backend`.
/// Both halves are already restricted to filename-safe characters by
/// construction (a lowercase hex digest and a stable backend name), so this is
/// safe to join.
QString fileNameFor(const QString& hex, Backend backend) {
    return QStringLiteral("%1-%2%3").arg(hex, stableName(backend),
                                         QString::fromLatin1(kRecipeDocumentSuffix));
}

/// The inverse of `fileNameFor`, or false when the name is not one this store
/// issues.
bool parseFileName(const QString& name, QString* hex, Backend* backend) {
    const QString suffix = QString::fromLatin1(kRecipeDocumentSuffix);
    if (!name.endsWith(suffix)) return false;
    const QString stem = name.left(name.size() - suffix.size());
    const qsizetype dash = stem.lastIndexOf(QLatin1Char('-'));
    if (dash < 0) return false;
    const QString digest = stem.left(dash);
    const QString token = stem.mid(dash + 1);
    if (!isSha256Hex(digest)) return false;
    const std::optional<Backend> parsed = backendFromName(token);
    if (!parsed.has_value()) return false;
    if (hex != nullptr) *hex = digest;
    if (backend != nullptr) *backend = *parsed;
    return true;
}

/// "recipe:sha256:<hex>:<backend>" back to its parts. A backend this build does
/// not know, or any other shape, is not an id this store issued.
bool parseRecipeId(const QString& id, QString* hex, Backend* backend) {
    const QString prefix = QString::fromLatin1(kRecipeIdPrefix) +
                           QString::fromLatin1(kContentIdPrefix);
    if (!id.startsWith(prefix)) return false;
    const QString rest = id.mid(prefix.size());
    const qsizetype colon = rest.lastIndexOf(QLatin1Char(':'));
    if (colon < 0) return false;
    const QString digest = rest.left(colon);
    const QString token = rest.mid(colon + 1);
    if (!isSha256Hex(digest)) return false;
    const std::optional<Backend> parsed = backendFromName(token);
    if (!parsed.has_value()) return false;
    if (hex != nullptr) *hex = digest;
    if (backend != nullptr) *backend = *parsed;
    return true;
}

/// Reads, parses and verifies one stored document. Every fact the document
/// carries is checked against the name it was found under, so a document can
/// never describe a recipe other than the one its name selects.
bool readRecipeDocument(const QString& root, const QString& hex, Backend backend, Recipe* out,
                        QString* reasonKey, QString* reason) {
    const QString name = fileNameFor(hex, backend);
    QString nameReason;
    const QString safe = safeRelativePath(name, &nameReason);
    if (safe.isEmpty()) {
        fail(reasonKey, reason, QStringLiteral("recipe.path.unsafe"),
             QStringLiteral("stored recipe name '%1' is not a safe relative path: %2")
                 .arg(name, nameReason));
        return false;
    }

    const QString candidate = QDir(root).filePath(safe);
    const QString canonical = canonicalPath(candidate);
    if (canonical.isEmpty()) {
        fail(reasonKey, reason, QStringLiteral("recipe.missing"),
             QStringLiteral("no stored recipe exists at %1").arg(candidate));
        return false;
    }
    if (!pathWithinRoot(root, canonical)) {
        fail(reasonKey, reason, QStringLiteral("recipe.path.escape"),
             QStringLiteral("stored recipe resolves outside the owned recipes root"));
        return false;
    }
    if (!pathHasNoReparsePoints(root, canonical)) {
        fail(reasonKey, reason, QStringLiteral("recipe.path.reparse"),
             QStringLiteral("stored recipe is reached through a reparse point inside %1").arg(root));
        return false;
    }

    const QFileInfo info(canonical);
    if (info.isSymLink() || !info.isFile()) {
        fail(reasonKey, reason, QStringLiteral("recipe.path.notRegular"),
             QStringLiteral("%1 is not a regular file").arg(canonical));
        return false;
    }
    if (info.size() > kMaxRecipeDocumentBytes) {
        fail(reasonKey, reason, QStringLiteral("recipe.document.tooLarge"),
             QStringLiteral("%1 is %2 bytes; a recipe document is far smaller")
                 .arg(name)
                 .arg(info.size()));
        return false;
    }

    QFile file(canonical);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(reasonKey, reason, QStringLiteral("recipe.document.unreadable"),
             QStringLiteral("%1 cannot be read: %2").arg(name, file.errorString()));
        return false;
    }
    const QByteArray payload = file.read(kMaxRecipeDocumentBytes + 1);
    if (file.error() != QFile::NoError) {
        fail(reasonKey, reason, QStringLiteral("recipe.document.unreadable"),
             QStringLiteral("%1 failed while reading: %2").arg(name, file.errorString()));
        return false;
    }
    if (payload.size() > kMaxRecipeDocumentBytes) {
        fail(reasonKey, reason, QStringLiteral("recipe.document.tooLarge"),
             QStringLiteral("%1 is larger than a recipe document can be").arg(name));
        return false;
    }

    Recipe recipe;
    QString errorKey;
    QString error;
    if (!parseRecipe(payload, &recipe, &errorKey, &error)) {
        fail(reasonKey, reason,
             errorKey.isEmpty() ? QStringLiteral("recipe.document.invalid") : errorKey,
             QStringLiteral("%1 does not parse: %2")
                 .arg(name, error.isEmpty() ? QStringLiteral("unknown document") : error));
        return false;
    }
    const RecipeVerdict verdict = validateRecipe(recipe);
    if (!verdict.ok) {
        fail(reasonKey, reason, verdict.reasonKey,
             QStringLiteral("%1 does not validate: %2").arg(name, verdict.reason));
        return false;
    }

    const QString expectedId = recipeIdFor(hex, backend);
    if (recipe.id != expectedId) {
        fail(reasonKey, reason, QStringLiteral("recipe.id.mismatch"),
             QStringLiteral("%1 carries id %2, which does not match its own name")
                 .arg(name, recipe.id));
        return false;
    }
    if (recipe.backend != backend) {
        fail(reasonKey, reason, QStringLiteral("recipe.backend.mismatch"),
             QStringLiteral("%1 carries backend %2, which does not match its own name")
                 .arg(name, stableName(recipe.backend)));
        return false;
    }
    // The body, not the label, is what the name encodes: a re-labelled document
    // is the same recipe, while a changed stage, order, factor, threshold or
    // backend is a document that no longer describes the recipe its own name
    // selects. It is refused rather than read as if it were.
    const QString actual = bodyHashOf(recipe);
    if (actual != hex) {
        fail(reasonKey, reason, QStringLiteral("recipe.body.mismatch"),
             QStringLiteral("%1 hashes to %2, which does not match the fingerprint in its own "
                            "name")
                 .arg(name, actual));
        return false;
    }

    *out = recipe;
    clear(reasonKey, reason);
    return true;
}

/// A cheap fingerprint of the owned directory: the names, sizes and stamps of
/// its documents. It changes whenever a document is added, removed or rewritten,
/// so the parsed cache can be trusted only while it is unchanged. No document is
/// opened to compute it.
QString directorySignature(const QString& root) {
    const QFileInfoList entries =
        QDir(root).entryInfoList({QStringLiteral("*") + QString::fromLatin1(kRecipeDocumentSuffix)},
                                 QDir::Files, QDir::Name);
    QStringList parts;
    parts.reserve(entries.size());
    for (const QFileInfo& entry : entries) {
        parts << QStringLiteral("%1|%2|%3")
                     .arg(entry.fileName())
                     .arg(entry.size())
                     .arg(entry.lastModified().toMSecsSinceEpoch());
    }
    return parts.join(QLatin1Char('\n'));
}

struct StoredRecipeCache {
    QString signature;
    QVector<Recipe> recipes;
};

struct StoredRecipeCacheRegistry {
    QMutex mutex;
    QHash<QString, StoredRecipeCache> byRoot;
};

/// Process-wide so a store constructed per status tick still benefits: the
/// directory fingerprint, not the store object, decides whether documents are
/// re-read.
StoredRecipeCacheRegistry& recipeCache() {
    static StoredRecipeCacheRegistry registry;
    return registry;
}

QString cacheKeyFor(const QString& root) {
    const QString canonical = canonicalPath(root);
    if (!canonical.isEmpty()) return canonical;
    return QDir::cleanPath(QFileInfo(root).absoluteFilePath());
}

void invalidateCache(const QString& root) {
    StoredRecipeCacheRegistry& registry = recipeCache();
    QMutexLocker locker(&registry.mutex);
    registry.byRoot.remove(cacheKeyFor(root));
}

}  // namespace

QString recipeBodyHash(const Recipe& recipe) { return bodyHashOf(recipe); }

QString storedRecipeIdFor(const Recipe& recipe) {
    return recipeIdFor(bodyHashOf(recipe), recipe.backend);
}

QString spatialRecipeIdFor(const ModelRecord& model, Backend backend) {
    if (!isValidModelId(model.id)) return {};
    return storedRecipeIdFor(spatialRecipeFor(model, backend));
}

QString UserRecipeStore::defaultRoot() { return nativeSubdirectory(QStringLiteral("recipes")); }

UserRecipeStore::UserRecipeStore() : root_(defaultRoot()), explicitRoot_(false) {}

UserRecipeStore::UserRecipeStore(QString root) : root_(std::move(root)), explicitRoot_(true) {}

bool UserRecipeStore::isOwned(QString* reason) const {
    if (root_.isEmpty()) {
        if (reason) *reason = QStringLiteral("no recipe store root was supplied");
        return false;
    }
    if (!QDir::isAbsolutePath(root_)) {
        if (reason) {
            *reason = QStringLiteral("recipe store root %1 is not absolute, so it cannot be "
                                     "contained").arg(root_);
        }
        return false;
    }
    // The app's own root must lie inside the app-owned recipes hierarchy. An
    // explicit root is caller-vouched - a test's temporary directory is the
    // reason the constructor exists - so it is used as given, but still never
    // reparse-redirected.
    if (!explicitRoot_) {
        QString ownedReason;
        const QString ownedRoot = nativeSubdirectory(QStringLiteral("recipes"), &ownedReason);
        if (ownedRoot.isEmpty()) {
            if (reason) {
                *reason = QStringLiteral("the app-owned recipes directory cannot be resolved: %1")
                              .arg(ownedReason);
            }
            return false;
        }
        const QString resolved = canonicalPath(root_);
        const QString compared = resolved.isEmpty() ? QDir::cleanPath(root_) : resolved;
        if (!pathWithinRoot(ownedRoot, compared)) {
            if (reason) {
                *reason = QStringLiteral("recipe store root %1 is outside the app-owned %2")
                              .arg(compared, ownedRoot);
            }
            return false;
        }
    }
    const QString canonical = canonicalPath(root_);
    if (canonical.isEmpty()) {
        // Absent. A root inside the owned hierarchy is created by `ensureOwned`,
        // so a not-yet-existing parent is expected there. An explicit root is
        // caller-vouched but still has to name a real place: its parent resolves
        // or the root is refused.
        if (explicitRoot_ && canonicalPath(QFileInfo(root_).absolutePath()).isEmpty()) {
            if (reason) {
                *reason = QStringLiteral("recipe store root %1 does not exist and its parent cannot "
                                         "be resolved").arg(root_);
            }
            return false;
        }
        clear(nullptr, reason);
        return true;
    }
    if (!QFileInfo(canonical).isDir()) {
        if (reason) *reason = QStringLiteral("recipe store root %1 is not a directory").arg(canonical);
        return false;
    }
    if (!pathHasNoReparsePoints(canonical, canonical)) {
        if (reason) *reason = QStringLiteral("recipe store root %1 is a reparse point").arg(canonical);
        return false;
    }
    clear(nullptr, reason);
    return true;
}

bool UserRecipeStore::ensureOwned(QString* reason) const {
    if (!isOwned(reason)) return false;
    if (QFileInfo(root_).isDir()) {
        clear(nullptr, reason);
        return true;
    }
    if (!QDir().mkpath(root_)) {
        if (reason) *reason = QStringLiteral("recipe store root %1 could not be created").arg(root_);
        return false;
    }
    clear(nullptr, reason);
    return true;
}

std::optional<Recipe> UserRecipeStore::saveRecipe(const Recipe& recipe, QString* reasonKey,
                                                  QString* reason) {
    const auto reject = [reasonKey, reason](const QString& key, const QString& text) {
        fail(reasonKey, reason, key, text);
        return std::optional<Recipe>();
    };

    // The id is derived from the body, never supplied: a caller cannot name a
    // recipe something its own stages do not say, and the id a request holds
    // always resolves to exactly these stages and this backend. It is assigned
    // before validation, because the id is itself one of the facts validation
    // checks, and no placeholder ever stands in for it.
    const QString hex = bodyHashOf(recipe);
    Recipe stored = recipe;
    stored.id = recipeIdFor(hex, stored.backend);

    const RecipeVerdict verdict = validateRecipe(stored);
    if (!verdict.ok) return reject(verdict.reasonKey, verdict.reason);

    if (root_.isEmpty()) {
        return reject(QStringLiteral("recipe.store.unowned"),
                      translated(QT_TRANSLATE_NOOP(
                          "NativeUserRecipes",
                          "No recipe store root was supplied.")));
    }
    if (!ensureOwned(reason)) {
        return reject(QStringLiteral("recipe.store.unowned"),
                      reason != nullptr && !reason->isEmpty()
                          ? *reason
                          : translated(QT_TRANSLATE_NOOP(
                                "NativeUserRecipes",
                                "The recipe store is not app-owned.")));
    }

    const QString name = fileNameFor(hex, stored.backend);
    const QString path = QDir(root_).filePath(name);
    if (QFileInfo::exists(path)) {
        // This exact body on this backend already resolves to this document. It
        // is read back rather than rewritten: a stored recipe is never silently
        // replaced, and the id a caller may already hold stays valid. A
        // document that no longer verifies is not "the same recipe".
        Recipe existing;
        QString key;
        QString text;
        if (!readRecipeDocument(root_, hex, stored.backend, &existing, &key, &text)) {
            return reject(QStringLiteral("recipe.corrupt"),
                          translated(QT_TRANSLATE_NOOP(
                              "NativeUserRecipes",
                              "A recipe document is already stored under %1 but does not "
                              "verify: %2"))
                              .arg(name, text.isEmpty() ? key : text));
        }
        clear(reasonKey, reason);
        return existing;
    }

    const QByteArray payload = serializeRecipe(stored);
    if (payload.isEmpty() || payload.size() > kMaxRecipeDocumentBytes) {
        return reject(QStringLiteral("recipe.document.tooLarge"),
                      translated(QT_TRANSLATE_NOOP(
                          "NativeUserRecipes",
                          "The recipe document would be larger than a stored recipe may "
                          "be.")));
    }
    {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return reject(QStringLiteral("recipe.write.failed"),
                          translated(QT_TRANSLATE_NOOP(
                              "NativeUserRecipes",
                              "The recipe could not be written: %1"))
                              .arg(file.errorString()));
        }
        if (file.write(payload) != payload.size() || !file.commit()) {
            return reject(QStringLiteral("recipe.write.failed"),
                          translated(QT_TRANSLATE_NOOP(
                              "NativeUserRecipes",
                              "The recipe could not be written: %1"))
                              .arg(file.errorString()));
        }
    }

    // The document that was written is the one that must read back, so a
    // serializer that drops a member, or a naming scheme that disagrees with its
    // own fingerprint, cannot ship a file this build cannot read.
    Recipe written;
    if (!readRecipeDocument(root_, hex, stored.backend, &written, reasonKey, reason)) {
        fail(reasonKey, reason, QStringLiteral("recipe.write.verify"),
             translated(QT_TRANSLATE_NOOP(
                 "NativeUserRecipes",
                 "The stored recipe does not read back: %1"))
                 .arg(reason != nullptr && !reason->isEmpty() ? *reason : QString()));
        return std::nullopt;
    }
    invalidateCache(root_);
    clear(reasonKey, reason);
    return written;
}

std::optional<Recipe> UserRecipeStore::addSpatialModel(const ModelRecord& model, Backend backend,
                                                       QString* reasonKey, QString* reason) {
    const auto reject = [reasonKey, reason](const QString& key, const QString& text) {
        fail(reasonKey, reason, key, text);
        return std::optional<Recipe>();
    };

    if (model.role != ModelRole::Spatial) {
        return reject(QStringLiteral("recipe.model.role"),
                      translated(QT_TRANSLATE_NOOP(
                          "NativeUserRecipes",
                          "A single-stage spatial recipe can only be built from a spatial "
                          "model.")));
    }

    // The row's own refusal is the profile, digest and identity check: an
    // ineligible boundary, a missing digest or a built-in/user mismatch is
    // reported with its own key rather than smoothed over here.
    if (!model.isOffered()) {
        QString key;
        QString text;
        model.refusal(&key, &text);
        return reject(key.isEmpty() ? QStringLiteral("recipe.model.refused") : key, text);
    }
    // A row that is not Ready names why (an unsupported dtype, a changed source);
    // offering it as a recipe would pretend that gap away.
    if (!model.availability.isUsable()) {
        return reject(QStringLiteral("recipe.model.notReady"),
                      translated(QT_TRANSLATE_NOOP(
                          "NativeUserRecipes",
                          "Model %1 is not ready to be offered: %2"))
                          .arg(model.id, model.availability.reason.isEmpty()
                                             ? translated(QT_TRANSLATE_NOOP(
                                                   "NativeUserRecipes",
                                                   "no reason was recorded"))
                                             : model.availability.reason));
    }

    // A content-addressed id must name the content it claims. An id that does
    // not match its own hash is not a stable identity and cannot key a recipe.
    if (isUserModelId(model.id)) {
        if (model.id != userModelIdFor(model.contentId())) {
            return reject(QStringLiteral("recipe.model.idInconsistent"),
                          translated(QT_TRANSLATE_NOOP(
                              "NativeUserRecipes",
                              "Model id %1 does not match its own content hash, so its "
                              "identity is not deterministic."))
                              .arg(model.id));
        }
    }
    // A row whose content identity is not even well formed cannot contribute a
    // stage a later join could resolve.
    if (contentHex(model.contentId()).isEmpty()) {
        return reject(QStringLiteral("recipe.model.contentId"),
                      translated(QT_TRANSLATE_NOOP(
                          "NativeUserRecipes",
                          "Model %1 has no usable content identity."))
                          .arg(model.id));
    }

    // One spatial stage, named and stored by the store's own body fingerprint:
    // `spatialRecipeIdFor` reports the id of exactly this body.
    Recipe recipe = spatialRecipeFor(model, backend);
    recipe.label = model.displayName.isEmpty()
                       ? QStringLiteral("%1 (%2)").arg(model.id, stableName(backend))
                       : QStringLiteral("%1 (%2)").arg(model.displayName, stableName(backend));
    return saveRecipe(recipe, reasonKey, reason);
}

std::optional<Recipe> UserRecipeStore::find(const QString& id, QString* reasonKey,
                                            QString* reason) const {
    QString hex;
    Backend backend = Backend::TensorRT;
    if (!parseRecipeId(id, &hex, &backend)) {
        fail(reasonKey, reason, QStringLiteral("recipe.id.invalid"),
             translated(QT_TRANSLATE_NOOP(
                 "NativeUserRecipes",
                 "'%1' is not a recipe id this store issues."))
                 .arg(id));
        return std::nullopt;
    }
    if (root_.isEmpty()) {
        fail(reasonKey, reason, QStringLiteral("recipe.store.unowned"),
             translated(QT_TRANSLATE_NOOP(
                 "NativeUserRecipes",
                 "No recipe store root was supplied.")));
        return std::nullopt;
    }
    if (!isOwned(reason)) {
        fail(reasonKey, reason, QStringLiteral("recipe.store.unowned"),
             reason != nullptr && !reason->isEmpty()
                 ? *reason
                 : translated(QT_TRANSLATE_NOOP(
                       "NativeUserRecipes",
                       "The recipe store is not app-owned.")));
        return std::nullopt;
    }
    Recipe recipe;
    if (!readRecipeDocument(root_, hex, backend, &recipe, reasonKey, reason)) return std::nullopt;
    return recipe;
}

QVector<Recipe> UserRecipeStore::recipes(QString* reason) const {
    QVector<Recipe> result;
    if (root_.isEmpty()) {
        if (reason) *reason = translated(QT_TRANSLATE_NOOP(
                                  "NativeUserRecipes",
                                  "No recipe store root was supplied."));
        return result;
    }
    if (!isOwned(reason)) return result;

    const QString key = cacheKeyFor(root_);
    const QString signature = directorySignature(root_);
    {
        StoredRecipeCacheRegistry& registry = recipeCache();
        QMutexLocker locker(&registry.mutex);
        const auto it = registry.byRoot.constFind(key);
        if (it != registry.byRoot.constEnd() && it->signature == signature) {
            if (reason) reason->clear();
            return it->recipes;
        }
    }

    const QFileInfoList entries =
        QDir(root_).entryInfoList({QStringLiteral("*") + QString::fromLatin1(kRecipeDocumentSuffix)},
                                  QDir::Files, QDir::Name);
    for (const QFileInfo& entry : entries) {
        if (result.size() >= kMaxStoredRecipes) break;
        QString hex;
        Backend backend = Backend::TensorRT;
        if (!parseFileName(entry.fileName(), &hex, &backend)) continue;
        Recipe recipe;
        if (!readRecipeDocument(root_, hex, backend, &recipe, nullptr, nullptr)) continue;
        result.append(recipe);
    }

    {
        StoredRecipeCacheRegistry& registry = recipeCache();
        QMutexLocker locker(&registry.mutex);
        registry.byRoot.insert(key, StoredRecipeCache{signature, result});
    }
    if (reason) reason->clear();
    return result;
}

}  // namespace native
}  // namespace mplayer
