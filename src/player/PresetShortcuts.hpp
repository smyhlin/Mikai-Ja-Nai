// The shared contract for Anime4K preset hotkeys: where the bindings live in
// QSettings, and which keys a preset may never take.
//
// Both halves are here because two owners need them and neither should own them:
// the settings dialog writes the bindings and has to refuse the reserved ones
// before it saves, and the player reads them back and has to know that a stored
// sequence is no longer allowed. Keeping one copy of the reserved list is what
// stops the two sides from drifting apart.

#pragma once

#include <QKeySequence>
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

}  // namespace shell
}  // namespace mplayer