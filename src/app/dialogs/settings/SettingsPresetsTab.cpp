#include "dialogs/SettingsDialog.hpp"

#include "dialogs/settings/SettingsHelpers.hpp"
#include "dialogs/settings/SettingsUi.hpp"
#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"

#include <player/Anime4K.hpp>
#include <player/PresetShortcuts.hpp>

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
