#pragma once

#include <QtTest/QTest>

class TestComparePipeline : public QObject {
    Q_OBJECT

private slots:
    void metricsMeasureLevelsAndRefuseMismatchedGeometry();
    void metricsSeparateATexturedFrameFromAFlatOne();
    void detailMeasureSurvivesAUpscaledCanvas();
    void verdictAcceptsAModelEffectAndRejectsAColourShift();
    void verdictRefusesAFlatReferenceFrame();
    void verdictRefusesABlankModelAndADitheredCard();
    void configuredSlotParsesGeometryAndDepth();
    void configuredSlotAcceptsAMissingDepthSuffix();
    void pipelineSpecMatchesThePlayersDecoderAndFilter();
    void pipelineSpecIsEmptyForADisabledPlan();
    void decoderVerdictRequiresADeviceFramesDecoder();
};