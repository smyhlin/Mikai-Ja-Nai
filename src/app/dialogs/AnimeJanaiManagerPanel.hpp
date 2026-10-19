#pragma once

#include <native/components/AnimeJanaiConfig.hpp>

#include <QWidget>
#include <atomic>
#include <memory>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QVBoxLayout;

namespace mikai::app {
class Runner;

class AnimeJanaiManagerPanel : public QWidget {
    Q_OBJECT

public:
    explicit AnimeJanaiManagerPanel(QWidget* parent = nullptr, Runner* runner = nullptr);
    ~AnimeJanaiManagerPanel() override;

    bool isDirty() const { return dirty_; }
    mplayer::native::AnimeJanaiConfig currentConfig() const;
    void reloadFromDisk();
    void refreshCustomModels();
    bool saveConfig(QString* error = nullptr);
    void retranslateUi();
    void showSaveError(const QString& message);

signals:
    void configModified();
    void modelsChanged();

private:
    struct ModelStepRow {
        QFrame* frame = nullptr;
        QLabel* stepLabel = nullptr;
        QComboBox* kindCombo = nullptr;
        QLabel* modelLabel = nullptr;
        QComboBox* modelCombo = nullptr;
        QLabel* resizeHeightLabel = nullptr;
        QSpinBox* resizeHeightSpin = nullptr;
        QLabel* resizeFactorLabel = nullptr;
        QSpinBox* resizeFactorSpin = nullptr;
        QLabel* scaleLabel = nullptr;
        QComboBox* scaleCombo = nullptr;
        QPushButton* moveUpButton = nullptr;
        QPushButton* moveDownButton = nullptr;
        QPushButton* removeButton = nullptr;
    };

    void buildUi();
    void loadConfigIntoWidgets(const mplayer::native::AnimeJanaiConfig& config);
    void syncTrtWidgetsFromText(const QString& settingsText);
    void rebuildSlotCombo();
    void loadSelectedSlotIntoEditor();
    void rebuildChainCombo();
    void loadSelectedChainIntoEditor();
    void rebuildModelStepRows();
    void commitCurrentChainFromEditor();
    void updateStatusBanner(const QString& message, const QString& tone = QStringLiteral("info"));
    void markDirty();

    void onImportOnnxModels();
    void onRemoveSelectedOnnxModel();
    void onOpenCustomModelsFolder();
    void onBrowseExternalModelsFolder();
    void onClearExternalModelsFolder();
    void onImportFullConf();
    void onExportFullConf();
    void onResetDefaults();
    void onCreateOrResetSlot();
    void onCloneSlotMenu();
    void onDeleteSlot();
    void onImportProfilePconf();
    void onExportProfilePconf();
    void onAddChain();
    void onRemoveChain();
    void onMoveChainUp();
    void onMoveChainDown();
    void onAddModelStep();
    void onRemoveModelStep(int index);
    void onMoveModelStep(int index, int direction);

    int currentSlotNumber() const;
    mplayer::native::AnimeJanaiSlot* findMutableSlot(int slotNumber);
    const mplayer::native::AnimeJanaiSlot* findSlot(int slotNumber) const;
    mplayer::native::AnimeJanaiChain* currentMutableChain();

    Runner* runner_ = nullptr;
    int editingSlotNumber_ = 1;
    int editingChainIndex_ = 0;
    std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
    mplayer::native::AnimeJanaiConfig config_;
    bool updatingUi_ = false;
    bool dirty_ = false;

    QLabel* bannerLabel_ = nullptr;
    QLabel* modelsHeading_ = nullptr;
    QLabel* modelsSubtitle_ = nullptr;
    QLabel* modelsCountBadge_ = nullptr;
    QTableWidget* customModelsTable_ = nullptr;
    QPushButton* importModelButton_ = nullptr;
    QPushButton* removeModelButton_ = nullptr;
    QPushButton* openModelsDirButton_ = nullptr;
    QLabel* externalDirLabel_ = nullptr;
    QLineEdit* externalDirEdit_ = nullptr;
    QPushButton* browseExternalDirButton_ = nullptr;
    QPushButton* clearExternalDirButton_ = nullptr;

    QLabel* globalHeading_ = nullptr;
    QLabel* globalSubtitle_ = nullptr;
    QLabel* qualityVariantLabel_ = nullptr;
    QComboBox* qualityVariantCombo_ = nullptr;
    QLabel* balancedVariantLabel_ = nullptr;
    QComboBox* balancedVariantCombo_ = nullptr;
    QLabel* performanceVariantLabel_ = nullptr;
    QComboBox* performanceVariantCombo_ = nullptr;
    QCheckBox* loggingCheck_ = nullptr;
    QLabel* trtModeLabel_ = nullptr;
    QComboBox* trtModeCombo_ = nullptr;
    QWidget* trtDynamicContainer_ = nullptr;
    QLabel* trtMinResLabel_ = nullptr;
    QComboBox* trtMinResCombo_ = nullptr;
    QLabel* trtOptResLabel_ = nullptr;
    QComboBox* trtOptResCombo_ = nullptr;
    QLabel* trtMaxResLabel_ = nullptr;
    QComboBox* trtMaxResCombo_ = nullptr;
    QLabel* trtOptLevelLabel_ = nullptr;
    QComboBox* trtOptLevelCombo_ = nullptr;
    QLabel* trtRawArgsLabel_ = nullptr;
    QLineEdit* trtRawArgsEdit_ = nullptr;
    QPushButton* importConfButton_ = nullptr;
    QPushButton* exportConfButton_ = nullptr;
    QPushButton* resetDefaultsButton_ = nullptr;

    QLabel* profilesHeading_ = nullptr;
    QLabel* profilesSubtitle_ = nullptr;
    QLabel* slotSelectLabel_ = nullptr;
    QComboBox* slotSelectCombo_ = nullptr;
    QPushButton* createSlotButton_ = nullptr;
    QPushButton* cloneSlotButton_ = nullptr;
    QPushButton* deleteSlotButton_ = nullptr;
    QPushButton* importPconfButton_ = nullptr;
    QPushButton* exportPconfButton_ = nullptr;
    QWidget* slotEditorContainer_ = nullptr;
    QLabel* profileNameLabel_ = nullptr;
    QLineEdit* profileNameEdit_ = nullptr;
    QLabel* chainSelectLabel_ = nullptr;
    QComboBox* chainSelectCombo_ = nullptr;
    QPushButton* addChainButton_ = nullptr;
    QPushButton* removeChainButton_ = nullptr;
    QPushButton* moveChainUpButton_ = nullptr;
    QPushButton* moveChainDownButton_ = nullptr;
    QLabel* minResLabel_ = nullptr;
    QLineEdit* minResEdit_ = nullptr;
    QLabel* maxResLabel_ = nullptr;
    QLineEdit* maxResEdit_ = nullptr;
    QLabel* minFpsLabel_ = nullptr;
    QSpinBox* minFpsSpin_ = nullptr;
    QLabel* maxFpsLabel_ = nullptr;
    QSpinBox* maxFpsSpin_ = nullptr;
    QLabel* upscaleStepsHeading_ = nullptr;
    QPushButton* addModelStepButton_ = nullptr;
    QWidget* modelStepsContainer_ = nullptr;
    QVBoxLayout* modelStepsLayout_ = nullptr;
    QVector<ModelStepRow> modelStepRows_;
    QCheckBox* chainRifeEnableCheck_ = nullptr;
    QWidget* chainRifeContainer_ = nullptr;
    QLabel* chainRifeModelLabel_ = nullptr;
    QComboBox* chainRifeModelCombo_ = nullptr;
    QLabel* chainRifeFactorLabel_ = nullptr;
    QSpinBox* chainRifeNumSpin_ = nullptr;
    QSpinBox* chainRifeDenSpin_ = nullptr;
    QLabel* chainRifeSceneLabel_ = nullptr;
    QDoubleSpinBox* chainRifeSceneSpin_ = nullptr;
    QCheckBox* chainRifeEnsembleCheck_ = nullptr;
    QCheckBox* chainRifeBeforeUpscaleCheck_ = nullptr;
};

}  // namespace mikai::app
