#include "TestNativeAssets.hpp"

#include "dialogs/NativeAssetsPanel.hpp"
#include "native/NativeArtifactStore.hpp"
#include "native/NativeFrameReference.hpp"
#include "native/NativeModelInventory.hpp"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/QVector>
#include <QtCore/QtEndian>

#include <cmath>

using namespace mplayer;
using namespace mplayer::native;
using mikai::app::NativeCacheEntry;
using mikai::app::deleteNativeCacheEntry;
using mikai::app::enumerateNativeCache;

// The cache surface's proof rules against real temporary directories. No engine
// is built, no runtime is loaded and no widget is shown: what is asserted is
// which directories become cache entries at all, and that deletion refuses
// everything that did not first prove itself.
namespace {

QString hexA() { return QString(64, QLatin1Char('a')); }
QString hexB() { return QString(64, QLatin1Char('b')); }
QString hexF() { return QString(64, QLatin1Char('f')); }

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

float fp16Value(const QByteArray& bytes, qsizetype index) {
  const auto* data = reinterpret_cast<const uchar*>(bytes.constData());
  const quint16 half = qFromLittleEndian<quint16>(data + index * 2);
  const float sign = (half & 0x8000u) == 0 ? 1.0f : -1.0f;
  const quint16 exponent = (half >> 10) & 0x1Fu;
  const quint16 mantissa = half & 0x03FFu;
  if (exponent == 0) return sign * std::ldexp(static_cast<float>(mantissa), -24);
  return sign * std::ldexp(static_cast<float>(1024u + mantissa),
                           static_cast<int>(exponent) - 25);
}

}  // namespace

void TestNativeAssets::rgbFrameComponentsArePreservedInNchwFp16Planes() {
  FrameFixture fixture;
  fixture.size = QSize(3, 2);
  fixture.rgb = QByteArray::fromHex(
      "ff0040"  // red-heavy
      "0080ff"  // blue-heavy
      "20c060"
      "e010a0"
      "115599"
      "f03f07");

  constexpr qsizetype planePixels = 3 * 2;
  // Largest half-ULP rounding error for normalized values in [0, 1].
  constexpr float maxFp16RoundingError = 0.000245f;
  QCOMPARE(fixture.rgb.size(), planePixels * 3);

  const QByteArray tensor = frameToFp16Tensor(fixture);
  QCOMPARE(tensor.size(), planePixels * 3 * 2);

  for (qsizetype channel = 0; channel < 3; ++channel) {
    for (qsizetype pixel = 0; pixel < planePixels; ++pixel) {
      const qsizetype rgbIndex = pixel * 3 + channel;
      const qsizetype nchwIndex = channel * planePixels + pixel;
      const float normalizedRgb =
          static_cast<float>(static_cast<quint8>(fixture.rgb.at(rgbIndex))) / 255.0f;
      const float packedFp16 = fp16Value(tensor, nchwIndex);
      const float error = std::abs(packedFp16 - normalizedRgb);
      QVERIFY2(error <= maxFp16RoundingError,
               qPrintable(QStringLiteral("channel %1 pixel %2: packed %3, expected normalized RGB %4, error %5")
                              .arg(channel)
                              .arg(pixel)
                              .arg(packedFp16, 0, 'g', 9)
                              .arg(normalizedRgb, 0, 'g', 9)
                              .arg(error, 0, 'g', 9)));
    }
  }
}

// The publication gate is the one place an import commits. These two cases pin
// its two orderings: a cancel that arrives first stops the write, and a cancel
// that arrives after the gate has been passed is refused by name instead of
// being mistaken for a successful cancellation.
void TestNativeAssets::cancellingBeforePublicationBlocksIt() {
  ImportCancellation cancellation;
  QVERIFY(!cancellation.isCancelled());

  QVERIFY(cancellation.cancel() == ImportCancellation::CancelResult::CancelledNow);
  QVERIFY(cancellation.isCancelled());
  QVERIFY(cancellation.state() == ImportCancellation::State::Cancelled);

  // The worker's gate can no longer move the state, so nothing is published.
  QVERIFY(!cancellation.beginPublication());
  QVERIFY(cancellation.isCancelled());

  // A second cancel is idempotent, not a new transition.
  QVERIFY(cancellation.cancel() == ImportCancellation::CancelResult::AlreadyCancelled);
}

void TestNativeAssets::publicationHavingBegunRefusesCancellation() {
  ImportCancellation cancellation;

  // The worker reaches the gate first: the state is now Publishing, which no
  // cancel can leave.
  QVERIFY(cancellation.beginPublication());
  QVERIFY(cancellation.state() == ImportCancellation::State::Publishing);

  // A cancel arriving now must not claim to have stopped anything: the committed
  // outcome stays known and is not re-labelled as a cancellation.
  QVERIFY(cancellation.cancel() == ImportCancellation::CancelResult::TooLate);
  QVERIFY(!cancellation.isCancelled());
  QVERIFY(cancellation.state() == ImportCancellation::State::Publishing);

  // The gate is single-shot: a second publication attempt is refused.
  QVERIFY(!cancellation.beginPublication());
}

void TestNativeAssets::onlyArtifactDirectoriesAreCacheEntries() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  const ArtifactStore store(temp.path());

  // Two artifact-shaped directories: one with no manifest at all and one whose
  // manifest is not a manifest. Both are ours by name and neither proves.
  QVERIFY(QDir().mkpath(QDir(temp.path()).filePath(QStringLiteral("artifact-") + hexA())));
  const QString malformed = QDir(temp.path()).filePath(QStringLiteral("artifact-") + hexB());
  QVERIFY(QDir().mkpath(malformed));
  QVERIFY(writeBytes(QDir(malformed).filePath(ArtifactStore::artifactManifestFileName()),
                     QByteArrayLiteral("not-a-manifest")));

  // Nothing of the following is a cache entry: a name that is not a digest, a
  // staging directory and a foreign user directory.
  QVERIFY(QDir().mkpath(QDir(temp.path()).filePath(QStringLiteral("artifact-nothex"))));
  QVERIFY(QDir().mkpath(QDir(temp.path()).filePath(QStringLiteral(".staging-dead"))));
  QVERIFY(QDir().mkpath(QDir(temp.path()).filePath(QStringLiteral("usermodels"))));

  const QVector<NativeCacheEntry> entries = enumerateNativeCache(store);
  QCOMPARE(int(entries.size()), 2);
  for (const NativeCacheEntry& entry : entries) {
    QVERIFY(!entry.proven);
    QVERIFY(entry.buildKey == QStringLiteral("sha256:") + hexA() ||
            entry.buildKey == QStringLiteral("sha256:") + hexB());
    QVERIFY(entry.directory.startsWith(QDir(temp.path()).absolutePath()));
  }
}

void TestNativeAssets::deletingAnUnknownCacheEntryTouchesNothing() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  const ArtifactStore store(temp.path());

  const QString artifact = QDir(temp.path()).filePath(QStringLiteral("artifact-") + hexA());
  QVERIFY(QDir().mkpath(artifact));
  const QString sentinelPath = QDir(temp.path()).filePath(QStringLiteral("user.onnx"));
  const QByteArray sentinel = QByteArrayLiteral("USER-FILE-DO-NOT-DELETE");
  QVERIFY(writeBytes(sentinelPath, sentinel));

  const QVector<NativeCacheEntry> entries = enumerateNativeCache(store);
  QCOMPARE(int(entries.size()), 1);

  QString key;
  QString reason;
  QVERIFY(!deleteNativeCacheEntry(store, entries, QStringLiteral("sha256:") + hexF(), &key,
                                  &reason));
  QCOMPARE(key, QStringLiteral("native.cache.unknown"));
  QVERIFY(QFileInfo(artifact).isDir());
  QCOMPARE(readBytes(sentinelPath), sentinel);
}

void TestNativeAssets::deletingAnUnprovenCacheEntryCarriesItsOwnReason() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  const ArtifactStore store(temp.path());

  const QString artifact = QDir(temp.path()).filePath(QStringLiteral("artifact-") + hexA());
  QVERIFY(QDir().mkpath(artifact));

  const QVector<NativeCacheEntry> entries = enumerateNativeCache(store);
  QCOMPARE(int(entries.size()), 1);
  QVERIFY(!entries.first().proven);
  // An artifact directory with no manifest is refused by the store itself.
  QCOMPARE(entries.first().reasonKey, QStringLiteral("artifact.manifestUnreadable"));

  QString key;
  QString reason;
  QVERIFY(!deleteNativeCacheEntry(store, entries, entries.first().buildKey, &key, &reason));
  // The refusal is the entry's own reason, not a fresh guess, and the directory
  // is untouched.
  QCOMPARE(key, entries.first().reasonKey);
  QVERIFY(QFileInfo(artifact).isDir());
  QCOMPARE(int(QDir(artifact).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size()), 0);
}

void TestNativeAssets::anUnownedRootYieldsNoEntries() {
  // A relative root is not app-owned, so nothing beside it is ever enumerated.
  const ArtifactStore store(QStringLiteral("relative/not-owned"));
  QVERIFY(!store.isOwned(nullptr));
  QVERIFY(enumerateNativeCache(store).isEmpty());
}
