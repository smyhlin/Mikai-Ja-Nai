#pragma once

#include <optional>

#include <QtCore/QLocale>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>
#include <QtWidgets/QWidget>

#include <native/models/NativeRecipe.hpp>
#include <native/status/NativeTypes.hpp>

class QCheckBox;
class QComboBox;
class QDoubleValidator;
class QEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace mikai::app {

class NativeRifeControls : public QWidget {
    Q_OBJECT

public:
    explicit NativeRifeControls(QWidget* parent = nullptr);

    /// The explicit backend a stored request names. Set from the dialog's own
    /// backend combo; the panel never infers one.
    void setRequestedBackend(mplayer::native::Backend backend);

    /// The owned model catalogue the pinned RIFE variants are resolved against.
    /// The resolver joins a variant's content pin to a real owned temporal row;
    /// without a catalogue every variant is honestly named as missing rather than
    /// assumed present. Never a claim that a chain runs.
    void setCatalog(const mplayer::native::CatalogView& catalog);

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
    std::optional<mplayer::native::TemporalStage> temporalStage(QString* reasonKey = nullptr,
                                                                QString* reason = nullptr) const;

signals:
    /// The store's id for a request this panel just persisted. Never an ordinal
    /// and never a name the panel invented.
    void recipeRequested(const QString& recipeId);

protected:
    void changeEvent(QEvent* event) override;

private:
    /// Restates every static caption in place after a LanguageChange: the
    /// accessible names, the role labels, the choice captions (through their own
    /// stable data, never their index), the button, the disclosure caption and
    /// the tooltip that carries a whole choice. The typed request the user has
    /// described - variant, ensemble, the exact rational factor, the exact
    /// threshold text, the order, the shape and the selected base - is never
    /// rewritten, and neither is the store re-read: a language switch changes
    /// words, not the request.
    void retranslateUi();

    void storeRequest();
    /// Rebuilds the variants for the selected revision from the exact pin table.
    void updateVariantChoices();
    /// Keeps the ensemble flag within combinations actually pinned for the
    /// selected revision and variant.
    void updateEnsembleChoice();
    void updateCombineControls();
    /// Shows the named reason the currently selected variant is not Ready, read
    /// from the capability table itself so the surface and the table cannot
    /// disagree.
    void updateCapabilityNote();
    bool combineSelected() const;
    /// The selected base, re-read from the store. Refuses by name when nothing
    /// is selected or the id no longer names a stored spatial-only recipe.
    std::optional<mplayer::native::Recipe> selectedBase(QString* reasonKey, QString* reason) const;
    void applyStoredRecipe(const mplayer::native::Recipe& recipe);
    QString recipeLabel(const mplayer::native::TemporalStage& stage, bool combined) const;
    void showStatus(bool ok, const QString& text);

    mplayer::native::Backend requestedBackend_ = mplayer::native::Backend::TensorRT;

    /// The owned rows the pinned variants are resolved against; empty until the
    /// settings dialog supplies them, which the panel names as an absent store.
    mplayer::native::CatalogView catalog_;

    /// The role label of every captioned row, in the order they were added. A
    /// retranslation restates these in place; a rebuilt row would reset the field
    /// it labels.
    QVector<QLabel*> roleLabels_;
    /// The caption key of each entry in `roleLabels_`, so a restated label is
    /// looked up in the current language rather than kept as the string it was
    /// built with.
    QStringList roleCaptionKeys_;

    /// The locale the threshold editor's current text was rendered in. It is
    /// recorded on every write, because a retranslation happens after the
    /// default locale has already moved on, and a number the user typed under
    /// the previous decimal separator would otherwise no longer read back as the
    /// number it was.
    QLocale thresholdLocale_;

    QComboBox* version_ = nullptr;
    QComboBox* variant_ = nullptr;
    QCheckBox* ensemble_ = nullptr;
    QSpinBox* factorNum_ = nullptr;
    QSpinBox* factorDen_ = nullptr;
    QLineEdit* threshold_ = nullptr;
    QDoubleValidator* thresholdValidator_ = nullptr;
    QComboBox* order_ = nullptr;
    QComboBox* mode_ = nullptr;
    QComboBox* base_ = nullptr;
    QPushButton* storeButton_ = nullptr;
    QLabel* status_ = nullptr;
};

}  // namespace mikai::app
