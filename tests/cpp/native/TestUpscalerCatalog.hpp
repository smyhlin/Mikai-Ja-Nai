#pragma once

#include <QtTest/QTest>

class TestUpscalerCatalog : public QObject {
    Q_OBJECT

private slots:
    void theCataloguePinsFourGraphsWithExactDigests();
    void everyRowResolvesByIdFileNameAndStem();
    void everyRowResolvesByItsOwnDigestAndNoOther();
    void theScaleAndPrecisionMatchThePublishedGraphs();
    void theUrlsStayOnTheirPinnedHosts();
    void anUnknownIdentifierResolvesNothing();
    void onlyThePinnedBytesVerify();
    void aTruncatedOrForeignDownloadIsRefusedByName();
};
