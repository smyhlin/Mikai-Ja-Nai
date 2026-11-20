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

void TestUpdateCheck::parsesReleaseAssetsAndSha256Sums() {
    const QByteArray json =
        R"({"tag_name":"v1.5.0","html_url":"https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v1.5.0","draft":false,"prerelease":false,"assets":[)"
        R"({"name":"Mikai-1.5.0-x86_64.AppImage","size":1048576,"browser_download_url":"https://github.com/smyhlin/Mikai-Ja-Nai/releases/download/v1.5.0/Mikai-1.5.0-x86_64.AppImage"},)"
        R"({"name":"SHA256SUMS","size":128,"browser_download_url":"https://github.com/smyhlin/Mikai-Ja-Nai/releases/download/v1.5.0/SHA256SUMS"},)"
        R"({"name":"evil.AppImage","size":10,"browser_download_url":"https://evil.example/evil.AppImage"}]})";
    const auto parsed = parseLatestReleaseResponse(json, QStringLiteral("1.4.0"));
    QCOMPARE(parsed.status, UpdateCheckStatus::UpdateAvailable);
    QCOMPARE(parsed.assets.size(), 2);
    const auto appImg = mikai::backend::selectUpdateAsset(parsed.assets, mikai::backend::InstallKind::LinuxAppImage);
    QVERIFY(appImg.has_value());
    QCOMPARE(appImg->name, QStringLiteral("Mikai-1.5.0-x86_64.AppImage"));
    const auto sums = mikai::backend::findChecksumsAsset(parsed.assets);
    QVERIFY(sums.has_value());
    const QByteArray sumText =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  Mikai-1.5.0-x86_64.AppImage\n";
    QCOMPARE(mikai::backend::parseSha256Sums(sumText, QStringLiteral("Mikai-1.5.0-x86_64.AppImage")),
             QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    QCOMPARE(mikai::backend::detectInstallKind(QStringLiteral("/usr/bin/mikai"), QString()),
             mikai::backend::InstallKind::SystemPackage);
}

int runTestUpdateCheck(int argc, char** argv) {
    TestUpdateCheck test;
    return QTest::qExec(&test, argc, argv);
}
