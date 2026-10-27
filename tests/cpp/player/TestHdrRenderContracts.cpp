#include "TestHdrRenderContracts.hpp"

#include "player/core/VideoSurface.hpp"

using mplayer::VideoSurface;

void TestHdrRenderContracts::unknownTransferRequiresSdrGate() {
    VideoSurface::SourceColorProfile profile;
    QVERIFY(!profile.hasTransferMetadata());
    QVERIFY(profile.requiresSdrGate());
    QVERIFY(!profile.isHdr());
    profile.gamma = QStringLiteral("vendor-unknown-transfer");
    QVERIFY(profile.hasTransferMetadata());
    QVERIFY(profile.requiresSdrGate());
    QVERIFY(!profile.isHdr());
}

void TestHdrRenderContracts::pqAndHlgRequireSdrGate() {
    VideoSurface::SourceColorProfile profile;
    profile.gamma = QStringLiteral("pq");
    QVERIFY(profile.hasTransferMetadata());
    QVERIFY(profile.isHdr());
    QVERIFY(profile.requiresSdrGate());
    profile.gamma = QStringLiteral("hlg");
    QVERIFY(profile.isHdr());
    QVERIFY(profile.requiresSdrGate());
}

void TestHdrRenderContracts::knownSdrTransferDoesNotRequireSdrGate() {
    VideoSurface::SourceColorProfile profile;
    profile.gamma = QStringLiteral("bt.1886");
    QVERIFY(profile.hasTransferMetadata());
    QVERIFY(!profile.isHdr());
    QVERIFY(!profile.requiresSdrGate());
}

void TestHdrRenderContracts::primariesDoNotMisclassifySdrAsHdr() {
    VideoSurface::SourceColorProfile profile;
    profile.gamma = QStringLiteral("bt.1886");
    profile.primaries = QStringLiteral("bt.2020");
    QVERIFY(!profile.isHdr());
    QVERIFY(!profile.requiresSdrGate());
}
void TestHdrRenderContracts::pacingConvertsRawRenderNanosecondsToMpvClock() {
    constexpr uint64_t api22 = (uint64_t{2} << 16) | 2;
    constexpr uint64_t api21 = (uint64_t{2} << 16) | 1;
    const auto nanosecondDelay = mplayer::clientPacingDelayMilliseconds(
        1'033'333'000, 1'000'000, 1.5, api22);
    const auto legacyMicrosecondDelay = mplayer::clientPacingDelayMilliseconds(
        1'033'333, 1'000'000, 1.5, api21);
    QVERIFY(nanosecondDelay.has_value());
    QVERIFY(legacyMicrosecondDelay.has_value());
    QCOMPARE(*nanosecondDelay, 32);
    QCOMPARE(*legacyMicrosecondDelay, 32);
}

void TestHdrRenderContracts::pacingRendersImmediatelyForElapsedOrDistantTargets() {
    constexpr uint64_t api22 = (uint64_t{2} << 16) | 2;
    QVERIFY(!mplayer::clientPacingDelayMilliseconds(
                 1'000'000'000, 1'000'000, 1.5, api22).has_value());
    QVERIFY(!mplayer::clientPacingDelayMilliseconds(
                 1'200'000'000, 1'000'000, 1.5, api22).has_value());
}
