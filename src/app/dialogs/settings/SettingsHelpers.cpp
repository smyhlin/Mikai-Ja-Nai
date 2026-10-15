#include "dialogs/settings/SettingsHelpers.hpp"

#include "theme/Metrics.hpp"

#include <player/Anime4K.hpp>

#include <QCoreApplication>
#include <QFont>
#include <QFontMetrics>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QToolButton>

namespace mikai::app {

QString backendLabel(mplayer::native::Backend backend) {
    switch (backend) {
        case mplayer::native::Backend::TensorRT: return QStringLiteral("TensorRT");
        case mplayer::native::Backend::DirectML: return QStringLiteral("DirectML");
    }
    return QString();
}

QString shortcutGroupHeading(const QString& categoryId) {
    const QString label = mplayer::anime4k::categoryLabel(categoryId);
    return label.isEmpty() ? QString() : label;
}

int shortcutFieldWidth(const QKeySequenceEdit* edit) {
    if (edit == nullptr) {
        return 160;
    }
    auto* field = edit->findChild<QLineEdit*>();
    if (field == nullptr) {
        return edit->sizeHint().width();
    }
    // Polishing the outer edit first resolves the stylesheet font down the whole
    // subtree, so the advance is the one the field paints with.
    const_cast<QKeySequenceEdit*>(edit)->ensurePolished();
    field->ensurePolished();

    QFont font = field->font();
    font.setFeature(QFont::Tag("tnum"), 1);
    field->setFont(font);

    const QStyle* style = field->style();
    int width =
        QFontMetrics(field->fontMetrics()).horizontalAdvance(QStringLiteral("Ctrl+Alt+Shift+F12"));

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
    // geometry is the space unavailable to text.
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

QString unavailableRecipeCaption(const QString& selection) {
    return QCoreApplication::translate("SettingsDialog", "%1 (not available)").arg(selection);
}

bool isReservedConflict(const mplayer::shell::ShortcutConflict& conflict) {
    return conflict.reasonKey == QLatin1String(mplayer::shell::kShortcutReservedReasonKey);
}

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

}  // namespace mikai::app
