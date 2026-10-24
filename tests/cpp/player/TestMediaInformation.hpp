#pragma once

#include <QtTest/QTest>

class TestMediaInformation : public QObject {
    Q_OBJECT

private slots:
    void overviewMeasurementDoesNotRelayoutTheLiveDocument();
    void overviewWrapsAndCopiesLiteralUnbrokenMetadata();
    void overviewKeepsSelectionAndScrollWhenSectionsChange();
    void overviewKeepsSelectedTextWhenAnotherPartOfTheValueChanges();
    void treeKeepsValuesVisibleWhenThePropertyHeaderIsDragged();
    void treeKeepsExpansionSelectionAndScrollAcrossRefresh();
    void treeDoesNotRecreateAClearedSelection();
    void treeCopiesLiteralMetadataWithoutMakingItEditable();
    void plainTextPreservesLiteralMetadataAndContinuationIndentation();
    void localizedDurationCarriesWithoutIntegerOverflow_data();
    void localizedDurationCarriesWithoutIntegerOverflow();
    void unavailableNumbersAndUnknownFlagsAreNotReportedAsZeroOrNo();
    void maximumIntegerMetadataRetainsItsExactValue();
};
