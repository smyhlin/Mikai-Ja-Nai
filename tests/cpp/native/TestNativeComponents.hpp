#pragma once

#include <QtTest/QTest>

/// The pinned TensorRT component catalogue and the store it installs into.
///
/// Five decisions are load-bearing and are exercised here without a network, a
/// GPU or a 180 MB archive: which pack a compute capability selects, which packs
/// one GPU may actually use, whether the pinned table is well formed enough to
/// refuse a re-uploaded asset by digest, a store record that round-trips through
/// its own bytes and is refused when it cannot be read, and which source
/// supplies the builder resource when the bundle and the store disagree.
class TestNativeComponents : public QObject {
  Q_OBJECT

 private slots:
  // Selection is computed from the numbers, never from an index, so both
  // directions are reproducible: a published generation resolves to its own pack
  // and an unpublished one falls back to the PTX closure.
  void selectionUsesTheExactPack();
  void selectionFallsBackToPtxWhenTheReleasePublishesNoPack();

  // `trt-sm{MA}{MI}` cannot express a double-digit minor, so that shape is
  // refused rather than approximated onto a pack that fits another generation.
  void selectionRefusesADoubleDigitMinor();

  // The tab lists only what one GPU may actually use: the runtime plus that
  // GPU's own resource pack, so other generations cannot offer downloads that
  // could never run here.
  void compatiblePacksNameRuntimeAndThisGpuOnly();
  void compatiblePacksAreEmptyWhenTheShapeIsUnusable();

  // A record published from real bytes reads back as installed, and one file
  // removed from under it turns the same record into a named absence.
  void storeRecordRoundTrips();

  // A record carrying a member the reader does not know, or naming a file that
  // escapes the release directory, is refused rather than partially honoured.
  void storeRefusesAnUnknownRecordKey();
  void storeRefusesAnUnsafeRecordPath();

  // The manual check behind the tab's Verify button re-hashes the bytes rather
  // than trusting the recorded size, so a file replaced in place is caught.
  void verifyPackCatchesReplacedBytes();
  void verifyPackAcceptsTheRecordedBytes();
  void installedPackNamesFollowTheRecord();
  void cancelledTensorRtProvisioningDoesNotTouchItsFixtures();
  void cancelledRifeProvisioningDoesNotTouchItsFixtures();

  void upscalingPackReviewsAndQueuesMissingGraphs_data();
  void upscalingPackReviewsAndQueuesMissingGraphs();
  void upscalingSelectionSurvivesRefreshAndMissingGraphsFailVerification();
  void concurrentUnitsSettleOnceEachAndCancelArmsEveryUnit();
  void componentProgressSeparatesCardsAndAggregatesSpatialGraphs();

  // RIFE status trusts only the app-owned record for the exact compiled release
  // and graph inventory; manual verification still hashes the selected graph.
  void rifeGraphWithoutInstallRecordIsMissing();
  void rifeGraphRecordTrustsVerifiedIdentityUntilManualVerify();
  void rifeGraphRecordRequiresTheExactInventory();

  // Where the builder resource comes from: the bundle's own declared inventory
  // wins, the store serves a GPU the bundle cannot describe, neither source is a
  // named refusal, and a machine with no NVIDIA device keeps the bundle's dirs.
  void toolchainPrefersTheBundleResource();
  void toolchainUsesTheStoreWhenTheBundleCarriesNoResourceForThisGpu();
  void toolchainRefusesWhenNeitherSourceHasTheResource();
  void toolchainKeepsTheBundleDirsWithoutAnNvidiaDevice();
  void componentDirectoryAvailabilityTracksMissingAndInstalledResources();
  void gpuResourceStatusRequiresACompatiblePack();
};