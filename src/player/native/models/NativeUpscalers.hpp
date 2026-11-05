// The optional upscalers this build offers beside Anime4K: two ONNX graphs
// pinned by content digest, their download identity, and the licence facts that
// travel with them.
//
// These are not AnimeJaNai's five spatial graphs: they are user-requested
// upscalers (one 4x Real-ESRGAN AnimeVideoV3 in fp32/fp16, one 2x HFA2kCompact in
// two training variants) that land in the same app-owned custom-model store an
// imported graph does, because that is the store a chain step can name. Nothing
// is bundled: a build ships this catalogue, and a download happens only when the
// viewer asks for one.

#pragma once

#include <optional>

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringView>
#include <QtCore/QVector>

namespace mplayer {
namespace native {

/// One pinned upscaler. The digest is the identity; the name is a label.
struct UpscalerModel {
    /// "sha256:<64 hex>", the store's stable id for these bytes.
    QString id;
    /// The label shown to a viewer.
    QString displayName;
    /// The file name the graph is stored under in the custom-model directory.
    QString fileName;
    /// Where the pinned bytes are downloaded from.
    QString url;
    qint64 bytes = 0;
    /// Lowercase hex SHA-256, never the truncated form.
    QString sha256;
    /// The upscale factor the graph itself performs.
    int scale = 2;
    /// "fp32" or "fp16"; both are offered where the publisher ships both.
    QString precision;
    /// Who published the graph and where its card is.
    QString sourceCard;
    /// What the publisher declares about reuse. These are not redistributed by
    /// this project; the viewer downloads them for private use.
    QString license;
};

/// The four offered graphs, in menu order: 4x first, then 2x.
QVector<UpscalerModel> upscalerCatalog();

/// Resolves a catalogue entry from its id, its stored file name or its stem.
std::optional<UpscalerModel> upscalerForId(QStringView idOrName);

/// The catalogue entry whose pinned bytes hash to `sha256`.
std::optional<UpscalerModel> upscalerForDigest(QStringView sha256);

/// True when `bytes` are exactly the pinned graph: length first, then digest.
/// A download is proved before a single byte reaches the store.
bool verifyUpscalerBytes(const UpscalerModel& model, const QByteArray& bytes,
                         QString* reasonOut = nullptr);

/// Stores verified bytes in the app-owned custom-model directory under the
/// catalogue's file name and publishes their integrity record, so a chain step
/// can name the graph. Refuses anything that does not verify, and never
/// replaces an existing registered graph. Returns the registered identifier.
QString installUpscalerBytes(const UpscalerModel& model, const QByteArray& bytes,
                             QString* reasonOut = nullptr);

/// Removes a stored upscaler and its integrity record. False when nothing owned
/// matches, which is not an error the caller has to invent words for.
bool removeInstalledUpscaler(const UpscalerModel& model, QString* reasonOut = nullptr);

/// Where a stored upscaler lives, or an empty path when it is not installed.
QString installedUpscalerPath(const UpscalerModel& model);

}  // namespace native
}  // namespace mplayer
