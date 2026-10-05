// The whole-plan CPU reference lineage worker (see the header for the contract).
//
// It owns three things and nothing else: the exact geometry transitions between
// stages, the original's own packers/resampler, and one bounded synchronous call
// to the existing `mikai_native_inspect` per stage whose output a later stage
// consumes. No ONNX Runtime is linked here; the inspector is the only thing that
// loads it, exactly as the builder already does.

#include "NativePreparationReferences.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QSaveFile>
#include <QtCore/QtEndian>

namespace mplayer {
namespace native {

namespace {

/// A hard bound on one inspector call. A CPU reference run is bounded work; a
/// helper that never returns must not hold the preparation lease forever, and a
/// killed helper is reported as `reference.timeout` rather than as a build.
constexpr int kReferenceRunTimeoutMs = 15 * 60 * 1000;

constexpr const char* kOrtModule = "onnxruntime.dll";

constexpr auto kReferenceManifestName = "reference.json";

void setReason(QString* key, QString* reason, const QString& k, const QString& r) {
  if (key != nullptr) *key = k;
  if (reason != nullptr) *reason = r;
}

bool refuse(QString* key, QString* reason, const QString& k, const QString& r) {
  setReason(key, reason, k, r);
  return false;
}

/// Binary32 -> binary16, round to nearest even, subnormals flushed to zero. The
/// same rules as the canonical frame packer's converter, which is private to its
/// translation unit; this is the auxiliary-plane conversion (timestep, mesh and
/// multipliers) at that same precision, and the tests hold it to the original's
/// own bytes.
quint16 halfFromFloat(float value) {
  quint32 bits;
  static_assert(sizeof(bits) == sizeof(value), "a binary32 float is 32 bits");
  memcpy(&bits, &value, sizeof(bits));

  const quint32 sign = (bits >> 16) & 0x8000u;
  const qint32 exponent = static_cast<qint32>((bits >> 23) & 0xFFu) - 127 + 15;
  const quint32 mantissa = bits & 0x007FFFFFu;

  if (exponent <= 0) {
    return static_cast<quint16>(sign);
  }
  if (exponent >= 31) {
    return static_cast<quint16>(sign | 0x7C00u);
  }
  const quint32 halfMantissa = mantissa >> 13;
  const quint32 remainder = mantissa & 0x1FFFu;
  quint32 result = sign | (static_cast<quint32>(exponent) << 10) | halfMantissa;
  if (remainder > 0x1000u || (remainder == 0x1000u && (halfMantissa & 1u) != 0u)) {
    ++result;
  }
  return static_cast<quint16>(result & 0xFFFFu);
}

float halfToFloat(quint16 half) {
  const quint32 sign = (half >> 15) & 0x1u;
  const quint32 exponent = (half >> 10) & 0x1Fu;
  const quint32 mantissa = half & 0x3FFu;
  float value = 0.0f;
  if (exponent == 0) {
    value = std::ldexp(static_cast<float>(mantissa), -24);
  } else if (exponent == 31) {
    value = mantissa == 0 ? std::numeric_limits<float>::infinity()
                          : std::numeric_limits<float>::quiet_NaN();
  } else {
    value = std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f,
                       static_cast<int>(exponent) - 15);
  }
  return sign != 0 ? -value : value;
}

quint16 readHalf(const QByteArray& bytes, qsizetype index) {
  return qFromLittleEndian<quint16>(
      reinterpret_cast<const uchar*>(bytes.constData()) + index * 2);
}

void writeHalf(QByteArray* bytes, qsizetype index, quint16 half) {
  qToLittleEndian<quint16>(half, bytes->data() + index * 2);
}

qint64 planeElements(const QSize& size) {
  return static_cast<qint64>(size.width()) * size.height();
}

bool isUsableGeometry(const QSize& size) {
  return size.width() > 0 && size.height() > 0 && size.width() <= kMaxDimension &&
         size.height() <= kMaxDimension;
}

/// The reference's device mirror (`mirr`): half-sample reflection, then clamp.
int mirrorIndex(int index, int count) {
  index = index < 0 ? -index - 1 : index;
  index = index >= count ? 2 * count - 1 - index : index;
  return std::min(std::max(index, 0), count - 1);
}

double spline36Kernel(double x) {
  x = std::fabs(x);
  if (x < 1.0) {
    return ((13.0 / 11.0 * x - 453.0 / 209.0) * x - 3.0 / 209.0) * x + 1.0;
  }
  if (x < 2.0) {
    x -= 1.0;
    return ((-6.0 / 11.0 * x + 270.0 / 209.0) * x - 156.0 / 209.0) * x;
  }
  if (x < 3.0) {
    x -= 2.0;
    return ((1.0 / 11.0 * x - 45.0 / 209.0) * x + 26.0 / 209.0) * x;
  }
  return 0.0;
}

struct ResamplePass {
  int taps = 0;
  int src = 0;
  int dst = 0;
  QVector<int> start;
  QVector<float> weights;
};

/// zimg's `compute_filter`, exactly: double positions, taps from the stretched
/// support, weights normalised to one and stored as float32.
ResamplePass computeResamplePass(int src, int dst, double shift) {
  constexpr double kSupport = 3.0;
  const double scale = static_cast<double>(dst) / src;
  const double step = scale < 1.0 ? scale : 1.0;
  int taps = static_cast<int>(std::ceil(kSupport / step)) * 2;
  if (taps < 1) taps = 1;

  ResamplePass pass;
  pass.taps = taps;
  pass.src = src;
  pass.dst = dst;
  pass.start.resize(dst);
  pass.weights.resize(static_cast<qsizetype>(dst) * taps);
  QVector<double> row(taps);
  for (int i = 0; i < dst; ++i) {
    const double position = (i + 0.5) / scale + shift;
    const double begin = std::floor(position - taps / 2.0 + 0.5) + 0.5;
    double sum = 0.0;
    for (int j = 0; j < taps; ++j) {
      row[j] = spline36Kernel((begin + j - position) * step);
      sum += row[j];
    }
    pass.start[i] = static_cast<int>(begin - 0.5);
    for (int j = 0; j < taps; ++j) {
      pass.weights[static_cast<qsizetype>(i) * taps + j] =
          static_cast<float>(row[j] / sum);
    }
  }
  return pass;
}

bool isPlainFile(const QString& path, QString* canonicalOut = nullptr) {
  const QFileInfo info(QDir::cleanPath(path));
  if (!info.isAbsolute() || !info.isFile()) return false;
  if (canonicalOut != nullptr) *canonicalOut = info.absoluteFilePath();
  return true;
}

bool isPlainDirectory(const QString& path, QString* canonicalOut = nullptr) {
  const QFileInfo info(QDir::cleanPath(path));
  if (!info.isAbsolute() || !info.isDir()) return false;
  if (canonicalOut != nullptr) *canonicalOut = info.absoluteFilePath();
  return true;
}

QString toolPathList(const BuilderToolPaths& tools) {
  QStringList parts;
  if (!tools.trtBinDir.isEmpty()) parts.append(QDir::toNativeSeparators(tools.trtBinDir));
  if (!tools.cudaBinDir.isEmpty()) parts.append(QDir::toNativeSeparators(tools.cudaBinDir));
  const QString systemRoot =
      QProcessEnvironment::systemEnvironment().value(QStringLiteral("SystemRoot"),
                                                     QStringLiteral("C:\\Windows"));
  parts.append(QDir::toNativeSeparators(systemRoot + QStringLiteral("\\System32")));
  parts.append(QDir::toNativeSeparators(systemRoot));
  return parts.join(QLatin1Char(';'));
}

/// The same child environment the builder gives its own inspector call: only the
/// observed provider directories, the system directories and this executable's
/// own directory, never the inherited PATH.
QProcessEnvironment inspectEnvironment(const BuilderToolPaths& tools) {
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("PATH"), toolPathList(tools));
  environment.remove(QStringLiteral("LD_LIBRARY_PATH"));
  environment.remove(QStringLiteral("DYLD_LIBRARY_PATH"));
  const QString exeDir = QFileInfo(QCoreApplication::applicationFilePath()).absolutePath();
  if (!exeDir.isEmpty()) {
    environment.insert(QStringLiteral("PATH"),
                       QDir::toNativeSeparators(exeDir) + QLatin1Char(';') +
                           environment.value(QStringLiteral("PATH")));
  }
  return environment;
}

struct ToolRun {
  bool started = false;
  bool cancelled = false;
  bool timedOut = false;
  int exitCode = -1;
  QProcess::ExitStatus exitStatus = QProcess::CrashExit;
  QString launchError;
  QByteArray output;
};

/// One bounded, cancellable inspector call. The helper is killed on cancel or on
/// the timeout, and the caller names which happened.
ToolRun runInspector(const QString& executable, const QStringList& arguments,
                     const QProcessEnvironment& environment,
                     NativeBuildCancellation* cancellation) {
  ToolRun run;
  QProcess process;
  process.setProcessEnvironment(environment);
  process.setProgram(executable);
  process.setArguments(arguments);
  process.start();
  if (!process.waitForStarted(10000)) {
    run.launchError = process.errorString();
    return run;
  }
  run.started = true;
  QElapsedTimer elapsed;
  elapsed.start();
  while (process.state() != QProcess::NotRunning) {
    if (cancellation != nullptr && cancellation->isCancelled()) {
      process.kill();
      process.waitForFinished(5000);
      run.cancelled = true;
      break;
    }
    if (elapsed.elapsed() > kReferenceRunTimeoutMs) {
      process.kill();
      process.waitForFinished(5000);
      run.timedOut = true;
      break;
    }
    process.waitForFinished(50);
  }
  if (!run.cancelled && !run.timedOut) {
    run.exitStatus = process.exitStatus();
    run.exitCode = process.exitCode();
  }
  run.output = process.readAllStandardOutput();
  return run;
}

QString tailText(const QByteArray& bytes) {
  const QByteArray trimmed = bytes.trimmed();
  constexpr qsizetype kMax = 400;
  if (trimmed.size() <= kMax) return QString::fromUtf8(trimmed);
  return QString::fromUtf8(trimmed.right(kMax));
}

/// The last JSON document the helper printed on stdout. The inspector prints one
/// compact array for `--json`, so this is the array itself; a non-JSON tail is
/// refused by the caller.
std::optional<QJsonDocument> lastJson(const QByteArray& output) {
  const QByteArray trimmed = output.trimmed();
  if (trimmed.isEmpty()) return std::nullopt;
  const qsizetype open = trimmed.indexOf('[');
  const qsizetype close = trimmed.lastIndexOf(']');
  if (open < 0 || close <= open) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(trimmed, &error);
    if (error.error != QJsonParseError::NoError) return std::nullopt;
    return document;
  }
  QJsonParseError error{};
  const QJsonDocument document =
      QJsonDocument::fromJson(trimmed.mid(open, close - open + 1), &error);
  if (error.error != QJsonParseError::NoError) return std::nullopt;
  return document;
}

/// One stage's real input tensor: planar fp16, channels-major.
struct StageTensor {
  QSize size;
  int channels = 3;
  QByteArray fp16;
};

bool tensorBytesMatch(const StageTensor& tensor) {
  if (!isUsableGeometry(tensor.size) || tensor.channels <= 0) return false;
  return tensor.fp16.size() ==
         static_cast<qsizetype>(planeElements(tensor.size) * tensor.channels) * 2;
}

/// Writes a file that must not exist yet. An export never overwrites.
bool writeNewFile(const QString& path, const QByteArray& bytes, QString* reasonKey,
                  QString* reason) {
  if (QFileInfo::exists(path)) {
    return refuse(reasonKey, reason, QStringLiteral("reference.write.exists"),
                  QStringLiteral("'%1' already exists; a lineage file is never overwritten")
                      .arg(path));
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    return refuse(reasonKey, reason, QStringLiteral("reference.write"),
                  QStringLiteral("'%1' could not be opened for writing: %2")
                      .arg(path, file.errorString()));
  }
  if (file.write(bytes) != bytes.size() || !file.commit()) {
    return refuse(reasonKey, reason, QStringLiteral("reference.write"),
                  QStringLiteral("'%1' could not be written completely: %2")
                      .arg(path, file.errorString()));
  }
  return true;
}

bool readWholeFile(const QString& path, QByteArray* out, QString* reasonKey, QString* reason) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return refuse(reasonKey, reason, QStringLiteral("reference.read"),
                  QStringLiteral("'%1' could not be read: %2").arg(path, file.errorString()));
  }
  *out = file.readAll();
  return true;
}

}  // namespace

// ---------------------------------------------------------------- geometry

bool applySourceGeometryToFrame(const ReferenceFrame& frame, const SourceGeometry& source,
                                FrameFixture* out, QString* reasonKey, QString* reason) {
  if (out == nullptr) {
    return refuse(reasonKey, reason, QStringLiteral("reference.frame.output"),
                  QStringLiteral("no frame output was supplied"));
  }
  const FrameFixture& input = frame.fixture;
  if (input.size.width() <= 0 || input.size.height() <= 0 ||
      input.rgb.size() != static_cast<qsizetype>(input.size.width()) * input.size.height() * 3) {
    return refuse(reasonKey, reason, QStringLiteral("reference.frame.bytes"),
                  QStringLiteral("the captured frame is %1x%2 with %3 RGB bytes, which is not an "
                                 "interleaved RGB8 frame")
                      .arg(input.size.width())
                      .arg(input.size.height())
                      .arg(input.rgb.size()));
  }

  const std::optional<QSize> display = source.displaySize();
  if (!display.has_value()) {
    return refuse(reasonKey, reason, QStringLiteral("reference.source.geometry"),
                  QStringLiteral("the source's crop and rotation do not resolve to a display "
                                 "geometry this build can apply"));
  }

  if (!frame.unprocessed) {
    // Already display geometry. A frame that is not the size the plan was built
    // for is refused, never stretched to fit.
    if (input.size != *display) {
      return refuse(reasonKey, reason, QStringLiteral("reference.frame.geometry"),
                    QStringLiteral("the frame says it is already display geometry at %1x%2, but "
                                   "the source display geometry is %3x%4")
                        .arg(input.size.width())
                        .arg(input.size.height())
                        .arg(display->width())
                        .arg(display->height()));
    }
    *out = input;
    return true;
  }

  // Coded bytes: the decoder's own report must describe exactly this frame.
  if (!source.codedSize.has_value() || *source.codedSize != input.size) {
    return refuse(reasonKey, reason, QStringLiteral("reference.frame.coded"),
                  QStringLiteral("the frame is %1x%2, which is not the coded size the decoder "
                                 "reported")
                      .arg(input.size.width())
                      .arg(input.size.height()));
  }

  QSize size = input.size;
  QByteArray rgb = input.rgb;
  if (source.crop.has_value()) {
    const QRect crop = *source.crop;
    if (crop.isEmpty() || crop.x() < 0 || crop.y() < 0 || crop.x() + crop.width() > size.width() ||
        crop.y() + crop.height() > size.height()) {
      return refuse(reasonKey, reason, QStringLiteral("reference.source.crop"),
                    QStringLiteral("the reported crop %1,%2 %3x%4 does not lie inside the coded "
                                   "%5x%6 frame")
                        .arg(crop.x())
                        .arg(crop.y())
                        .arg(crop.width())
                        .arg(crop.height())
                        .arg(size.width())
                        .arg(size.height()));
    }
    const int rowBytes = crop.width() * 3;
    QByteArray cut(static_cast<qsizetype>(rowBytes) * crop.height(), '\0');
    for (int y = 0; y < crop.height(); ++y) {
      const qsizetype sourceOffset = (static_cast<qsizetype>(crop.y() + y) * size.width() +
                                      crop.x()) * 3;
      memcpy(cut.data() + static_cast<qsizetype>(y) * rowBytes, rgb.constData() + sourceOffset,
             static_cast<size_t>(rowBytes));
    }
    rgb = cut;
    size = crop.size();
  }

  if (source.rotationDegrees.has_value()) {
    const int degrees = *source.rotationDegrees;
    if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) {
      return refuse(reasonKey, reason, QStringLiteral("reference.source.rotation"),
                    QStringLiteral("the decoder reported a %1 degree rotation, which this build "
                                   "does not model")
                        .arg(degrees));
    }
    if (degrees != 0) {
      const int sourceWidth = size.width();
      const int sourceHeight = size.height();
      const int targetWidth = (degrees == 90 || degrees == 270) ? sourceHeight : sourceWidth;
      const int targetHeight = (degrees == 90 || degrees == 270) ? sourceWidth : sourceHeight;
      QByteArray rotated(static_cast<qsizetype>(targetWidth) * targetHeight * 3, '\0');
      for (int y = 0; y < targetHeight; ++y) {
        for (int x = 0; x < targetWidth; ++x) {
          int sx = x;
          int sy = y;
          if (degrees == 90) {          // clockwise, matching mpv's video-rotate
            sx = y;
            sy = sourceHeight - 1 - x;
          } else if (degrees == 180) {
            sx = sourceWidth - 1 - x;
            sy = sourceHeight - 1 - y;
          } else {                      // 270 clockwise
            sx = sourceWidth - 1 - y;
            sy = x;
          }
          const qsizetype sourceOffset = (static_cast<qsizetype>(sy) * sourceWidth + sx) * 3;
          const qsizetype targetOffset = (static_cast<qsizetype>(y) * targetWidth + x) * 3;
          memcpy(rotated.data() + targetOffset, rgb.constData() + sourceOffset, 3);
        }
      }
      rgb = rotated;
      size = QSize(targetWidth, targetHeight);
    }
  }

  if (size != *display) {
    return refuse(reasonKey, reason, QStringLiteral("reference.frame.geometry"),
                  QStringLiteral("the crop and rotation produce %1x%2, but the source's display "
                                 "geometry is %3x%4")
                    .arg(size.width())
                    .arg(size.height())
                    .arg(display->width())
                    .arg(display->height()));
  }

  out->size = size;
  out->rgb = rgb;
  out->path = input.path;
  out->fileBytes = input.fileBytes;
  out->fileSha256 = input.fileSha256;
  return true;
}

bool resampleFp16Planar(const QByteArray& source, const QSize& from, const QSize& to, int channels,
                        QByteArray* out, QString* reasonKey, QString* reason) {
  if (out == nullptr) {
    return refuse(reasonKey, reason, QStringLiteral("reference.resize.output"),
                  QStringLiteral("no resize output was supplied"));
  }
  if (!isUsableGeometry(from) || !isUsableGeometry(to) || channels <= 0) {
    return refuse(reasonKey, reason, QStringLiteral("reference.resize.geometry"),
                  QStringLiteral("a resize needs a positive bounded source and target and at "
                                 "least one channel"));
  }
  const qsizetype expected =
      static_cast<qsizetype>(planeElements(from) * channels) * 2;
  if (source.size() != expected) {
    return refuse(reasonKey, reason, QStringLiteral("reference.resize.bytes"),
                  QStringLiteral("the resize source holds %1 bytes but %2x%3 with %4 channels "
                                 "needs %5")
                      .arg(source.size())
                      .arg(from.width())
                      .arg(from.height())
                      .arg(channels)
                      .arg(expected));
  }

  const ResamplePass horizontal = computeResamplePass(from.width(), to.width(), 0.0);
  const ResamplePass vertical = computeResamplePass(from.height(), to.height(), 0.0);
  const qint64 sourcePlane = planeElements(from);
  const qint64 targetPlane = planeElements(to);

  QByteArray result(static_cast<qsizetype>(targetPlane * channels) * 2, '\0');
  // The horizontal pass writes one row per *source* row, so the intermediate is
  // as tall as `from`: the vertical pass then gathers from it by source index.
  // Sizing it by the target plane overflows it whenever the target is shorter.
  QVector<float> intermediate(static_cast<qsizetype>(from.height()) * to.width(), 0.0f);
  for (int channel = 0; channel < channels; ++channel) {
    const qint64 sourceBase = channel * sourcePlane;
    // Horizontal pass: fp16 in, float32 out, j ascending - the reference's own
    // accumulation order, because float addition is not associative.
    for (int y = 0; y < from.height(); ++y) {
      for (int x = 0; x < to.width(); ++x) {
        const int first = horizontal.start.at(x);
        const float* weights = horizontal.weights.constData() + static_cast<qsizetype>(x) *
                                                                      horizontal.taps;
        float accumulated = 0.0f;
        for (int j = 0; j < horizontal.taps; ++j) {
          const int sx = mirrorIndex(first + j, horizontal.src);
          accumulated += weights[j] * halfToFloat(readHalf(source, sourceBase +
                                                                       static_cast<qint64>(y) *
                                                                           horizontal.src +
                                                                       sx));
        }
        intermediate[static_cast<qsizetype>(y) * to.width() + x] = accumulated;
      }
    }
    // Vertical pass: float32 in, fp16 out.
    for (int y = 0; y < to.height(); ++y) {
      const int first = vertical.start.at(y);
      const float* weights =
          vertical.weights.constData() + static_cast<qsizetype>(y) * vertical.taps;
      for (int x = 0; x < to.width(); ++x) {
        float accumulated = 0.0f;
        for (int j = 0; j < vertical.taps; ++j) {
          const int sy = mirrorIndex(first + j, vertical.src);
          accumulated +=
              weights[j] * intermediate[static_cast<qsizetype>(sy) * to.width() + x];
        }
        writeHalf(&result, channel * targetPlane + static_cast<qint64>(y) * to.width() + x,
                  halfFromFloat(accumulated));
      }
    }
  }
  *out = result;
  return true;
}

bool packRifeElevenChannel(const QByteArray& a, const QByteArray& b, const QSize& content,
                           int padL, int padT, const QSize& padded, double timestep,
                           QByteArray* out, QString* reasonKey, QString* reason) {
  if (out == nullptr) {
    return refuse(reasonKey, reason, QStringLiteral("reference.rife.output"),
                  QStringLiteral("no RIFE output was supplied"));
  }
  if (!isUsableGeometry(content) || !isUsableGeometry(padded)) {
    return refuse(reasonKey, reason, QStringLiteral("reference.rife.geometry"),
                  QStringLiteral("a RIFE input needs a positive bounded content and padded "
                                 "geometry"));
  }
  if (padL < 0 || padT < 0 || padL + content.width() > padded.width() ||
      padT + content.height() > padded.height()) {
    return refuse(reasonKey, reason, QStringLiteral("reference.rife.pad"),
                  QStringLiteral("the %1x%2 content does not fit the %3x%4 padded plane at "
                                 "(%5, %6)")
                      .arg(content.width())
                      .arg(content.height())
                      .arg(padded.width())
                      .arg(padded.height())
                      .arg(padL)
                      .arg(padT));
  }
  if (padded.width() < 2 || padded.height() < 2) {
    return refuse(reasonKey, reason, QStringLiteral("reference.rife.geometry"),
                  QStringLiteral("the %1x%2 padded plane cannot carry the mesh grid, whose "
                                 "divisor is the side minus one")
                      .arg(padded.width())
                      .arg(padded.height()));
  }
  if (!(timestep > 0.0 && timestep < 1.0)) {
    return refuse(reasonKey, reason, QStringLiteral("reference.rife.timestep"),
                  QStringLiteral("the timestep %1 is not strictly inside (0, 1), which is the "
                                 "only range the original interpolates in")
                      .arg(timestep));
  }
  const qint64 contentPlane = planeElements(content);
  const qsizetype expected = static_cast<qsizetype>(contentPlane * 3) * 2;
  if (a.size() != expected || b.size() != expected) {
    return refuse(reasonKey, reason, QStringLiteral("reference.rife.bytes"),
                  QStringLiteral("the two three-channel inputs must each be %1 bytes at %2x%3, but "
                                 "they are %4 and %5")
                      .arg(expected)
                      .arg(content.width())
                      .arg(content.height())
                      .arg(a.size())
                      .arg(b.size()));
  }

  const qint64 paddedPlane = planeElements(padded);
  QByteArray tensor(static_cast<qsizetype>(paddedPlane * 11) * 2, '\0');

  const auto place = [&](int firstChannel, const QByteArray& source) {
    for (int channel = 0; channel < 3; ++channel) {
      const qint64 sourceBase = channel * contentPlane;
      const qint64 targetBase = (firstChannel + channel) * paddedPlane;
      for (int y = 0; y < content.height(); ++y) {
        for (int x = 0; x < content.width(); ++x) {
          const quint16 half = readHalf(source, sourceBase + static_cast<qint64>(y) *
                                                              content.width() + x);
          const qint64 target = targetBase +
                                static_cast<qint64>(padT + y) * padded.width() + (padL + x);
          writeHalf(&tensor, target, half);
        }
      }
    }
  };
  place(0, a);
  place(3, b);

  // Channel 6: the timestep plane.
  {
    const quint16 half = halfFromFloat(static_cast<float>(timestep));
    for (qint64 pixel = 0; pixel < paddedPlane; ++pixel) {
      writeHalf(&tensor, 6 * paddedPlane + pixel, half);
    }
  }

  // Channels 7..10: the original's constant planes, over the padded plane.
  const float widthDivisor = static_cast<float>(padded.width() - 1);
  const float heightDivisor = static_cast<float>(padded.height() - 1);
  const quint16 multiplyWidth = halfFromFloat(2.0f / widthDivisor);
  const quint16 multiplyHeight = halfFromFloat(2.0f / heightDivisor);
  for (int y = 0; y < padded.height(); ++y) {
    const quint16 meshY =
        halfFromFloat(2.0f * static_cast<float>(y) / heightDivisor - 1.0f);
    for (int x = 0; x < padded.width(); ++x) {
      const quint16 meshX =
          halfFromFloat(2.0f * static_cast<float>(x) / widthDivisor - 1.0f);
      const qint64 pixel = static_cast<qint64>(y) * padded.width() + x;
      writeHalf(&tensor, 7 * paddedPlane + pixel, meshX);
      writeHalf(&tensor, 8 * paddedPlane + pixel, meshY);
      writeHalf(&tensor, 9 * paddedPlane + pixel, multiplyWidth);
      writeHalf(&tensor, 10 * paddedPlane + pixel, multiplyHeight);
    }
  }

  *out = tensor;
  return true;
}

bool cropFp16Planar(const QByteArray& padded, const QSize& paddedSize, int padL, int padT,
                    const QSize& content, int channels, QByteArray* out, QString* reasonKey,
                    QString* reason) {
  if (out == nullptr) {
    return refuse(reasonKey, reason, QStringLiteral("reference.crop.output"),
                  QStringLiteral("no crop output was supplied"));
  }
  if (!isUsableGeometry(paddedSize) || !isUsableGeometry(content) || channels <= 0) {
    return refuse(reasonKey, reason, QStringLiteral("reference.crop.geometry"),
                  QStringLiteral("a crop needs a positive bounded padded and content geometry and "
                                 "at least one channel"));
  }
  if (padL < 0 || padT < 0 || padL + content.width() > paddedSize.width() ||
      padT + content.height() > paddedSize.height()) {
    return refuse(reasonKey, reason, QStringLiteral("reference.crop.window"),
                  QStringLiteral("the %1x%2 window at (%3, %4) does not lie inside the %5x%6 "
                                 "padded tensor")
                      .arg(content.width())
                      .arg(content.height())
                      .arg(padL)
                      .arg(padT)
                      .arg(paddedSize.width())
                      .arg(paddedSize.height()));
  }
  const qint64 paddedPlane = planeElements(paddedSize);
  const qint64 contentPlane = planeElements(content);
  const qsizetype expected = static_cast<qsizetype>(paddedPlane * channels) * 2;
  if (padded.size() != expected) {
    return refuse(reasonKey, reason, QStringLiteral("reference.crop.bytes"),
                  QStringLiteral("the padded tensor holds %1 bytes but %2x%3 with %4 channels "
                                 "needs %5")
                      .arg(padded.size())
                      .arg(paddedSize.width())
                      .arg(paddedSize.height())
                      .arg(channels)
                      .arg(expected));
  }

  QByteArray result(static_cast<qsizetype>(contentPlane * channels) * 2, '\0');
  for (int channel = 0; channel < channels; ++channel) {
    const qint64 sourceBase = channel * paddedPlane;
    const qint64 targetBase = channel * contentPlane;
    for (int y = 0; y < content.height(); ++y) {
      for (int x = 0; x < content.width(); ++x) {
        const qint64 source =
            sourceBase + static_cast<qint64>(padT + y) * paddedSize.width() + (padL + x);
        const qint64 target = targetBase + static_cast<qint64>(y) * content.width() + x;
        writeHalf(&result, target, readHalf(padded, source));
      }
    }
  }
  *out = result;
  return true;
}

std::optional<double> rifeFirstTimestep(const Rational& factor) {
  if (!factor.isValid()) return std::nullopt;
  const double timestep = static_cast<double>(factor.den) / static_cast<double>(factor.num);
  if (!(timestep > 0.0 && timestep < 1.0)) return std::nullopt;
  return timestep;
}

// ---------------------------------------------------------------- transitions

bool planStageInputs(const PreparationPlan& plan, int frameCount, QVector<StageInputPlan>* out,
                     QString* reasonKey, QString* reason) {
  if (out == nullptr) {
    return refuse(reasonKey, reason, QStringLiteral("reference.plan.output"),
                  QStringLiteral("no transition output was supplied"));
  }
  if (!plan.isResolved()) {
    if (!plan.blockers.isEmpty()) {
      return refuse(reasonKey, reason, plan.blockers.first().reasonKey,
                    plan.blockers.first().reason);
    }
    return refuse(reasonKey, reason, QStringLiteral("plan.preparing"),
                  plan.unknowns.isEmpty() ? QStringLiteral("the plan is not resolved")
                                          : plan.unknowns.first());
  }
  if (plan.stages.isEmpty()) {
    return refuse(reasonKey, reason, QStringLiteral("reference.plan.stages"),
                  QStringLiteral("the plan resolved to no stages"));
  }
  const std::optional<QSize> display = plan.source.displaySize();
  if (!display.has_value()) {
    return refuse(reasonKey, reason, QStringLiteral("reference.source.geometry"),
                  QStringLiteral("the source's crop and rotation do not resolve to a display "
                                 "geometry"));
  }

  int temporalIndex = -1;
  int temporalCount = 0;
  for (int index = 0; index < plan.stages.size(); ++index) {
    if (plan.stages.at(index).kind == StageKind::Temporal) {
      ++temporalCount;
      temporalIndex = index;
    }
  }
  if (temporalCount > 1) {
    return refuse(reasonKey, reason, QStringLiteral("reference.temporal.multiple"),
                  QStringLiteral("the plan carries %1 temporal stages; one temporal stage is the "
                                 "only contract this build supports")
                      .arg(temporalCount));
  }
  if (temporalIndex >= 0 && frameCount < 2) {
    return refuse(reasonKey, reason, QStringLiteral("reference.frames.pair"),
                  QStringLiteral("the plan carries an interpolation stage, which needs two actual "
                                 "captured frames; %1 were supplied")
                      .arg(frameCount));
  }

  const PreResizeStep* hoisted = nullptr;
  for (const PreResizeStep& step : plan.resizes) {
    if (step.beforeTemporal) {
      hoisted = &step;
      break;
    }
  }

  QVector<StageInputPlan> inputs;
  inputs.reserve(plan.stages.size());
  QSize previousOutput = *display;
  for (int index = 0; index < plan.stages.size(); ++index) {
    const ResolvedStage& stage = plan.stages.at(index);
    const bool temporal = stage.kind == StageKind::Temporal;
    StageInputPlan entry;
    entry.channels = stage.inputChannels;
    entry.tensorGeometry = stage.inputGeometry;
    entry.padL = stage.padL;
    entry.padT = stage.padT;
    entry.contentGeometry = temporal ? stage.contentGeometry : stage.inputGeometry;

    if (temporal) {
      if (index == 0 && stage.order != TemporalOrder::BeforeSpatial) {
        return refuse(reasonKey, reason, QStringLiteral("reference.temporal.order"),
                      QStringLiteral("the interpolation stage runs first but the plan says it "
                                     "runs after the spatial chain"));
      }
      if (index > 0 && stage.order != TemporalOrder::AfterSpatial) {
        return refuse(reasonKey, reason, QStringLiteral("reference.temporal.order"),
                      QStringLiteral("the interpolation stage runs after the spatial chain but "
                                     "the plan says it runs first"));
      }
      entry.source = index == 0 ? StageInputSource::CapturedPair : StageInputSource::ChainPair;
    } else {
      entry.source = index == 0 ? StageInputSource::CapturedFrameA
                                : StageInputSource::PreviousStage;
    }

    const PreResizeStep* step = nullptr;
    for (const PreResizeStep& candidate : plan.resizes) {
      if (candidate.stageIndex == index) {
        step = &candidate;
        break;
      }
    }
    if (temporal && index == 0 && hoisted != nullptr) step = hoisted;

    const QSize expectedSource = index == 0 ? *display : previousOutput;
    if (step != nullptr) {
      if (step->from != expectedSource) {
        return refuse(reasonKey, reason,
                      QStringLiteral("reference.resize.chain"),
                      QStringLiteral("stage %1 names a pre-resize from %2x%3, but its source is "
                                     "%4x%5")
                          .arg(index)
                          .arg(step->from.width())
                          .arg(step->from.height())
                          .arg(expectedSource.width())
                          .arg(expectedSource.height()));
      }
      if (step->to != entry.contentGeometry) {
        return refuse(reasonKey, reason, QStringLiteral("reference.resize.chain"),
                      QStringLiteral("stage %1 names a pre-resize to %2x%3, but the stage input "
                                     "is %4x%5")
                          .arg(index)
                          .arg(step->to.width())
                          .arg(step->to.height())
                          .arg(entry.contentGeometry.width())
                          .arg(entry.contentGeometry.height()));
      }
      entry.resizeNamed = true;
      entry.resizeFrom = step->from;
      entry.resizeTo = step->to;
    } else if (entry.contentGeometry != expectedSource) {
      return refuse(reasonKey, reason, QStringLiteral("reference.stage.chain"),
                    QStringLiteral("stage %1 takes %2x%3 but its source is %4x%5 and the plan "
                                   "names no pre-resize")
                        .arg(index)
                        .arg(entry.contentGeometry.width())
                        .arg(entry.contentGeometry.height())
                        .arg(expectedSource.width())
                        .arg(expectedSource.height()));
    }

    if (!isUsableGeometry(entry.contentGeometry) || !isUsableGeometry(entry.tensorGeometry) ||
        entry.channels <= 0) {
      return refuse(reasonKey, reason, QStringLiteral("reference.stage.geometry"),
                    QStringLiteral("stage %1 has an unbindable %2-channel %3x%4 tensor")
                      .arg(index)
                      .arg(entry.channels)
                      .arg(entry.tensorGeometry.width())
                      .arg(entry.tensorGeometry.height()));
    }

    if (temporal) {
      int alignL = 0;
      int alignT = 0;
      const QSize aligned = padToTensorAlignment(entry.contentGeometry, &alignL, &alignT);
      if (aligned != entry.tensorGeometry || alignL != entry.padL || alignT != entry.padT) {
        return refuse(reasonKey, reason, QStringLiteral("reference.temporal.pad"),
                      QStringLiteral("the interpolation stage at %1x%2 does not carry the "
                                     "centred tensor-alignment padding this build computes")
                          .arg(entry.contentGeometry.width())
                          .arg(entry.contentGeometry.height()));
      }
      if (stage.outputGeometry != entry.tensorGeometry) {
        return refuse(reasonKey, reason, QStringLiteral("reference.temporal.geometry"),
                      QStringLiteral("the interpolation stage binds %1x%2 but reports an output "
                                     "of %3x%4")
                          .arg(entry.tensorGeometry.width())
                          .arg(entry.tensorGeometry.height())
                          .arg(stage.outputGeometry.width())
                          .arg(stage.outputGeometry.height()));
      }
    } else if (entry.tensorGeometry != entry.contentGeometry || entry.padL != 0 ||
               entry.padT != 0) {
      return refuse(reasonKey, reason, QStringLiteral("reference.stage.pad"),
                    QStringLiteral("the spatial stage at index %1 carries padding this build "
                                   "never applies to a spatial stage")
                        .arg(index));
    }

    entry.outputContentGeometry = temporal ? stage.contentGeometry : stage.outputGeometry;
    previousOutput = entry.outputContentGeometry;
    inputs.append(entry);
  }

  *out = inputs;
  return true;
}

// ---------------------------------------------------------------- the worker

namespace {

struct Lineage {
  const PlanReferenceRequest* request = nullptr;
  QVector<StageInputPlan> inputs;
  QVector<QString> inputNames;
  QVector<QString> inputPaths;
  QProcessEnvironment environment;
  QString inspectExecutable;
  QString scratch;
  QVector<BuilderStageReference> references;
};

QString shapeText(const QJsonArray& shape) {
  QStringList parts;
  for (const QJsonValue& dimension : shape) {
    parts.append(QString::number(dimension.toInteger()));
  }
  return parts.join(QLatin1Char('x'));
}

QString rawShapeText(const StageTensor& tensor) {
  return QStringLiteral("1x%1x%2x%3")
      .arg(tensor.channels)
      .arg(tensor.size.height())
      .arg(tensor.size.width());
}

bool cancelledResult(QString* reasonKey, QString* reason) {
  return refuse(reasonKey, reason, QStringLiteral("native.prepare.cancelled"),
                QStringLiteral("the preparation was cancelled while the CPU reference lineage was "
                               "being built"));
}

void progress(Lineage& lineage, int index, const QString& phase, const QString& detail) {
  if (!lineage.request->progress) return;
  BuilderProgress event;
  event.stageIndex = index;
  event.modelId = index >= 0 && index < lineage.inputs.size()
                      ? lineage.request->plan.stages.at(index).modelId
                      : QString();
  event.phase = phase;
  event.detail = detail;
  lineage.request->progress(event);
}

bool checkCancelled(const Lineage& lineage, QString* reasonKey, QString* reason) {
  if (lineage.request->cancellation != nullptr && lineage.request->cancellation->isCancelled()) {
    return cancelledResult(reasonKey, reason);
  }
  return true;
}

/// The graph's real input tensor name, read from the inspector's own declaration
/// of the owned bytes. Cached per model id.
bool inputNameFor(Lineage& lineage, int index, QString* reasonKey, QString* reason) {
  if (!lineage.inputNames.at(index).isEmpty()) return true;
  const QString& modelId = lineage.request->plan.stages.at(index).modelId;
  QString name;
  for (int earlier = 0; earlier < index; ++earlier) {
    if (lineage.request->plan.stages.at(earlier).modelId == modelId &&
        !lineage.inputNames.at(earlier).isEmpty()) {
      name = lineage.inputNames.at(earlier);
      break;
    }
  }
  if (name.isEmpty()) {
    const QString stored = lineage.request->models.at(index).storedPath;
    if (!isPlainFile(stored)) {
      return refuse(reasonKey, reason, QStringLiteral("model.owned.missing"),
                    QStringLiteral("the owned copy of %1 is not a readable file").arg(modelId));
    }
    const ToolRun run =
        runInspector(lineage.inspectExecutable,
                     {QStringLiteral("--runtime"), lineage.request->tools.ortRuntimeDir,
                      QStringLiteral("--json"), stored},
                     lineage.environment, lineage.request->cancellation);
    if (run.cancelled) return cancelledResult(reasonKey, reason);
    if (run.timedOut) {
      return refuse(reasonKey, reason, QStringLiteral("reference.timeout"),
                    QStringLiteral("the inspector did not read %1's declaration within the "
                                   "bound").arg(modelId));
    }
    if (!run.started || run.exitStatus != QProcess::NormalExit) {
      const QString detail =
          run.launchError.isEmpty() ? tailText(run.output) : run.launchError;
      return refuse(reasonKey, reason, QStringLiteral("reference.inspect.failed"),
                    QStringLiteral("the inspector did not report %1's declaration: %2")
                        .arg(modelId, detail));
    }
    // A declared read that the inspector's own spatial eligibility then refused
    // still carries the real input name, so a non-zero exit is not itself a
    // failure here; only a declaration it could not read at all (ok false) is,
    // and the reference run reports everything else.
    const std::optional<QJsonDocument> document = lastJson(run.output);
    if (!document.has_value() || !document->isArray() || document->array().isEmpty()) {
      return refuse(reasonKey, reason, QStringLiteral("reference.inspect.output"),
                    QStringLiteral("the inspector's declaration of %1 is not the JSON array this "
                                   "build reads").arg(modelId));
    }
    const QJsonObject first = document->array().first().toObject();
    if (!first.value(QStringLiteral("ok")).toBool(false) ||
        first.value(QStringLiteral("inputName")).toString().isEmpty()) {
      return refuse(reasonKey, reason,
                    first.value(QStringLiteral("reasonKey")).toString(
                        QStringLiteral("reference.inspect.refused")),
                    first.value(QStringLiteral("reason"))
                        .toString(QStringLiteral("%1 declares no input this build can bind")
                                      .arg(modelId)));
    }
    name = first.value(QStringLiteral("inputName")).toString();
  }
  lineage.inputNames[index] = name;
  return true;
}

/// Writes one stage's genuine input bytes into the scratch directory. `asLineage`
/// makes it the file the `BuilderStageReference` names; the second pass a
/// temporal-after plan needs writes a separate file under `tag` so the two
/// frames' real inputs never collide.
bool writeStageInput(Lineage& lineage, int index, const StageTensor& tensor, bool asLineage,
                     const QString& tag, QString* reasonKey, QString* reason) {
  if (!tensorBytesMatch(tensor)) {
    return refuse(reasonKey, reason, QStringLiteral("reference.tensor.bytes"),
                  QStringLiteral("stage %1's input is not a %2-channel fp16 tensor at %3x%4")
                      .arg(index)
                      .arg(tensor.channels)
                      .arg(tensor.size.width())
                      .arg(tensor.size.height()));
  }
  if (asLineage && !inputNameFor(lineage, index, reasonKey, reason)) return false;
  const QString path = QDir(lineage.scratch)
                           .filePath(QStringLiteral("stage-%1%2-input-fp16.bin")
                                         .arg(index)
                                         .arg(tag));
  if (asLineage) {
    progress(lineage, index, QStringLiteral("reference"),
             QStringLiteral("writing the stage input lineage"));
  }
  if (!writeNewFile(path, tensor.fp16, reasonKey, reason)) return false;
  lineage.inputPaths[index] = path;
  if (!asLineage) return true;

  BuilderRawInput raw;
  raw.name = lineage.inputNames.at(index);
  raw.dtype = QStringLiteral("fp16");
  raw.shape = rawShapeText(tensor);
  raw.filePath = path;
  BuilderStageReference reference;
  reference.rawInputs.append(raw);
  lineage.references.append(reference);
  return true;
}

/// Runs the stage's real original graph over the bytes at `inputPath` and returns
/// the output the original produced. Used only when a later stage consumes it.
bool runStageGraph(Lineage& lineage, int index, const StageTensor& tensor,
                   const QString& inputPath, const QString& tag, StageTensor* output,
                   QString* reasonKey, QString* reason) {
  const ResolvedStage& stage = lineage.request->plan.stages.at(index);
  const QString runDirectory = QDir(lineage.scratch)
                                   .filePath(QStringLiteral("stage-%1%2-reference")
                                                 .arg(index)
                                                 .arg(tag));
  if (QFileInfo::exists(runDirectory)) {
    return refuse(reasonKey, reason, QStringLiteral("reference.directory.exists"),
                  QStringLiteral("'%1' already exists; a reference export always writes a fresh "
                                 "directory").arg(runDirectory));
  }
  progress(lineage, index, QStringLiteral("reference"),
           QStringLiteral("running the original graph on the real stage input"));

  QStringList arguments{QStringLiteral("--runtime"), lineage.request->tools.ortRuntimeDir,
                        QStringLiteral("--reference-dir"), runDirectory,
                        QStringLiteral("--reference-input"), lineage.inputNames.at(index),
                        QStringLiteral("fp16"), rawShapeText(tensor), inputPath,
                        QStringLiteral("--json"),
                        lineage.request->models.at(index).storedPath};
  const ToolRun run =
      runInspector(lineage.inspectExecutable, arguments, lineage.environment,
                   lineage.request->cancellation);
  if (run.cancelled) return cancelledResult(reasonKey, reason);
  if (run.timedOut) {
    return refuse(reasonKey, reason, QStringLiteral("reference.timeout"),
                  QStringLiteral("the original graph for %1 did not return within the bound")
                      .arg(stage.modelId));
  }
  if (!run.started || run.exitStatus != QProcess::NormalExit || run.exitCode != 0) {
    const QString detail = run.launchError.isEmpty() ? tailText(run.output) : run.launchError;
    return refuse(reasonKey, reason, QStringLiteral("reference.export.failed"),
                  QStringLiteral("the original graph for %1 did not run: %2")
                      .arg(stage.modelId, detail));
  }

  const QString manifestPath = QDir(runDirectory).filePath(QLatin1String(kReferenceManifestName));
  QByteArray manifestBytes;
  if (!readWholeFile(manifestPath, &manifestBytes, reasonKey, reason)) return false;
  QJsonParseError parseError{};
  const QJsonDocument manifest = QJsonDocument::fromJson(manifestBytes, &parseError);
  if (parseError.error != QJsonParseError::NoError || !manifest.isObject()) {
    return refuse(reasonKey, reason, QStringLiteral("reference.manifest"),
                  QStringLiteral("the reference manifest for %1 is not JSON: %2")
                      .arg(stage.modelId, parseError.errorString()));
  }
  const QJsonArray outputs = manifest.object().value(QStringLiteral("outputs")).toArray();
  if (outputs.size() != 1) {
    return refuse(reasonKey, reason, QStringLiteral("reference.manifest"),
                  QStringLiteral("the original graph for %1 returned %2 outputs; this bridge binds "
                                 "exactly one").arg(stage.modelId).arg(outputs.size()));
  }
  const QJsonObject tensorJson = outputs.first().toObject();
  if (tensorJson.value(QStringLiteral("dtype")).toString() !=
      QLatin1String(kEligibleIoType)) {
    return refuse(reasonKey, reason, QStringLiteral("reference.output.dtype"),
                  QStringLiteral("the original graph for %1 returned a %2 output; this bridge "
                                 "binds fp16 only")
                      .arg(stage.modelId, tensorJson.value(QStringLiteral("dtype")).toString()));
  }
  const QJsonArray shape = tensorJson.value(QStringLiteral("shape")).toArray();
  if (shape.size() != 4 || shape.at(0).toInteger() != 1 ||
      shape.at(1).toInteger() != stage.outputChannels ||
      shape.at(2).toInteger() != stage.outputGeometry.height() ||
      shape.at(3).toInteger() != stage.outputGeometry.width()) {
    return refuse(reasonKey, reason, QStringLiteral("reference.output.geometry"),
                  QStringLiteral("the original graph for %1 returned %2, but this stage's output "
                                 "is 1x%3x%4x%5")
                      .arg(stage.modelId, shapeText(shape))
                      .arg(stage.outputChannels)
                      .arg(stage.outputGeometry.height())
                      .arg(stage.outputGeometry.width()));
  }
  const QString outputFile = tensorJson.value(QStringLiteral("file")).toString();
  if (outputFile.isEmpty()) {
    return refuse(reasonKey, reason, QStringLiteral("reference.manifest"),
                  QStringLiteral("the reference manifest for %1 names no output file")
                      .arg(stage.modelId));
  }
  QByteArray bytes;
  if (!readWholeFile(QDir(runDirectory).filePath(outputFile), &bytes, reasonKey, reason)) {
    return false;
  }
  output->size = stage.outputGeometry;
  output->channels = stage.outputChannels;
  output->fp16 = bytes;
  if (!tensorBytesMatch(*output)) {
    return refuse(reasonKey, reason, QStringLiteral("reference.output.bytes"),
                  QStringLiteral("the original graph for %1 returned %2 bytes for a %3-channel "
                                 "%4x%5 output")
                      .arg(stage.modelId)
                      .arg(bytes.size())
                      .arg(stage.outputChannels)
                      .arg(stage.outputGeometry.width())
                      .arg(stage.outputGeometry.height()));
  }
  progress(lineage, index, QStringLiteral("reference"),
           QStringLiteral("the original graph returned the real stage output"));
  return true;
}

}  // namespace

bool buildPlanReferenceLineage(const PlanReferenceRequest& request,
                               QVector<BuilderStageReference>* references, QString* reasonKey,
                               QString* reason) {
  setReason(reasonKey, reason, QString(), QString());
  if (references == nullptr) {
    return refuse(reasonKey, reason, QStringLiteral("reference.output"),
                  QStringLiteral("no references output was supplied"));
  }

  const PreparationPlan& plan = request.plan;
  if (request.models.size() != plan.stages.size()) {
    return refuse(reasonKey, reason, QStringLiteral("reference.models.count"),
                  QStringLiteral("the plan has %1 stage(s) but %2 owned row(s) were supplied")
                      .arg(plan.stages.size())
                      .arg(request.models.size()));
  }
  for (int index = 0; index < plan.stages.size(); ++index) {
    if (request.models.at(index).record.id != plan.stages.at(index).modelId) {
      return refuse(reasonKey, reason, QStringLiteral("reference.models.mismatch"),
                    QStringLiteral("stage %1 names %2 but the owned row supplied for it is %3")
                        .arg(index)
                        .arg(plan.stages.at(index).modelId)
                        .arg(request.models.at(index).record.id));
    }
  }

  QString inspectExecutable;
  if (!isPlainFile(request.tools.inspectExecutable, &inspectExecutable)) {
    return refuse(reasonKey, reason, QStringLiteral("tool.missing"),
                  QStringLiteral("the native_inspect path is not a plain file"));
  }
  QString ortDirectory;
  if (!isPlainDirectory(request.tools.ortRuntimeDir, &ortDirectory) ||
      !isPlainFile(QDir(ortDirectory).filePath(QLatin1String(kOrtModule)))) {
    return refuse(reasonKey, reason, QStringLiteral("tool.runtime.missing"),
                  QStringLiteral("the ORT runtime directory holds no plain onnxruntime.dll"));
  }
  QString scratch;
  if (!isPlainDirectory(request.scratchDirectory, &scratch)) {
    return refuse(reasonKey, reason, QStringLiteral("reference.scratch"),
                  QStringLiteral("'%1' is not an existing directory this worker may write inside")
                      .arg(request.scratchDirectory));
  }

  const ReferenceFrame* frameA = nullptr;
  const ReferenceFrame* frameB = nullptr;
  for (const ReferenceFrame& frame : request.frames) {
    if (frame.sourceIndex == 0) {
      if (frameA != nullptr) {
        return refuse(reasonKey, reason, QStringLiteral("reference.frames.index"),
                      QStringLiteral("more than one captured frame names source index 0"));
      }
      frameA = &frame;
    } else if (frame.sourceIndex == 1) {
      if (frameB != nullptr) {
        return refuse(reasonKey, reason, QStringLiteral("reference.frames.index"),
                      QStringLiteral("more than one captured frame names source index 1"));
      }
      frameB = &frame;
    } else {
      return refuse(reasonKey, reason, QStringLiteral("reference.frames.index"),
                    QStringLiteral("a captured frame names source index %1; only 0 and 1 exist")
                        .arg(frame.sourceIndex));
    }
  }
  if (frameA == nullptr) {
    return refuse(reasonKey, reason, QStringLiteral("reference.frames.missing"),
                  QStringLiteral("no captured frame names source index 0"));
  }

  QVector<StageInputPlan> inputs;
  if (!planStageInputs(plan, request.frames.size(), &inputs, reasonKey, reason)) return false;

  const int stageCount = plan.stages.size();
  int temporalIndex = -1;
  for (int index = 0; index < stageCount; ++index) {
    if (plan.stages.at(index).kind == StageKind::Temporal) {
      temporalIndex = index;
      break;
    }
  }
  std::optional<double> timestep;
  if (temporalIndex >= 0) {
    const Rational factor = plan.stages.at(temporalIndex).factor;
    timestep = rifeFirstTimestep(factor);
    if (!timestep.has_value()) {
      return refuse(reasonKey, reason, QStringLiteral("reference.temporal.timestep"),
                    QStringLiteral("the interpolation stage's factor %1/%2 produces no intermediate "
                                   "frame strictly inside (0, 1)")
                        .arg(factor.num)
                        .arg(factor.den));
    }
    if (frameB == nullptr) {
      return refuse(reasonKey, reason, QStringLiteral("reference.frames.missing"),
                    QStringLiteral("the interpolation stage needs a second captured frame, which "
                                   "was not supplied"));
    }
  }

  FrameFixture displayA;
  if (!applySourceGeometryToFrame(*frameA, request.source, &displayA, reasonKey, reason)) {
    return false;
  }
  StageTensor sourceA;
  sourceA.size = displayA.size;
  sourceA.channels = 3;
  sourceA.fp16 = frameToFp16Tensor(displayA);
  StageTensor sourceB;
  if (temporalIndex >= 0) {
    FrameFixture displayB;
    if (!applySourceGeometryToFrame(*frameB, request.source, &displayB, reasonKey, reason)) {
      return false;
    }
    sourceB.size = displayB.size;
    sourceB.channels = 3;
    sourceB.fp16 = frameToFp16Tensor(displayB);
  }

  if (request.cancellation != nullptr && request.cancellation->isCancelled()) {
    return cancelledResult(reasonKey, reason);
  }

  Lineage lineage;
  lineage.request = &request;
  lineage.inputs = inputs;
  lineage.scratch = scratch;
  lineage.inspectExecutable = inspectExecutable;
  lineage.environment = inspectEnvironment(request.tools);
  lineage.inputNames.resize(stageCount);
  lineage.inputPaths.resize(stageCount);

  const auto resizeTensor = [&](const StageInputPlan& step, StageTensor* tensor) -> bool {
    if (!step.resizeNamed) return true;
    if (tensor->channels != 3 || tensor->size != step.resizeFrom) {
      return refuse(reasonKey, reason, QStringLiteral("reference.resize.chain"),
                    QStringLiteral("a pre-resize from %1x%2 is named, but the tensor entering it "
                                   "is %3x%4 with %5 channel(s)")
                        .arg(step.resizeFrom.width())
                        .arg(step.resizeFrom.height())
                        .arg(tensor->size.width())
                        .arg(tensor->size.height())
                        .arg(tensor->channels));
    }
    QByteArray resized;
    if (!resampleFp16Planar(tensor->fp16, step.resizeFrom, step.resizeTo, tensor->channels,
                            &resized, reasonKey, reason)) {
      return false;
    }
    tensor->fp16 = resized;
    tensor->size = step.resizeTo;
    return true;
  };

  const auto buildRife = [&](int index, const StageTensor& a, const StageTensor& b,
                             StageTensor* out) -> bool {
    if (a.channels != 3 || b.channels != 3 || a.size != inputs.at(index).contentGeometry ||
        b.size != inputs.at(index).contentGeometry) {
      return refuse(reasonKey, reason, QStringLiteral("reference.rife.inputs"),
                    QStringLiteral("the interpolation stage at %1x%2 needs two three-channel "
                                   "frames at that geometry, but received %3x%4 and %5x%6")
                        .arg(inputs.at(index).contentGeometry.width())
                        .arg(inputs.at(index).contentGeometry.height())
                        .arg(a.size.width())
                        .arg(a.size.height())
                        .arg(b.size.width())
                        .arg(b.size.height()));
    }
    out->size = inputs.at(index).tensorGeometry;
    out->channels = 11;
    return packRifeElevenChannel(a.fp16, b.fp16, inputs.at(index).contentGeometry,
                                 inputs.at(index).padL, inputs.at(index).padT,
                                 inputs.at(index).tensorGeometry, *timestep, &out->fp16, reasonKey,
                                 reason);
  };

  const auto runSpatialRange = [&](int begin, int end, StageTensor start, bool asLineage,
                                   const QString& tag, StageTensor* result) -> bool {
    for (int index = begin; index < end; ++index) {
      if (!checkCancelled(lineage, reasonKey, reason)) return false;
      if (!resizeTensor(inputs.at(index), &start)) return false;
      if (start.size != inputs.at(index).contentGeometry ||
          start.channels != inputs.at(index).channels) {
        return refuse(reasonKey, reason, QStringLiteral("reference.stage.input"),
                      QStringLiteral("stage %1 binds a %2-channel %3x%4 tensor, but its real "
                                     "input is a %5-channel %6x%7")
                          .arg(index)
                          .arg(inputs.at(index).channels)
                          .arg(inputs.at(index).contentGeometry.width())
                          .arg(inputs.at(index).contentGeometry.height())
                          .arg(start.channels)
                          .arg(start.size.width())
                          .arg(start.size.height()));
      }
      if (!writeStageInput(lineage, index, start, asLineage, tag, reasonKey, reason)) return false;
      if (index == end - 1 && end >= stageCount) break;  // no downstream stage consumes it
      StageTensor output;
      if (!runStageGraph(lineage, index, start, lineage.inputPaths.at(index), tag, &output,
                         reasonKey, reason)) {
        return false;
      }
      start = output;
    }
    *result = start;
    return true;
  };

  const auto cropTemporalOutput = [&](const StageTensor& output, StageTensor* cropped) -> bool {
    cropped->channels = output.channels;
    cropped->size = inputs.at(temporalIndex).contentGeometry;
    return cropFp16Planar(output.fp16, output.size, inputs.at(temporalIndex).padL,
                          inputs.at(temporalIndex).padT, cropped->size, output.channels,
                          &cropped->fp16, reasonKey, reason);
  };

  if (temporalIndex < 0) {
    StageTensor tail;
    if (!runSpatialRange(0, stageCount, sourceA, true, QString(), &tail)) return false;
  } else if (temporalIndex == 0) {
    // Interpolation runs first: the pair is the two captured frames, after any
    // hoisted pre-resize the plan names for the first model.
    if (!resizeTensor(inputs.at(0), &sourceA)) return false;
    if (!resizeTensor(inputs.at(0), &sourceB)) return false;
    StageTensor rife;
    if (!buildRife(0, sourceA, sourceB, &rife)) return false;
    if (!writeStageInput(lineage, 0, rife, true, QString(), reasonKey, reason)) return false;
    if (stageCount > 1) {
      StageTensor output;
      if (!runStageGraph(lineage, 0, rife, lineage.inputPaths.at(0), QString(), &output, reasonKey,
                         reason)) {
        return false;
      }
      StageTensor cropped;
      if (!cropTemporalOutput(output, &cropped)) return false;
      StageTensor tail;
      if (!runSpatialRange(1, stageCount, cropped, true, QString(), &tail)) return false;
    }
  } else {
    // Interpolation runs after the spatial chain: both frames' real chain outputs
    // are needed, so the upstream graphs run twice and the pair is built from
    // what they actually returned.
    StageTensor chainA;
    if (!runSpatialRange(0, temporalIndex, sourceA, true, QString(), &chainA)) return false;
    if (!checkCancelled(lineage, reasonKey, reason)) return false;
    StageTensor chainB;
    if (!runSpatialRange(0, temporalIndex, sourceB, false, QStringLiteral("-b"), &chainB)) {
      return false;
    }
    StageTensor rife;
    if (!buildRife(temporalIndex, chainA, chainB, &rife)) return false;
    if (!writeStageInput(lineage, temporalIndex, rife, true, QString(), reasonKey, reason)) {
      return false;
    }
    if (temporalIndex + 1 < stageCount) {
      StageTensor output;
      if (!runStageGraph(lineage, temporalIndex, rife, lineage.inputPaths.at(temporalIndex),
                         QString(), &output, reasonKey, reason)) {
        return false;
      }
      StageTensor cropped;
      if (!cropTemporalOutput(output, &cropped)) return false;
      StageTensor tail;
      if (!runSpatialRange(temporalIndex + 1, stageCount, cropped, true, QString(), &tail)) {
        return false;
      }
    }
  }

  if (lineage.references.size() != stageCount) {
    return refuse(reasonKey, reason, QStringLiteral("reference.lineage.count"),
                  QStringLiteral("the lineage produced %1 reference(s) for a %2-stage plan")
                      .arg(lineage.references.size())
                      .arg(stageCount));
  }
  *references = lineage.references;
  setReason(reasonKey, reason, QString(), QString());
  return true;
}

}  // namespace native
}  // namespace mplayer
