#include "dialogs/SettingsDialog.hpp"

#include "I18n.hpp"
#include "state/Feeds.hpp"
#include "theme/Colors.hpp"
#include "theme/Icons.hpp"
#include "theme/Metrics.hpp"
#include "workers/Runner.hpp"

#include <backend/MikaiClient.hpp>
#include <native/NativeAssetsPanel.hpp>
#include <native/NativeRifeControls.hpp>
#include <native/NativeRuntimeSelection.hpp>
#include <player/Anime4K.hpp>
#include <player/PresetShortcuts.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QTabWidget>
#include <QTimer>
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
                    .horizontalAdvance(QStringLiteral("Ctrl+Shift+Alt+F12"));

    // The styled border and padding. The field has no laid-out width while the
    // dialog is still being built, so the insets are read off a rect wide enough
    // that its transient geometry cannot skew them.
    QStyleOptionFrame frame;
    frame.initFrom(field);
    frame.rect = QRect(0, 0, width + theme::kSpacingLarge, frame.rect.height());
    const QRect contents = style->subElementRect(QStyle::SE_LineEditContents, &frame, field);
    width += frame.rect.width() - contents.width();
    width += 2 * style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, field);

    // The clear action is drawn inside the field, so its glyph and the margin the
    // style keeps around it are width the binding has to fit beside.
    if (field->isClearButtonEnabled()) {
        width += style->pixelMetric(QStyle::PM_LineEditIconSize, nullptr, field);
        width += style->pixelMetric(QStyle::PM_LineEditIconMargin, nullptr, field);
    }
    return width + theme::kSpacingMedium;
}

/// The claim the native toggle holds. There is one native action, so this is the
/// only native claim a map ever holds, and it is built the same way everywhere.
mplayer::shell::ShortcutClaim nativeToggleClaim() {
    mplayer::shell::ShortcutClaim claim;
    claim.ownerId = QLatin1String(mplayer::native::kNativeToggleProcessingKey);
    claim.ownerLabel =
        QCoreApplication::translate("SettingsDialog", "Processing toggle");
    claim.kind = mplayer::shell::ShortcutOwnerKind::Native;
    return claim;
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
        case mplayer::shell::ShortcutOwnerKind::Native:
            return QCoreApplication::translate("SettingsDialog", "the native action “%1”")
                .arg(conflict.ownerLabel);
        case mplayer::shell::ShortcutOwnerKind::Reserved:
            break;
    }
    return QCoreApplication::translate("SettingsDialog", "the player");
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

QString nativeConflictMessage(const mplayer::shell::ShortcutConflict& conflict,
                              const QString& portable) {
    if (isReservedConflict(conflict)) {
        return QCoreApplication::translate(
                   "SettingsDialog",
                   "Key %1 is reserved by the player and cannot be assigned to native "
                   "processing.")
            .arg(portable);
    }
    return QCoreApplication::translate("SettingsDialog",
                                       "Combination %1 is already assigned to %2.")
        .arg(portable, shortcutOwnerName(conflict));
}

}  // namespace

SettingsDialog::SettingsDialog(backend::MikaiClient& client, Runner& runner, QWidget* parent)
    : QDialog(parent), client_(client), runner_(runner) {
    setWindowTitle(tr("Settings"));
    setMinimumSize(theme::kSettingsDialogMinWidth, theme::kSettingsDialogMinHeight);
    resize(theme::kSettingsDialogDefaultWidth, theme::kSettingsDialogDefaultHeight);

    // Read the native request before any tab is built: the Presets tab has to
    // know the native binding, so a stored preset that collides with it is
    // skipped on load the same way a reserved one is.
    nativePreferences_ = mplayer::native::loadNativePreferences();
    nativeAvailability_ = mplayer::native::currentNativeAvailability();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(theme::kSpacingLarge, theme::kSpacingLarge, theme::kSpacingLarge,
                             theme::kSpacingLarge);
    root->setSpacing(theme::kSpacingMedium);

    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    tabs_->addTab(buildGeneralTab(), theme::gearIcon(theme::kIconSize, QLatin1String(theme::kAccent)),
                  tr("General"));
    tabs_->addTab(buildPresetsTab(),
                  theme::keyboardIcon(theme::kIconSize, QLatin1String(theme::kAccent)),
                  tr("Presets"));
    // The gear is the only "machine settings" glyph the icon set carries; adding
    // a fourth icon is another owner's file, and reusing one beats inventing a
    // glyph here.
    tabs_->addTab(buildProcessingTab(),
                  theme::gearIcon(theme::kIconSize, QLatin1String(theme::kAccent)),
                  tr("Processing"));
    if (!presetError_->isHidden()) {
        tabs_->setCurrentWidget(presetError_->parentWidget());
    }
    root->addWidget(tabs_, 1);

    auto* buttons = new QDialogButtonBox(this);
    auto* cancel = buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    auto* save = buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("primary"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, &SettingsDialog::save);
    root->addWidget(buttons);

    quotaTimer_ = new QTimer(this);
    quotaTimer_->setInterval(1000);
    connect(quotaTimer_, &QTimer::timeout, this, &SettingsDialog::refreshQuota);
    quotaTimer_->start();
    refreshQuota();
}

SettingsDialog::~SettingsDialog() = default;

QWidget* SettingsDialog::buildGeneralTab() {
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium, theme::kSpacingMedium,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    auto* headerRow = new QHBoxLayout();
    auto* iconLabel = new QLabel(page);
    iconLabel->setPixmap(theme::gearIcon(22, QLatin1String(theme::kAccent)).pixmap(22, 22));
    auto* heading = new QLabel(tr("Mikai API access token"), page);
    heading->setObjectName(QStringLiteral("heading"));
    headerRow->addWidget(iconLabel);
    headerRow->addWidget(heading);
    headerRow->addStretch();
    root->addLayout(headerRow);

    auto* description = new QLabel(
        tr("An API key raises the limit from 60 to 300 requests per minute and is stored "
           "locally in the .env file."),
        page);
    description->setObjectName(QStringLiteral("muted"));
    description->setWordWrap(true);
    root->addWidget(description);

    auto* link = new QLabel(page);
    link->setTextFormat(Qt::RichText);
    link->setText(tr("<a style=\"color: %1; text-decoration: none;\" "
                     "href=\"https://mikai.me/user/developers\">"
                     "Get a key from the developer dashboard on mikai.me ↗</a>")
                       .arg(QLatin1String(theme::kAccent)));
    link->setOpenExternalLinks(true);
    link->setWordWrap(true);
    root->addWidget(link);

    token_ = new QLineEdit(page);
    token_->setPlaceholderText(tr("mk_... (leave blank for anonymous access)"));
    token_->setClearButtonEnabled(true);
    const QString current = client_.apiKey();
    if (!current.isEmpty()) {
        token_->setText(current);
    }
    root->addWidget(token_);

    // The token check is an API concern, so it belongs with the token rather
    // than as a dialog-wide action sitting under the preset bindings.
    auto* tokenRow = new QHBoxLayout();
    tokenRow->setSpacing(theme::kSpacingSmall);
    tokenRow->addStretch();
    testButton_ = new QPushButton(tr("Test"), page);
    connect(testButton_, &QPushButton::clicked, this, &SettingsDialog::testToken);
    tokenRow->addWidget(testButton_);
    root->addLayout(tokenRow);

    auto* quotaTitle = new QLabel(tr("API usage"), page);
    quotaTitle->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(quotaTitle);
    quota_ = new QLabel(page);
    quota_->setObjectName(QStringLiteral("descriptionPanel"));
    quota_->setWordWrap(true);
    root->addWidget(quota_);

    auto* feedsTitle = new QLabel(tr("Home page sections"), page);
    feedsTitle->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(feedsTitle);
    auto* feedsHint = new QLabel(
        tr("Disabled sections are not loaded and do not use API requests. Search and title "
           "pages are always available."),
        page);
    feedsHint->setObjectName(QStringLiteral("muted"));
    feedsHint->setWordWrap(true);
    root->addWidget(feedsHint);

    QSettings settings;
    const QSet<QString> enabled = feeds::enabled(settings.value(feeds::settingsKey()).toString());
    // Remembered so the caller can tell a feed edit from a native or language
    // edit and refresh the feeds only for the former.
    initialFeeds_ = enabled;
    for (const QString& key : feeds::all()) {
        auto* box = new QCheckBox(feeds::label(key), page);
        box->setChecked(enabled.contains(key));
        root->addWidget(box);
        feedBoxes_.insert(key, box);
    }

    auto* languageTitle = new QLabel(tr("Interface language"), page);
    languageTitle->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(languageTitle);

    language_ = new QComboBox(page);
    language_->addItem(tr("Use system language"), QStringLiteral("system"));
    language_->addItem(tr("English"), QStringLiteral("en"));
    language_->addItem(tr("Ukrainian"), QStringLiteral("uk"));
    language_->setAccessibleName(tr("Interface language"));
    const int languageIndex = language_->findData(languagePreference());
    if (languageIndex >= 0) {
        language_->setCurrentIndex(languageIndex);
    }
    root->addWidget(language_);

    auto* languageHint =
        new QLabel(tr("Changes take effect after restarting Mikai."), page);
    languageHint->setObjectName(QStringLiteral("muted"));
    languageHint->setWordWrap(true);
    root->addWidget(languageHint);

    status_ = new QLabel(page);
    status_->setObjectName(QStringLiteral("status"));
    status_->setWordWrap(true);
    root->addWidget(status_);

    root->addStretch();
    return page;
}

QWidget* SettingsDialog::buildPresetsTab() {
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium, theme::kSpacingMedium,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    auto* heading = new QLabel(tr("Anime4K preset shortcuts"), page);
    heading->setObjectName(QStringLiteral("heading"));
    root->addWidget(heading);

    auto* description = new QLabel(
        tr("Focus a field and press a key or combination with Ctrl, Alt, or Shift. "
           "The × button clears a binding. A combination can be assigned to only one "
           "preset; built-in player shortcuts are reserved."),
        page);
    description->setObjectName(QStringLiteral("muted"));
    description->setWordWrap(true);
    root->addWidget(description);

    presetError_ = new QLabel(page);
    presetError_->setObjectName(QStringLiteral("status"));
    presetError_->setWordWrap(true);
    presetError_->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(theme::kDanger)));
    presetError_->hide();
    root->addWidget(presetError_);

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMaximumHeight(theme::kSettingsPresetListMaxHeight);
    auto* content = new QWidget(scroll);
    auto* list = new QVBoxLayout(content);
    list->setContentsMargins(0, 0, theme::kSpacingSmall, 0);
    list->setSpacing(theme::kSpacingXs);

    QSettings settings;
    settings.beginGroup(QLatin1String(mplayer::shell::kPresetShortcutSettingsGroup));
    // Seeded with the native binding so a stored preset that collides with it is
    // skipped exactly as one that collides with a reserved key is; the native
    // claim also names the native owner in the message.
    QMap<QString, mplayer::shell::ShortcutClaim> claims;
    if (!nativePreferences_.toggleProcessingShortcut.isEmpty()) {
        claims.insert(nativePreferences_.toggleProcessingShortcut, nativeToggleClaim());
    }
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
        }

        auto* row = new QHBoxLayout();
        row->setSpacing(theme::kSpacingMedium);

        auto* name = new QLabel(label, content);
        name->setToolTip(mode.hint);
        row->addWidget(name, 1);

        auto* edit = new QKeySequenceEdit(content);
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
        row->addWidget(edit, 0, Qt::AlignVCenter);

        list->addLayout(row);
        presetRows_.append({mode.id, label, edit});
        // A message left over from a rejected save names a key the user has
        // already changed, so it goes as soon as they touch the binding.
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
    list->addStretch();
    scroll->setWidget(content);
    root->addWidget(scroll, 1);
    return page;
}

QMap<QString, mplayer::shell::ShortcutClaim> SettingsDialog::shortcutClaims(
    const QKeySequenceEdit* exclude) const {
    QMap<QString, mplayer::shell::ShortcutClaim> claims;

    // The native binding first, so a refusal names the native owner even while a
    // preset field is the one being edited. Before the Processing tab is built
    // the field does not exist yet, and the stored request answers instead.
    const QString nativeStored =
        nativeToggleEdit_ != nullptr
            ? mplayer::shell::shortcutClaimKey(nativeToggleEdit_->keySequence())
            : nativePreferences_.toggleProcessingShortcut;
    if (nativeToggleEdit_ != exclude && !nativeStored.isEmpty()) {
        claims.insert(nativeStored, nativeToggleClaim());
    }

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
    // The native surfaces are taller than any sane dialog, so the page scrolls as
    // one piece: the dialog keeps the size it was asked for and the Save/Cancel
    // dock stays on screen.
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("nativeProcessingScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* page = new QWidget(scroll);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium, theme::kSpacingMedium,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    auto* heading = new QLabel(tr("Native processing"), page);
    heading->setObjectName(QStringLiteral("heading"));
    root->addWidget(heading);

    auto* description = new QLabel(
        tr("Native processing runs the AnimeJaNai engine on the GPU. It is a separate track "
           "from the Anime4K presets and never changes or clears them."),
        page);
    description->setObjectName(QStringLiteral("muted"));
    description->setWordWrap(true);
    description->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(description);

    nativeError_ = new QLabel(page);
    nativeError_->setObjectName(QStringLiteral("status"));
    nativeError_->setWordWrap(true);
    nativeError_->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(theme::kDanger)));
    nativeError_->hide();
    root->addWidget(nativeError_);

    // A schema this build does not read is shown, not edited: the stored block
    // belongs to another version and must survive being looked at.
    const bool editable = nativePreferences_.schemaRecognised;

    auto addRow = [&](const QString& label, QWidget* field) {
        auto* row = new QHBoxLayout();
        row->setSpacing(theme::kSpacingMedium);
        auto* name = new QLabel(label, page);
        row->addWidget(name, 1);
        row->addWidget(field, 0, Qt::AlignVCenter);
        root->addLayout(row);
    };

    nativeEnabled_ = new QCheckBox(tr("Enable native processing"), page);
    nativeEnabled_->setObjectName(QStringLiteral("nativeEnabledCheck"));
    nativeEnabled_->setAccessibleName(tr("Enable native processing"));
    nativeEnabled_->setChecked(nativePreferences_.enabled);
    nativeEnabled_->setEnabled(editable);
    root->addWidget(nativeEnabled_);

    nativeBackend_ = new QComboBox(page);
    nativeBackend_->setObjectName(QStringLiteral("nativeBackendCombo"));
    nativeBackend_->setAccessibleName(tr("Native backend"));
    for (const mplayer::native::Backend backend : nativeAvailability_.backends) {
        nativeBackend_->addItem(backendLabel(backend), static_cast<int>(backend));
    }
    if (nativeBackend_->count() == 0) {
        for (const mplayer::native::Backend backend : mplayer::native::allBackends()) {
            nativeBackend_->addItem(backendLabel(backend), static_cast<int>(backend));
        }
    }
    nativeBackend_->setCurrentIndex(
        nativeBackend_->findData(static_cast<int>(nativePreferences_.backend)));
    nativeBackend_->setEnabled(editable);
    addRow(tr("Backend"), nativeBackend_);

    nativeRecipe_ = new QComboBox(page);
    nativeRecipe_->setObjectName(QStringLiteral("nativeRecipeCombo"));
    nativeRecipe_->setAccessibleName(tr("Native recipe"));
    // A stored request may name a recipe no catalogue offers, and that id is long:
    // sizing the combo to its widest entry would widen the whole page past the
    // smallest window. The popup still shows the entry in full.
    nativeRecipe_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    nativeRecipe_->setMinimumContentsLength(theme::kSettingsChoiceLengthChars);
    // The row must survive the smallest window: a long stored id already sizes
    // to its char budget, and the combo yields the rest instead of pushing the
    // page past the viewport. The popup still shows every entry in full.
    nativeRecipe_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    populateRecipeChoices(nativePreferences_.recipeId);
    addRow(tr("Recipe"), nativeRecipe_);

    customModelsDir_ = new QLineEdit(page);
    customModelsDir_->setObjectName(QStringLiteral("nativeModelsDirEdit"));
    customModelsDir_->setReadOnly(true);
    customModelsDir_->setText(nativePreferences_.customModelsDir);
    customModelsDir_->setPlaceholderText(tr("Not set"));
    customModelsDir_->setAccessibleName(tr("Custom models directory"));
    customModelsDir_->setToolTip(
        tr("A validated, read-only model directory. Mikai never writes to it."));
    addRow(tr("Custom models directory"), customModelsDir_);

    nativeToggleEdit_ = new QKeySequenceEdit(page);
    nativeToggleEdit_->setObjectName(QStringLiteral("nativeToggleShortcutEdit"));
    nativeToggleEdit_->setMaximumSequenceLength(1);
    nativeToggleEdit_->setClearButtonEnabled(true);
    nativeToggleEdit_->setMinimumHeight(theme::kSettingsControlSize);
    nativeToggleEdit_->setFixedWidth(shortcutFieldWidth(nativeToggleEdit_));
    if (auto* input = nativeToggleEdit_->findChild<QLineEdit*>()) {
        input->setPlaceholderText(tr("Press a key"));
    }
    nativeToggleEdit_->setAccessibleName(tr("Shortcut to toggle native processing"));
    nativeToggleEdit_->setToolTip(tr("A single key or combination; it must not collide with a "
                                     "reserved player key or an Anime4K preset."));
    nativeToggleEdit_->setEnabled(editable);

    // A block this build does not read is shown, not edited; naming it here is
    // what makes the disabled fields legible instead of mysterious.
    QString nativeLoadError;
    if (!nativePreferences_.schemaRecognised) {
        for (const mplayer::native::NativePreferenceIssue& issue : nativePreferences_.issues) {
            if (issue.reasonKey == QLatin1String("native.schemaUnknown")) {
                nativeLoadError = issue.reason;
                break;
            }
        }
    }
    if (!nativePreferences_.toggleProcessingShortcut.isEmpty()) {
        const QKeySequence sequence = QKeySequence::fromString(
            nativePreferences_.toggleProcessingShortcut, QKeySequence::PortableText);
        if (!sequence.isEmpty()) {
            const mplayer::shell::ShortcutConflict conflict =
                mplayer::shell::findShortcutConflict(sequence, shortcutClaims(nativeToggleEdit_));
            if (conflict.blocked) {
                // Skipped, never installed over a key the player or a preset
                // already acts on, and reported.
                nativeLoadError =
                    nativeConflictMessage(conflict, mplayer::shell::shortcutClaimKey(sequence));
            } else {
                nativeToggleEdit_->setKeySequence(sequence);
            }
        }
    }
    addRow(tr("Toggle shortcut"), nativeToggleEdit_);

    connect(nativeEnabled_, &QCheckBox::toggled, this, [this](bool) { refreshNativeStatus(); });
    connect(nativeBackend_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshNativeStatus(); });
    connect(nativeRecipe_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshNativeStatus(); });
    connect(nativeToggleEdit_, &QKeySequenceEdit::keySequenceChanged, this,
            [this, previous = nativeToggleEdit_->keySequence()](
                const QKeySequence& sequence) mutable {
                if (!sequence.isEmpty()) {
                    const mplayer::shell::ShortcutConflict conflict =
                        mplayer::shell::findShortcutConflict(sequence,
                                                             shortcutClaims(nativeToggleEdit_));
                    if (conflict.blocked) {
                        QSignalBlocker blocker(nativeToggleEdit_);
                        nativeToggleEdit_->setKeySequence(previous);
                        showNativeError(nativeConflictMessage(
                            conflict, mplayer::shell::shortcutClaimKey(sequence)));
                        return;
                    }
                }
                previous = sequence;
                nativeError_->clear();
                nativeError_->hide();
            });

    nativeStatus_ = new QLabel(page);
    nativeStatus_->setObjectName(QStringLiteral("descriptionPanel"));
    nativeStatus_->setAccessibleName(tr("Native processing status"));
    nativeStatus_->setWordWrap(true);
    root->addWidget(nativeStatus_);

    auto* runtimeHeading = new QLabel(tr("mpv runtime"), page);
    runtimeHeading->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(runtimeHeading);

    runtimeChoice_ = new QComboBox(page);
    runtimeChoice_->setObjectName(QStringLiteral("runtimeChoiceCombo"));
    runtimeChoice_->setAccessibleName(tr("mpv runtime"));
    runtimeChoice_->addItem(tr("Standard"),
                            static_cast<int>(mplayer::native::RuntimeChoice::Standard));
    runtimeChoice_->addItem(tr("Native"),
                            static_cast<int>(mplayer::native::RuntimeChoice::Native));
    const int runtimeIndex =
        runtimeChoice_->findData(static_cast<int>(mplayer::native::storedRuntimeChoice()));
    if (runtimeIndex >= 0) {
        runtimeChoice_->setCurrentIndex(runtimeIndex);
    }
    addRow(tr("Runtime module"), runtimeChoice_);

    auto* runtimeHint = new QLabel(
        tr("The runtime module is chosen once per launch. Changing it takes effect after "
           "restarting Mikai."),
        page);
    runtimeHint->setObjectName(QStringLiteral("muted"));
    runtimeHint->setWordWrap(true);
    runtimeHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(runtimeHint);

    runtimeStatus_ = new QLabel(page);
    runtimeStatus_->setObjectName(QStringLiteral("muted"));
    runtimeStatus_->setAccessibleName(tr("mpv runtime status"));
    runtimeStatus_->setWordWrap(true);
    root->addWidget(runtimeStatus_);

    refreshNativeStatus();
    refreshRuntimeStatus();

    if (!nativeLoadError.isEmpty()) {
        nativeError_->setText(nativeLoadError);
        nativeError_->show();
    }

    // M35: the assets surface. It lists the owned models and the prepared
    // artifacts, offers the one import this build can honestly run, and states
    // every obligation it cannot meet - the source grant for preparation, the
    // asset lineage for downloads, and the missing recipe bridge for selecting a
    // model. Nothing here pretends a preparation or an import happened.
    auto* assetsPanel = new mplayer::native::NativeAssetsPanel(page);
    assetsPanel->setObjectName(QStringLiteral("nativeAssetsPanel"));
    assetsPanel->setAccessibleName(tr("Native model assets"));
    assetsPanel->setRunner(&runner_);
    assetsPanel->setClient(&client_);
    // The panel registers a recipe for the backend the user is asking for here,
    // and hands the id back; the combo shows it and Save is the only writer.
    assetsPanel->setRequestedBackend(
        static_cast<mplayer::native::Backend>(nativeBackend_->currentData().toInt()));
    connect(assetsPanel, &mplayer::native::NativeAssetsPanel::recipeRequested, this,
            &SettingsDialog::applyRequestedRecipe);
    connect(nativeBackend_, &QComboBox::currentIndexChanged, this, [this, assetsPanel](int) {
        assetsPanel->setRequestedBackend(
            static_cast<mplayer::native::Backend>(nativeBackend_->currentData().toInt()));
    });

    // The page scrolls as a whole, so the panel is laid out at full height; a
    // second scrollbar inside the tab would only trap the wheel.
    root->addWidget(assetsPanel);

    // The RIFE configuration surface. Naming a request is legal on its own:
    // every capability is Unavailable while the original weight hop and a
    // prepared native pipeline are open, and that fact must not block describing
    // the request. The panel runs nothing; it stores the exact recipe through the
    // user recipe store and hands the id back for the combo and the existing
    // Save, which is the only writer of the requested preference.
    auto* rifeHeading = new QLabel(tr("RIFE interpolation"), page);
    rifeHeading->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(rifeHeading);

    auto* rifeHint = new QLabel(
        tr("Describe the interpolation request: the v4.14 variant, the ensemble flag, the exact "
           "frame factor, the scene threshold and the order - on its own or combined with a "
           "stored spatial recipe. Every variant is unavailable while the original weight hop "
           "and a prepared native pipeline are open; the request is stored, and the picture "
           "keeps using the original pipeline."),
        page);
    rifeHint->setObjectName(QStringLiteral("muted"));
    rifeHint->setWordWrap(true);
    rifeHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(rifeHint);

    nativeRife_ = new mplayer::native::NativeRifeControls(page);
    nativeRife_->setObjectName(QStringLiteral("nativeRifePanel"));
    nativeRife_->setAccessibleName(tr("RIFE interpolation"));
    nativeRife_->setRequestedBackend(
        static_cast<mplayer::native::Backend>(nativeBackend_->currentData().toInt()));
    nativeRife_->loadStoredRecipe(nativePreferences_.recipeId);
    connect(nativeRife_, &mplayer::native::NativeRifeControls::recipeRequested, this,
            &SettingsDialog::applyRequestedRecipe);
    connect(nativeBackend_, &QComboBox::currentIndexChanged, this, [this](int) {
        nativeRife_->setRequestedBackend(
            static_cast<mplayer::native::Backend>(nativeBackend_->currentData().toInt()));
    });
    root->addWidget(nativeRife_);

    root->addStretch();
    scroll->setWidget(page);
    return scroll;
}

void SettingsDialog::refreshNativeStatus() {
    if (nativeStatus_ == nullptr) {
        return;
    }
    // The requested state is what the widgets say; the effective state and the
    // reason come from the producer, so a requested-but-unrunnable choice shows
    // as requested and not active instead of looking applied.
    mplayer::native::NativePreferences request = nativePreferences_;
    request.enabled = nativeEnabled_->isChecked();
    request.backend =
        static_cast<mplayer::native::Backend>(nativeBackend_->currentData().toInt());
    request.recipeId = nativeRecipe_->currentData().toString();

    const mplayer::native::NativeSelectionView view =
        mplayer::native::evaluateNativeSelection(request, nativeAvailability_);
    if (view.requestAvailable) {
        nativeStatus_->setText(
            tr("Requested, and available on this runtime. It is applied while the engine "
               "runs; until then the picture uses the original pipeline."));
    } else if (!view.requestedEnabled) {
        nativeStatus_->setText(tr("Off. The picture uses the original pipeline."));
    } else {
        nativeStatus_->setText(
            tr("Requested, but not available: %1 The picture uses the original pipeline.")
                .arg(view.availability.reason));
    }
}

void SettingsDialog::refreshRuntimeStatus() {
    if (runtimeStatus_ == nullptr) {
        return;
    }
    const QString status = mplayer::native::runtimeStatusText();
    runtimeStatus_->setText(status.isEmpty() ? tr("The runtime has not been selected yet.")
                                             : status);
}

void SettingsDialog::populateRecipeChoices(const QString& selection) {
    if (nativeRecipe_ == nullptr) {
        return;
    }
    // Rebuilding must not look like a user edit: the selection is applied
    // afterwards, and `refreshNativeStatus` reads whatever ends up selected.
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
        // The requested recipe stays visible as an unavailable entry rather than
        // being quietly dropped for being absent from a catalogue that has not
        // been produced yet.
        nativeRecipe_->addItem(tr("%1 (not available)").arg(selection), selection);
        offered = true;
    }
    if (nativeRecipe_->count() == 1 && !offered) {
        nativeRecipe_->setEnabled(false);
        nativeRecipe_->setToolTip(tr("No native recipe is available in this build."));
    } else {
        nativeRecipe_->setEnabled(nativePreferences_.schemaRecognised);
    }
    const int index = nativeRecipe_->findData(selection);
    nativeRecipe_->setCurrentIndex(index >= 0 ? index : 0);
}

void SettingsDialog::applyRequestedRecipe(const QString& recipeId) {
    // A recipe the user just registered. What it resolves to may have changed,
    // so the snapshot is re-read before the choice is shown. Nothing is written:
    // Save persists the request through the existing native patch, and Cancel
    // leaves the stored value exactly as it was.
    nativeAvailability_ = mplayer::native::currentNativeAvailability();
    populateRecipeChoices(recipeId);
    refreshNativeStatus();
    // The RIFE panel re-reads the store: a spatial recipe stored alongside this
    // request is now selectable as a base, and the stored temporal values are
    // shown exactly as the document carries them.
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
    runner_.run(
        [this, candidate] {
            const QMap<QString, QVariant> result = client_.validateApiKey(candidate);
            return QVariant::fromValue(result);
        },
        [this](const QVariant& raw) {
            testButton_->setEnabled(true);
            const QMap<QString, QVariant> result = raw.toMap();
            if (result.value(QStringLiteral("valid")).toBool()) {
                const bool authenticated = result.value(QStringLiteral("authenticated")).toBool();
                const int limit = result.value(QStringLiteral("rate_limit"), 60).toInt();
                const QString name = result.value(QStringLiteral("name")).toString();
                if (authenticated && !name.isEmpty()) {
                    status_->setText(
                        tr("Key is valid (user: %1, limit: %2/min)").arg(name).arg(limit));
                } else if (authenticated) {
                    status_->setText(tr("Key is valid (limit: %1/min)").arg(limit));
                } else {
                    status_->setText(tr("Anonymous access (limit: %1/min)").arg(limit));
                }
            } else {
                const QString code = result.value(QStringLiteral("error_code")).toString();
                const QString error = result.value(QStringLiteral("error")).toString();
                const QString translatedError = localizedApiError(code);
                const QString displayError =
                    translatedError.isEmpty() ? (error.isEmpty() ? tr("Invalid token") : error)
                                              : translatedError;
                status_->setText(tr("Error: %1").arg(displayError));
            }
        },
        [this](const QString& message) {
            testButton_->setEnabled(true);
            status_->setText(tr("Connection error: %1").arg(message));
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
    // The native binding is checked first, so a preset that would take the
    // native combination is refused and the message names the native owner. The
    // map is then built up as presets validate, so a duplicate inside the preset
    // space is caught by the same shared check.
    QMap<QString, mplayer::shell::ShortcutClaim> claims;
    const QString nativeStored =
        nativeToggleEdit_ != nullptr
            ? mplayer::shell::shortcutClaimKey(nativeToggleEdit_->keySequence())
            : nativePreferences_.toggleProcessingShortcut;
    if (!nativeStored.isEmpty()) {
        claims.insert(nativeStored, nativeToggleClaim());
    }

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

QString SettingsDialog::collectNativeShortcuts(QString* toggleProcessing) const {
    toggleProcessing->clear();
    if (nativeToggleEdit_ == nullptr) {
        return {};
    }
    const QKeySequence sequence = nativeToggleEdit_->keySequence();
    if (sequence.isEmpty()) {
        return {};
    }
    const QString portable = mplayer::shell::shortcutClaimKey(sequence);

    // Every preset is a claim here; the native field is the one being checked,
    // and there is no other native action to collide with.
    QMap<QString, mplayer::shell::ShortcutClaim> claims;
    for (const PresetRow& row : presetRows_) {
        const QKeySequence other = row.edit->keySequence();
        if (other.isEmpty()) {
            continue;
        }
        const QString key = mplayer::shell::shortcutClaimKey(other);
        if (!claims.contains(key)) {
            claims.insert(key, presetClaim(row.id, row.label));
        }
    }

    const mplayer::shell::ShortcutConflict conflict =
        mplayer::shell::findShortcutConflict(sequence, claims);
    if (conflict.blocked) {
        return nativeConflictMessage(conflict, portable);
    }
    *toggleProcessing = portable;
    return {};
}

void SettingsDialog::showPresetError(const QString& message) {
    presetError_->setText(message);
    presetError_->show();
    tabs_->setCurrentIndex(1);
}

void SettingsDialog::showNativeError(const QString& message) {
    nativeError_->setText(message);
    nativeError_->show();
    tabs_->setCurrentIndex(2);
}

void SettingsDialog::save() {
    // Every binding, preset and native, is checked before anything is written: a
    // rejected shortcut must not leave the token, feeds or native request saved
    // while the dialog stays open.
    QMap<QString, QString> presets;
    const QString presetProblem = collectPresetShortcuts(&presets);
    if (!presetProblem.isEmpty()) {
        showPresetError(presetProblem);
        return;
    }
    QString toggleProcessing;
    const QString nativeProblem = collectNativeShortcuts(&toggleProcessing);
    if (!nativeProblem.isEmpty()) {
        showNativeError(nativeProblem);
        return;
    }
    presetError_->clear();
    presetError_->hide();
    nativeError_->clear();
    nativeError_->hide();

    const QString candidate = token_->text().trimmed();
    if (candidate != client_.apiKey()) {
        client_.setApiKey(candidate);
    }
    QSet<QString> chosen;
    for (auto it = feedBoxes_.constBegin(); it != feedBoxes_.constEnd(); ++it) {
        if (it.value()->isChecked()) {
            chosen.insert(it.key());
        }
    }
    QSettings settings;
    settings.setValue(QString::fromLatin1(kUiLanguageSettingKey),
                      language_->currentData().toString());
    settings.setValue(feeds::settingsKey(), feeds::serialize(chosen));
    settings.beginGroup(QLatin1String(mplayer::shell::kPresetShortcutSettingsGroup));
    for (const PresetRow& row : presetRows_) {
        // A blank binding is a removal, not an empty value: the player reads an
        // unset preset as the key being absent.
        if (presets.contains(row.id)) {
            settings.setValue(row.id, presets.value(row.id));
        } else {
            settings.remove(row.id);
        }
    }
    settings.endGroup();

    // Native preferences: only the fields the user actually changed are named,
    // so an untouched block writes nothing. That is what keeps a General save -
    // the language, the token, the feeds, the presets - out of native storage,
    // and it means a block this build does not read is never rewritten by a
    // dialog that cannot edit it. A field of such a block that *was* changed is
    // refused by the store below, with the reason shown instead of a reset.
    mplayer::native::NativePreferencePatch patch;
    const mplayer::native::Backend chosenBackend =
        static_cast<mplayer::native::Backend>(nativeBackend_->currentData().toInt());
    if (nativeEnabled_->isChecked() != nativePreferences_.enabled) {
        patch.enabled = nativeEnabled_->isChecked();
    }
    if (chosenBackend != nativePreferences_.backend) {
        patch.backend = chosenBackend;
    }
    const QString chosenRecipe = nativeRecipe_->currentData().toString();
    if (chosenRecipe != nativePreferences_.recipeId) {
        patch.recipeId = chosenRecipe;
    }
    if (toggleProcessing != nativePreferences_.toggleProcessingShortcut) {
        patch.toggleProcessingShortcut = toggleProcessing;
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

    // The runtime choice is a separate key owned by the runtime selection. It is
    // written only here, on Save, and takes effect on the next launch.
    QString runtimeReason;
    mplayer::native::storeRuntimeChoice(
        static_cast<mplayer::native::RuntimeChoice>(runtimeChoice_->currentData().toInt()),
        &runtimeReason);

    accept();
}

}  // namespace mikai::app