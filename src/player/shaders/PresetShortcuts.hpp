// The shared contract for Anime4K preset shortcuts: where bindings live in
// QSettings, which player keys are reserved, and how duplicate assignments are
// rejected by both the settings dialog and the player.

#pragma once

#include <QKeySequence>
#include <QMap>
#include <QString>
#include <Qt>

namespace mplayer {
namespace shell {

/// QSettings group the bindings live under. Every key in it is a stable
/// `anime4k::ModeInfo::id`, and every value is a single QKeySequence written in
/// PortableText, so a binding survives a reordering of the menu.
inline constexpr char kPresetShortcutSettingsGroup[] = "player/presetShortcuts";

/// Whether `sequence` must not be bound to a preset.
///
/// True when the sequence is not exactly one key: a binding is a single
/// keystroke, and an empty or multi-key sequence either means nothing or means
/// something no shortcut can stand for.
///
/// Also true for the keys the player already acts on. Both PlayerShell's and
/// PlayerDialog's key handlers switch on the key alone and ignore the modifiers,
/// so Ctrl+S would toggle the episode drawer exactly like a bare S. The keys are
/// therefore reserved in every form, which is why this takes a sequence and not
/// a key code.
inline bool isReservedPresetShortcut(const QKeySequence& sequence) {
    if (sequence.count() != 1) {
        return true;
    }
    switch (sequence[0].key()) {
        case Qt::Key_Space:
        case Qt::Key_F:
        case Qt::Key_F11:
        case Qt::Key_M:
        case Qt::Key_Escape:
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_D:
        case Qt::Key_Question:
        case Qt::Key_Slash:
        case Qt::Key_S:
        case Qt::Key_T:
        case Qt::Key_C:
        case Qt::Key_Q:
            return true;
        default:
            return false;
    }
}

/// Who already holds a combination. Reserved player keys are answered by
/// `isReservedPresetShortcut()` rather than stored as claims.
enum class ShortcutOwnerKind { Reserved, Preset };

/// One combination that is already taken, and by whom. The portable form of the
/// sequence is the map key; see `shortcutClaimKey()`.
struct ShortcutClaim {
    /// Stable preset id.
    QString ownerId;
    /// The label shown to the user in a refusal.
    QString ownerLabel;
    ShortcutOwnerKind kind = ShortcutOwnerKind::Preset;
};

/// Stable reason keys a caller can map to its own wording.
inline constexpr char kShortcutReservedReasonKey[] = "shortcut.reserved";
inline constexpr char kShortcutDuplicateReasonKey[] = "shortcut.duplicate";

/// The verdict of the one collision check. `blocked` is the only success signal;
/// on a refusal `ownerId`/`ownerLabel`/`ownerKind` say what to name.
struct ShortcutConflict {
    bool blocked = false;
    QString ownerId;
    QString ownerLabel;
    ShortcutOwnerKind ownerKind = ShortcutOwnerKind::Reserved;
    QString reasonKey;
};

/// The PortableText form two bindings are compared in. Two fields showing the
/// same combination must collide however either was typed, so this - not the raw
/// keystroke - is the map key.
QString shortcutClaimKey(const QKeySequence& sequence);

/// The one collision check used while loading and editing preset bindings.
///
/// A reserved player key is refused first, then a combination some other claim
/// already holds. Callers skip an empty sequence before asking; an empty
/// sequence reads as reserved here, exactly as it does in
/// `isReservedPresetShortcut()`.
ShortcutConflict findShortcutConflict(const QKeySequence& sequence,
                                      const QMap<QString, ShortcutClaim>& claims);

}  // namespace shell
}  // namespace mplayer