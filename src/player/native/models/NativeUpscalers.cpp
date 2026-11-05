#include "NativeUpscalers.hpp"

#include <QtCore/QFileInfo>

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
