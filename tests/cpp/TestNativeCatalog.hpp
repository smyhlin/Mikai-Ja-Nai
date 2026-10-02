#pragma once

#include <QtTest/QTest>

class TestNativeCatalog : public QObject {
  Q_OBJECT

 private slots:
  // Runtime location: an absent runtime is named, never searched for on PATH.
  void anAbsentRuntimeIsNamedNotSearched();

  // The store: identity, atomic publish, integrity, and what is never written.
  void aPublishedModelIsStoredUnderItsContentDigest();
  // First use: the default owned root is eligible before it exists, and a
  // writable directory outside the app-owned tree is still not a store. Walking
  // back out of the tree, or through a reparse point, is neither.
  void aFreshOwnedStoreRootIsEligibleBeforeItExists();
  void aTraversalOrReparsePointedRootIsNotAStore();
  // What the store's own read says: a fresh owned store is honestly empty and
  // says nothing about its absent root, a successful import makes the reload the
  // store as it now is rather than the pre-import snapshot, and a root that is
  // not an owned store is still refused with no rows.
  void aFreshOwnedStoreReadsEmptyAndReloadsWithoutAMissingRootDiagnostic();
  // Backend support is model compatibility, carried by the row's own document:
  // exactly the provisioner's vouch, never this build's vocabulary.
  void aRowsRecordedBackendSupportIsExactlyWhatWasVouched();
  void aReimportUpdatesOnlyTheRecordedSupportAndNeverErasesIt();
  void twoFilesSharingABasenameGetTwoIdentities();
  void republishingTheSameGraphIsIdempotent();
  void anUnsupportedDtypeIsRecordedNotExecuted();
  void aDynamicGraphWhoseReferenceDidNotRunIsRecordedUnavailable();
  void aRefusedBoundaryLeavesTheStoreUnchanged();
  void anImportCancelledBeforePublishingWritesNothing();
  void anEditedOrRemovedSourceDoesNotInvalidateTheOwnedCopy();
  void aTamperedCopyIsReportedUnavailableNotDropped();
  void anEditedManifestIdentityIsReportedNotTrusted();

  // Actual inspection through the ORT C API, over graphs generated in-process.
  void aGeneratedDynamicGraphResolvesItsActualGeometry();
  void aGeneratedFixedGraphIsNotExecuted();
  void aTruncatedGraphIsRefusedAndPublishesNothing();
  void aTwoInputGraphIsRefusedNamingTheCounts();
  // The reference's own boundary is checked, not assumed: a run that returns an
  // unbindable N/C/rank yields no geometry and no proof, and a declared symbolic
  // batch is settled only by a real run's actual 1x3xHxW.
  void aMalformedReferenceBoundaryYieldsNoGeometryOrProof();
  void aGeneratedSymbolicOutputBatchIsProvedByARealReference();

  // The acquired original graphs, when the machine has the assets.
  void theRealDynamicGraphLearnsItsScaleAtStageGeometry();
  void theRealHdGraphKeepsItsDeclaredSymbolicBatchAndProvesTwoBy();

  // The actual inspected-profile adapter: M13's dynamic resolver over the owned
  // store. Real generated graphs and real CPU runs, never a mocked answer.
  void anOwnedDynamicStageResolvesThroughThePlanner();
  void aReloadedRowKeepsItsRecordedBackendsAndTheRuntimeIsStillRequired();
  void twoSameNamedOwnedGraphsResolveTheirOwnBytes();
  void aMovedOrRemovedSourceDoesNotStopTheOwnedResolution();
  void aChangedSourceReimportsAsADistinctRowAndTheOldOneStaysValid();
  void aCorruptOwnedCopyRefusesTheOwnedResolution();
  void aMutatedOwnedCopyRefusesAfterItWasCached();
  void aRowWhoseBytesDeclareAnotherBoundaryIsRefused();

  // M11 regression coverage: boundary edges for declared profile inspection.
  void fp32GraphIsRecordedUnsupportedWithDtypeReason();
  void bf16GraphIsRecordedUnsupportedWithDtypeReason();
  void truncatedByteArrayRefusesWithReasonKey();
  void corruptProtobufTailRefusesWithReason();
  void zeroOrMultipleIOCountsAreRefusedByNames();
  void rank3AndRank5GraphsAreRefusedNamingShape();
  void declaredInputBatchNotOneIsRefused();
  void channelC1GraphIsRefused();
  void dynamicGraphAtFixedStageProvesReferenceAndKeepsDeclaredMeta();
};
