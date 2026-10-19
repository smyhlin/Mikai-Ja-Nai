#include "dialogs/SettingsDialog.hpp"

#include "dialogs/AnimeJanaiManagerPanel.hpp"
#include "dialogs/settings/SettingsHelpers.hpp"
#include "dialogs/settings/SettingsUi.hpp"
#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"
#include "workers/Runner.hpp"

#include <backend/MikaiClient.hpp>
#include <dialogs/NativeAssetsPanel.hpp>
#include <dialogs/NativeRifeControls.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>
#include <native/status/NativeStatus.hpp>
#include <player/shell/ProcessingSettingsIntent.hpp>

#include <initializer_list>

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace mikai::app {

QWidget* SettingsDialog::buildProcessingTab() {
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("nativeProcessingScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    applySettingsTheme(scroll);

    auto* page = new QWidget(scroll);
    page->setObjectName(QStringLiteral("processingPage"));
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(theme::kSpacingSmall, theme::kSpacingMedium, theme::kSpacingSmall,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    // ── Editorial Page Header Card ───────────────────────────────────────────
    QFrame* pageHeader = makeSettingsPageHeader(
        page, &nativeHeading_, &nativeDescription_, tr("Processing runtime"),
        tr("Standard manages official AnimeJaNai models and profiles. "
           "Select Native to configure the separate private Native workflow."));
    root->addWidget(pageHeader);

    nativeError_ = new QLabel(page);
    nativeError_->setObjectName(QStringLiteral("status"));
    nativeError_->setWordWrap(true);
    yieldCaptionWidth(nativeError_);
    styleStatusBanner(nativeError_, QStringLiteral("danger"));
    nativeError_->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(theme::kDanger)));
    nativeError_->hide();
    root->addWidget(nativeError_);

    const bool editable = nativePreferences_.schemaRecognised;

    auto addRowTo = [](QVBoxLayout* targetLayout, QWidget* parentWidget,
                       const QString& label, QWidget* field) {
        auto* form = new QFormLayout();
        form->setContentsMargins(0, 0, 0, 0);
        form->setHorizontalSpacing(theme::kSpacingMedium);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        auto* name = new QLabel(label, parentWidget);
        name->setObjectName(QStringLiteral("SettingsFormLabel"));
        name->setWordWrap(true);
        name->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
        name->setBuddy(field);
        if (auto* combo = qobject_cast<QComboBox*>(field)) {
            combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            combo->setMinimumContentsLength(theme::kSettingsCompactChoiceLengthChars);
            combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        }
        form->addRow(name, field);
        targetLayout->addLayout(form);
        return name;
    };

    // ── Card 1: Runtime Module Selection (Always Visible) ────────────────────
    QVBoxLayout* runtimeLayout = nullptr;
    QFrame* runtimeCard = makeSettingsCard(page, &runtimeLayout);
    runtimeLayout->setSpacing(theme::kSpacingSmall);

    runtimeChoice_ = new QComboBox(runtimeCard);
    runtimeChoice_->setObjectName(QStringLiteral("runtimeChoiceCombo"));
    runtimeChoice_->setAccessibleName(tr("mpv runtime"));
    polishInteractiveControl(runtimeChoice_, theme::kSettingsControlSize);
    runtimeChoice_->addItem(tr("Standard"),
                            static_cast<int>(mplayer::native::RuntimeChoice::Standard));
    runtimeChoice_->addItem(tr("Native"),
                            static_cast<int>(mplayer::native::RuntimeChoice::Native));
    const int runtimeIndex =
        runtimeChoice_->findData(static_cast<int>(mplayer::native::storedRuntimeChoice()));
    if (runtimeIndex >= 0) {
        runtimeChoice_->setCurrentIndex(runtimeIndex);
    }
    runtimeModuleRowLabel_ =
        addRowTo(runtimeLayout, runtimeCard, tr("Runtime module"), runtimeChoice_);

    runtimeHint_ = new QLabel(runtimeCard);
    styleMutedCaption(runtimeHint_);
    runtimeLayout->addWidget(runtimeHint_);

    root->addWidget(runtimeCard);

    QVBoxLayout* ajiLayout = nullptr;
    animeJanaiManagerCard_ = makeSettingsCard(page, &ajiLayout);
    animeJanaiPanel_ = new AnimeJanaiManagerPanel(animeJanaiManagerCard_, &runner_);
    animeJanaiPanel_->setAccessibleName(tr("AnimeJaNai model and profile manager"));
    connect(animeJanaiPanel_, &AnimeJanaiManagerPanel::modelsChanged, this,
            &SettingsDialog::refreshBenchmarkCatalogue);
    ajiLayout->addWidget(animeJanaiPanel_);
    root->addWidget(animeJanaiManagerCard_);

    // ── Card 2: Private Native Backend & Recipe Selection ────────────────────
    QVBoxLayout* nativeConfigLayout = nullptr;
    nativeConfigCard_ = makeSettingsCard(page, &nativeConfigLayout);
    nativeConfigLayout->setSpacing(theme::kSpacingSmall);

    nativeBackend_ = new QComboBox(nativeConfigCard_);
    nativeBackend_->setObjectName(QStringLiteral("nativeBackendCombo"));
    nativeBackend_->setAccessibleName(tr("Native backend"));
    polishInteractiveControl(nativeBackend_, theme::kSettingsControlSize);
    for (const mplayer::native::Backend backend : nativeAvailability_.backends) {
        nativeBackend_->addItem(backendLabel(backend), static_cast<int>(backend));
    }
    if (nativeBackend_->findData(static_cast<int>(nativePreferences_.backend)) < 0) {
        nativeBackend_->addItem(backendLabel(nativePreferences_.backend),
                                static_cast<int>(nativePreferences_.backend));
    }
    nativeBackend_->setCurrentIndex(
        nativeBackend_->findData(static_cast<int>(nativePreferences_.backend)));
    nativeBackend_->setEnabled(editable);
    backendRowLabel_ =
        addRowTo(nativeConfigLayout, nativeConfigCard_, tr("Backend"), nativeBackend_);

    nativeRecipe_ = new QComboBox(nativeConfigCard_);
    nativeRecipe_->setObjectName(QStringLiteral("nativeRecipeCombo"));
    nativeRecipe_->setAccessibleName(tr("Native recipe"));
    polishInteractiveControl(nativeRecipe_, theme::kSettingsControlSize);
    populateRecipeChoices(nativePreferences_.recipeId);
    recipeRowLabel_ =
        addRowTo(nativeConfigLayout, nativeConfigCard_, tr("Recipe"), nativeRecipe_);

    QString nativeLoadError;
    if (!nativePreferences_.schemaRecognised) {
        for (const mplayer::native::NativePreferenceIssue& issue : nativePreferences_.issues) {
            if (issue.reasonKey == QLatin1String("native.schemaUnknown")) {
                nativeLoadError = issue.reason;
                break;
            }
        }
    }
    connect(nativeBackend_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshNativeStatus(); });
    connect(nativeRecipe_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshNativeStatus(); });

    nativeStatus_ = new QLabel(nativeConfigCard_);
    nativeStatus_->setObjectName(QStringLiteral("descriptionPanel"));
    nativeStatus_->setAccessibleName(tr("Native processing status"));
    nativeStatus_->setWordWrap(true);
    yieldCaptionWidth(nativeStatus_);
    nativeConfigLayout->addWidget(nativeStatus_);

    root->addWidget(nativeConfigCard_);

    connect(runtimeChoice_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshRuntimeHint(); });

    refreshNativeStatus();
    refreshRuntimeStatus();

    if (!nativeLoadError.isEmpty()) {
        nativeError_->setText(nativeLoadError);
        nativeError_->show();
    }

    // ── Card 3: Native Model Assets & Engine Preparation ─────────────────────
    QVBoxLayout* assetsCardLayout = nullptr;
    nativeAssetsCard_ = makeSettingsCard(page, &assetsCardLayout);
    assetsCardLayout->setSpacing(theme::kSpacingSmall);

    auto* assetsPanel = new mikai::app::NativeAssetsPanel(nativeAssetsCard_);
    assetsPanel->setObjectName(QStringLiteral("nativeAssetsPanel"));
    assetsPanel->setAccessibleName(tr("Native model assets"));
    assetsPanel->setRunner(&runner_);
    assetsPanel->setRequestedBackend(
        static_cast<mplayer::native::Backend>(nativeBackend_->currentData().toInt()));
    assetsPanel->setPreparationContextProvider([this] {
        return preparationContextProvider_
                   ? preparationContextProvider_()
                   : std::optional<mplayer::native::NativePreparationContext>{};
    });
    assetsPanel->setPreparationSourcePresence([this](QString* reasonKey, QString* reason) {
        if (!preparationSourcePresence_) {
            if (reasonKey != nullptr) *reasonKey = QStringLiteral("native.prepare.source");
            if (reason != nullptr) {
                *reason = tr("No source-presence path is wired, so there is no live video to "
                             "prepare for.");
            }
            return false;
        }
        return preparationSourcePresence_(reasonKey, reason);
    });
    assetsPanel->setPreparationContextRequest([this](quint64 generation) {
        if (preparationContextRequest_) preparationContextRequest_(generation);
    });
    assetsPanel->setPreparationFrameRequest([this](int frameIndex, quint64 generation) {
        if (preparationFrameRequest_) preparationFrameRequest_(frameIndex, generation);
    });
    assetsPanel_ = assetsPanel;
    connect(assetsPanel, &mikai::app::NativeAssetsPanel::preparationStarted, this,
            &SettingsDialog::nativePreparationStarted);
    connect(assetsPanel, &mikai::app::NativeAssetsPanel::preparationProgress, this,
            &SettingsDialog::nativePreparationProgress);
    connect(assetsPanel, &mikai::app::NativeAssetsPanel::preparationFinished, this,
            &SettingsDialog::nativePreparationFinished);
    assetsPanel_->setPreparationDesiredBackend([this]() {
        return nativePreparationDesiredBackend();
    });
    assetsPanel->setRequestedRecipe(nativeRecipe_->currentData().toString());
    connect(nativeRecipe_, &QComboBox::currentIndexChanged, this, [this, assetsPanel](int) {
        assetsPanel->setRequestedRecipe(nativeRecipe_->currentData().toString());
    });
    connect(assetsPanel, &mikai::app::NativeAssetsPanel::recipeRequested, this,
            &SettingsDialog::applyRequestedRecipe);
    connect(nativeBackend_, &QComboBox::currentIndexChanged, this, [this, assetsPanel](int) {
        assetsPanel->setRequestedBackend(
            static_cast<mplayer::native::Backend>(nativeBackend_->currentData().toInt()));
    });

    assetsCardLayout->addWidget(assetsPanel);
    root->addWidget(nativeAssetsCard_);

    // ── Card 4: RIFE Temporal Interpolation ──────────────────────────────────
    QVBoxLayout* rifeCardLayout = nullptr;
    nativeRifeCard_ = makeSettingsCard(page, &rifeCardLayout);
    rifeCardLayout->setSpacing(theme::kSpacingSmall);

    rifeHeading_ = new QLabel(tr("RIFE interpolation"), nativeRifeCard_);
    styleSectionHeading(rifeHeading_);
    rifeCardLayout->addWidget(rifeHeading_);

    nativeRife_ = new mikai::app::NativeRifeControls(nativeRifeCard_);
    nativeRife_->setObjectName(QStringLiteral("nativeRifePanel"));
    nativeRife_->setAccessibleName(tr("RIFE interpolation"));
    nativeRife_->setRequestedBackend(
        static_cast<mplayer::native::Backend>(nativeBackend_->currentData().toInt()));
    connect(assetsPanel, &mikai::app::NativeAssetsPanel::catalogRefreshed, this,
            &SettingsDialog::refreshRifeCatalog);
    nativeRife_->setCatalog(ownedModelCatalog());
    nativeRife_->loadStoredRecipe(nativePreferences_.recipeId);
    connect(nativeRife_, &mikai::app::NativeRifeControls::recipeRequested, this,
            &SettingsDialog::applyRequestedRecipe);
    connect(nativeBackend_, &QComboBox::currentIndexChanged, this, [this](int) {
        nativeRife_->setRequestedBackend(
            static_cast<mplayer::native::Backend>(nativeBackend_->currentData().toInt()));
    });
    rifeCardLayout->addWidget(nativeRife_);
    root->addWidget(nativeRifeCard_);

    const bool showPrivateWorkflow =
        runtimeChoice_->currentData().toInt() ==
            static_cast<int>(mplayer::native::RuntimeChoice::Native) ||
        mplayer::native::restartRequiredFor(mplayer::native::RuntimeChoice::Standard);
    animeJanaiManagerCard_->setVisible(!showPrivateWorkflow);
    for (QWidget* widget : std::initializer_list<QWidget*>{
             nativeConfigCard_, nativeAssetsCard_, nativeRifeCard_,
             nativeBackend_, nativeRecipe_, backendRowLabel_, recipeRowLabel_,
             nativeStatus_, nativeError_, assetsPanel, rifeHeading_, nativeRife_}) {
        if (widget != nullptr) widget->setVisible(showPrivateWorkflow);
    }
    if (!nativeLoadError.isEmpty()) {
        nativeError_->show();
    }
    connect(runtimeChoice_, &QComboBox::currentIndexChanged, this, [this](int) {
        refreshRuntimeHint();
        const bool showPrivateWorkflow =
            runtimeChoice_->currentData().toInt() ==
                static_cast<int>(mplayer::native::RuntimeChoice::Native) ||
            mplayer::native::restartRequiredFor(mplayer::native::RuntimeChoice::Standard);
        const bool assetsWereRefreshed =
            assetsPanel_ != nullptr && assetsPanel_->hasRefreshed();
        animeJanaiManagerCard_->setVisible(!showPrivateWorkflow);
        for (QWidget* widget : std::initializer_list<QWidget*>{
                 nativeConfigCard_, nativeAssetsCard_, nativeRifeCard_,
                 nativeBackend_, nativeRecipe_, backendRowLabel_, recipeRowLabel_,
                 nativeStatus_, nativeError_, assetsPanel_, rifeHeading_, nativeRife_}) {
            if (widget != nullptr) widget->setVisible(showPrivateWorkflow);
        }
        if (showPrivateWorkflow && assetsWereRefreshed) assetsPanel_->refresh();
        refreshNativeStatus();
    });

    root->addStretch();
    scroll->setWidget(page);
    processingScroll_ = scroll;
    return scroll;
}

void SettingsDialog::refreshNativeStatus() {
    if (nativeStatus_ == nullptr) {
        return;
    }
    const bool privateRuntimeSelected =
        runtimeChoice_ != nullptr &&
        (runtimeChoice_->currentData().toInt() ==
             static_cast<int>(mplayer::native::RuntimeChoice::Native) ||
         mplayer::native::restartRequiredFor(mplayer::native::RuntimeChoice::Standard));
    if (!privateRuntimeSelected) {
        nativeStatus_->clear();
        nativeStatus_->hide();
        return;
    }
    mplayer::native::NativePreferences request = nativePreferences_;
    request.backend =
        static_cast<mplayer::native::Backend>(nativeBackend_->currentData().toInt());
    request.recipeId = nativeRecipe_->currentData().toString();

    const mplayer::native::NativeSelectionView view =
        mplayer::native::evaluateNativeSelection(request, nativeAvailability_);
    if (view.requestedEnabled && !view.requestAvailable &&
        nativePreferences_.schemaRecognised) {
        nativeStatus_->setText(tr("Unavailable: %1").arg(view.availability.reason));
        nativeStatus_->show();
    } else {
        nativeStatus_->clear();
        nativeStatus_->hide();
    }
}

void SettingsDialog::refreshRuntimeStatus() {
    if (runtimeHint_ == nullptr) {
        return;
    }
    refreshRuntimeHint();
    const bool privateRuntimeSelected =
        runtimeChoice_ != nullptr &&
        (runtimeChoice_->currentData().toInt() ==
             static_cast<int>(mplayer::native::RuntimeChoice::Native) ||
         mplayer::native::restartRequiredFor(mplayer::native::RuntimeChoice::Standard));
    if (assetsPanel_ != nullptr && privateRuntimeSelected && !retranslating_) {
        assetsPanel_->refresh();
    }
}

mplayer::native::CatalogView SettingsDialog::ownedModelCatalog() const {
    mplayer::native::CatalogView catalog;
    if (assetsPanel_ != nullptr) {
        for (const mplayer::native::StoredModel& model : assetsPanel_->storedModels()) {
            catalog.models.append(model.record);
        }
    }
    return catalog;
}

void SettingsDialog::refreshRifeCatalog() {
    if (nativeRife_ == nullptr) {
        return;
    }
    nativeRife_->setCatalog(ownedModelCatalog());
}

mplayer::native::NativeAvailability SettingsDialog::currentSourceAvailability() const {
    std::optional<mplayer::native::NativePreparationContext> context;
    if (preparationContextProvider_) {
        context = preparationContextProvider_();
    }
    const mplayer::native::NativeFactoryObservation* factory =
        (context.has_value() && context->factory.has_value()) ? &*context->factory : nullptr;
    return mplayer::native::currentNativeAvailability(factory);
}

void SettingsDialog::refreshRuntimeHint() {
    if (runtimeHint_ == nullptr || runtimeChoice_ == nullptr) {
        return;
    }
    const auto chosen =
        static_cast<mplayer::native::RuntimeChoice>(runtimeChoice_->currentData().toInt());
    const bool restartRequired = mplayer::native::restartRequiredFor(chosen);
    runtimeHint_->setVisible(restartRequired);
    if (restartRequired) {
        runtimeHint_->setText(
            tr("Runtime will change to %1 after restarting Mikai.")
                .arg(runtimeChoice_->currentText()));
        return;
    }
    runtimeHint_->clear();
}

void SettingsDialog::populateRecipeChoices(const QString& selection) {
    if (nativeRecipe_ == nullptr) {
        return;
    }
    QSignalBlocker blocker(nativeRecipe_);
    nativeRecipe_->clear();
    nativeRecipe_->addItem(tr("None (original pipeline)"), QString());
    bool offered = false;
    for (const mplayer::native::NativeRecipeOption& option : nativeAvailability_.recipes) {
        nativeRecipe_->addItem(option.label.isEmpty() ? option.id : option.label, option.id);
        if (option.id == selection) {
            offered = true;
        }
    }
    if (!selection.isEmpty() && !offered) {
        unavailableRecipeId_ = selection;
        nativeRecipe_->addItem(unavailableRecipeCaption(selection), selection);
        offered = true;
    } else {
        unavailableRecipeId_.clear();
    }
    if (nativeRecipe_->count() == 1 && !offered) {
        nativeRecipe_->setEnabled(false);
        nativeRecipe_->setToolTip(tr("No native recipe is available in this build."));
    } else {
        nativeRecipe_->setEnabled(nativePreferences_.schemaRecognised);
    }
    const int index = nativeRecipe_->findData(selection);
    nativeRecipe_->setCurrentIndex(index >= 0 ? index : 0);
    if (assetsPanel_ != nullptr) {
        assetsPanel_->setRequestedRecipe(nativeRecipe_->currentData().toString());
    }
}

void SettingsDialog::applyRequestedRecipe(const QString& recipeId) {
    nativeAvailability_ = currentSourceAvailability();
    populateRecipeChoices(recipeId);
    refreshNativeStatus();
    if (nativeRife_ != nullptr) {
        nativeRife_->loadStoredRecipe(recipeId);
    }
}

void SettingsDialog::retranslateProcessingTab() {
    nativeHeading_->setText(tr("Processing runtime"));
    nativeDescription_->setText(
        tr("Standard manages official AnimeJaNai models and profiles. "
           "Select Native to configure the separate private Native workflow."));
    backendRowLabel_->setText(tr("Backend"));
    recipeRowLabel_->setText(tr("Recipe"));
    runtimeModuleRowLabel_->setText(tr("Runtime module"));

    nativeBackend_->setAccessibleName(tr("Native backend"));
    nativeRecipe_->setAccessibleName(tr("Native recipe"));
    if (!unavailableRecipeId_.isEmpty()) {
        const int index = nativeRecipe_->findData(unavailableRecipeId_);
        if (index >= 0) {
            nativeRecipe_->setItemText(index, unavailableRecipeCaption(unavailableRecipeId_));
        }
    }
    const int noneIndex = nativeRecipe_->findData(QString());
    if (noneIndex >= 0) {
        nativeRecipe_->setItemText(noneIndex, tr("None (original pipeline)"));
    }
    if (!nativeRecipe_->isEnabled()) {
        nativeRecipe_->setToolTip(tr("No native recipe is available in this build."));
    }
    nativeStatus_->setAccessibleName(tr("Native processing availability"));
    const int standardIndex =
        runtimeChoice_->findData(static_cast<int>(mplayer::native::RuntimeChoice::Standard));
    const int nativeIndex =
        runtimeChoice_->findData(static_cast<int>(mplayer::native::RuntimeChoice::Native));
    if (standardIndex >= 0) runtimeChoice_->setItemText(standardIndex, tr("Standard"));
    if (nativeIndex >= 0) runtimeChoice_->setItemText(nativeIndex, tr("Native"));
    runtimeChoice_->setAccessibleName(tr("mpv runtime"));

    rifeHeading_->setText(tr("RIFE interpolation"));
    if (animeJanaiPanel_ != nullptr) {
        animeJanaiPanel_->setAccessibleName(tr("AnimeJaNai model and profile manager"));
        animeJanaiPanel_->retranslateUi();
    }
    if (assetsPanel_ != nullptr) {
        assetsPanel_->setAccessibleName(tr("Native model assets"));
    }
    if (nativeRife_ != nullptr) {
        nativeRife_->setAccessibleName(tr("RIFE interpolation"));
    }

    refreshNativeStatus();
    refreshRuntimeStatus();
}

mplayer::native::Backend SettingsDialog::nativePreparationDesiredBackend() const {
    if (preparationDesiredBackendProvider_) {
        return preparationDesiredBackendProvider_();
    }
    if (nativeBackend_ != nullptr) {
        return static_cast<mplayer::native::Backend>(nativeBackend_->currentData().toInt());
    }
    return mplayer::native::loadNativePreferences().backend;
}

void SettingsDialog::setNativePreparationDesiredBackend(
    std::function<mplayer::native::Backend()> provider) {
    preparationDesiredBackendProvider_ = std::move(provider);
}

}  // namespace mikai::app
