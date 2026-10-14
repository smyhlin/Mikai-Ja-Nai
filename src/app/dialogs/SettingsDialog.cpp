#include "dialogs/SettingsDialog.hpp"

#include "I18n.hpp"
#include "state/Feeds.hpp"
#include "theme/Colors.hpp"
#include "theme/Icons.hpp"
#include "theme/Metrics.hpp"
#include "workers/Runner.hpp"

#include <initializer_list>

#include <backend/MikaiClient.hpp>
#include <dialogs/ComponentStoreTab.hpp>
#include <dialogs/NativeAssetsPanel.hpp>
#include <dialogs/NativeRifeControls.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>
#include <native/status/NativeStatus.hpp>
#include <player/Anime4K.hpp>
#include <player/PresetShortcuts.hpp>
#include <player/ProcessingSettingsIntent.hpp>

#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHash>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QPointer>
#include <QScrollArea>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace mikai::app {
namespace {

/// Backend names are proper nouns and are never translated; the stable name is
/// what a stored document carries, and this is only what the combo shows.
QString backendLabel(mplayer::native::Backend backend) {
    switch (backend) {
        case mplayer::native::Backend::TensorRT: return QStringLiteral("TensorRT");
        case mplayer::native::Backend::DirectML: return QStringLiteral("DirectML");
    }
    return QString();
}

/// Rows follow the preset menu categories; experiment labels retain their family.
QString shortcutGroupHeading(const QString& categoryId) {
    const QString label = mplayer::anime4k::categoryLabel(categoryId);
    return label.isEmpty() ? QString() : label;
}

/// A caption that must never set its page's width floor. The styled heading and
/// section-heading fonts are wide and a translated caption is wider still: left
/// to its own size hint a caption votes its whole advance into the page, and a
/// page wider than its viewport can only be read by panning it sideways. Under
/// an Ignored horizontal policy the caption yields and wraps instead. A caption
/// across a row's cross axis is already stretched to the page width and so loses
/// nothing; one across the main axis is handed exactly the space left beside its
/// glyph. This is the same yield the recipe combo and the wrapped hints already
/// declare, applied to the captions that were still voting a width.
void yieldCaptionWidth(QLabel* label) {
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Ignored, label->sizePolicy().verticalPolicy());
}

/// Creates a styled surface card with standard internal margins and spacing.
QFrame* makeSettingsCard(QWidget* parent, QVBoxLayout** layoutOut) {
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("settingsCard"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium,
                               theme::kSpacingMedium, theme::kSpacingMedium);
    layout->setSpacing(theme::kSpacingSmall);
    if (layoutOut != nullptr) *layoutOut = layout;
    return card;
}

/// The widest thing a one-key field can show: a full modifier stack in front of
/// a function key. The outer QKeySequenceEdit carries neither the styled font
/// nor the trailing clear action, so both are measured on its inner QLineEdit,
/// which is what actually paints the binding. Polishing the outer edit first
/// resolves the stylesheet font down the whole subtree, so the advance is the
/// one the field paints with, not the default inherited from an unstyled parent.
int shortcutFieldWidth(const QKeySequenceEdit* edit) {
    auto* field = edit->findChild<QLineEdit*>();
    if (field == nullptr) {
        return edit->sizeHint().width();
    }
    edit->ensurePolished();
    field->ensurePolished();

    const QStyle* style = field->style();
    int width = QFontMetrics(field->fontMetrics())
                    .horizontalAdvance(QStringLiteral("Ctrl+Alt+Shift+F12"));

    // The styled border and padding. The field has no laid-out geometry while
    // the dialog is still being built, so measure against its polished size hint
    // rather than the transient zero-height child geometry.
    QStyleOptionFrame frame;
    frame.initFrom(field);
    frame.rect = QRect(0, 0, width + theme::kSpacingLarge, field->sizeHint().height());
    const QRect contents = style->subElementRect(QStyle::SE_LineEditContents, &frame, field);
    width += frame.rect.width() - contents.width();
    width += 2 * style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, field);

    // The clear action is drawn inside the field, so its actual child widget
    // geometry is the space unavailable to text. The style's nominal icon size
    // can be smaller than the QToolButton QLineEdit creates for this action,
    // especially when the field is disabled.
    if (field->isClearButtonEnabled()) {
        if (auto* clearButton = field->findChild<QToolButton*>()) {
            clearButton->ensurePolished();
            width += clearButton->sizeHint().width();
        } else {
            width += style->pixelMetric(QStyle::PM_LineEditIconSize, nullptr, field);
            width += style->pixelMetric(QStyle::PM_LineEditIconMargin, nullptr, field);
        }
    }
    return width + theme::kSpacingMedium + theme::kSpacingSmall;
}

mplayer::shell::ShortcutClaim presetClaim(const QString& id, const QString& label) {
    mplayer::shell::ShortcutClaim claim;
    claim.ownerId = id;
    claim.ownerLabel = label;
    claim.kind = mplayer::shell::ShortcutOwnerKind::Preset;
    return claim;
}

/// Names the holder of a combination in a refusal, so the message says who
/// already owns the key rather than only that it is taken.
QString shortcutOwnerName(const mplayer::shell::ShortcutConflict& conflict) {
    switch (conflict.ownerKind) {
        case mplayer::shell::ShortcutOwnerKind::Preset:
            return QCoreApplication::translate("SettingsDialog", "preset “%1”")
                .arg(conflict.ownerLabel);
        case mplayer::shell::ShortcutOwnerKind::Reserved:
            break;
    }
    return QCoreApplication::translate("SettingsDialog", "the player");
}

/// The caption the synthetic "requested but not offered" recipe entry carries.
/// A retranslation restates this one entry rather than rebuilding the combo, so
/// the user's selection is never moved by a language switch.
QString unavailableRecipeCaption(const QString& selection) {
    return QCoreApplication::translate("SettingsDialog", "%1 (not available)").arg(selection);
}

bool isReservedConflict(const mplayer::shell::ShortcutConflict& conflict) {
    return conflict.reasonKey == QLatin1String(mplayer::shell::kShortcutReservedReasonKey);
}

/// A refusal while editing or saving: the key is not written.
QString presetConflictMessage(const mplayer::shell::ShortcutConflict& conflict,
                              const QString& portable, const QString& label) {
    if (isReservedConflict(conflict)) {
        return QCoreApplication::translate(
                   "SettingsDialog",
                   "Key %1 is reserved by the player and cannot be assigned to preset “%2”.")
            .arg(portable, label);
    }
    return QCoreApplication::translate("SettingsDialog",
                                       "Combination %1 is already assigned to %2.")
        .arg(portable, shortcutOwnerName(conflict));
}

/// A refusal while loading: the stored binding is skipped, never installed, and
/// the reason names who holds the key now.
QString presetLoadMessage(const mplayer::shell::ShortcutConflict& conflict,
                          const QString& portable, const QString& label) {
    if (isReservedConflict(conflict)) {
        return QCoreApplication::translate(
                   "SettingsDialog",
                   "Saved key %1 is reserved by the player; the binding for “%2” was cleared.")
            .arg(portable, label);
    }
    return QCoreApplication::translate(
               "SettingsDialog",
               "Saved combination %1 is already assigned to %2; it was kept there and cleared "
               "for “%3”.")
        .arg(portable, shortcutOwnerName(conflict), label);
}

}  // namespace

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
    // by default, which four tabs could not afford inside 560 logical pixels.
    tabs_->setStyleSheet(
        QStringLiteral("QTabBar::tab { padding: 7px %1px; }").arg(theme::kSpacingSmall));
    generalTab_ = buildGeneralTab();
    presetsTab_ = buildPresetsTab();
    processingTab_ = buildProcessingTab();
    componentsTab_ = buildComponentsTab();
    tabs_->addTab(generalTab_, theme::gearIcon(theme::kIconSize, QLatin1String(theme::kAccent)),
                  tr("General"));
    tabs_->addTab(presetsTab_, theme::keyboardIcon(theme::kIconSize, QLatin1String(theme::kAccent)),
                  tr("Presets"));
    tabs_->addTab(processingTab_, theme::slidersIcon(theme::kIconSize, QLatin1String(theme::kAccent)),
                  tr("Processing"));
    tabs_->addTab(componentsTab_, theme::packageIcon(theme::kIconSize, QLatin1String(theme::kAccent)),
                  tr("Components"));
    if (!presetError_->isHidden()) {
        tabs_->setCurrentWidget(presetsTab_);
    }
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (tabs_->widget(index) == componentsTab_) emit componentsTabOpened(this);
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

QWidget* SettingsDialog::buildGeneralTab() {
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("nativeGeneralScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* page = new QWidget(scroll);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(theme::kSpacingSmall, theme::kSpacingMedium, theme::kSpacingSmall,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    // ── Card 1: Mikai API Access Token & Live Rate-Limit Readout ──────────────
    QVBoxLayout* apiLayout = nullptr;
    QFrame* apiCard = makeSettingsCard(page, &apiLayout);

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(theme::kSpacingSmall);
    auto* iconLabel = new QLabel(apiCard);
    iconLabel->setObjectName(QStringLiteral("muted"));
    iconLabel->setPixmap(theme::gearIcon(20, QLatin1String(theme::kAccent)).pixmap(20, 20));
    tokenHeading_ = new QLabel(tr("Mikai API access token"), apiCard);
    tokenHeading_->setObjectName(QStringLiteral("heading"));
    yieldCaptionWidth(tokenHeading_);
    headerRow->addWidget(iconLabel, 0, Qt::AlignVCenter);
    headerRow->addWidget(tokenHeading_, 1, Qt::AlignVCenter);
    apiLayout->addLayout(headerRow);

    tokenDescription_ = new QLabel(
        tr("An API key raises the limit from 60 to 300 requests per minute and is stored "
           "locally in the .env file."),
        apiCard);
    tokenDescription_->setObjectName(QStringLiteral("muted"));
    tokenDescription_->setWordWrap(true);
    apiLayout->addWidget(tokenDescription_);

    tokenLink_ = new QLabel(apiCard);
    tokenLink_->setObjectName(QStringLiteral("muted"));
    tokenLink_->setTextFormat(Qt::RichText);
    tokenLink_->setText(tr("<a style=\"color: %1; text-decoration: none;\" "
                           "href=\"https://mikai.me/user/developers\">"
                           "Get a key from the developer dashboard on mikai.me ↗</a>")
                             .arg(QLatin1String(theme::kAccent)));
    tokenLink_->setOpenExternalLinks(true);
    tokenLink_->setWordWrap(true);
    apiLayout->addWidget(tokenLink_);

    auto* tokenInputRow = new QHBoxLayout();
    tokenInputRow->setSpacing(theme::kSpacingSmall);
    token_ = new QLineEdit(apiCard);
    token_->setPlaceholderText(tr("mk_... (leave blank for anonymous access)"));
    token_->setAccessibleName(tr("Mikai API access token"));
    token_->setClearButtonEnabled(true);
    token_->setMinimumHeight(theme::kSettingsControlSize);
    const QString current = client_.apiKey();
    if (!current.isEmpty()) {
        token_->setText(current);
    }
    tokenInputRow->addWidget(token_, 1);

    testButton_ = new QPushButton(tr("Test"), apiCard);
    testButton_->setAccessibleName(tr("Test API token"));
    testButton_->setMinimumHeight(theme::kSettingsControlSize);
    connect(testButton_, &QPushButton::clicked, this, &SettingsDialog::testToken);
    tokenInputRow->addWidget(testButton_, 0);
    apiLayout->addLayout(tokenInputRow);

    status_ = new QLabel(apiCard);
    status_->setObjectName(QStringLiteral("status"));
    status_->setWordWrap(true);
    apiLayout->addWidget(status_);

    quotaTitle_ = new QLabel(tr("API usage"), apiCard);
    quotaTitle_->setObjectName(QStringLiteral("sectionHeading"));
    yieldCaptionWidth(quotaTitle_);
    apiLayout->addWidget(quotaTitle_);
    quota_ = new QLabel(apiCard);
    quota_->setObjectName(QStringLiteral("descriptionPanel"));
    quota_->setWordWrap(true);
    apiLayout->addWidget(quota_);

    root->addWidget(apiCard);

    // ── Card 2: Home Page Feed Sections (2-Column Grid) ───────────────────────
    QVBoxLayout* feedsLayout = nullptr;
    QFrame* feedsCard = makeSettingsCard(page, &feedsLayout);

    feedsTitle_ = new QLabel(tr("Home page sections"), feedsCard);
    feedsTitle_->setObjectName(QStringLiteral("sectionHeading"));
    yieldCaptionWidth(feedsTitle_);
    feedsLayout->addWidget(feedsTitle_);
    feedsHint_ = new QLabel(
        tr("Disabled sections are not loaded and do not use API requests. Search and title "
           "pages are always available."),
        feedsCard);
    feedsHint_->setObjectName(QStringLiteral("muted"));
    feedsHint_->setWordWrap(true);
    feedsLayout->addWidget(feedsHint_);

    QSettings settings;
    const QSet<QString> enabled = feeds::enabled(settings.value(feeds::settingsKey()).toString());
    initialFeeds_ = enabled;
    auto* feedsGrid = new QGridLayout();
    feedsGrid->setHorizontalSpacing(theme::kSpacingMedium);
    feedsGrid->setVerticalSpacing(theme::kSpacingXs);
    const QStringList allFeeds = feeds::all();
    for (int i = 0; i < allFeeds.size(); ++i) {
        const QString& key = allFeeds.at(i);
        auto* box = new QCheckBox(feeds::label(key), feedsCard);
        box->setAccessibleName(feeds::label(key));
        box->setMinimumHeight(theme::kSettingsControlSize);
        box->setChecked(enabled.contains(key));
        feedsGrid->addWidget(box, i / 2, i % 2);
        feedBoxes_.insert(key, box);
    }
    feedsGrid->setColumnStretch(0, 1);
    feedsGrid->setColumnStretch(1, 1);
    feedsLayout->addLayout(feedsGrid);

    root->addWidget(feedsCard);

    // ── Card 3: Interface Language ────────────────────────────────────────────
    QVBoxLayout* languageLayout = nullptr;
    QFrame* languageCard = makeSettingsCard(page, &languageLayout);

    languageTitle_ = new QLabel(tr("Interface language"), languageCard);
    languageTitle_->setObjectName(QStringLiteral("sectionHeading"));
    yieldCaptionWidth(languageTitle_);
    languageLayout->addWidget(languageTitle_);

    language_ = new QComboBox(languageCard);
    language_->setObjectName(QStringLiteral("uiLanguageCombo"));
    language_->setMinimumHeight(theme::kSettingsControlSize);
    language_->addItem(tr("Use system language"), QStringLiteral("system"));
    language_->addItem(tr("English"), QStringLiteral("en"));
    language_->addItem(tr("Ukrainian"), QStringLiteral("uk"));
    language_->setAccessibleName(tr("Interface language"));
    const int languageIndex = language_->findData(languagePreference());
    if (languageIndex >= 0) {
        language_->setCurrentIndex(languageIndex);
    }
    languageLayout->addWidget(language_);

    languageHint_ = new QLabel(
        tr("Applied immediately to the running windows, including the open player."),
        languageCard);
    languageHint_->setObjectName(QStringLiteral("muted"));
    languageHint_->setWordWrap(true);
    languageLayout->addWidget(languageHint_);

    root->addWidget(languageCard);

    root->addStretch();
    scroll->setWidget(page);
    generalScroll_ = scroll;
    return scroll;
}

QWidget* SettingsDialog::buildPresetsTab() {
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(theme::kSpacingSmall, theme::kSpacingMedium, theme::kSpacingSmall,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingSmall);

    presetsHeading_ = new QLabel(tr("Anime4K preset shortcuts"), page);
    presetsHeading_->setObjectName(QStringLiteral("heading"));
    yieldCaptionWidth(presetsHeading_);
    root->addWidget(presetsHeading_);

    presetsDescription_ = new QLabel(
        tr("Focus a field and press a key or combination with Ctrl, Alt, or Shift. "
           "The × button clears a binding. A combination can be assigned to only one "
           "preset; built-in player shortcuts are reserved."),
        page);
    presetsDescription_->setObjectName(QStringLiteral("muted"));
    presetsDescription_->setWordWrap(true);
    root->addWidget(presetsDescription_);

    // Toolbar row: quick filter input + Clear all shortcuts action.
    auto* filterRow = new QHBoxLayout();
    filterRow->setSpacing(theme::kSpacingSmall);
    presetFilterEdit_ = new QLineEdit(page);
    presetFilterEdit_->setObjectName(QStringLiteral("presetFilterEdit"));
    presetFilterEdit_->setPlaceholderText(tr("Filter presets…"));
    presetFilterEdit_->setAccessibleName(tr("Filter presets"));
    presetFilterEdit_->setClearButtonEnabled(true);
    presetFilterEdit_->setMinimumHeight(theme::kSettingsControlSize);
    filterRow->addWidget(presetFilterEdit_, 1);

    clearAllPresetsButton_ = new QPushButton(tr("Clear all"), page);
    clearAllPresetsButton_->setObjectName(QStringLiteral("clearAllPresetsButton"));
    clearAllPresetsButton_->setAccessibleName(tr("Clear all preset shortcuts"));
    clearAllPresetsButton_->setMinimumHeight(theme::kSettingsControlSize);
    filterRow->addWidget(clearAllPresetsButton_, 0);
    root->addLayout(filterRow);

    presetError_ = new QLabel(page);
    presetError_->setObjectName(QStringLiteral("status"));
    presetError_->setWordWrap(true);
    presetError_->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(theme::kDanger)));
    presetError_->hide();
    root->addWidget(presetError_);

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* list = new QVBoxLayout(content);
    list->setContentsMargins(0, 0, theme::kSpacingSmall, 0);
    list->setSpacing(theme::kSpacingXs);

    QSettings settings;
    settings.beginGroup(QLatin1String(mplayer::shell::kPresetShortcutSettingsGroup));
    QMap<QString, mplayer::shell::ShortcutClaim> claims;
    QString loadError;
    QString headingForCategory;
    for (const mplayer::anime4k::ModeInfo& mode : mplayer::anime4k::availableModes()) {
        const QString label = mplayer::anime4k::displayLabel(mode.mode);
        const QString headingText = shortcutGroupHeading(mode.category);
        if (!headingText.isEmpty() && headingText != headingForCategory) {
            headingForCategory = headingText;
            auto* groupHeading = new QLabel(headingText, content);
            groupHeading->setObjectName(QStringLiteral("sectionHeading"));
            list->addWidget(groupHeading);
            presetGroupHeadings_.append({mode.category, groupHeading});
        }

        auto* rowWidget = new QWidget(content);
        rowWidget->setObjectName(QStringLiteral("presetShortcutRow"));
        auto* row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(theme::kSpacingMedium, theme::kSpacingXs,
                                theme::kSpacingMedium, theme::kSpacingXs);
        row->setSpacing(theme::kSpacingMedium);

        auto* name = new QLabel(label, rowWidget);
        name->setToolTip(mode.hint);
        name->setWordWrap(true);
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        row->addWidget(name, 1);

        auto* edit = new QKeySequenceEdit(rowWidget);
        edit->setMaximumSequenceLength(1);
        edit->setClearButtonEnabled(true);
        edit->setMinimumHeight(theme::kSettingsControlSize);
        edit->setFixedWidth(shortcutFieldWidth(edit));
        if (auto* input = edit->findChild<QLineEdit*>()) {
            input->setPlaceholderText(tr("Press a key"));
        }
        const QString stored = settings.value(mode.id).toString();
        if (!stored.isEmpty()) {
            const QKeySequence sequence =
                QKeySequence::fromString(stored, QKeySequence::PortableText);
            if (!sequence.isEmpty()) {
                const QString portable = mplayer::shell::shortcutClaimKey(sequence);
                const mplayer::shell::ShortcutConflict conflict =
                    mplayer::shell::findShortcutConflict(sequence, claims);
                if (conflict.blocked) {
                    if (loadError.isEmpty()) {
                        loadError = presetLoadMessage(conflict, portable, label);
                    }
                } else {
                    edit->setKeySequence(sequence);
                    claims.insert(portable, presetClaim(mode.id, label));
                }
            }
        }
        edit->setAccessibleName(tr("Shortcut for %1").arg(label));
        edit->setToolTip(mode.hint);
        name->setBuddy(edit);
        row->addWidget(edit, 0, Qt::AlignVCenter);

        list->addWidget(rowWidget);
        presetRows_.append({mode.id, label, mode.category, edit, name, rowWidget, mode.mode});
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this,
                [this, edit, label, previous = edit->keySequence()](const QKeySequence& sequence) mutable {
                    if (!sequence.isEmpty()) {
                        const mplayer::shell::ShortcutConflict conflict =
                            mplayer::shell::findShortcutConflict(sequence, shortcutClaims(edit));
                        if (conflict.blocked) {
                            QSignalBlocker blocker(edit);
                            edit->setKeySequence(previous);
                            showPresetError(presetConflictMessage(
                                conflict, mplayer::shell::shortcutClaimKey(sequence), label));
                            return;
                        }
                    }
                    previous = sequence;
                    presetError_->clear();
                    presetError_->hide();
                });
    }
    settings.endGroup();
    if (!loadError.isEmpty()) {
        presetError_->setText(loadError);
        presetError_->show();
    }

    connect(presetFilterEdit_, &QLineEdit::textChanged, this, &SettingsDialog::filterPresetRows);
    connect(clearAllPresetsButton_, &QPushButton::clicked, this, [this] {
        for (const PresetRow& row : presetRows_) {
            if (row.edit != nullptr) {
                row.edit->clear();
            }
        }
        presetError_->clear();
        presetError_->hide();
    });

    list->addStretch();
    scroll->setWidget(content);
    root->addWidget(scroll, 1);
    presetsScroll_ = scroll;
    return page;
}

void SettingsDialog::filterPresetRows(const QString& query) {
    const QString needle = query.trimmed();
    QSet<QString> visibleCategories;
    for (const PresetRow& row : presetRows_) {
        if (row.container == nullptr) continue;
        const QString keyText = row.edit != nullptr
                                    ? row.edit->keySequence().toString(QKeySequence::NativeText)
                                    : QString();
        const bool matches = needle.isEmpty() ||
                             row.label.contains(needle, Qt::CaseInsensitive) ||
                             keyText.contains(needle, Qt::CaseInsensitive);
        row.container->setVisible(matches);
        if (matches) {
            visibleCategories.insert(row.category);
        }
    }
    for (const PresetGroupHeading& heading : presetGroupHeadings_) {
        if (heading.label != nullptr) {
            heading.label->setVisible(needle.isEmpty() ||
                                      visibleCategories.contains(heading.category));
        }
    }
}

QMap<QString, mplayer::shell::ShortcutClaim> SettingsDialog::shortcutClaims(
    const QKeySequenceEdit* exclude) const {
    QMap<QString, mplayer::shell::ShortcutClaim> claims;

    for (const PresetRow& row : presetRows_) {
        if (row.edit == exclude) {
            continue;
        }
        const QKeySequence sequence = row.edit->keySequence();
        if (sequence.isEmpty()) {
            continue;
        }
        const QString key = mplayer::shell::shortcutClaimKey(sequence);
        if (!claims.contains(key)) {
            claims.insert(key, presetClaim(row.id, row.label));
        }
    }
    return claims;
}

QWidget* SettingsDialog::buildProcessingTab() {
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("nativeProcessingScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* page = new QWidget(scroll);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(theme::kSpacingSmall, theme::kSpacingMedium, theme::kSpacingSmall,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    nativeHeading_ = new QLabel(tr("Processing runtime"), page);
    nativeHeading_->setObjectName(QStringLiteral("heading"));
    yieldCaptionWidth(nativeHeading_);
    root->addWidget(nativeHeading_);

    nativeDescription_ = new QLabel(
        tr("Standard uses official AnimeJaNai choices from the player menu and Components. "
           "Select Native to configure the separate private Native workflow."),
        page);
    nativeDescription_->setObjectName(QStringLiteral("muted"));
    nativeDescription_->setWordWrap(true);
    nativeDescription_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(nativeDescription_);

    nativeError_ = new QLabel(page);
    nativeError_->setObjectName(QStringLiteral("status"));
    nativeError_->setWordWrap(true);
    nativeError_->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(theme::kDanger)));
    nativeError_->hide();
    root->addWidget(nativeError_);

    const bool editable = nativePreferences_.schemaRecognised;

    auto addRowTo = [](QVBoxLayout* targetLayout, QWidget* parentWidget,
                       const QString& label, QWidget* field) {
        auto* row = new QHBoxLayout();
        row->setSpacing(theme::kSpacingMedium);
        auto* name = new QLabel(label, parentWidget);
        name->setBuddy(field);
        row->addWidget(name, 0);
        row->addWidget(field, 1, Qt::AlignVCenter);
        targetLayout->addLayout(row);
        return name;
    };

    // ── Card 1: Runtime Module Selection (Always Visible) ─────────────────────
    QVBoxLayout* runtimeLayout = nullptr;
    QFrame* runtimeCard = makeSettingsCard(page, &runtimeLayout);

    runtimeChoice_ = new QComboBox(runtimeCard);
    runtimeChoice_->setObjectName(QStringLiteral("runtimeChoiceCombo"));
    runtimeChoice_->setAccessibleName(tr("mpv runtime"));
    runtimeChoice_->setMinimumHeight(theme::kSettingsControlSize);
    runtimeChoice_->addItem(tr("Standard"),
                            static_cast<int>(mplayer::native::RuntimeChoice::Standard));
    runtimeChoice_->addItem(tr("Native"),
                            static_cast<int>(mplayer::native::RuntimeChoice::Native));
    const int runtimeIndex =
        runtimeChoice_->findData(static_cast<int>(mplayer::native::storedRuntimeChoice()));
    if (runtimeIndex >= 0) {
        runtimeChoice_->setCurrentIndex(runtimeIndex);
    }
    runtimeModuleRowLabel_ = addRowTo(runtimeLayout, runtimeCard, tr("Runtime module"), runtimeChoice_);

    runtimeHint_ = new QLabel(runtimeCard);
    runtimeHint_->setObjectName(QStringLiteral("muted"));
    runtimeHint_->setWordWrap(true);
    runtimeHint_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    runtimeLayout->addWidget(runtimeHint_);

    root->addWidget(runtimeCard);

    // ── Card 2: Private Native Backend & Recipe Selection ─────────────────────
    QVBoxLayout* nativeConfigLayout = nullptr;
    nativeConfigCard_ = makeSettingsCard(page, &nativeConfigLayout);

    nativeBackend_ = new QComboBox(nativeConfigCard_);
    nativeBackend_->setObjectName(QStringLiteral("nativeBackendCombo"));
    nativeBackend_->setAccessibleName(tr("Native backend"));
    nativeBackend_->setMinimumHeight(theme::kSettingsControlSize);
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
    backendRowLabel_ = addRowTo(nativeConfigLayout, nativeConfigCard_, tr("Backend"), nativeBackend_);

    nativeRecipe_ = new QComboBox(nativeConfigCard_);
    nativeRecipe_->setObjectName(QStringLiteral("nativeRecipeCombo"));
    nativeRecipe_->setAccessibleName(tr("Native recipe"));
    nativeRecipe_->setMinimumHeight(theme::kSettingsControlSize);
    nativeRecipe_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    nativeRecipe_->setMinimumContentsLength(theme::kSettingsChoiceLengthChars);
    nativeRecipe_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    populateRecipeChoices(nativePreferences_.recipeId);
    recipeRowLabel_ = addRowTo(nativeConfigLayout, nativeConfigCard_, tr("Recipe"), nativeRecipe_);

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

    // ── Card 3: Native Model Assets & Engine Preparation ──────────────────────
    QVBoxLayout* assetsCardLayout = nullptr;
    nativeAssetsCard_ = makeSettingsCard(page, &assetsCardLayout);

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

    // ── Card 4: RIFE Temporal Interpolation ───────────────────────────────────
    QVBoxLayout* rifeCardLayout = nullptr;
    nativeRifeCard_ = makeSettingsCard(page, &rifeCardLayout);

    rifeHeading_ = new QLabel(tr("RIFE interpolation"), nativeRifeCard_);
    rifeHeading_->setObjectName(QStringLiteral("sectionHeading"));
    yieldCaptionWidth(rifeHeading_);
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

QWidget* SettingsDialog::buildComponentsTab() {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* page = new QWidget(scroll);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    ComponentStoreTab* tab = nullptr;
    if (adoptedComponents_ != nullptr) {
        adoptedComponentsTab_ = true;
        adoptedComponentsHome_ = adoptedComponents_->parentWidget();
        tab = adoptedComponents_;
        tab->setParent(page);
        tab->show();
    } else {
        tab = new ComponentStoreTab(page);
        tab->setRunner(&runner_);
        tab->setClient(&client_);
    }
    componentStoreTab_ = tab;
    if (componentProvisioner_ != nullptr) tab->setComponentProvisioner(componentProvisioner_);
    root->addWidget(tab);

    scroll->setWidget(page);
    componentsScroll_ = scroll;
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

bool SettingsDialog::feedsChanged() const {
    QSet<QString> chosen;
    for (auto it = feedBoxes_.constBegin(); it != feedBoxes_.constEnd(); ++it) {
        if (it.value()->isChecked()) {
            chosen.insert(it.key());
        }
    }
    return chosen != initialFeeds_;
}

void SettingsDialog::testToken() {
    const QString candidate = token_->text().trimmed();
    status_->setText(tr("Checking token…"));
    testButton_->setEnabled(false);
    QPointer<SettingsDialog> self(this);
    backend::MikaiClient* client = &client_;
    runner_.run(
        [client, candidate] {
            const QMap<QString, QVariant> result = client->validateApiKey(candidate);
            return QVariant::fromValue(result);
        },
        [self](const QVariant& raw) {
            if (!self) return;
            self->testButton_->setEnabled(true);
            const QMap<QString, QVariant> result = raw.toMap();
            if (result.value(QStringLiteral("valid")).toBool()) {
                const bool authenticated = result.value(QStringLiteral("authenticated")).toBool();
                const int limit = result.value(QStringLiteral("rate_limit"), 60).toInt();
                const QString name = result.value(QStringLiteral("name")).toString();
                if (authenticated && !name.isEmpty()) {
                    self->status_->setText(
                        tr("Key is valid (user: %1, limit: %2/min)").arg(name).arg(limit));
                } else if (authenticated) {
                    self->status_->setText(tr("Key is valid (limit: %1/min)").arg(limit));
                } else {
                    self->status_->setText(
                        tr("Anonymous access (limit: %1/min)").arg(limit));
                }
            } else {
                const QString code = result.value(QStringLiteral("error_code")).toString();
                const QString error = result.value(QStringLiteral("error")).toString();
                const QString translatedError = localizedApiError(code);
                const QString displayError =
                    translatedError.isEmpty()
                        ? (error.isEmpty() ? tr("Invalid token") : error)
                        : translatedError;
                self->status_->setText(tr("Error: %1").arg(displayError));
            }
        },
        [self](const backend::Failure& failure) {
            if (!self) return;
            self->testButton_->setEnabled(true);
            self->status_->setText(tr("Connection error: %1").arg(backend::failureLine(failure)));
        });
}

void SettingsDialog::refreshQuota() {
    const backend::RateLimitState state = client_.rateLimitState();
    const QString limit = state.limit > 0 ? QString::number(state.limit) : QStringLiteral("—");
    QString text;
    if (!state.used.has_value() || !state.remaining.has_value()) {
        text = tr("Limit: %1 requests/min · waiting for the next API response to update the counter")
                   .arg(limit);
    } else {
        text = tr("Window: %1 used, %2 of %3 requests available")
                   .arg(*state.used)
                   .arg(*state.remaining)
                   .arg(limit);
        if (state.resetIn > 0) {
            text += tr(" · resets in %1 s").arg(state.resetIn);
        }
    }
    text += tr("\nActive: %1 · queued: %2").arg(state.inFlight).arg(state.queued);
    if (state.queued > 0) {
        text += tr(" · requests will continue automatically when the limit resets");
    }
    quota_->setText(text);
}

QString SettingsDialog::collectPresetShortcuts(QMap<QString, QString>* presets) const {
    QMap<QString, mplayer::shell::ShortcutClaim> claims;
    for (const PresetRow& row : presetRows_) {
        const QKeySequence sequence = row.edit->keySequence();
        if (sequence.isEmpty()) {
            continue;
        }
        const QString portable = mplayer::shell::shortcutClaimKey(sequence);
        const mplayer::shell::ShortcutConflict conflict =
            mplayer::shell::findShortcutConflict(sequence, claims);
        if (conflict.blocked) {
            return presetConflictMessage(conflict, portable, row.label);
        }
        claims.insert(portable, presetClaim(row.id, row.label));
        presets->insert(row.id, portable);
    }
    return {};
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
        }
    }
    cancelButton_->setText(tr("Cancel"));
    saveButton_->setText(tr("Save"));

    retranslateGeneralTab();
    retranslatePresetsTab();
    retranslateProcessingTab();
    retranslating_ = false;
}

void SettingsDialog::retranslateGeneralTab() {
    tokenHeading_->setText(tr("Mikai API access token"));
    tokenDescription_->setText(
        tr("An API key raises the limit from 60 to 300 requests per minute and is stored "
           "locally in the .env file."));
    tokenLink_->setText(tr("<a style=\"color: %1; text-decoration: none;\" "
                           "href=\"https://mikai.me/user/developers\">"
                           "Get a key from the developer dashboard on mikai.me ↗</a>")
                            .arg(QLatin1String(theme::kAccent)));
    token_->setPlaceholderText(tr("mk_... (leave blank for anonymous access)"));
    token_->setAccessibleName(tr("Mikai API access token"));
    testButton_->setText(tr("Test"));
    testButton_->setAccessibleName(tr("Test API token"));

    quotaTitle_->setText(tr("API usage"));
    feedsTitle_->setText(tr("Home page sections"));
    feedsHint_->setText(
        tr("Disabled sections are not loaded and do not use API requests. Search and title "
           "pages are always available."));
    for (auto it = feedBoxes_.constBegin(); it != feedBoxes_.constEnd(); ++it) {
        it.value()->setText(feeds::label(it.key()));
        it.value()->setAccessibleName(feeds::label(it.key()));
    }

    languageTitle_->setText(tr("Interface language"));
    languageHint_->setText(tr("Applied immediately to the running windows, including the open player."));
    const auto restateLanguage = [this](const QString& data, const QString& caption) {
        const int index = language_->findData(data);
        if (index >= 0) language_->setItemText(index, caption);
    };
    restateLanguage(QStringLiteral("system"), tr("Use system language"));
    restateLanguage(QStringLiteral("en"), tr("English"));
    restateLanguage(QStringLiteral("uk"), tr("Ukrainian"));
    language_->setAccessibleName(tr("Interface language"));

    refreshQuota();
}

void SettingsDialog::retranslatePresetsTab() {
    presetsHeading_->setText(tr("Anime4K preset shortcuts"));
    presetsDescription_->setText(
        tr("Focus a field and press a key or combination with Ctrl, Alt, or Shift. "
           "The × button clears a binding. A combination can be assigned to only one "
           "preset; built-in player shortcuts are reserved."));
    if (presetFilterEdit_ != nullptr) {
        presetFilterEdit_->setPlaceholderText(tr("Filter presets…"));
        presetFilterEdit_->setAccessibleName(tr("Filter presets"));
    }
    if (clearAllPresetsButton_ != nullptr) {
        clearAllPresetsButton_->setText(tr("Clear all"));
        clearAllPresetsButton_->setAccessibleName(tr("Clear all preset shortcuts"));
    }
    for (const PresetGroupHeading& heading : presetGroupHeadings_) {
        heading.label->setText(shortcutGroupHeading(heading.category));
    }
    QHash<mplayer::anime4k::Mode, mplayer::anime4k::ModeInfo> catalogue;
    for (const mplayer::anime4k::ModeInfo& info : mplayer::anime4k::availableModes()) {
        catalogue.insert(info.mode, info);
    }
    for (PresetRow& row : presetRows_) {
        const auto found = catalogue.constFind(row.mode);
        if (found != catalogue.constEnd()) {
            row.label = found->label;
        }
        const QString hint = found != catalogue.constEnd() ? found->hint : QString();
        row.name->setText(row.label);
        row.name->setToolTip(hint);
        row.edit->setToolTip(hint);
        row.edit->setAccessibleName(tr("Shortcut for %1").arg(row.label));
        if (auto* input = row.edit->findChild<QLineEdit*>()) {
            input->setPlaceholderText(tr("Press a key"));
        }
    }
    if (presetFilterEdit_ != nullptr) {
        filterPresetRows(presetFilterEdit_->text());
    }
}

void SettingsDialog::retranslateProcessingTab() {
    nativeHeading_->setText(tr("Processing runtime"));
    nativeDescription_->setText(
        tr("Standard uses official AnimeJaNai choices from the player menu and Components. "
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