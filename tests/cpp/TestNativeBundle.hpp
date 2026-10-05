#pragma once

#include <QtTest/QTest>

class TestNativeBundle : public QObject {
  Q_OBJECT

 private slots:
  void staticExportsDistinguishTheCoreFromAHelper();
  void malformedExportsNeverClassifyAnImageAsTheCore_data();
  void malformedExportsNeverClassifyAnImageAsTheCore();
  void aForwardedClientApiIsRefusedWithoutResolvingIt();
  void aValidStandardBundleIsReadAndValidated();
  void anUnknownSchemaIsRefusedNotGuessed();
  void anUnknownMemberIsRefusedNotIgnored();
  void aMissingRequiredMemberIsRefused();
  void anUnknownKindIsRefusedNotDefaulted();
  void aMalformedVersionIsRefused();
  void aTraversingFilePathIsRefused();
  void anAbsoluteOrBackslashedPathIsRefused();
  void theManifestMayNotListItself();
  void aModuleOutsideTheInventoryIsRefused();
  void aTruncatedModuleIsRefusedBySize();
  void aTamperedModuleIsRefusedByDigest();
  void anUndeclaredFileInTheRootIsRefused();
  void aNonPeModuleIsRefused();
  void anX86ModuleIsRefused();
  void anImportOutsideTheBundleIsRefused();
  void aDelayImportOutsideTheBundleIsRefused();
  void aSystemImportIsAccepted();
  void aDependencyDirectoryHoldingNoDeclaredFileIsRefused();
  void anUnsupportedNativeProtocolVersionIsRefused();
  void aStandardBundleDeclaringANativeProtocolIsRefused();
  void anMpvApiVersionOutsideTheSupportedWindowIsRefused();
  void aNativeBundleWithoutAnMpvApiVersionIsRefused();
  void theDescriptorCarriesTheValidatedRootModuleAndDeps();

  void discoveryReportsNothingWhenThereIsNothing();
  void discoveryFindsAValidBundleAndNamesNoRejection();
  void theCurrentPointerOutranksAnOlderVersion();
  void anInvalidCandidateDoesNotHideAValidOneOfTheSameKind();
  void aCorruptCandidateIsRejectedByName();
  void aStagingDirectoryIsNeverACandidate();
  void aCorruptPointerDoesNotHideVersionedBundles();
  void aCorruptBundleBehindTheCurrentPointerStillSelectsThePreviousOne();
  void discoveryOverTheDefaultRootsFindsAnInstalledBundleAndSkipsTheStockTree();
  void aDirectoryNamedForOneKindHoldingTheOtherIsRefusedByName();

  void installPublishesAnImmutableVersionedDirectoryAndSelectsIt();
  void theInstalledBundleIsSelfContainedAfterTheSourceIsGone();
  void anInterruptedInstallLeavesNoVisibleBundleAndKeepsTheSelection();
  void aFailingInstallKeepsThePreviousSelectionUsable();
  void republishingAnExistingVersionIsRefused();
  void installingIntoTheStockRuntimeTreeIsRefused();
  void aNativeBundleInstallsAndIsDiscoveredAsNative();
  void aBundlePublishedWithoutAPointerFlipLeavesThePreviousSelection();

  void aUnicodeAndMixedCaseRootInstallsAndDiscovers();
  void aDifferentlyCasedFileNameResolvesOnWindows();
  void aJunctionInsideTheBundleIsRefused();

  // The path representability boundary, exercised through the callers that
  // depend on it. Each case reaches a real refusal - a bundle root the reader is
  // asked to open, a store root it is asked to own - rather than asserting on
  // the helper's own return type.
  void aTruncatingBundleRootIsRefusedByName();
  void aDeviceOrShareBundleRootIsRefusedByName();
  void aNonAsciiLocalBundleRootIsAccepted();
  void aColonCommaPercentLocalPathIsAccepted();
  void aMalformedUnicodePathIsRefusedNotReplaced();
  void anOverlongPathComponentIsRefusedAtItsExactLimit();
  void aPathAtTheWideLimitIsAcceptedAndOneBeyondIsRefused();
  void anUnrepresentableStoreRootIsRefusedAsUnowned();
  void aStoredRelativeNameWithMalformedUnicodeIsRefused();

  void theNativeLayoutResolvesUnderOneAppLocalHierarchy();

  void aPublishedBundleDirectoryIsNotDescribedAsAStockRuntime();
  void theStockRuntimeIsDescribedFromItsActualBytesWithoutWriting();
  void aModuleImportingSomethingAbsentIsRefusedByName();

  // The optional preparation block, trusted only through the real reader and
  // validator: a playback-only bundle stays valid without one, a complete block
  // resolves to owned tools and a verified backend, and every partial, false or
  // escaping statement is refused before a consumer could observe it.
  void aPlaybackOnlyNativeBundleRemainsValidWithoutPreparation();
  void aCompletePreparationBlockNamesOwnedToolsAndVerifiedBackend();
  void aPreparationBlockMissingAnySubMemberIsRefused();
  void aPlaceholderOrZeroProvenanceIsRefused();
  void aStandardBundleCarryingPreparationIsRefused();
  void anUnknownDuplicateOrEmptyBackendListIsRefused();
  void anUnsafeToolPathOrOneOutsideTheInventoryIsRefused();
  void aRuntimeDirectoryOutsideTheBundleIsRefused();
  void aRuntimeDirectoryWithNoOwnedFileIsRefused();
  void aProducerOrBackendLibraryAbsentOrTamperedIsRefused();
  void anAllowedPreparationSurvivesInstallIntoANewOwnedRoot();
  void noUnverifiedPreparationIsEverPresentedAsAvailable();
};