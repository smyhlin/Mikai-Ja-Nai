// The API client's public contract, over a loopback server.
//
// These are the promises a caller of MikaiClient actually relies on: a failure
// the server names arrives with its own code and status instead of being
// flattened into a transport error; the request it sends carries the credential
// that is current when it is sent; and saving a key reports whether the key
// survived the restart. All three used to be silent: unknown, flattened, or
// ignored.
//
// The server is a QTcpServer on 127.0.0.1 inside this process that answers
// whatever the client asks for, so the base URL is pointed at it and no test
// touches the real API, spends quota, or needs a key. The .env the client writes
// is redirected into a temporary directory, so a test never modifies the
// repository one.

#pragma once

#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

class TestApiClient : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase();
  void cleanupTestCase();

  // A failure envelope on the wire keeps its code, its message and its status,
  // so the settings dialog can say "the key was disabled" rather than "the
  // network failed".
  void aDisabledKeyFailureKeepsItsOwnCodeAndMessage();
  void aMissingResourceFailureKeepsItsStatus();
  void aMalformedSuccessEnvelopeFailsRatherThanParsingGarbage();
  void aPlainEnvelopeSuccessParsesIntoTheResult();
  void catalogueFiltersNarrowTheReturnedRows();
  void dictionariesAndMetaKeepEveryValueTheServerOffers();
  void teamCatalogueAndProfileCarryTheirOwnData();
  void updatesCarryTheFeedIdentityAndPagingTheServerReported();
  void everyApiRequestNamesTheApplicationAndItsVersion();
  void anInvalidCatalogueFilterSurfacesTheApiValidationResponse();
  void aFreshResponseIsServedWithoutAnotherRoundTrip();
  void aStaleResponseIsRevalidatedAndTheNewBodyIsServed();
  void aStaleResponseTheServerStillHoldsIsReusedUnchanged();
  void aRevalidatedBodySurvivesEvictionWhileItsRequestIsInFlight();
  void aNoCacheResponseIsRevalidatedEveryTime();
  void aNoStoreResponseIsNeverKeptAndTheCredentialDecidesWhatIsKept();
  void aFailureIsNotCachedAndACancelledCallReadsNothing();
  void fetchBytesReportsLoopbackDownloadProgress();
  void anArtworkTimeoutDoesNotWaitForAnAlreadyFinishedAbort();

  // A transport failure is still reported as one, and is not confused with the
  // server answering an error.
  void aTransportFailureIsNotAnApiFailure();
  void aCrossOriginRedirectDoesNotForwardTheApiCredential();
  void aCrossOriginValidationRedirectDoesNotForwardTheCandidateCredential();
  void aCredentialedTransportRequestBlocksCrossOriginRedirects();
  void aCustomRequestHeaderIsNotForwardedAcrossOriginRedirects();
  void aSameOriginRedirectKeepsTheApiCredential();
  void aThreadLocalTransportManagerIsDeletedWhenItsThreadFinishes();
  void explicitApiKeyConfigurationKeepsItsEnvironmentPrecedence();

  // Validating a candidate key is the same reading path: a named failure comes
  // back named, so the dialog can offer the API's reason rather than a guess.
  void aValidateCallKeepsTheEnvelopeItWasGiven();

  // The credential that goes out is the one current at send time; a parked
  // caller never re-sends under an epoch it did not belong to.
  void aKeyChangedWhileARequestWasParkedDoesNotSendTheOldKey();
  void validatingAParkedOldKeyDoesNotChangeTheNewKeysQuota();

  // Saving a key says whether it was saved, and a save that could not write
  // leaves the old key and the previous file exactly as they were.
  void aSavedKeyIsReportedAsSaved();
  void aKeyThatCouldNotBeSavedNamesTheReasonAndKeepsTheOldOne();
  void clearingAKeyEmptiesTheStoredFileAndReportsIt();

  // The envelope decoder is the shared contract in Errors.hpp: it is what both
  // the client and the settings dialog read a failure through.
  void anEnvelopeFailureDecodesItsNamedReason();

 private:
  // Where the .env the client writes is redirected, so no test ever reads or
  // overwrites the repository's own key file.
  QTemporaryDir envDirectory_;
};

/// Standalone entry point for the mikai_tests runner; the class above carries
/// the suite so it can also be driven by QTest directly.
int runTestApiClient(int argc, char** argv);
