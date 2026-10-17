#pragma once

#include <dialogs/NativeAssetsPanel.hpp>
#include <dialogs/NativeRifeControls.hpp>
#include <native/bundle/NativePrefs.hpp>
#include <player/shaders/Anime4K.hpp>
#include <player/shaders/PresetShortcuts.hpp>
#include <player/shell/ProcessingSettingsIntent.hpp>

#include <QDialog>
#include <QElapsedTimer>
#include <QPointer>
#include <functional>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QEvent;
class QFrame;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace mikai::backend {
class MikaiClient;
}

namespace mikai::app {

class Runner;
class NativeComponentProvisioner;
class ComponentStoreTab;

/// API key, live quota readout, the home-page feed switches, Anime4K preset
/// hotkeys, runtime choice and private Native processing preferences, in tabs.
/// Standard Processing keeps the private Native editor out of view; selecting
/// Native deliberately exposes its separate recipe and preparation workflow.
/// Official AnimeJaNai choices remain in the player menu and Components tab.
///
/// Preset and native bindings are only written once every binding validates, so
/// a reserved, duplicated or cross-space key cannot half-apply. Only native
/// fields the user actually changed are written, so an untouched block - or one
/// this build does not read - survives a General save.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    /// `adoptedComponents` is the app-owned Components tab. The dialog shows it
    /// and gives it back, still running, when the dialog closes. Null builds a
    /// tab this dialog owns and destroys, which is what probes use.
    SettingsDialog(backend::MikaiClient& client, Runner& runner, QWidget* parent = nullptr,
                   ComponentStoreTab* adoptedComponents = nullptr);
    ~SettingsDialog() override;

    /// True when the feed selection now differs from what was stored when the
    /// dialog opened. The caller refreshes the home feeds only then, so a native
    /// or language preference change never resets the feed state or the cache.
    bool feedsChanged() const;

    /// The Processing tab, brought into view. A remedy that only wants the
    /// existing surface seen reaches this and nothing else: no preparation is
    /// armed, no request is issued and no private store is read.
    void showProcessingTab();

    /// The Processing tab with the backend choice focused when the private
    /// Native workflow is visible, or the runtime choice otherwise. It changes
    /// nothing; Save remains the only writer of the request.
    void focusNativeProviderChoice();

    /// The Processing tab with the preparation command invoked once when the
    /// private Native workflow is visible. Otherwise it focuses the runtime
    /// choice so a Standard user can deliberately select Native first.
    void triggerNativePrepare();

    /// Routes a typed remedy to the surface that answers it. Prepare runs only
    /// in the visible Native workflow; otherwise it focuses runtime selection.
    /// Providers focuses the private backend when visible, or runtime selection.
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

    /// The TensorRT component provisioner the Processing tab's assets panel
    /// drives. Optional and null-safe; forwarded to the panel after construction.
    void setComponentProvisioner(NativeComponentProvisioner* provisioner);

    /// Brings the Components tab into view: the downloadable backend elements,
    /// their install state and the manual Verify. Changes nothing by itself.
    void showComponentsTab();
    /// Starts the Components tab's recommended download without a confirmation.
    void installRecommended();


    /// Asks the in-flight engine build to stop. False when nothing was running.
    /// The request reaches the worker's own cancellation gate; the GUI thread
    /// never waits for it.
    bool cancelNativePreparation();

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

signals:
    /// The viewer navigated to the Components tab.
    void componentsTabOpened(SettingsDialog* dialog);

    /// The engine build the Processing tab started, forwarded so a surface that
    /// outlives this dialog - the player's preparation window - can follow it.
    /// The dialog owns no build of its own; these are the panel's own events.
    void nativePreparationStarted();
    void nativePreparationProgress(int stageIndex, int stageCount, const QString& phase,
                                   const QString& detail);
    void nativePreparationFinished(bool ok);

private:
    /// One editable preset binding: the stable catalogue id the shortcut is
    /// stored under, the label shown to the user, and the editor holding it.
    struct PresetRow {
        QString id;
        /// The catalogue label, refreshed from `mode` on a LanguageChange: the
        /// preset name is translated catalogue text, so keeping the copy the row
        /// was built with would leave the page in the language it opened in.
        QString label;
        QString category;
        QKeySequenceEdit* edit = nullptr;
        QLabel* name = nullptr;
        QWidget* container = nullptr;
        /// The catalogue entry behind the row. The label and the hint are read
        /// back from it rather than stored, so one catalogue lookup serves both
        /// and neither can go stale.
        mplayer::anime4k::Mode mode = mplayer::anime4k::Mode::Off;
    };

    struct PresetGroupHeading {
        QString category;
        QLabel* label = nullptr;
    };

    QWidget* buildGeneralTab();
    QWidget* buildPresetsTab();
    QWidget* buildProcessingTab();
    QWidget* buildComponentsTab();
    QWidget* buildBenchmarkTab();
    void runBenchmark();
    void filterPresetRows(const QString& query);
    /// Resolves one benchmark clip name against the dist layout, the source tree
    /// beside the build directory, and the current working directory. Empty when
    /// the clip is nowhere to be found.
    static QString locateBenchmarkClip(const QString& name);
    /// Repopulates the grouped benchmark model tree from the live Anime4K,
    /// AnimeJaNai, spatial and RIFE catalogues. Called when the tab is built and
    /// whenever it is re-shown, so a newly acquired model appears immediately.
    void refreshBenchmarkCatalogue();
    /// The catalogue id carried by a checkable benchmark model row.
    QString benchmarkModelId(const QTreeWidgetItem* item) const;
    /// The clip file name behind one clip checkbox.
    QString benchmarkClipName(const QCheckBox* checkbox) const;
    void updateBenchmarkSelectionSummary();
    void filterBenchmarkModels(const QString& query);

    /// Detaches an adopted Components tab before this dialog deletes its children.
    void releaseAdoptedComponentsTab();
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
    void retranslateBenchmarkTab();

    /// The bindings every current row holds, minus the field named by `exclude`.
    QMap<QString, mplayer::shell::ShortcutClaim> shortcutClaims(
        const QKeySequenceEdit* exclude) const;

    /// Collects the preset bindings and reports the first one that must not be
    /// saved: a reserved player key or a combination another binding already
    /// holds. Empty on success, in which case `presets` holds every non-empty
    /// binding keyed by the preset id.
    QString collectPresetShortcuts(QMap<QString, QString>* presets) const;

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
    QLineEdit* presetFilterEdit_ = nullptr;
    QPushButton* clearAllPresetsButton_ = nullptr;
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
    QWidget* componentsTab_ = nullptr;
    QWidget* benchmarkTab_ = nullptr;
    QScrollArea* benchmarkScroll_ = nullptr;
    QVector<QCheckBox*> benchmarkClipChecks_;
    QLineEdit* benchmarkModelSearch_ = nullptr;
    QTreeWidget* benchmarkModelTree_ = nullptr;
    QCheckBox* benchmarkRawBox_ = nullptr;
    QPushButton* benchmarkRunButton_ = nullptr;
    QPushButton* benchmarkSelectAllClips_ = nullptr;
    QPushButton* benchmarkSelectAllModels_ = nullptr;
    QProgressBar* benchmarkProgress_ = nullptr;
    QProgressBar* benchmarkEngineProgress_ = nullptr;
    QTimer* benchmarkEngineTimer_ = nullptr;
    QElapsedTimer benchmarkEngineBuildElapsed_;
    QStackedWidget* benchmarkResultStack_ = nullptr;
    QTableWidget* benchmarkResults_ = nullptr;
    QLabel* benchmarkEmptyState_ = nullptr;
    QLabel* benchmarkActivity_ = nullptr;
    QLabel* benchmarkEngineElapsedLabel_ = nullptr;
    QLabel* benchmarkNote_ = nullptr;
    QLabel* benchmarkHeading_ = nullptr;
    QLabel* benchmarkDescription_ = nullptr;
    QLabel* benchmarkVideoTitle_ = nullptr;
    QLabel* benchmarkModelTitle_ = nullptr;
    QLabel* benchmarkClipCount_ = nullptr;
    QLabel* benchmarkModelCount_ = nullptr;
    QLabel* benchmarkRunSummary_ = nullptr;
    QLabel* benchmarkRunStatus_ = nullptr;
    QLabel* benchmarkResultsTitle_ = nullptr;
    QLabel* benchmarkResultDetail_ = nullptr;
    bool benchmarkSelectionUpdating_ = false;
    bool benchmarkRunning_ = false;
    QScrollArea* generalScroll_ = nullptr;
    QScrollArea* presetsScroll_ = nullptr;
    QScrollArea* processingScroll_ = nullptr;
    QScrollArea* componentsScroll_ = nullptr;
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
    QLabel* runtimeModuleRowLabel_ = nullptr;
    QLabel* rifeHeading_ = nullptr;
    QFrame* nativeConfigCard_ = nullptr;
    QFrame* nativeAssetsCard_ = nullptr;
    QFrame* nativeRifeCard_ = nullptr;
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
    /// The exact preference stored when the dialog opened, which may differ from
    /// the language currently installed in-process. Restored verbatim on refusal.
    QString storedLanguagePreference_;

    // Native processing. The request is loaded once; backend and recipe widgets
    // start from it, and the availability snapshot is what the status explains.
    mplayer::native::NativePreferences nativePreferences_;
    mplayer::native::NativeAvailability nativeAvailability_;
    QComboBox* nativeBackend_ = nullptr;
    QComboBox* nativeRecipe_ = nullptr;
    NativeRifeControls* nativeRife_ = nullptr;
    QComboBox* runtimeChoice_ = nullptr;
    QLabel* runtimeHint_ = nullptr;
    QLabel* nativeStatus_ = nullptr;
    QLabel* nativeError_ = nullptr;

    /// The assets panel this dialog owns. The preparation providers are
    /// forwarded to it live, so one installed after construction still takes.
    NativeAssetsPanel* assetsPanel_ = nullptr;
    NativeComponentProvisioner* componentProvisioner_ = nullptr;
    ComponentStoreTab* componentStoreTab_ = nullptr;
    /// The Components tab handed in at construction, when this dialog is showing
    /// the app-owned one instead of a tab it created.
    ComponentStoreTab* adoptedComponents_ = nullptr;
    /// Parent that tab returns to. Kept separately so a release still happens
    /// when that parent is the main window; a QPointer because the home can go
    /// away while the dialog is open, and reparenting onto freed memory is a
    /// use-after-free.
    QPointer<QWidget> adoptedComponentsHome_;
    bool adoptedComponentsTab_ = false;
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
