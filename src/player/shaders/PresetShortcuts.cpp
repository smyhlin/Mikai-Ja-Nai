// The single implementation of the preset shortcut collision check. The
// reserved-key predicate stays inline in the header because it is a pure table.

#include "PresetShortcuts.hpp"

namespace mplayer {
namespace shell {

QString shortcutClaimKey(const QKeySequence& sequence) {
    return sequence.toString(QKeySequence::PortableText);
}

ShortcutConflict findShortcutConflict(const QKeySequence& sequence,
                                      const QMap<QString, ShortcutClaim>& claims) {
    // Reserved first: the player's own handlers act on the key alone, so a
    // reserved key is refused in every form and no claim can ever override it.
    if (isReservedPresetShortcut(sequence)) {
        ShortcutConflict conflict;
        conflict.blocked = true;
        conflict.ownerKind = ShortcutOwnerKind::Reserved;
        conflict.ownerId = QStringLiteral("player");
        conflict.reasonKey = QString::fromLatin1(kShortcutReservedReasonKey);
        return conflict;
    }

    const auto held = claims.constFind(shortcutClaimKey(sequence));
    if (held == claims.constEnd()) {
        return ShortcutConflict{};
    }

    ShortcutConflict conflict;
    conflict.blocked = true;
    conflict.ownerKind = held->kind;
    conflict.ownerId = held->ownerId;
    conflict.ownerLabel = held->ownerLabel;
    conflict.reasonKey = QString::fromLatin1(kShortcutDuplicateReasonKey);
    return conflict;
}

}  // namespace shell
}  // namespace mplayer
