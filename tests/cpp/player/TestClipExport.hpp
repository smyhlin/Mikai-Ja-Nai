#pragma once

#include <QtTest/QTest>

class TestClipExport : public QObject {
    Q_OBJECT

private:
    bool requireRuntime();
    QString selectionError_;

private slots:
    void initTestCase();
    void rejectsNonFiniteAndInvalidRanges();
    void rejectsCutMomentWhenThereIsNoVideoTimeline();
    void videoOnlySourceProducesBoundedPlayableMp4();
    void aacSourcePreservesAudio();
    void selectedAudioTrackExportPreservesSignal();
    void externalAudioOverridesEmbeddedAudio();
    void missingExternalAudioPreservesExistingDestination();
    void sourceFailurePreservesExistingDestination();
    void noConsentPreservesExistingDestination();
    void danglingSymlinkIsPreservedWithoutConsent();
    void cancellationPreservesDestinationAndRemovesPartialOutput();
};