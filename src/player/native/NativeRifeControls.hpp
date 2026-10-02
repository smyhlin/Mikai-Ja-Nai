// The Processing tab's RIFE configuration surface: the typed controls that name
// one RIFE request, and the one button that stores it.
//
// The panel is a *configuration* surface, not an execution one. It never claims
// a graph runs: it builds the exact recipe the user described, stores it through
// the user recipe store, and hands the stored id back so the parent combo and
// the existing Save can select it. The request is legal on its own, even though
// every capability is Unavailable in this build - blocking the configuration on
// the missing runtime is exactly the coupling this surface removes.
//
// Three details are load-bearing.
//
//  * No ordinal identity. Variant, order and mode are read back by their typed
//    value, and a reloaded recipe's settings are applied from its fields, never
//    from a remembered index. A combo reordering cannot repoint a request.
//
//  * Combining needs a real, stored, spatial-only recipe. The base is re-read
//    from the store by id at store time, so a removed or edited recipe is refused
//    by name rather than combined from a stale list, and no chain is ever
//    invented to fill an empty selection.
//
//  * No caption and no identity dictates the panel's width. A stored id is one
//    unbroken token with no wrap point, and the choice captions are whole
//    sentences: either one, reserved as a minimum width, would push the whole
//    Processing page past the smallest window and force horizontal panning. The
//    captions are therefore bounded - the combos take whatever width their row
//    gives them, the role labels wrap, and a long choice stays readable in the
//    tooltip and the popup - and the identity lives behind a disclosure row, in a
//    read-only field that shows it verbatim and lets it be copied. Nothing here
//    has to be panned to.

#pragma once

#include <optional>

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include "NativeRecipe.hpp"
#include "NativeTypes.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QToolButton;

namespace mplayer {
namespace native {

class NativeRifeControls : public QWidget {
    Q_OBJECT

public:
    explicit NativeRifeControls(QWidget* parent = nullptr);

    /// The explicit backend a stored request names. Set from the dialog's own
    /// backend combo; the panel never infers one.
    void setRequestedBackend(Backend backend);

    /// Re-reads the stored spatial recipes a combination may name. Also used
    /// after a save so a freshly written base is immediately selectable.
    void refreshStoredRecipes();

    /// Loads the exact temporal values a stored recipe carries and selects the
    /// spatial recipe it was combined with, matched by its chain rather than by
    /// an index. An id this store does not hold, or a recipe without a temporal
    /// stage, leaves the controls exactly as they are.
    void loadStoredRecipe(const QString& recipeId);

    /// The temporal stage the controls describe: exact, unreduced, and with an
    /// explicit order. The scene threshold is parsed from the editor's own text
    /// in the current locale; blank, non-finite or out-of-range text yields no
    /// stage and a named reason, never a value coerced to 0.
    std::optional<TemporalStage> temporalStage(QString* reasonKey = nullptr,
                                               QString* reason = nullptr) const;

signals:
    /// The store's id for a request this panel just persisted. Never an ordinal
    /// and never a name the panel invented.
    void recipeRequested(const QString& recipeId);

private:
    void storeRequest();
    void updateCombineControls();
    /// Shows the named reason the currently selected variant is not Ready, read
    /// from the capability table itself so the surface and the table cannot
    /// disagree.
    void updateCapabilityNote();
    bool combineSelected() const;
    /// The selected base, re-read from the store. Refuses by name when nothing
    /// is selected or the id no longer names a stored spatial-only recipe.
    std::optional<Recipe> selectedBase(QString* reasonKey, QString* reason) const;
    void applyStoredRecipe(const Recipe& recipe);
    QString recipeLabel(const TemporalStage& stage, bool combined) const;
    /// Reveals the exact identity of a stored request behind the panel's
    /// disclosure row, or collapses the row again when there is no id: the field
    /// is never filled with a placeholder that is not an id this store issued.
    void showRequestId(const QString& id);
    /// Shows the status text. `detail` is the exact identity the text describes
    /// (a stored id); it goes to the tooltip - and therefore to the accessible
    /// description - rather than into the label, because an unbroken id would
    /// otherwise set the panel's minimum width.
    void showStatus(bool ok, const QString& text, const QString& detail = QString());

    Backend requestedBackend_ = Backend::TensorRT;

    QComboBox* variant_ = nullptr;
    QCheckBox* ensemble_ = nullptr;
    QSpinBox* factorNum_ = nullptr;
    QSpinBox* factorDen_ = nullptr;
    QLineEdit* threshold_ = nullptr;
    QComboBox* order_ = nullptr;
    QComboBox* mode_ = nullptr;
    QComboBox* base_ = nullptr;
    QPushButton* storeButton_ = nullptr;
    QLabel* status_ = nullptr;
    QToolButton* idToggle_ = nullptr;
    QLineEdit* idEdit_ = nullptr;
};

}  // namespace native
}  // namespace mplayer
