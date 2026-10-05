#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QRect>
#include <QSize>
#include <QStringList>
#include <QThread>

#include <cstdio>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <vector>

#include "MpvApi.hpp"
#include "MpvHandle.hpp"
#include "native/NativeBundle.hpp"
#include "native/NativeFilterCommand.hpp"
#include "native/NativeRuntimeSelection.hpp"
#include "native/NativeStatus.hpp"

using namespace mplayer;
using namespace mplayer::native;

namespace {

int passed = 0;
int failed = 0;

// The two fixtures the GPU smoke hands this probe: the primary clip is exactly
// 24/1 and its 23.976 twin exactly 24000/1001. A 1/1000 rational grid would
// publish 2997/125 for the twin, so the exact values are the point of the check.
constexpr Rational kPrimaryCadence{24, 1};
constexpr Rational kTwinCadence{24000, 1001};

bool check(bool condition, const char* behavior) {
    std::fprintf(stderr, "STATUS %s %s\n", condition ? "ok" : "FAIL", behavior);
    condition ? ++passed : ++failed;
    return condition;
}

void pump(MpvHandle& handle) {
    handle.drainEvents();
    QCoreApplication::processEvents();
    QThread::msleep(5);
}

AsyncReply command(MpvHandle& handle, std::vector<std::string> arguments) {
    std::optional<AsyncReply> reply;
    const AsyncRequestId request = handle.commandAsync(
        std::move(arguments), [&reply](const AsyncReply& value) { reply = value; });
    if (!request.valid()) throw std::runtime_error("core refused the asynchronous command");
    QElapsedTimer clock;
    clock.start();
    while (!reply.has_value() && clock.elapsed() < 10000) pump(handle);
    if (!reply.has_value()) {
        handle.cancelAsync(request);
        throw std::runtime_error("core command did not finish within ten seconds");
    }
    return *reply;
}

QString stringProperty(MpvHandle& handle, const char* name) {
    std::optional<AsyncReply> reply;
    const AsyncRequestId request = handle.getPropertyAsync(
        name, MPV_FORMAT_STRING, [&reply](const AsyncReply& value) { reply = value; });
    if (!request.valid()) throw std::runtime_error("core refused the property request");
    QElapsedTimer clock;
    clock.start();
    while (!reply.has_value() && clock.elapsed() < 10000) pump(handle);
    if (!reply.has_value()) {
        handle.cancelAsync(request);
        throw std::runtime_error("core property did not finish within ten seconds");
    }
    return reply->ok() ? QString::fromStdString(reply->text) : QString();
}

// A core integer sub-property as an observed number, or nothing when the core
// cannot answer for it. `video-dec-params/w` and `/h` are the decoder's own
// frame dimensions, so they are the independent source for the geometry the
// filter publishes; nothing here substitutes a default when they are absent.
std::optional<int> intProperty(MpvHandle& handle, const char* name) {
    const QString text = stringProperty(handle, name);
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok) return std::nullopt;
    return value;
}

// The `vf` list read back as the core's own node: one map per entry, with the
// entry's `name`, `label`, `enabled` and its decoded `params`. This is how a
// spec is verified - by what the core parsed, never by re-reading the string
// this process encoded.
AsyncReply nodeProperty(MpvHandle& handle, const char* name) {
    std::optional<AsyncReply> reply;
    const AsyncRequestId request = handle.getPropertyAsync(
        name, MPV_FORMAT_NODE, [&reply](const AsyncReply& value) { reply = value; });
    if (!request.valid()) throw std::runtime_error("core refused the node property request");
    QElapsedTimer clock;
    clock.start();
    while (!reply.has_value() && clock.elapsed() < 10000) pump(handle);
    if (!reply.has_value()) {
        handle.cancelAsync(request);
        throw std::runtime_error("core node property did not finish within ten seconds");
    }
    return *reply;
}

const NodeValue* mapEntry(const NodeValue& node, const char* key) {
    if (node.format != MPV_FORMAT_NODE_MAP) return nullptr;
    for (const std::pair<std::string, NodeValue>& entry : node.map) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

// The core's parse of one labelled filter, or nothing when the list has no such
// entry. `params` holds the decoded option values exactly as the core read them
// out of the spec.
struct ParsedFilterEntry {
    QString name;
    bool enabled = false;
    QMap<QString, QString> params;
};

std::optional<ParsedFilterEntry> parsedFilterEntry(MpvHandle& handle, const char* label) {
    const AsyncReply reply = nodeProperty(handle, "vf");
    if (!reply.ok() || reply.node.format != MPV_FORMAT_NODE_ARRAY) return std::nullopt;
    for (const NodeValue& entry : reply.node.list) {
        const NodeValue* entryLabel = mapEntry(entry, "label");
        if (entryLabel == nullptr || entryLabel->format != MPV_FORMAT_STRING ||
            entryLabel->text != label) {
            continue;
        }
        ParsedFilterEntry parsed;
        const NodeValue* name = mapEntry(entry, "name");
        if (name != nullptr && name->format == MPV_FORMAT_STRING) {
            parsed.name = QString::fromStdString(name->text);
        }
        const NodeValue* enabled = mapEntry(entry, "enabled");
        parsed.enabled = enabled != nullptr && enabled->format == MPV_FORMAT_FLAG && enabled->flag;
        const NodeValue* params = mapEntry(entry, "params");
        if (params != nullptr && params->format == MPV_FORMAT_NODE_MAP) {
            for (const std::pair<std::string, NodeValue>& param : params->map) {
                if (param.second.format == MPV_FORMAT_STRING) {
                    parsed.params.insert(QString::fromStdString(param.first),
                                         QString::fromStdString(param.second.text));
                }
            }
        }
        return parsed;
    }
    return std::nullopt;
}

NativeStatusView readStatus(MpvHandle& handle, bool proveBound = false) {
    NativeStatusConsumer consumer;
    consumer.attach(&handle);
    const quint64 epoch = handle.sourceEpoch();
    consumer.tick(0, epoch, true);
    QElapsedTimer clock;
    clock.start();
    while (consumer.availability().reasonKey == QStringLiteral("native.status.unread") &&
           clock.elapsed() < 8000) {
        pump(handle);
        consumer.tick(clock.elapsed(), epoch, true);
    }
    if (proveBound) {
        const quint64 requests = consumer.requestCount();
        consumer.tick(100, epoch, true);
        consumer.tick(10000, epoch, false);
        check(consumer.requestCount() == requests,
              "an in-window tick and a disabled poll cannot stack metadata requests");
    }
    const NativeStatusView result = consumer.status();
    consumer.detach();
    return result;
}

NativeStatusView waitProbe(MpvHandle& handle) {
    QElapsedTimer clock;
    clock.start();
    NativeStatusView latest;
    while (clock.elapsed() < 15000) {
        latest = readStatus(handle);
        if (latest.snapshot.has_value() && latest.snapshot->probe.has_value() &&
            latest.snapshot->probe->source.has_value() &&
            latest.snapshot->probe->source->codedSize.has_value() &&
            latest.snapshot->probe->source->fps.has_value() &&
            latest.snapshot->probe->source->rotationDegrees.has_value() &&
            latest.snapshot->probe->device.has_value() &&
            !latest.snapshot->probe->device->decoderDeviceUuid.isEmpty()) {
            return latest;
        }
        pump(handle);
        QThread::msleep(45);
    }
    return latest;
}

std::optional<StatusSnapshot> waitEpochChange(MpvHandle& handle, qint64 oldEpoch) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 15000) {
        const NativeStatusView view = readStatus(handle);
        if (view.snapshot.has_value() && view.snapshot->epoch.has_value() &&
            *view.snapshot->epoch != oldEpoch) {
            return view.snapshot;
        }
        pump(handle);
        QThread::msleep(45);
    }
    return std::nullopt;
}

// A reload publishes several epochs before it is done: the reset bumps one
// while the previous source's facts are still current. Only a snapshot whose
// observed cadence is the new source's is the reload actually finishing, so the
// wait keys on the expected rate rather than on the epoch alone.
std::optional<StatusSnapshot> waitObservedSource(MpvHandle& handle, qint64 oldEpoch,
                                                 const Rational& expected) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 20000) {
        const NativeStatusView view = readStatus(handle);
        const std::optional<StatusSnapshot>& snapshot = view.snapshot;
        if (snapshot.has_value() && snapshot->epoch.has_value() &&
            *snapshot->epoch != oldEpoch && snapshot->probe.has_value() &&
            snapshot->probe->source.has_value() &&
            snapshot->probe->source->codedSize.has_value() &&
            snapshot->probe->source->fps.has_value() &&
            *snapshot->probe->source->fps == expected &&
            snapshot->probe->device.has_value() &&
            !snapshot->probe->device->decoderDeviceUuid.isEmpty()) {
            return snapshot;
        }
        pump(handle);
        QThread::msleep(45);
    }
    return std::nullopt;
}

// ---- managed prepared admission -------------------------------------------
//
// The fixture carries the plan (stages, digests, paths, requested selection);
// the source and device facts it is resolved against are filled from what this
// run actually observed, and the filter re-validates every one of them against
// the device the frames live on. A member the fixture states itself is left
// exactly as given, so a deliberately wrong fixture stays wrong.

QJsonObject observedSource(const SourceGeometry& source) {
  QJsonObject object;
  if (source.codedSize.has_value()) {
    object.insert(QStringLiteral("codedWidth"), source.codedSize->width());
    object.insert(QStringLiteral("codedHeight"), source.codedSize->height());
  }
  if (source.crop.has_value()) {
    QJsonObject crop;
    crop.insert(QStringLiteral("x"), source.crop->x());
    crop.insert(QStringLiteral("y"), source.crop->y());
    crop.insert(QStringLiteral("width"), source.crop->width());
    crop.insert(QStringLiteral("height"), source.crop->height());
    object.insert(QStringLiteral("crop"), crop);
  }
  if (source.rotationDegrees.has_value())
    object.insert(QStringLiteral("rotation"), *source.rotationDegrees);
  if (source.fps.has_value()) {
    QJsonObject fps;
    fps.insert(QStringLiteral("num"), source.fps->num);
    fps.insert(QStringLiteral("den"), source.fps->den);
    object.insert(QStringLiteral("fps"), fps);
  }
  if (!source.csp.isEmpty()) object.insert(QStringLiteral("csp"), source.csp);
  if (!source.matrix.isEmpty()) object.insert(QStringLiteral("matrix"), source.matrix);
  if (!source.range.isEmpty()) object.insert(QStringLiteral("range"), source.range);
  if (!source.siting.isEmpty()) object.insert(QStringLiteral("siting"), source.siting);
  if (source.bitDepth != 0) object.insert(QStringLiteral("bitDepth"), source.bitDepth);
  return object;
}

QJsonObject observedDevice(const DeviceIdentity& device) {
  QJsonObject object;
  if (!device.decoderDeviceUuid.isEmpty())
    object.insert(QStringLiteral("decoderDeviceUuid"), device.decoderDeviceUuid);
  if (!device.computeDeviceUuid.isEmpty())
    object.insert(QStringLiteral("computeDeviceUuid"), device.computeDeviceUuid);
  if (!device.adapterLuid.isEmpty())
    object.insert(QStringLiteral("adapterLuid"), device.adapterLuid);
  object.insert(QStringLiteral("smMajor"), device.smMajor);
  object.insert(QStringLiteral("smMinor"), device.smMinor);
  return object;
}

std::optional<QJsonObject> loadAdmissionDocument(const QString& path,
                                                 const StatusSnapshot& observed,
                                                 QString* reason) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    *reason = QStringLiteral("the admission fixture cannot be read");
    return std::nullopt;
  }
  const QByteArray bytes = file.readAll();
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    *reason = QStringLiteral("the admission fixture is not one JSON object: %1")
                  .arg(error.errorString());
    return std::nullopt;
  }
  QJsonObject object = document.object();
  if (!observed.probe.has_value()) {
    *reason = QStringLiteral("no observed probe payload resolves the plan's source and device");
    return std::nullopt;
  }
  if (!object.contains(QStringLiteral("source")) && observed.probe->source.has_value())
    object.insert(QStringLiteral("source"), observedSource(*observed.probe->source));
  if (!object.contains(QStringLiteral("device")) && observed.probe->device.has_value())
    object.insert(QStringLiteral("device"), observedDevice(*observed.probe->device));
  return object;
}

using SnapshotPredicate = std::function<bool(const StatusSnapshot&)>;

std::optional<StatusSnapshot> waitForSnapshot(MpvHandle& handle, const SnapshotPredicate& predicate,
                                              qint64 timeoutMs) {
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < timeoutMs) {
    const NativeStatusView view = readStatus(handle);
    if (view.snapshot.has_value() && predicate(*view.snapshot)) return view.snapshot;
    pump(handle);
    QThread::msleep(45);
  }
  return std::nullopt;
}

bool isUnavailableProbe(const NativeStatusView& view) {
    return view.snapshot.has_value() && view.snapshot->state == NativeState::Probe &&
           view.availability.value == Availability::Unavailable &&
           !view.snapshot->reason.isEmpty() && !view.snapshot->effective.has_value() &&
           !view.snapshot->counters.has_value();
}

// The factory reports the adopted device in the ABI's hex form; comparing it to
// the probe's own UUID means comparing the same digits, not the same spelling.
QString compactUuid(const QString& value) {
    QString compact = value;
    compact.remove(QLatin1Char('-'));
    return compact.toLower();
}

// The shim the filter loads is the bundle's own producer dispatcher, never an
// arbitrary DLL path: the named file must be the declared producer library
// inside the selected native bundle and must hash to the digest that bundle's
// inventory records. Empty return means refused, with `reason` naming why.
QString resolveRifeLibrary(const QString& named, const std::optional<Bundle>& selected,
                           QString* reason) {
    const auto refuse = [reason](const QString& text) -> QString {
        if (reason != nullptr) *reason = text;
        return {};
    };
    if (reason != nullptr) reason->clear();
    if (!selected.has_value() || selected->kind != BundleKind::Native) {
        return refuse(QStringLiteral("no native bundle is selected to own the named shim"));
    }
    const QString canonical = QFileInfo(named).canonicalFilePath();
    if (canonical.isEmpty()) {
        return refuse(QStringLiteral("the named shim %1 is not an existing file").arg(named));
    }
    const QString producer = bundlePreparationProducerLibrary();
    const BundleFile* declared = nullptr;
    for (const BundleFile& file : selected->files) {
        if (QFileInfo(file.path).fileName().compare(producer, Qt::CaseInsensitive) != 0) continue;
        const QString absolute =
            QFileInfo(QDir::cleanPath(QDir(selected->root).filePath(file.path))).canonicalFilePath();
        if (absolute.isEmpty()) continue;
        if (QString::compare(absolute, canonical, Qt::CaseInsensitive) == 0) {
            declared = &file;
            break;
        }
    }
    if (declared == nullptr) {
        return refuse(QStringLiteral("the named shim %1 is not the bundle's declared %2")
                          .arg(canonical, producer));
    }
    QFile file(canonical);
    if (!file.open(QIODevice::ReadOnly)) {
        return refuse(QStringLiteral("the named shim %1 cannot be read").arg(canonical));
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return refuse(QStringLiteral("the named shim %1 failed while hashing").arg(canonical));
    }
    const QString digest = QString::fromLatin1(hash.result().toHex());
    if (digest.compare(declared->sha256, Qt::CaseInsensitive) != 0) {
        return refuse(QStringLiteral("the named shim %1 does not match the bundle inventory digest")
                          .arg(canonical));
    }
    return canonical;
}

int finish() {
    std::fprintf(stderr, "STATUS %d passed, %d failed\nRESULT: %s\n", passed, failed,
                 failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    QString module = qEnvironmentVariable("MIKAI_MPV_MODULE");
    if (arguments.size() > 1) module = arguments.at(1);
    if (arguments.size() > 5) {
        std::fprintf(stderr, "Usage: native_status_probe [module.dll] [media] [second-media] "
                             "[prepared-admission.json]\n");
        return 2;
    }

    try {
        std::optional<Bundle> selectedBundle;
        if (!module.isEmpty()) {
            // An explicitly named module is a bundle, never a bare DLL: the
            // manifest beside it is read and validated, and it must declare the
            // very file that was named, so the loader gets that bundle's own
            // descriptor id, protocol and dependency closure rather than a
            // search path built around whatever directory the DLL sat in.
            const QFileInfo moduleInfo(module);
            QString reason;
            const std::optional<Bundle> bundle = readBundle(moduleInfo.absolutePath(), &reason);
            if (!bundle.has_value()) throw std::runtime_error(reason.toStdString());
            const QString givenModule = moduleInfo.canonicalFilePath();
            const QString declaredModule = QFileInfo(bundle->modulePath).canonicalFilePath();
            if (givenModule.isEmpty() || declaredModule.isEmpty() ||
                QString::compare(givenModule, declaredModule, Qt::CaseInsensitive) != 0) {
                throw std::runtime_error(
                    QStringLiteral("the bundle at %1 declares module '%2', not '%3'")
                        .arg(bundle->root, bundle->modulePath, moduleInfo.absoluteFilePath())
                        .toStdString());
            }
            if (!validateBundle(*bundle, &reason)) throw std::runtime_error(reason.toStdString());
            const MpvBundle descriptor = bundle->toMpvBundleDescriptor();
            if (!MpvApi::instance().load(descriptor, &reason)) {
                throw std::runtime_error(reason.toStdString());
            }
            std::fprintf(stderr, "bundle deps: %lld\n",
                         static_cast<long long>(descriptor.deps.size()));
            selectedBundle = bundle;
        } else {
            const RuntimeSelection selection = selectStandardRuntime();
            if (!selection.loaded) throw std::runtime_error(selection.reason.toStdString());
            selectedBundle = selection.resolvedBundle;
        }

        MpvHandle handle({{"vo", arguments.size() >= 3 ? "gpu-next" : "null"},
                          {"gpu-api", "opengl"}, {"idle", "yes"}, {"audio", "no"},
                          {"hwdec", "nvdec"}, {"hwdec-codecs", "all"}, {"pause", "yes"},
                          {"input-default-bindings", "no"}});
        const NativeStatusView absent = readStatus(handle, true);
        check(!absent.snapshot.has_value() &&
                  absent.availability.value == Availability::Unavailable &&
                  absent.availability.reasonKey == QStringLiteral("native.status.missing"),
              "an unattached filter publishes named Unavailable, not a stale or ready plan");

        // The producer's refusals need no core: an option name that is not a
        // plain mpv name and a value carrying U+0000 are named, never truncated
        // at the C-string boundary the core takes the spec through.
        QString refusalKey;
        QString refusalReason;
        const QByteArray badNameSpec = nativeFilterSpec(
            {{QStringLiteral("not a name"), QStringLiteral("x")}}, &refusalKey, &refusalReason);
        check(badNameSpec.isEmpty() && refusalKey == QStringLiteral("native.filter.optionName"),
              "an option name that is not a plain mpv name is refused by name");
        const QByteArray nulSpec = nativeFilterSpec(
            {{QStringLiteral("model-dir"), QString::fromUtf8(QByteArray("a\0b", 3))}},
            &refusalKey, &refusalReason);
        check(nulSpec.isEmpty() && refusalKey == QStringLiteral("native.filter.value.embeddedNul"),
              "a value with an embedded NUL is refused by name, never truncated");
        if (arguments.size() < 3) return finish();

        const QString media = QFileInfo(arguments.at(2)).absoluteFilePath();
        if (!QFileInfo(media).isFile()) throw std::runtime_error("media fixture does not exist");

        // The real consumer path: the one producer builds the spec, the core
        // parses it, and the parse is read back below. These values carry a
        // drive colon, a comma, a percent, an `=` and Ukrainian characters, plus
        // an empty value; `stats` also carries an escape-shaped `%5%` run, which
        // must stay literal inside the counted bytes. Every one of them must
        // survive the round trip.
        const QVector<NativeFilterOption> smokeOptions{
            {QStringLiteral("model-dir"), QStringLiteral("C:\\шлях до моделей, 50%")},
            {QStringLiteral("stats"), QStringLiteral("Київ=тест%5%,2")},
            {QStringLiteral("rife-model-dir"), QString()},
        };
        const QByteArray smokeSpec = nativeFilterSpec(smokeOptions, &refusalKey, &refusalReason);
        if (!check(!smokeSpec.isEmpty() && refusalKey.isEmpty(),
                   "the producer builds a spec for probe-only options")) {
            std::fprintf(stderr, "refusal: %s %s\n", qPrintable(refusalKey),
                         qPrintable(refusalReason));
            return finish();
        }
        if (!command(handle, {"vf", "add", smokeSpec.toStdString()}).ok()) {
            throw std::runtime_error("the selected module refused its named native filter");
        }
        const std::optional<ParsedFilterEntry> parsed =
            parsedFilterEntry(handle, kNativeStatusLabel);
        check(parsed.has_value(),
              "the core's vf list is readable as a node with the labelled native entry");
        if (parsed.has_value()) {
            check(parsed->name == QLatin1String(kNativeFilterName) && parsed->enabled,
                  "the parsed entry is the enabled animejanai filter");
            const bool exact = parsed->params.size() == smokeOptions.size() &&
                               parsed->params.value(QStringLiteral("model-dir")) ==
                                   smokeOptions.at(0).value &&
                               parsed->params.value(QStringLiteral("stats")) ==
                                   smokeOptions.at(1).value &&
                               parsed->params.contains(QStringLiteral("rife-model-dir")) &&
                               parsed->params.value(QStringLiteral("rife-model-dir")).isEmpty();
            if (!exact) {
                for (auto option = parsed->params.constBegin();
                     option != parsed->params.constEnd(); ++option) {
                    std::fprintf(stderr, "parsed option %s: %s\n", qPrintable(option.key()),
                                 qPrintable(option.value()));
                }
            }
            check(exact,
                  "the core's decoded options are the exact values, drive colon, comma, percent, "
                  "`=` and Ukrainian characters included, and the empty value stays present");
        }
        handle.loadFile(media.toStdString());
        const NativeStatusView first = waitProbe(handle);
        if (!check(isUnavailableProbe(first),
                   "a no-engine hardware passthrough reports Probe without effective work")) {
            std::fprintf(stderr, "refusal: %s %s\n", qPrintable(first.availability.reasonKey),
                         qPrintable(first.availability.reason));
            return finish();
        }
        const StatusSnapshot initial = *first.snapshot;
        if (!check(initial.probe.has_value() && initial.probe->source.has_value() &&
                       initial.probe->device.has_value(),
                   "the filter supplies typed source and actual decoder device facts")) {
            std::fprintf(stderr, "snapshot: %s\n", serializeStatusSnapshot(initial).constData());
            for (const char* property : {"hwdec-current", "video-format", "video-params/w",
                                         "video-params/h", "core-idle", "pause"}) {
                std::fprintf(stderr, "%s: %s\n", property,
                             qPrintable(stringProperty(handle, property)));
            }
            return finish();
        }
        const SourceGeometry& source = *initial.probe->source;
        const std::optional<int> decodedWidth = intProperty(handle, "video-dec-params/w");
        const std::optional<int> decodedHeight = intProperty(handle, "video-dec-params/h");
        const bool decodedSizeKnown = decodedWidth.has_value() && decodedHeight.has_value() &&
                                      *decodedWidth > 0 && *decodedHeight > 0;
        check(decodedSizeKnown &&
                  source.codedSize == QSize(*decodedWidth, *decodedHeight),
              "the observed coded size agrees with the core's decoded frame, not a default");
        check(decodedSizeKnown && source.crop.has_value() &&
                  *source.crop == QRect(0, 0, *decodedWidth, *decodedHeight),
              "the decoded frame's own full-extent crop is observed, not synthesised");
        check(!initial.probe->device->decoderDeviceUuid.isEmpty(),
              "the decoder device UUID is observed, not a synthesized default");
        check(source.rotationDegrees.has_value() && *source.rotationDegrees == 0,
              "an unrotated source reports an observed zero rotation, not an absence");
        check(source.fps.has_value() && *source.fps == kPrimaryCadence,
              "the primary fixture publishes its exact 24/1 cadence");
        const QString coreMatrix = stringProperty(handle, "video-params/colormatrix");
        const QString coreLevels = stringProperty(handle, "video-params/colorlevels");
        const QString coreSiting = stringProperty(handle, "video-params/chroma-location");
        const QString coreSubfmt = stringProperty(handle, "video-params/hw-pixelformat");
        check(!coreMatrix.isEmpty() && source.matrix == coreMatrix && !coreLevels.isEmpty() &&
                  source.range == coreLevels && !coreSiting.isEmpty() &&
                  source.siting == coreSiting && !coreSubfmt.isEmpty() &&
                  source.csp == coreSubfmt,
              "the published colour facts are the core's own observed tags, not placeholders");
        const QString decoder = stringProperty(handle, "hwdec-current");
        const QString frameFormat = stringProperty(handle, "video-dec-params/pixelformat");
        std::fprintf(stderr, "decoder: %s; decoded frame format: %s\n",
                     qPrintable(decoder), qPrintable(frameFormat));
        check(!decoder.isEmpty() && decoder != QStringLiteral("no") &&
                  frameFormat == QStringLiteral("cuda"),
              "hardware decoding delivers CUDA frames to the observed device");
        check(command(handle, {"set", "pause", "yes"}).ok(),
              "the decoded source can be frozen before a stable reread");
        const NativeStatusView reread = readStatus(handle);
        check(reread.snapshot.has_value() && *reread.snapshot == initial,
              "unchanged frozen source facts keep the same revision, epoch and snapshot");
        const QByteArray serialized = serializeStatusSnapshot(initial);
        std::fprintf(stdout, "%s\n", serialized.constData());
        std::fflush(stdout);

        if (!check(initial.epoch.has_value(), "an observed source has a lifecycle epoch")) {
            return finish();
        }
        check(command(handle, {"seek", "1.0", "absolute+exact"}).ok(),
              "the real decoded source accepts a seek discontinuity");
        const auto sought = waitEpochChange(handle, *initial.epoch);
        check(sought.has_value() && sought->state == NativeState::Probe &&
                  !sought->effective.has_value() && !sought->counters.has_value(),
              "seek replaces the probe epoch without claiming an active engine");

        const QString secondMedia = arguments.size() > 3
                                        ? QFileInfo(arguments.at(3)).absoluteFilePath() : media;
        if (!QFileInfo(secondMedia).isFile()) {
            throw std::runtime_error("second media fixture does not exist");
        }
        const bool twin = arguments.size() > 3;
        const Rational secondCadence = twin ? kTwinCadence : kPrimaryCadence;
        const qint64 previousEpoch = sought.has_value() ? *sought->epoch : *initial.epoch;
        handle.loadFile(secondMedia.toStdString());
        const auto reloaded = waitObservedSource(handle, previousEpoch, secondCadence);
        if (!check(reloaded.has_value(),
                   "reloading waits for the new source's observed facts, not an empty epoch")) {
            return finish();
        }
        const SourceGeometry& reloadedSource = *reloaded->probe->source;
        check(reloaded->state == NativeState::Probe && !reloaded->effective.has_value() &&
                  !reloaded->counters.has_value(),
              "the reloaded source is Probe passthrough with no native effective work");
        const std::optional<int> reloadedWidth = intProperty(handle, "video-dec-params/w");
        const std::optional<int> reloadedHeight = intProperty(handle, "video-dec-params/h");
        const bool reloadedSizeKnown = reloadedWidth.has_value() && reloadedHeight.has_value() &&
                                       *reloadedWidth > 0 && *reloadedHeight > 0;
        check(reloadedSizeKnown &&
                  reloadedSource.codedSize == QSize(*reloadedWidth, *reloadedHeight) &&
                  reloadedSource.rotationDegrees.has_value() &&
                  *reloadedSource.rotationDegrees == 0,
              "the reloaded source keeps the core's observed unrotated geometry");
        check(reloadedSource.fps.has_value() && *reloadedSource.fps == secondCadence,
              twin ? "the 23.976 twin publishes the exact 24000/1001 cadence"
                   : "reloading the same source keeps its exact cadence");
        const QString reloadedDecoder = stringProperty(handle, "hwdec-current");
        const QString reloadedFormat = stringProperty(handle, "video-dec-params/pixelformat");
        check(!reloadedDecoder.isEmpty() && reloadedDecoder != QStringLiteral("no") &&
                  reloadedFormat == QStringLiteral("cuda"),
              "the reloaded source is real NVDEC CUDA frames, not a software upload");

        const qint64 reloadedEpoch = *reloaded->epoch;
        check(command(handle, {"vf", "remove", std::string("@") + kNativeStatusLabel}).ok(),
              "the filter is removable from the existing core");
        const NativeStatusView removed = readStatus(handle);
        check(!removed.snapshot.has_value() &&
                  removed.availability.value == Availability::Unavailable &&
                  removed.availability.reasonKey == QStringLiteral("native.status.missing"),
              "filter removal clears metadata instead of retaining a stale probe");
        check(command(handle, {"vf", "add", smokeSpec.toStdString()}).ok(),
              "the filter can be re-added to the same core");
        const auto recreated = waitObservedSource(handle, reloadedEpoch, secondCadence);
        if (!check(recreated.has_value(),
                   "re-adding the filter waits for a fresh recreation epoch with real facts")) {
            return finish();
        }
        const std::optional<int> recreatedWidth = intProperty(handle, "video-dec-params/w");
        const std::optional<int> recreatedHeight = intProperty(handle, "video-dec-params/h");
        const bool recreatedSizeKnown = recreatedWidth.has_value() &&
                                        recreatedHeight.has_value() && *recreatedWidth > 0 &&
                                        *recreatedHeight > 0;
        check(recreated->state == NativeState::Probe && recreatedSizeKnown &&
                  recreated->probe->source->codedSize == QSize(*recreatedWidth, *recreatedHeight) &&
                  recreated->probe->source->csp == QStringLiteral("nv12") &&
                  recreated->probe->device.has_value() &&
                  !recreated->probe->device->decoderDeviceUuid.isEmpty(),
              "the recreated filter re-observes the real CUDA source and decoder device");

        // The shim the filter loads is the selected bundle's own producer
        // library; it is validated against that bundle before any attach, and
        // the optional factory proof and the prepared admission both run
        // against it.
        const QString admissionPath =
            arguments.size() > 4 ? QFileInfo(arguments.at(4)).absoluteFilePath() : QString();
        const QString namedShim = qEnvironmentVariable("MIKAI_AJI_LIB");
        QString shim;
        if (!namedShim.isEmpty()) {
            QString shimReason;
            shim = resolveRifeLibrary(namedShim, selectedBundle, &shimReason);
            if (!check(!shim.isEmpty(),
                       "the named shim is the selected bundle's own declared producer library")) {
                std::fprintf(stderr, "shim: %s\n", qPrintable(shimReason));
                handle.stop();
                return finish();
            }
        } else if (!admissionPath.isEmpty()) {
            check(false, "an admission run names the shim it admits against (MIKAI_AJI_LIB)");
            handle.stop();
            return finish();
        }

        NativeStatusView bare;
        if (!shim.isEmpty()) {
            check(command(handle, {"vf", "remove", std::string("@") + kNativeStatusLabel}).ok(),
                  "the probe-only filter detaches before the shim-only re-attach");
            const QByteArray shimSpec =
                nativeFilterSpec({{QStringLiteral("lib"), shim}}, &refusalKey, &refusalReason);
            if (!check(!shimSpec.isEmpty() && refusalKey.isEmpty(),
                       "the same producer builds the shim-only filter spec")) {
                handle.stop();
                return finish();
            }
            check(command(handle, {"vf", "add", shimSpec.toStdString()}).ok(),
                  "the shim-only filter attaches with no conf and no engine");
            bare = waitProbe(handle);
            check(bare.snapshot.has_value() && bare.snapshot->state != NativeState::Active &&
                      !bare.snapshot->effective.has_value() && !bare.snapshot->counters.has_value(),
                  "a shim-only attach publishes no plan and is never Active before an admission");

            // The factory observation is the producer's own answer for the
            // source this core actually decodes; the proof is strict about it
            // and never reads a structural fact as availability.
            const std::optional<StatusSnapshot> factoryView = waitForSnapshot(
                handle,
                [](const StatusSnapshot& snapshot) {
                    return snapshot.factory.has_value() && snapshot.probe.has_value() &&
                           snapshot.probe->device.has_value();
                },
                20000);
            if (!check(factoryView.has_value(),
                       "the shim-only attach publishes its optional factory observation")) {
                std::fprintf(stderr, "factory: no observation within the bounded wait\n");
            } else {
                const NativeFactoryObservation& factory = *factoryView->factory;
                const DeviceIdentity& device = *factoryView->probe->device;
                if (!check(factory.isUsable(),
                           "the factory observation is an executed, refusal-free answer")) {
                    std::fprintf(stderr, "factory: %s %s\n", qPrintable(factory.reasonKey),
                                 qPrintable(factory.reason));
                }
                check(factory.backend == QStringLiteral("tensorrt"),
                      "the observed factory names TensorRT as the backend it checked");
                check(factory.spatial && factory.rife,
                      "the observed factory answered for both compiled roles");
                std::fprintf(stderr, "factory runtime: %s\n", qPrintable(factory.runtimeVersion));
                check(!factory.runtimeVersion.isEmpty() && !factory.deviceIdentity.isEmpty() &&
                          compactUuid(factory.deviceIdentity) ==
                              compactUuid(device.computeDeviceUuid) &&
                          factory.smMajor == device.smMajor && factory.smMinor == device.smMinor,
                      "the factory's measured device and capability are the probe's compute device");
                if (!check(factoryView->state != NativeState::Active &&
                               factoryView->state != NativeState::Preparing &&
                               factoryView->state != NativeState::NeedsPreparation &&
                               !factoryView->effective.has_value() &&
                               !factoryView->counters.has_value(),
                           "the factory proof claims no prepared or active plan, no effective work and no counters")) {
                    std::fprintf(stderr, "factory snapshot: %s\n",
                                 serializeStatusSnapshot(*factoryView).constData());
                }
            }
        }

        // The managed prepared admission scenario runs only when a fixture is
        // supplied; it uses the shim-only attach above, so the admitted plan is
        // the only chain and nothing is built or loaded before it.
        if (!admissionPath.isEmpty()) {
            QString admissionReason;
            const std::optional<QJsonObject> admission =
                loadAdmissionDocument(admissionPath, *recreated, &admissionReason);
            if (!check(admission.has_value(),
                       "the admission fixture resolves against the observed source and device")) {
                std::fprintf(stderr, "admission: %s\n", qPrintable(admissionReason));
                handle.stop();
                return finish();
            }
            const QByteArray admitJson = QJsonDocument(*admission).toJson(QJsonDocument::Compact);
            const qint64 planEpoch =
                static_cast<qint64>(admission->value(QStringLiteral("epoch")).toDouble());
            const qint64 planRevision =
                static_cast<qint64>(admission->value(QStringLiteral("revision")).toDouble());
            const QString planRecipe = admission->value(QStringLiteral("requested"))
                                           .toObject()
                                           .value(QStringLiteral("recipe"))
                                           .toString();
            const qint64 beforeAdmit = bare.snapshot.has_value() ? bare.snapshot->revision : 0;

            check(command(handle, {"vf-command", "mikai_native", "admit", admitJson.toStdString()}).ok(),
                  "the named vf-command transport carries the admission document");
            const auto admitted = waitForSnapshot(
                handle,
                [&](const StatusSnapshot& snapshot) {
                    return snapshot.revision > beforeAdmit && snapshot.state == NativeState::Preparing;
                },
                30000);
            check(admitted.has_value() && !admitted->effective.has_value() &&
                      !admitted->counters.has_value(),
                  "an accepted admission is Preparing with no effective work until a real output");
            check(admitted.has_value() && admitted->requested.has_value() &&
                      (planRecipe.isEmpty() || admitted->requested->recipeId == planRecipe),
                  "the snapshot's requested selection is the caller's, not the reader's");

            check(command(handle, {"set", "pause", "no"}).ok(),
                  "the decoded source is unfrozen so real output can complete");
            const auto active = waitForSnapshot(
                handle,
                [](const StatusSnapshot& snapshot) { return snapshot.state == NativeState::Active; },
                120000);
            check(command(handle, {"set", "pause", "yes"}).ok(),
                  "the decoded source is frozen after the admitted output");
            check(active.has_value() && active->reason.isEmpty() && active->effective.has_value() &&
                      active->counters.has_value() && active->counters->framesOut > 0,
                  "Active appears only after frames completed under the admitted plan, with effective and counters");
            if (active.has_value() && active->effective.has_value()) {
                const NativeEffectivePlan& effective = *active->effective;
                const QSize observedCoded = *recreated->probe->source->codedSize;
                check(effective.outputSize.isValid() && !effective.model.isEmpty() &&
                          !effective.provider.isEmpty() &&
                          (planRecipe.isEmpty() || effective.recipe == planRecipe),
                      "the effective identity is the admitted plan's own, with a real output geometry");
                check(effective.sourceSize.w == observedCoded.width() &&
                          effective.sourceSize.h == observedCoded.height(),
                      "the effective source size is the observed coded frame, not a descriptor promise");
                if (!effective.interpolation.has_value()) {
                    check(active->counters->framesOut == active->counters->framesIn &&
                              active->counters->interpolated == 0,
                          "a spatial-only chain counts frames out equal to frames in and no interpolation");
                } else {
                    check(active->counters->framesOut > active->counters->framesIn &&
                              active->counters->interpolated > 0 &&
                              active->counters->interpolated <= active->counters->framesOut,
                          "an interpolating chain counts the frames it synthesised beyond the source frames it consumed");
                }
            }

            // A replacement with a tampered digest is well-formed transport but
            // must be refused by the producer, keeping the effective plan.
            QJsonObject tampered = *admission;
            QJsonArray stages = tampered.value(QStringLiteral("stages")).toArray();
            if (!stages.isEmpty()) {
                QJsonObject stage = stages.at(0).toObject();
                const QString digest = stage.value(QStringLiteral("engineSha256")).toString();
                if (!digest.isEmpty()) {
                    QString flipped = digest;
                    flipped[0] = flipped.at(0) == QLatin1Char('0') ? QLatin1Char('1')
                                                                  : QLatin1Char('0');
                    stage.insert(QStringLiteral("engineSha256"), flipped);
                    stages.replace(0, stage);
                    tampered.insert(QStringLiteral("stages"), stages);
                    check(command(handle, {"vf-command", "mikai_native", "admit",
                                           QJsonDocument(tampered)
                                               .toJson(QJsonDocument::Compact)
                                               .toStdString()})
                              .ok(),
                          "a well-formed replacement is accepted by the transport");
                    const auto refused = waitForSnapshot(
                        handle,
                        [](const StatusSnapshot& snapshot) {
                            return snapshot.state == NativeState::Failed;
                        },
                        60000);
                    check(refused.has_value() && !refused->reason.isEmpty() &&
                              refused->effective.has_value(),
                          "a refused replacement names a reason and keeps the previously effective plan");
                }
            }

            // Put the good plan back and let it produce its own output.
            check(command(handle, {"vf-command", "mikai_native", "admit", admitJson.toStdString()}).ok(),
                  "the good plan replaces a refused one through the same transport");
            check(command(handle, {"set", "pause", "no"}).ok(),
                  "playback resumes for the replacement output");
            const auto restored = waitForSnapshot(
                handle,
                [](const StatusSnapshot& snapshot) { return snapshot.state == NativeState::Active; },
                120000);
            check(command(handle, {"set", "pause", "yes"}).ok(),
                  "playback freezes after the replacement output");
            check(restored.has_value() && restored->effective.has_value(),
                  "the replacement is Active only after its own completed output");

            const auto controlJson = [&](const char* action, int timeoutMs) {
                QJsonObject object;
                object.insert(QStringLiteral("schema"), QStringLiteral("mikai.native.control/1"));
                object.insert(QStringLiteral("protocol"), 1);
                object.insert(QStringLiteral("action"), QString::fromLatin1(action));
                object.insert(QStringLiteral("epoch"), static_cast<double>(planEpoch));
                object.insert(QStringLiteral("revision"), static_cast<double>(planRevision));
                object.insert(QStringLiteral("timeoutMs"), timeoutMs);
                return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
            };

            const qint64 beforeDrain = restored.has_value() ? restored->revision : 0;
            check(command(handle, {"vf-command", "mikai_native", "control", controlJson("drain", 15000)})
                      .ok(),
                  "the drain control is accepted by the named transport");
            const auto drained = waitForSnapshot(
                handle,
                [&](const StatusSnapshot& snapshot) {
                    return snapshot.revision > beforeDrain && snapshot.state == NativeState::Active;
                },
                30000);
            check(drained.has_value(),
                  "a completed drain keeps the chain Active and advances the revision");

            check(command(handle, {"vf-command", "mikai_native", "control", controlJson("suspend", 15000)})
                      .ok(),
                  "the suspend control is accepted");
            const auto suspended = waitForSnapshot(
                handle,
                [](const StatusSnapshot& snapshot) { return snapshot.state == NativeState::Suspended; },
                30000);
            check(suspended.has_value() && suspended->effective.has_value(),
                  "suspend reports Suspended and retains the effective plan");

            check(command(handle, {"vf-command", "mikai_native", "control", controlJson("resume", 15000)})
                      .ok(),
                  "the resume control is accepted");
            const auto resumed = waitForSnapshot(
                handle,
                [](const StatusSnapshot& snapshot) { return snapshot.state == NativeState::Active; },
                30000);
            check(resumed.has_value() && resumed->effective.has_value(),
                  "resume reacquires the admitted plan and reports it Active again");

            const qint64 beforeSeekEpoch = resumed.has_value() && resumed->epoch.has_value()
                                               ? *resumed->epoch
                                               : -1;
            check(command(handle, {"seek", "1.0", "absolute+exact"}).ok(),
                  "the admitted source accepts a seek discontinuity");
            const auto soughtAdmission = waitForSnapshot(
                handle,
                [&](const StatusSnapshot& snapshot) {
                    return snapshot.epoch.has_value() && *snapshot.epoch != beforeSeekEpoch;
                },
                20000);
            check(soughtAdmission.has_value() && soughtAdmission->state == NativeState::Active &&
                      soughtAdmission->effective.has_value(),
                  "a source seek keeps the admitted plan, advancing the epoch without a reload");

            handle.stop();
            return finish();
        }

        handle.stop();
        return finish();
    } catch (const std::exception& error) {
        check(false, error.what());
        return finish();
    }
}
