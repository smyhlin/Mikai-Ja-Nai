#include "dialogs/SettingsDialog.hpp"

#include "dialogs/settings/SettingsHelpers.hpp"
#include "dialogs/settings/SettingsUi.hpp"
#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"

#include <player/shaders/Anime4K.hpp>
#include <player/shaders/PresetShortcuts.hpp>

#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace mikai::app {

QWidget* SettingsDialog::buildPresetsTab() {
    auto* page = new QWidget(this);
    page->setObjectName(QStringLiteral("presetsPage"));

    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(theme::kSpacingSmall, theme::kSpacingMedium, theme::kSpacingSmall,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    // ── Header & Search Toolbar Card ─────────────────────────────────────────
    QVBoxLayout* headerCardLayout = nullptr;
    QFrame* headerCard = makeSettingsCard(page, &headerCardLayout);
    headerCardLayout->setSpacing(theme::kSpacingSmall);

    presetsHeading_ = new QLabel(tr("Anime4K preset shortcuts"), headerCard);
    stylePageHeading(presetsHeading_);
    headerCardLayout->addWidget(presetsHeading_);

    presetsDescription_ = new QLabel(
        tr("Focus a field and press a key or combination with Ctrl, Alt, or Shift. "
           "The × button clears a binding. A combination can be assigned to only one "
           "preset; built-in player shortcuts are reserved."),
        headerCard);
    styleMutedCaption(presetsDescription_);
    headerCardLayout->addWidget(presetsDescription_);

    headerCardLayout->addWidget(makeSettingsDivider(headerCard));

    // Toolbar row: quick filter input + Clear all shortcuts action.
    auto* filterRow = new QHBoxLayout();
    filterRow->setContentsMargins(0, theme::kSpacingXs, 0, 0);
    filterRow->setSpacing(theme::kSpacingSmall);

    presetFilterEdit_ = new QLineEdit(headerCard);
    presetFilterEdit_->setObjectName(QStringLiteral("presetFilterEdit"));
    presetFilterEdit_->setPlaceholderText(tr("Filter presets…"));
    presetFilterEdit_->setAccessibleName(tr("Filter presets"));
    presetFilterEdit_->setClearButtonEnabled(true);
    presetFilterEdit_->setMinimumHeight(theme::kSettingsControlSize);
    filterRow->addWidget(presetFilterEdit_, 1);

    clearAllPresetsButton_ = new QPushButton(tr("Clear all"), headerCard);
    clearAllPresetsButton_->setObjectName(QStringLiteral("clearAllPresetsButton"));
    clearAllPresetsButton_->setAccessibleName(tr("Clear all preset shortcuts"));
    polishInteractiveControl(clearAllPresetsButton_, theme::kSettingsControlSize);
    filterRow->addWidget(clearAllPresetsButton_, 0);
    headerCardLayout->addLayout(filterRow);

    presetError_ = new QLabel(headerCard);
    presetError_->setObjectName(QStringLiteral("status"));
    presetError_->setWordWrap(true);
    yieldCaptionWidth(presetError_);
    styleStatusBanner(presetError_, QStringLiteral("danger"));
    presetError_->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(theme::kDanger)));
    presetError_->hide();
    headerCardLayout->addWidget(presetError_);

    root->addWidget(headerCard);

    // ── Scrollable Preset Category & Shortcut Bindings List ──────────────────
    auto* scroll = new QScrollArea(page);
    scroll->setObjectName(QStringLiteral("presetsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    auto* content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("presetsPage"));
    auto* list = new QVBoxLayout(content);
    list->setContentsMargins(0, 0, theme::kSpacingXs, 0);
    list->setSpacing(theme::kSpacingSmall);

    QSettings settings;
    settings.beginGroup(QLatin1String(mplayer::shell::kPresetShortcutSettingsGroup));
    QMap<QString, mplayer::shell::ShortcutClaim> claims;
    QString loadError;
    QString headingForCategory;
    for (const mplayer::anime4k::ModeInfo& mode : mplayer::anime4k::availableModes()) {
        const QString label = mplayer::anime4k::displayLabel(mode.mode);
        const QString headingText = shortcutGroupHeading(mode.category);
        if (!headingText.isEmpty() && headingText != headingForCategory) {
            if (!headingForCategory.isEmpty()) {
                list->addSpacing(theme::kSpacingXs);
            }
            headingForCategory = headingText;
            auto* groupHeading = new QLabel(headingText, content);
            styleSectionHeading(groupHeading);
            list->addWidget(groupHeading);
            presetGroupHeadings_.append({mode.category, groupHeading});
        }

        auto* rowWidget = new QFrame(content);
        rowWidget->setObjectName(QStringLiteral("presetShortcutRow"));
        auto* row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(theme::kSpacingMedium, theme::kSpacingSmall,
                                theme::kSpacingMedium, theme::kSpacingSmall);
        row->setSpacing(theme::kSpacingMedium);

        auto* name = new QLabel(label, rowWidget);
        name->setObjectName(QStringLiteral("SettingsFormLabel"));
        name->setToolTip(mode.hint);
        name->setWordWrap(true);
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        row->addWidget(name, 1, Qt::AlignVCenter);

        auto* edit = new QKeySequenceEdit(rowWidget);
        edit->setMaximumSequenceLength(1);
        edit->setClearButtonEnabled(true);
        polishInteractiveControl(edit, theme::kSettingsControlSize);
        applyTabularNumerals(edit);
        edit->setFixedWidth(shortcutFieldWidth(edit));
        if (auto* input = edit->findChild<QLineEdit*>()) {
            input->setPlaceholderText(tr("Press a key"));
            input->setAlignment(Qt::AlignCenter);
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
        rowWidget->setProperty("hasBinding", !edit->keySequence().isEmpty());
        edit->setAccessibleName(tr("Shortcut for %1").arg(label));
        edit->setToolTip(mode.hint);
        name->setBuddy(edit);
        row->addWidget(edit, 0, Qt::AlignVCenter);

        list->addWidget(rowWidget);
        presetRows_.append({mode.id, label, mode.category, edit, name, rowWidget, mode.mode});
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this,
                [this, edit, rowWidget, label,
                 previous = edit->keySequence()](const QKeySequence& sequence) mutable {
                    if (!sequence.isEmpty()) {
                        const mplayer::shell::ShortcutConflict conflict =
                            mplayer::shell::findShortcutConflict(sequence, shortcutClaims(edit));
                        if (conflict.blocked) {
                            QSignalBlocker blocker(edit);
                            edit->setKeySequence(previous);
                            rowWidget->setProperty("hasBinding", !previous.isEmpty());
                            repolishWidget(rowWidget);
                            showPresetError(presetConflictMessage(
                                conflict, mplayer::shell::shortcutClaimKey(sequence), label));
                            return;
                        }
                    }
                    previous = sequence;
                    rowWidget->setProperty("hasBinding", !sequence.isEmpty());
                    repolishWidget(rowWidget);
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
            if (row.container != nullptr) {
                row.container->setProperty("hasBinding", false);
                repolishWidget(row.container);
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

}  // namespace mikai::app
