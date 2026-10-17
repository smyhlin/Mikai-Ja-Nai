#pragma once

#include <QtTest/QTest>

class TestNativeTypes : public QObject {
  Q_OBJECT

 private slots:
  void enumNamesRoundTripAndRefuseUnknownValues();
  void aRifeModelNameComesFromRevisionVariantAndEnsemble();
  void anUnreducedRationalStaysUnreducedAndStillReadsAsItsOwnValue();
  void aRationalRefusesNonPositiveNumeratorOrDenominator();
  void availabilityCarriesAMandatoryReasonWheneverItIsNotReady();
  void modelIdentitySeparatesBuiltInNamesFromContentAddressedImports();
  void contentIdentityMovesWithTheGraphAndNotWithTheMachine();
  void anUnsupportedDtypeIsRefusedByNameRatherThanConverted();
  void aDynamicProfileAdmitsNoKnownGeometry();
  void aSymbolicOutputBatchIsHonouredOnlyWithAProvingRun();
};