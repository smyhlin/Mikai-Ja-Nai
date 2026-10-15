#pragma once

#include <native/bundle/NativePrefs.hpp>
#include <player/PresetShortcuts.hpp>

#include <QString>

class QKeySequenceEdit;

namespace mikai::app {

/// Backend names are proper nouns and are never translated; the stable name is
/// what a stored document carries, and this is only what the combo shows.
QString backendLabel(mplayer::native::Backend backend);

/// Rows follow the preset menu categories; experiment labels retain their family.
QString shortcutGroupHeading(const QString& categoryId);

/// The widest thing a one-key field can show: a full modifier stack in front of
/// a function key, measured on the inner QLineEdit the stylesheet actually paints.
int shortcutFieldWidth(const QKeySequenceEdit* edit);

mplayer::shell::ShortcutClaim presetClaim(const QString& id, const QString& label);

/// Names the holder of a combination in a refusal, so the message says who
/// already owns the key rather than only that it is taken.
QString shortcutOwnerName(const mplayer::shell::ShortcutConflict& conflict);

/// The caption the synthetic "requested but not offered" recipe entry carries.
QString unavailableRecipeCaption(const QString& selection);

bool isReservedConflict(const mplayer::shell::ShortcutConflict& conflict);

/// A refusal while editing or saving: the key is not written.
QString presetConflictMessage(const mplayer::shell::ShortcutConflict& conflict,
                              const QString& portable, const QString& label);

/// A refusal while loading: the stored binding is skipped, never installed, and
/// the reason names who holds the key now.
QString presetLoadMessage(const mplayer::shell::ShortcutConflict& conflict,
                          const QString& portable, const QString& label);

}  // namespace mikai::app
