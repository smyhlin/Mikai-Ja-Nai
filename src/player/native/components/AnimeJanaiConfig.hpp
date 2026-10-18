#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QStringView>
#include <QtCore/QVector>

#include <functional>
#include <optional>

namespace mplayer::native {

struct CustomAnimeJanaiModel {
    QString id;
    QString fileName;
    QString displayName;
    QString filePath;
    qint64 bytes = 0;
    QString sha256;
    int scaleFactor = 2;       ///< Filename-inferred hint, not graph inspection.
    QString architectureHint;  ///< Filename-inferred hint, not runtime compatibility.
    bool valid = false;
    QString reason;
};

/// Directory where user-imported custom AnimeJaNai ONNX models are stored
/// (`<nativeRoot>/animejanai/models/custom`).
QString customAnimeJanaiModelsDir();

/// Validates whether a candidate file is a plausible ONNX binary graph.
bool validateCustomOnnxFile(const QString& filePath, qint64* bytesOut = nullptr,
                            QString* reasonOut = nullptr);

QVector<CustomAnimeJanaiModel> listCustomAnimeJanaiModels();

std::optional<CustomAnimeJanaiModel> findCustomAnimeJanaiModel(const QString& idOrStem);

/// Lists external-folder ONNX candidates; unpinned candidates are invalid until explicitly registered.
QVector<CustomAnimeJanaiModel> listExternalCustomAnimeJanaiModelCandidates(
    QString* errorOut = nullptr);
QVector<CustomAnimeJanaiModel> listExternalCustomAnimeJanaiModelCandidates(
    const QString& externalDirectory, QString* errorOut = nullptr);

/// Pins an external model in an app-owned registry. Call on a worker; never writes beside the model.
bool registerExternalCustomAnimeJanaiModel(const QString& sourceFilePath,
                                            CustomAnimeJanaiModel* registeredOut = nullptr,
                                            QString* errorOut = nullptr,
                                            const std::function<bool()>& cancelled = {},
                                            const QString& externalDirectory = {});

/// Atomically imports a user `.onnx` file into `<nativeRoot>/animejanai/models/custom/`.
bool importCustomAnimeJanaiModel(const QString& sourceFilePath,
                                 CustomAnimeJanaiModel* importedOut = nullptr,
                                 QString* errorOut = nullptr,
                                 const std::function<bool()>& cancelled = {});

/// External graphs are borrowed; removing their registration never removes the graph.
bool removeCustomAnimeJanaiModel(const QString& id, QString* errorOut = nullptr);

struct AnimeJanaiChainModel {
    int number = 1;
    QString name;
    int resizeHeightBeforeUpscale = 0;    ///< px (0 = disabled)
    int resizeFactorBeforeUpscale = 100;  ///< percent (100 = no pre-resize)

    bool operator==(const AnimeJanaiChainModel&) const = default;
};

struct AnimeJanaiChain {
    int number = 1;
    QString minResolution = QStringLiteral("0x0");
    QString maxResolution = QStringLiteral("0x0");
    int minFps = 0;
    int maxFps = 0;
    QVector<AnimeJanaiChainModel> models;
    bool rifeEnabled = false;
    QString rifeModel = QStringLiteral("RIFE 4.26");
    int rifeFactorNumerator = 2;
    int rifeFactorDenominator = 1;
    double rifeSceneDetectThreshold = 0.150;
    bool rifeEnsemble = false;
    bool rifeBeforeUpscale = true;

    bool operator==(const AnimeJanaiChain&) const = default;
};

struct AnimeJanaiSlot {
    int number = 1;
    QString profileName = QStringLiteral("Custom Profile");
    QString descriptionText;
    QVector<AnimeJanaiChain> chains;

    bool operator==(const AnimeJanaiSlot&) const = default;
};

struct AnimeJanaiConfig {
    bool enableLogging = true;
    bool qualitySharp = false;
    bool balancedSharp = false;
    bool performanceSharp = false;
    QString trtEngineSettings = QStringLiteral(
        "--builderOptimizationLevel=5 --optShapes=input:%video_resolution% --skipInference");
    QString externalModelsDir;
    QVector<AnimeJanaiSlot> customSlots;

    bool operator==(const AnimeJanaiConfig&) const = default;
};

/// AJI schema-v3 INI and AnimeJaNaiManager .pconf files share the same slot keys.
class AnimeJanaiConfigStore final {
public:
    static QString configFilePath();

    static AnimeJanaiConfig load(QString* error = nullptr);
    static AnimeJanaiConfig loadFromFile(const QString& path, QString* error = nullptr);
    static AnimeJanaiConfig parse(QStringView text);
    static bool save(const AnimeJanaiConfig& config, QString* error = nullptr);
    static bool saveToFile(const QString& path, const AnimeJanaiConfig& config,
                           QString* error = nullptr);
    static QString serialize(const AnimeJanaiConfig& config);

    static AnimeJanaiSlot loadProfile(const QString& path, bool* ok = nullptr,
                                      QString* error = nullptr);
    static AnimeJanaiSlot parseProfile(QStringView text, bool* ok = nullptr,
                                       QString* error = nullptr);
    static bool saveProfile(const QString& path, const AnimeJanaiSlot& slot,
                            QString* error = nullptr);
    static QString serializeProfile(const AnimeJanaiSlot& slot);
    static QString serializeSlotSection(const AnimeJanaiSlot& slot,
                                        int overrideSlotNumber = -1);

    static QVector<AnimeJanaiSlot> defaultProfiles(const AnimeJanaiConfig& config);

    static QStringList availableUpscaleModelNames();
    static QStringList availableRifeModelLabels();

    static bool trtDynamicSelected(QStringView settings);
    static bool trtStaticSelected(QStringView settings);
    static bool trtStaticOnnxSelected(QStringView settings);
    static QString trtDynamicMinResolution(QStringView settings);
    static QString trtDynamicOptResolution(QStringView settings);
    static QString trtDynamicMaxResolution(QStringView settings);
    static QString setTrtDynamicMinResolution(QString settings, QStringView resolution);
    static QString setTrtDynamicOptResolution(QString settings, QStringView resolution);
    static QString setTrtDynamicMaxResolution(QString settings, QStringView resolution);
    static QString trtBuilderOptimizationLevel(QStringView settings);
    static QString setTrtBuilderOptimizationLevel(QString settings, QStringView level);
    static QString setTrtStaticOnnx(QString settings);
    static QString setTrtStatic(QString settings);
    static QString setTrtDynamic(QString settings);

    static QString rifeLabelToValue(QStringView label);
    static QString rifeValueToLabel(QStringView value);
};

}  // namespace mplayer::native
