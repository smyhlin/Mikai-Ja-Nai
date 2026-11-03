#pragma once

#include "theme/Appearance.hpp"

#include <QString>

class QAbstractButton;

namespace mikai::app::theme {

/// The action hierarchy the whole application styles against. A control states
/// its role once; the sheets decide what that role looks like, so the same
/// decision cannot render as five different buttons on five pages.
///
/// `Primary`   - accent fill, contrast ink, weight 600. One per view.
/// `Secondary` - raised neutral fill, 3:1 boundary, weight 500. The default.
/// `Ghost`     - no fill, accent text. Inline and header actions.
/// `Danger`    - destructive only: danger text on a 1 px danger outline.
enum class ActionRole { Primary, Secondary, Ghost, Danger };

/// The dynamic-property value (`actionRole`) the sheets select on.
QString actionRoleName(ActionRole role);

/// States `button`'s role: sets the dynamic property the sheets select on and
/// re-polishes the widget. It deliberately does not size the control - the
/// sheet owns the role's height, so a role can never fight a size its caller
/// fixed. The sheet paints the role; a caller must not also set a private
/// stylesheet.
void applyActionRole(QAbstractButton* button, ActionRole role);

/// The application-wide Qt stylesheet for one appearance.
QString buildStylesheet(const Appearance& appearance);

/// The appearance the application ships with: dark bed, violet accent, MD
/// corners.
QString buildStylesheet();

/// The resolved colour roles of the main page's header rows. The page's scoped
/// sheet and the glyphs it paints read the same roles, so a theme or accent
/// change cannot leave an icon on the previous surface.
struct SearchHeaderPalette {
    /// The search field's bed and the hairline every control shares.
    QString input;
    QString inputBorder;
    QString border;
    QString borderStrong;
    /// Filters, Teams…, and the Open video menu.
    QString secondary;
    QString secondaryHover;
    QString text;
    /// The passive quota pill and its dot tones. `dotHealthy` is only ever
    /// painted when a response actually carried quota figures.
    QString pillSurface;
    QString pillBorder;
    QString pillText;
    QString dotHealthy;
    QString dotLow;
    QString dotExhausted;
    QString dotUnknown;
    /// The Enter keycap inset at the search field's trailing edge.
    QString keycapSurface;
    QString keycapBorder;
    /// The primary action and the ink that sits on it.
    QString primary;
    QString primaryHover;
    QString onPrimary;
};

SearchHeaderPalette searchHeaderPalette(const Appearance& appearance);

/// Installs the palette Qt draws its own sub-controls from (combo arrows,
/// menu indicators, checkbox ticks) so they match the sheet in every theme.
void applyPalette(const Appearance& appearance);

}  // namespace mikai::app::theme
