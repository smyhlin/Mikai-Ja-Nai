#include "dialogs/SettingsDialog.hpp"

#include "I18n.hpp"
#include "dialogs/settings/SettingsHelpers.hpp"
#include "dialogs/settings/SettingsUi.hpp"
#include "state/Feeds.hpp"
#include "theme/Colors.hpp"
#include "theme/Icons.hpp"
#include "theme/Metrics.hpp"
#include "workers/Runner.hpp"

#include <backend/MikaiClient.hpp>
#include <dialogs/ComponentStoreTab.hpp>
#include <dialogs/NativeAssetsPanel.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>
#include <native/status/NativeStatus.hpp>
#include <player/Anime4K.hpp>
#include <player/PresetShortcuts.hpp>

#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QSettings>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace mikai::app {

SettingsDialog::SettingsDialog(backend::MikaiClient& client, Runner& runner, QWidget* parent,
                                 ComponentStoreTab* adoptedComponents)
    : QDialog(parent), client_(client), runner_(runner), adoptedComponents_(adoptedComponents) {
    setWindowTitle(tr("Settings"));
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

    // What the running application is already speaking. Save compares the choice
    // against this rather than against the stored preference, because the
    // question is whether the translator has to be reloaded, not whether the key
    // would change.
    appliedLanguagePreference_ = activeInterfaceLanguage();
    languagePreferenceWasStored_ =
        QSettings().contains(QString::fromLatin1(kUiLanguageSettingKey));
    storedLanguagePreference_ = languagePreference();
    nativePreferences_ = mplayer::native::loadNativePreferences();
    // Cold: no core and no observation exists while the dialog is being built, so
    // the availability starts from the no-factory default. The application's
    // context provider, installed later, supplies the live core's observation.
    nativeAvailability_ = mplayer::native::currentNativeAvailability(nullptr);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(theme::kSpacingLarge, theme::kSpacingMedium, theme::kSpacingLarge,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("settingsTabs"));
    tabs_->setDocumentMode(true);
    // Every tab must sit wholly inside the tab bar at the minimum supported
    // width: a tab the bar cannot show is an unreachable action, and QTabBar
    // answers an over-wide row with scroll arrows rather than by eliding the
    // caption. The captions are translated, so the padding is the part that
    // yields - the glyph and the whole caption stay. kSpacingSmall is the tab
    // padding at this width rather than the wider kSpacingLarge the bar carries
    // by default.
    tabs_->setStyleSheet(
        QStringLiteral("QTabBar::tab { padding: 7px %1px; }").arg(theme::kSpacingSmall));
    generalTab_ = buildGeneralTab();
    presetsTab_ = buildPresetsTab();
    processingTab_ = buildProcessingTab();
    componentsTab_ = buildComponentsTab();
    benchmarkTab_ = buildBenchmarkTab();
    tabs_->addTab(generalTab_, tr("General"));
    tabs_->addTab(presetsTab_, tr("Presets"));
    tabs_->addTab(processingTab_, tr("Processing"));
    tabs_->addTab(componentsTab_, tr("Components"));
    tabs_->addTab(benchmarkTab_, tr("Benchmark"));
    if (!presetError_->isHidden()) {
        tabs_->setCurrentWidget(presetsTab_);
    }
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        QWidget* current = tabs_->widget(index);
        if (current == componentsTab_) emit componentsTabOpened(this);
        if (current == benchmarkTab_) refreshBenchmarkCatalogue();
    });
    root->addWidget(tabs_, 1);

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
}

SettingsDialog::~SettingsDialog() {
    // Before QObject deletes children. An adopted tab is the app's, so closing
    // Settings must not destroy it or cancel the download it is running.
    releaseAdoptedComponentsTab();
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

void SettingsDialog::showComponentsTab() {
    if (tabs_ != nullptr && componentsTab_ != nullptr) tabs_->setCurrentWidget(componentsTab_);
}

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

void SettingsDialog::retranslateUi() {
    retranslating_ = true;
    setWindowTitle(tr("Settings"));
    for (int index = 0; index < tabs_->count(); ++index) {
        QWidget* page = tabs_->widget(index);
        if (page == generalTab_) {
            tabs_->setTabText(index, tr("General"));
        } else if (page == presetsTab_) {
            tabs_->setTabText(index, tr("Presets"));
        } else if (page == processingTab_) {
            tabs_->setTabText(index, tr("Processing"));
        } else if (page == componentsTab_) {
            tabs_->setTabText(index, tr("Components"));
        } else if (page == benchmarkTab_) {
            tabs_->setTabText(index, tr("Benchmark"));
        }
    }
    cancelButton_->setText(tr("Cancel"));
    saveButton_->setText(tr("Save"));

    retranslateGeneralTab();
    retranslatePresetsTab();
    retranslateProcessingTab();
    retranslateBenchmarkTab();
    retranslating_ = false;
}

void SettingsDialog::showPresetError(const QString& message) {
    presetError_->setText(message);
    presetError_->show();
    tabs_->setCurrentIndex(1);
}

void SettingsDialog::showGeneralError(const QString& message) {
    status_->setText(message);
    status_->show();
    tabs_->setCurrentIndex(0);
    QAccessibleEvent alert(status_, QAccessible::Alert);
    QAccessible::updateAccessibility(&alert);
}

void SettingsDialog::showProcessingTab() {
    if (tabs_ != nullptr) {
        tabs_->setCurrentWidget(processingTab_);
    }
}

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
    tabs_->setCurrentIndex(2);
}

void SettingsDialog::save() {
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
    QSettings settings;
    const QString chosenLanguage = language_->currentData().toString();
    settings.setValue(QString::fromLatin1(kUiLanguageSettingKey), chosenLanguage);
    settings.setValue(feeds::settingsKey(), feeds::serialize(chosen));
    settings.beginGroup(QLatin1String(mplayer::shell::kPresetShortcutSettingsGroup));
    for (const PresetRow& row : presetRows_) {
        if (presets.contains(row.id)) {
            settings.setValue(row.id, presets.value(row.id));
        } else {
            settings.remove(row.id);
        }
    }
    settings.endGroup();

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

    if (chosenLanguage != appliedLanguagePreference_ &&
        !applyInterfaceLanguage(chosenLanguage)) {
        if (languagePreferenceWasStored_) {
            settings.setValue(QString::fromLatin1(kUiLanguageSettingKey),
                              storedLanguagePreference_);
        } else {
            settings.remove(QString::fromLatin1(kUiLanguageSettingKey));
        }
        showGeneralError(
            tr("The %1 interface could not be applied, so the language was left unchanged.")
                .arg(resolveLanguageCode(chosenLanguage, QLocale::system().language())));
        return;
    }
    appliedLanguagePreference_ = chosenLanguage;
    storedLanguagePreference_ = chosenLanguage;

    accept();
}

}  // namespace mikai::app
