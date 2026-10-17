#pragma once

#include <QtTest/QTest>

/// The owned model store, consumed without the inspector.
///
/// These are the consumer-visible facts a build with `MIKAI_WITH_MODEL_INSPECTION`
/// off must still be able to read: a published row is re-proved from its immutable
/// copy, an unsupported dtype and an unproved dynamic boundary are recorded rather
/// than trusted, and a mutated copy or manifest is reported instead of being read
/// as some other row. The same rows assemble into the catalogue a pure consumer
/// plans from. Nothing here loads ONNX Runtime, runs inference, or mocks an
/// inspector answer: the store's own bytes and documents are the evidence.
class TestNativeStoredModels : public QObject {
  Q_OBJECT

 private slots:
  // A real owned copy, published from bytes and read back, verifies against the
  // identity it was published under.
  void aPublishedOwnedRowReadsBackAndVerifiesWithoutRuntime();

  // A non-FP16 graph is recorded, not executed: the row is offered and unusable,
  // naming the dtype it actually declared, and the reload keeps that fact.
  void anUnsupportedDtypeRowIsRecordedAndReadBackUnavailable();

  // A temporal row is offered only with the CPU proof recorded beside it; without
  // one it stays Unavailable, and a provisioner that cannot run the boundary
  // leaves it unavailable rather than selectable.
  void aTemporalRowIsOfferedOnlyWithItsRecordedProof();

  // A changed copy and an edited manifest identity are reported as the rows they
  // now are - never dropped, never deleted, never trusted.
  void hostileCopyAndManifestTransitionsAreReportedNotDropped();

  // A root outside the app-owned tree is refused, and an owned root that does not
  // exist yet reads as the empty inventory it is.
  void aForeignRootIsRefusedAndAFreshOwnedRootReadsEmpty();

  // The stored rows assemble into a catalogue and resolve a plan with no runtime
  // and no resolver, because the model is fully declared.
  void ownedRowsAssembleIntoAPureCatalogAndPlanWithoutRuntime();
};
