#include "dialogs/SettingsDialog.hpp"

#include "I18n.hpp"
#include "dialogs/AnimeJanaiManagerPanel.hpp"
#include "dialogs/settings/SettingsHelpers.hpp"
#include "dialogs/settings/SettingsUi.hpp"
#include "state/Feeds.hpp"
#include "theme/Colors.hpp"
#include "theme/Theme.hpp"
#include "theme/Icons.hpp"
#include "theme/Metrics.hpp"
#include "workers/Runner.hpp"

#include <backend/MikaiClient.hpp>
#include <dialogs/ComponentStoreTab.hpp>
#include <dialogs/NativeAssetsPanel.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>
#include <native/status/NativeStatus.hpp>
#include <player/shell/ShellTheme.hpp>
#include <player/shaders/Anime4K.hpp>
#include <player/shaders/PresetShortcuts.hpp>

#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDebug>
#include <QDialogButtonBox>
#include <QEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <VideoSurface.hpp>

#include <QSettings>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace mikai::app {

SettingsDialog::SettingsDialog(backend::MikaiClient& client, Runner& runner, QWidget* parent,
                                 ComponentStoreTab* adoptedComponents)
    : QDialog(parent), client_(client), runner_(runner), adoptedComponents_(adoptedComponents) {
    setWindowTitle(tr("Settings"));
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    setSizeGripEnabled(true);
    QScreen* targetScreen = parent != nullptr ? parent->screen() : QGuiApplication::primaryScreen();
    QSize initialSize(theme::kSettingsDialogDefaultWidth, theme::kSettingsDialogDefaultHeight);
    int minimumWidth = theme::kSettingsDialogMinWidth;
    int minimumHeight = theme::kSettingsDialogMinHeight;
    if (targetScreen != nullptr) {
        const QSize available = targetScreen->availableGeometry().size();
        // The tab pages scroll, so a work area narrower or shorter than the
        // preferred floors shrinks the dialog instead of pushing the fixed
        // Save/Cancel dock outside the screen.
        minimumWidth = qMin(minimumWidth, available.width());
        minimumHeight = qMin(minimumHeight, available.height());
        initialSize = initialSize.boundedTo(available);
    }
    setMinimumSize(minimumWidth, minimumHeight);
    resize(initialSize);

    // What the running application is already speaking, kept apart from the
    // preference that produced it. Preview installs a choice without writing the
    // store, so closing the dialog has to restore this exact language while the
    // stored preference stays exactly as it was.
    openingLanguageCode_ = activeInterfaceLanguage();
    openingLanguagePreference_ = languagePreference();
    previewLanguagePreference_ = openingLanguagePreference_;
    nativePreferences_ = mplayer::native::loadNativePreferences();
    // Cold: no core and no observation exists while the dialog is being built, so
    // the availability starts from the no-factory default. The application's
    // context provider, installed later, supplies the live core's observation.
    nativeAvailability_ = mplayer::native::currentNativeAvailability(nullptr);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(theme::kSpacingLarge, theme::kSpacingMedium, theme::kSpacingLarge,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    openingAppearance_ = currentAppearance();
    appearanceDraft_ = openingAppearance_;
    styledAppearance_ = openingAppearance_;
    appearanceStyleTimer_ = new QTimer(this);
    appearanceStyleTimer_->setSingleShot(true);
    appearanceStyleTimer_->setInterval(60);
    connect(appearanceStyleTimer_, &QTimer::timeout, this,
            &SettingsDialog::applyAppearanceStyles);
    generalTab_ = buildGeneralTab();
    presetsTab_ = buildPresetsTab();
    processingTab_ = buildProcessingTab();
    componentsTab_ = buildComponentsTab();
    benchmarkTab_ = buildBenchmarkTab();
    customizationGeneralPage_ = buildCustomizationGeneralTab();
    appearancePage_ = buildAppearanceTab();
    refreshAppearanceControls();
    buildNavigation();
    if (!presetError_->isHidden()) {
        showPage(QStringLiteral("shortcuts"));
    }
    auto* footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("settingsFooter"));
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(0, theme::kSpacingSmall, 0, 0);
    footerLayout->setSpacing(theme::kSpacingSmall);
    buttonBox_ = new QDialogButtonBox(footer);
    cancelButton_ = buttonBox_->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    cancelButton_->setMinimumHeight(theme::kSettingsControlSize);
    saveButton_ = buttonBox_->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    saveButton_->setObjectName(QStringLiteral("primary"));
    saveButton_->setProperty("primaryAction", true);
    saveButton_->setMinimumHeight(theme::kSettingsControlSize);
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
    connect(saveButton_, &QPushButton::clicked, this, &SettingsDialog::save);
    footerLayout->addWidget(buttonBox_);
    root->addWidget(footer);

    quotaTimer_ = new QTimer(this);
    quotaTimer_->setInterval(1000);
    connect(quotaTimer_, &QTimer::timeout, this, &SettingsDialog::refreshQuota);
    quotaTimer_->start();
    refreshQuota();

    // The dialog paints the settings sheet itself, so a preview of a new theme,
    // accent or radius re-styles every page from one place. Applied once the
    // whole tree exists, footer included.
    setStyleSheet(settingsStylesheet(appearanceDraft_));
}

SettingsDialog::~SettingsDialog() {
    // A dialog destroyed without done() - a parent going away, a stack frame
    // unwinding - still owes the user the language and the look they opened in.
    restoreOpeningLanguage();
    restoreOpeningAppearance();
    // Before QObject deletes children. An adopted tab is the app's, so closing
    // Settings must not destroy it or cancel the download it is running.
    releaseAdoptedComponentsTab();
    if (benchmarkCancellation_) benchmarkCancellation_->store(true);
}

void SettingsDialog::done(int result) {
    if (benchmarkCancellation_) benchmarkCancellation_->store(true);
    // An accepted result is only produced by save(), which has already written
    // and committed the choice; every other close rolls the preview back.
    if (result != QDialog::Accepted) {
        restoreOpeningLanguage();
        restoreOpeningAppearance();
    }
    roundDone_ = true;
    QDialog::done(result);
}

void SettingsDialog::releaseAdoptedComponentsTab() {
    if (!adoptedComponentsTab_ || componentStoreTab_ == nullptr) return;
    adoptedComponentsTab_ = false;
    // Process shutdown destroys this dialog as a child. Leaving the tab where
    // it is cancels with the process; reparenting onto a window that is already
    // going away is not safe.
    if (QCoreApplication::closingDown() || adoptedComponentsHome_ == nullptr) {
        adoptedComponentsHome_ = nullptr;
        return;
    }
    ComponentStoreTab* tab = componentStoreTab_;
    componentStoreTab_ = nullptr;
    tab->hide();
    if (QWidget* parent = tab->parentWidget()) {
        if (QLayout* layout = parent->layout()) layout->removeWidget(tab);
    }
    tab->setParent(adoptedComponentsHome_);
    adoptedComponentsHome_ = nullptr;
}

void SettingsDialog::setComponentProvisioner(NativeComponentProvisioner* provisioner) {
    // The Components tab owns every download, so the provisioner lands there.
    // The assets panel shows a pointer row instead and takes a tab-switch
    // callback (wired in buildProcessingTab).
    componentProvisioner_ = provisioner;
    if (componentStoreTab_ != nullptr) componentStoreTab_->setComponentProvisioner(provisioner);
}

void SettingsDialog::showComponentsTab() { showPage(QStringLiteral("components")); }

void SettingsDialog::installRecommended() {
    if (componentStoreTab_ != nullptr) componentStoreTab_->installRecommended();
}

bool SettingsDialog::cancelNativePreparation() {
    if (assetsPanel_ == nullptr || !assetsPanel_->prepareInFlight()) return false;
    assetsPanel_->cancelPrepare();
    return true;
}

void SettingsDialog::setNativePreparationContextProvider(NativePreparationContextProvider provider) {
    // The assets panel reads this live, so installing it after construction (the
    // application's own wiring point) still reaches the preparation path.
    preparationContextProvider_ = std::move(provider);
    if (assetsPanel_ != nullptr) assetsPanel_->setPreparationContextProvider([this] {
        return preparationContextProvider_
                   ? preparationContextProvider_()
                   : std::optional<mplayer::native::NativePreparationContext>{};
    });
    // A provider installed after construction may already carry the live core's
    // observation, so the availability is re-derived from it rather than left at
    // the cold default. Reading it probes nothing; the selection is preserved
    // because this is not an edit.
    nativeAvailability_ = currentSourceAvailability();
    populateRecipeChoices(nativeRecipe_ != nullptr ? nativeRecipe_->currentData().toString()
                                                   : nativePreferences_.recipeId);
    refreshNativeStatus();
}

void SettingsDialog::setNativePreparationSourcePresence(NativePreparationSourcePresence presence) {
    preparationSourcePresence_ = std::move(presence);
    if (assetsPanel_ != nullptr) {
        assetsPanel_->setPreparationSourcePresence([this](QString* reasonKey, QString* reason) {
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
    }
}

void SettingsDialog::setNativePreparationContextRequest(NativePreparationContextRequest request) {
    preparationContextRequest_ = std::move(request);
    if (assetsPanel_ != nullptr) {
        assetsPanel_->setPreparationContextRequest([this](quint64 generation) {
            if (preparationContextRequest_) preparationContextRequest_(generation);
        });
    }
}

void SettingsDialog::setNativePreparationFrameRequest(NativePreparationFrameRequest request) {
    preparationFrameRequest_ = std::move(request);
    if (assetsPanel_ != nullptr) {
        assetsPanel_->setPreparationFrameRequest([this](int frameIndex, quint64 generation) {
            if (preparationFrameRequest_) preparationFrameRequest_(frameIndex, generation);
        });
    }
}

void SettingsDialog::deliverNativePreparationContext(
    const mplayer::native::NativePreparationContext& context) {
    if (assetsPanel_ != nullptr) assetsPanel_->deliverPreparationContext(context);
    // The delivered context is the producer's own observation for the live
    // source, so the eligibility is re-derived from its factory. The stored
    // request and the selected backend/recipe are left as the user set them:
    // this is an observation, not an edit.
    const mplayer::native::NativeFactoryObservation* factory =
        context.factory.has_value() ? &*context.factory : nullptr;
    nativeAvailability_ = mplayer::native::currentNativeAvailability(factory);
    populateRecipeChoices(nativeRecipe_ != nullptr ? nativeRecipe_->currentData().toString()
                                                   : nativePreferences_.recipeId);
    refreshNativeStatus();
}

void SettingsDialog::failNativePreparationContext(const QString& reasonKey, const QString& reason) {
    if (assetsPanel_ != nullptr) assetsPanel_->failPreparationContext(reasonKey, reason);
}

void SettingsDialog::deliverNativePreparationFrame(int frameIndex,
                                                   const mplayer::native::FrameFixture& frame,
                                                   quint64 sourceEpoch, double observedPosition) {
    if (assetsPanel_ != nullptr) {
        assetsPanel_->deliverPreparationFrame(frameIndex, frame, sourceEpoch, observedPosition);
    }
}

void SettingsDialog::failNativePreparationFrame(int frameIndex, const QString& reasonKey,
                                                const QString& reason) {
    if (assetsPanel_ != nullptr) assetsPanel_->failPreparationFrame(frameIndex, reasonKey, reason);
}

void SettingsDialog::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void SettingsDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // Only a finished dialog that is shown again needs a baseline: a plain
    // hide()/show() of the same round must keep the preview it is showing. What
    // a later Cancel restores is the language live now, and the selector goes
    // back to the preference in the store, which the last round may have
    // committed.
    if (!roundDone_) return;
    roundDone_ = false;
    refreshSystemIntegrationControls();
    openingAppearance_ = currentAppearance();
    appearanceDraft_ = openingAppearance_;
    appearanceCommitted_ = false;
    refreshAppearanceControls();
    openingLanguageCode_ = activeInterfaceLanguage();
    openingLanguagePreference_ = languagePreference();
    previewLanguagePreference_ = openingLanguagePreference_;
    languageCommitted_ = false;
    if (language_ != nullptr) {
        const int index = language_->findData(openingLanguagePreference_);
        if (index >= 0 && index != language_->currentIndex()) {
            QSignalBlocker blocker(language_);
            language_->setCurrentIndex(index);
        }
    }
}

void SettingsDialog::previewInterfaceLanguage(int index) {
    if (!languageSelectorReady_ || retranslating_ || language_ == nullptr || index < 0) {
        return;
    }
    const QString preference = language_->itemData(index).toString();
    if (preference == previewLanguagePreference_) return;
    if (applyInterfaceLanguage(preference)) {
        // Only the live translator changed; the preference reaches the store
        // through Save and through nothing else.
        previewLanguagePreference_ = preference;
        return;
    }
    // The catalog was refused, so the working language is untouched. Leaving the
    // selector on the refused choice would let Save claim a language this
    // process never installed, so the field goes back to the installed one and
    // names the failure where the control is.
    const int previous = language_->findData(previewLanguagePreference_);
    if (previous >= 0 && previous != language_->currentIndex()) {
        QSignalBlocker blocker(language_);
        language_->setCurrentIndex(previous);
    }
    showGeneralError(
        tr("The %1 interface could not be applied, so the language was left unchanged.")
            .arg(resolveLanguageCode(preference, QLocale::system().language())));
}

void SettingsDialog::restoreOpeningLanguage() {
    if (languageCommitted_) return;
    languageCommitted_ = true;
    previewLanguagePreference_ = openingLanguagePreference_;
    if (language_ != nullptr) {
        const int index = language_->findData(openingLanguagePreference_);
        if (index >= 0 && index != language_->currentIndex()) {
            QSignalBlocker blocker(language_);
            language_->setCurrentIndex(index);
        }
    }
    // Shutdown has no windows left to retranslate, and no application to hold
    // the translators the restore would install.
    if (QCoreApplication::instance() == nullptr || QCoreApplication::closingDown()) return;
    if (activeInterfaceLanguage() == openingLanguageCode_) return;
    if (!applyInterfaceLanguage(openingLanguageCode_)) {
        qWarning().noquote() << "mikai: could not restore the opening interface language"
                             << openingLanguageCode_;
    }
}

void SettingsDialog::retranslateUi() {
    retranslating_ = true;
    setWindowTitle(tr("Settings"));
    retranslateNavigation();
    cancelButton_->setText(tr("Cancel"));
    saveButton_->setText(tr("Save"));

    retranslateGeneralTab();
    retranslateCustomizationTabs();
    retranslatePresetsTab();
    retranslateProcessingTab();
    retranslateBenchmarkTab();
    retranslating_ = false;
}

void SettingsDialog::showPresetError(const QString& message) {
    presetError_->setText(message);
    presetError_->show();
    showPage(QStringLiteral("shortcuts"));
}

void SettingsDialog::showGeneralError(const QString& message) {
    status_->setText(message);
    status_->show();
    showPage(QStringLiteral("access"));
    QAccessibleEvent alert(status_, QAccessible::Alert);
    QAccessible::updateAccessibility(&alert);
}

void SettingsDialog::showProcessingTab() { showPage(QStringLiteral("processing")); }

void SettingsDialog::focusNativeProviderChoice() {
    showProcessingTab();
    if (nativeBackend_ != nullptr && nativeBackend_->isVisible()) {
        nativeBackend_->setFocus();
    } else if (runtimeChoice_ != nullptr) {
        runtimeChoice_->setFocus();
    }
}

void SettingsDialog::triggerNativePrepare() {
    showProcessingTab();
    if (assetsPanel_ == nullptr) return;
    if (!assetsPanel_->isVisible()) {
        if (runtimeChoice_ != nullptr) runtimeChoice_->setFocus();
        return;
    }
    assetsPanel_->prepareArtifact();
}

void SettingsDialog::applyProcessingIntent(mplayer::shell::ProcessingSettingsIntent intent) {
    switch (intent) {
        case mplayer::shell::ProcessingSettingsIntent::Prepare:
            triggerNativePrepare();
            return;
        case mplayer::shell::ProcessingSettingsIntent::Providers:
            focusNativeProviderChoice();
            return;
    }
}

void SettingsDialog::showNativeError(const QString& message) {
    nativeError_->setText(message);
    nativeError_->show();
    showPage(QStringLiteral("processing"));
}

void SettingsDialog::save() {
    systemIntegrationError_->clear();
    systemIntegrationError_->hide();
    QMap<QString, QString> presets;
    const QString presetProblem = collectPresetShortcuts(&presets);
    if (!presetProblem.isEmpty()) {
        showPresetError(presetProblem);
        return;
    }
    presetError_->clear();
    presetError_->hide();
    nativeError_->clear();
    nativeError_->hide();

    const QString candidate = token_->text().trimmed();
    if (candidate != client_.apiKey()) {
        const backend::ApiKeySaveResult saveResult = client_.setApiKey(candidate);
        if (!saveResult.ok) {
            showGeneralError(tr("The API key could not be saved, so it was left unchanged: %1")
                                 .arg(saveResult.reason.isEmpty()
                                          ? tr("the key file could not be written")
                                          : saveResult.reason));
            return;
        }
    }
    QSet<QString> chosen;
    for (auto it = feedBoxes_.constBegin(); it != feedBoxes_.constEnd(); ++it) {
        if (it.value()->isChecked()) {
            chosen.insert(it.key());
        }
    }

    if (animeJanaiPanel_ != nullptr && animeJanaiPanel_->isDirty()) {
        QString ajiReason;
        if (!animeJanaiPanel_->saveConfig(&ajiReason)) {
            showNativeError(ajiReason.isEmpty()
                                ? tr("The AnimeJaNai configuration could not be saved.")
                                : ajiReason);
            if (processingScroll_ != nullptr) {
                processingScroll_->ensureWidgetVisible(nativeError_);
            }
            return;
        }
        refreshBenchmarkCatalogue();
    }

    mplayer::native::NativePreferencePatch patch;
    const mplayer::native::Backend chosenBackend =
        static_cast<mplayer::native::Backend>(nativeBackend_->currentData().toInt());
    if (chosenBackend != nativePreferences_.backend) {
        patch.backend = chosenBackend;
    }
    const QString chosenRecipe = nativeRecipe_->currentData().toString();
    if (chosenRecipe != nativePreferences_.recipeId) {
        patch.recipeId = chosenRecipe;
    }
    if (!patch.isEmpty()) {
        QString nativeReason;
        if (!mplayer::native::saveNativePreferences(patch, &nativeReason)) {
            showNativeError(nativeReason.isEmpty()
                                ? tr("The native preferences could not be saved.")
                                : nativeReason);
            return;
        }
    }

    const auto runtimeChoice =
        static_cast<mplayer::native::RuntimeChoice>(runtimeChoice_->currentData().toInt());
    if (runtimeChoice != mplayer::native::storedRuntimeChoice()) {
        QString runtimeReason;
        if (!mplayer::native::storeRuntimeChoice(runtimeChoice, &runtimeReason)) {
            showNativeError(runtimeReason.isEmpty()
                                ? tr("The runtime choice could not be saved.")
                                : runtimeReason);
            return;
        }
    }

    const QString chosenLanguage = language_->currentData().toString();
    const QString chosenCode = resolveLanguageCode(chosenLanguage, QLocale::system().language());
    // A preview normally installed this very language already; loading the same
    // catalog again would only churn translators. Comparing the resolved codes
    // also keeps a process with no manager - a probe that never applied a
    // language - from being forced to load a catalog merely to save an unrelated
    // preference, which is what the dialog did before preview existed.
    if (chosenCode != activeInterfaceLanguage() && !applyInterfaceLanguage(chosenLanguage)) {
        // A preview has already installed the working choice, so a refusal here
        // means the live language changed behind the dialog. Put the selector on
        // the preference that is actually installed; nothing has been written.
        const int previous = language_->findData(previewLanguagePreference_);
        if (previous >= 0 && previous != language_->currentIndex()) {
            QSignalBlocker blocker(language_);
            language_->setCurrentIndex(previous);
        }
        showGeneralError(
            tr("The %1 interface could not be applied, so the language was left unchanged.")
                .arg(chosenCode));
        return;
    }
    previewLanguagePreference_ = chosenLanguage;

    // OS integrations are the final fallible writes. Their own rollback does
    // not undo independent stores already committed above.
    if (!saveSystemIntegrations()) {
        showSystemIntegrationError(
            systemIntegrationError_->text() +
            tr("\nAny other settings already saved remain saved, even if you cancel. "
               "Unsaved language and appearance previews will still be restored."));
        return;
    }

    // Every fallible write has succeeded, so the remaining preferences can now
    // be committed. Cancel rolls back previews, not previously saved stores.
    applyAppearancePreview();
    appearanceStyleTimer_->stop();
    applyAppearanceStyles();
    saveAppearance(appearanceDraft_);
    mplayer::shell::setAccent(theme::accentColor(appearanceDraft_));
    mplayer::shell::setCornerRadius(theme::radiusSmall(appearanceDraft_));
    appearanceCommitted_ = true;
    QSettings settings;
    settings.setValue(QString::fromLatin1(kUiLanguageSettingKey), chosenLanguage);
    settings.setValue(feeds::settingsKey(), feeds::serialize(chosen));
    // T-101/T-104: the HDR tone-mapping policy and the native-on-HDR override
    // commit with the rest: the statics are the live truth every engine reads
    // at creation and every running surface adopts through its own pump, so no
    // player restart and no per-surface plumbing is needed here.
    const int committedHdrPolicy = hdrPolicy_->currentData().toInt();
    settings.setValue(QString::fromLatin1(mplayer::VideoSurface::kHdrToneMappingSettingKey),
                     committedHdrPolicy);
    settings.setValue(
        QString::fromLatin1(mplayer::VideoSurface::kAnimeJanaiHdrOverrideSettingKey),
        nativeHdrOverride_->isChecked());
    using HdrPolicy = mplayer::VideoSurface::HdrToneMappingPolicy;
    switch (committedHdrPolicy) {
        case 1: mplayer::VideoSurface::setHdrToneMappingPolicy(HdrPolicy::Stable); break;
        case 2: mplayer::VideoSurface::setHdrToneMappingPolicy(HdrPolicy::Vivid); break;
        default: mplayer::VideoSurface::setHdrToneMappingPolicy(HdrPolicy::Auto); break;
    }
    mplayer::VideoSurface::setAnimeJanaiHdrOverride(nativeHdrOverride_->isChecked());
    settings.beginGroup(QLatin1String(mplayer::shell::kPresetShortcutSettingsGroup));
    for (const PresetRow& row : presetRows_) {
        if (presets.contains(row.id)) {
            settings.setValue(row.id, presets.value(row.id));
        } else {
            settings.remove(row.id);
        }
    }
    settings.endGroup();

    // From here the choice is the committed one: closing must not undo it.
    languageCommitted_ = true;
    accept();
}

}  // namespace mikai::app
