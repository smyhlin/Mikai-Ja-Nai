#include "TestUpdateCheck.hpp"

#include <backend/UpdateCheck.hpp>

using mikai::backend::UpdateCheckStatus;
using mikai::backend::parseLatestReleaseResponse;

namespace {
QByteArray payload(const QByteArray& tag, const QByteArray& url = {},
                  bool draft = false, bool prerelease = false) {
    const QByteArray releaseUrl = url.isEmpty()
        ? QByteArray("https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/") + tag : url;
    return QByteArray("{\"tag_name\":\"") + tag + "\",\"html_url\":\"" + releaseUrl +
           "\",\"draft\":" + (draft ? "true" : "false") +
           ",\"prerelease\":" + (prerelease ? "true" : "false") + "}";
}
}

void TestUpdateCheck::comparesNumericVersionComponents() {
    auto result = parseLatestReleaseResponse(payload("v1.10.0"), QStringLiteral("1.9.9"));
    QCOMPARE(result.status, UpdateCheckStatus::UpdateAvailable);
    QCOMPARE(result.version, QStringLiteral("1.10.0"));
    result = parseLatestReleaseResponse(payload("v1.2.3"), QStringLiteral("1.2.3"));
    QCOMPARE(result.status, UpdateCheckStatus::UpToDate);
    result = parseLatestReleaseResponse(payload("v1.2.3"), QStringLiteral("1.2.4"));
    QCOMPARE(result.status, UpdateCheckStatus::UpToDate);
    QCOMPARE(parseLatestReleaseResponse(payload("v2.0.0"), QStringLiteral("invalid")).status,
             UpdateCheckStatus::Failed);
}

void TestUpdateCheck::rejectsMalformedAndUnsafeResponses() {
    QCOMPARE(parseLatestReleaseResponse("not json", QStringLiteral("1.0.0")).status,
             UpdateCheckStatus::Failed);
    QCOMPARE(parseLatestReleaseResponse(payload("v1.2"), QStringLiteral("1.0.0")).status,
             UpdateCheckStatus::Failed);
    QCOMPARE(parseLatestReleaseResponse(payload("v1.2.3", "https://github.com.evil.test/x"),
                                        QStringLiteral("1.0.0")).status,
             UpdateCheckStatus::Failed);
    QCOMPARE(parseLatestReleaseResponse(payload("v1.2.3", "http://github.com/x"),
                                        QStringLiteral("1.0.0")).status,
             UpdateCheckStatus::Failed);
}

void TestUpdateCheck::ignoresPrereleaseAndDrafts() {
    QCOMPARE(parseLatestReleaseResponse(payload("v2.0.0", "https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v2.0.0", false, true),
                                        QStringLiteral("1.0.0")).status,
             UpdateCheckStatus::Failed);
    QCOMPARE(parseLatestReleaseResponse(payload("v2.0.0", "https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v2.0.0", true),
                                        QStringLiteral("1.0.0")).status,
             UpdateCheckStatus::Failed);
}

int runTestUpdateCheck(int argc, char** argv) {
    TestUpdateCheck test;
    return QTest::qExec(&test, argc, argv);
}
