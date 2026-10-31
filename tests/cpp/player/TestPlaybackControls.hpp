// Pointer and keyboard control of the shell sliders, the PiP float and the
// main player window.
//
// The slider cases need a real QWidget receiving a real mouse event. The native
// cases need a real mpv core, because they cover steps that must be resolved by
// the core rather than by a position the UI sampled a pump interval ago. They
// select a runtime only from MIKAI_MPV_MODULE and skip without one.

#pragma once

#include <QtCore/QString>
#include <QtTest/QTest>

class TestPlaybackControls : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void finalValueFollowsTheReleasePosition_data();
    void finalValueFollowsTheReleasePosition();

    void anUnrelatedButtonReleaseKeepsTheLeftDragAlive_data();
    void anUnrelatedButtonReleaseKeepsTheLeftDragAlive();

    void aLostReleaseEndsTheDragWithoutCommittingTheHover_data();
    void aLostReleaseEndsTheDragWithoutCommittingTheHover();

    void aNewPlayerStartsAtTheShownVolume();
    void aPipKeepsItsVolumeAndMuteBeforeOpening();

    void aPiPArrowStepIsResolvedByTheCoreWhenThePumpIsSuspended_data();
    void aPiPArrowStepIsResolvedByTheCoreWhenThePumpIsSuspended();

    void aSkipForwardAtTheEndDoesNotRestartTheSource();

    void aPlayerDialogArrowStepIgnoresALateStaleProgressReport_data();
    void aPlayerDialogArrowStepIgnoresALateStaleProgressReport();

private:
    // True when a runtime is selected. Without a module it skips; a selection
    // that failed in initTestCase fails here with the loader's own reason.
    bool requireRuntime();

    QString selectionError_;
};
