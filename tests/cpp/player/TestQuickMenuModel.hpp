#pragma once

#include <QtTest/QTest>

class TestQuickMenuModel : public QObject {
    Q_OBJECT

private slots:
    void incomingFlagsMoveTheMarkWhenTheSelectionChanges();
    void selectingAnotherExistingRowMovesTheMark();
    void aRebuildWithoutSelectionClearsTheMark();
    void aRemovedSelectedRowLeavesNoStaleMark();
    void disabledAndHeadingRowsNeverCarryTheMark();
    void reorderedRowsKeepTheirPersistentIdentity();
    void outOfRangeAndUnknownIdentitiesAreInert();
};
