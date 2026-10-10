// Pure consumer of the requested preference, the catalogue availability, the
// selected runtime and the filter's status snapshot; it invents none. A row may
// never invent a stable id from a row index or enum ordinal, nor claim activation -
// only the filter's status snapshot may report a chain as running.

#pragma once

#include <QtCore/QString>
#include <QtCore/QVector>

#include "native/bundle/NativePrefs.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "native/status/NativeStatus.hpp"

namespace mplayer {
namespace native {
namespace menu {

/// Written as a stable string so the settings dialog and the player share one
/// vocabulary.
enum class Action {
    None,
    TurnOff,
    TurnOn,
    SetBackend,
    SetRecipe,
};

QString actionName(Action action);
/// An unknown name reads as `Action::None`, never as the first enumerator.
Action actionFromName(const QString& name);

struct Row {
    /// Stable identity the current mark follows: `native/off`, `native/on`,
    /// `native/backend/<name>`, `native/recipe/<id>`, `rife/recipe/<id>` for a
    /// configured RIFE recipe, `rife/<model name>` for a missing variant.
    QString id;
    QString label;
    QString tooltip;
    /// Shown but not choosable. A disabled row never emits.
    bool disabled = false;
    /// Leads to another level; activating it leaves the popover open.
    bool submenu = false;
    bool current = false;
    Action action = Action::None;
    /// The backend stable name or the recipe id the row would select.
    QString value;
};

/// The catalogue's own level strings, so the shell routes them without a table.
inline constexpr char kNativeLevelId[] = "native";
inline constexpr char kRifeLevelId[] = "rife";

/// An empty input is not an error: nothing is available yet and the reason says so.
struct Inputs {
    NativePreferences preferences;
    NativeAvailability availability;
    RuntimeSelection runtime;
    NativeStatusView status;
};

bool ownsLevel(const QString& levelId);

/// The tooltip for the root rows this module is authoritative about, or empty when
/// the catalogue's own text stands.
QString categoryTooltip(const QString& levelId, const Inputs& in);

/// The row id the request marks as current; empty for Off, marked on the Off row.
QString currentRowId(const NativePreferences& preferences);

/// Off is always choosable; every provider and model row is disabled with the fact
/// that blocks it.
QVector<Row> nativeRows(const Inputs& in);

/// The configured native RIFE recipes, each routing its exact saved recipe id
/// through `Action::SetRecipe`. One row per stored temporal recipe, so several
/// recipes for the same variant stay separately selectable; a variant with no
/// configured recipe gets one honest disabled row asking for configuration and
/// naming the actual runtime gap. Never fabricates a recipe, factor or model.
QVector<Row> rifeRows(const Inputs& in);

/// The enhancement-tree root rows this module owns: the Native and RIFE
/// submenus. `value` is the exact level id the shell drills into with
/// `level|<value>`; both stay available so the RIFE page can explain itself
/// even with no configured recipe.
QVector<Row> rootRows(const Inputs& in);

/// Any owned level's rows, or empty for a level this module does not own.
QVector<Row> levelRows(const QString& levelId, const Inputs& in);

/// Every field is empty when the snapshot does not state it: an unpublished
/// geometry is unknown, never zero.
struct StatusLines {
    /// A default Off machine gets no native chip at all.
    bool visible = false;
    /// True only when the filter's own snapshot says Active. Never inferred.
    bool active = false;
    QString headline;
    /// The filter's state name, or "unavailable" when no snapshot was read.
    QString state;
    /// All empty when the snapshot does not state them.
    QString provider;
    QString model;
    /// "1920x1080 → 3840x2160", empty when the geometry is not published.
    QString geometry;
    QString fps;
    /// The rational interpolation factor, empty for a spatial-only plan.
    QString interpolation;
    /// The filter's own dropped-frame count, empty when counters are absent.
    QString dropped;
    /// The fact, for every state that is not Active.
    QString reason;
};

StatusLines statusLines(const Inputs& in);

}  // namespace menu
}  // namespace native
}  // namespace mplayer
