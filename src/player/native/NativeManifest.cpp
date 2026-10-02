#include "NativeManifest.hpp"

#include <limits>

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QRegularExpression>

#include "NativeDiagnostics.hpp"
#include "NativePaths.hpp"

namespace mplayer {
namespace native {

namespace {

constexpr const char* kSchema = "mikai.native.manifest/1";

/// The manifest format this build writes. A document carrying a different number
/// was written by code that may lay out the stage or the build block differently,
/// so it is refused rather than reinterpreted.
constexpr int kMikaiFormat = 1;

/// Only this build option's value is genuinely an unordered set. Every other
/// value is handed to the builder as written, because an order inside one of
/// them is part of the request: the first `--optShapes` entry is the minimum
/// shape, and reordering it changes what gets compiled.
const QStringList kSetValuedOptions = {QStringLiteral("tacticSources")};

/// Words a caller writes when it has no observed value. Present in a build
/// request, they would produce a complete-looking build key naming no real
/// machine or no real library, and a cache entry built on that key would be
/// reused on hardware it was never compiled for.
bool isPlaceholderValue(const QString& value) {
  static const QStringList placeholders = {
      QStringLiteral("unknown"), QStringLiteral("unspecified"), QStringLiteral("none"),
      QStringLiteral("null"),    QStringLiteral("n/a"),        QStringLiteral("-"),
      QStringLiteral("?"),       QStringLiteral("0"),          QStringLiteral("0.0"),
      QStringLiteral("0.0.0"),   QStringLiteral("0.0.0.0"),    QStringLiteral("undefined")};
  const QString trimmed = value.trimmed();
  if (trimmed.isEmpty()) return true;
  return placeholders.contains(trimmed, Qt::CaseInsensitive);
}

const QRegularExpression& shapePattern() {
  static const QRegularExpression pattern(
      QStringLiteral("^([1-9][0-9]*)x([1-9][0-9]*)x([1-9][0-9]*)x([1-9][0-9]*)$"));
  return pattern;
}

const QRegularExpression& smPattern() {
  static const QRegularExpression pattern(QStringLiteral("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$"));
  return pattern;
}

/// Options are `--name` or `--name=value`. A name is a bare token: no spaces, no
/// `=`, no quotes. Anything looser would let one option's text masquerade as
/// another, which is how two different build requests collapse into one key.
const QRegularExpression& optionNamePattern() {
  static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*$"));
  return pattern;
}

/// 40 lower-case hex characters: the inference core's source commit, never a
/// marketing version string.
bool isSha1Hex(const QString& text) {
  if (text.size() != 40) return false;
  for (const QChar c : text) {
    if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
          (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) {
      return false;
    }
  }
  return true;
}

bool jsonInteger(const QJsonValue& value, qint64* out) {
  if (!value.isDouble()) return false;
  const double d = value.toDouble();
  const double intMax = 9007199254740992.0;  // 2^53: past here a double is not exact
  if (d != static_cast<double>(static_cast<qint64>(d))) return false;
  if (d < -intMax || d > intMax) return false;
  *out = static_cast<qint64>(d);
  return true;
}

bool jsonIntInRange(const QJsonValue& value, int* out) {
  qint64 wide = 0;
  if (!jsonInteger(value, &wide)) return false;
  if (wide < std::numeric_limits<int>::min() || wide > std::numeric_limits<int>::max()) return false;
  *out = static_cast<int>(wide);
  return true;
}

bool jsonString(const QJsonObject& object, const QString& key, QString* out) {
  const QJsonValue value = object.value(key);
  if (!value.isString()) return false;
  *out = value.toString();
  return true;
}

bool jsonBool(const QJsonObject& object, const QString& key, bool* out) {
  const QJsonValue value = object.value(key);
  if (!value.isBool()) return false;
  *out = value.toBool();
  return true;
}

bool noUnknownKeys(const QJsonObject& object, const QStringList& known, QString* offender) {
  for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
    if (known.contains(it.key())) continue;
    if (offender) *offender = it.key();
    return false;
  }
  return true;
}

void fail(QString* reasonKey, QString* reason, const QString& key, const QString& text) {
  if (reasonKey) *reasonKey = key;
  // Every refusal leaves through here, so redacting here covers a value echoed
  // by a shape check, an unknown member name and a raw parse error alike.
  if (reason) *reason = redactNativeDiagnosticText(text);
}

QStringList manifestKeys() {
  return {QStringLiteral("schema"),    QStringLiteral("buildKey"), QStringLiteral("engineSha256"),
          QStringLiteral("source"),     QStringLiteral("stage"),    QStringLiteral("build"),
          QStringLiteral("device"),     QStringLiteral("engine"),   QStringLiteral("createdAt"),
          QStringLiteral("mikaiFormat")};
}

QStringList sourceKeys() {
  return {QStringLiteral("modelContentId"), QStringLiteral("onnxSha256"),
          QStringLiteral("onnxBytes"),       QStringLiteral("profile")};
}

QStringList stageKeys() {
  return {QStringLiteral("role"),    QStringLiteral("input"),  QStringLiteral("output"),
          QStringLiteral("io"),      QStringLiteral("padL"),   QStringLiteral("padT")};
}

QStringList buildKeys() {
  return {QStringLiteral("provider"),      QStringLiteral("runtime"),
          QStringLiteral("plugin"),        QStringLiteral("coreSha"),
          QStringLiteral("sourceAbi"),     QStringLiteral("precision"),
          QStringLiteral("tf32"),          QStringLiteral("stronglyTyped"),
          QStringLiteral("skipInference"), QStringLiteral("options")};
}

QStringList deviceKeys() {
  return {QStringLiteral("uuid"), QStringLiteral("sm"), QStringLiteral("driver"),
          QStringLiteral("cuda"), QStringLiteral("luid")};
}

QStringList engineKeys() {
  return {QStringLiteral("file"), QStringLiteral("sha256"), QStringLiteral("bytes")};
}

QStringList ioFormatParts(const QString& text) {
  return text.split(QLatin1Char(':'));
}

QStringList tensorProfileKeys() {
  return {QStringLiteral("dtype"), QStringLiteral("rank"), QStringLiteral("n"),
          QStringLiteral("c"),     QStringLiteral("h"),    QStringLiteral("w")};
}

QStringList modelProfileKeys() {
  return {QStringLiteral("input"), QStringLiteral("output")};
}

/// A tensor a graph actually declared. An empty dtype, or a zero rank, batch or
/// channel count, is a default-constructed tensor rather than a declaration:
/// such a profile cannot hash to the content id the catalogue computed for the
/// graph, so a request or manifest carrying one is refused instead of completed
/// with a boundary no graph stated.
bool tensorIsDeclared(const TensorProfile& tensor) {
  return !tensor.dtype.isEmpty() && tensor.rank > 0 && tensor.batch != 0 && tensor.channels > 0;
}

bool profileIsDeclared(const ModelProfile& profile) {
  return tensorIsDeclared(profile.input) && tensorIsDeclared(profile.output);
}

QJsonObject tensorProfileToJson(const TensorProfile& tensor) {
  const auto dimension = [](const std::optional<qint64>& value) {
    return value ? QJsonValue(static_cast<double>(*value)) : QJsonValue();
  };
  QJsonObject object;
  object.insert(QStringLiteral("dtype"), redactNativeDiagnosticText(tensor.dtype));
  object.insert(QStringLiteral("rank"), tensor.rank);
  object.insert(QStringLiteral("n"), static_cast<double>(tensor.batch));
  object.insert(QStringLiteral("c"), static_cast<double>(tensor.channels));
  object.insert(QStringLiteral("h"), dimension(tensor.height));
  object.insert(QStringLiteral("w"), dimension(tensor.width));
  return object;
}

QJsonObject modelProfileToJson(const ModelProfile& profile) {
  QJsonObject object;
  object.insert(QStringLiteral("input"), tensorProfileToJson(profile.input));
  object.insert(QStringLiteral("output"), tensorProfileToJson(profile.output));
  return object;
}

bool tensorProfileFromJson(const QJsonObject& object, TensorProfile* tensor, QString* reason) {
  const auto reject = [reason](const QString& text) {
    if (reason) *reason = text;
    return false;
  };
  QString offender;
  if (!noUnknownKeys(object, tensorProfileKeys(), &offender)) {
    return reject(QStringLiteral("'%1' is not a declared-tensor member").arg(offender));
  }
  if (!jsonString(object, QStringLiteral("dtype"), &tensor->dtype)) {
    return reject(QStringLiteral("a declared tensor needs a dtype"));
  }
  qint64 rank = 0;
  qint64 batch = 0;
  qint64 channels = 0;
  if (!jsonInteger(object.value(QStringLiteral("rank")), &rank) ||
      !jsonInteger(object.value(QStringLiteral("n")), &batch) ||
      !jsonInteger(object.value(QStringLiteral("c")), &channels)) {
    return reject(QStringLiteral("a declared tensor needs integer rank, n and c"));
  }
  if (rank < 0 || rank > std::numeric_limits<int>::max()) {
    return reject(QStringLiteral("rank %1 is not a usable tensor rank").arg(rank));
  }
  tensor->rank = static_cast<int>(rank);
  tensor->batch = batch;
  tensor->channels = channels;
  const auto dimension = [&](const QString& key, std::optional<qint64>* out) {
    const QJsonValue value = object.value(key);
    if (value.isNull()) {
      out->reset();
      return true;
    }
    qint64 measured = 0;
    if (!jsonInteger(value, &measured)) return false;
    *out = measured;
    return true;
  };
  if (!dimension(QStringLiteral("h"), &tensor->height) ||
      !dimension(QStringLiteral("w"), &tensor->width)) {
    return reject(QStringLiteral("a declared tensor's h and w are each an integer or null"));
  }
  return true;
}

bool modelProfileFromJson(const QJsonObject& object, ModelProfile* profile, QString* reason) {
  const auto reject = [reason](const QString& text) {
    if (reason) *reason = text;
    return false;
  };
  QString offender;
  if (!noUnknownKeys(object, modelProfileKeys(), &offender)) {
    return reject(QStringLiteral("'%1' is not a declared-profile member").arg(offender));
  }
  if (!object.value(QStringLiteral("input")).isObject() ||
      !object.value(QStringLiteral("output")).isObject()) {
    return reject(QStringLiteral("a declared profile needs an input and an output tensor"));
  }
  if (!tensorProfileFromJson(object.value(QStringLiteral("input")).toObject(), &profile->input,
                             reason)) {
    return false;
  }
  return tensorProfileFromJson(object.value(QStringLiteral("output")).toObject(), &profile->output,
                               reason);
}

std::optional<QVector<BuildOption>> parseOptionsImpl(const QString& raw, QString* reason) {
  QVector<BuildOption> options;
  QString token;
  bool inQuotes = false;
  bool sawToken = false;

  const auto flush = [&]() -> bool {
    if (token.isEmpty()) return true;  // runs of whitespace separate, they do not name
    if (!token.startsWith(QLatin1String("--"))) {
      if (reason) {
        *reason = QStringLiteral("'%1' is not a build option of the form --name or --name=value")
                      .arg(token);
      }
      return false;
    }
    const QString body = token.mid(2);
    const qsizetype equals = body.indexOf(QLatin1Char('='));
    BuildOption option;
    option.name = equals < 0 ? body : body.left(equals);
    option.value = equals < 0 ? QString() : body.mid(equals + 1);
    option.hasValue = equals >= 0;
    if (!optionNamePattern().match(option.name).hasMatch()) {
      if (reason) {
        *reason = QStringLiteral("build option name '%1' is not a bare token").arg(option.name);
      }
      return false;
    }
    options.append(option);
    token.clear();
    return true;
  };

  for (const QChar c : raw) {
    if (c == QLatin1Char('"')) {
      inQuotes = !inQuotes;
      sawToken = true;
      continue;
    }
    if (!inQuotes && (c == QLatin1Char(' ') || c == QLatin1Char('\t'))) {
      if (!flush()) return std::nullopt;
      continue;
    }
    token.append(c);
    sawToken = true;
  }
  if (inQuotes) {
    if (reason) *reason = QStringLiteral("build options end inside an unterminated quote");
    return std::nullopt;
  }
  if (sawToken && !flush()) return std::nullopt;
  return options;
}

}  // namespace

// ------------------------------------------------------------------ precision

QString stableName(EnginePrecision precision) {
  switch (precision) {
    case EnginePrecision::Fp16: return QStringLiteral("fp16");
    case EnginePrecision::Fp32: return QStringLiteral("fp32");
  }
  return {};
}

std::optional<EnginePrecision> enginePrecisionFromName(const QString& name) {
  if (name == QLatin1String("fp16")) return EnginePrecision::Fp16;
  if (name == QLatin1String("fp32")) return EnginePrecision::Fp32;
  return std::nullopt;
}

// -------------------------------------------------------------- build options

std::optional<QVector<BuildOption>> parseBuildOptions(const QString& raw, QString* reason) {
  return parseOptionsImpl(raw, reason);
}

QString canonicalizeBuildOptions(const QVector<BuildOption>& options) {
  // The options are rendered in the order and with the spelling they were given:
  // the string is handed to a real builder whose option semantics are its own.
  // Only a genuinely unordered set is normalized, and a value holding spaces is
  // re-quoted so the canonical string parses back to the same options.
  QStringList rendered;
  rendered.reserve(options.size());
  for (const BuildOption& option : options) {
    if (!option.hasValue) {
      rendered.append(QStringLiteral("--%1").arg(option.name));
      continue;
    }
    QString value = option.value;
    if (kSetValuedOptions.contains(option.name)) {
      QStringList members = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
      members.sort();
      members.removeDuplicates();
      value = members.join(QLatin1Char(','));
    }
    if (value.contains(QLatin1Char(' ')) || value.contains(QLatin1Char('\t'))) {
      rendered.append(QStringLiteral("--%1=\"%2\"").arg(option.name, value));
    } else {
      rendered.append(QStringLiteral("--%1=%2").arg(option.name, value));
    }
  }
  return rendered.join(QLatin1Char(' '));
}

QString canonicalBuildOptionString(const QString& raw, QString* reason) {
  const auto parsed = parseBuildOptions(raw, reason);
  if (!parsed) return {};
  return canonicalizeBuildOptions(*parsed);
}

// ------------------------------------------------------------ graph identity

bool GraphContentIdentity::isComplete() const {
  // The declared boundary is half the identity: without it the content id names
  // a graph whose boundary is unknown, which is not the identity the catalogue
  // computed for any row.
  return isSha256Hex(graphSha256) && profileIsDeclared(declaredProfile);
}

QString GraphContentIdentity::payload() const {
  return contentIdentityPayload(role, graphSha256, declaredProfile);
}

QString GraphContentIdentity::contentId() const {
  return computeContentId(role, graphSha256, declaredProfile);
}

bool GraphContentIdentity::operator==(const GraphContentIdentity& other) const {
  // Compared through the canonical payload, which covers the declared boundary
  // member by member, so two profiles that describe the same tensors cannot
  // differ by a field the payload happens to omit.
  return payload() == other.payload();
}

GraphContentIdentity graphIdentityOf(const ModelRecord& record) {
  GraphContentIdentity identity;
  identity.role = record.role;
  identity.graphSha256 = record.onnxSha256.toLower();
  identity.declaredProfile = record.actualProfile;
  return identity;
}

// ----------------------------------------------------------- build identity

QString EngineBuildIdentity::smText() const {
  return QStringLiteral("%1.%2").arg(smMajor).arg(smMinor);
}

QString EngineBuildIdentity::firstSensitiveField() const {
  // Ordered so the reported field is stable for the same request.
  if (containsSensitiveNativeText(runtimeVersion)) return QStringLiteral("runtimeVersion");
  if (containsSensitiveNativeText(pluginVersion)) return QStringLiteral("pluginVersion");
  if (containsSensitiveNativeText(deviceUuid)) return QStringLiteral("deviceUuid");
  if (containsSensitiveNativeText(cudaVersion)) return QStringLiteral("cudaVersion");
  if (containsSensitiveNativeText(driverVersion)) return QStringLiteral("driverVersion");
  if (containsSensitiveNativeText(adapterLuid)) return QStringLiteral("adapterLuid");
  // Build options are handed to a real builder and recorded verbatim, so a
  // value that is really a signed URL or a token is refused here too - before
  // any comparison can echo it - rather than stored as if it were a compiler
  // flag. Both the parsed values and the canonical rendering are checked, so a
  // request is refused whether or not it has been canonicalized yet.
  for (const BuildOption& option : options) {
    if (containsSensitiveNativeText(option.name) || containsSensitiveNativeText(option.value)) {
      return QStringLiteral("options");
    }
  }
  if (containsSensitiveNativeText(canonicalOptions())) return QStringLiteral("options");
  return {};
}

QString EngineBuildIdentity::firstMissingRequiredField() const {
  // Ordered so the reported field is stable for the same incomplete request. A
  // credential-looking value is named ahead of the ordinary shape checks: it is
  // not a runtime, plugin or device at all, and must never be recorded.
  if (const QString sensitive = firstSensitiveField(); !sensitive.isEmpty()) return sensitive;
  if (isPlaceholderValue(runtimeVersion)) return QStringLiteral("runtimeVersion");
  if (isPlaceholderValue(pluginVersion)) return QStringLiteral("pluginVersion");
  if (!isSha1Hex(coreSha)) return QStringLiteral("coreSha");
  if (sourceAbi <= 0) return QStringLiteral("sourceAbi");
  if (isPlaceholderValue(deviceUuid)) return QStringLiteral("deviceUuid");
  if (smMajor <= 0) return QStringLiteral("smMajor");
  if (smMinor < 0) return QStringLiteral("smMinor");
  if (isPlaceholderValue(cudaVersion)) return QStringLiteral("cudaVersion");
  if (isPlaceholderValue(driverVersion)) return QStringLiteral("driverVersion");
  if (provider == Backend::DirectML && isPlaceholderValue(adapterLuid)) {
    return QStringLiteral("adapterLuid");
  }
  return {};
}

QString EngineBuildIdentity::payload() const {
  QStringList parts;
  parts << QStringLiteral("provider=%1").arg(stableName(provider));
  parts << QStringLiteral("runtime=%1").arg(runtimeVersion);
  parts << QStringLiteral("plugin=%1").arg(pluginVersion);
  parts << QStringLiteral("coreSha=%1").arg(coreSha.toLower());
  parts << QStringLiteral("sourceAbi=%1").arg(sourceAbi);
  parts << QStringLiteral("precision=%1").arg(stableName(precision));
  parts << QStringLiteral("tf32=%1").arg(tf32 ? QStringLiteral("1") : QStringLiteral("0"));
  parts << QStringLiteral("stronglyTyped=%1")
               .arg(stronglyTyped ? QStringLiteral("1") : QStringLiteral("0"));
  parts << QStringLiteral("skipInference=%1")
               .arg(skipInference ? QStringLiteral("1") : QStringLiteral("0"));
  parts << QStringLiteral("device.uuid=%1").arg(deviceUuid);
  // Present for TensorRT too, as an explicit empty: the key must say that no
  // adapter LUID applies, not merely omit the fact.
  parts << QStringLiteral("device.luid=%1").arg(adapterLuid);
  parts << QStringLiteral("device.sm=%1").arg(smText());
  parts << QStringLiteral("cuda=%1").arg(cudaVersion);
  parts << QStringLiteral("driver=%1").arg(driverVersion);
  parts << QStringLiteral("options=%1").arg(canonicalOptions());
  return parts.join(QLatin1Char('\n'));
}

bool EngineBuildIdentity::operator==(const EngineBuildIdentity& other) const {
  return payload() == other.payload();
}

// ------------------------------------------------------------ stage request

bool StageBuildRequest::isComplete() const {
  return isStageShapeText(inputShape) && isStageShapeText(outputShape) &&
         isIoFormatText(ioFormat) && padLeft >= 0 && padTop >= 0;
}

QString StageBuildRequest::payload() const {
  return QStringLiteral("role=%1\ninput=%2\noutput=%3\nio=%4\npadL=%5\npadT=%6")
      .arg(stableName(role), inputShape, outputShape, ioFormat)
      .arg(padLeft)
      .arg(padTop);
}

// ------------------------------------------------------------ build request

QString BuildRequest::firstMissingRequiredField() const {
  if (!graph.isComplete()) return QStringLiteral("graph.contentIdentity");
  if (!stage.isComplete()) return QStringLiteral("stage.request");
  const QString missingBuildField = build.firstMissingRequiredField();
  if (!missingBuildField.isEmpty()) {
    return QStringLiteral("build.%1").arg(missingBuildField);
  }
  return {};
}

QString BuildRequest::payload() const {
  // The graph half is its whole content identity - role, digest and the declared
  // boundary - rendered by the one function that also computes the content id,
  // so a build key and a `modelContentId` can never disagree about what the
  // graph declares. The padded stage shapes are what the builder compiles. Every
  // input is recorded in the manifest, which is what keeps this key
  // recomputable there.
  return QStringLiteral("graph\n%1\nstage\n%2\nbuild\n%3")
      .arg(contentIdentityPayload(graph.role, graph.graphSha256.toLower(),
                                  graph.declaredProfile),
           stage.payload(), build.payload());
}

QString BuildRequest::buildKey() const {
  const QByteArray digest = QCryptographicHash::hash(payload().toUtf8(),
                                                    QCryptographicHash::Sha256);
  return QStringLiteral("sha256:") + QString::fromLatin1(digest.toHex());
}

// ------------------------------------------------------------------ manifest

bool ManifestStage::operator==(const ManifestStage& other) const {
  return role == other.role && inputShape == other.inputShape &&
         outputShape == other.outputShape && ioFormat == other.ioFormat &&
         padLeft == other.padLeft && padTop == other.padTop;
}

GraphContentIdentity ArtifactManifest::graphIdentity() const {
  // Role, digest and the declared boundary are all recorded, so the identity
  // rebuilds whole and its `contentId()` must equal `source.modelContentId`.
  // That equality is what keeps the recorded build key and the recorded content
  // id from describing two different graphs.
  GraphContentIdentity identity;
  identity.role = stage.role;
  identity.graphSha256 = source.onnxSha256;
  identity.declaredProfile = source.declaredProfile;
  return identity;
}

EngineBuildIdentity ArtifactManifest::buildIdentity() const {
  EngineBuildIdentity identity;
  identity.provider = build.provider;
  identity.runtimeVersion = build.runtime;
  identity.pluginVersion = build.plugin;
  identity.coreSha = build.coreSha;
  identity.sourceAbi = build.sourceAbi;
  identity.precision = build.precision;
  identity.tf32 = build.tf32;
  identity.stronglyTyped = build.stronglyTyped;
  identity.skipInference = build.skipInference;
  identity.deviceUuid = device.uuid;
  identity.adapterLuid = device.luid;
  const QStringList sm = device.sm.split(QLatin1Char('.'));
  if (sm.size() == 2) {
    identity.smMajor = sm.at(0).toInt();
    identity.smMinor = sm.at(1).toInt();
  }
  identity.cudaVersion = device.cuda;
  identity.driverVersion = device.driver;
  // The options were verified canonical, so this parse round-trips.
  const auto parsed = parseBuildOptions(build.canonicalOptions, nullptr);
  if (parsed) identity.options = *parsed;
  return identity;
}

StageBuildRequest ArtifactManifest::stageRequest() const {
  StageBuildRequest request;
  request.role = stage.role;
  request.inputShape = stage.inputShape;
  request.outputShape = stage.outputShape;
  request.ioFormat = stage.ioFormat;
  request.padLeft = stage.padLeft;
  request.padTop = stage.padTop;
  return request;
}

BuildRequest ArtifactManifest::request() const {
  BuildRequest request;
  request.graph = graphIdentity();
  request.stage = stageRequest();
  request.build = buildIdentity();
  return request;
}

bool ArtifactManifest::isSelfConsistent(QString* reasonKey, QString* reason) const {
  const auto reject = [&](const QString& key, const QString& text) {
    fail(reasonKey, reason, key, text);
    return false;
  };

  // A build-identity field that looks like a URL or a credential is not a
  // runtime, plugin or device at all. It is refused by name before any later
  // comparison can quote it back.
  if (const QString sensitive = buildIdentity().firstSensitiveField(); !sensitive.isEmpty()) {
    return reject(QStringLiteral("artifact.runtime.sensitiveData"),
                  QStringLiteral("the manifest records %1 as a URL or a credential-looking "
                                 "value, which is never a real build identity")
                      .arg(sensitive));
  }

  if (schema != artifactManifestSchema()) {
    return reject(QStringLiteral("manifest.schema"),
                  QStringLiteral("manifest schema is '%1', not '%2'")
                      .arg(schema, artifactManifestSchema()));
  }
  if (mikaiFormat != kMikaiFormat) {
    return reject(QStringLiteral("manifest.mikaiFormat"),
                  QStringLiteral("mikaiFormat is %1, this build writes %2")
                      .arg(mikaiFormat)
                      .arg(kMikaiFormat));
  }
  if (!isSha256Hex(engineSha256)) {
    return reject(QStringLiteral("manifest.engineSha256"),
                  QStringLiteral("engineSha256 is not a 64-character sha256"));
  }
  if (engineSha256 != engine.sha256) {
    return reject(QStringLiteral("manifest.engineShaMismatch"),
                  QStringLiteral("engineSha256 (%1) and engine.sha256 (%2) name different bytes")
                      .arg(engineSha256, engine.sha256));
  }
  if (!isSha256Hex(engine.sha256)) {
    return reject(QStringLiteral("manifest.engine.sha256"),
                  QStringLiteral("engine.sha256 is not a 64-character sha256"));
  }
  if (engine.bytes <= 0) {
    return reject(QStringLiteral("manifest.engine.bytes"),
                  QStringLiteral("engine.bytes is %1; a prepared engine is never empty")
                      .arg(engine.bytes));
  }
  if (!buildKey.startsWith(QLatin1String("sha256:")) || !isSha256Hex(buildKey.mid(7))) {
    return reject(QStringLiteral("manifest.buildKey"),
                  QStringLiteral("buildKey is not sha256:<hex>"));
  }
  if (!isSha256Hex(source.onnxSha256)) {
    return reject(QStringLiteral("manifest.source.onnxSha256"),
                  QStringLiteral("the manifest records no 64-character ONNX sha256"));
  }
  if (source.onnxBytes <= 0) {
    return reject(QStringLiteral("manifest.source.onnxBytes"),
                  QStringLiteral("the manifest records an empty graph file"));
  }
  if (!source.modelContentId.startsWith(QLatin1String("sha256:")) ||
      !isSha256Hex(source.modelContentId.mid(7))) {
    return reject(QStringLiteral("manifest.source.modelContentId"),
                  QStringLiteral("modelContentId is not sha256:<hex>"));
  }
  if (!profileIsDeclared(source.declaredProfile)) {
    return reject(QStringLiteral("manifest.source.profile"),
                  QStringLiteral("the manifest records no populated declared boundary, so the "
                                 "graph's content identity cannot be recomputed or checked"));
  }
  // The recorded content id must be the content id of the graph the manifest
  // records: the same role, digest and declared boundary. A manifest whose
  // content id was copied from elsewhere, or whose boundary was edited after the
  // id was computed, is refused here rather than served under a key it does not
  // describe.
  if (source.modelContentId != graphIdentity().contentId()) {
    return reject(QStringLiteral("manifest.source.modelContentId"),
                  QStringLiteral("modelContentId does not identify the recorded graph: role, "
                                 "digest and the declared boundary hash to a different content "
                                 "id"));
  }
  if (!isStageShapeText(stage.inputShape) || !isStageShapeText(stage.outputShape)) {
    return reject(QStringLiteral("manifest.stage.shape"),
                  QStringLiteral("a stage shape is not four positive NCHW dimensions"));
  }
  if (!isIoFormatText(stage.ioFormat)) {
    return reject(QStringLiteral("manifest.stage.io"),
                  QStringLiteral("stage io is not <eligible dtype>:<known layout>"));
  }
  if (stage.padLeft < 0 || stage.padTop < 0) {
    return reject(QStringLiteral("manifest.stage.pad"),
                  QStringLiteral("a padding of %1/%2 is not a position").arg(stage.padLeft).arg(stage.padTop));
  }
  // The engine name is one file beside its manifest. A nested name would be
  // removed separately from its manifest and leave a directory behind that
  // blocks re-publication; the engine/manifest name collision is refused where
  // the artifact is published, where both names are written into one directory.
  {
    QString nameReason;
    const QString safe = safeRelativePath(engine.file, &nameReason);
    if (safe.isEmpty()) {
      return reject(QStringLiteral("manifest.enginePath"),
                    QStringLiteral("engine file name '%1' is not a safe relative path: %2")
                        .arg(engine.file, nameReason));
    }
    if (safe.contains(QLatin1Char('/'))) {
      return reject(QStringLiteral("manifest.enginePath"),
                    QStringLiteral("engine file name '%1' names a nested path; an artifact's "
                                   "engine is one file beside its manifest")
                        .arg(engine.file));
    }
  }
  if (!isSmText(device.sm)) {
    return reject(QStringLiteral("manifest.device.sm"),
                  QStringLiteral("device.sm is '%1', not major.minor").arg(device.sm));
  }
  const QString missing = buildIdentity().firstMissingRequiredField();
  if (!missing.isEmpty()) {
    return reject(QStringLiteral("manifest.build.missing"),
                  QStringLiteral("the manifest records no %1 for this build, so the request it "
                                 "describes was never fully observed")
                      .arg(missing));
  }
  // The stored option string must already be canonical. Normalizing a
  // hand-edited one here would let a document claim a build key it does not
  // correspond to.
  if (canonicalBuildOptionString(build.canonicalOptions, nullptr) != build.canonicalOptions) {
    return reject(QStringLiteral("manifest.build.options"),
                  QStringLiteral("the recorded build options are not in canonical form, so the "
                                 "recorded buildKey cannot be the key of this request"));
  }
  // The recorded key must be the key of the recorded request. Everything the key
  // hashes is checked above, so a key that was copied in from elsewhere, or a
  // field edited after the key was computed, is caught here.
  if (buildKey != request().buildKey()) {
    return reject(QStringLiteral("manifest.buildKey"),
                  QStringLiteral("the recorded build key is not the key of the request the rest of "
                                 "the manifest describes"));
  }
  if (createdAt.isEmpty()) {
    return reject(QStringLiteral("manifest.createdAt"),
                  QStringLiteral("the manifest records no creation time"));
  }
  if (reasonKey) reasonKey->clear();
  if (reason) reason->clear();
  return true;
}

QString artifactManifestSchema() { return QString::fromLatin1(kSchema); }

QJsonObject artifactManifestToJson(const ArtifactManifest& manifest) {
  // A caller can hand this writer a directly constructed manifest that was never
  // validated. Every string member is therefore redacted on the way out, so a
  // credential planted in any field is never emitted in the clear; an ordinary
  // hash, role, shape or file name is returned unchanged, so a real document is
  // still byte-for-byte what was recorded.
  const auto text = [](const QString& value) { return redactNativeDiagnosticText(value); };

  QJsonObject source;
  source.insert(QStringLiteral("modelContentId"), text(manifest.source.modelContentId));
  source.insert(QStringLiteral("onnxSha256"), text(manifest.source.onnxSha256));
  source.insert(QStringLiteral("onnxBytes"), static_cast<double>(manifest.source.onnxBytes));
  source.insert(QStringLiteral("profile"), modelProfileToJson(manifest.source.declaredProfile));

  QJsonObject stage;
  stage.insert(QStringLiteral("role"), stableName(manifest.stage.role));
  stage.insert(QStringLiteral("input"), text(manifest.stage.inputShape));
  stage.insert(QStringLiteral("output"), text(manifest.stage.outputShape));
  stage.insert(QStringLiteral("io"), text(manifest.stage.ioFormat));
  stage.insert(QStringLiteral("padL"), manifest.stage.padLeft);
  stage.insert(QStringLiteral("padT"), manifest.stage.padTop);

  QJsonObject build;
  build.insert(QStringLiteral("provider"), stableName(manifest.build.provider));
  build.insert(QStringLiteral("runtime"), text(manifest.build.runtime));
  build.insert(QStringLiteral("plugin"), text(manifest.build.plugin));
  build.insert(QStringLiteral("coreSha"), text(manifest.build.coreSha));
  build.insert(QStringLiteral("sourceAbi"), manifest.build.sourceAbi);
  build.insert(QStringLiteral("precision"), stableName(manifest.build.precision));
  build.insert(QStringLiteral("tf32"), manifest.build.tf32);
  build.insert(QStringLiteral("stronglyTyped"), manifest.build.stronglyTyped);
  build.insert(QStringLiteral("skipInference"), manifest.build.skipInference);
  build.insert(QStringLiteral("options"), text(manifest.build.canonicalOptions));

  QJsonObject device;
  device.insert(QStringLiteral("uuid"), text(manifest.device.uuid));
  device.insert(QStringLiteral("sm"), text(manifest.device.sm));
  device.insert(QStringLiteral("driver"), text(manifest.device.driver));
  device.insert(QStringLiteral("cuda"), text(manifest.device.cuda));
  device.insert(QStringLiteral("luid"), text(manifest.device.luid));

  QJsonObject engine;
  engine.insert(QStringLiteral("file"), text(manifest.engine.file));
  engine.insert(QStringLiteral("sha256"), text(manifest.engine.sha256));
  engine.insert(QStringLiteral("bytes"), static_cast<double>(manifest.engine.bytes));

  QJsonObject document;
  document.insert(QStringLiteral("schema"), artifactManifestSchema());
  document.insert(QStringLiteral("buildKey"), text(manifest.buildKey));
  document.insert(QStringLiteral("engineSha256"), text(manifest.engineSha256));
  document.insert(QStringLiteral("source"), source);
  document.insert(QStringLiteral("stage"), stage);
  document.insert(QStringLiteral("build"), build);
  document.insert(QStringLiteral("device"), device);
  document.insert(QStringLiteral("engine"), engine);
  document.insert(QStringLiteral("createdAt"), text(manifest.createdAt));
  document.insert(QStringLiteral("mikaiFormat"), manifest.mikaiFormat);
  return document;
}

bool parseArtifactManifest(const QJsonObject& object, ArtifactManifest* manifest,
                           QString* reasonKey, QString* reason) {
  if (!manifest) return false;

  const auto reject = [&](const QString& key, const QString& text) {
    fail(reasonKey, reason, key, text);
    return false;
  };

  QString offender;
  if (!noUnknownKeys(object, manifestKeys(), &offender)) {
    return reject(QStringLiteral("manifest.unknownMember"),
                  QStringLiteral("'%1' is not a member of %2").arg(offender, artifactManifestSchema()));
  }

  ArtifactManifest parsed;
  if (!jsonString(object, QStringLiteral("schema"), &parsed.schema) ||
      !jsonString(object, QStringLiteral("buildKey"), &parsed.buildKey) ||
      !jsonString(object, QStringLiteral("engineSha256"), &parsed.engineSha256) ||
      !jsonString(object, QStringLiteral("createdAt"), &parsed.createdAt)) {
    return reject(QStringLiteral("manifest.member"),
                  QStringLiteral("schema, buildKey, engineSha256 and createdAt are all required"));
  }
  if (!jsonIntInRange(object.value(QStringLiteral("mikaiFormat")), &parsed.mikaiFormat)) {
    return reject(QStringLiteral("manifest.mikaiFormat"), QStringLiteral("mikaiFormat is required"));
  }
  if (parsed.schema != artifactManifestSchema()) {
    return reject(QStringLiteral("manifest.schema"),
                  QStringLiteral("manifest schema is '%1'; this build writes and reads only '%2'")
                      .arg(parsed.schema, artifactManifestSchema()));
  }
  if (parsed.mikaiFormat != kMikaiFormat) {
    return reject(QStringLiteral("manifest.mikaiFormat"),
                  QStringLiteral("mikaiFormat is %1; this build understands %2")
                      .arg(parsed.mikaiFormat)
                      .arg(kMikaiFormat));
  }

  for (const QString& block : {QStringLiteral("source"), QStringLiteral("stage"),
                               QStringLiteral("build"), QStringLiteral("device"),
                               QStringLiteral("engine")}) {
    if (!object.value(block).isObject()) {
      return reject(QStringLiteral("manifest.member"),
                    QStringLiteral("'%1' must be an object").arg(block));
    }
  }

  const QJsonObject source = object.value(QStringLiteral("source")).toObject();
  if (!noUnknownKeys(source, sourceKeys(), &offender)) {
    return reject(QStringLiteral("manifest.source.unknownMember"),
                  QStringLiteral("'%1' is not a source member").arg(offender));
  }
  if (!jsonString(source, QStringLiteral("modelContentId"), &parsed.source.modelContentId) ||
      !jsonString(source, QStringLiteral("onnxSha256"), &parsed.source.onnxSha256)) {
    return reject(QStringLiteral("manifest.source.member"),
                  QStringLiteral("modelContentId and onnxSha256 are required"));
  }
  if (!jsonInteger(source.value(QStringLiteral("onnxBytes")), &parsed.source.onnxBytes)) {
    return reject(QStringLiteral("manifest.source.onnxBytes"),
                  QStringLiteral("onnxBytes is required and must be an integer"));
  }
  const QJsonValue profileValue = source.value(QStringLiteral("profile"));
  if (!profileValue.isObject()) {
    return reject(QStringLiteral("manifest.source.member"),
                  QStringLiteral("the declared boundary 'profile' must be an object"));
  }
  QString profileReason;
  if (!modelProfileFromJson(profileValue.toObject(), &parsed.source.declaredProfile,
                            &profileReason)) {
    return reject(QStringLiteral("manifest.source.member"),
                  QStringLiteral("the declared boundary is not usable: %1").arg(profileReason));
  }

  const QJsonObject stage = object.value(QStringLiteral("stage")).toObject();
  if (!noUnknownKeys(stage, stageKeys(), &offender)) {
    return reject(QStringLiteral("manifest.stage.unknownMember"),
                  QStringLiteral("'%1' is not a stage member").arg(offender));
  }
  QString roleName;
  if (!jsonString(stage, QStringLiteral("role"), &roleName)) {
    return reject(QStringLiteral("manifest.stage.role"), QStringLiteral("role is required"));
  }
  const auto role = modelRoleFromName(roleName);
  if (!role) {
    return reject(QStringLiteral("manifest.stage.role"),
                  QStringLiteral("'%1' is not a known model role").arg(roleName));
  }
  parsed.stage.role = *role;
  if (!jsonString(stage, QStringLiteral("input"), &parsed.stage.inputShape) ||
      !jsonString(stage, QStringLiteral("output"), &parsed.stage.outputShape) ||
      !jsonString(stage, QStringLiteral("io"), &parsed.stage.ioFormat)) {
    return reject(QStringLiteral("manifest.stage.member"),
                  QStringLiteral("input, output and io are required"));
  }
  if (!jsonIntInRange(stage.value(QStringLiteral("padL")), &parsed.stage.padLeft) ||
      !jsonIntInRange(stage.value(QStringLiteral("padT")), &parsed.stage.padTop)) {
    return reject(QStringLiteral("manifest.stage.pad"),
                  QStringLiteral("padL and padT are required and must be integers"));
  }

  const QJsonObject build = object.value(QStringLiteral("build")).toObject();
  if (!noUnknownKeys(build, buildKeys(), &offender)) {
    return reject(QStringLiteral("manifest.build.unknownMember"),
                  QStringLiteral("'%1' is not a build member").arg(offender));
  }
  QString providerName;
  if (!jsonString(build, QStringLiteral("provider"), &providerName)) {
    return reject(QStringLiteral("manifest.build.provider"), QStringLiteral("provider is required"));
  }
  const auto provider = backendFromName(providerName);
  if (!provider) {
    return reject(QStringLiteral("manifest.build.provider"),
                  QStringLiteral("'%1' is not a known provider").arg(providerName));
  }
  parsed.build.provider = *provider;
  QString precisionName;
  if (!jsonString(build, QStringLiteral("runtime"), &parsed.build.runtime) ||
      !jsonString(build, QStringLiteral("plugin"), &parsed.build.plugin) ||
      !jsonString(build, QStringLiteral("coreSha"), &parsed.build.coreSha) ||
      !jsonString(build, QStringLiteral("precision"), &precisionName) ||
      !jsonString(build, QStringLiteral("options"), &parsed.build.canonicalOptions)) {
    return reject(QStringLiteral("manifest.build.member"),
                  QStringLiteral("runtime, plugin, coreSha, precision and options are required"));
  }
  const auto precision = enginePrecisionFromName(precisionName);
  if (!precision) {
    return reject(QStringLiteral("manifest.build.precision"),
                  QStringLiteral("'%1' is not a known build precision").arg(precisionName));
  }
  parsed.build.precision = *precision;
  if (!jsonIntInRange(build.value(QStringLiteral("sourceAbi")), &parsed.build.sourceAbi) ||
      !jsonBool(build, QStringLiteral("tf32"), &parsed.build.tf32) ||
      !jsonBool(build, QStringLiteral("stronglyTyped"), &parsed.build.stronglyTyped) ||
      !jsonBool(build, QStringLiteral("skipInference"), &parsed.build.skipInference)) {
    return reject(QStringLiteral("manifest.build.member"),
                  QStringLiteral("sourceAbi must be an integer and tf32, stronglyTyped and "
                                 "skipInference must be booleans"));
  }

  const QJsonObject device = object.value(QStringLiteral("device")).toObject();
  if (!noUnknownKeys(device, deviceKeys(), &offender)) {
    return reject(QStringLiteral("manifest.device.unknownMember"),
                  QStringLiteral("'%1' is not a device member").arg(offender));
  }
  if (!jsonString(device, QStringLiteral("uuid"), &parsed.device.uuid) ||
      !jsonString(device, QStringLiteral("sm"), &parsed.device.sm) ||
      !jsonString(device, QStringLiteral("driver"), &parsed.device.driver) ||
      !jsonString(device, QStringLiteral("cuda"), &parsed.device.cuda) ||
      !jsonString(device, QStringLiteral("luid"), &parsed.device.luid)) {
    return reject(QStringLiteral("manifest.device.member"),
                  QStringLiteral("uuid, sm, driver, cuda and luid are required"));
  }

  const QJsonObject engine = object.value(QStringLiteral("engine")).toObject();
  if (!noUnknownKeys(engine, engineKeys(), &offender)) {
    return reject(QStringLiteral("manifest.engine.unknownMember"),
                  QStringLiteral("'%1' is not an engine member").arg(offender));
  }
  if (!jsonString(engine, QStringLiteral("file"), &parsed.engine.file) ||
      !jsonString(engine, QStringLiteral("sha256"), &parsed.engine.sha256)) {
    return reject(QStringLiteral("manifest.engine.member"),
                  QStringLiteral("file and sha256 are required"));
  }
  if (!jsonInteger(engine.value(QStringLiteral("bytes")), &parsed.engine.bytes)) {
    return reject(QStringLiteral("manifest.engine.bytes"),
                  QStringLiteral("engine.bytes is required and must be an integer"));
  }

  if (!parsed.isSelfConsistent(reasonKey, reason)) return false;

  *manifest = parsed;
  if (reasonKey) reasonKey->clear();
  if (reason) reason->clear();
  return true;
}

bool parseArtifactManifest(const QByteArray& payload, ArtifactManifest* manifest,
                           QString* reasonKey, QString* reason) {
  QJsonParseError parseError{};
  const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    fail(reasonKey, reason, QStringLiteral("manifest.json"),
         QStringLiteral("manifest is not a JSON object: %1").arg(parseError.errorString()));
    return false;
  }
  return parseArtifactManifest(document.object(), manifest, reasonKey, reason);
}

QByteArray serializeArtifactManifest(const ArtifactManifest& manifest) {
  return QJsonDocument(artifactManifestToJson(manifest)).toJson(QJsonDocument::Indented);
}

// ---------------------------------------------------------------- shape rules

bool isStageShapeText(const QString& text) {
  const QRegularExpressionMatch match = shapePattern().match(text);
  if (!match.hasMatch()) return false;
  // "1x3x1080x1920" exactly: four components, so a match that consumed the
  // whole string is the only accept.
  return match.capturedLength() == text.size();
}

bool isIoFormatText(const QString& text) {
  const QStringList parts = ioFormatParts(text);
  if (parts.size() != 2) return false;
  // The dtype must be one this bridge can actually bind. An unknown one is
  // refused, never converted and never rounded to fp16.
  if (!isEligibleIoType(parts.at(0))) return false;
  return parts.at(1) == QLatin1String("chw") || parts.at(1) == QLatin1String("nhwc");
}

bool isSmText(const QString& text) {
  const QRegularExpressionMatch match = smPattern().match(text);
  if (!match.hasMatch()) return false;
  return match.capturedLength() == text.size();
}

}  // namespace native
}  // namespace mplayer
