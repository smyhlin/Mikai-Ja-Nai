#pragma once

#include <dialogs/NativeAssetsPanel.hpp>
#include <dialogs/NativeRifeControls.hpp>
#include <native/bundle/NativePrefs.hpp>
#include <player/Anime4K.hpp>
#include <player/PresetShortcuts.hpp>
#include <player/ProcessingSettingsIntent.hpp>

#include <QDialog>
#include <functional>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QEvent;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
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

    /// The Processing tab, brought into view. A remedy that only wants the
    /// existing surface seen reaches this and nothing else: no preparation is
    /// armed, no request is issued and no store is read.
    void showProcessingTab();

    /// The Processing tab with the existing backend choice focused, for a
    /// remedy whose answer is "choose the processing provider". It changes
    /// nothing; Save remains the only writer of the request.
    void focusNativeProviderChoice();

    /// The Processing tab with the existing preparation command invoked once,
    /// for a remedy whose answer is "prepare". The command is the panel's own
    /// `prepareArtifact()`, so a request it cannot submit states its named
    /// refusal in the panel's own status line and adds no second truth here.
    void triggerNativePrepare();

    /// Routes a typed remedy to the surface that answers it. Prepare runs the
    /// command; Providers only brings the choice into view.
    void applyProcessingIntent(mplayer::shell::ProcessingSettingsIntent intent);

    /// The backend a preparation job should align the core to: the panel's
    /// UNSAVED provider selection when one is made, otherwise the stored
    /// preference. Never the effective plan's provider - the desired backend may
    /// differ from whatever is currently admitted.
    mplayer::native::Backend nativePreparationDesiredBackend() const;
    void setNativePreparationDesiredBackend(
        std::function<mplayer::native::Backend()> provider);

    /// The live source-context provider the preparation panel pulls at readiness
    /// and click time: the active playback core's observed geometry and device,
    /// or nothing when nothing has been observed. The application owner installs
    /// it from the running player; probes may install their own. Until one is
    /// installed, preparation refuses by name rather than guessing.
    void setNativePreparationContextProvider(NativePreparationContextProvider provider);

    /// True when an actual playing or paused video surface exists that a
    /// preparation job may probe and capture from. The application owner wires
    /// it; it is what lets Prepare be armed before any device fact is observed.
    void setNativePreparationSourcePresence(NativePreparationSourcePresence presence);

    /// Asks the application to arm the live core's native probe for the current
    /// preparation job and deliver the observed context asynchronously. The panel
    /// calls it only after a Prepare click, never on opening settings.
    void setNativePreparationContextRequest(NativePreparationContextRequest request);

    /// Asks the application to capture one actual decoded frame for the current
    /// preparation job. The panel calls it only after a Prepare click.
    void setNativePreparationFrameRequest(NativePreparationFrameRequest request);

    /// The application's push answer to one probe request: the context the live
    /// core observed, or a named refusal. Forwarded to the panel and matched to
    /// the current job by its generation.
    void deliverNativePreparationContext(const mplayer::native::NativePreparationContext& context);
    void failNativePreparationContext(const QString& reasonKey, const QString& reason);

    /// The application's push answer to one capture request: the actual decoded
    /// frame the core observed, or a named refusal. Forwarded to the panel and
    /// matched to the frame index the job is waiting for.
    void deliverNativePreparationFrame(int frameIndex, const mplayer::native::FrameFixture& frame,
                                       quint64 sourceEpoch, double observedPosition);
    void failNativePreparationFrame(int frameIndex, const QString& reasonKey,
                                    const QString& reason);

private:
    /// One editable preset binding: the stable catalogue id the shortcut is
    /// stored under, the label shown to the user, and the editor holding it.
    struct PresetRow {
        QString id;
        /// The catalogue label, refreshed from `mode` on a LanguageChange: the
        /// preset name is translated catalogue text, so keeping the copy the row
        /// was built with would leave the page in the language it opened in.
        QString label;
        QKeySequenceEdit* edit = nullptr;
        QLabel* name = nullptr;
        /// The catalogue entry behind the row. The label and the hint are read
        /// back from it rather than stored, so one catalogue lookup serves both
        /// and neither can go stale.
        mplayer::anime4k::Mode mode = mplayer::anime4k::Mode::Off;
    };

    struct PresetGroupHeading {
        QString category;
        QLabel* label = nullptr;
    };

    /// One captioned row the Processing tab builds through its own `addRow`
    /// lambda: the label it shows, and the key of the caption behind it. A
    /// retranslation restates the caption in place rather than rebuilding the
    /// row, because rebuilding a row discards whatever the user typed in it.
    struct ProcessingRowCaption {
        QLabel* label = nullptr;
        QString captionKey;
    };

    QWidget* buildGeneralTab();
    QWidget* buildPresetsTab();
    QWidget* buildProcessingTab();
    void testToken();
    void refreshQuota();

    void changeEvent(QEvent* event) override;
    /// Re-reads every static caption in place after a LanguageChange: window
    /// and tab titles, buttons, headings, hints, row labels, combo items (by
    /// stable data, never by index), placeholders, tooltips and accessible
    /// names. Edits, checks, stable ids, exact factor/threshold values, the
    /// current tab, scrollbar positions and focus survive; no store is
    /// re-read or written merely to translate. Dynamic result texts (token
    /// check, shortcut conflicts, store reasons) keep their verdict until the
    /// next action produces them in the new language.
    void retranslateUi();
    void retranslateGeneralTab();
    void retranslatePresetsTab();
    void retranslateProcessingTab();

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
    /// A refusal that belongs to the General page - the token, the feeds or the
    /// interface language. It names what stayed unchanged and returns the dialog
    /// there, so the message is read next to the control it is about.
    void showGeneralError(const QString& message);
    void refreshNativeStatus();
    void refreshRuntimeStatus();

    /// The availability this dialog may show, derived from the live core's own
    /// factory observation when the installed context provider carries one and
    /// from the cold no-observation default otherwise. The provider is read, not
    /// probed: opening settings never arms a probe or invents a source.
    mplayer::native::NativeAvailability currentSourceAvailability() const;

    /// The owned model rows the assets panel last read, as the catalogue the RIFE
    /// surface resolves its pinned variants against. The panel is the only
    /// reader of the store; this hands its rows over rather than re-reading.
    mplayer::native::CatalogView ownedModelCatalog() const;

    /// Pushes `ownedModelCatalog()` at the RIFE surface and re-reads the note, so
    /// a freshly imported pinned graph becomes visible without reopening settings.
    void refreshRifeCatalog();

    /// What the pending choice costs, stated in the dialog itself: a module is
    /// pinned once per launch, so a choice that differs from the loaded one is
    /// saved and applied after a restart rather than swapped in place.
    void refreshRuntimeHint();

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
    QVector<PresetGroupHeading> presetGroupHeadings_;

    // Retranslation handles: every static caption the dialog paints, so a
    // LanguageChange updates them without rebuilding the dialog. Dynamic
    // result labels (presetError_, nativeError_, status_) are intentionally
    // absent: they are re-issued by the next validation in the new language.
    QDialogButtonBox* buttonBox_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QWidget* generalTab_ = nullptr;
    QWidget* presetsTab_ = nullptr;
    QWidget* processingTab_ = nullptr;
    QScrollArea* generalScroll_ = nullptr;
    QScrollArea* presetsScroll_ = nullptr;
    QScrollArea* processingScroll_ = nullptr;
    QLabel* tokenHeading_ = nullptr;
    QLabel* tokenDescription_ = nullptr;
    QLabel* tokenLink_ = nullptr;
    QLabel* quotaTitle_ = nullptr;
    QLabel* feedsTitle_ = nullptr;
    QLabel* feedsHint_ = nullptr;
    QLabel* languageTitle_ = nullptr;
    QLabel* languageHint_ = nullptr;
    QLabel* presetsHeading_ = nullptr;
    QLabel* presetsDescription_ = nullptr;
    QLabel* nativeHeading_ = nullptr;
    QLabel* nativeDescription_ = nullptr;
    QLabel* backendRowLabel_ = nullptr;
    QLabel* recipeRowLabel_ = nullptr;
    QLabel* modelsDirRowLabel_ = nullptr;
    QLabel* toggleRowLabel_ = nullptr;
    QLabel* runtimeModuleRowLabel_ = nullptr;
    QLabel* runtimeHeading_ = nullptr;
    QLabel* rifeHeading_ = nullptr;
    QLabel* rifeHint_ = nullptr;
    QVector<ProcessingRowCaption> processingRowCaptions_;
    /// The stable id the recipe combo was given for the synthetic
    /// "not available" entry, so a retranslation can restate that one entry's
    /// caption without rebuilding the list and losing the user's selection.
    QString unavailableRecipeId_;
    /// True only for the duration of a retranslate pass. It is what stops the
    /// status refreshes the pass calls from re-reading the owned stores: a
    /// language switch changes words, and redrawing the same rows from the same
    /// facts would drop an in-flight job's visible state for nothing.
    bool retranslating_ = false;
    /// Whether the store held an explicit language preference when this dialog
    /// opened. A refused save has to put that back exactly: a preference the user
    /// never chose must not be pinned by a refusal.
    bool languagePreferenceWasStored_ = false;
    /// The language preference the running application last installed, so Save
    /// knows whether the choice it is about to write is one this process does
    /// not already have. Never used to skip anything: it only decides whether
    /// an already-active choice needs applying at all.
    QString appliedLanguagePreference_;

    // Native processing. The request is loaded once, read-only, and the widgets
    // start from it; the availability snapshot is what the status explains.
    mplayer::native::NativePreferences nativePreferences_;
    mplayer::native::NativeAvailability nativeAvailability_;
    QCheckBox* nativeEnabled_ = nullptr;
    QComboBox* nativeBackend_ = nullptr;
    QComboBox* nativeRecipe_ = nullptr;
    NativeRifeControls* nativeRife_ = nullptr;
    QLineEdit* customModelsDir_ = nullptr;
    QKeySequenceEdit* nativeToggleEdit_ = nullptr;
    QComboBox* runtimeChoice_ = nullptr;
    QLabel* runtimeHint_ = nullptr;
    QLabel* nativeStatus_ = nullptr;
    QLabel* runtimeStatus_ = nullptr;
    QLabel* nativeError_ = nullptr;

    /// The assets panel this dialog owns. The preparation providers are
    /// forwarded to it live, so one installed after construction still takes.
    NativeAssetsPanel* assetsPanel_ = nullptr;
    NativePreparationContextProvider preparationContextProvider_;
    NativePreparationSourcePresence preparationSourcePresence_;
    NativePreparationContextRequest preparationContextRequest_;
    std::function<mplayer::native::Backend()> preparationDesiredBackendProvider_;
    NativePreparationFrameRequest preparationFrameRequest_;

    /// The feed selection as loaded, so `feedsChanged()` compares against what
    /// the user actually saw rather than the widget defaults.
    QSet<QString> initialFeeds_;
};

}  // namespace mikai::app
