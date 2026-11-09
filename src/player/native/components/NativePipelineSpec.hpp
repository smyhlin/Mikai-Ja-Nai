#pragma once

#include "AnimeJanaiRuntime.hpp"

#include <QSize>
#include <QString>
#include <QStringList>

namespace mplayer::native {

/// The replay description of one native plan: the decoder that produces the
/// frames the backend binds, and the filter string itself. Both the playing
/// surface and the comparison renderer must load exactly this, because a
/// comparison that builds its own variant measures a different program - which
/// is how a pane could configure a stage and still show a pass-through.
struct NativePipelineSpec {
    /// mpv's own decoder method name. Empty when the plan has no native stage.
    QString hwdec;
    /// The complete vf entry, byte for byte what the player attaches.
    QString vf;
    /// Every AnimeJaNai stage label in chain order, for build evidence.
    QStringList ajiLabels;
    /// The geometry the plan's stages declare, given the source's coded size.
    QSize expectedOutput;
};

inline QSize nativeExpectedOutput(const AnimeJanaiPlan& plan, const QSize& coded) {
    int scale = 1;
    for (const AnimeJanaiPlanStage& stage : plan.stages) scale = qMax(scale, stage.scaleFactor);
    if (!coded.isValid() || scale <= 1) return coded;
    return QSize(qMin(7680, coded.width() * scale), qMin(4320, coded.height() * scale));
}

/// `coded` is the source's coded geometry (`video-params/w,h`), not the display
/// size: the fork converts from the decoded frame.
inline NativePipelineSpec nativePipelineFor(const AnimeJanaiPlan& plan, const QSize& coded) {
    NativePipelineSpec spec;
    if (!plan.enabled) return spec;
    // The TensorRT family, classic and RTX alike, consumes CUDA frames; only
    // DirectML binds D3D11 textures. A `-copy` variant would download the frame,
    // so the method - not its availability - is what the backend needs.
    spec.hwdec = isTensorRtFamily(plan.backend) ? QStringLiteral("nvdec")
                                                : QStringLiteral("d3d11");
    spec.vf = QString::fromUtf8(plan.filterSpec);
    for (const AnimeJanaiPlanStage& stage : plan.stages) {
        if (stage.kind == AnimeJanaiPlanStage::Kind::Aji && !stage.filterLabel.isEmpty() &&
            !spec.ajiLabels.contains(stage.filterLabel)) {
            spec.ajiLabels.append(stage.filterLabel);
        }
    }
    if (spec.ajiLabels.isEmpty() && plan.requiresAji)
        spec.ajiLabels.append(QString::fromLatin1(kAnimeJanaiFilterLabel));
    spec.expectedOutput = nativeExpectedOutput(plan, coded);
    return spec;
}

/// True when the decoder the core reports really feeds the backend: CUDA interop
/// for the TensorRT family, D3D11 textures for DirectML. A software decoder or
/// any `-copy` variant hands mpv system-memory frames, which carry no device the
/// stage can adopt.
inline bool decoderFeedsNativeBackend(const QString& decoder, Backend backend) {
    const QString name = decoder.trimmed().toLower();
    if (name.isEmpty() || name == QLatin1String("no") ||
        name.endsWith(QLatin1String("-copy"))) {
        return false;
    }
    if (isTensorRtFamily(backend)) {
        return name.startsWith(QLatin1String("nvdec")) ||
               name.startsWith(QLatin1String("cuda"));
    }
    return name.startsWith(QLatin1String("d3d11"));
}

}  // namespace mplayer::native
