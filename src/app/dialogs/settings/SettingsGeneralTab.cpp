#include "dialogs/SettingsDialog.hpp"
#include "state/AppearanceStore.hpp"
#include "state/SettingsKeys.hpp"

#include <VideoSession.hpp>

#include "I18n.hpp"
#include "dialogs/settings/SettingsHelpers.hpp"
#include "dialogs/settings/SettingsUi.hpp"
#include "gui/Win32Support.hpp"
#include "state/Feeds.hpp"
#include "support/SystemContextMenu.hpp"
#include "support/SystemStartup.hpp"
#include "theme/Colors.hpp"
#include "theme/Icons.hpp"
#include "theme/Metrics.hpp"
#include "theme/Palette.hpp"
#include "workers/Runner.hpp"

#include <backend/MikaiClient.hpp>
#include <dialogs/ComponentStoreTab.hpp>
#include <player/shaders/Anime4K.hpp>

#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include "widgets/FlowLayout.hpp"
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPair>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace mikai::app {
namespace {

/// A QLabel that paints its QIcon instead of a pixmap snapshot. The glyph
/// engine rasterises for the painter's own device ratio, so the badge is drawn
/// at the resolution of the screen the dialog is actually on - including after
/// the window moves to a screen with a different ratio - rather than the ratio
/// that happened to be current when the page was built.
class IconLabel final : public QLabel {
public:
    IconLabel(const QIcon& icon, int extent, QWidget* parent = nullptr)
        : QLabel(parent), icon_(icon), extent_(qMax(1, extent)) {
        setAlignment(Qt::AlignCenter);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        const QRect box(QPoint((width() - extent_) / 2, (height() - extent_) / 2),
                        QSize(extent_, extent_));
        icon_.paint(&painter, box, Qt::AlignCenter);
    }

    bool event(QEvent* e) override {
        // QWidget::event() consumes both of these without forwarding them to
        // changeEvent(), so this override is the only place the badge is told
        // the ratio changed. The engine picks its raster per paint, so a
        // repaint is all it needs: no cached pixmap and no extra allocation.
        const bool handled = QLabel::event(e);
        if (e->type() == QEvent::DevicePixelRatioChange ||
            e->type() == QEvent::ScreenChangeInternal) {
            update();
        }
        return handled;
    }

private:
    QIcon icon_;
    int extent_ = 0;
};

}  // namespace

QWidget* SettingsDialog::buildGeneralTab() {
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("nativeGeneralScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    auto* page = new QWidget(scroll);
    page->setObjectName(QStringLiteral("generalPage"));
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(theme::kSpacingSmall, theme::kSpacingMedium, theme::kSpacingSmall,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    // ── Card 1: Mikai API Access Token & Live Rate-Limit Telemetry ───────────
    QVBoxLayout* apiLayout = nullptr;
    QFrame* apiCard = makeSettingsCard(page, &apiLayout);
    apiCard->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    apiLayout->setSpacing(theme::kSpacingSmall);

    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(theme::kSpacingSmall);

    auto* iconBadge = new QFrame(apiCard);
    iconBadge->setObjectName(QStringLiteral("settingsSubCard"));
    iconBadge->setFixedSize(32, 32);
    auto* iconBadgeLayout = new QHBoxLayout(iconBadge);
    iconBadgeLayout->setContentsMargins(0, 0, 0, 0);
    auto* iconLabel =
        new IconLabel(theme::gearIcon(theme::kIconSize,
                                      theme::paletteFor(currentAppearance()).accent),
                      theme::kIconSize, iconBadge);
    iconLabel->setObjectName(QStringLiteral("muted"));
    iconBadgeLayout->addWidget(iconLabel);

    tokenHeading_ = new QLabel(tr("Mikai API access token"), apiCard);
    stylePageHeading(tokenHeading_);
    headerRow->addWidget(iconBadge, 0, Qt::AlignVCenter);
    headerRow->addWidget(tokenHeading_, 1, Qt::AlignVCenter);
    apiLayout->addLayout(headerRow);

    tokenDescription_ = new QLabel(apiCard);
    styleMutedCaption(tokenDescription_);
    apiLayout->addWidget(tokenDescription_);

    tokenLink_ = new QLabel(apiCard);
    tokenLink_->setObjectName(QStringLiteral("muted"));
    tokenLink_->setTextFormat(Qt::RichText);
    tokenLink_->setText(tr("<a style=\"color: %1; text-decoration: none; font-weight: 500;\" "
                           "href=\"https://mikai.me/user/developers\">"
                           "Get a key from the developer dashboard on mikai.me ↗</a>")
                            .arg(theme::paletteFor(currentAppearance()).accent));
    tokenLink_->setOpenExternalLinks(true);
    tokenLink_->setWordWrap(true);
    tokenLink_->setCursor(Qt::PointingHandCursor);
    apiLayout->addWidget(tokenLink_);

    auto* tokenInputRow = new QHBoxLayout();
    tokenInputRow->setContentsMargins(0, theme::kSpacingXs, 0, 0);
    tokenInputRow->setSpacing(theme::kSpacingSmall);

    token_ = new QLineEdit(apiCard);
    token_->setObjectName(QStringLiteral("apiTokenEdit"));
    token_->setPlaceholderText(tr("mk_... (leave blank for anonymous access)"));
    token_->setAccessibleName(tr("Mikai API access token"));
    token_->setClearButtonEnabled(true);
    token_->setMinimumHeight(theme::kSettingsControlSize);
    applyTabularNumerals(token_);
    const QString current = client_.apiKey();
    if (!current.isEmpty()) {
        token_->setText(current);
    }
    tokenInputRow->addWidget(token_, 1);

    testButton_ = new QPushButton(tr("Test"), apiCard);
    testButton_->setObjectName(QStringLiteral("testTokenButton"));
    testButton_->setAccessibleName(tr("Test API token"));
    polishInteractiveControl(testButton_, theme::kSettingsControlSize);
    connect(testButton_, &QPushButton::clicked, this, &SettingsDialog::testToken);
    tokenInputRow->addWidget(testButton_, 0);
    apiLayout->addLayout(tokenInputRow);

    status_ = new QLabel(apiCard);
    status_->setObjectName(QStringLiteral("status"));
    status_->setWordWrap(true);
    yieldCaptionWidth(status_);
    styleStatusBanner(status_, QStringLiteral("info"));
    apiLayout->addWidget(status_);

    apiLayout->addWidget(makeSettingsDivider(apiCard));

    QVBoxLayout* quotaSubLayout = nullptr;
    QFrame* quotaSubCard = makeSettingsSubCard(apiCard, &quotaSubLayout);

    quotaTitle_ = new QLabel(tr("API usage"), quotaSubCard);
    styleSectionHeading(quotaTitle_);
    quotaSubLayout->addWidget(quotaTitle_);

    quota_ = new QLabel(quotaSubCard);
    quota_->setObjectName(QStringLiteral("descriptionPanel"));
    quota_->setWordWrap(true);
    yieldCaptionWidth(quota_);
    applyTabularNumerals(quota_);
    quotaSubLayout->addWidget(quota_);

    apiLayout->addWidget(quotaSubCard);
    root->addWidget(apiCard);

    QVBoxLayout* subtitlesLayout = nullptr;
    QFrame* subtitlesCard = makeSettingsCard(page, &subtitlesLayout);
    subtitlesCard->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    subtitlesLayout->setSpacing(theme::kSpacingSmall);
    openSubtitlesTitle_ = new QLabel(tr("OpenSubtitles"), subtitlesCard);
    stylePageHeading(openSubtitlesTitle_);
    subtitlesLayout->addWidget(openSubtitlesTitle_);
    openSubtitlesHint_ = new QLabel(
#ifdef Q_OS_WIN
        tr("Enter your OpenSubtitles API key to search for subtitles in the player. "
           "The key is protected with Windows DPAPI and stored in the app data folder."),
#else
        tr("Enter your OpenSubtitles API key to search for subtitles in the player. "
           "The key is stored in a file only your user account can read, not in a "
           "keyring."),
#endif
        subtitlesCard);
    styleMutedCaption(openSubtitlesHint_);
    subtitlesLayout->addWidget(openSubtitlesHint_);
    openSubtitlesLink_ = new QLabel(subtitlesCard);
    openSubtitlesLink_->setObjectName(QStringLiteral("muted"));
    openSubtitlesLink_->setTextFormat(Qt::RichText);
    openSubtitlesLink_->setOpenExternalLinks(true);
    openSubtitlesLink_->setWordWrap(true);
    openSubtitlesLink_->setCursor(Qt::PointingHandCursor);
    subtitlesLayout->addWidget(openSubtitlesLink_);
    openSubtitlesKey_ = new QLineEdit(subtitlesCard);
    openSubtitlesKey_->setObjectName(QStringLiteral("openSubtitlesApiKeyEdit"));
    openSubtitlesKey_->setEchoMode(QLineEdit::Password);
    openSubtitlesKey_->setClearButtonEnabled(true);
    openSubtitlesKey_->setMinimumHeight(theme::kSettingsControlSize);
    openSubtitlesKey_->setText(mikai::gui::loadOpenSubtitlesApiKey());
    subtitlesLayout->addWidget(openSubtitlesKey_);
    root->addWidget(subtitlesCard);

    // ── Card 2: Home Page Feed Sections (2-Column Selectable Tile Grid) ──────
    QVBoxLayout* feedsLayout = nullptr;
    QFrame* feedsCard = makeSettingsCard(page, &feedsLayout);
    feedsCard->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    feedsLayout->setSpacing(theme::kSpacingSmall);

    feedsTitle_ = new QLabel(tr("Home page sections"), feedsCard);
    styleSectionHeading(feedsTitle_);
    feedsLayout->addWidget(feedsTitle_);

    feedsHint_ = new QLabel(
        tr("Disabled sections are not loaded and do not use API requests. Search and title "
           "pages are always available."),
        feedsCard);
    styleMutedCaption(feedsHint_);
    feedsLayout->addWidget(feedsHint_);

    feedsLayout->addWidget(makeSettingsDivider(feedsCard));

    QSettings settings;
    const QSet<QString> enabled = feeds::enabled(settings.value(feeds::settingsKey()).toString());
    initialFeeds_ = enabled;
    // Wrapping rather than a fixed two-column grid: a long translation would
    // otherwise set a page minimum wider than a narrow dialog can show.
    auto* feedsGrid = new FlowLayout(theme::kSpacingSmall, theme::kSpacingSmall);
    feedsGrid->setContentsMargins(0, theme::kSpacingXs, 0, 0);
    const QStringList allFeeds = feeds::all();
    for (const QString& key : allFeeds) {
        auto* box = new QCheckBox(feeds::label(key), feedsCard);
        box->setProperty("tileCheck", true);
        box->setAccessibleName(feeds::label(key));
        polishInteractiveControl(box, theme::kSettingsControlSize);
        box->setChecked(enabled.contains(key));
        feedsGrid->addWidget(box);
        feedBoxes_.insert(key, box);
    }
    feedsLayout->addLayout(feedsGrid);

    root->addWidget(feedsCard);

    // ── Card 3: Interface Language ───────────────────────────────────────────
    QVBoxLayout* languageLayout = nullptr;
    QFrame* languageCard = makeSettingsCard(page, &languageLayout);
    languageCard->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    languageLayout->setSpacing(theme::kSpacingSmall);

    languageTitle_ = new QLabel(tr("Interface language"), languageCard);
    styleSectionHeading(languageTitle_);
    languageLayout->addWidget(languageTitle_);

    languageHint_ = new QLabel(
        tr("Applied immediately to the running windows, including the open player."),
        languageCard);
    styleMutedCaption(languageHint_);
    languageLayout->addWidget(languageHint_);

    language_ = new QComboBox(languageCard);
    language_->setObjectName(QStringLiteral("uiLanguageCombo"));
    polishInteractiveControl(language_, theme::kSettingsControlSize);
    language_->addItem(tr("Use system language"), QStringLiteral("system"));
    language_->addItem(tr("English"), QStringLiteral("en"));
    language_->addItem(tr("Ukrainian"), QStringLiteral("uk"));
    language_->setAccessibleName(tr("Interface language"));
    const int languageIndex = language_->findData(previewLanguagePreference_);
    if (languageIndex >= 0) {
        language_->setCurrentIndex(languageIndex);
    }
    // A different choice is installed immediately, on this thread, without
    // rebuilding any page. The flag keeps the initial selection above and the
    // item restatements in retranslateGeneralTab from applying anything.
    connect(language_, &QComboBox::currentIndexChanged, this,
            [this](int index) { previewInterfaceLanguage(index); });
    languageSelectorReady_ = true;
    languageLayout->addWidget(language_);

    root->addWidget(languageCard);

    // ── Card 4: HDR playback (T-101/T-104) ─────────────────────────────
    QVBoxLayout* hdrLayout = nullptr;
    QFrame* hdrCard = makeSettingsCard(page, &hdrLayout);
    hdrCard->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    hdrLayout->setSpacing(theme::kSpacingSmall);

    playbackTitle_ = new QLabel(tr("HDR playback"), hdrCard);
    styleSectionHeading(playbackTitle_);
    hdrLayout->addWidget(playbackTitle_);

    playbackHint_ = new QLabel(
        tr("HDR tone mapping controls how HDR video is mapped to the display. "
           "It is separate from Native enhancement; saved changes reach a running player "
           "within a moment."),
        hdrCard);
    styleMutedCaption(playbackHint_);
    hdrLayout->addWidget(playbackHint_);

    hdrPolicy_ = new QComboBox(hdrCard);
    hdrPolicy_->setObjectName(QStringLiteral("hdrPolicyCombo"));
    polishInteractiveControl(hdrPolicy_, theme::kSettingsControlSize);
    hdrPolicy_->addItem(tr("Auto (recommended)"), 0);
    hdrPolicy_->addItem(tr("Stable - no per-frame adaptation"), 1);
    hdrPolicy_->addItem(tr("Vivid - Mobius curve"), 2);
    const int storedPolicy =
        static_cast<int>(mplayer::VideoSession::hdrToneMappingPolicy());
    const int policyIndex = hdrPolicy_->findData(storedPolicy);
    hdrPolicy_->setCurrentIndex(policyIndex >= 0 ? policyIndex : 0);
    hdrPolicy_->setAccessibleName(tr("HDR tone mapping"));
    hdrLayout->addWidget(hdrPolicy_);

    nativeHdrOverride_ = new QCheckBox(tr("Native enhancement for HDR"), hdrCard);
    nativeHdrOverride_->setObjectName(QStringLiteral("nativeHdrOverrideCheck"));
    polishInteractiveControl(nativeHdrOverride_, theme::kSettingsControlSize);
    nativeHdrOverride_->setChecked(mplayer::VideoSession::animeJanaiHdrOverride());
    hdrLayout->addWidget(nativeHdrOverride_);

    nativeHdrOverrideHint_ = new QLabel(
        tr("Native enhancement stages require 8-bit NV12 and are normally skipped for HDR "
           "sources. Enabling this allows that processing on HDR video; it does not convert "
           "the source to SDR, and may reduce HDR image quality."),
        hdrCard);
    styleMutedCaption(nativeHdrOverrideHint_);
    hdrLayout->addWidget(nativeHdrOverrideHint_);

    // P0-08: the one auto-advance setting, default ON, in the existing
    // playback card - no new settings page.
    autoAdvance_ = new QCheckBox(tr("Automatically play the next episode"), hdrCard);
    autoAdvance_->setObjectName(QStringLiteral("autoAdvanceCheck"));
    polishInteractiveControl(autoAdvance_, theme::kSettingsControlSize);
    QSettings playbackSettings;
    autoAdvance_->setChecked(playbackSettings.value(
        QString::fromLatin1(mplayer::VideoSession::kAutoAdvanceSettingKey), true).toBool());
    hdrLayout->addWidget(autoAdvance_);

    updateNotifications_ =
        new QCheckBox(tr("Check for updates automatically"), hdrCard);
    updateNotifications_->setObjectName(QStringLiteral("updateNotificationsCheck"));
    polishInteractiveControl(updateNotifications_, theme::kSettingsControlSize);
    updateNotifications_->setChecked(
        QSettings().value(QString::fromLatin1(kUpdateNotificationsEnabled), true).toBool());
    hdrLayout->addWidget(updateNotifications_);

    updateNotificationsHint_ = new QLabel(
        tr("Checks for a newer stable release using one unauthenticated request to GitHub. "
           "No update is downloaded or installed."),
        hdrCard);
    styleMutedCaption(updateNotificationsHint_);
    updateNotificationsHint_->setWordWrap(true);
    hdrLayout->addWidget(updateNotificationsHint_);

    root->addWidget(hdrCard);

    // ── Card 5: Video scaling (GOAL-H2) ─────────────────────────────────
    QVBoxLayout* scalingLayout = nullptr;
    QFrame* scalingCard = makeSettingsCard(page, &scalingLayout);
    scalingCard->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    scalingLayout->setSpacing(theme::kSpacingSmall);

    scalingTitle_ = new QLabel(scalingCard);
    styleSectionHeading(scalingTitle_);
    scalingLayout->addWidget(scalingTitle_);

    scalingHint_ = new QLabel(scalingCard);
    styleMutedCaption(scalingHint_);
    scalingLayout->addWidget(scalingHint_);

    scalingLayout->addWidget(makeSettingsDivider(scalingCard));

    const mplayer::VideoScalePolicy storedScalePolicy =
        mplayer::VideoSession::videoScalePolicy();
    scalingMode_ = new QComboBox(scalingCard);
    scalingMode_->setObjectName(QStringLiteral("videoScaleModeCombo"));
    polishInteractiveControl(scalingMode_, theme::kSettingsControlSize);
    for (const mplayer::VideoScaleMode mode :
         {mplayer::VideoScaleMode::Fit, mplayer::VideoScaleMode::Fill,
          mplayer::VideoScaleMode::Stretch, mplayer::VideoScaleMode::Aspect16x9,
          mplayer::VideoScaleMode::Aspect4x3, mplayer::VideoScaleMode::Original}) {
        scalingMode_->addItem(mplayer::videoScaleModeLabel(mode), static_cast<int>(mode));
    }
    const int modeIndex =
        scalingMode_->findData(static_cast<int>(storedScalePolicy.mode));
    scalingMode_->setCurrentIndex(modeIndex >= 0 ? modeIndex : 0);
    scalingMode_->setAccessibleName(tr("Video scaling mode"));
    scalingLayout->addWidget(scalingMode_);

    scalingZoomCaption_ = new QLabel(scalingCard);
    styleMutedCaption(scalingZoomCaption_);
    scalingLayout->addWidget(scalingZoomCaption_);

    scalingZoom_ = new QSpinBox(scalingCard);
    scalingZoom_->setObjectName(QStringLiteral("videoZoomSpin"));
    scalingZoom_->setRange(-50, 100);
    scalingZoom_->setSingleStep(5);
    scalingZoom_->setSuffix(QStringLiteral(" %"));
    scalingZoom_->setValue(storedScalePolicy.zoomPercent);
    scalingZoom_->setAccessibleName(tr("Video zoom"));
    polishInteractiveControl(scalingZoom_, theme::kSettingsControlSize);
    scalingLayout->addWidget(scalingZoom_);

    // Stretch ignores the percentage by definition, so the control says so by
    // being unusable rather than by quietly doing nothing.
    const auto syncZoomEnabled = [this] {
        const bool stretch =
            scalingMode_->currentData().toInt() ==
            static_cast<int>(mplayer::VideoScaleMode::Stretch);
        scalingZoom_->setEnabled(!stretch);
    };
    connect(scalingMode_, &QComboBox::currentIndexChanged, this, syncZoomEnabled);
    syncZoomEnabled();

    scalingLayout->addWidget(makeSettingsDivider(scalingCard));

    // The ids are what the pinned core accepts verbatim; the labels are the
    // viewer's. EWA Lanczos (soft) is the id we ship, not "auto": the core
    // refuses the literal auto for these options.
    const QVector<QPair<QString, QString>> scalerChoices{
        {QStringLiteral("bilinear"), tr("Bilinear (fast)")},
        {QStringLiteral("bicubic"), tr("Bicubic")},
        {QStringLiteral("catmull_rom"), tr("Catmull-Rom")},
        {QStringLiteral("lanczos"), tr("Lanczos (sharp)")},
        {QStringLiteral("ewa_lanczossoft"), tr("Ewa Lanczos (soft)")},
        {QStringLiteral("mitchell"), tr("Mitchell (smooth)")}};

    scalingUpscalerCaption_ = new QLabel(scalingCard);
    styleMutedCaption(scalingUpscalerCaption_);
    scalingLayout->addWidget(scalingUpscalerCaption_);

    scalingUpscaler_ = new QComboBox(scalingCard);
    scalingUpscaler_->setObjectName(QStringLiteral("videoUpscalerCombo"));
    polishInteractiveControl(scalingUpscaler_, theme::kSettingsControlSize);
    for (const auto& scaler : scalerChoices) {
        scalingUpscaler_->addItem(scaler.second, scaler.first);
    }
    const int upscalerIndex = scalingUpscaler_->findData(storedScalePolicy.upscale);
    scalingUpscaler_->setCurrentIndex(upscalerIndex >= 0 ? upscalerIndex : 0);
    scalingUpscaler_->setAccessibleName(tr("Upscaler"));
    scalingLayout->addWidget(scalingUpscaler_);

    scalingDownscalerCaption_ = new QLabel(scalingCard);
    styleMutedCaption(scalingDownscalerCaption_);
    scalingLayout->addWidget(scalingDownscalerCaption_);

    scalingDownscaler_ = new QComboBox(scalingCard);
    scalingDownscaler_->setObjectName(QStringLiteral("videoDownscalerCombo"));
    polishInteractiveControl(scalingDownscaler_, theme::kSettingsControlSize);
    // Auto is the empty id: mpv's own default is read from the core and
    // restored, because the pinned runtime refuses the literal "auto".
    scalingDownscaler_->addItem(tr("Auto (recommended)"), QString());
    for (const auto& scaler : scalerChoices) {
        scalingDownscaler_->addItem(scaler.second, scaler.first);
    }
    const int downscalerIndex =
        scalingDownscaler_->findData(storedScalePolicy.downscale);
    scalingDownscaler_->setCurrentIndex(downscalerIndex >= 0 ? downscalerIndex : 0);
    scalingDownscaler_->setAccessibleName(tr("Downscaler"));
    scalingLayout->addWidget(scalingDownscaler_);

    scalingComparisonHint_ = new QLabel(scalingCard);
    styleMutedCaption(scalingComparisonHint_);
    scalingLayout->addWidget(scalingComparisonHint_);

    root->addWidget(scalingCard);

    root->addStretch();
    scroll->setWidget(page);
    generalScroll_ = scroll;
    return scroll;
}

QFrame* SettingsDialog::buildSystemIntegrationCard(QWidget* parent) {
    QVBoxLayout* layout = nullptr;
    QFrame* card = makeSettingsCard(parent, &layout);
    layout->setSpacing(theme::kSpacingSmall);
    systemIntegrationTitle_ = new QLabel(card);
    styleSectionHeading(systemIntegrationTitle_);
    layout->addWidget(systemIntegrationTitle_);
    systemIntegrationHint_ = new QLabel(card);
    styleMutedCaption(systemIntegrationHint_);
    layout->addWidget(systemIntegrationHint_);
    layout->addWidget(makeSettingsDivider(card));

    // These two captions are whole sentences; a one-line check box would reserve
    // their full advance and widen the page past its own viewport.
    systemStartup_ = new WrappingCheckBox(card);
    systemStartup_->setObjectName(QStringLiteral("systemStartupCheck"));
    polishInteractiveControl(systemStartup_, theme::kSettingsControlSize);
    layout->addWidget(systemStartup_);
    systemStartupHint_ = new QLabel(card);
    styleMutedCaption(systemStartupHint_);
    layout->addWidget(systemStartupHint_);
    layout->addWidget(makeSettingsDivider(card));

    systemContextMenu_ = new WrappingCheckBox(card);
    systemContextMenu_->setObjectName(QStringLiteral("systemContextMenuCheck"));
    polishInteractiveControl(systemContextMenu_, theme::kSettingsControlSize);
    layout->addWidget(systemContextMenu_);
    systemContextMenuHint_ = new QLabel(card);
    styleMutedCaption(systemContextMenuHint_);
    layout->addWidget(systemContextMenuHint_);

    systemIntegrationError_ = new QLabel(card);
    systemIntegrationError_->setObjectName(QStringLiteral("systemIntegrationError"));
    systemIntegrationError_->setWordWrap(true);
    yieldCaptionWidth(systemIntegrationError_);
    styleStatusBanner(systemIntegrationError_, QStringLiteral("error"));
    systemIntegrationError_->hide();
    layout->addWidget(systemIntegrationError_);
    retranslateSystemIntegration();
    refreshSystemIntegrationControls();
    return card;
}

void SettingsDialog::refreshSystemIntegrationControls() {
    initialSystemStartup_ = systemStartupSupported() && systemStartupEnabled();
    initialSystemContextMenu_ = systemContextMenuSupported() && systemContextMenuEnabled();
    systemContextMenuWriteFailed_ = false;
    systemStartup_->setEnabled(systemStartupSupported());
    systemStartup_->setChecked(initialSystemStartup_);
    systemContextMenu_->setEnabled(systemContextMenuSupported());
    systemContextMenu_->setChecked(initialSystemContextMenu_);
    systemIntegrationError_->clear();
    systemIntegrationError_->hide();
}

void SettingsDialog::retranslateSystemIntegration() {
    if (systemIntegrationTitle_ == nullptr) return;
    systemIntegrationTitle_->setText(tr("System integration"));
#ifdef Q_OS_LINUX
    systemIntegrationHint_->setText(
        tr("Applies to your user account only. Changes take effect when you save."));
#else
    systemIntegrationHint_->setText(
        tr("Applies to your Windows account only. Changes take effect when you save."));
#endif
    systemStartup_->setText(tr("Start Mikai Ja Nai when I &sign in"));
    systemStartup_->setAccessibleName(tr("Start Mikai Ja Nai when I sign in"));
#ifdef Q_OS_LINUX
    const QString startupHint = tr("Open Mikai Ja Nai automatically after signing in. Disable this before moving a portable folder, then enable it again from the new location.");
#else
    const QString startupHint = systemStartupSupported()
                                    ? tr("Open Mikai Ja Nai automatically after signing in to Windows. "
                                         "Disable this before moving a portable folder, then enable "
                                         "it again from the new location.")
                                    : tr("Automatic startup is only available on Windows.");
#endif
    systemStartupHint_->setText(startupHint);
    systemStartup_->setToolTip(startupHint);
    systemStartup_->setAccessibleDescription(startupHint);
#ifdef Q_OS_LINUX
    systemContextMenu_->setText(tr("Add Mikai Ja Nai to video files' &Open With list"));
    systemContextMenu_->setAccessibleName(tr("Add Mikai Ja Nai to video files' Open With list"));
    const QString contextHint = tr("Registers Mikai Ja Nai as an Open With and default-player candidate without changing your defaults. Desktop file managers decide where to show it. If default-player registration was requested, disabling Open With retains the desktop identity so existing defaults continue to work. A package-managed entry cannot be disabled here.");
#else
    systemContextMenu_->setText(tr("Add Mikai Ja Nai to the video file &context menu"));
    systemContextMenu_->setAccessibleName(tr("Add Mikai Ja Nai to the video file context menu"));
    const QString contextHint =
        systemContextMenuSupported()
            ? tr("Add an Open in Mikai Ja Nai command for video files without changing their default app. "
                 "After moving a portable folder, enable this again to update its path.")
            : tr("The video file context menu integration is only available on Windows.");
#endif
    systemContextMenuHint_->setText(contextHint);
    systemContextMenu_->setToolTip(contextHint);
    systemContextMenu_->setAccessibleDescription(contextHint);
    if (!systemStartupSupported() && !systemContextMenuSupported()) {
        systemIntegrationHint_->setText(
            tr("These system integrations are only available on Windows and are disabled here."));
    }
}

void SettingsDialog::showSystemIntegrationError(const QString& message) {
    systemIntegrationError_->setText(message);
    systemIntegrationError_->show();
    showPage(QStringLiteral("customization/general"));
    if (customizationGeneralScroll_ != nullptr) {
        customizationGeneralScroll_->ensureWidgetVisible(systemIntegrationError_);
    }
    QAccessibleEvent alert(systemIntegrationError_, QAccessible::Alert);
    QAccessible::updateAccessibility(&alert);
}

bool SettingsDialog::saveSystemIntegrations() {
    const bool changeStartup = systemStartup_->isEnabled() &&
                               systemStartup_->isChecked() != initialSystemStartup_;
    const bool changeContextMenu = systemContextMenu_->isEnabled() &&
                                   (systemContextMenu_->isChecked() != initialSystemContextMenu_ ||
                                    systemContextMenuWriteFailed_);
    if (!changeStartup && !changeContextMenu) return true;

    // Capture the live state, not just the opening state: another process may
    // have changed the registration while this dialog was open.
    const bool previousStartup = systemStartupEnabled();
    QString reason;
    if (changeStartup && !setSystemStartupEnabled(systemStartup_->isChecked(), &reason)) {
        showSystemIntegrationError(
            tr("Automatic startup could not be saved. Settings remain open with your edits: %1")
                .arg(reason));
        return false;
    }
    if (changeContextMenu &&
        !setSystemContextMenuEnabled(systemContextMenu_->isChecked(), &reason)) {
        // The backend attempts to restore its exact prior values. If rollback
        // itself failed, a partial registration may not be representable as a
        // bool. Keep this attempted edit dirty even if the user unchecks it,
        // so a later Save explicitly applies that choice rather than accepting
        // unknown partial state. Only undo the earlier startup change here.
        systemContextMenuWriteFailed_ = true;
        QString rollbackReason;
        const bool restored =
            !changeStartup || setSystemStartupEnabled(previousStartup, &rollbackReason);
        QString message = tr("The video context menu could not be saved. Settings remain open "
                             "with your edits: %1")
                              .arg(reason);
        if (!restored) {
            initialSystemStartup_ = systemStartupEnabled();
            message += tr("\nAutomatic startup could not be restored: %1. Its current state is %2.")
                           .arg(rollbackReason,
                                initialSystemStartup_ ? tr("enabled") : tr("disabled"));
        }
        showSystemIntegrationError(message);
        return false;
    }
    initialSystemStartup_ = systemStartupEnabled();
    initialSystemContextMenu_ = systemContextMenuEnabled();
    systemContextMenuWriteFailed_ = false;
    return true;
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
    styleStatusBanner(status_, QStringLiteral("info"));
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
                styleStatusBanner(self->status_, QStringLiteral("success"));
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
                styleStatusBanner(self->status_, QStringLiteral("danger"));
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
            styleStatusBanner(self->status_, QStringLiteral("danger"));
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

void SettingsDialog::retranslateGeneralTab() {
    tokenHeading_->setText(tr("Mikai API access token"));
    tokenDescription_->setText(
        tr("An API key raises the limit from %1 to %2 requests per minute and is stored "
           "locally in the .env file.")
            .arg(backend::kApiAnonymousRequestsPerMinute)
            .arg(backend::kApiKeyedRequestsPerMinute));
    tokenLink_->setText(tr("<a style=\"color: %1; text-decoration: none; font-weight: 500;\" "
                           "href=\"https://mikai.me/user/developers\">"
                           "Get a key from the developer dashboard on mikai.me ↗</a>")
                            .arg(theme::paletteFor(currentAppearance()).accent));
    token_->setPlaceholderText(tr("mk_... (leave blank for anonymous access)"));
    token_->setAccessibleName(tr("Mikai API access token"));
    testButton_->setText(tr("Test"));
    testButton_->setAccessibleName(tr("Test API token"));
    openSubtitlesTitle_->setText(tr("OpenSubtitles"));
    openSubtitlesHint_->setText(
        tr("Enter your OpenSubtitles API key to search for subtitles in the player."));
    openSubtitlesLink_->setText(
        tr("<a style=\"color: %1; text-decoration: none; font-weight: 500;\" "
           "href=\"https://www.opensubtitles.com/en/consumers\">"
           "Get a key from the OpenSubtitles API consumers page ↗</a>")
            .arg(theme::paletteFor(currentAppearance()).accent));
    openSubtitlesKey_->setPlaceholderText(tr("OpenSubtitles API key"));
    openSubtitlesKey_->setAccessibleName(tr("OpenSubtitles API key"));

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
    languageHint_->setText(
        tr("Applied immediately to the running windows, including the open player."));
    const auto restateLanguage = [this](const QString& data, const QString& caption) {
        const int index = language_->findData(data);
        if (index >= 0) language_->setItemText(index, caption);
    };
    restateLanguage(QStringLiteral("system"), tr("Use system language"));
    restateLanguage(QStringLiteral("en"), tr("English"));
    restateLanguage(QStringLiteral("uk"), tr("Ukrainian"));
    language_->setAccessibleName(tr("Interface language"));

    updateNotifications_->setText(tr("Check for updates automatically"));
    updateNotificationsHint_->setText(
        tr("Checks for a newer stable release using one unauthenticated request to GitHub. "
           "No update is downloaded or installed."));
    playbackTitle_->setText(tr("HDR playback"));
    playbackHint_->setText(
        tr("HDR tone mapping controls how HDR video is mapped to the display. "
           "It is separate from Native enhancement; saved changes reach a running player "
           "within a moment."));
    const auto restatePolicy = [this](int data, const QString& caption) {
        const int index = hdrPolicy_->findData(data);
        if (index >= 0) hdrPolicy_->setItemText(index, caption);
    };
    restatePolicy(0, tr("Auto (recommended)"));
    restatePolicy(1, tr("Stable - no per-frame adaptation"));
    restatePolicy(2, tr("Vivid - Mobius curve"));
    hdrPolicy_->setAccessibleName(tr("HDR tone mapping"));
    nativeHdrOverride_->setText(tr("Native enhancement for HDR"));
    nativeHdrOverrideHint_->setText(
        tr("Native enhancement stages require 8-bit NV12 and are normally skipped for HDR "
           "sources. Enabling this allows that processing on HDR video; it does not convert "
           "the source to SDR, and may reduce HDR image quality."));
    autoAdvance_->setText(tr("Automatically play the next episode"));

    scalingTitle_->setText(tr("Video scaling"));
    scalingHint_->setText(
        tr("Controls how the video is mapped to the window and resampled for display. "
           "Does not change AI upscaling; AnimeJaNai processes run at their own resolution."));
    scalingComparisonHint_->setText(
        tr("Applies to the player immediately; Compare always uses Fit so panes remain "
           "comparable."));
    scalingZoomCaption_->setText(tr("Zoom"));
    scalingUpscalerCaption_->setText(tr("Upscaler"));
    scalingDownscalerCaption_->setText(tr("Downscaler"));
    scalingZoom_->setAccessibleName(tr("Video zoom"));
    scalingMode_->setAccessibleName(tr("Video scaling mode"));
    scalingUpscaler_->setAccessibleName(tr("Upscaler"));
    scalingDownscaler_->setAccessibleName(tr("Downscaler"));
    // The mode labels come from one shared helper, so only the row order has to
    // be restated here.
    const auto restateScaler = [](QComboBox* combo,
                                  const QVector<QPair<QString, QString>>& labels) {
        for (const auto& entry : labels) {
            const int index = combo->findData(entry.first);
            if (index >= 0) combo->setItemText(index, entry.second);
        }
    };
    const QVector<QPair<QString, QString>> scalerLabels{
        {QStringLiteral("bilinear"), tr("Bilinear (fast)")},
        {QStringLiteral("bicubic"), tr("Bicubic")},
        {QStringLiteral("catmull_rom"), tr("Catmull-Rom")},
        {QStringLiteral("lanczos"), tr("Lanczos (sharp)")},
        {QStringLiteral("ewa_lanczossoft"), tr("Ewa Lanczos (soft)")},
        {QStringLiteral("mitchell"), tr("Mitchell (smooth)")}};
    restateScaler(scalingUpscaler_, scalerLabels);
    restateScaler(scalingDownscaler_, scalerLabels);
    const int autoIndex = scalingDownscaler_->findData(QString());
    if (autoIndex >= 0) scalingDownscaler_->setItemText(autoIndex, tr("Auto (recommended)"));
    for (int i = 0; i < scalingMode_->count(); ++i) {
        const auto mode = static_cast<mplayer::VideoScaleMode>(scalingMode_->itemData(i).toInt());
        scalingMode_->setItemText(i, mplayer::videoScaleModeLabel(mode));
    }

    refreshQuota();
}

}  // namespace mikai::app
