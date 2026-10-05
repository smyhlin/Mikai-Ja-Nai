// The one native preference that needs Qt's GUI layer to read: the user's
// stored PortableText toggle binding.
//
// NativePrefs is a QtCore-only contract. Canonicalising a key chord is
// QKeySequence's job - and Qt puts it in QtGui - so the read and the parse live
// here, in the player/UI layer, and NativePrefs calls this reader rather than
// including a GUI header or inventing a second key grammar.
//
// Nothing is written here, and an invalid stored text is never silently
// dropped: it is reported as stored-but-invalid so the preferences layer can
// name the reason.

#include "native/NativePrefs.hpp"

#include <QtCore/QSettings>
#include <QtGui/QKeySequence>

namespace mplayer {
namespace native {

NativeToggleShortcut loadStoredToggleShortcut() {
    NativeToggleShortcut result;

    QSettings settings;
    settings.beginGroup(nativeShortcutsSettingsGroup());
    // Presence, not value: a cleared binding was still set by the user and must
    // not re-offer a default.
    result.stored = settings.contains(QLatin1String(kNativeToggleProcessingKey));
    if (result.stored) {
        const QString stored =
            settings.value(QLatin1String(kNativeToggleProcessingKey)).toString().trimmed();
        if (!stored.isEmpty()) {
            const QKeySequence sequence =
                QKeySequence::fromString(stored, QKeySequence::PortableText);
            result.value = sequence.toString(QKeySequence::PortableText);
            result.valid = !result.value.isEmpty();
        }
    }
    settings.endGroup();

    return result;
}

}  // namespace native
}  // namespace mplayer
