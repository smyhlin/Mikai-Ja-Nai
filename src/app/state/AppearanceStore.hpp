#pragma once

#include "../theme/Appearance.hpp"

#include <QObject>

namespace mikai::app {

/// Consumers compare their own paint dependencies against the previous value
/// so a gradient edit cannot invalidate cached poster artwork.
class AppearanceNotifier : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

signals:
    void changed(const theme::Appearance& previous);
};

AppearanceNotifier& appearanceNotifier();

/// Reads the stored appearance. A value that does not parse falls back to the
/// default, so a hand-edited or truncated setting cannot leave the application
/// with an unreadable palette.
theme::Appearance loadAppearance();

/// Writes every appearance key. Called only when the viewer saves Settings.
void saveAppearance(const theme::Appearance& appearance);

/// The appearance the running application actually uses. Loaded once at
/// startup and replaced on save, so callers that paint (covers, page glow) and
/// callers that style (the stylesheet) always agree.
const theme::Appearance& currentAppearance();

/// Installs `appearance` as the current one; the caller is responsible for
/// rebuilding the stylesheet afterwards.
void setCurrentAppearance(const theme::Appearance& appearance);

}  // namespace mikai::app
