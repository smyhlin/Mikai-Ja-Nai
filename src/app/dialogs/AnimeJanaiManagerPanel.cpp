#include "dialogs/AnimeJanaiManagerPanel.hpp"

#include "dialogs/settings/SettingsUi.hpp"
#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"
#include "workers/Runner.hpp"
#include "widgets/FlowLayout.hpp"
#include "player/shaders/Anime4K.hpp"

#include <QAbstractItemView>
#include <QCoreApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QUrl>
#include <QVariantMap>
#include <QVBoxLayout>

#include <algorithm>

namespace mikai::app {
namespace {

constexpr int kMaxCustomSlots = 10;

QString formatByteSize(qint64 bytes) {
    if (bytes <= 0) return QStringLiteral("0 B");
    const double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0) return QStringLiteral("%1 KB").arg(kb, 0, 'f', 1);
    const double mb = kb / 1024.0;
    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 2);
}

QLabel* makeFieldLabel(const QString& text, QWidget* parent, QWidget* buddy = nullptr) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("SettingsFormLabel"));
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    if (buddy != nullptr) label->setBuddy(buddy);
    return label;
}

}  // namespace

AnimeJanaiManagerPanel::AnimeJanaiManagerPanel(QWidget* parent, Runner* runner)
    : QWidget(parent), runner_(runner) {
    setObjectName(QStringLiteral("animeJanaiManagerPanel"));
    buildUi();
    reloadFromDisk();
}

AnimeJanaiManagerPanel::~AnimeJanaiManagerPanel() {
    alive_->store(false, std::memory_order_release);
}

mplayer::native::AnimeJanaiConfig AnimeJanaiManagerPanel::currentConfig() const {
    return config_;
}

void AnimeJanaiManagerPanel::reloadFromDisk() {
    QString error;
    const auto loaded = mplayer::native::AnimeJanaiConfigStore::load(&error);
    if (!error.isEmpty()) {
        showSaveError(error);
        return;
    }
    updatingUi_ = true;
    config_ = loaded;
    loadConfigIntoWidgets(config_);
    refreshCustomModels();
    dirty_ = false;
    updatingUi_ = false;
}

bool AnimeJanaiManagerPanel::saveConfig(QString* error) {
    commitCurrentChainFromEditor();
    QString saveError;
    if (!mplayer::native::AnimeJanaiConfigStore::save(config_, &saveError)) {
        if (error != nullptr) *error = saveError;
        updateStatusBanner(saveError, QStringLiteral("danger"));
        return false;
    }
    dirty_ = false;
    return true;
}

void AnimeJanaiManagerPanel::markDirty() {
    if (updatingUi_) return;
    dirty_ = true;
    emit configModified();
}

void AnimeJanaiManagerPanel::updateStatusBanner(const QString& message, const QString& tone) {
    if (bannerLabel_ == nullptr) return;
    if (message.trimmed().isEmpty()) {
        bannerLabel_->clear();
        bannerLabel_->hide();
        return;
    }
    bannerLabel_->setText(message);
    styleStatusBanner(bannerLabel_, tone);
    bannerLabel_->show();
    if (tone == QLatin1String("danger")) {
        for (QWidget* ancestor = parentWidget(); ancestor != nullptr;
             ancestor = ancestor->parentWidget()) {
            if (auto* scroll = qobject_cast<QScrollArea*>(ancestor)) {
                scroll->ensureWidgetVisible(bannerLabel_);
                break;
            }
        }
    }
}
void AnimeJanaiManagerPanel::showSaveError(const QString& message) {
    updateStatusBanner(message, QStringLiteral("danger"));
}

void AnimeJanaiManagerPanel::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(theme::kSpacingMedium);

    bannerLabel_ = new QLabel(this);
    bannerLabel_->setObjectName(QStringLiteral("status"));
    bannerLabel_->setWordWrap(true);
    yieldCaptionWidth(bannerLabel_);
    bannerLabel_->hide();
    root->addWidget(bannerLabel_);

    QVBoxLayout* modelsLayout = nullptr;
    QFrame* modelsCard = makeSettingsSubCard(this, &modelsLayout);
    modelsLayout->setSpacing(theme::kSpacingSmall);

    auto* modelsHeaderRow = new QHBoxLayout();
    modelsHeaderRow->setContentsMargins(0, 0, 0, 0);
    modelsHeaderRow->setSpacing(theme::kSpacingSmall);
    modelsHeading_ = new QLabel(tr("Custom RealESRGAN / ONNX spatial models"), modelsCard);
    styleSectionHeading(modelsHeading_);
    modelsCountBadge_ = new QLabel(QStringLiteral("0"), modelsCard);
    stylePillBadge(modelsCountBadge_, QStringLiteral("purple"));
    modelsHeaderRow->addWidget(modelsHeading_, 1, Qt::AlignVCenter);
    modelsHeaderRow->addWidget(modelsCountBadge_, 0, Qt::AlignVCenter);
    modelsLayout->addLayout(modelsHeaderRow);

    modelsSubtitle_ = new QLabel(
        tr("Import compatible RealESRGAN, Compact (SRVGGNet), SPAN, DAT, or SwinIR .onnx graph "
           "(4D 1x3xHxW RGB). Imported models appear immediately in the player's "
           "ANIMEJANAI -> Spatial models menu, Compare window, Benchmark tab, and custom profile chains."),
        modelsCard);
    styleMutedCaption(modelsSubtitle_);
    modelsLayout->addWidget(modelsSubtitle_);

    customModelsTable_ = new QTableWidget(0, 5, modelsCard);
    customModelsTable_->setObjectName(QStringLiteral("animeJanaiCustomModelsTable"));
    customModelsTable_->setAlternatingRowColors(true);
    customModelsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    customModelsTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    customModelsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    customModelsTable_->verticalHeader()->setVisible(false);
    customModelsTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    customModelsTable_->horizontalHeader()->setMinimumSectionSize(theme::kSettingsControlSize);
    customModelsTable_->setMinimumHeight(140);
    applyTabularNumerals(customModelsTable_);
    modelsLayout->addWidget(customModelsTable_);

    auto* modelsToolbar = new FlowLayout(theme::kSpacingSmall, theme::kSpacingSmall);
    importModelButton_ = new QPushButton(tr("Import .onnx Model…"), modelsCard);
    importModelButton_->setObjectName(QStringLiteral("ajiImportModelButton"));
    importModelButton_->setProperty("primaryAction", true);
    polishInteractiveControl(importModelButton_);
    removeModelButton_ = new QPushButton(tr("Remove Selected"), modelsCard);
    removeModelButton_->setObjectName(QStringLiteral("ajiRemoveModelButton"));
    polishInteractiveControl(removeModelButton_);
    openModelsDirButton_ = new QPushButton(tr("Open Models Folder"), modelsCard);
    openModelsDirButton_->setObjectName(QStringLiteral("ajiOpenModelsFolderButton"));
    polishInteractiveControl(openModelsDirButton_);
    modelsToolbar->addWidget(importModelButton_);
    modelsToolbar->addWidget(removeModelButton_);
    modelsToolbar->addWidget(openModelsDirButton_);
    modelsLayout->addLayout(modelsToolbar);

    auto* externalDirRow = new QVBoxLayout();
    externalDirRow->setContentsMargins(0, 0, 0, 0);
    externalDirRow->setSpacing(theme::kSpacingSmall);
    externalDirEdit_ = new QLineEdit(modelsCard);
    externalDirEdit_->setObjectName(QStringLiteral("ajiExternalModelsDirEdit"));
    externalDirEdit_->setPlaceholderText(
        tr("Optional external directory containing additional .onnx models…"));
    polishInteractiveControl(externalDirEdit_);
    externalDirLabel_ = makeFieldLabel(tr("External models folder"), modelsCard, externalDirEdit_);
    browseExternalDirButton_ = new QPushButton(tr("Browse…"), modelsCard);
    polishInteractiveControl(browseExternalDirButton_);
    clearExternalDirButton_ = new QPushButton(tr("Clear"), modelsCard);
    polishInteractiveControl(clearExternalDirButton_);
    externalDirRow->addWidget(externalDirLabel_);
    externalDirRow->addWidget(externalDirEdit_);
    auto* externalDirActions = new FlowLayout(theme::kSpacingSmall, theme::kSpacingSmall);
    externalDirActions->addWidget(browseExternalDirButton_);
    externalDirActions->addWidget(clearExternalDirButton_);
    externalDirRow->addLayout(externalDirActions);
    modelsLayout->addLayout(externalDirRow);

    root->addWidget(modelsCard);

    QVBoxLayout* globalLayout = nullptr;
    QFrame* globalCard = makeSettingsSubCard(this, &globalLayout);
    globalLayout->setSpacing(theme::kSpacingSmall);

    globalHeading_ =
        new QLabel(tr("Built-in preset variants & TensorRT engine settings"), globalCard);
    styleSectionHeading(globalHeading_);
    globalLayout->addWidget(globalHeading_);

    globalSubtitle_ = new QLabel(
        tr("Switch built-in Quality, Balanced, and Performance profiles between Standard (V3.1) "
           "and Sharp (V3.1Sharp1) models, and customize how TensorRT compiles ONNX graphs."),
        globalCard);
    styleMutedCaption(globalSubtitle_);
    globalLayout->addWidget(globalSubtitle_);

    auto makeVariantCombo = [globalCard]() {
        auto* combo = new QComboBox(globalCard);
        polishInteractiveControl(combo);
        combo->addItem(QObject::tr("Standard (V3.1)"), false);
        combo->addItem(QObject::tr("Sharp (V3.1Sharp1)"), true);
        return combo;
    };

    auto* variantsForm = new QFormLayout();
    variantsForm->setContentsMargins(0, 0, 0, 0);
    variantsForm->setHorizontalSpacing(theme::kSpacingMedium);
    variantsForm->setVerticalSpacing(theme::kSpacingSmall);
    variantsForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    variantsForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    qualityVariantCombo_ = makeVariantCombo();
    qualityVariantLabel_ =
        makeFieldLabel(tr("Quality preset (Slot 1001)"), globalCard, qualityVariantCombo_);
    balancedVariantCombo_ = makeVariantCombo();
    balancedVariantLabel_ =
        makeFieldLabel(tr("Balanced preset (Slot 1002)"), globalCard, balancedVariantCombo_);
    performanceVariantCombo_ = makeVariantCombo();
    performanceVariantLabel_ =
        makeFieldLabel(tr("Performance preset (Slot 1003)"), globalCard, performanceVariantCombo_);

    variantsForm->addRow(qualityVariantLabel_, qualityVariantCombo_);
    variantsForm->addRow(balancedVariantLabel_, balancedVariantCombo_);
    variantsForm->addRow(performanceVariantLabel_, performanceVariantCombo_);
    globalLayout->addLayout(variantsForm);

    loggingCheck_ = new QCheckBox(tr("Backend logging"), globalCard);
    polishInteractiveControl(loggingCheck_, 0);
    globalLayout->addWidget(loggingCheck_);

    globalLayout->addWidget(makeSettingsDivider(globalCard));

    auto* trtForm = new QFormLayout();
    trtForm->setContentsMargins(0, 0, 0, 0);
    trtForm->setHorizontalSpacing(theme::kSpacingMedium);
    trtForm->setVerticalSpacing(theme::kSpacingSmall);
    trtForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    trtForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    trtModeCombo_ = new QComboBox(globalCard);
    polishInteractiveControl(trtModeCombo_);
    trtModeCombo_->addItem(tr("Static (Match Video Resolution)"), QStringLiteral("static"));
    trtModeCombo_->addItem(tr("Static ONNX (Fixed Input Shape)"), QStringLiteral("static_onnx"));
    trtModeCombo_->addItem(tr("Dynamic (Min / Opt / Max Shapes)"), QStringLiteral("dynamic"));
    trtModeLabel_ = makeFieldLabel(tr("TensorRT engine type"), globalCard, trtModeCombo_);
    trtForm->addRow(trtModeLabel_, trtModeCombo_);

    trtOptLevelCombo_ = new QComboBox(globalCard);
    polishInteractiveControl(trtOptLevelCombo_);
    trtOptLevelCombo_->addItem(tr("0"), QStringLiteral("0"));
    trtOptLevelCombo_->addItem(tr("1"), QStringLiteral("1"));
    trtOptLevelCombo_->addItem(tr("2"), QStringLiteral("2"));
    trtOptLevelCombo_->addItem(tr("3 (TensorRT Default)"), QStringLiteral("3"));
    trtOptLevelCombo_->addItem(tr("4"), QStringLiteral("4"));
    trtOptLevelCombo_->addItem(tr("5 (Maximum Optimization)"), QStringLiteral("5"));
    trtOptLevelLabel_ =
        makeFieldLabel(tr("Builder optimization level"), globalCard, trtOptLevelCombo_);
    trtForm->addRow(trtOptLevelLabel_, trtOptLevelCombo_);
    globalLayout->addLayout(trtForm);

    trtDynamicContainer_ = new QWidget(globalCard);
    auto* dynForm = new QFormLayout(trtDynamicContainer_);
    dynForm->setContentsMargins(0, 0, 0, 0);
    dynForm->setHorizontalSpacing(theme::kSpacingMedium);
    dynForm->setVerticalSpacing(theme::kSpacingXs);
    dynForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    dynForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    auto populateResCombo = [](QComboBox* combo, const QStringList& resolutions) {
        combo->setEditable(true);
        polishInteractiveControl(combo);
        for (const QString& res : resolutions) combo->addItem(res, res);
    };
    trtMinResCombo_ = new QComboBox(trtDynamicContainer_);
    populateResCombo(trtMinResCombo_,
                     {QStringLiteral("64x64"), QStringLiteral("480x480"),
                      QStringLiteral("960x540"), QStringLiteral("1280x720"),
                      QStringLiteral("1920x1080")});
    trtMinResLabel_ =
        makeFieldLabel(tr("Dynamic minimum resolution"), trtDynamicContainer_, trtMinResCombo_);

    trtOptResCombo_ = new QComboBox(trtDynamicContainer_);
    populateResCombo(trtOptResCombo_,
                     {QStringLiteral("960x540"), QStringLiteral("1280x720"),
                      QStringLiteral("1920x1080"), QStringLiteral("2560x1440"),
                      QStringLiteral("3840x2160")});
    trtOptResLabel_ =
        makeFieldLabel(tr("Dynamic optimal resolution"), trtDynamicContainer_, trtOptResCombo_);

    trtMaxResCombo_ = new QComboBox(trtDynamicContainer_);
    populateResCombo(trtMaxResCombo_,
                     {QStringLiteral("960x540"), QStringLiteral("1280x720"),
                      QStringLiteral("1920x1080"), QStringLiteral("2560x1440"),
                      QStringLiteral("3840x2160")});
    trtMaxResLabel_ =
        makeFieldLabel(tr("Dynamic maximum resolution"), trtDynamicContainer_, trtMaxResCombo_);

    dynForm->addRow(trtMinResLabel_, trtMinResCombo_);
    dynForm->addRow(trtOptResLabel_, trtOptResCombo_);
    dynForm->addRow(trtMaxResLabel_, trtMaxResCombo_);
    globalLayout->addWidget(trtDynamicContainer_);

    auto* rawArgsForm = new QFormLayout();
    rawArgsForm->setContentsMargins(0, 0, 0, 0);
    rawArgsForm->setHorizontalSpacing(theme::kSpacingMedium);
    rawArgsForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    rawArgsForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    trtRawArgsEdit_ = new QLineEdit(globalCard);
    trtRawArgsEdit_->setObjectName(QStringLiteral("ajiTrtEngineSettingsEdit"));
    polishInteractiveControl(trtRawArgsEdit_);
    trtRawArgsLabel_ = makeFieldLabel(tr("trtexec engine arguments"), globalCard, trtRawArgsEdit_);
    rawArgsForm->addRow(trtRawArgsLabel_, trtRawArgsEdit_);
    globalLayout->addLayout(rawArgsForm);

    auto* confToolbar = new FlowLayout(theme::kSpacingSmall, theme::kSpacingSmall);
    importConfButton_ = new QPushButton(tr("Import animejanai.conf…"), globalCard);
    polishInteractiveControl(importConfButton_);
    exportConfButton_ = new QPushButton(tr("Export animejanai.conf…"), globalCard);
    polishInteractiveControl(exportConfButton_);
    resetDefaultsButton_ = new QPushButton(tr("Reset Defaults"), globalCard);
    polishInteractiveControl(resetDefaultsButton_);
    confToolbar->addWidget(importConfButton_);
    confToolbar->addWidget(exportConfButton_);
    confToolbar->addWidget(resetDefaultsButton_);
    globalLayout->addLayout(confToolbar);

    root->addWidget(globalCard);

    QVBoxLayout* profilesLayout = nullptr;
    QFrame* profilesCard = makeSettingsSubCard(this, &profilesLayout);
    profilesLayout->setSpacing(theme::kSpacingSmall);

    profilesHeading_ =
        new QLabel(tr("Custom multi-chain profile editor (Slots 1–10)"), profilesCard);
    styleSectionHeading(profilesHeading_);
    profilesLayout->addWidget(profilesHeading_);

    profilesSubtitle_ = new QLabel(
        tr("Build custom AnimeJaNai profile slots with resolution/FPS activation conditions, "
           "ordered ONNX and Anime4K processing steps, input resizing, and per-chain RIFE interpolation. "
           "Active custom slots appear in the player's ANIMEJANAI menu."),
        profilesCard);
    styleMutedCaption(profilesSubtitle_);
    profilesLayout->addWidget(profilesSubtitle_);

    auto* slotSelectForm = new QFormLayout();
    slotSelectForm->setContentsMargins(0, 0, 0, 0);
    slotSelectForm->setHorizontalSpacing(theme::kSpacingSmall);
    slotSelectForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    slotSelectForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    slotSelectCombo_ = new QComboBox(profilesCard);
    slotSelectCombo_->setObjectName(QStringLiteral("ajiSlotSelectCombo"));
    polishInteractiveControl(slotSelectCombo_);
    slotSelectLabel_ = makeFieldLabel(tr("Profile slot"), profilesCard, slotSelectCombo_);
    slotSelectForm->addRow(slotSelectLabel_, slotSelectCombo_);
    profilesLayout->addLayout(slotSelectForm);

    auto* slotToolbar = new FlowLayout(theme::kSpacingSmall, theme::kSpacingSmall);
    createSlotButton_ = new QPushButton(tr("Activate / Reset Slot"), profilesCard);
    createSlotButton_->setObjectName(QStringLiteral("ajiCreateSlotButton"));
    polishInteractiveControl(createSlotButton_);
    cloneSlotButton_ = new QPushButton(tr("Clone From…"), profilesCard);
    polishInteractiveControl(cloneSlotButton_);
    deleteSlotButton_ = new QPushButton(tr("Clear Slot"), profilesCard);
    polishInteractiveControl(deleteSlotButton_);
    importPconfButton_ = new QPushButton(tr("Import .pconf…"), profilesCard);
    polishInteractiveControl(importPconfButton_);
    exportPconfButton_ = new QPushButton(tr("Export .pconf…"), profilesCard);
    polishInteractiveControl(exportPconfButton_);
    slotToolbar->addWidget(createSlotButton_);
    slotToolbar->addWidget(cloneSlotButton_);
    slotToolbar->addWidget(deleteSlotButton_);
    slotToolbar->addWidget(importPconfButton_);
    slotToolbar->addWidget(exportPconfButton_);
    profilesLayout->addLayout(slotToolbar);

    profilesLayout->addWidget(makeSettingsDivider(profilesCard));

    slotEditorContainer_ = new QWidget(profilesCard);
    auto* editorLayout = new QVBoxLayout(slotEditorContainer_);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(theme::kSpacingSmall);

    auto* profileNameForm = new QFormLayout();
    profileNameForm->setContentsMargins(0, 0, 0, 0);
    profileNameForm->setHorizontalSpacing(theme::kSpacingMedium);
    profileNameForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    profileNameForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    profileNameEdit_ = new QLineEdit(slotEditorContainer_);
    profileNameEdit_->setObjectName(QStringLiteral("ajiProfileNameEdit"));
    polishInteractiveControl(profileNameEdit_);
    profileNameLabel_ =
        makeFieldLabel(tr("Profile name"), slotEditorContainer_, profileNameEdit_);
    profileNameForm->addRow(profileNameLabel_, profileNameEdit_);
    editorLayout->addLayout(profileNameForm);

    auto* chainToolbar = new QVBoxLayout();
    chainToolbar->setContentsMargins(0, 0, 0, 0);
    chainToolbar->setSpacing(theme::kSpacingSmall);
    chainSelectCombo_ = new QComboBox(slotEditorContainer_);
    polishInteractiveControl(chainSelectCombo_);
    chainSelectLabel_ =
        makeFieldLabel(tr("Active chain"), slotEditorContainer_, chainSelectCombo_);
    addChainButton_ = new QPushButton(tr("Add Chain"), slotEditorContainer_);
    polishInteractiveControl(addChainButton_);
    removeChainButton_ = new QPushButton(tr("Remove Chain"), slotEditorContainer_);
    polishInteractiveControl(removeChainButton_);
    moveChainUpButton_ = new QPushButton(tr("Move Up"), slotEditorContainer_);
    polishInteractiveControl(moveChainUpButton_);
    moveChainDownButton_ = new QPushButton(tr("Move Down"), slotEditorContainer_);
    polishInteractiveControl(moveChainDownButton_);
    chainToolbar->addWidget(chainSelectLabel_);
    chainToolbar->addWidget(chainSelectCombo_);
    auto* chainActions = new FlowLayout(theme::kSpacingSmall, theme::kSpacingSmall);
    chainActions->addWidget(addChainButton_);
    chainActions->addWidget(removeChainButton_);
    chainActions->addWidget(moveChainUpButton_);
    chainActions->addWidget(moveChainDownButton_);
    chainToolbar->addLayout(chainActions);
    editorLayout->addLayout(chainToolbar);

    auto* condForm = new QFormLayout();
    condForm->setContentsMargins(0, 0, 0, 0);
    condForm->setHorizontalSpacing(theme::kSpacingMedium);
    condForm->setVerticalSpacing(theme::kSpacingXs);
    condForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    condForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    minResEdit_ = new QLineEdit(slotEditorContainer_);
    minResEdit_->setPlaceholderText(QStringLiteral("0x0"));
    polishInteractiveControl(minResEdit_);
    minResLabel_ = makeFieldLabel(tr("Min resolution (WxH)"), slotEditorContainer_, minResEdit_);

    maxResEdit_ = new QLineEdit(slotEditorContainer_);
    maxResEdit_->setPlaceholderText(QStringLiteral("0x0 (0x0 = unbounded)"));
    polishInteractiveControl(maxResEdit_);
    maxResLabel_ = makeFieldLabel(tr("Max resolution (WxH)"), slotEditorContainer_, maxResEdit_);

    minFpsSpin_ = new QSpinBox(slotEditorContainer_);
    minFpsSpin_->setRange(0, 240);
    minFpsSpin_->setSpecialValueText(tr("0 (Any)"));
    polishInteractiveControl(minFpsSpin_);
    minFpsLabel_ = makeFieldLabel(tr("Min FPS"), slotEditorContainer_, minFpsSpin_);

    maxFpsSpin_ = new QSpinBox(slotEditorContainer_);
    maxFpsSpin_->setRange(0, 240);
    maxFpsSpin_->setSpecialValueText(tr("0 (Unbounded)"));
    polishInteractiveControl(maxFpsSpin_);
    maxFpsLabel_ = makeFieldLabel(tr("Max FPS"), slotEditorContainer_, maxFpsSpin_);

    condForm->addRow(minResLabel_, minResEdit_);
    condForm->addRow(maxResLabel_, maxResEdit_);
    condForm->addRow(minFpsLabel_, minFpsSpin_);
    condForm->addRow(maxFpsLabel_, maxFpsSpin_);
    editorLayout->addLayout(condForm);

    auto* stepsHeaderRow = new QHBoxLayout();
    stepsHeaderRow->setContentsMargins(0, 0, 0, 0);
    stepsHeaderRow->setSpacing(theme::kSpacingSmall);
    upscaleStepsHeading_ = new QLabel(tr("Processing steps in this chain"), slotEditorContainer_);
    styleSectionHeading(upscaleStepsHeading_);
    addModelStepButton_ = new QPushButton(tr("Add Processing Step"), slotEditorContainer_);
    addModelStepButton_->setObjectName(QStringLiteral("ajiAddStepButton"));
    polishInteractiveControl(addModelStepButton_);
    stepsHeaderRow->addWidget(upscaleStepsHeading_, 1, Qt::AlignVCenter);
    stepsHeaderRow->addWidget(addModelStepButton_, 0, Qt::AlignVCenter);
    editorLayout->addLayout(stepsHeaderRow);

    modelStepsContainer_ = new QWidget(slotEditorContainer_);
    modelStepsLayout_ = new QVBoxLayout(modelStepsContainer_);
    modelStepsLayout_->setContentsMargins(0, 0, 0, 0);
    modelStepsLayout_->setSpacing(theme::kSpacingXs);
    editorLayout->addWidget(modelStepsContainer_);

    chainRifeEnableCheck_ =
        new QCheckBox(tr("RIFE interpolation"), slotEditorContainer_);
    polishInteractiveControl(chainRifeEnableCheck_, 0);
    editorLayout->addWidget(chainRifeEnableCheck_);

    chainRifeContainer_ = new QWidget(slotEditorContainer_);
    auto* rifeForm = new QFormLayout(chainRifeContainer_);
    rifeForm->setContentsMargins(0, 0, 0, 0);
    rifeForm->setHorizontalSpacing(theme::kSpacingMedium);
    rifeForm->setVerticalSpacing(theme::kSpacingXs);
    rifeForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    rifeForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    chainRifeModelCombo_ = new QComboBox(chainRifeContainer_);
    polishInteractiveControl(chainRifeModelCombo_);
    for (const QString& label : mplayer::native::AnimeJanaiConfigStore::availableRifeModelLabels()) {
        chainRifeModelCombo_->addItem(label, label);
    }
    chainRifeModelLabel_ =
        makeFieldLabel(tr("Chain RIFE model"), chainRifeContainer_, chainRifeModelCombo_);

    auto* factorBox = new QWidget(chainRifeContainer_);
    auto* factorLayout = new QHBoxLayout(factorBox);
    factorLayout->setContentsMargins(0, 0, 0, 0);
    factorLayout->setSpacing(theme::kSpacingXs);
    chainRifeNumSpin_ = new QSpinBox(factorBox);
    chainRifeNumSpin_->setRange(1, 16);
    polishInteractiveControl(chainRifeNumSpin_);
    auto* slashLabel = new QLabel(QStringLiteral("/"), factorBox);
    chainRifeDenSpin_ = new QSpinBox(factorBox);
    chainRifeDenSpin_->setRange(1, 16);
    polishInteractiveControl(chainRifeDenSpin_);
    factorLayout->addWidget(chainRifeNumSpin_);
    factorLayout->addWidget(slashLabel);
    factorLayout->addWidget(chainRifeDenSpin_);
    factorLayout->addStretch(1);
    chainRifeFactorLabel_ =
        makeFieldLabel(tr("Interpolation FPS factor"), chainRifeContainer_, chainRifeNumSpin_);

    chainRifeSceneSpin_ = new QDoubleSpinBox(chainRifeContainer_);
    chainRifeSceneSpin_->setDecimals(3);
    chainRifeSceneSpin_->setRange(0.0, 1.0);
    chainRifeSceneSpin_->setSingleStep(0.010);
    polishInteractiveControl(chainRifeSceneSpin_);
    chainRifeSceneLabel_ =
        makeFieldLabel(tr("Scene change threshold"), chainRifeContainer_, chainRifeSceneSpin_);

    chainRifeEnsembleCheck_ = new QCheckBox(tr("RIFE ensemble"), chainRifeContainer_);
    polishInteractiveControl(chainRifeEnsembleCheck_, 0);
    chainRifeBeforeUpscaleCheck_ =
        new QCheckBox(tr("RIFE before upscaling"), chainRifeContainer_);
    polishInteractiveControl(chainRifeBeforeUpscaleCheck_, 0);

    rifeForm->addRow(chainRifeModelLabel_, chainRifeModelCombo_);
    rifeForm->addRow(chainRifeFactorLabel_, factorBox);
    rifeForm->addRow(chainRifeSceneLabel_, chainRifeSceneSpin_);
    rifeForm->addRow(chainRifeEnsembleCheck_);
    rifeForm->addRow(chainRifeBeforeUpscaleCheck_);
    editorLayout->addWidget(chainRifeContainer_);

    profilesLayout->addWidget(slotEditorContainer_);
    root->addWidget(profilesCard);

    connect(importModelButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onImportOnnxModels);
    connect(removeModelButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onRemoveSelectedOnnxModel);
    connect(customModelsTable_, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) {
                const auto* item = row >= 0 ? customModelsTable_->item(row, 0) : nullptr;
                removeModelButton_->setEnabled(
                    item != nullptr && !item->data(Qt::UserRole + 1).toBool());
            });
    connect(openModelsDirButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onOpenCustomModelsFolder);
    connect(browseExternalDirButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onBrowseExternalModelsFolder);
    connect(clearExternalDirButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onClearExternalModelsFolder);
    connect(externalDirEdit_, &QLineEdit::editingFinished, this, [this]() {
        if (updatingUi_) return;
        const QString trimmed = externalDirEdit_->text().trimmed();
        if (config_.externalModelsDir != trimmed) {
            config_.externalModelsDir = trimmed;
            markDirty();
            refreshCustomModels();
        }
    });

    connect(qualityVariantCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (updatingUi_) return;
        config_.qualitySharp = qualityVariantCombo_->currentData().toBool();
        markDirty();
    });
    connect(balancedVariantCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (updatingUi_) return;
        config_.balancedSharp = balancedVariantCombo_->currentData().toBool();
        markDirty();
    });
    connect(performanceVariantCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (updatingUi_) return;
        config_.performanceSharp = performanceVariantCombo_->currentData().toBool();
        markDirty();
    });
    connect(loggingCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        if (updatingUi_) return;
        config_.enableLogging = checked;
        markDirty();
    });

    connect(trtModeCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (updatingUi_) return;
        const QString mode = trtModeCombo_->currentData().toString();
        if (mode == QStringLiteral("static_onnx")) {
            config_.trtEngineSettings =
                mplayer::native::AnimeJanaiConfigStore::setTrtStaticOnnx(config_.trtEngineSettings);
        } else if (mode == QStringLiteral("dynamic")) {
            config_.trtEngineSettings =
                mplayer::native::AnimeJanaiConfigStore::setTrtDynamic(config_.trtEngineSettings);
        } else {
            config_.trtEngineSettings =
                mplayer::native::AnimeJanaiConfigStore::setTrtStatic(config_.trtEngineSettings);
        }
        syncTrtWidgetsFromText(config_.trtEngineSettings);
        markDirty();
    });
    connect(trtOptLevelCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (updatingUi_) return;
        config_.trtEngineSettings =
            mplayer::native::AnimeJanaiConfigStore::setTrtBuilderOptimizationLevel(
                config_.trtEngineSettings, trtOptLevelCombo_->currentData().toString());
        syncTrtWidgetsFromText(config_.trtEngineSettings);
        markDirty();
    });
    connect(trtMinResCombo_, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        if (updatingUi_) return;
        config_.trtEngineSettings =
            mplayer::native::AnimeJanaiConfigStore::setTrtDynamicMinResolution(
                config_.trtEngineSettings, text.trimmed());
        QSignalBlocker blocker(trtRawArgsEdit_);
        trtRawArgsEdit_->setText(config_.trtEngineSettings);
        markDirty();
    });
    connect(trtOptResCombo_, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        if (updatingUi_) return;
        config_.trtEngineSettings =
            mplayer::native::AnimeJanaiConfigStore::setTrtDynamicOptResolution(
                config_.trtEngineSettings, text.trimmed());
        QSignalBlocker blocker(trtRawArgsEdit_);
        trtRawArgsEdit_->setText(config_.trtEngineSettings);
        markDirty();
    });
    connect(trtMaxResCombo_, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        if (updatingUi_) return;
        config_.trtEngineSettings =
            mplayer::native::AnimeJanaiConfigStore::setTrtDynamicMaxResolution(
                config_.trtEngineSettings, text.trimmed());
        QSignalBlocker blocker(trtRawArgsEdit_);
        trtRawArgsEdit_->setText(config_.trtEngineSettings);
        markDirty();
    });
    connect(trtRawArgsEdit_, &QLineEdit::textEdited, this, [this](const QString& text) {
        if (updatingUi_) return;
        config_.trtEngineSettings = text;
        syncTrtWidgetsFromText(config_.trtEngineSettings);
        markDirty();
    });

    connect(importConfButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onImportFullConf);
    connect(exportConfButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onExportFullConf);
    connect(resetDefaultsButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onResetDefaults);

    connect(slotSelectCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (updatingUi_) return;
        commitCurrentChainFromEditor();
        loadSelectedSlotIntoEditor();
    });
    connect(createSlotButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onCreateOrResetSlot);
    connect(cloneSlotButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onCloneSlotMenu);
    connect(deleteSlotButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onDeleteSlot);
    connect(importPconfButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onImportProfilePconf);
    connect(exportPconfButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onExportProfilePconf);

    connect(profileNameEdit_, &QLineEdit::textEdited, this, [this](const QString& text) {
        if (updatingUi_) return;
        if (auto* slot = findMutableSlot(currentSlotNumber())) {
            slot->profileName = text;
            const int idx = slotSelectCombo_->currentIndex();
            if (idx >= 0) {
                slotSelectCombo_->setItemText(
                    idx, tr("Slot %1: %2").arg(slot->number).arg(
                             slot->profileName.trimmed().isEmpty() ? tr("Custom Profile")
                                                                   : slot->profileName.trimmed()));
            }
            markDirty();
        }
    });
    connect(chainSelectCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (updatingUi_) return;
        commitCurrentChainFromEditor();
        loadSelectedChainIntoEditor();
    });
    connect(addChainButton_, &QPushButton::clicked, this, &AnimeJanaiManagerPanel::onAddChain);
    connect(removeChainButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onRemoveChain);
    connect(moveChainUpButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onMoveChainUp);
    connect(moveChainDownButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onMoveChainDown);
    connect(addModelStepButton_, &QPushButton::clicked, this,
            &AnimeJanaiManagerPanel::onAddModelStep);

    const auto bindChainFieldEdit = [this]() {
        if (updatingUi_) return;
        commitCurrentChainFromEditor();
        markDirty();
    };
    connect(minResEdit_, &QLineEdit::textEdited, this, bindChainFieldEdit);
    connect(maxResEdit_, &QLineEdit::textEdited, this, bindChainFieldEdit);
    connect(minFpsSpin_, &QSpinBox::valueChanged, this, bindChainFieldEdit);
    connect(maxFpsSpin_, &QSpinBox::valueChanged, this, bindChainFieldEdit);
    connect(chainRifeEnableCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        if (chainRifeContainer_ != nullptr) chainRifeContainer_->setVisible(checked);
        if (updatingUi_) return;
        commitCurrentChainFromEditor();
        markDirty();
    });
    connect(chainRifeModelCombo_, &QComboBox::currentIndexChanged, this, bindChainFieldEdit);
    connect(chainRifeNumSpin_, &QSpinBox::valueChanged, this, bindChainFieldEdit);
    connect(chainRifeDenSpin_, &QSpinBox::valueChanged, this, bindChainFieldEdit);
    connect(chainRifeSceneSpin_, &QDoubleSpinBox::valueChanged, this, bindChainFieldEdit);
    connect(chainRifeEnsembleCheck_, &QCheckBox::toggled, this, bindChainFieldEdit);
    connect(chainRifeBeforeUpscaleCheck_, &QCheckBox::toggled, this, bindChainFieldEdit);

    for (QComboBox* combo : findChildren<QComboBox*>()) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(theme::kSettingsCompactChoiceLengthChars);
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    retranslateUi();
}

void AnimeJanaiManagerPanel::loadConfigIntoWidgets(
    const mplayer::native::AnimeJanaiConfig& config) {
    const bool prevUpdating = updatingUi_;
    updatingUi_ = true;

    externalDirEdit_->setText(config.externalModelsDir);
    qualityVariantCombo_->setCurrentIndex(qualityVariantCombo_->findData(config.qualitySharp));
    balancedVariantCombo_->setCurrentIndex(balancedVariantCombo_->findData(config.balancedSharp));
    performanceVariantCombo_->setCurrentIndex(
        performanceVariantCombo_->findData(config.performanceSharp));
    loggingCheck_->setChecked(config.enableLogging);

    syncTrtWidgetsFromText(config.trtEngineSettings);
    rebuildSlotCombo();
    loadSelectedSlotIntoEditor();

    updatingUi_ = prevUpdating;
}

void AnimeJanaiManagerPanel::syncTrtWidgetsFromText(const QString& settingsText) {
    const bool prevUpdating = updatingUi_;
    updatingUi_ = true;

    if (trtRawArgsEdit_->text() != settingsText) {
        trtRawArgsEdit_->setText(settingsText);
    }

    const bool isDynamic =
        mplayer::native::AnimeJanaiConfigStore::trtDynamicSelected(settingsText);
    const bool isStaticOnnx =
        mplayer::native::AnimeJanaiConfigStore::trtStaticOnnxSelected(settingsText);

    const QString modeKey = isDynamic      ? QStringLiteral("dynamic")
                            : isStaticOnnx ? QStringLiteral("static_onnx")
                                           : QStringLiteral("static");
    const int modeIndex = trtModeCombo_->findData(modeKey);
    if (modeIndex >= 0) trtModeCombo_->setCurrentIndex(modeIndex);

    trtDynamicContainer_->setVisible(isDynamic);
    if (isDynamic) {
        trtMinResCombo_->setCurrentText(
            mplayer::native::AnimeJanaiConfigStore::trtDynamicMinResolution(settingsText));
        trtOptResCombo_->setCurrentText(
            mplayer::native::AnimeJanaiConfigStore::trtDynamicOptResolution(settingsText));
        trtMaxResCombo_->setCurrentText(
            mplayer::native::AnimeJanaiConfigStore::trtDynamicMaxResolution(settingsText));
    }

    const QString optLevel =
        mplayer::native::AnimeJanaiConfigStore::trtBuilderOptimizationLevel(settingsText);
    const int optIndex = trtOptLevelCombo_->findData(optLevel);
    if (optIndex >= 0) trtOptLevelCombo_->setCurrentIndex(optIndex);

    updatingUi_ = prevUpdating;
}

void AnimeJanaiManagerPanel::refreshCustomModels() {
    QVector<mplayer::native::CustomAnimeJanaiModel> models =
        mplayer::native::listCustomAnimeJanaiModels();
    QString inventoryError;
    const QVector<mplayer::native::CustomAnimeJanaiModel> candidates =
        mplayer::native::listExternalCustomAnimeJanaiModelCandidates(
            config_.externalModelsDir, &inventoryError);
    const int registeredCount = models.size();
    for (const auto& candidate : candidates) {
        if (std::none_of(models.cbegin(), models.cend(), [&candidate](const auto& item) {
                return item.filePath == candidate.filePath;
            }))
            models.append(candidate);
    }
    customModelsTable_->setRowCount(static_cast<int>(models.size()));
    for (int row = 0; row < models.size(); ++row) {
        const auto& model = models.at(row);
        const bool externalCandidate = row >= registeredCount;
        auto* nameItem = new QTableWidgetItem(model.displayName);
        nameItem->setData(Qt::UserRole, model.id);
        nameItem->setData(Qt::UserRole + 1, externalCandidate);
        nameItem->setToolTip(model.filePath);
        customModelsTable_->setItem(row, 0, nameItem);
        customModelsTable_->setItem(row, 1, new QTableWidgetItem(model.architectureHint));
        customModelsTable_->setItem(
            row, 2, new QTableWidgetItem(QStringLiteral("%1x").arg(model.scaleFactor)));
        customModelsTable_->setItem(row, 3, new QTableWidgetItem(formatByteSize(model.bytes)));
        customModelsTable_->setItem(
            row, 4, new QTableWidgetItem(
                        externalCandidate ? tr("External — select and Import to register")
                                          : model.valid ? tr("Ready")
                                                        : tr("Invalid: %1").arg(model.reason)));
        for (int column = 1; column < customModelsTable_->columnCount(); ++column) {
            auto* item = customModelsTable_->item(row, column);
            item->setToolTip(item->text());
        }
    }
    modelsCountBadge_->setText(
        tr("%1 custom").arg(QLocale().toString(static_cast<qlonglong>(models.size()))));
    if (!inventoryError.isEmpty())
        updateStatusBanner(inventoryError, QStringLiteral("danger"));
    const int row = customModelsTable_->currentRow();
    removeModelButton_->setEnabled(row >= 0 && row < registeredCount);

    rebuildModelStepRows();
}

int AnimeJanaiManagerPanel::currentSlotNumber() const {
    if (slotSelectCombo_ == nullptr || slotSelectCombo_->currentIndex() < 0) return 1;
    return slotSelectCombo_->currentData().toInt();
}

mplayer::native::AnimeJanaiSlot* AnimeJanaiManagerPanel::findMutableSlot(int slotNumber) {
    for (mplayer::native::AnimeJanaiSlot& slot : config_.customSlots) {
        if (slot.number == slotNumber) return &slot;
    }
    return nullptr;
}

const mplayer::native::AnimeJanaiSlot* AnimeJanaiManagerPanel::findSlot(int slotNumber) const {
    for (const mplayer::native::AnimeJanaiSlot& slot : config_.customSlots) {
        if (slot.number == slotNumber) return &slot;
    }
    return nullptr;
}

mplayer::native::AnimeJanaiChain* AnimeJanaiManagerPanel::currentMutableChain() {
    auto* slot = findMutableSlot(editingSlotNumber_);
    if (slot == nullptr || slot->chains.isEmpty()) return nullptr;
    const int idx = editingChainIndex_;
    if (idx < 0 || idx >= slot->chains.size()) return &slot->chains[0];
    return &slot->chains[idx];
}

void AnimeJanaiManagerPanel::rebuildSlotCombo() {
    const bool prevUpdating = updatingUi_;
    updatingUi_ = true;
    const int previousSlot = currentSlotNumber();
    slotSelectCombo_->clear();
    for (int slotNum = 1; slotNum <= kMaxCustomSlots; ++slotNum) {
        const auto* slot = findSlot(slotNum);
        if (slot != nullptr && !slot->chains.isEmpty()) {
            const QString title = slot->profileName.trimmed().isEmpty()
                                      ? tr("Custom Profile")
                                      : slot->profileName.trimmed();
            slotSelectCombo_->addItem(tr("Slot %1: %2").arg(slotNum).arg(title), slotNum);
        } else {
            slotSelectCombo_->addItem(tr("Slot %1 (Unassigned)").arg(slotNum), slotNum);
        }
    }
    const int idx = slotSelectCombo_->findData(previousSlot);
    slotSelectCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
    updatingUi_ = prevUpdating;
}

void AnimeJanaiManagerPanel::loadSelectedSlotIntoEditor() {
    const bool prevUpdating = updatingUi_;
    updatingUi_ = true;

    const int slotNum = currentSlotNumber();
    editingSlotNumber_ = slotNum;
    editingChainIndex_ = std::max(0, chainSelectCombo_->currentIndex());
    const auto* slot = findSlot(slotNum);
    const bool active = slot != nullptr && !slot->chains.isEmpty();

    slotEditorContainer_->setVisible(active);
    deleteSlotButton_->setEnabled(active);
    exportPconfButton_->setEnabled(active);

    if (active) {
        profileNameEdit_->setText(slot->profileName);
        rebuildChainCombo();
        loadSelectedChainIntoEditor();
    } else {
        profileNameEdit_->clear();
        chainSelectCombo_->clear();
    }

    updatingUi_ = prevUpdating;
}

void AnimeJanaiManagerPanel::rebuildChainCombo() {
    const bool prevUpdating = updatingUi_;
    updatingUi_ = true;
    const int previousIndex = std::max(0, chainSelectCombo_->currentIndex());
    chainSelectCombo_->clear();
    const auto* slot = findSlot(currentSlotNumber());
    if (slot != nullptr) {
        for (int i = 0; i < slot->chains.size(); ++i) {
            const auto& chain = slot->chains.at(i);
            chainSelectCombo_->addItem(
                tr("Chain %1 (%2 -> %3)")
                    .arg(i + 1)
                    .arg(chain.minResolution,
                         chain.maxResolution == QStringLiteral("0x0") ? tr("inf")
                                                                      : chain.maxResolution),
                i);
        }
        if (!slot->chains.isEmpty()) {
            chainSelectCombo_->setCurrentIndex(
                std::min(previousIndex, static_cast<int>(slot->chains.size()) - 1));
        }
    }
    updatingUi_ = prevUpdating;
}

void AnimeJanaiManagerPanel::loadSelectedChainIntoEditor() {
    editingSlotNumber_ = currentSlotNumber();
    editingChainIndex_ = std::max(0, chainSelectCombo_->currentIndex());
    const bool prevUpdating = updatingUi_;
    updatingUi_ = true;

    const auto* slot = findSlot(currentSlotNumber());
    const int chainIdx = chainSelectCombo_->currentIndex();
    if (slot == nullptr || chainIdx < 0 || chainIdx >= slot->chains.size()) {
        updatingUi_ = prevUpdating;
        return;
    }

    const auto& chain = slot->chains.at(chainIdx);
    minResEdit_->setText(chain.minResolution);
    maxResEdit_->setText(chain.maxResolution);
    minFpsSpin_->setValue(chain.minFps);
    maxFpsSpin_->setValue(chain.maxFps);

    chainRifeEnableCheck_->setChecked(chain.rifeEnabled);
    chainRifeContainer_->setVisible(chain.rifeEnabled);
    const int rifeModelIdx = chainRifeModelCombo_->findData(chain.rifeModel);
    if (rifeModelIdx >= 0) {
        chainRifeModelCombo_->setCurrentIndex(rifeModelIdx);
    } else if (!chain.rifeModel.isEmpty()) {
        chainRifeModelCombo_->addItem(chain.rifeModel, chain.rifeModel);
        chainRifeModelCombo_->setCurrentIndex(chainRifeModelCombo_->count() - 1);
    }
    chainRifeNumSpin_->setValue(std::max(1, chain.rifeFactorNumerator));
    chainRifeDenSpin_->setValue(std::max(1, chain.rifeFactorDenominator));
    chainRifeSceneSpin_->setValue(chain.rifeSceneDetectThreshold);
    chainRifeEnsembleCheck_->setChecked(chain.rifeEnsemble);
    chainRifeBeforeUpscaleCheck_->setChecked(chain.rifeBeforeUpscale);

    removeChainButton_->setEnabled(slot->chains.size() > 1);
    moveChainUpButton_->setEnabled(chainIdx > 0);
    moveChainDownButton_->setEnabled(chainIdx + 1 < slot->chains.size());

    rebuildModelStepRows();
    updatingUi_ = prevUpdating;
}

void AnimeJanaiManagerPanel::rebuildModelStepRows() {
    const bool prevUpdating = updatingUi_;
    updatingUi_ = true;
    for (ModelStepRow& row : modelStepRows_) {
        if (row.frame != nullptr) {
            modelStepsLayout_->removeWidget(row.frame);
            row.frame->hide();
            row.frame->setParent(nullptr);
            row.frame->deleteLater();
        }
    }
    modelStepRows_.clear();

    auto* chain = currentMutableChain();
    if (chain == nullptr) {
        updatingUi_ = prevUpdating;
        return;
    }
    const QStringList availableModels =
        mplayer::native::AnimeJanaiConfigStore::availableUpscaleModelNames();
    const auto a4kModes = mplayer::anime4k::availableModes();
    const auto stableA4kId = [](const QString& id) {
        static const QStringList ids = {
            QStringLiteral("fast_a"), QStringLiteral("fast_b"), QStringLiteral("fast_c"),
            QStringLiteral("hq_a"), QStringLiteral("hq_b"), QStringLiteral("hq_c"),
            QStringLiteral("hq_aa"), QStringLiteral("hq_bb"), QStringLiteral("hq_ca")};
        return ids.contains(id);
    };

    for (int i = 0; i < chain->steps.size(); ++i) {
        const auto& step = chain->steps.at(i);
        ModelStepRow row;
        row.frame = new QFrame(modelStepsContainer_);
        row.frame->setObjectName(QStringLiteral("animeJanaiStepRow_%1").arg(i + 1));
        auto* rowLayout = new QVBoxLayout(row.frame);
        rowLayout->setContentsMargins(theme::kSpacingSmall, theme::kSpacingXs,
                                      theme::kSpacingSmall, theme::kSpacingXs);
        rowLayout->setSpacing(theme::kSpacingXs);

        row.stepLabel = new QLabel(tr("Step %1").arg(i + 1), row.frame);
        row.stepLabel->setObjectName(QStringLiteral("SettingsFormLabel"));
        row.kindCombo = new QComboBox(row.frame);
        row.kindCombo->setObjectName(QStringLiteral("animeJanaiStepKind_%1").arg(i + 1));
        row.kindCombo->setProperty("stepIndex", i);
        row.kindCombo->addItem(tr("ONNX model"), static_cast<int>(
            mplayer::native::AnimeJanaiStepKind::OnnxModel));
        row.kindCombo->addItem(tr("Anime4K preset"), static_cast<int>(
            mplayer::native::AnimeJanaiStepKind::Anime4K));
        row.kindCombo->setCurrentIndex(row.kindCombo->findData(static_cast<int>(step.kind)));
        row.kindCombo->setAccessibleName(tr("Step %1 type").arg(i + 1));
        polishInteractiveControl(row.kindCombo);

        const bool isA4k = step.kind == mplayer::native::AnimeJanaiStepKind::Anime4K;
        row.modelCombo = new QComboBox(row.frame);
        row.modelCombo->setObjectName(QStringLiteral("animeJanaiStepId_%1").arg(i + 1));
        row.modelCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        row.modelCombo->setMinimumContentsLength(theme::kSettingsCompactChoiceLengthChars);
        row.modelCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        if (isA4k) {
            for (const auto& mode : a4kModes) {
                if (!stableA4kId(mode.id)) continue;
                QString label;
                if (mode.id == QLatin1String("fast_a")) label = QCoreApplication::translate("Anime4K", "A (Fast)");
                else if (mode.id == QLatin1String("fast_b")) label = QCoreApplication::translate("Anime4K", "B (Fast)");
                else if (mode.id == QLatin1String("fast_c")) label = QCoreApplication::translate("Anime4K", "C (Fast)");
                else if (mode.id == QLatin1String("hq_a")) label = QCoreApplication::translate("Anime4K", "A (High Quality)");
                else if (mode.id == QLatin1String("hq_b")) label = QCoreApplication::translate("Anime4K", "B (High Quality)");
                else if (mode.id == QLatin1String("hq_c")) label = QCoreApplication::translate("Anime4K", "C (High Quality)");
                else if (mode.id == QLatin1String("hq_aa")) label = QCoreApplication::translate("Anime4K", "A+A (Maximum)");
                else if (mode.id == QLatin1String("hq_bb")) label = QCoreApplication::translate("Anime4K", "B+B (Maximum)");
                else if (mode.id == QLatin1String("hq_ca")) label = QCoreApplication::translate("Anime4K", "C+A (Maximum)");
                row.modelCombo->addItem(label, mode.id);
            }
            int found = row.modelCombo->findData(step.id);
            if (found < 0 && !step.id.isEmpty()) {
                row.modelCombo->addItem(step.id, step.id);
                found = row.modelCombo->count() - 1;
            }
            if (found >= 0) row.modelCombo->setCurrentIndex(found);
            row.modelCombo->setEditable(false);
        } else {
            row.modelCombo->setEditable(true);
            row.modelCombo->addItem(tr("(None)"), QString());
            for (const QString& name : availableModels) row.modelCombo->addItem(name, name);
            if (!step.id.isEmpty() && row.modelCombo->findData(step.id) < 0)
                row.modelCombo->addItem(step.id, step.id);
            const int found = row.modelCombo->findData(step.id);
            if (found >= 0) row.modelCombo->setCurrentIndex(found);
            else row.modelCombo->setCurrentText(step.id);
        }
        row.modelLabel = makeFieldLabel(isA4k ? tr("Anime4K preset") : tr("ONNX model"),
                                        row.frame, row.modelCombo);
        row.modelCombo->setAccessibleName(isA4k ? tr("Step %1 Anime4K preset").arg(i + 1)
                                                : tr("Step %1 ONNX model").arg(i + 1));
        polishInteractiveControl(row.modelCombo);

        row.resizeHeightLabel = makeFieldLabel(tr("Input resize height (px)"), row.frame);
        row.resizeHeightSpin = new QSpinBox(row.frame);
        row.resizeHeightSpin->setObjectName(QStringLiteral("animeJanaiStepResizeHeight_%1").arg(i + 1));
        row.resizeHeightSpin->setRange(0, 4320);
        row.resizeHeightSpin->setSpecialValueText(tr("0 (Off)"));
        row.resizeHeightSpin->setValue(step.resizeHeightBeforeUpscale);
        row.resizeHeightSpin->setAccessibleName(tr("Step %1 input resize height").arg(i + 1));
        polishInteractiveControl(row.resizeHeightSpin);
        row.resizeHeightLabel->setBuddy(row.resizeHeightSpin);

        row.resizeFactorLabel = makeFieldLabel(tr("Input resize percentage"), row.frame);
        row.resizeFactorSpin = new QSpinBox(row.frame);
        row.resizeFactorSpin->setObjectName(QStringLiteral("animeJanaiStepResizePercent_%1").arg(i + 1));
        row.resizeFactorSpin->setRange(1, 100);
        row.resizeFactorSpin->setSuffix(QStringLiteral("%"));
        row.resizeFactorSpin->setValue(std::clamp(step.resizeFactorBeforeUpscale, 1, 100));
        row.resizeFactorSpin->setAccessibleName(tr("Step %1 input resize percentage").arg(i + 1));
        polishInteractiveControl(row.resizeFactorSpin);
        row.resizeFactorLabel->setBuddy(row.resizeFactorSpin);

        row.scaleLabel = makeFieldLabel(tr("Anime4K output scale"), row.frame);
        row.scaleCombo = new QComboBox(row.frame);
        row.scaleCombo->setObjectName(QStringLiteral("animeJanaiStepScale_%1").arg(i + 1));
        for (const int scale : {1, 2, 4}) row.scaleCombo->addItem(tr("%1×").arg(scale), scale);
        const int scaleIndex = row.scaleCombo->findData(step.scaleFactor);
        row.scaleCombo->setCurrentIndex(scaleIndex >= 0 ? scaleIndex : row.scaleCombo->findData(2));
        row.scaleCombo->setAccessibleName(tr("Step %1 Anime4K output scale").arg(i + 1));
        polishInteractiveControl(row.scaleCombo);
        row.scaleLabel->setBuddy(row.scaleCombo);
        row.scaleLabel->setVisible(isA4k);
        row.scaleCombo->setVisible(isA4k);

        row.moveUpButton = new QPushButton(QStringLiteral("↑"), row.frame);
        row.moveUpButton->setToolTip(tr("Move Up"));
        row.moveUpButton->setObjectName(QStringLiteral("animeJanaiStepMoveUp_%1").arg(i + 1));
        row.moveUpButton->setAccessibleName(tr("Move step %1 up").arg(i + 1));
        row.moveUpButton->setEnabled(i > 0);
        row.moveDownButton = new QPushButton(QStringLiteral("↓"), row.frame);
        row.moveDownButton->setToolTip(tr("Move Down"));
        row.moveDownButton->setObjectName(QStringLiteral("animeJanaiStepMoveDown_%1").arg(i + 1));
        row.moveDownButton->setAccessibleName(tr("Move step %1 down").arg(i + 1));
        row.moveDownButton->setEnabled(i + 1 < chain->steps.size());
        row.removeButton = new QPushButton(QStringLiteral("×"), row.frame);
        row.removeButton->setAccessibleName(tr("Remove step %1").arg(i + 1));
        row.removeButton->setToolTip(tr("Remove"));
        row.removeButton->setObjectName(QStringLiteral("animeJanaiStepRemove_%1").arg(i + 1));
        polishInteractiveControl(row.moveUpButton);
        polishInteractiveControl(row.moveDownButton);
        polishInteractiveControl(row.removeButton);

        auto* stepHeader = new QHBoxLayout();
        stepHeader->setContentsMargins(0, 0, 0, 0);
        stepHeader->setSpacing(theme::kSpacingSmall);
        stepHeader->addWidget(row.stepLabel, 1);
        stepHeader->addWidget(row.moveUpButton);
        stepHeader->addWidget(row.moveDownButton);
        stepHeader->addWidget(row.removeButton);
        rowLayout->addLayout(stepHeader);
        auto* stepForm = new QFormLayout();
        stepForm->setContentsMargins(0, 0, 0, 0);
        stepForm->setHorizontalSpacing(theme::kSpacingSmall);
        stepForm->setVerticalSpacing(theme::kSpacingXs);
        stepForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
        stepForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        auto* kindLabel = makeFieldLabel(tr("Step type"), row.frame, row.kindCombo);
        stepForm->addRow(kindLabel, row.kindCombo);
        stepForm->addRow(row.modelLabel, row.modelCombo);
        stepForm->addRow(row.resizeHeightLabel, row.resizeHeightSpin);
        stepForm->addRow(row.resizeFactorLabel, row.resizeFactorSpin);
        stepForm->addRow(row.scaleLabel, row.scaleCombo);
        rowLayout->addLayout(stepForm);

        const int stepIndex = i;
        connect(row.kindCombo, &QComboBox::currentIndexChanged, this, [this](int) {
            if (updatingUi_) return;
            auto* kindCombo = qobject_cast<QComboBox*>(sender());
            const int changedIndex = kindCombo != nullptr
                                         ? kindCombo->property("stepIndex").toInt() : -1;
            commitCurrentChainFromEditor();
            auto* currentChain = currentMutableChain();
            if (currentChain != nullptr && changedIndex >= 0 &&
                changedIndex < currentChain->steps.size()) {
                auto& changedStep = currentChain->steps[changedIndex];
                if (changedStep.kind == mplayer::native::AnimeJanaiStepKind::Anime4K)
                    changedStep.id = QStringLiteral("fast_a");
                else if (changedStep.kind == mplayer::native::AnimeJanaiStepKind::OnnxModel) {
                    const QStringList models =
                        mplayer::native::AnimeJanaiConfigStore::availableUpscaleModelNames();
                    changedStep.id = models.isEmpty() ? QString() : models.first();
                }
            }
            rebuildModelStepRows();
            markDirty();
        });
        connect(row.modelCombo, &QComboBox::currentTextChanged, this, [this](const QString&) {
            if (updatingUi_) return;
            commitCurrentChainFromEditor();
            markDirty();
        });
        connect(row.resizeHeightSpin, &QSpinBox::valueChanged, this, [this](int) {
            if (updatingUi_) return;
            commitCurrentChainFromEditor();
            markDirty();
        });
        connect(row.resizeFactorSpin, &QSpinBox::valueChanged, this, [this](int) {
            if (updatingUi_) return;
            commitCurrentChainFromEditor();
            markDirty();
        });
        connect(row.scaleCombo, &QComboBox::currentIndexChanged, this, [this](int) {
            if (updatingUi_) return;
            commitCurrentChainFromEditor();
            markDirty();
        });
        connect(row.moveUpButton, &QPushButton::clicked, this, [this, stepIndex]() {
            onMoveModelStep(stepIndex, -1);
        });
        connect(row.moveDownButton, &QPushButton::clicked, this, [this, stepIndex]() {
            onMoveModelStep(stepIndex, 1);
        });
        connect(row.removeButton, &QPushButton::clicked, this, [this, stepIndex]() {
            onRemoveModelStep(stepIndex);
        });
        modelStepsLayout_->addWidget(row.frame);
        modelStepRows_.append(row);
    }
    updatingUi_ = prevUpdating;
}

void AnimeJanaiManagerPanel::commitCurrentChainFromEditor() {
    auto* slot = findMutableSlot(editingSlotNumber_);
    if (slot == nullptr || slot->chains.isEmpty()) return;
    slot->profileName = profileNameEdit_->text().trimmed().isEmpty()
                            ? tr("Slot %1").arg(slot->number)
                            : profileNameEdit_->text().trimmed();

    auto* chain = currentMutableChain();
    if (chain == nullptr) return;
    chain->minResolution = minResEdit_->text().trimmed().isEmpty()
                               ? QStringLiteral("0x0")
                               : minResEdit_->text().trimmed();
    chain->maxResolution = maxResEdit_->text().trimmed().isEmpty()
                               ? QStringLiteral("0x0")
                               : maxResEdit_->text().trimmed();
    chain->minFps = minFpsSpin_->value();
    chain->maxFps = maxFpsSpin_->value();

    chain->steps.clear();
    for (int i = 0; i < modelStepRows_.size(); ++i) {
        const ModelStepRow& row = modelStepRows_.at(i);
        mplayer::native::AnimeJanaiChainStep step;
        step.number = i + 1;
        step.kind = static_cast<mplayer::native::AnimeJanaiStepKind>(
            row.kindCombo->currentData().toInt());
        if (step.kind == mplayer::native::AnimeJanaiStepKind::Anime4K &&
            row.modelCombo->currentData().isValid()) {
            step.id = row.modelCombo->currentData().toString();
        } else {
            step.id = row.modelCombo->currentText() == tr("(None)")
                          ? QString() : row.modelCombo->currentText().trimmed();
        }
        step.resizeHeightBeforeUpscale = row.resizeHeightSpin->value();
        step.resizeFactorBeforeUpscale = row.resizeFactorSpin->value();
        step.scaleFactor = row.scaleCombo->currentData().toInt();
        chain->steps.append(step);
    }

    chain->rifeEnabled = chainRifeEnableCheck_->isChecked();
    chain->rifeModel = chainRifeModelCombo_->currentText().trimmed();
    chain->rifeFactorNumerator = chainRifeNumSpin_->value();
    chain->rifeFactorDenominator = chainRifeDenSpin_->value();
    chain->rifeSceneDetectThreshold = chainRifeSceneSpin_->value();
    chain->rifeEnsemble = chainRifeEnsembleCheck_->isChecked();
    chain->rifeBeforeUpscale = chainRifeBeforeUpscaleCheck_->isChecked();
}

void AnimeJanaiManagerPanel::onImportOnnxModels() {
    const int selectedRow = customModelsTable_->currentRow();
    const QTableWidgetItem* selectedItem =
        selectedRow >= 0 ? customModelsTable_->item(selectedRow, 0) : nullptr;
    if (selectedItem != nullptr && selectedItem->data(Qt::UserRole + 1).toBool()) {
        if (runner_ == nullptr) {
            updateStatusBanner(tr("External model registration is unavailable."),
                               QStringLiteral("danger"));
            return;
        }
        const QString sourcePath = selectedItem->toolTip();
        const QString externalDirectory = config_.externalModelsDir;
        const auto alive = alive_;
        updateStatusBanner(tr("Registering external ONNX model…"), QStringLiteral("info"));
        runner_->runTransfer(
            [sourcePath, externalDirectory]() -> QVariant {
                mplayer::native::CustomAnimeJanaiModel registered;
                QString error;
                const bool ok = mplayer::native::registerExternalCustomAnimeJanaiModel(
                    sourcePath, &registered, &error, {}, externalDirectory);
                return QVariantMap{{QStringLiteral("ok"), ok},
                                   {QStringLiteral("error"), error}};
            },
            [this, alive](const QVariant& value) {
                if (!alive->load(std::memory_order_acquire)) return;
                const QVariantMap result = value.toMap();
                if (!result.value(QStringLiteral("ok")).toBool()) {
                    updateStatusBanner(result.value(QStringLiteral("error")).toString(),
                                       QStringLiteral("danger"));
                    return;
                }
                refreshCustomModels();
                emit modelsChanged();
                updateStatusBanner(tr("External ONNX model registered."),
                                   QStringLiteral("success"));
            },
            [this, alive](const backend::Failure& failure) {
                if (alive->load(std::memory_order_acquire))
                    updateStatusBanner(failure.message, QStringLiteral("danger"));
            });
        return;
    }

    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Import RealESRGAN / ONNX Spatial Models"), QString(),
        tr("ONNX Models (*.onnx)"));
    if (paths.isEmpty()) return;
    if (runner_ == nullptr) {
        updateStatusBanner(tr("Model import is unavailable."), QStringLiteral("danger"));
        return;
    }

    auto remaining = std::make_shared<int>(paths.size());
    auto importedCount = std::make_shared<int>(0);
    auto lastError = std::make_shared<QString>();
    const auto alive = alive_;
    const auto dispatch = runner_->post();
    updateStatusBanner(tr("Importing ONNX models…"), QStringLiteral("info"));
    for (const QString& path : paths) {
        runner_->runTransfer(
            [path, alive, this, dispatch]() -> QVariant {
                dispatch([alive, this]() {
                    if (alive->load(std::memory_order_acquire))
                        updateStatusBanner(tr("Importing ONNX model…"), QStringLiteral("info"));
                });
                mplayer::native::CustomAnimeJanaiModel imported;
                QString error;
                const bool ok =
                    mplayer::native::importCustomAnimeJanaiModel(path, &imported, &error);
                return QVariantMap{{QStringLiteral("ok"), ok},
                                   {QStringLiteral("id"), imported.id},
                                   {QStringLiteral("error"), error}};
            },
            [this, alive, remaining, importedCount, lastError](const QVariant& value) {
                if (!alive->load(std::memory_order_acquire)) return;
                const QVariantMap result = value.toMap();
                if (result.value(QStringLiteral("ok")).toBool()) {
                    ++*importedCount;
                } else {
                    *lastError = result.value(QStringLiteral("error")).toString();
                }
                if (--*remaining != 0) return;
                refreshCustomModels();
                if (*importedCount > 0) emit modelsChanged();
                if (!lastError->isEmpty())
                    updateStatusBanner(*lastError, QStringLiteral("danger"));
                else
                    updateStatusBanner(tr("Imported %1 custom ONNX model(s).").arg(*importedCount),
                                       QStringLiteral("success"));
            },
            [this, alive, remaining, importedCount, lastError](const backend::Failure& failure) {
                if (!alive->load(std::memory_order_acquire)) return;
                *lastError = failure.message;
                if (--*remaining == 0) {
                    refreshCustomModels();
                    if (*importedCount > 0) emit modelsChanged();
                    updateStatusBanner(*lastError, QStringLiteral("danger"));
                }
            });
    }
}

void AnimeJanaiManagerPanel::onRemoveSelectedOnnxModel() {
    const int row = customModelsTable_->currentRow();
    if (row < 0 || runner_ == nullptr) return;
    const QTableWidgetItem* item = customModelsTable_->item(row, 0);
    if (item == nullptr) return;
    const QString id = item->data(Qt::UserRole).toString();
    const auto alive = alive_;
    updateStatusBanner(tr("Removing custom ONNX model…"), QStringLiteral("info"));
    runner_->runTransfer(
        [id]() -> QVariant {
            QString error;
            const bool ok = mplayer::native::removeCustomAnimeJanaiModel(id, &error);
            return QVariantMap{{QStringLiteral("ok"), ok}, {QStringLiteral("error"), error}};
        },
        [this, alive, id](const QVariant& value) {
            if (!alive->load(std::memory_order_acquire)) return;
            const QVariantMap result = value.toMap();
            if (!result.value(QStringLiteral("ok")).toBool()) {
                updateStatusBanner(result.value(QStringLiteral("error")).toString(),
                                   QStringLiteral("danger"));
                return;
            }
            refreshCustomModels();
            emit modelsChanged();
            updateStatusBanner(tr("Removed custom ONNX model '%1'.").arg(id),
                               QStringLiteral("success"));
        },
        [this, alive](const backend::Failure& failure) {
            if (alive->load(std::memory_order_acquire))
                updateStatusBanner(failure.message, QStringLiteral("danger"));
        });
}

void AnimeJanaiManagerPanel::onOpenCustomModelsFolder() {
    const QString dirPath = mplayer::native::customAnimeJanaiModelsDir();
    if (!QDir().mkpath(dirPath) ||
        !QDesktopServices::openUrl(QUrl::fromLocalFile(dirPath))) {
        updateStatusBanner(tr("Could not open the custom models folder."),
                           QStringLiteral("danger"));
    }
}

void AnimeJanaiManagerPanel::onBrowseExternalModelsFolder() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select External ONNX Models Directory"), externalDirEdit_->text());
    if (dir.isEmpty()) return;
    externalDirEdit_->setText(QDir::cleanPath(dir));
    config_.externalModelsDir = externalDirEdit_->text();
    markDirty();
    refreshCustomModels();
}

void AnimeJanaiManagerPanel::onClearExternalModelsFolder() {
    if (externalDirEdit_->text().isEmpty()) return;
    externalDirEdit_->clear();
    config_.externalModelsDir.clear();
    markDirty();
    refreshCustomModels();
}

void AnimeJanaiManagerPanel::onImportFullConf() {
    commitCurrentChainFromEditor();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import animejanai.conf"), QString(),
        tr("AnimeJaNai Config (*.conf *.ini);;All Files (*)"));
    if (path.isEmpty()) return;
    QString error;
    const auto imported = mplayer::native::AnimeJanaiConfigStore::loadFromFile(path, &error);
    if (!error.isEmpty()) {
        showSaveError(error);
        return;
    }
    config_ = imported;
    loadConfigIntoWidgets(config_);
    markDirty();
    updateStatusBanner(tr("Imported configuration from '%1'.").arg(QFileInfo(path).fileName()),
                       QStringLiteral("success"));
}

void AnimeJanaiManagerPanel::onExportFullConf() {
    commitCurrentChainFromEditor();
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export animejanai.conf"), QStringLiteral("animejanai.conf"),
        tr("AnimeJaNai Config (*.conf *.ini)"));
    if (path.isEmpty()) return;
    QString error;
    if (!mplayer::native::AnimeJanaiConfigStore::saveToFile(path, config_, &error)) {
        updateStatusBanner(error, QStringLiteral("danger"));
        return;
    }
    updateStatusBanner(tr("Exported configuration to '%1'.").arg(QFileInfo(path).fileName()),
                       QStringLiteral("success"));
}

void AnimeJanaiManagerPanel::onResetDefaults() {
    commitCurrentChainFromEditor();
    config_ = mplayer::native::AnimeJanaiConfig{};
    loadConfigIntoWidgets(config_);
    markDirty();
    updateStatusBanner(tr("Reset AnimeJaNai settings and custom slots to defaults."),
                       QStringLiteral("info"));
}

void AnimeJanaiManagerPanel::onCreateOrResetSlot() {
    commitCurrentChainFromEditor();
    const int slotNum = currentSlotNumber();
    mplayer::native::AnimeJanaiSlot fresh;
    fresh.number = slotNum;
    fresh.profileName = tr("Custom Slot %1").arg(slotNum);
    mplayer::native::AnimeJanaiChain defaultChain;
    defaultChain.number = 1;
    defaultChain.minResolution = QStringLiteral("0x0");
    defaultChain.maxResolution = QStringLiteral("1920x1080");
    const QStringList models =
        mplayer::native::AnimeJanaiConfigStore::availableUpscaleModelNames();
    mplayer::native::AnimeJanaiChainStep step;
    step.number = 1;
    step.kind = mplayer::native::AnimeJanaiStepKind::OnnxModel;
    step.id = models.isEmpty() ? QString() : models.first();
    defaultChain.steps.append(step);
    fresh.chains.append(defaultChain);

    if (auto* existing = findMutableSlot(slotNum)) {
        *existing = fresh;
    } else {
        config_.customSlots.append(fresh);
        std::sort(config_.customSlots.begin(), config_.customSlots.end(),
                  [](const auto& a, const auto& b) { return a.number < b.number; });
    }
    rebuildSlotCombo();
    loadSelectedSlotIntoEditor();
    markDirty();
}

void AnimeJanaiManagerPanel::onCloneSlotMenu() {
    commitCurrentChainFromEditor();
    QMenu menu(this);
    const QVector<mplayer::native::AnimeJanaiSlot> defaults =
        mplayer::native::AnimeJanaiConfigStore::defaultProfiles(config_);
    for (const auto& builtin : defaults) {
        menu.addAction(tr("Built-in: %1").arg(builtin.profileName), this, [this, builtin]() {
            const int targetNum = currentSlotNumber();
            mplayer::native::AnimeJanaiSlot cloned = builtin;
            cloned.number = targetNum;
            cloned.profileName = tr("%1 (Copy)").arg(builtin.profileName);
            if (auto* existing = findMutableSlot(targetNum)) {
                *existing = cloned;
            } else {
                config_.customSlots.append(cloned);
                std::sort(config_.customSlots.begin(), config_.customSlots.end(),
                          [](const auto& a, const auto& b) { return a.number < b.number; });
            }
            rebuildSlotCombo();
            loadSelectedSlotIntoEditor();
            markDirty();
        });
    }
    if (!config_.customSlots.isEmpty()) {
        menu.addSeparator();
        for (const auto& custom : config_.customSlots) {
            if (custom.chains.isEmpty()) continue;
            menu.addAction(
                tr("Slot %1: %2").arg(custom.number).arg(custom.profileName), this,
                [this, custom]() {
                    const int targetNum = currentSlotNumber();
                    mplayer::native::AnimeJanaiSlot cloned = custom;
                    cloned.number = targetNum;
                    cloned.profileName = tr("%1 (Slot %2)").arg(custom.profileName).arg(targetNum);
                    if (auto* existing = findMutableSlot(targetNum)) {
                        *existing = cloned;
                    } else {
                        config_.customSlots.append(cloned);
                        std::sort(config_.customSlots.begin(), config_.customSlots.end(),
                                  [](const auto& a, const auto& b) { return a.number < b.number; });
                    }
                    rebuildSlotCombo();
                    loadSelectedSlotIntoEditor();
                    markDirty();
                });
        }
    }
    menu.exec(cloneSlotButton_->mapToGlobal(QPoint(0, cloneSlotButton_->height())));
}

void AnimeJanaiManagerPanel::onDeleteSlot() {
    commitCurrentChainFromEditor();
    const int slotNum = currentSlotNumber();
    config_.customSlots.erase(
        std::remove_if(config_.customSlots.begin(), config_.customSlots.end(),
                       [slotNum](const auto& slot) { return slot.number == slotNum; }),
        config_.customSlots.end());
    rebuildSlotCombo();
    loadSelectedSlotIntoEditor();
    markDirty();
}

void AnimeJanaiManagerPanel::onImportProfilePconf() {
    commitCurrentChainFromEditor();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Profile (.pconf)"), QString(),
        tr("AnimeJaNai Profile (*.pconf *.conf);;All Files (*)"));
    if (path.isEmpty()) return;
    bool ok = false;
    QString error;
    mplayer::native::AnimeJanaiSlot imported =
        mplayer::native::AnimeJanaiConfigStore::loadProfile(path, &ok, &error);
    if (!ok) {
        updateStatusBanner(error, QStringLiteral("danger"));
        return;
    }
    const int slotNum = currentSlotNumber();
    imported.number = slotNum;
    if (imported.profileName.trimmed().isEmpty()) {
        imported.profileName = QFileInfo(path).completeBaseName();
    }
    if (auto* existing = findMutableSlot(slotNum)) {
        *existing = imported;
    } else {
        config_.customSlots.append(imported);
        std::sort(config_.customSlots.begin(), config_.customSlots.end(),
                  [](const auto& a, const auto& b) { return a.number < b.number; });
    }
    rebuildSlotCombo();
    loadSelectedSlotIntoEditor();
    markDirty();
    updateStatusBanner(
        tr("Imported profile '%1' into Slot %2.").arg(imported.profileName).arg(slotNum),
        QStringLiteral("success"));
}

void AnimeJanaiManagerPanel::onExportProfilePconf() {
    commitCurrentChainFromEditor();
    const auto* slot = findSlot(currentSlotNumber());
    if (slot == nullptr || slot->chains.isEmpty()) return;
    const QString suggested =
        (slot->profileName.trimmed().isEmpty() ? QStringLiteral("profile") : slot->profileName) +
        QStringLiteral(".pconf");
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Profile (.pconf)"), suggested,
        tr("AnimeJaNai Profile (*.pconf)"));
    if (path.isEmpty()) return;
    QString error;
    if (!mplayer::native::AnimeJanaiConfigStore::saveProfile(path, *slot, &error)) {
        updateStatusBanner(error, QStringLiteral("danger"));
        return;
    }
    updateStatusBanner(tr("Exported Slot %1 to '%2'.").arg(slot->number).arg(QFileInfo(path).fileName()),
                       QStringLiteral("success"));
}

void AnimeJanaiManagerPanel::onAddChain() {
    commitCurrentChainFromEditor();
    auto* slot = findMutableSlot(currentSlotNumber());
    if (slot == nullptr) return;
    mplayer::native::AnimeJanaiChain chain;
    chain.number = static_cast<int>(slot->chains.size()) + 1;
    chain.minResolution = QStringLiteral("0x0");
    chain.maxResolution = QStringLiteral("0x0");
    mplayer::native::AnimeJanaiChainStep step;
    step.number = 1;
    step.kind = mplayer::native::AnimeJanaiStepKind::OnnxModel;
    const QStringList models =
        mplayer::native::AnimeJanaiConfigStore::availableUpscaleModelNames();
    if (!models.isEmpty()) step.id = models.first();
    chain.steps.append(step);
    slot->chains.append(chain);
    rebuildChainCombo();
    {
        QSignalBlocker blocker(chainSelectCombo_);
        chainSelectCombo_->setCurrentIndex(static_cast<int>(slot->chains.size()) - 1);
    }
    loadSelectedChainIntoEditor();
    markDirty();
}

void AnimeJanaiManagerPanel::onRemoveChain() {
    commitCurrentChainFromEditor();
    auto* slot = findMutableSlot(currentSlotNumber());
    if (slot == nullptr || slot->chains.size() <= 1) return;
    const int idx = chainSelectCombo_->currentIndex();
    if (idx < 0 || idx >= slot->chains.size()) return;
    slot->chains.removeAt(idx);
    for (int i = 0; i < slot->chains.size(); ++i) {
        slot->chains[i].number = i + 1;
    }
    rebuildChainCombo();
    loadSelectedChainIntoEditor();
    markDirty();
}

void AnimeJanaiManagerPanel::onMoveChainUp() {
    commitCurrentChainFromEditor();
    auto* slot = findMutableSlot(currentSlotNumber());
    const int idx = chainSelectCombo_->currentIndex();
    if (slot == nullptr || idx <= 0 || idx >= slot->chains.size()) return;
    slot->chains.swapItemsAt(idx, idx - 1);
    for (int i = 0; i < slot->chains.size(); ++i) slot->chains[i].number = i + 1;
    rebuildChainCombo();
    {
        QSignalBlocker blocker(chainSelectCombo_);
        chainSelectCombo_->setCurrentIndex(idx - 1);
    }
    loadSelectedChainIntoEditor();
    markDirty();
}

void AnimeJanaiManagerPanel::onMoveChainDown() {
    commitCurrentChainFromEditor();
    auto* slot = findMutableSlot(currentSlotNumber());
    const int idx = chainSelectCombo_->currentIndex();
    if (slot == nullptr || idx < 0 || idx + 1 >= slot->chains.size()) return;
    slot->chains.swapItemsAt(idx, idx + 1);
    for (int i = 0; i < slot->chains.size(); ++i) slot->chains[i].number = i + 1;
    rebuildChainCombo();
    {
        QSignalBlocker blocker(chainSelectCombo_);
        chainSelectCombo_->setCurrentIndex(idx + 1);
    }
    loadSelectedChainIntoEditor();
    markDirty();
}

void AnimeJanaiManagerPanel::onAddModelStep() {
    commitCurrentChainFromEditor();
    auto* chain = currentMutableChain();
    if (chain == nullptr) return;
    mplayer::native::AnimeJanaiChainStep step;
    step.number = static_cast<int>(chain->steps.size()) + 1;
    step.kind = mplayer::native::AnimeJanaiStepKind::OnnxModel;
    const QStringList models =
        mplayer::native::AnimeJanaiConfigStore::availableUpscaleModelNames();
    if (!models.isEmpty()) step.id = models.first();
    chain->steps.append(step);
    rebuildModelStepRows();
    markDirty();
}

void AnimeJanaiManagerPanel::onRemoveModelStep(int index) {
    commitCurrentChainFromEditor();
    auto* chain = currentMutableChain();
    if (chain == nullptr || index < 0 || index >= chain->steps.size()) return;
    chain->steps.removeAt(index);
    for (int i = 0; i < chain->steps.size(); ++i) chain->steps[i].number = i + 1;
    rebuildModelStepRows();
    markDirty();
}

void AnimeJanaiManagerPanel::onMoveModelStep(int index, int direction) {
    commitCurrentChainFromEditor();
    auto* chain = currentMutableChain();
    const int target = index + direction;
    if (chain == nullptr || index < 0 || index >= chain->steps.size() ||
        target < 0 || target >= chain->steps.size()) return;
    chain->steps.swapItemsAt(index, target);
    for (int i = 0; i < chain->steps.size(); ++i) chain->steps[i].number = i + 1;
    rebuildModelStepRows();
    markDirty();
}

void AnimeJanaiManagerPanel::retranslateUi() {
    commitCurrentChainFromEditor();
    modelsHeading_->setText(tr("Custom RealESRGAN / ONNX spatial models"));
    modelsSubtitle_->setText(
        tr("Import compatible RealESRGAN, Compact (SRVGGNet), SPAN, DAT, or SwinIR .onnx graph "
           "(4D 1x3xHxW RGB). Imported models appear immediately in the player's "
           "ANIMEJANAI -> Spatial models menu, Compare window, Benchmark tab, and custom profile chains."));
    customModelsTable_->setHorizontalHeaderLabels(
        {tr("Model"), tr("Architecture"), tr("Scale"), tr("Size"), tr("Status")});
    importModelButton_->setText(tr("Import .onnx Model…"));
    removeModelButton_->setText(tr("Remove Selected"));
    openModelsDirButton_->setText(tr("Open Models Folder"));
    externalDirLabel_->setText(tr("External models folder"));
    externalDirEdit_->setPlaceholderText(
        tr("Optional external directory containing additional .onnx models…"));
    browseExternalDirButton_->setText(tr("Browse…"));
    clearExternalDirButton_->setText(tr("Clear"));

    globalHeading_->setText(tr("Built-in preset variants & TensorRT engine settings"));
    globalSubtitle_->setText(
        tr("Switch built-in Quality, Balanced, and Performance profiles between Standard (V3.1) "
           "and Sharp (V3.1Sharp1) models, and customize how TensorRT compiles ONNX graphs."));
    qualityVariantLabel_->setText(tr("Quality preset (Slot 1001)"));
    balancedVariantLabel_->setText(tr("Balanced preset (Slot 1002)"));
    performanceVariantLabel_->setText(tr("Performance preset (Slot 1003)"));
    loggingCheck_->setText(tr("Backend logging"));
    trtModeLabel_->setText(tr("TensorRT engine type"));
    trtOptLevelLabel_->setText(tr("Builder optimization level"));
    trtMinResLabel_->setText(tr("Dynamic minimum resolution"));
    trtOptResLabel_->setText(tr("Dynamic optimal resolution"));
    trtMaxResLabel_->setText(tr("Dynamic maximum resolution"));
    trtRawArgsLabel_->setText(tr("trtexec engine arguments"));
    importConfButton_->setText(tr("Import animejanai.conf…"));
    exportConfButton_->setText(tr("Export animejanai.conf…"));
    resetDefaultsButton_->setText(tr("Reset Defaults"));

    profilesHeading_->setText(tr("Custom multi-chain profile editor (Slots 1–10)"));
    profilesSubtitle_->setText(
        tr("Build custom AnimeJaNai profile slots with resolution/FPS activation conditions, "
           "ordered ONNX and Anime4K processing steps, input resizing, and per-chain RIFE interpolation. "
           "Active custom slots appear in the player's ANIMEJANAI menu."));
    slotSelectLabel_->setText(tr("Profile slot"));
    createSlotButton_->setText(tr("Activate / Reset Slot"));
    cloneSlotButton_->setText(tr("Clone From…"));
    deleteSlotButton_->setText(tr("Clear Slot"));
    importPconfButton_->setText(tr("Import .pconf…"));
    exportPconfButton_->setText(tr("Export .pconf…"));
    profileNameLabel_->setText(tr("Profile name"));
    chainSelectLabel_->setText(tr("Active chain"));
    addChainButton_->setText(tr("Add Chain"));
    removeChainButton_->setText(tr("Remove Chain"));
    moveChainUpButton_->setText(tr("Move Up"));
    moveChainDownButton_->setText(tr("Move Down"));
    minResLabel_->setText(tr("Min resolution (WxH)"));
    maxResLabel_->setText(tr("Max resolution (WxH)"));
    minFpsLabel_->setText(tr("Min FPS"));
    maxFpsLabel_->setText(tr("Max FPS"));
    upscaleStepsHeading_->setText(tr("Processing steps in this chain"));
    addModelStepButton_->setText(tr("Add Processing Step"));
    chainRifeEnableCheck_->setText(tr("RIFE interpolation"));
    chainRifeModelLabel_->setText(tr("Chain RIFE model"));
    chainRifeFactorLabel_->setText(tr("Interpolation FPS factor"));
    chainRifeSceneLabel_->setText(tr("Scene change threshold"));
    chainRifeEnsembleCheck_->setText(tr("RIFE ensemble"));
    chainRifeBeforeUpscaleCheck_->setText(tr("RIFE before upscaling"));

    refreshCustomModels();
    rebuildSlotCombo();
    rebuildChainCombo();
    rebuildModelStepRows();
}

}  // namespace mikai::app
