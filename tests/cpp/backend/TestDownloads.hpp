// The download transport's own contract, over a real loopback socket.
//
// downloadToFile() is the only place the app takes remote bytes onto disk, so
// the rules that matter to a caller are all here: a destination that already
// belongs to someone must survive a refusal untouched, bytes that reached the
// disk must be the bytes that were hashed, and a transfer that did not finish
// must not leave a partial file behind claiming to be one.
//
// The server is a QTcpServer on 127.0.0.1 inside this process. It is slow on
// purpose where the test needs it to be: cancellation and a stalled download
// are about a request that is still in flight, and a fixture that answered
// instantly could not produce either.

#pragma once

#include <QtTest/QTest>

class TestDownloads : public QObject {
  Q_OBJECT

 private slots:
  // A file that is already there is not ours to destroy: the refusal must be
  // loud, and the file must round-trip afterwards exactly as it came in.
  void aFileThatIsAlreadyThereSurvivesTheRefusal();
  void theRefusalNamesWhatItRefusedToOverwrite();

  // The verdict has to describe the disk, not the socket: same length, same
  // digest, and the progress callback's final number matching the file.
  void aDownloadedFileIsTheBytesThatWereSentAndWritten();
  void anHttpErrorDoesNotPublishMatchingResponseBytes();
  void fetchUrlBytesPrefersHttpStatusToQtReplyError();
  void anHttpRedirectWithoutLocationDoesNotPublishMatchingResponseBytes();
  void theProgressCallbackEndsOnTheWrittenSize();

  // A rating that did not match what reached the disk is a failure, and the
  // half-written file is removed with it.
  void anUnexpectedDigestRefusesTheFileAndRemovesIt();
  void anUnexpectedSizeRefusesTheFileAndRemovesIt();

  // A destination that cannot be written at all is a named failure, not a
  // success with a silent empty file. A pre-existing destination is never
  // truncated in place either: the create is exclusive.
  void anUnwritableDestinationFailsWithoutClaimingSuccess();

  // Cancellation is observed while bytes are still arriving, and the partial
  // file does not survive the user changing their mind.
  void cancellingMidTransferRemovesWhatWasWritten();
  void aCancelledDownloadReportsThatItWasCancelled();
  void cancellingTheFinalLocalChunkRemovesTheCopy();
  void cancellingAQuietFetchReturnsWithoutWaitingForAnotherSignal();
  void aQuietFetchReportsItsDeadline();
  void cancellationTakesPrecedenceWhenSevenZipIsAlreadyInstalled();
};

/// Standalone entry point for the mikai_tests runner; the class above carries
/// the suite so it can also be driven by QTest directly.
int runTestDownloads(int argc, char** argv);
