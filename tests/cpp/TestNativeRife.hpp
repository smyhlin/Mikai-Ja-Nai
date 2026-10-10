#pragma once

#include <QtTest/QTest>

class TestNativeRife : public QObject {
    Q_OBJECT

private slots:
    void unknownRifeVariantsAreRejected();
    void requestedVariantsResolveTheExactOriginalModelNames();
    void allPinnedGraphsHaveDistinctExactDigests();
    void thePinnedOwnedRowResolvesToItsContentIdentity();
    void aSubstitutedOrWrongRowIsRefusedByItsOwnReason();
    void thePinnedHeavyGraphResolvesOnlyWithOwnedProof();

    void aRifeOnlyRequestBuildsARifeOnlyRecipe();
    void theFactoryPreservesTheExactRationalFactorAndOrder();
    void aCombinedRequestPreservesTheOrderedSpatialChainAndItsResizes();
    void theBackendIsTheExplicitOneNeverInferredFromTheBase();
    void aCombinedRequestRoundTripsThroughItsDocumentUnchanged();

    void combiningWithANonSpatialBaseIsRefusedByName();
    void anEmptyBaseIsRefusedRatherThanTreatedAsRifeOnly();
    void aMalformedFactorIsRefusedByName();
    void anOutOfRangeThresholdIsRefusedByName();
    void theLabelNeverChangesTheRecipeIdentity();
};
