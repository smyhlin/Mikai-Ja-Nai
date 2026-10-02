#pragma once

#include <QtTest/QTest>

class TestNativeBundle : public QObject {
  Q_OBJECT

 private slots:
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

  void theNativeLayoutResolvesUnderOneAppLocalHierarchy();

  void aPublishedBundleDirectoryIsNotDescribedAsAStockRuntime();
  void theStockRuntimeIsDescribedFromItsActualBytesWithoutWriting();
  void aModuleImportingSomethingAbsentIsRefusedByName();
};