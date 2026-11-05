#include "NativeUpscalers.hpp"

#include "native/components/AnimeJanaiConfig.hpp"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>

namespace mplayer {
namespace native {

namespace {

// The licence position is the acquisition record's, not an invention: neither
// publisher ships a licence in the download and this project redistributes
// neither graph. The viewer fetches the pinned bytes for private use, and the
// catalogue states exactly that instead of implying a permission that was never
// granted.
const QString kUndeclaredLicense = QStringLiteral(
    "The model card declares no licence. Downloaded by the viewer for private use; "
    "this project does not redistribute these bytes.");

QVector<UpscalerModel> makeCatalog() {
    return {
        {QStringLiteral("sha256:78baa685a1a92cac6e14ab2af2a8ad0ef56fa48629563b1414e3a7acc54d86e2"),
         QStringLiteral("4x Real-ESRGAN AnimeVideo v3 (fp32)"),
         QStringLiteral("realesr-animevideov3-4x-fp32.onnx"),
         QStringLiteral("https://huggingface.co/buckets/smyhlin/realesr-animevideov3-bucket/"
                        "resolve/model.onnx?download=true"),
         2492908,
         QStringLiteral("78baa685a1a92cac6e14ab2af2a8ad0ef56fa48629563b1414e3a7acc54d86e2"),
         4,
         QStringLiteral("fp32"),
         QStringLiteral("https://huggingface.co/buckets/smyhlin/realesr-animevideov3-bucket"),
         kUndeclaredLicense},
        {QStringLiteral("sha256:3e8ba0496441c428408c9b8f1192d956100095f4a978322aa93fcd630ce144c9"),
         QStringLiteral("4x Real-ESRGAN AnimeVideo v3 (fp16)"),
         QStringLiteral("realesr-animevideov3-4x-fp16.onnx"),
         QStringLiteral("https://huggingface.co/buckets/smyhlin/realesr-animevideov3-bucket/"
                        "resolve/model_fp16.onnx?download=true"),
         1253495,
         QStringLiteral("3e8ba0496441c428408c9b8f1192d956100095f4a978322aa93fcd630ce144c9"),
         4,
         QStringLiteral("fp16"),
         QStringLiteral("https://huggingface.co/buckets/smyhlin/realesr-animevideov3-bucket"),
         kUndeclaredLicense},
        {QStringLiteral("sha256:eef4f756cebad996c95d11aa863cba0d70174ec2865f87d5b1bb5cc69578ff3c"),
         QStringLiteral("2x HFA2kCompact (fp32)"),
         QStringLiteral("2xHFA2kCompact-fp32-opset17.onnx"),
         QStringLiteral("https://huggingface.co/nesaorg/2xHFA2kCompact_fp32_opset17/resolve/main/"
                        "2xHFA2kCompact_fp32_opset17.onnx"),
         2412381,
         QStringLiteral("eef4f756cebad996c95d11aa863cba0d70174ec2865f87d5b1bb5cc69578ff3c"),
         2,
         QStringLiteral("fp32"),
         QStringLiteral("https://huggingface.co/nesaorg/2xHFA2kCompact_fp32_opset17"),
         kUndeclaredLicense},
        {QStringLiteral("sha256:aac5853cc061a53126d733c16e0a2532ec4c029c1449fdede40d5139c995d3ff"),
         QStringLiteral("2x HFA2kCompact multijpg (fp32)"),
         QStringLiteral("2xHFA2k-compact-multijpg-fp32-opset17.onnx"),
         QStringLiteral("https://huggingface.co/nesaorg/"
                        "2xHFA2k_compact_multijpg_fp32_opset17/resolve/main/"
                        "2xHFA2k_compact_multijpg_fp32_opset17.onnx"),
         2412381,
         QStringLiteral("aac5853cc061a53126d733c16e0a2532ec4c029c1449fdede40d5139c995d3ff"),
         2,
         QStringLiteral("fp32"),
         QStringLiteral("https://huggingface.co/nesaorg/2xHFA2k_compact_multijpg_fp32_opset17"),
         kUndeclaredLicense},
    };
}

}  // namespace

bool verifyUpscalerBytes(const UpscalerModel& model, const QByteArray& bytes,
                         QString* reasonOut) {
    const auto fail = [reasonOut](const QString& reason) {
        if (reasonOut != nullptr) *reasonOut = reason;
        return false;
    };
    if (model.sha256.isEmpty() || model.bytes <= 0) return fail(QStringLiteral("invalid pin"));
    // Length first: a truncated transfer never has to be hashed to be refused.
    if (bytes.size() != model.bytes) {
        return fail(QStringLiteral("expected %1 bytes, received %2")
                        .arg(model.bytes)
                        .arg(bytes.size()));
    }
    const QString actual = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    if (actual != model.sha256) {
        return fail(QStringLiteral("digest mismatch: expected %1, received %2")
                        .arg(model.sha256, actual));
    }
    if (reasonOut != nullptr) reasonOut->clear();
    return true;
}

QString installedUpscalerPath(const UpscalerModel& model) {
    const QString path = QDir(customAnimeJanaiModelsDir()).filePath(model.fileName);
    return QFileInfo(path).isFile() ? path : QString();
}

QString installUpscalerBytes(const UpscalerModel& model, const QByteArray& bytes,
                             QString* reasonOut) {
    const auto fail = [reasonOut](const QString& reason) {
        if (reasonOut != nullptr) *reasonOut = reason;
        return QString();
    };
    QString verifyReason;
    if (!verifyUpscalerBytes(model, bytes, &verifyReason)) return fail(verifyReason);

    const QString directory = customAnimeJanaiModelsDir();
    if (directory.isEmpty()) return fail(QStringLiteral("the model store is unavailable"));
    if (!QDir().mkpath(directory)) return fail(QStringLiteral("cannot create %1").arg(directory));
    const QString target = QDir(directory).filePath(model.fileName);

    // A graph already registered under this name is never rewritten: a second
    // download of the same pin is a no-op, and different bytes under the same
    // name would silently invalidate the integrity record beside it.
    if (QFileInfo(target).isFile()) {
        QFile existing(target);
        if (existing.open(QIODevice::ReadOnly) && existing.readAll() == bytes) {
            CustomAnimeJanaiModel registered;
            QString reason;
            if (registerOwnedCustomAnimeJanaiModel(target, &registered, &reason)) {
                if (reasonOut != nullptr) reasonOut->clear();
                return registered.id;
            }
            // Already registered with a sidecar: report its identity.
            if (!reason.isEmpty() && reason.contains(QStringLiteral("already"), Qt::CaseInsensitive)) {
                if (reasonOut != nullptr) reasonOut->clear();
                return QStringLiteral("sha256:") + model.sha256;
            }
            return fail(reason);
        }
        return fail(QStringLiteral("%1 already exists with different bytes").arg(model.fileName));
    }

    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly)) return fail(file.errorString());
    if (file.write(bytes) != bytes.size()) return fail(file.errorString());
    if (!file.commit()) return fail(file.errorString());

    CustomAnimeJanaiModel registered;
    QString reason;
    if (!registerOwnedCustomAnimeJanaiModel(target, &registered, &reason)) {
        // The graph is ours and unregistered; dropping it keeps the store honest
        // instead of leaving an executable-looking file nothing vouches for.
        QFile::remove(target);
        return fail(reason);
    }
    if (reasonOut != nullptr) reasonOut->clear();
    return registered.id;
}

bool removeInstalledUpscaler(const UpscalerModel& model, QString* reasonOut) {
    const QString path = installedUpscalerPath(model);
    if (path.isEmpty()) {
        if (reasonOut != nullptr) *reasonOut = QStringLiteral("not installed");
        return false;
    }
    QString reason;
    const bool removed = removeCustomAnimeJanaiModel(model.fileName, &reason);
    if (!removed) {
        if (reasonOut != nullptr) *reasonOut = reason;
        return false;
    }
    if (reasonOut != nullptr) reasonOut->clear();
    return true;
}

QVector<UpscalerModel> upscalerCatalog() {
    return makeCatalog();
}

std::optional<UpscalerModel> upscalerForId(QStringView idOrName) {
    const QString key = idOrName.toString().trimmed();
    if (key.isEmpty()) return std::nullopt;
    for (const UpscalerModel& model : makeCatalog()) {
        // The row's own stem, never the caller's: comparing the key with a stem
        // derived from itself would match the first row for every input.
        if (key == model.id || key == model.fileName ||
            key == QFileInfo(model.fileName).completeBaseName()) {
            return model;
        }
    }
    return std::nullopt;
}

std::optional<UpscalerModel> upscalerForDigest(QStringView sha256) {
    const QString key = sha256.toString().trimmed().toLower();
    for (const UpscalerModel& model : makeCatalog()) {
        if (key == model.sha256) return model;
    }
    return std::nullopt;
}

}  // namespace native
}  // namespace mplayer
