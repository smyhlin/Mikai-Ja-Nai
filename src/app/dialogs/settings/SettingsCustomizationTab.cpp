#include "dialogs/SettingsDialog.hpp"

#include "dialogs/settings/SettingsUi.hpp"
#include "theme/Appearance.hpp"
#include "theme/Metrics.hpp"
#include "theme/Palette.hpp"
#include "theme/Theme.hpp"
#include <player/shell/ShellTheme.hpp>

#include "widgets/FlowLayout.hpp"
#include "widgets/SwatchButton.hpp"
#include "widgets/ToggleSwitch.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QPointer>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace mikai::app {
namespace {

QString groupCaption(const QString& group) {
    if (group == QStringLiteral("customization")) return SettingsDialog::tr("Customization");
    if (group == QStringLiteral("playback")) return SettingsDialog::tr("Playback");
    return SettingsDialog::tr("Application");
}

QString entryCaption(const QString& id) {
    if (id == QStringLiteral("customization/general")) return SettingsDialog::tr("General");
    if (id == QStringLiteral("customization/appearance")) return SettingsDialog::tr("Appearance");
    if (id == QStringLiteral("access")) return SettingsDialog::tr("Access");
    if (id == QStringLiteral("shortcuts")) return SettingsDialog::tr("Keyboard shortcuts");
    if (id == QStringLiteral("processing")) return SettingsDialog::tr("Processing");
    if (id == QStringLiteral("components")) return SettingsDialog::tr("Components");
    if (id == QStringLiteral("benchmark")) return SettingsDialog::tr("Benchmark");
    return QString();
}

QComboBox* makeChoice(QWidget* parent, const QString& name) {
    auto* combo = new QComboBox(parent);
    combo->setObjectName(name);
    combo->setMinimumHeight(theme::kSettingsControlSize);
    return combo;
}

QLabel* makeFieldLabel(QWidget* parent, const QString& text) {
    auto* label = new QLabel(text, parent);
    styleSectionHeading(label);
    return label;
}

QLabel* makeFieldHint(QWidget* parent, const QString& text) {
    auto* hint = new QLabel(text, parent);
    styleMutedCaption(hint);
    // A hint must never widen the page: its own text wraps instead.
    hint->setWordWrap(true);
    return hint;
}

}  // namespace

QPushButton* SettingsDialog::addNavigationEntry(const QString& id, const QString& group,
                                                QWidget* page) {
    if (navStack_ == nullptr || page == nullptr) return nullptr;
    if (!navGroupLabels_.contains(group)) {
        auto* caption = new QLabel(groupCaption(group), navPanel_);
        caption->setObjectName(QStringLiteral("settingsNavGroup"));
        navLayout_->insertWidget(navLayout_->count() - 1, caption);
        navGroupLabels_.insert(group, caption);
    }
    auto* button = new QPushButton(entryCaption(id), navPanel_);
    button->setObjectName(QStringLiteral("settingsNavItem"));
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumHeight(theme::kSettingsControlSize);
    navLayout_->insertWidget(navLayout_->count() - 1, button);
    navStack_->addWidget(page);
    navEntries_.append({id, group, page, button});
    connect(button, &QPushButton::clicked, this, [this, page] { selectPage(page); });
    return button;
}

void SettingsDialog::buildNavigation() {
    auto* body = new QWidget(this);
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(theme::kSpacingLarge);

    navPanel_ = new QWidget(body);
    navPanel_->setObjectName(QStringLiteral("settingsNav"));
    navPanel_->setFixedWidth(theme::kSettingsNavWidth);
    navLayout_ = new QVBoxLayout(navPanel_);
    navLayout_->setContentsMargins(0, 0, 0, 0);
    navLayout_->setSpacing(theme::kSpacingXs);
    navLayout_->addStretch(1);
    bodyLayout->addWidget(navPanel_, 0, Qt::AlignTop);

    navStack_ = new QStackedWidget(body);
    navStack_->setObjectName(QStringLiteral("settingsStack"));
    bodyLayout->addWidget(navStack_, 1);

    if (auto* root = qobject_cast<QVBoxLayout*>(layout())) {
        root->insertWidget(0, body, 1);
    }

    addNavigationEntry(QStringLiteral("customization/general"), QStringLiteral("customization"),
                       customizationGeneralPage_);
    addNavigationEntry(QStringLiteral("customization/appearance"), QStringLiteral("customization"),
                       appearancePage_);
    addNavigationEntry(QStringLiteral("access"), QStringLiteral("application"), generalTab_);
    addNavigationEntry(QStringLiteral("shortcuts"), QStringLiteral("application"), presetsTab_);
    addNavigationEntry(QStringLiteral("processing"), QStringLiteral("playback"), processingTab_);
    addNavigationEntry(QStringLiteral("components"), QStringLiteral("playback"), componentsTab_);
    addNavigationEntry(QStringLiteral("benchmark"), QStringLiteral("playback"), benchmarkTab_);
    selectPage(customizationGeneralPage_);
}

void SettingsDialog::selectPage(QWidget* page) {
    if (navStack_ == nullptr || page == nullptr) return;
    for (const NavEntry& entry : navEntries_) {
        if (entry.button != nullptr) {
            QSignalBlocker blocker(entry.button);
            entry.button->setChecked(entry.page == page);
        }
    }
    navStack_->setCurrentWidget(page);
    if (page == componentsTab_) emit componentsTabOpened(this);
    if (page == benchmarkTab_) refreshBenchmarkCatalogue();
}

bool SettingsDialog::showPage(const QString& id) {
    for (const NavEntry& entry : navEntries_) {
        if (entry.id != id) continue;
        selectPage(entry.page);
        return true;
    }
    return false;
}

QString SettingsDialog::currentPageId() const {
    QWidget* current = navStack_ != nullptr ? navStack_->currentWidget() : nullptr;
    for (const NavEntry& entry : navEntries_) {
        if (entry.page == current) return entry.id;
    }
    return QString();
}

QStringList SettingsDialog::pageIds() const {
    QStringList ids;
    ids.reserve(navEntries_.size());
    for (const NavEntry& entry : navEntries_) ids.append(entry.id);
    return ids;
}

void SettingsDialog::retranslateNavigation() {
    for (auto it = navGroupLabels_.constBegin(); it != navGroupLabels_.constEnd(); ++it) {
        it.value()->setText(groupCaption(it.key()));
    }
    for (const NavEntry& entry : navEntries_) {
        if (entry.button != nullptr) entry.button->setText(entryCaption(entry.id));
    }
}

QWidget* SettingsDialog::buildCustomizationGeneralTab() {
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("customizationGeneralScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    customizationGeneralScroll_ = scroll;

    auto* page = new QWidget(scroll);
    page->setObjectName(QStringLiteral("customizationGeneralPage"));
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(theme::kSpacingSmall, theme::kSpacingMedium, theme::kSpacingSmall,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    root->addWidget(makeSettingsPageHeader(
        page, &customizationHeading_, &customizationDescription_, tr("General"),
        tr("Adjust how content and more are displayed")));

    QVBoxLayout* cardLayout = nullptr;
    QFrame* card = makeSettingsCard(page, &cardLayout);
    cardLayout->setSpacing(theme::kSpacingMedium);

    siteThemeLabel_ = makeFieldLabel(card, tr("Site theme"));
    siteTheme_ = makeChoice(card, QStringLiteral("siteThemeCombo"));
    siteTheme_->addItem(tr("Dark theme"), QStringLiteral("dark"));
    siteTheme_->addItem(tr("Light theme"), QStringLiteral("light"));
    cardLayout->addWidget(siteThemeLabel_);
    cardLayout->addWidget(siteTheme_);
    connect(siteTheme_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (siteTheme_ == nullptr || index < 0) return;
        appearanceDraft_.siteTheme =
            theme::themeFromId(siteTheme_->itemData(index).toString());
        applyAppearancePreview();
    });

    titleLanguageLabel_ = makeFieldLabel(card, tr("Content titles language"));
    titleLanguage_ = makeChoice(card, QStringLiteral("titleLanguageCombo"));
    titleLanguage_->addItem(tr("Ukrainian"), QStringLiteral("uk"));
    titleLanguage_->addItem(tr("English"), QStringLiteral("en"));
    titleLanguage_->addItem(tr("Original"), QStringLiteral("original"));
    cardLayout->addWidget(titleLanguageLabel_);
    cardLayout->addWidget(titleLanguage_);
    connect(titleLanguage_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (titleLanguage_ == nullptr || index < 0) return;
        appearanceDraft_.titleLanguage =
            theme::titleLanguageFromId(titleLanguage_->itemData(index).toString());
        applyAppearancePreview();
    });

    nameLanguageLabel_ = makeFieldLabel(card, tr("Names language"));
    nameLanguage_ = makeChoice(card, QStringLiteral("nameLanguageCombo"));
    nameLanguage_->addItem(tr("As the interface"), QStringLiteral("interface"));
    nameLanguage_->addItem(tr("Ukrainian"), QStringLiteral("uk"));
    nameLanguage_->addItem(tr("English"), QStringLiteral("en"));
    nameLanguage_->addItem(tr("Original"), QStringLiteral("original"));
    cardLayout->addWidget(nameLanguageLabel_);
    cardLayout->addWidget(nameLanguage_);
    connect(nameLanguage_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (nameLanguage_ == nullptr || index < 0) return;
        appearanceDraft_.nameLanguage =
            theme::nameLanguageFromId(nameLanguage_->itemData(index).toString());
        applyAppearancePreview();
    });
    root->addWidget(card);

    QVBoxLayout* displayLayout = nullptr;
    QFrame* displayCard = makeSettingsCard(page, &displayLayout);
    displayLayout->setSpacing(theme::kSpacingSmall);

    const auto addToggleRow = [displayCard, displayLayout](const QString& title,
                                                           const QString& hint,
                                                           const QString& name, QLabel** titleOut,
                                                           QLabel** hintOut) {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(theme::kSpacingMedium);
        auto* column = new QVBoxLayout();
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(theme::kSpacingXs);
        auto* heading = new QLabel(title, displayCard);
        styleSectionHeading(heading);
        auto* description = new QLabel(hint, displayCard);
        styleMutedCaption(description);
        column->addWidget(heading);
        column->addWidget(description);
        auto* toggle = new ToggleSwitch(displayCard);
        toggle->setObjectName(name);
        toggle->setAccessibleName(title);
        row->addLayout(column, 1);
        row->addWidget(toggle, 0, Qt::AlignVCenter);
        displayLayout->addLayout(row);
        *titleOut = heading;
        *hintOut = description;
        return toggle;
    };

    coverGradientSwitch_ = addToggleRow(tr("Gradient overlay"),
                                        tr("Overlay a gradient on the cover image"),
                                        QStringLiteral("coverGradientSwitch"),
                                        &coverGradientLabel_, &coverGradientHint_);
    connect(coverGradientSwitch_, &QAbstractButton::toggled, this, [this](bool on) {
        appearanceDraft_.coverGradient = on;
        applyAppearancePreview();
    });
    displayLayout->addWidget(makeSettingsDivider(displayCard));
    adultContentSwitch_ = addToggleRow(tr("18+ content"),
                                       tr("Show explicit content without a warning"),
                                       QStringLiteral("adultContentSwitch"), &adultContentLabel_,
                                       &adultContentHint_);
    connect(adultContentSwitch_, &QAbstractButton::toggled, this, [this](bool on) {
        appearanceDraft_.showAdult = on;
        applyAppearancePreview();
    });
    root->addWidget(displayCard);
    root->addWidget(buildSystemIntegrationCard(page));
    root->addStretch(1);

    scroll->setWidget(page);
    refreshAppearanceControls();
    return scroll;
}

QWidget* SettingsDialog::buildAppearanceTab() {
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("appearanceScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    appearanceScroll_ = scroll;

    auto* page = new QWidget(scroll);
    page->setObjectName(QStringLiteral("appearancePage"));
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(theme::kSpacingSmall, theme::kSpacingMedium, theme::kSpacingSmall,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    auto* header = new QFrame(page);
    header->setObjectName(QStringLiteral("settingsPageHeader"));
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium,
                                     theme::kSpacingMedium, theme::kSpacingSmall);
    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(theme::kSpacingMedium);
    auto* headerColumn = new QVBoxLayout();
    headerColumn->setContentsMargins(0, 0, 0, 0);
    headerColumn->setSpacing(theme::kSpacingXs);
    appearanceHeading_ = new QLabel(tr("Appearance"), header);
    stylePageHeading(appearanceHeading_);
    appearanceDescription_ = new QLabel(tr("Adjust themes, colors and display"), header);
    styleMutedCaption(appearanceDescription_);
    headerColumn->addWidget(appearanceHeading_);
    headerColumn->addWidget(appearanceDescription_);
    headerRow->addLayout(headerColumn, 1);
    resetAppearanceButton_ = new QPushButton(tr("Reset"), header);
    resetAppearanceButton_->setObjectName(QStringLiteral("resetAppearanceButton"));
    polishInteractiveControl(resetAppearanceButton_, theme::kSettingsControlSize);
    headerRow->addWidget(resetAppearanceButton_, 0, Qt::AlignTop);
    headerLayout->addLayout(headerRow);
    root->addWidget(header);

    QVBoxLayout* accentLayout = nullptr;
    QFrame* accentCard = makeSettingsCard(page, &accentLayout);
    accentSectionLabel_ = makeFieldLabel(accentCard, tr("Accent color"));
    accentSectionHint_ = makeFieldHint(accentCard, tr("Pick a preset or a custom accent color"));
    accentLayout->addWidget(accentSectionLabel_);
    accentLayout->addWidget(accentSectionHint_);
    auto* accentRow = new FlowLayout(theme::kSpacingSmall, theme::kSpacingSmall);
    accentRow->setContentsMargins(0, theme::kSpacingXs, 0, 0);
    for (const theme::AccentPreset& preset : theme::accentPresets()) {
        auto* swatch = new SwatchButton(preset.color, accentCard);
        swatch->setObjectName(QStringLiteral("accentSwatch"));
        swatch->setCheckable(true);
        swatch->setAutoExclusive(true);
        swatch->setFixedSize(theme::kSettingsSwatchSize, theme::kSettingsSwatchSize);
        swatch->setCursor(Qt::PointingHandCursor);
        const QString readable = theme::presetName(preset.color);
        swatch->setAccessibleName(readable.isEmpty() ? preset.color
                                                     : tr("%1 accent").arg(readable));
        swatch->setToolTip(preset.color);
        const QString color = preset.color;
        connect(swatch, &QPushButton::clicked, this, [this, color] {
            appearanceDraft_.accent = color;
            applyAppearancePreview();
        });
        accentRow->addWidget(swatch);
        accentSwatches_.append({color, swatch});
    }
    customAccentButton_ = new SwatchButton(QColor(), accentCard);
    customAccentButton_->setSpectrum(true);
    customAccentButton_->setObjectName(QStringLiteral("customAccentSwatch"));
    customAccentButton_->setFixedSize(theme::kSettingsSwatchSize, theme::kSettingsSwatchSize);
    customAccentButton_->setCursor(Qt::PointingHandCursor);
    customAccentButton_->setAccessibleName(tr("Custom accent color"));
    customAccentButton_->setToolTip(tr("Pick a custom accent color"));
    connect(customAccentButton_, &QPushButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(theme::accentColor(appearanceDraft_), this,
                                                     tr("Custom accent color"));
        if (!chosen.isValid()) return;
        appearanceDraft_.accent = theme::hexOf(chosen);
        applyAppearancePreview();
    });
    accentRow->addWidget(customAccentButton_);
    accentValueLabel_ = new QLabel(accentCard);
    accentValueLabel_->setObjectName(QStringLiteral("accentValueLabel"));
    accentValueLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    applyTabularNumerals(accentValueLabel_);
    accentRow->addWidget(accentValueLabel_);
    accentLayout->addLayout(accentRow);
    root->addWidget(accentCard);

    QVBoxLayout* radiusLayout = nullptr;
    QFrame* radiusCard = makeSettingsCard(page, &radiusLayout);
    radiusSectionLabel_ = makeFieldLabel(radiusCard, tr("Corner radius"));
    radiusSectionHint_ = makeFieldHint(radiusCard, tr("Rounding of buttons, cards and fields"));
    radiusLayout->addWidget(radiusSectionLabel_);
    radiusLayout->addWidget(radiusSectionHint_);
    auto* radiusRow = new FlowLayout(theme::kSpacingXs, theme::kSpacingXs);
    radiusRow->setContentsMargins(0, theme::kSpacingXs, 0, 0);
    const QStringList radiusLabels = theme::radiusNames();
    for (int step = 0; step < radiusLabels.size(); ++step) {
        auto* chip = new QPushButton(radiusLabels.at(step), radiusCard);
        chip->setObjectName(QStringLiteral("radiusChip"));
        chip->setCheckable(true);
        chip->setAutoExclusive(true);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setMinimumHeight(theme::kSettingsControlSize);
        chip->setAccessibleName(radiusLabels.at(step));
        chip->setToolTip(tr("%1 px").arg(theme::radiusSteps().value(step)));
        connect(chip, &QPushButton::clicked, this, [this, step] {
            appearanceDraft_.radiusStep = step;
            applyAppearancePreview();
        });
        radiusRow->addWidget(chip);
        radiusButtons_.append(chip);
    }
    radiusLayout->addLayout(radiusRow);
    root->addWidget(radiusCard);

    QVBoxLayout* gradientLayout = nullptr;
    QFrame* gradientCard = makeSettingsCard(page, &gradientLayout);
    gradientLayout->setSpacing(theme::kSpacingSmall);
    auto* gradientRow = new QHBoxLayout();
    gradientRow->setContentsMargins(0, 0, 0, 0);
    gradientRow->setSpacing(theme::kSpacingMedium);
    auto* gradientColumn = new QVBoxLayout();
    gradientColumn->setContentsMargins(0, 0, 0, 0);
    gradientColumn->setSpacing(theme::kSpacingXs);
    backgroundGradientLabel_ = new QLabel(tr("Background gradient"), gradientCard);
    styleSectionHeading(backgroundGradientLabel_);
    backgroundGradientHint_ = new QLabel(tr("Accent glow at the top of the page"), gradientCard);
    styleMutedCaption(backgroundGradientHint_);
    gradientColumn->addWidget(backgroundGradientLabel_);
    gradientColumn->addWidget(backgroundGradientHint_);
    gradientRow->addLayout(gradientColumn, 1);
    auto* backgroundSwitch = new ToggleSwitch(gradientCard);
    backgroundSwitch->setObjectName(QStringLiteral("backgroundGradientSwitch"));
    backgroundSwitch->setAccessibleName(tr("Background gradient"));
    gradientRow->addWidget(backgroundSwitch, 0, Qt::AlignVCenter);
    gradientLayout->addLayout(gradientRow);
    QPointer<ToggleSwitch> background = backgroundSwitch;
    connect(background, &QAbstractButton::toggled, this, [this](bool on) {
        appearanceDraft_.backgroundGradient = on;
        applyAppearancePreview();
    });

    gradientLayout->addWidget(makeSettingsDivider(gradientCard));

    const auto addSliderRow = [gradientCard, gradientLayout](const QString& caption,
                                                             QSlider** sliderOut,
                                                             QLabel** valueOut) {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(theme::kSpacingSmall);
        auto* label = new QLabel(caption, gradientCard);
        styleSectionHeading(label);
        row->addWidget(label, 1);
        auto* value = new QLabel(gradientCard);
        value->setObjectName(QStringLiteral("appearanceValue"));
        value->setFixedWidth(theme::kSettingsSliderValueWidth);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        applyTabularNumerals(value);
        row->addWidget(value, 0);
        gradientLayout->addLayout(row);
        auto* slider = new QSlider(Qt::Horizontal, gradientCard);
        slider->setObjectName(QStringLiteral("appearanceSlider"));
        slider->setRange(0, 100);
        slider->setSingleStep(1);
        slider->setPageStep(10);
        slider->setMinimumHeight(theme::kSettingsControlSize);
        gradientLayout->addWidget(slider);
        *sliderOut = slider;
        *valueOut = value;
        return label;
    };

    intensityLabel_ = addSliderRow(tr("Intensity"), &intensitySlider_, &intensityValueLabel_);
    heightLabel_ = addSliderRow(tr("Height"), &heightSlider_, &heightValueLabel_);
    connect(intensitySlider_, &QSlider::valueChanged, this, [this](int value) {
        appearanceDraft_.gradientIntensity = value;
        if (intensityValueLabel_ != nullptr) {
            intensityValueLabel_->setText(QStringLiteral("%1%").arg(value));
        }
        applyAppearancePreview();
    });
    connect(heightSlider_, &QSlider::valueChanged, this, [this](int value) {
        appearanceDraft_.gradientHeight = value;
        if (heightValueLabel_ != nullptr) {
            heightValueLabel_->setText(QStringLiteral("%1%").arg(value));
        }
        applyAppearancePreview();
    });

    gradientLayout->addWidget(makeSettingsDivider(gradientCard));
    gradientColorLabel_ = makeFieldLabel(gradientCard, tr("Gradient color"));
    gradientLayout->addWidget(gradientColorLabel_);
    auto* colorRow = new FlowLayout(theme::kSpacingSmall, theme::kSpacingSmall);
    for (const theme::AccentPreset& preset : theme::accentPresets()) {
        auto* swatch = new SwatchButton(preset.color, gradientCard);
        swatch->setObjectName(QStringLiteral("gradientSwatch"));
        swatch->setCheckable(true);
        swatch->setAutoExclusive(true);
        swatch->setFixedSize(theme::kSettingsSwatchSize, theme::kSettingsSwatchSize);
        swatch->setCursor(Qt::PointingHandCursor);
        const QString readable = theme::presetName(preset.color);
        swatch->setAccessibleName(readable.isEmpty() ? preset.color
                                                     : tr("%1 gradient").arg(readable));
        swatch->setToolTip(preset.color);
        const QString color = preset.color;
        connect(swatch, &QPushButton::clicked, this, [this, color] {
            appearanceDraft_.gradientColor = color;
            applyAppearancePreview();
        });
        colorRow->addWidget(swatch);
        gradientSwatches_.append({color, swatch});
    }
    customGradientButton_ = new SwatchButton(QColor(), gradientCard);
    customGradientButton_->setSpectrum(true);
    customGradientButton_->setObjectName(QStringLiteral("customGradientSwatch"));
    customGradientButton_->setFixedSize(theme::kSettingsSwatchSize, theme::kSettingsSwatchSize);
    customGradientButton_->setCursor(Qt::PointingHandCursor);
    customGradientButton_->setAccessibleName(tr("Custom gradient color"));
    customGradientButton_->setToolTip(tr("Pick a custom gradient color"));
    connect(customGradientButton_, &QPushButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(theme::gradientColor(appearanceDraft_), this,
                                                     tr("Custom gradient color"));
        if (!chosen.isValid()) return;
        appearanceDraft_.gradientColor = theme::hexOf(chosen);
        applyAppearancePreview();
    });
    colorRow->addWidget(customGradientButton_);
    gradientValueLabel_ = new QLabel(gradientCard);
    gradientValueLabel_->setObjectName(QStringLiteral("gradientValueLabel"));
    gradientValueLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    applyTabularNumerals(gradientValueLabel_);
    colorRow->addWidget(gradientValueLabel_);
    gradientLayout->addLayout(colorRow);
    root->addWidget(gradientCard);
    root->addStretch(1);

    connect(resetAppearanceButton_, &QPushButton::clicked, this, [this] {
        const theme::Appearance shipped;
        appearanceDraft_.accent = shipped.accent;
        appearanceDraft_.radiusStep = shipped.radiusStep;
        appearanceDraft_.backgroundGradient = shipped.backgroundGradient;
        appearanceDraft_.gradientIntensity = shipped.gradientIntensity;
        appearanceDraft_.gradientHeight = shipped.gradientHeight;
        appearanceDraft_.gradientColor = shipped.gradientColor;
        applyAppearancePreview();
    });

    scroll->setWidget(page);
    refreshAppearanceControls();
    return scroll;
}

void SettingsDialog::refreshAppearanceCaptions() {
    const QString accent = theme::hexOf(theme::accentColor(appearanceDraft_));
    const QString name = theme::presetName(accent);
    if (accentValueLabel_ != nullptr) {
        accentValueLabel_->setText(name.isEmpty() ? accent : tr("%1 %2").arg(name, accent));
    }
    const QString gradient = theme::hexOf(theme::gradientColor(appearanceDraft_));
    const QString gradientName = theme::presetName(gradient);
    if (gradientValueLabel_ != nullptr) {
        gradientValueLabel_->setText(gradientName.isEmpty()
                                         ? gradient
                                         : tr("%1 %2").arg(gradientName, gradient));
    }
}

void SettingsDialog::refreshAppearanceControls() {
    if (appearancePage_ == nullptr) return;
    const QString accent = theme::hexOf(theme::accentColor(appearanceDraft_));
    const QString gradient = theme::hexOf(theme::gradientColor(appearanceDraft_));
    const auto matches = [](const auto& swatch, const QString& value) {
        return swatch.first.compare(value, Qt::CaseInsensitive) == 0;
    };

    for (auto& swatch : accentSwatches_) {
        const bool selected = matches(swatch, accent);
        QSignalBlocker blocker(swatch.second);
        swatch.second->setChecked(selected);
        swatch.second->setSelected(selected);
    }
    if (customAccentButton_ != nullptr) {
        const bool custom = std::none_of(accentSwatches_.cbegin(), accentSwatches_.cend(),
                                         [&](const auto& swatch) { return matches(swatch, accent); });
        customAccentButton_->setSelected(custom);
    }
    for (int step = 0; step < radiusButtons_.size(); ++step) {
        QSignalBlocker blocker(radiusButtons_.at(step));
        radiusButtons_.at(step)->setChecked(step == appearanceDraft_.radiusStep);
    }
    for (auto& swatch : gradientSwatches_) {
        const bool selected = matches(swatch, gradient);
        QSignalBlocker blocker(swatch.second);
        swatch.second->setChecked(selected);
        swatch.second->setSelected(selected);
    }
    if (customGradientButton_ != nullptr) {
        const bool custom =
            std::none_of(gradientSwatches_.cbegin(), gradientSwatches_.cend(),
                         [&](const auto& swatch) { return matches(swatch, gradient); });
        customGradientButton_->setSelected(custom);
    }

    const auto syncChoice = [](QComboBox* combo, const QString& value) {
        if (combo == nullptr) return;
        const int index = combo->findData(value);
        if (index < 0 || index == combo->currentIndex()) return;
        QSignalBlocker blocker(combo);
        combo->setCurrentIndex(index);
    };
    syncChoice(siteTheme_, theme::themeId(appearanceDraft_.siteTheme));
    syncChoice(titleLanguage_, theme::languageId(appearanceDraft_.titleLanguage));
    syncChoice(nameLanguage_, theme::nameLanguageId(appearanceDraft_.nameLanguage));

    const auto syncSwitch = [](QAbstractButton* button, bool on) {
        if (button == nullptr) return;
        QSignalBlocker blocker(button);
        button->setChecked(on);
    };
    syncSwitch(coverGradientSwitch_, appearanceDraft_.coverGradient);
    syncSwitch(adultContentSwitch_, appearanceDraft_.showAdult);
    if (appearancePage_ != nullptr) {
        syncSwitch(appearancePage_->findChild<ToggleSwitch*>(
                       QStringLiteral("backgroundGradientSwitch")),
                   appearanceDraft_.backgroundGradient);
    }
    if (intensitySlider_ != nullptr) {
        QSignalBlocker blocker(intensitySlider_);
        intensitySlider_->setValue(appearanceDraft_.gradientIntensity);
    }
    if (heightSlider_ != nullptr) {
        QSignalBlocker blocker(heightSlider_);
        heightSlider_->setValue(appearanceDraft_.gradientHeight);
    }
    if (intensityValueLabel_ != nullptr) {
        intensityValueLabel_->setText(QStringLiteral("%1%").arg(appearanceDraft_.gradientIntensity));
    }
    if (heightValueLabel_ != nullptr) {
        heightValueLabel_->setText(QStringLiteral("%1%").arg(appearanceDraft_.gradientHeight));
    }
    const bool gradientOn = appearanceDraft_.backgroundGradient;
    if (intensitySlider_ != nullptr) intensitySlider_->setEnabled(gradientOn);
    if (heightSlider_ != nullptr) heightSlider_->setEnabled(gradientOn);
    if (gradientColorLabel_ != nullptr) gradientColorLabel_->setEnabled(gradientOn);
    for (auto& swatch : gradientSwatches_) swatch.second->setEnabled(gradientOn);
    if (customGradientButton_ != nullptr) customGradientButton_->setEnabled(gradientOn);
    refreshAppearanceCaptions();
}

void SettingsDialog::applyAppearancePreview() {
    const auto previous = currentAppearance();
    const bool styleEdited = previous.siteTheme != appearanceDraft_.siteTheme ||
                             previous.accent != appearanceDraft_.accent ||
                             previous.radiusStep != appearanceDraft_.radiusStep;
    setCurrentAppearance(appearanceDraft_);
    if (previous.accent != appearanceDraft_.accent) {
        mplayer::shell::setAccent(theme::accentColor(appearanceDraft_));
    }
    if (previous.radiusStep != appearanceDraft_.radiusStep) {
        mplayer::shell::setCornerRadius(theme::radiusSmall(appearanceDraft_));
    }
    refreshAppearanceControls();

    if (styledAppearance_.siteTheme == appearanceDraft_.siteTheme &&
        styledAppearance_.accent == appearanceDraft_.accent &&
        styledAppearance_.radiusStep == appearanceDraft_.radiusStep) {
        appearanceStyleTimer_->stop();
        return;
    }
    if (styleEdited || !appearanceStyleTimer_->isActive()) {
        appearanceStyleTimer_->start();
    }
}

void SettingsDialog::applyAppearanceStyles() {
    const bool paletteChanged = styledAppearance_.siteTheme != appearanceDraft_.siteTheme ||
                                styledAppearance_.accent != appearanceDraft_.accent;
    if (!paletteChanged && styledAppearance_.radiusStep == appearanceDraft_.radiusStep) return;

    // A burst of style edits shares one repolish; gradient edits use only the
    // painted surfaces. Save and Cancel flush the final style synchronously.
    styledAppearance_ = appearanceDraft_;
    if (paletteChanged) theme::applyPalette(appearanceDraft_);
    qApp->setStyleSheet(theme::buildStylesheet(appearanceDraft_));
    setStyleSheet(settingsStylesheet(appearanceDraft_));
}

void SettingsDialog::restoreOpeningAppearance() {
    if (appearanceCommitted_) return;
    appearanceCommitted_ = true;
    appearanceDraft_ = openingAppearance_;
    applyAppearancePreview();
    appearanceStyleTimer_->stop();
    applyAppearanceStyles();
}

void SettingsDialog::retranslateCustomizationTabs() {
    retranslateSystemIntegration();
    if (customizationHeading_ != nullptr) customizationHeading_->setText(tr("General"));
    if (customizationDescription_ != nullptr) {
        customizationDescription_->setText(tr("Adjust how content and more are displayed"));
    }
    if (siteThemeLabel_ != nullptr) siteThemeLabel_->setText(tr("Site theme"));
    if (siteTheme_ != nullptr) {
        siteTheme_->setItemText(siteTheme_->findData(QStringLiteral("dark")),
                                tr("Dark theme"));
        siteTheme_->setItemText(siteTheme_->findData(QStringLiteral("light")),
                                tr("Light theme"));
    }
    if (titleLanguageLabel_ != nullptr) {
        titleLanguageLabel_->setText(tr("Content titles language"));
    }
    if (nameLanguageLabel_ != nullptr) nameLanguageLabel_->setText(tr("Names language"));
    const auto retranslateChoices = [this](QComboBox* combo) {
        if (combo == nullptr) return;
        combo->setItemText(combo->findData(QStringLiteral("interface")), tr("As the interface"));
        combo->setItemText(combo->findData(QStringLiteral("uk")), tr("Ukrainian"));
        combo->setItemText(combo->findData(QStringLiteral("en")), tr("English"));
        combo->setItemText(combo->findData(QStringLiteral("original")), tr("Original"));
    };
    retranslateChoices(titleLanguage_);
    retranslateChoices(nameLanguage_);
    if (coverGradientLabel_ != nullptr) coverGradientLabel_->setText(tr("Gradient overlay"));
    if (coverGradientHint_ != nullptr) {
        coverGradientHint_->setText(tr("Overlay a gradient on the cover image"));
    }
    if (adultContentLabel_ != nullptr) adultContentLabel_->setText(tr("18+ content"));
    if (adultContentHint_ != nullptr) {
        adultContentHint_->setText(tr("Show explicit content without a warning"));
    }

    if (appearanceHeading_ != nullptr) appearanceHeading_->setText(tr("Appearance"));
    if (appearanceDescription_ != nullptr) {
        appearanceDescription_->setText(tr("Adjust themes, colors and display"));
    }
    if (resetAppearanceButton_ != nullptr) resetAppearanceButton_->setText(tr("Reset"));
    if (accentSectionLabel_ != nullptr) accentSectionLabel_->setText(tr("Accent color"));
    if (accentSectionHint_ != nullptr) {
        accentSectionHint_->setText(tr("Pick a preset or a custom accent color"));
    }
    if (radiusSectionLabel_ != nullptr) radiusSectionLabel_->setText(tr("Corner radius"));
    if (radiusSectionHint_ != nullptr) {
        radiusSectionHint_->setText(tr("Rounding of buttons, cards and fields"));
    }
    const QStringList radiusLabels = theme::radiusNames();
    for (int step = 0; step < radiusButtons_.size() && step < radiusLabels.size(); ++step) {
        radiusButtons_.at(step)->setText(radiusLabels.at(step));
    }
    if (backgroundGradientLabel_ != nullptr) {
        backgroundGradientLabel_->setText(tr("Background gradient"));
    }
    if (backgroundGradientHint_ != nullptr) {
        backgroundGradientHint_->setText(tr("Accent glow at the top of the page"));
    }
    if (intensityLabel_ != nullptr) intensityLabel_->setText(tr("Intensity"));
    if (heightLabel_ != nullptr) heightLabel_->setText(tr("Height"));
    if (gradientColorLabel_ != nullptr) gradientColorLabel_->setText(tr("Gradient color"));
    if (customAccentButton_ != nullptr) {
        customAccentButton_->setAccessibleName(tr("Custom accent color"));
        customAccentButton_->setToolTip(tr("Pick a custom accent color"));
    }
    if (customGradientButton_ != nullptr) {
        customGradientButton_->setAccessibleName(tr("Custom gradient color"));
        customGradientButton_->setToolTip(tr("Pick a custom gradient color"));
    }
    refreshAppearanceCaptions();
}

}  // namespace mikai::app
