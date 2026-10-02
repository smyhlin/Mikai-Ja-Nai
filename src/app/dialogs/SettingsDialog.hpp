#pragma once

#include <native/NativePrefs.hpp>
#include <native/NativeRifeControls.hpp>
#include <player/PresetShortcuts.hpp>

#include <QDialog>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>

class QCheckBox;
class QComboBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTimer;

namespace mikai::backend {
class MikaiClient;
}

namespace mikai::app {

class Runner;

/// API key, live quota readout, the home-page feed switches, the Anime4K preset
/// hotkeys and the native processing preferences, in tabs: the General tab is
/// the account and the home page, the Presets tab the Anime4K keys, and the
/// Processing tab the native track.
///
/// Preset and native bindings are only written once every binding validates, so
/// a reserved, duplicated or cross-space key cannot half-apply. Only native
/// fields the user actually changed are written, so an untouched block - or one
/// this build does not read - survives a General save.
///
/// The Processing tab is honest by construction: it persists the *requested*
/// native preference and shows eligibility separately from activation, which only
/// the filter's own status can report. Nothing here pretends a preparation, an
/// engine or an import exists.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    SettingsDialog(backend::MikaiClient& client, Runner& runner, QWidget* parent = nullptr);
    ~SettingsDialog() override;

    /// True when the feed selection now differs from what was stored when the
    /// dialog opened. The caller refreshes the home feeds only then, so a native
    /// or language preference change never resets the feed state or the cache.
    bool feedsChanged() const;

private:
    /// One editable preset binding: the stable catalogue id the shortcut is
    /// stored under, the label shown to the user, and the editor holding it.
    struct PresetRow {
        QString id;
        QString label;
        QKeySequenceEdit* edit = nullptr;
    };

    QWidget* buildGeneralTab();
    QWidget* buildPresetsTab();
    QWidget* buildProcessingTab();
    void testToken();
    void refreshQuota();

    /// The bindings every current row holds, minus the field named by `exclude`.
    /// Seeded by the native request too, so the two key spaces collide across
    /// each other and not only within one tab.
    QMap<QString, mplayer::shell::ShortcutClaim> shortcutClaims(
        const QKeySequenceEdit* exclude) const;

    /// Collects the preset bindings and reports the first one that must not be
    /// saved: a reserved player key or a combination another binding already
    /// holds. Empty on success, in which case `presets` holds every non-empty
    /// binding keyed by the preset id.
    QString collectPresetShortcuts(QMap<QString, QString>* presets) const;

    /// The same check for the one native binding. Empty on success, in which
    /// case `toggleProcessing` holds its PortableText form or is cleared.
    QString collectNativeShortcuts(QString* toggleProcessing) const;

    void showPresetError(const QString& message);
    void showNativeError(const QString& message);
    void refreshNativeStatus();
    void refreshRuntimeStatus();

    /// Rebuilds the recipe choices from the current availability snapshot and
    /// selects `selection`. The stored request is not written here: only Save
    /// patches it, and Cancel leaves it exactly as it was.
    void populateRecipeChoices(const QString& selection);

    /// A recipe the user just registered in the assets panel. Re-reads what the
    /// catalogue now offers and selects the new id in the combo; the id reaches
    /// `NativePreferences` only through the existing Save patch.
    void applyRequestedRecipe(const QString& recipeId);

    void save();

    backend::MikaiClient& client_;
    Runner& runner_;
    QLineEdit* token_ = nullptr;
    QComboBox* language_ = nullptr;
    QLabel* quota_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* testButton_ = nullptr;
    QMap<QString, QCheckBox*> feedBoxes_;
    QTimer* quotaTimer_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QLabel* presetError_ = nullptr;
    QVector<PresetRow> presetRows_;

    // Native processing. The request is loaded once, read-only, and the widgets
    // start from it; the availability snapshot is what the status explains.
    mplayer::native::NativePreferences nativePreferences_;
    mplayer::native::NativeAvailability nativeAvailability_;
    QCheckBox* nativeEnabled_ = nullptr;
    QComboBox* nativeBackend_ = nullptr;
    QComboBox* nativeRecipe_ = nullptr;
    mplayer::native::NativeRifeControls* nativeRife_ = nullptr;
    QLineEdit* customModelsDir_ = nullptr;
    QKeySequenceEdit* nativeToggleEdit_ = nullptr;
    QComboBox* runtimeChoice_ = nullptr;
    QLabel* nativeStatus_ = nullptr;
    QLabel* runtimeStatus_ = nullptr;
    QLabel* nativeError_ = nullptr;

    /// The feed selection as loaded, so `feedsChanged()` compares against what
    /// the user actually saw rather than the widget defaults.
    QSet<QString> initialFeeds_;
};

}  // namespace mikai::app
