// Redaction at the diagnostics boundary.
//
// The plan promises tokens never enter logs or exported diagnostics, so this
// suite says what that means with concrete strings: a signed media URL, an
// Authorization header, an api_key in a query and a bearer token are all gone
// from what would be persisted, and what survives is still useful to a person
// reading a report - the scheme, the host and the surrounding sentence.
//
// Nothing here writes a real secret: every value is a synthetic marker that says
// what it is, so a failure prints a fixture rather than a credential.

#pragma once

#include <QtTest/QTest>

class TestDiagnostics : public QObject {
  Q_OBJECT

 private slots:
  // A signed URL is not preserved in any part: not the path, not the query, not
  // the userinfo. What is left is enough to know which request it was.
  void aSignedUrlLosesItsPathItsQueryAndItsUserinfo();
  void aBareHostAndPathWithoutSecretsIsLeftAlone();
  void theSchemeAndHostSurviveSoTheReportIsStillUseable();

  // Key/value secrets are removed in every spelling the transport and the query
  // string produce, in both header and query form.
  void anAuthorizationHeaderLosesItsCredential();
  void anApiKeyQueryParameterLosesItsValue();
  void aJsonSecretLosesItsValue();
  void everyKeyNameSpellingIsCovered_data();
  void everyKeyNameSpellingIsCovered();

  // Ordinary logging must be untouched by this: a redactor that ate normal text
  // would turn a readable report into a misleading one.
  void ordinaryTextIsPreservedVerbatim();
  void aWordContainingKeyIsNotASecret();
  void anEmptyStringIsStillEmpty();

  // The shape an mpv log line has when it reaches Qt logging: redaction has to
  // happen to the whole line, because that is exactly the string the boundary
  // receives.
  void aMediaLogLineCarriesNoSignedUrl();

  // The persistence boundary, not just the helper: the session log is what a
  // crash report is cut from, so a token that redacted only inside the helper
  // and was written raw here would still be a disclosure.
  void aSignedUrlWrittenToTheLogFileIsRedactedOnDisk();
  void qtMessageHandlerPreservesTheOriginalMessage();
};
