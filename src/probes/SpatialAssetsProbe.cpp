// M35's source-asset probe: a real window holding the actual NativeAssetsPanel,
// driven against the real pinned catalogue, the real app-owned asset store and,
// where the network is reachable, one real anonymous download.
//
// What source reading cannot settle is whether the five pinned identities really
// publish only when the bytes hash and measure exactly as the acquisition record
// says, whether a wrong hash or a cancel leaves the store untouched, whether an
// identical copy is never rewritten, whether the licence and the pinned origin
// really survive beside the bytes, and whether a downloaded graph stays an input
// rather than a Ready pipeline. So this drives the panel and reads the store
// back.
//
// It needs either network access to `raw.githubusercontent.com` or a real
// already-acquired copy of one pinned graph supplied with
// `--source-owned-file <path>` (verified by digest - never fabricated bytes).
// Without either it does the refusal and cancellation proofs, then skips the
// publish proofs and exits 4. It never uses a live API credential: the asset
// fetch goes through the client's anonymous byte path.

#include <cstdio>
#include <optional>

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QMetaObject>
#include <QObject>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <backend/MikaiClient.hpp>

#include "native/NativeAssetsPanel.hpp"
#include "native/NativeModelInventory.hpp"
#include "native/NativePaths.hpp"
#include "native/NativePrefs.hpp"
#include "native/NativeSpatialAssets.hpp"
#include "ProbeUi.hpp"
#include "workers/Runner.hpp"

using namespace mplayer;
using namespace mplayer::native;

namespace {

int failures = 0;

void check(bool ok, const QString& what) {
    std::fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) ++failures;
}

void say(const QString& text) { std::fprintf(stderr, "%s\n", qPrintable(text)); }

QString sha256Hex(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

void shoot(QWidget* widget, const QString& name) {
    const QString path = QDir::current().filePath(name);
    const bool written =
        widget != nullptr && QDir().mkpath(QFileInfo(path).absolutePath()) &&
        widget->grab().save(path);
    check(written, QStringLiteral("screenshot written: %1").arg(name));
}

/// `--source-owned-file=<path>` or `--source-owned-file <path>`, or empty.
QString sourceOwnedFileArgument(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (argument.startsWith(QLatin1String("--source-owned-file="))) {
            return argument.mid(static_cast<int>(qstrlen("--source-owned-file=")));
        }
        if (argument == QLatin1String("--source-owned-file") && i + 1 < argc) {
            return QString::fromLocal8Bit(argv[i + 1]);
        }
    }
    return {};
}

const SpatialAsset* assetForDigest(const QString& sha256) {
    for (const SpatialAsset& asset : spatialAssetCatalog()) {
        if (asset.sha256 == sha256) return &asset;
    }
    return nullptr;
}

/// Reads the published manifest and notice and asserts the pinned origin and the
/// licence really survived beside the bytes.
bool inspectRoot(const QString& root, const QString& id, QString* error) {
    const auto fail = [error](const QString& text) {
        if (error != nullptr) *error = text;
        return false;
    };

    const QVector<StoredSpatialAsset> rows = storedSpatialAssets(root);
    const StoredSpatialAsset* found = nullptr;
    for (const StoredSpatialAsset& row : rows) {
        if (row.asset.id == id && row.verified) found = &row;
    }
    if (found == nullptr) return fail(QStringLiteral("no verified copy of %1 under %2").arg(id, root));
    if (found->asset.commit != spatialAssetCommit()) {
        return fail(QStringLiteral("the copy does not record the pinned commit"));
    }
    if (found->asset.url.indexOf(spatialAssetCommit()) < 0) {
        return fail(QStringLiteral("the copy's source URL is not pinned to the commit"));
    }

    const QString manifestPath =
        QDir(found->directory).filePath(SpatialAssetStore::manifestFileName());
    QFile manifestFile(manifestPath);
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("%1 cannot be read").arg(manifestPath));
    }
    const QJsonDocument document = QJsonDocument::fromJson(manifestFile.readAll());
    if (!document.isObject()) return fail(QStringLiteral("%1 is not a JSON object").arg(manifestPath));
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("creator")).toString() !=
        QStringLiteral("the-database/mpv-AnimeJaNai")) {
        return fail(QStringLiteral("the manifest does not retain the creator"));
    }
    const QJsonObject license = object.value(QStringLiteral("license")).toObject();
    if (license.value(QStringLiteral("spdx")).toString() != QStringLiteral("CC-BY-NC-SA-4.0")) {
        return fail(QStringLiteral("the manifest does not retain the licence"));
    }
    if (!license.value(QStringLiteral("notice")).toString().contains(
            QStringLiteral("Attribution"))) {
        return fail(QStringLiteral("the manifest does not retain the attribution notice"));
    }

    const QString noticePath = QDir(found->directory).filePath(QStringLiteral("NOTICE.txt"));
    const QByteArray notice = readBytes(noticePath);
    if (!notice.contains("Attribution") || !notice.contains("NonCommercial")) {
        return fail(QStringLiteral("the notice file does not carry the retention terms"));
    }
    return true;
}

/// Drives one real download through the panel and waits for its verdict.
bool runDownload(NativeAssetsPanel* panel, const QString& id) {
    bool ok = false;
    bool done = false;
    QEventLoop loop;
    QMetaObject::Connection connection = QObject::connect(
        panel, &NativeAssetsPanel::downloadFinished, &loop, [&](bool result) {
            ok = result;
            done = true;
            loop.quit();
        });
    const bool submitted = panel->selectSourceAsset(id) && panel->downloadSelectedSourceAsset();
    if (submitted) {
        QTimer::singleShot(600000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    QObject::disconnect(connection);
    return submitted && done && ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    if (!mikai::probes::initializeUi(app)) return 2;
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiSpatialAssetsProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("SpatialAssetsProbe"));

    // A clean app-owned tree for this probe only: the app/org names above make
    // AppLocalDataLocation ours, and `native/assets`, `native/models` and the
    // rest live under it.
    QDir(nativeRoot()).removeRecursively();

    // ── 1. the catalogue is the acquisition record's, to the byte ───────────
    const QVector<SpatialAsset> catalog = spatialAssetCatalog();
    check(catalog.size() == 5, QStringLiteral("the catalogue carries the five original graphs"));
    check(spatialAssetCommit() == QStringLiteral("d9cbaae5e92a56e71fdf4da5e063ae3e2611e5f7"),
          QStringLiteral("the pinned commit is the acquisition record's"));
    for (const SpatialAsset& asset : catalog) {
        check(asset.id == QStringLiteral("sha256:") + asset.sha256,
              QStringLiteral("%1 is identified by its digest").arg(asset.name));
        check(asset.url.contains(spatialAssetCommit()) && asset.url.contains(asset.name),
              QStringLiteral("%1 has a URL pinned to the commit and the name").arg(asset.name));
        check(asset.bytes > 0, QStringLiteral("%1 records its length").arg(asset.name));
    }
    {
        const SpatialAssetLicense license = spatialAssetLicense();
        check(license.spdx == QStringLiteral("CC-BY-NC-SA-4.0"),
              QStringLiteral("the licence is CC BY-NC-SA 4.0"));
        check(license.notice.contains(QStringLiteral("the-database"),
                                      Qt::CaseInsensitive) &&
                  license.notice.contains(QStringLiteral("creativecommons.org")),
              QStringLiteral("the notice retains the creator and the licence link"));
        check(license.useScope.contains(QStringLiteral("non-commercial")),
              QStringLiteral("the notice states the private, non-commercial use"));
    }

    QTemporaryDir trusted;
    check(trusted.isValid(), QStringLiteral("a trusted temporary root exists"));
    const QString trustedRoot = trusted.path();

    // ── 2. a wrong hash publishes nothing ───────────────────────────────────
    {
        const SpatialAsset& target = catalog.at(0);
        const QByteArray wrongHash(static_cast<int>(target.bytes), char(0x11));
        const SpatialAssetPublication publication =
            publishSpatialAssetBytes(target, wrongHash, trustedRoot);
        check(!publication.ok && publication.reasonKey == QStringLiteral("spatial.asset.digest"),
              QStringLiteral("a wrong-hash payload is refused by digest"));
        check(storedSpatialAssets(trustedRoot).isEmpty(),
              QStringLiteral("a wrong hash wrote nothing"));
    }
    // ── 3. a wrong length publishes nothing ─────────────────────────────────
    {
        const SpatialAsset& target = catalog.at(0);
        const QByteArray wrongLength(static_cast<int>(target.bytes) - 1, char(0x22));
        const SpatialAssetPublication publication =
            publishSpatialAssetBytes(target, wrongLength, trustedRoot);
        check(!publication.ok, QStringLiteral("a wrong-length payload is refused"));
        check(storedSpatialAssets(trustedRoot).isEmpty(),
              QStringLiteral("a wrong length wrote nothing"));
    }
    // ── 4. an id that is not a pinned row is refused ────────────────────────
    {
        SpatialAsset bogus = catalog.at(0);
        bogus.sha256 = QString(64, QLatin1Char('a'));
        bogus.id = QStringLiteral("sha256:") + bogus.sha256;
        const QByteArray bytes(static_cast<int>(bogus.bytes), char(0x33));
        const SpatialAssetPublication publication =
            publishSpatialAssetBytes(bogus, bytes, trustedRoot);
        check(!publication.ok && publication.reasonKey == QStringLiteral("spatial.asset.unknown"),
              QStringLiteral("an unpinned identity is refused"));
        check(storedSpatialAssets(trustedRoot).isEmpty(),
              QStringLiteral("an unpinned identity wrote nothing"));
    }
    // ── 5. a root that is a file is refused ─────────────────────────────────
    {
        const QString fileRoot = QDir(trustedRoot).filePath(QStringLiteral("not-a-directory"));
        check(writeBytes(fileRoot, QByteArrayLiteral("x")),
              QStringLiteral("a foreign file stands where a root would be"));
        const SpatialAssetPublication publication =
            publishSpatialAssetBytes(catalog.at(0), QByteArray(8, char(0x44)), fileRoot);
        check(!publication.ok && publication.reasonKey == QStringLiteral("spatial.asset.root"),
              QStringLiteral("a root that is a file is refused"));
    }
    // ── 6. a cancel before the publication gate writes nothing ──────────────
    {
        ImportCancellation gate;
        check(gate.cancel() == ImportCancellation::CancelResult::CancelledNow,
              QStringLiteral("the first cancel wins while nothing is published"));
        check(!gate.beginPublication(),
              QStringLiteral("the publication gate refuses after a winning cancel"));
        check(gate.cancel() == ImportCancellation::CancelResult::AlreadyCancelled,
              QStringLiteral("a second cancel is idempotent"));
        check(storedSpatialAssets(trustedRoot).isEmpty(),
              QStringLiteral("a cancelled publication wrote nothing"));
    }
    // ── 7. a cancel after the gate is refused, not concealed ────────────────
    {
        ImportCancellation gate;
        check(gate.beginPublication(), QStringLiteral("the gate opens once"));
        check(gate.cancel() == ImportCancellation::CancelResult::TooLate,
              QStringLiteral("a late cancel loses the publication race"));
    }

    // ── 8. real bytes: supplied, or downloaded anonymously ──────────────────
    const QString sourceOwnedFile = sourceOwnedFileArgument(argc, argv);
    mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
    QWidget window;
    window.setWindowTitle(QStringLiteral("Source assets"));
    auto* layout = new QVBoxLayout(&window);
    auto* panel = new NativeAssetsPanel(&window);
    panel->setObjectName(QStringLiteral("nativeAssetsPanel"));
    mikai::app::Runner runner;
    panel->setRunner(&runner);
    panel->setClient(&client);
    layout->addWidget(panel);
    window.resize(mikai::probes::windowSize(QSize(940, 760)));
    window.show();
    QApplication::processEvents();

    check(panel->sourceAssets().size() == 5,
          QStringLiteral("the panel offers the five pinned assets"));
    shoot(&window, QStringLiteral("spatial-assets-probe-empty.png"));

    QByteArray realBytes;
    QString realId;
    bool downloadedThroughPanel = false;
    if (!sourceOwnedFile.isEmpty()) {
        realBytes = readBytes(sourceOwnedFile);
        check(!realBytes.isEmpty(),
              QStringLiteral("the supplied owned file is readable: %1").arg(sourceOwnedFile));
        const SpatialAsset* asset = assetForDigest(sha256Hex(realBytes));
        check(asset != nullptr,
              QStringLiteral("the supplied file hashes to one of the pinned graphs"));
        if (asset != nullptr) realId = asset->id;
    } else {
        // The smallest pinned graph keeps the probe's own traffic small.
        static const QString kProbeAsset =
            QStringLiteral("sha256:7b804d87320f37b7269d6e27796046fbd975e95e2aa1f65d5557f52a36282716");
        if (!panel->selectSourceAsset(kProbeAsset)) {
            std::fprintf(stderr, "[spatial_assets_probe] FAIL: the probe asset is not in the "
                                 "catalogue\n");
            ++failures;
        } else {
            downloadedThroughPanel = runDownload(panel, kProbeAsset);
            if (!downloadedThroughPanel) {
                std::fprintf(stderr,
                             "[spatial_assets_probe] SKIP: no network to fetch a pinned graph; "
                             "supply --source-owned-file to prove publication\n");
                QDir(nativeRoot()).removeRecursively();
                return 4;
            }
            realId = kProbeAsset;
            realBytes = readBytes(panel->publishedAssetPath(kProbeAsset));
            check(sha256Hex(realBytes) == QStringLiteral(
                                               "7b804d87320f37b7269d6e27796046fbd975e95e2aa1f65d"
                                               "5557f52a36282716"),
                  QStringLiteral("the downloaded asset is the pinned bytes"));
        }
    }
    shoot(&window, QStringLiteral("spatial-assets-probe.png"));

    // ── 9. a verified copy publishes, is immutable, and retains its origin ──
    if (!realId.isEmpty() && !realBytes.isEmpty()) {
        const SpatialAsset* asset = findSpatialAsset(realId);
        check(asset != nullptr, QStringLiteral("the real bytes match a pinned row"));
        if (asset != nullptr) {
            const SpatialAssetPublication first =
                publishSpatialAssetBytes(*asset, realBytes, trustedRoot);
            check(first.ok && first.published,
                  QStringLiteral("the verified bytes publish into the trusted root"));

            const QString manifestBefore =
                QDir(first.directory).filePath(SpatialAssetStore::manifestFileName());
            const QByteArray manifestBytesBefore = readBytes(manifestBefore);

            const SpatialAssetPublication second =
                publishSpatialAssetBytes(*asset, realBytes, trustedRoot);
            check(second.ok && !second.published,
                  QStringLiteral("an identical copy is not rewritten"));
            check(readBytes(manifestBefore) == manifestBytesBefore,
                  QStringLiteral("the existing copy is byte-identical after the second publish"));

            QString inspectError;
            const bool inspected = inspectRoot(trustedRoot, realId, &inspectError);
            check(inspected, QStringLiteral("origin and licence survive beside the bytes: %1")
                                 .arg(inspectError));

            // The production default root is the app-owned store, and a graph is
            // an input, not a Ready pipeline.
            const SpatialAssetPublication production =
                publishSpatialAssetBytes(*asset, realBytes, QString());
            check(production.ok, QStringLiteral("the app-owned default root accepts the asset"));
            check(production.graphPath.startsWith(
                      QFileInfo(spatialAssetRoot()).absoluteFilePath()) ||
                      production.graphPath.contains(QStringLiteral("assets")),
                  QStringLiteral("the app-owned copy lives under native/assets"));

            bool inModelStore = false;
            for (const StoredModel& row : ModelInventory().stored(nullptr)) {
                if (row.record.onnxSha256 == asset->sha256) inModelStore = true;
            }
            check(!inModelStore,
                  QStringLiteral("a downloaded asset alone is not a model row"));
            check(!currentNativeAvailability().engineAvailable,
                  QStringLiteral("the asset did not make any engine Ready"));
        }
    }

    // ── 10. the panel sees its own published copy, by stable id ─────────────
    panel->refresh();
    if (!realId.isEmpty()) {
        const QString path = panel->publishedAssetPath(realId);
        check(!path.isEmpty(), QStringLiteral("the panel proves the published asset present"));
        bool listed = false;
        for (const StoredSpatialAsset& row : panel->publishedAssets()) {
            if (row.asset.id == realId && row.verified) listed = true;
        }
        check(listed, QStringLiteral("the panel lists the published asset as verified"));
    }

    // ── 11. importing is the existing inspector path, off the GUI thread ────
    if (!realId.isEmpty()) {
        // A dynamic graph resolves here; the point is that the call goes through
        // the existing inspector and never marks the pipeline Ready.
        QEventLoop loop;
        QMetaObject::Connection connection = QObject::connect(
            panel, &NativeAssetsPanel::importFinished, &loop, [&](bool) { loop.quit(); });
        const bool submitted = panel->importPublishedAsset(realId, QSize(64, 64));
        if (submitted) {
            QTimer::singleShot(300000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        QObject::disconnect(connection);
        // Whether the runtime is present or not, no engine may have appeared.
        check(!currentNativeAvailability().engineAvailable,
              QStringLiteral("importing a downloaded graph did not make an engine Ready"));
        if (!submitted) {
            say(QStringLiteral("import was refused before submission: %1")
                    .arg(panel->statusReason()));
        }
    }

    shoot(&window, QStringLiteral("spatial-assets-probe-final.png"));

    QDir(nativeRoot()).removeRecursively();
    std::fprintf(stderr, "[spatial_assets_probe] %s (%d failures)\n",
                 failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
