#pragma once

#include <QtTest/QTest>

/// The runtime selection the app performs at startup: the stored choice, the
/// one-module-per-process rule, the recovery latch, and the refusal a missing
/// or corrupt native runtime takes.
///
/// The first cases never load a module, so they run before the live ones, which
/// pin this process's module. QTest runs the slots in declaration order, which
/// is why they share one class instead of several.
class TestRuntimeSelection : public QObject {
    Q_OBJECT

 private slots:
    void anAbsentOrCorruptStoredChoiceReadsAsStandard();
    void theStoredChoiceRoundTripsThroughTheIsolatedSettings();
    void recoveryIsOneAttemptAndNeverReloads();
    void aNativeDescriptorIsNeverLoadedFromItsLabelAlone();
    void aRefusedNativeRequestFallsBackToStandardAndNamesWhy();
    void aResolvedNativeHandoffCarriesItsDependencyClosureAndProtocol();
    void aStaleOrForgedBundleIsRefusedBeforeTheLoader();
    void publishingAStockRuntimeNeverTouchesTheSourceOrTheReferenceInstall();
    void forkIdentityRequiresTheValidatedModuleAndActualApi();

    // The M07 loader handoff and the M08 failure surfaces. These stay ahead of
    // the live cases because none of them loads a module: once one is pinned the
    // whole process keeps it, so these have to run while nothing is selected.
    void theLoaderPinsExactlyTheValidatedClosure();
    void anUnresolvedSelectionNeverReachesTheLoaderWithACapabilityClaim();
    void theUnavailablePlaybackLineNamesTheSameReasonAsTheStatusLine();

    void selectingThePackagedForkLoadsWithoutMutation();
    void aDifferentChoiceNeverHotSwapsThePinnedModule();
};
