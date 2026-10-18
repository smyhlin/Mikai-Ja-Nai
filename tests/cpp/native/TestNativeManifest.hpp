#pragma once

#include <QtTest/QTest>

class TestNativeManifest : public QObject {
  Q_OBJECT

 private slots:
  void aGraphKeepsOneContentIdentityAcrossTwoMachines();
  void twoFilesWithTheSameBasenameHaveDifferentIdentities();
  void aChangedDriverChangesTheBuildKey();
  void theManifestRecomputesItsOwnBuildKey();
  void anUnknownMemberIsRefused();
  void aMissingRequiredMemberIsRefused();
  void aFutureSchemaIsRefused();
  void aBuildKeyThatIsNotTheRequestsKeyIsRefused();
  void theEngineDigestIsRecordedSeparatelyFromTheBuildKey();
  void optionValuesRoundTripAndKeepTheirOrder();
  void theOptionParserRefusesATokenThatIsNotAnOption();
  void precisionAndTf32ArePartOfTheBuildKey();
  void aPlaceholderOrMalformedFieldLeavesTheRequestIncomplete();
  void aMalformedGraphDigestIsRefused();
  void aStageShapeWithAZeroDimensionIsRefused();
  void aSensitiveRuntimeIsRefusedAndNeverExported();

  void publishingWritesAnArtifactThatVerifies();
  void publishingLeavesNoStagingDirectoryBehind();
  void aCancelledBuildsStagingIsNeverSelectable();
  void aTruncatedEngineIsRefusedBeforeUse();
  void aTamperedEngineIsRefusedByDigest();
  void aChangedSourceFileInvalidatesTheArtifact();
  void aChangedDriverInvalidatesTheArtifact();
  void anExternalModelDirectorySentinelSurvivesTheLifecycle();
  void removalRequiresProofAndLeavesForeignFiles();
  void anArtifactDirectoryWithoutAManifestIsNotRemoved();
  void aLockedManifestRemovalDoesNotExposeAPartialArtifact();
  void anEngineNameThatEscapesTheRootIsRefused();
  void aStoreRootThatIsNotADirectoryIsNotOwned();

  void acquiringALeaseRequiresARealArtifact();
  void aLeaseHeldThroughAnotherStoreRefusesRemoval();
  void aLeaseOnOneArtifactLeavesAnotherRemovable();

  // Hostile identity, manifest and ownership cases, in risk order.
  void declaredProfileAndBuildKeyAgree();
  void aManifestNameCollisionIsRefused();
  void aCopiedManifestUnderAnotherKeyIsRefused();
  void anOnDiskManifestEditIsCaught();
  void anEngineShaMismatchIsRefused();
  void aNonCanonicalOptionStringIsRefused();
  void everyRuntimeFieldDisagreementIsNamed();
  void aDirectMlLuidBuildVerifiesAndInvalidates();
  void aStoreCannotBePointedAtAModelDirectory();
  void everyEntryPointRefusesAnUnownedRoot();
  void aSymlinkOrNonRegularEngineIsRefused();
  void aNestedOrUnknownManifestMemberIsRefused();
  void publishRefusesAMismatchedOrExistingArtifact();
  void discardStagingRefusesForeignNamesAndTrees();
  void pathWithinRootFollowsPlatformCaseRules();
  void volumeRootsContainDescendantsAndRemainInTheTrustChain();
};
