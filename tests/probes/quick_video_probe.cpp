// Native Qt Quick video+overlay proof for the core/render phase.
//
// The scene is a real QQuickWindow on a desktop OpenGL platform: an
// MpvVideoItem presents mpv's own rendered fixture video and the QML
// rectangles/lists above it are ordinary scene items. Nothing here paints a
// fake video, uses QQuickWidget or asserts source text; every verdict is read
// from QQuickWindow::grabWindow() or the session's overlay-free frame.
//
// The fixture is assumed to be a decodable local video whose first frame is
// non-black and whose t=0 and t=mid frames differ. The probe fails explicitly
// when either assumption does not hold instead of recording a pass.
//
// Usage: quick_video_probe --media <absolute video path>
//                           --output <absolute output directory>
//                           [--width <px>] [--height <px>]
//                           [--timeout-ms <ms>]
//
// It must run through mikai_test_launcher so the executable-adjacent native
// store is an empty scratch tree, and the launcher stages the mpv runtime that
// ProbeRuntime selects. A desktop platform (windows/xcb) with desktop OpenGL is
// required; offscreen/minimal are refused.
//
// After the scene checks it proves the public core lifetime: two isolated
// window/session scopes build a real core, hold only a weak lease from
// VideoSession::coreLease(), and destroy owners in both orders (session first
// with item/window alive, then the reverse). The weak core must expire after
// the last owner and queued render cleanup settle; a still-live core means a
// leaked decoder or retained bridge storage.
//
// Example fixture (project-generated, deterministic motion):
//   build/quick-composition-fixture.mkv  (FFV1, testsrc2, 640x360@24, 12 s)

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QSaveFile>
#include <QSettings>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QVariant>
#include <QtQml/qqml.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

#include "ProbeRuntime.hpp"
#include "VideoSession.hpp"
#include "quick/MpvVideoItem.hpp"

namespace {

using mplayer::MpvHandle;
using mplayer::VideoSession;
using mplayer::quick::MpvVideoItem;

void ensureMpvVideoItemRegistered() {
    static const bool registered = [] {
        qmlRegisterType<MpvVideoItem>("Mikai.Probe", 1, 0, "MpvVideoItem");
        return true;
    }();
    Q_UNUSED(registered);
}

constexpr const char* kSceneQml = R"QML(
import QtQuick
import Mikai.Probe 1.0

Item {
    id: root
    objectName: "probeRoot"
    width: 640
    height: 360

    Rectangle {
        anchors.fill: parent
        color: "#101010"
        z: -100
    }

    Item {
        id: videoHost
        objectName: "videoHost"
        anchors.fill: parent

        MpvVideoItem {
            objectName: "video"
            width: videoHost.width
            height: videoHost.height
            session: probeSession
        }
    }

    Item {
        id: overlayLayer
        objectName: "overlayLayer"
        anchors.fill: parent
        visible: true

        Rectangle {
            id: controlBar
            objectName: "controlBar"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 56
            color: "#ff202040"
            z: 10

            Rectangle {
                id: controlButton
                objectName: "controlButton"
                x: 16
                y: 8
                width: 120
                height: 40
                color: "#ff30a0f0"
            }

            Rectangle {
                objectName: "controlOverlayPatch"
                x: controlButton.x
                y: controlButton.y
                width: controlButton.width
                height: controlButton.height
                color: "#00000000"
                visible: false
                z: 0
            }

            ListView {
                objectName: "trackList"
                x: 160
                y: 4
                width: 320
                height: 48
                clip: true
                orientation: ListView.Horizontal
                model: ["#ffcc3333", "#ff33cc33", "#ff3333cc",
                        "#ffcccc33", "#ff33cccc", "#ffcc33cc"]
                delegate: Rectangle {
                    width: 48
                    height: 48
                    color: modelData
                }
            }
        }

        Rectangle {
            objectName: "transparentPatch"
            x: parent.width * 0.05
            y: parent.height * 0.10
            width: parent.width * 0.22
            height: parent.height * 0.22
            color: "#00ffffff"
            visible: false
            z: 20
        }

        Rectangle {
            objectName: "translucentPatch"
            x: parent.width * 0.32
            y: parent.height * 0.10
            width: parent.width * 0.22
            height: parent.height * 0.22
            color: "#80ff00ff"
            visible: false
            z: 21
        }

        Rectangle {
            objectName: "opaquePatch"
            x: parent.width * 0.59
            y: parent.height * 0.10
            width: parent.width * 0.22
            height: parent.height * 0.22
            color: "#ff00ff00"
            visible: false
            z: 22
        }

        Rectangle {
            objectName: "lowerPatch"
            x: parent.width * 0.20
            y: parent.height * 0.45
            width: parent.width * 0.30
            height: parent.height * 0.25
            color: "#ffff0000"
            visible: false
            z: 23
        }

        Rectangle {
            objectName: "upperPatch"
            x: parent.width * 0.30
            y: parent.height * 0.45
            width: parent.width * 0.30
            height: parent.height * 0.25
            color: "#ff0000ff"
            visible: false
            z: 24
        }
    }
}
)QML";

const QColor kOpaqueGreen(0x00, 0xff, 0x00);
const QColor kOpaqueRed(0xff, 0x00, 0x00);
const QColor kOpaqueBlue(0x00, 0x00, 0xff);
const QColor kTranslucentMagenta(0xff, 0x00, 0xff, 0x80);
const QColor kTransparentWhite(0xff, 0xff, 0xff, 0x00);
const QColor kControlColor(0x30, 0xa0, 0xf0);
const QColor kListFirst(0xcc, 0x33, 0x33);
const QColor kListSecond(0x33, 0xcc, 0x33);

struct LumaStats {
    double mean = 0.0;
    double variance = 0.0;
    int minimum = 255;
    int maximum = 0;
    int pixels = 0;

    bool black() const { return pixels == 0 || maximum < 32 || std::sqrt(variance) < 2.0; }
};

LumaStats lumaStats(const QImage& image) {
    LumaStats stats;
    if (image.isNull()) return stats;
    const QImage rgb = image.convertToFormat(QImage::Format_RGBA8888);
    double sum = 0.0;
    double sumSquares = 0.0;
    for (int y = 0; y < rgb.height(); ++y) {
        const uchar* row = rgb.constScanLine(y);
        for (int x = 0; x < rgb.width(); ++x) {
            const int luma = qGray(row[x * 4], row[x * 4 + 1], row[x * 4 + 2]);
            sum += luma;
            sumSquares += double(luma) * double(luma);
            stats.minimum = std::min(stats.minimum, luma);
            stats.maximum = std::max(stats.maximum, luma);
            ++stats.pixels;
        }
    }
    if (stats.pixels > 0) {
        stats.mean = sum / stats.pixels;
        const double meanSquared = sumSquares / stats.pixels;
        stats.variance = std::max(0.0, meanSquared - stats.mean * stats.mean);
    }
    return stats;
}

struct ImageDiff {
    bool valid = false;
    double mean = 0.0;
    int maximum = 0;
    double changedFraction = 0.0;
};

ImageDiff diffImages(const QImage& left, const QImage& right) {
    ImageDiff diff;
    if (left.isNull() || right.isNull() || left.size() != right.size()) return diff;
    const QImage a = left.convertToFormat(QImage::Format_RGBA8888);
    const QImage b = right.convertToFormat(QImage::Format_RGBA8888);
    quint64 channelSum = 0;
    quint64 channelCount = 0;
    quint64 changed = 0;
    quint64 sampled = 0;
    for (int y = 0; y < a.height(); y += 2) {
        const uchar* rowA = a.constScanLine(y);
        const uchar* rowB = b.constScanLine(y);
        for (int x = 0; x < a.width(); x += 2) {
            int pixelMax = 0;
            for (int channel = 0; channel < 3; ++channel) {
                const int delta = std::abs(int(rowA[x * 4 + channel]) - int(rowB[x * 4 + channel]));
                channelSum += quint64(delta);
                ++channelCount;
                pixelMax = std::max(pixelMax, delta);
            }
            diff.maximum = std::max(diff.maximum, pixelMax);
            if (pixelMax > 8) ++changed;
            ++sampled;
        }
    }
    diff.valid = channelCount > 0;
    if (diff.valid) {
        diff.mean = double(channelSum) / double(channelCount);
        diff.changedFraction = double(changed) / double(sampled);
    }
    return diff;
}

QColor samplePixel(const QImage& image, qreal logicalX, qreal logicalY, qreal scale) {
    if (image.isNull()) return QColor();
    const int x = std::clamp(int(std::lround(logicalX * scale)), 0, image.width() - 1);
    const int y = std::clamp(int(std::lround(logicalY * scale)), 0, image.height() - 1);
    return image.pixelColor(x, y);
}

QColor blendOver(const QColor& background, const QColor& overlay) {
    const qreal alpha = overlay.alphaF();
    return QColor::fromRgbF(overlay.redF() * alpha + background.redF() * (1.0 - alpha),
                            overlay.greenF() * alpha + background.greenF() * (1.0 - alpha),
                            overlay.blueF() * alpha + background.blueF() * (1.0 - alpha));
}

bool colorNear(const QColor& actual, const QColor& expected, int tolerance) {
    return std::abs(actual.red() - expected.red()) <= tolerance &&
           std::abs(actual.green() - expected.green()) <= tolerance &&
           std::abs(actual.blue() - expected.blue()) <= tolerance;
}

QString colorText(const QColor& color) {
    return QStringLiteral("#%1%2%3")
        .arg(color.red(), 2, 16, QLatin1Char('0'))
        .arg(color.green(), 2, 16, QLatin1Char('0'))
        .arg(color.blue(), 2, 16, QLatin1Char('0'));
}

struct Options {
    QString media;
    QString outputDirectory;
    int width = 640;
    int height = 360;
    int timeoutMs = 240000;
};

void printUsage() {
    std::fprintf(stderr,
                 "usage: quick_video_probe --media <absolute video> --output <absolute dir>\n"
                 "                         [--width <px>] [--height <px>] [--timeout-ms <ms>]\n");
}

bool parseOptions(const QStringList& arguments, Options* options, QString* error) {
    for (int index = 1; index < arguments.size(); ++index) {
        const QString& argument = arguments.at(index);
        auto takeValue = [&](QString* target) {
            if (index + 1 >= arguments.size()) {
                *error = QStringLiteral("%1 needs a value").arg(argument);
                return false;
            }
            *target = arguments.at(++index);
            return true;
        };
        if (argument == QLatin1String("--media")) {
            if (!takeValue(&options->media)) return false;
        } else if (argument == QLatin1String("--output") ||
                   argument == QLatin1String("--evidence-dir")) {
            if (!takeValue(&options->outputDirectory)) return false;
        } else if (argument == QLatin1String("--width")) {
            QString value;
            if (!takeValue(&value)) return false;
            bool ok = false;
            options->width = value.toInt(&ok);
            if (!ok || options->width < 320) {
                *error = QStringLiteral("--width must be an integer >= 320");
                return false;
            }
        } else if (argument == QLatin1String("--height")) {
            QString value;
            if (!takeValue(&value)) return false;
            bool ok = false;
            options->height = value.toInt(&ok);
            if (!ok || options->height < 180) {
                *error = QStringLiteral("--height must be an integer >= 180");
                return false;
            }
        } else if (argument == QLatin1String("--timeout-ms")) {
            QString value;
            if (!takeValue(&value)) return false;
            bool ok = false;
            options->timeoutMs = value.toInt(&ok);
            if (!ok || options->timeoutMs < 20000) {
                *error = QStringLiteral("--timeout-ms must be an integer >= 20000");
                return false;
            }
        } else if (argument == QLatin1String("--help") || argument == QLatin1String("-h")) {
            *error = QStringLiteral("help requested");
            return false;
        } else if (argument.startsWith(QLatin1Char('-'))) {
            *error = QStringLiteral("unknown option: %1").arg(argument);
            return false;
        } else if (options->media.isEmpty()) {
            options->media = argument;
        } else {
            *error = QStringLiteral("unexpected argument: %1").arg(argument);
            return false;
        }
    }
    if (options->media.isEmpty()) {
        *error = QStringLiteral("--media is required");
        return false;
    }
    if (!QFileInfo(options->media).isAbsolute()) {
        *error = QStringLiteral("--media must be an absolute path (the launcher runs the probe in its own scratch working directory)");
        return false;
    }
    if (!QFileInfo(options->media).isFile()) {
        *error = QStringLiteral("media does not exist: %1").arg(options->media);
        return false;
    }
    if (options->outputDirectory.isEmpty()) {
        *error = QStringLiteral("--output is required");
        return false;
    }
    if (!QFileInfo(options->outputDirectory).isAbsolute()) {
        *error = QStringLiteral("--output must be an absolute path (the launcher deletes its scratch working directory)");
        return false;
    }
    return true;
}

class Probe {
public:
    Probe(Options options, std::unique_ptr<VideoSession> session);
    ~Probe();

    int run();

private:
    bool check(const QString& name, bool ok, const QString& detail = QString());
    bool require(bool ok, const QString& name, const QString& detail = QString());
    void processEvents(int milliseconds);
    int budget(int requestedMs) const;
    bool waitFor(const std::function<bool()>& predicate, int timeoutMs);
    void settle(int milliseconds);

    bool createScene();
    bool waitForPresenter();
    bool openMedia();
    bool pauseAndWait();
    bool runCompositionScenario();
    bool runPauseScenario();
    bool runSeekScenario();
    void runResourceLifecycleScopes();
    void runOneLifecycleScope(bool sessionFirst);
    void teardownScene();
    void writeEvidence();

    QQuickItem* findItem(const char* name) const;
    void setItemVisible(const char* name, bool visible);
    void setItemZ(const char* name, qreal z);
    void setItemColor(const char* name, const QColor& color);
    void hideAllOverlays();
    qreal logicalWidth() const;
    qreal logicalHeight() const;

    QImage grabComposite(int timeoutMs = 6000);
    QImage grabVideoFrame(int timeoutMs = 6000);
    bool waitForVideoFrame(int timeoutMs);
    QImage saveCapture(const QString& key, const QImage& image);
    QJsonObject captureRecord(const QString& file, const QImage& image) const;
    qreal compositeScale(const QImage& image) const;
    QColor sampleComposite(const QImage& image, qreal x, qreal y) const;
    QString sceneFilePath();

    Options options_;
    std::unique_ptr<VideoSession> session_;
    std::unique_ptr<QQuickView> view_;
    QTemporaryDir sceneDirectory_;
    QQuickWindow* window_ = nullptr;
    MpvVideoItem* video_ = nullptr;
    QQuickItem* host_ = nullptr;

    QElapsedTimer clock_;
    QJsonArray checks_;
    QJsonObject captures_;
    QStringList failures_;
    QString lastError_;
    int failureCount_ = 0;
    bool setupFailed_ = false;
    bool mediaOpened_ = false;
    bool closeObserved_ = false;
    QJsonObject mediaRecord_;
    QJsonObject windowRecord_;
    QJsonObject sessionRecord_;
    QString startedAt_;
};

Probe::Probe(Options options, std::unique_ptr<VideoSession> session)
    : options_(std::move(options)), session_(std::move(session)) {
    startedAt_ = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

Probe::~Probe() = default;

bool Probe::check(const QString& name, bool ok, const QString& detail) {
    QJsonObject entry;
    entry.insert(QStringLiteral("name"), name);
    entry.insert(QStringLiteral("ok"), ok);
    if (!detail.isEmpty()) entry.insert(QStringLiteral("detail"), detail);
    checks_.append(entry);
    if (!ok) {
        ++failureCount_;
        failures_.append(detail.isEmpty() ? name : name + QStringLiteral(": ") + detail);
    }
    std::fprintf(stderr, "%s %s%s%s\n", ok ? "ok  " : "FAIL", qPrintable(name),
                 detail.isEmpty() ? "" : " - ", detail.isEmpty() ? "" : qPrintable(detail));
    return ok;
}

bool Probe::require(bool ok, const QString& name, const QString& detail) {
    if (!check(name, ok, detail)) setupFailed_ = true;
    return ok;
}

void Probe::processEvents(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    } while (timer.elapsed() < milliseconds);
}

int Probe::budget(int requestedMs) const {
    if (options_.timeoutMs <= 0) return requestedMs;
    const qint64 remaining = options_.timeoutMs - clock_.elapsed();
    return int(std::max<qint64>(0, std::min<qint64>(requestedMs, remaining)));
}

bool Probe::waitFor(const std::function<bool()>& predicate, int timeoutMs) {
    const int limit = budget(timeoutMs);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() <= limit) {
        if (predicate()) return true;
        processEvents(10);
    }
    return predicate();
}

void Probe::settle(int milliseconds) { processEvents(budget(milliseconds)); }

QQuickItem* Probe::findItem(const char* name) const {
    if (window_ == nullptr) return nullptr;
    return window_->findChild<QQuickItem*>(QString::fromLatin1(name));
}

void Probe::setItemVisible(const char* name, bool visible) {
    if (QQuickItem* item = findItem(name)) item->setVisible(visible);
}

void Probe::setItemZ(const char* name, qreal z) {
    if (QQuickItem* item = findItem(name)) item->setZ(z);
}

void Probe::setItemColor(const char* name, const QColor& color) {
    if (QQuickItem* item = findItem(name)) item->setProperty("color", color);
}

void Probe::hideAllOverlays() {
    setItemVisible("controlBar", false);
    setItemVisible("controlOverlayPatch", false);
    setItemVisible("transparentPatch", false);
    setItemVisible("translucentPatch", false);
    setItemVisible("opaquePatch", false);
    setItemVisible("lowerPatch", false);
    setItemVisible("upperPatch", false);
}

qreal Probe::logicalWidth() const { return window_ ? window_->width() : 0.0; }
qreal Probe::logicalHeight() const { return window_ ? window_->height() : 0.0; }

QImage Probe::grabComposite(int timeoutMs) {
    if (window_ == nullptr) return QImage();
    QImage image;
    QElapsedTimer timer;
    timer.start();
    const int limit = budget(timeoutMs);
    while (timer.elapsed() <= limit) {
        window_->requestUpdate();
        image = window_->grabWindow();
        if (!image.isNull() && image.width() > 1) return image;
        processEvents(15);
    }
    return image;
}

bool Probe::waitForVideoFrame(int timeoutMs) {
    if (video_ == nullptr) return false;
    if (video_->hasVisibleOutputFrame()) return true;
    bool presented = false;
    const QMetaObject::Connection connection =
        QObject::connect(video_, &MpvVideoItem::videoFramePresented, video_,
                         [&presented] { presented = true; });
    const bool fired =
        waitFor([&] { return presented || video_->hasVisibleOutputFrame(); }, timeoutMs);
    QObject::disconnect(connection);
    return fired;
}

QImage Probe::grabVideoFrame(int timeoutMs) {
    if (session_ == nullptr) return QImage();
    if (video_ != nullptr && waitForVideoFrame(timeoutMs)) {
        QString reason;
        const QImage frame = video_->grabVideoFrame(&reason);
        if (!frame.isNull()) return frame;
        std::fprintf(stderr, "diagnostic: the render item could not return a frame: %s\n",
                     qPrintable(reason));
    }
    QImage image;
    const int limit = budget(timeoutMs);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() <= limit) {
        image = session_->grabVideoFramebuffer();
        if (!image.isNull()) return image;
        if (window_) window_->requestUpdate();
        processEvents(15);
    }
    return image;
}

qreal Probe::compositeScale(const QImage& image) const {
    if (image.isNull() || logicalWidth() <= 0.0) return 1.0;
    return image.width() / logicalWidth();
}

QColor Probe::sampleComposite(const QImage& image, qreal x, qreal y) const {
    return samplePixel(image, x, y, compositeScale(image));
}

QJsonObject Probe::captureRecord(const QString& file, const QImage& image) const {
    const LumaStats stats = lumaStats(image);
    QJsonObject record;
    record.insert(QStringLiteral("file"), file);
    record.insert(QStringLiteral("width"), image.width());
    record.insert(QStringLiteral("height"), image.height());
    record.insert(QStringLiteral("meanLuma"), stats.mean);
    record.insert(QStringLiteral("minLuma"), stats.minimum);
    record.insert(QStringLiteral("maxLuma"), stats.maximum);
    record.insert(QStringLiteral("lumaStdDev"), std::sqrt(stats.variance));
    record.insert(QStringLiteral("black"), stats.black());
    return record;
}

QString Probe::sceneFilePath() {
    if (!sceneDirectory_.isValid()) return QString();
    const QString path =
        QDir(sceneDirectory_.path()).filePath(QStringLiteral("ProbeScene.qml"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    const QByteArray source(kSceneQml);
    if (file.write(source) != source.size()) return QString();
    return path;
}

QImage Probe::saveCapture(const QString& key, const QImage& image) {
    if (image.isNull()) {
        check(QStringLiteral("capture.%1").arg(key), false,
              QStringLiteral("no image was produced"));
        return image;
    }
    const QString file = key + QStringLiteral(".png");
    const QString path = QDir(options_.outputDirectory).filePath(file);
    if (!image.save(path, "PNG")) {
        check(QStringLiteral("capture.%1").arg(key), false,
              QStringLiteral("cannot write %1").arg(path));
    }
    captures_.insert(key, captureRecord(file, image));
    return image;
}

bool Probe::createScene() {
    ensureMpvVideoItemRegistered();
    const QString scenePath = sceneFilePath();
    if (scenePath.isEmpty()) {
        return require(false, QStringLiteral("scene.file"),
                       QStringLiteral("the QML scene cannot be written to probe scratch"));
    }
    view_ = std::make_unique<QQuickView>();
    view_->setResizeMode(QQuickView::SizeRootObjectToView);
    view_->rootContext()->setContextProperty(QStringLiteral("probeSession"),
                                             QVariant::fromValue(session_.get()));
    view_->resize(options_.width, options_.height);
    view_->setSource(QUrl::fromLocalFile(scenePath));
    if (view_->status() == QQuickView::Loading)
        waitFor([&] { return view_->status() != QQuickView::Loading; }, 8000);
    if (view_->status() == QQuickView::Error || view_->rootObject() == nullptr) {
        QStringList errors;
        for (const QQmlError& error : view_->errors()) errors.append(error.toString());
        view_.reset();
        return require(false, QStringLiteral("scene.load"),
                       errors.isEmpty()
                           ? QStringLiteral("the QML scene produced no root item")
                           : errors.join(QStringLiteral(" | ")));
    }
    window_ = view_.get();
    video_ = view_->rootObject()->findChild<MpvVideoItem*>(QStringLiteral("video"));
    host_ = view_->rootObject()->findChild<QQuickItem*>(QStringLiteral("videoHost"));
    if (video_ == nullptr || host_ == nullptr) {
        view_.reset();
        return require(false, QStringLiteral("scene.videoItem"),
                       QStringLiteral("the scene has no MpvVideoItem/videoHost"));
    }
    if (video_->session() != session_.get()) video_->setSession(session_.get());
    window_->show();
    const bool exposed = waitFor([&] { return window_->isExposed(); }, 8000);
    if (!require(exposed, QStringLiteral("scene.exposed"),
                 QStringLiteral("window width=%1 height=%2").arg(window_->width()).arg(window_->height())))
        return false;
    check(QStringLiteral("scene.videoItemInScene"),
          video_->window() == window_ && video_->isVisible() && video_->width() > 0.0 &&
              video_->height() > 0.0,
          QStringLiteral("window=%1 visible=%2 size=%3x%4")
              .arg(video_->window() == window_)
              .arg(video_->isVisible())
              .arg(video_->width())
              .arg(video_->height()));
    check(QStringLiteral("scene.graphicsApiOpenGL"),
          QQuickWindow::graphicsApi() == QSGRendererInterface::OpenGL,
          QStringLiteral("graphicsApi=%1").arg(int(QQuickWindow::graphicsApi())));
    return check(QStringLiteral("scene.videoItemSession"),
                 video_->session() == session_.get(),
                 QStringLiteral("the render item presents this probe's VideoSession"));
}

bool Probe::waitForPresenter() {
    const bool attached = waitFor([&] { return session_->presenter() != nullptr; }, 8000);
    return require(attached, QStringLiteral("renderer.presenter"),
                   QStringLiteral("presenterInstalled=%1")
                       .arg(session_->presenter() != nullptr));
}

bool Probe::openMedia() {
    QObject::connect(session_.get(), &VideoSession::errorOccurred, session_.get(),
                     [this](const QString& message) { lastError_ = message; });
    const QString url = QUrl::fromLocalFile(options_.media).toString();
    // The session core exists only after open()/ensureEngine(); the render
    // context cannot exist before it. open() queues the load until the context
    // is ready, so open first and only then wait for the real context.
    if (!require(session_->open(url, 0x51564944u), QStringLiteral("media.openEngine"),
                 QStringLiteral("VideoSession::open could not create the engine")))
        return false;
    const bool contextReady = waitFor(
        [&] {
            if (window_ != nullptr) window_->requestUpdate();
            return session_->isContextReady() && session_->renderContextGeneration() > 0;
        },
        25000);
    if (!require(contextReady, QStringLiteral("render.contextReady"),
                 QStringLiteral("contextReady=%1 generation=%2 after open()")
                     .arg(session_->isContextReady())
                     .arg(session_->renderContextGeneration())))
        return false;

    const bool loaded = waitFor(
        [&] {
            return session_->duration() > 0.0 && session_->hasVideo() && !session_->isOpening();
        },
        25000);
    if (!require(loaded, QStringLiteral("media.loaded"),
                 lastError_.isEmpty()
                     ? QStringLiteral("duration=%1 video=%2 opening=%3")
                           .arg(session_->duration())
                           .arg(session_->hasVideo())
                           .arg(session_->isOpening())
                     : lastError_))
        return false;
    mediaOpened_ = true;
    QElapsedTimer firstFrameTimer;
    firstFrameTimer.start();
    const bool firstPresented = waitForVideoFrame(20000);
    check(QStringLiteral("render.firstPresentedFrame"), firstPresented,
          QStringLiteral("hasVisibleOutputFrame=%1 waitedMs=%2")
              .arg(video_ != nullptr && video_->hasVisibleOutputFrame())
              .arg(firstFrameTimer.elapsed()));
    sessionRecord_.insert(QStringLiteral("sourceUrl"), session_->sourceUrl());
    sessionRecord_.insert(QStringLiteral("durationSeconds"), session_->duration());
    sessionRecord_.insert(QStringLiteral("videoWidth"), session_->videoWidth());
    sessionRecord_.insert(QStringLiteral("videoHeight"), session_->videoHeight());
    sessionRecord_.insert(QStringLiteral("sourceEpoch"), double(session_->sourceEpoch()));
    sessionRecord_.insert(QStringLiteral("renderContextGeneration"),
                          double(session_->renderContextGeneration()));

    const bool durationOk = session_->duration() >= 2.5;
    if (!require(durationOk, QStringLiteral("media.fixtureDuration"),
                 QStringLiteral("duration=%1 s; the fixture must be at least 2.5 s so the seek "
                                "checks have a distinct mid position")
                     .arg(session_->duration(), 0, 'f', 3)))
        return false;
    const bool sizeOk = session_->videoWidth() > 0 && session_->videoHeight() > 0;
    if (!require(sizeOk, QStringLiteral("media.fixtureVideoSize"),
                 QStringLiteral("%1x%2").arg(session_->videoWidth()).arg(session_->videoHeight())))
        return false;
    return true;
}

bool Probe::pauseAndWait() {
    session_->setPaused(true);
    const bool paused = waitFor(
        [&] { return session_->playbackPauseIntent() && session_->pauseObserved(); }, 8000);
    settle(200);
    return require(paused, QStringLiteral("paused.intentObserved"),
                   QStringLiteral("playbackPauseIntent=%1 pauseObserved=%2")
                       .arg(session_->playbackPauseIntent())
                       .arg(session_->pauseObserved()));
}

bool Probe::runCompositionScenario() {
    hideAllOverlays();
    setItemVisible("overlayLayer", false);
    settle(150);

    QImage base = grabComposite();
    saveCapture(QStringLiteral("01-composite-baseline"), base);
    if (!check(QStringLiteral("capture.compositeBaseline"), !base.isNull(),
               QStringLiteral("QQuickWindow::grabWindow produced no image")))
        return false;

    const QImage pausedFrame = grabVideoFrame();
    saveCapture(QStringLiteral("01-video-frame-paused"), pausedFrame);
    const LumaStats frameStats = lumaStats(pausedFrame);
    if (!check(QStringLiteral("video.frameAvailable"), !pausedFrame.isNull(),
               QStringLiteral("session grab returned no frame")))
        return false;
    check(QStringLiteral("video.fixtureNonBlack"), !frameStats.black(),
          QStringLiteral("mean=%1 max=%2 stddev=%3; the fixture's first paused frame is black, "
                         "so this run cannot prove visible video")
              .arg(frameStats.mean, 0, 'f', 2)
              .arg(frameStats.maximum)
              .arg(std::sqrt(frameStats.variance), 0, 'f', 2));

    video_->setVisible(false);
    const QImage withoutVideo = grabComposite();
    saveCapture(QStringLiteral("02-video-hidden"), withoutVideo);
    video_->setVisible(true);
    const QImage withVideo = grabComposite();
    saveCapture(QStringLiteral("02-video-visible"), withVideo);
    const ImageDiff visibleDiff = diffImages(withoutVideo, withVideo);
    check(QStringLiteral("video.compositedAboveBackground"),
          visibleDiff.valid && visibleDiff.mean >= 4.0 && visibleDiff.maximum >= 32,
          QStringLiteral("meanDiff=%1 maxDiff=%2 changed=%3")
              .arg(visibleDiff.mean, 0, 'f', 2)
              .arg(visibleDiff.maximum)
              .arg(visibleDiff.changedFraction, 0, 'f', 3));

    const qreal w = logicalWidth();
    const qreal h = logicalHeight();
    setItemVisible("overlayLayer", true);
    hideAllOverlays();
    setItemColor("opaquePatch", kOpaqueGreen);
    setItemColor("translucentPatch", kTranslucentMagenta);
    setItemColor("transparentPatch", kTransparentWhite);

    setItemVisible("opaquePatch", true);
    const QImage opaqueShot = grabComposite();
    saveCapture(QStringLiteral("03-opaque-over-video"), opaqueShot);
    const QColor opaquePixel = sampleComposite(opaqueShot, w * 0.70, h * 0.21);
    check(QStringLiteral("overlay.opaqueAboveVideo"), colorNear(opaquePixel, kOpaqueGreen, 6),
          QStringLiteral("sampled %1 expected %2")
              .arg(colorText(opaquePixel), colorText(kOpaqueGreen)));

    setItemVisible("opaquePatch", false);
    const QColor videoUnderTranslucent = sampleComposite(base, w * 0.43, h * 0.21);
    setItemVisible("translucentPatch", true);
    const QImage translucentShot = grabComposite();
    saveCapture(QStringLiteral("04-translucent-over-video"), translucentShot);
    const QColor translucentPixel = sampleComposite(translucentShot, w * 0.43, h * 0.21);
    const QColor expectedTranslucent = blendOver(videoUnderTranslucent, kTranslucentMagenta);
    check(QStringLiteral("overlay.translucentBlendsVideo"),
          colorNear(translucentPixel, expectedTranslucent, 16) &&
              !colorNear(translucentPixel, videoUnderTranslucent, 6),
          QStringLiteral("sampled %1 expected blend %2 over video %3")
              .arg(colorText(translucentPixel), colorText(expectedTranslucent),
                   colorText(videoUnderTranslucent)));

    setItemVisible("translucentPatch", false);
    const QColor videoUnderTransparent = sampleComposite(base, w * 0.16, h * 0.21);
    setItemVisible("transparentPatch", true);
    const QImage transparentShot = grabComposite();
    saveCapture(QStringLiteral("05-transparent-over-video"), transparentShot);
    const QColor transparentPixel = sampleComposite(transparentShot, w * 0.16, h * 0.21);
    check(QStringLiteral("overlay.transparentPassesVideo"),
          colorNear(transparentPixel, videoUnderTransparent, 4),
          QStringLiteral("sampled %1 video %2")
              .arg(colorText(transparentPixel), colorText(videoUnderTransparent)));

    setItemVisible("transparentPatch", false);
    setItemVisible("controlBar", true);
    const QImage controlsShot = grabComposite();
    saveCapture(QStringLiteral("06-controls-over-video"), controlsShot);
    const qreal buttonX = 76.0;
    const qreal buttonY = h - 28.0;
    const QColor controlPixel = sampleComposite(controlsShot, buttonX, buttonY);
    check(QStringLiteral("controls.visibleAboveVideo"), colorNear(controlPixel, kControlColor, 6),
          QStringLiteral("sampled %1 expected %2")
              .arg(colorText(controlPixel), colorText(kControlColor)));

    setItemVisible("controlOverlayPatch", true);
    setItemZ("controlOverlayPatch", 5);
    setItemColor("controlOverlayPatch", kTranslucentMagenta);
    const QImage translucentControlShot = grabComposite();
    saveCapture(QStringLiteral("07-translucent-over-control"), translucentControlShot);
    const QColor translucentControlPixel =
        sampleComposite(translucentControlShot, buttonX, buttonY);
    const QColor expectedControlBlend = blendOver(kControlColor, kTranslucentMagenta);
    check(QStringLiteral("overlay.translucentBlendsControls"),
          colorNear(translucentControlPixel, expectedControlBlend, 16),
          QStringLiteral("sampled %1 expected blend %2")
              .arg(colorText(translucentControlPixel), colorText(expectedControlBlend)));

    setItemColor("controlOverlayPatch", kTransparentWhite);
    const QImage transparentControlShot = grabComposite();
    saveCapture(QStringLiteral("07-transparent-over-control"), transparentControlShot);
    const QColor transparentControlPixel =
        sampleComposite(transparentControlShot, buttonX, buttonY);
    check(QStringLiteral("overlay.transparentAboveControls"),
          colorNear(transparentControlPixel, kControlColor, 4),
          QStringLiteral("sampled %1 control %2")
              .arg(colorText(transparentControlPixel), colorText(kControlColor)));

    setItemColor("controlOverlayPatch", kOpaqueGreen);
    const QImage opaqueControlShot = grabComposite();
    saveCapture(QStringLiteral("07-opaque-over-control"), opaqueControlShot);
    const QColor opaqueControlPixel = sampleComposite(opaqueControlShot, buttonX, buttonY);
    check(QStringLiteral("overlay.opaqueAboveControls"),
          colorNear(opaqueControlPixel, kOpaqueGreen, 6),
          QStringLiteral("sampled %1 expected %2")
              .arg(colorText(opaqueControlPixel), colorText(kOpaqueGreen)));

    setItemZ("controlOverlayPatch", -1);
    const QImage reorderedControlShot = grabComposite();
    saveCapture(QStringLiteral("07-control-above-overlay"), reorderedControlShot);
    const QColor reorderedControlPixel =
        sampleComposite(reorderedControlShot, buttonX, buttonY);
    check(QStringLiteral("controls.reorderAboveOverlay"),
          colorNear(reorderedControlPixel, kControlColor, 6),
          QStringLiteral("sampled %1 expected control %2")
              .arg(colorText(reorderedControlPixel), colorText(kControlColor)));

    setItemVisible("controlOverlayPatch", false);
    setItemVisible("controlBar", false);
    setItemColor("lowerPatch", kOpaqueRed);
    setItemColor("upperPatch", kOpaqueBlue);
    setItemZ("lowerPatch", 23);
    setItemZ("upperPatch", 24);
    setItemVisible("lowerPatch", true);
    setItemVisible("upperPatch", true);
    const QImage reorderFirst = grabComposite();
    saveCapture(QStringLiteral("08-reorder-blue-on-top"), reorderFirst);
    const QColor topStart = sampleComposite(reorderFirst, w * 0.40, h * 0.575);
    check(QStringLiteral("overlay.reorderTopStartsBlue"), colorNear(topStart, kOpaqueBlue, 6),
          QStringLiteral("sampled %1 expected blue %2")
              .arg(colorText(topStart), colorText(kOpaqueBlue)));

    setItemZ("upperPatch", 19);
    const QImage reorderSecond = grabComposite();
    saveCapture(QStringLiteral("08-reorder-red-on-top"), reorderSecond);
    const QColor topAfter = sampleComposite(reorderSecond, w * 0.40, h * 0.575);
    check(QStringLiteral("overlay.reorderChangesTopItem"), colorNear(topAfter, kOpaqueRed, 6),
          QStringLiteral("sampled %1 expected red %2")
              .arg(colorText(topAfter), colorText(kOpaqueRed)));
    return true;
}

bool Probe::runPauseScenario() {
    setItemVisible("overlayLayer", false);
    hideAllOverlays();
    settle(150);
    const QImage closedBaseline = grabComposite();
    const QImage closedFrame = grabVideoFrame();
    saveCapture(QStringLiteral("09-paused-closed"), closedBaseline);

    setItemVisible("overlayLayer", true);
    setItemVisible("controlBar", true);
    setItemVisible("controlOverlayPatch", true);
    setItemZ("controlOverlayPatch", 5);
    setItemColor("controlOverlayPatch", kOpaqueGreen);
    setItemVisible("transparentPatch", true);
    setItemVisible("translucentPatch", true);
    setItemVisible("opaquePatch", true);
    setItemVisible("lowerPatch", true);
    setItemVisible("upperPatch", true);
    const QImage openComposite = grabComposite();
    const QImage openFrame = grabVideoFrame();
    saveCapture(QStringLiteral("09-paused-overlays-open"), openComposite);

    const ImageDiff videoDiff = diffImages(closedFrame, openFrame);
    check(QStringLiteral("paused.uiOnlyOverlayLeavesVideoFrame"),
          videoDiff.valid && videoDiff.mean <= 1.0 && videoDiff.maximum <= 6,
          QStringLiteral("meanDiff=%1 maxDiff=%2")
              .arg(videoDiff.mean, 0, 'f', 3)
              .arg(videoDiff.maximum));

    const ImageDiff openDiff = diffImages(closedBaseline, openComposite);
    check(QStringLiteral("paused.overlayOpenChangesComposite"),
          openDiff.valid && openDiff.mean >= 4.0,
          QStringLiteral("meanDiff=%1 changed=%2")
              .arg(openDiff.mean, 0, 'f', 2)
              .arg(openDiff.changedFraction, 0, 'f', 3));

    setItemVisible("overlayLayer", false);
    const QImage closedComposite = grabComposite();
    saveCapture(QStringLiteral("09-paused-overlays-closed"), closedComposite);
    const ImageDiff closeDiff = diffImages(closedBaseline, closedComposite);
    check(QStringLiteral("paused.overlayCloseRestoresComposite"),
          closeDiff.valid && closeDiff.mean <= 2.0,
          QStringLiteral("meanDiff=%1").arg(closeDiff.mean, 0, 'f', 3));

    setItemVisible("overlayLayer", true);
    hideAllOverlays();
    setItemVisible("controlBar", true);
    setItemVisible("trackList", true);
    if (QQuickItem* list = findItem("trackList")) list->setProperty("contentX", 0.0);
    settle(100);
    const QImage listStart = grabComposite();
    saveCapture(QStringLiteral("10-list-start"), listStart);
    const QColor firstDelegate = sampleComposite(listStart, 184.0, logicalHeight() - 28.0);
    if (QQuickItem* list = findItem("trackList")) list->setProperty("contentX", 48.0);
    settle(100);
    const QImage listScrolled = grabComposite();
    saveCapture(QStringLiteral("10-list-scrolled"), listScrolled);
    const QColor secondDelegate = sampleComposite(listScrolled, 184.0, logicalHeight() - 28.0);
    check(QStringLiteral("list.delegatesAboveVideo"),
          colorNear(firstDelegate, kListFirst, 6),
          QStringLiteral("sampled %1 expected first delegate %2")
              .arg(colorText(firstDelegate), colorText(kListFirst)));
    check(QStringLiteral("list.scrollsAboveVideo"),
          colorNear(secondDelegate, kListSecond, 6) &&
              !colorNear(firstDelegate, secondDelegate, 1),
          QStringLiteral("sampled %1 expected second delegate %2")
              .arg(colorText(secondDelegate), colorText(kListSecond)));
    return true;
}

bool Probe::runSeekScenario() {
    const QImage originFrame = grabVideoFrame();
    saveCapture(QStringLiteral("11-paused-origin"), originFrame);

    const double duration = session_->duration();
    const double midTarget = std::min(std::max(1.0, duration * 0.5), duration - 0.5);
    session_->seek(midTarget);
    const bool midLanded =
        waitFor([&] { return std::abs(session_->position() - midTarget) <= 1.0; }, 10000);
    settle(400);

    QImage midFrame;
    bool midRefreshed = false;
    QElapsedTimer midTimer;
    midTimer.start();
    while (midTimer.elapsed() <= budget(10000)) {
        midFrame = grabVideoFrame(2000);
        if (!midFrame.isNull()) {
            const ImageDiff diff = diffImages(originFrame, midFrame);
            if (diff.valid && diff.maximum >= 24) {
                midRefreshed = true;
                break;
            }
        }
        processEvents(50);
    }
    if (midFrame.isNull()) midFrame = grabVideoFrame(2000);
    saveCapture(QStringLiteral("11-seek-mid"), midFrame);
    check(QStringLiteral("seek.midLanded"), midLanded,
          QStringLiteral("target=%1 position=%2")
              .arg(midTarget, 0, 'f', 3)
              .arg(session_->position(), 0, 'f', 3));
    check(QStringLiteral("seek.midPausedRefresh"), midRefreshed,
          QStringLiteral("the paused picture %1 the mid frame (the fixture must differ from "
                         "the t=0 frame)")
              .arg(midRefreshed ? QStringLiteral("shows") : QStringLiteral("never showed")));

    session_->seek(0.0);
    const bool zeroLanded = waitFor([&] { return session_->position() <= 0.75; }, 10000);

    QImage zeroFrame;
    bool frameChanged = false;
    QElapsedTimer refreshTimer;
    refreshTimer.start();
    while (refreshTimer.elapsed() <= budget(10000)) {
        zeroFrame = grabVideoFrame(2000);
        if (!zeroFrame.isNull()) {
            const ImageDiff diff = diffImages(midFrame, zeroFrame);
            if (diff.valid && diff.maximum >= 24) {
                frameChanged = true;
                break;
            }
        }
        processEvents(50);
    }
    if (zeroFrame.isNull()) zeroFrame = grabVideoFrame(2000);
    saveCapture(QStringLiteral("11-seek-zero"), zeroFrame);
    const LumaStats zeroStats = lumaStats(zeroFrame);

    check(QStringLiteral("seek.zeroPosition"),
          zeroLanded && session_->position() <= 0.75,
          QStringLiteral("position=%1").arg(session_->position(), 0, 'f', 3));
    check(QStringLiteral("seek.pausedFrameRefreshedAtZero"),
          frameChanged && !zeroStats.black(),
          frameChanged
              ? QStringLiteral("the paused picture changed to the t=0 frame (maxLuma=%1)")
                    .arg(zeroStats.maximum)
              : QStringLiteral("the paused picture never changed after seeking to 0 within the "
                               "bound; the fixture must differ at t=0 and t=mid, and the paused "
                               "frame must refresh on a seek"));
    check(QStringLiteral("seek.pausedIntentPreserved"), session_->playbackPauseIntent(),
          QStringLiteral("playbackPauseIntent=%1 pauseObserved=%2")
              .arg(session_->playbackPauseIntent())
              .arg(session_->pauseObserved()));
    return true;
}

void Probe::runResourceLifecycleScopes() {
    ensureMpvVideoItemRegistered();
    runOneLifecycleScope(/*sessionFirst=*/true);
    runOneLifecycleScope(/*sessionFirst=*/false);
}

// The core lifetime an adapter must not leak: after the render item and the
// session are both gone, nothing may still own the mpv core. The observation is
// the public shared lease held weakly, never a private field.
void Probe::runOneLifecycleScope(bool sessionFirst) {
    const QString scope = sessionFirst ? QStringLiteral("lifecycle.sessionBeforeWindow")
                                       : QStringLiteral("lifecycle.windowBeforeSession");
    auto session = std::make_unique<VideoSession>();
    const QString lifecycleScene = sceneFilePath();
    auto view = std::make_unique<QQuickView>();
    view->setResizeMode(QQuickView::SizeRootObjectToView);
    view->rootContext()->setContextProperty(QStringLiteral("probeSession"),
                                            QVariant::fromValue(session.get()));
    view->resize(options_.width, options_.height);
    if (!lifecycleScene.isEmpty()) view->setSource(QUrl::fromLocalFile(lifecycleScene));
    if (view->status() == QQuickView::Loading)
        waitFor([&] { return view->status() != QQuickView::Loading; }, 8000);
    QQuickWindow* window =
        view->status() == QQuickView::Ready ? view.get() : nullptr;
    MpvVideoItem* video = window != nullptr && view->rootObject() != nullptr
                              ? view->rootObject()->findChild<MpvVideoItem*>(
                                    QStringLiteral("video"))
                              : nullptr;
    if (!check(scope + QStringLiteral(".sceneMounted"), window != nullptr && video != nullptr,
               lifecycleScene.isEmpty()
                   ? QStringLiteral("the QML scene file could not be written")
                   : QStringLiteral("a separate window/session scope for this teardown order"))) {
        if (video != nullptr) video->setSession(nullptr);
        if (window != nullptr) window->hide();
        view.reset();
        session.reset();
        return;
    }

    if (video->session() != session.get()) video->setSession(session.get());
    window->resize(options_.width, options_.height);
    window->show();

    const bool exposed = waitFor([&] { return window->isExposed(); }, 8000);
    bool ok = check(scope + QStringLiteral(".exposed"), exposed,
                    QStringLiteral("visible=%1 exposed=%2")
                        .arg(window->isVisible())
                        .arg(window->isExposed()));
    if (ok) {
        const bool attached = waitFor([&] { return session->presenter() != nullptr; }, 8000);
        ok = check(scope + QStringLiteral(".presenter"), attached,
                   QStringLiteral("presenterInstalled=%1")
                       .arg(session->presenter() != nullptr));
    }
    if (ok) {
        const bool opened =
            session->open(QUrl::fromLocalFile(options_.media).toString(), 0x4c494645u);
        ok = check(scope + QStringLiteral(".open"), opened,
                   opened ? QStringLiteral("engine created; load queued until the context is ready")
                          : QStringLiteral("VideoSession::open could not create the engine"));
    }
    if (ok) {
        const bool contextReady = waitFor(
            [&] {
                window->requestUpdate();
                return session->isContextReady() && session->renderContextGeneration() > 0;
            },
            25000);
        ok = check(scope + QStringLiteral(".context"), contextReady,
                   QStringLiteral("contextReady=%1 generation=%2 after open()")
                       .arg(session->isContextReady())
                       .arg(session->renderContextGeneration()));
    }
    if (ok) {
        const bool loaded = waitFor(
            [&] {
                return session->duration() > 0.0 && session->hasVideo() && !session->isOpening();
            },
            25000);
        ok = check(scope + QStringLiteral(".loaded"), loaded,
                   QStringLiteral("duration=%1 video=%2 opening=%3")
                       .arg(session->duration(), 0, 'f', 3)
                       .arg(session->hasVideo())
                       .arg(session->isOpening()));
    }
    if (ok) {
        QElapsedTimer firstFrameTimer;
        firstFrameTimer.start();
        bool presented = video->hasVisibleOutputFrame();
        if (!presented) {
            const QMetaObject::Connection connection = QObject::connect(
                video, &MpvVideoItem::videoFramePresented, video,
                [&presented] { presented = true; });
            presented = waitFor([&] { return presented || video->hasVisibleOutputFrame(); },
                                20000);
            QObject::disconnect(connection);
        }
        ok = check(scope + QStringLiteral(".firstFrame"), presented,
                   QStringLiteral("presented=%1 hasVisibleOutputFrame=%2 waitedMs=%3")
                       .arg(presented)
                       .arg(video->hasVisibleOutputFrame())
                       .arg(firstFrameTimer.elapsed()));
    }

    std::weak_ptr<MpvHandle> weakCore;
    if (ok) {
        weakCore = session->coreLease();
        const bool leaseObservable = !weakCore.expired();
        ok = check(scope + QStringLiteral(".coreLease"), leaseObservable,
                   leaseObservable
                       ? QStringLiteral("core lease observable; generation=%1")
                             .arg(session->renderContextGeneration())
                       : QStringLiteral("the session exposed no core lease"));
    }
    if (!ok) {
        video->setSession(nullptr);
        window->hide();
        view.reset();
        session.reset();
        return;
    }

    if (sessionFirst) {
        const bool itemStayedMounted = video->window() == window && window->isVisible();
        session.reset();
        // The item's destroyed handler must detach the still-live QML binding
        // itself; the probe only retires the dangling context-property value
        // afterwards so the later item teardown cannot re-resolve it.
        const bool bindingCleared = video->session() == nullptr;
        check(scope + QStringLiteral(".bindingCleared"), bindingCleared,
              QStringLiteral("itemSessionCleared=%1").arg(bindingCleared));
        view->rootContext()->setContextProperty(
            QStringLiteral("probeSession"), QVariant::fromValue<VideoSession*>(nullptr));
        video->setSession(nullptr);
        // The bridge keeps the core while a live render context exists, so the
        // release is deferred to the item/window teardown below, not the session.
        const bool survivesSession = itemStayedMounted && video->window() == window;
        check(scope + QStringLiteral(".itemSurvivesSession"), survivesSession,
              QStringLiteral("mountedBeforeSessionDeath=%1 mountedAfterSessionDeath=%2 "
                             "windowVisible=%3")
                  .arg(itemStayedMounted)
                  .arg(video->window() == window)
                  .arg(window->isVisible()));
        video->setParentItem(nullptr);
        QPointer<QQuickWindow> windowGuard(window);
        window->hide();
        view.reset();
        // The QQuickView owns the QQuickWindow; the guard covers an unexpected
        // owner so the SG shutdown always runs before the lease is checked.
        if (QQuickWindow* remaining = windowGuard.data()) delete remaining;
        processEvents(200);
        QElapsedTimer expiryTimer;
        expiryTimer.start();
        const bool expiredAfterAllOwners = waitFor([&] { return weakCore.expired(); }, 10000);
        check(scope + QStringLiteral(".coreExpiresAfterAllOwners"), expiredAfterAllOwners,
              expiredAfterAllOwners
                  ? QStringLiteral("core released after %1 ms once the SG node teardown freed "
                                   "the render context and its bridge lease")
                        .arg(expiryTimer.elapsed())
                  : QStringLiteral("the core outlived the session and the item/window that "
                                   "presented it; a leaked decoder or retired bridge storage "
                                   "still owns it"));
        return;
    }

    QPointer<QQuickWindow> windowGuard(window);
    QPointer<MpvVideoItem> itemGuard(video);
    window->hide();
    view.reset();
    if (QQuickWindow* remaining = windowGuard.data()) delete remaining;
    const bool itemDiedWithWindow = itemGuard.isNull();
    if (MpvVideoItem* remainingItem = itemGuard.data()) delete remainingItem;
    processEvents(400);
    check(scope + QStringLiteral(".itemDestroyedWithWindow"), itemDiedWithWindow,
          itemDiedWithWindow
              ? QStringLiteral("the render item was destroyed by the scene teardown")
              : QStringLiteral("the render item outlived its window; presenter detach below is "
                               "not the destructor path"));
    check(scope + QStringLiteral(".presenterDetachedOnItemDestruction"),
          itemDiedWithWindow && session->presenter() == nullptr,
          QStringLiteral("itemDied=%1 presenter=%2")
              .arg(itemDiedWithWindow)
              .arg(session->presenter() == nullptr));
    const bool heldBySession = !weakCore.expired();
    check(scope + QStringLiteral(".coreHeldBySession"), heldBySession,
          heldBySession
              ? QStringLiteral("the session still owns the core after its window/item are gone")
              : QStringLiteral("the core disappeared before the session that owns it was destroyed"));
    session.reset();
    QElapsedTimer expiryTimer;
    expiryTimer.start();
    const bool expiredAfterSession = waitFor([&] { return weakCore.expired(); }, 10000);
    check(scope + QStringLiteral(".coreExpiresAfterSession"), expiredAfterSession,
          expiredAfterSession
              ? QStringLiteral("core released after %1 ms once the last owner died")
                    .arg(expiryTimer.elapsed())
              : QStringLiteral("the core outlived every owner; the bridge or a retired storage "
                               "leaked it"));
}

void Probe::teardownScene() {
    if (session_ == nullptr) return;
    session_->close();
    closeObserved_ = waitFor([&] { return !session_->hasVideo(); }, 8000);
    if (mediaOpened_) {
        check(QStringLiteral("teardown.closeReleasesSource"), closeObserved_,
              QStringLiteral("hasVideo=%1 sourceUrl=%2")
                  .arg(session_->hasVideo())
                  .arg(session_->sourceUrl().isEmpty() ? QStringLiteral("<empty>")
                                                       : session_->sourceUrl()));
    }
    if (video_ != nullptr) {
        video_->setSession(nullptr);
        video_->setParentItem(nullptr);
    }
    QPointer<QQuickWindow> windowGuard(window_);
    if (window_ != nullptr) window_->hide();
    video_ = nullptr;
    host_ = nullptr;
    window_ = nullptr;
    view_.reset();
    if (QQuickWindow* remaining = windowGuard.data()) delete remaining;
}

void Probe::writeEvidence() {
    QJsonObject root;
    root.insert(QStringLiteral("probe"), QStringLiteral("quick-video-probe"));
    root.insert(QStringLiteral("schemaVersion"), 1);
    root.insert(QStringLiteral("pass"), failures_.isEmpty());
    root.insert(QStringLiteral("failureCount"), int(failures_.size()));
    QJsonArray failures;
    for (const QString& failure : failures_) failures.append(failure);
    root.insert(QStringLiteral("failures"), failures);
    root.insert(QStringLiteral("checks"), checks_);
    root.insert(QStringLiteral("captures"), captures_);
    root.insert(QStringLiteral("startedAt"), startedAt_);
    root.insert(QStringLiteral("finishedAt"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    root.insert(QStringLiteral("durationMs"), qint64(clock_.isValid() ? clock_.elapsed() : 0));
    root.insert(QStringLiteral("media"), mediaRecord_);

    QJsonObject fixture;
    fixture.insert(QStringLiteral("path"), options_.media);
    fixture.insert(QStringLiteral("requiresNonBlack"), true);
    fixture.insert(QStringLiteral("requiresDistinctZeroAndMidFrames"), true);
    fixture.insert(QStringLiteral("minimumDurationSeconds"), 2.5);
    fixture.insert(
        QStringLiteral("notes"),
        QStringLiteral("A decodable local video. The probe refuses to record a pass when the "
                       "first paused frame is black or when seeking to 0 does not change the "
                       "paused picture; use a real motion clip, not a black benchmark clip."));
    root.insert(QStringLiteral("fixture"), fixture);

    QJsonObject environment;
    environment.insert(QStringLiteral("platform"),
                       QGuiApplication::platformName());
    environment.insert(QStringLiteral("qtVersion"), QString::fromLatin1(qVersion()));
    environment.insert(QStringLiteral("applicationDirPath"),
                       QCoreApplication::applicationDirPath());
    environment.insert(
        QStringLiteral("runtimeNote"),
        QStringLiteral("mpv is selected by the shared probe runtime helper from the staged "
                       "MIKAI_MPV_MODULE or MIKAI_PROBE_BUNDLE; this probe never reads an "
                       "installed native model store."));
    root.insert(QStringLiteral("environment"), environment);

    QJsonObject windowInfo = windowRecord_;
    windowInfo.insert(QStringLiteral("requestedWidth"), options_.width);
    windowInfo.insert(QStringLiteral("requestedHeight"), options_.height);
    root.insert(QStringLiteral("window"), windowInfo);

    QJsonObject sessionInfo = sessionRecord_;
    sessionInfo.insert(QStringLiteral("closeObserved"), closeObserved_);
    sessionInfo.insert(QStringLiteral("sourceAfterClose"), session_->sourceUrl());
    root.insert(QStringLiteral("session"), sessionInfo);

    const QString path =
        QDir(options_.outputDirectory).filePath(QStringLiteral("quick-video-probe.json"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "FAIL evidence.write - cannot open %s\n", qPrintable(path));
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit())
        std::fprintf(stderr, "FAIL evidence.write - cannot commit %s\n", qPrintable(path));
}

int Probe::run() {
    clock_.start();
    mediaRecord_.insert(QStringLiteral("path"), options_.media);
    mediaRecord_.insert(QStringLiteral("bytes"), double(QFileInfo(options_.media).size()));

    bool stageOk = createScene();
    if (stageOk) stageOk = waitForPresenter();
    if (stageOk) stageOk = openMedia();
    if (stageOk) stageOk = pauseAndWait();

    if (stageOk && session_ && session_->duration() > 0.0) {
        mediaRecord_.insert(QStringLiteral("durationSeconds"), session_->duration());
        mediaRecord_.insert(QStringLiteral("videoWidth"), session_->videoWidth());
        mediaRecord_.insert(QStringLiteral("videoHeight"), session_->videoHeight());
        mediaRecord_.insert(QStringLiteral("sourceEpoch"), double(session_->sourceEpoch()));
    }

    if (stageOk) runCompositionScenario();

    if (stageOk && window_) {
        const QSizeF initialItemSize = video_ != nullptr ? video_->size() : QSizeF();
        const quint64 epochBeforeResize = session_->sourceEpoch();
        window_->resize(800, 450);
        waitFor([&] { return window_->width() == 800 && window_->height() == 450; }, 4000);
        settle(250);
        const QImage resized = grabComposite();
        saveCapture(QStringLiteral("12-resized-composite"), resized);
        const QSizeF itemSize = video_ != nullptr ? video_->size() : QSizeF();
        check(QStringLiteral("resize.windowReachedRequestedSize"),
              window_->width() == 800 && window_->height() == 450,
              QStringLiteral("window=%1x%2").arg(window_->width()).arg(window_->height()));
        check(QStringLiteral("resize.videoItemTargetSize"),
              video_ != nullptr && qRound(itemSize.width()) == window_->width() &&
                  qRound(itemSize.height()) == window_->height(),
              QStringLiteral("item=%1x%2 window=%3x%4 source=%5x%6")
                  .arg(itemSize.width(), 0, 'f', 1)
                  .arg(itemSize.height(), 0, 'f', 1)
                  .arg(window_->width())
                  .arg(window_->height())
                  .arg(session_->videoWidth())
                  .arg(session_->videoHeight()));
        check(QStringLiteral("resize.videoItemChangedFromInitial"),
              itemSize != initialItemSize && qRound(itemSize.width()) == 800 &&
                  qRound(itemSize.height()) == 450,
              QStringLiteral("initial=%1x%2 now=%3x%4")
                  .arg(initialItemSize.width(), 0, 'f', 1)
                  .arg(initialItemSize.height(), 0, 'f', 1)
                  .arg(itemSize.width(), 0, 'f', 1)
                  .arg(itemSize.height(), 0, 'f', 1));
        const qreal observedDpr =
            window_->width() > 0 ? resized.width() / qreal(window_->width()) : 0.0;
        const int expectedGrabWidth = qRound(window_->width() * window_->devicePixelRatio());
        const int expectedGrabHeight = qRound(window_->height() * window_->devicePixelRatio());
        windowRecord_.insert(QStringLiteral("logicalWidth"), window_->width());
        windowRecord_.insert(QStringLiteral("logicalHeight"), window_->height());
        windowRecord_.insert(QStringLiteral("devicePixelRatio"), window_->devicePixelRatio());
        windowRecord_.insert(QStringLiteral("graphicsApi"), int(window_->graphicsApi()));
        sessionRecord_.insert(QStringLiteral("sourceUrl"), session_->sourceUrl());
        sessionRecord_.insert(QStringLiteral("durationSeconds"), session_->duration());
        sessionRecord_.insert(QStringLiteral("videoWidth"), session_->videoWidth());
        sessionRecord_.insert(QStringLiteral("videoHeight"), session_->videoHeight());
        sessionRecord_.insert(QStringLiteral("sourceEpoch"), double(session_->sourceEpoch()));
        sessionRecord_.insert(QStringLiteral("renderContextGeneration"),
                              double(session_->renderContextGeneration()));
        const QImage resizedFrame = grabVideoFrame();
        check(QStringLiteral("resize.compositeMatchesWindow"),
              !resized.isNull() && resized.width() == expectedGrabWidth &&
                  resized.height() == expectedGrabHeight,
              QStringLiteral("grab=%1x%2 expected=%3x%4 window=%5x%6 dpr=%7")
                  .arg(resized.width())
                  .arg(resized.height())
                  .arg(expectedGrabWidth)
                  .arg(expectedGrabHeight)
                  .arg(window_->width())
                  .arg(window_->height())
                  .arg(window_->devicePixelRatio(), 0, 'f', 3));
        check(QStringLiteral("resize.devicePixelRatioConsistent"),
              std::abs(observedDpr - window_->devicePixelRatio()) <= 0.01,
              QStringLiteral("grabScale=%1 windowDpr=%2")
                  .arg(observedDpr, 0, 'f', 4)
                  .arg(window_->devicePixelRatio(), 0, 'f', 4));
        check(QStringLiteral("resize.videoFrameStillPresent"),
              !resizedFrame.isNull() && !lumaStats(resizedFrame).black(),
              QStringLiteral("frame=%1x%2")
                  .arg(resizedFrame.width())
                  .arg(resizedFrame.height()));
        if (host_ != nullptr && video_ != nullptr) {
            check(QStringLiteral("resize.videoItemTracksHost"),
                  std::abs(video_->width() - host_->width()) <= 1.0 &&
                      std::abs(video_->height() - host_->height()) <= 1.0,
                  QStringLiteral("item=%1x%2 host=%3x%4")
                      .arg(video_->width())
                      .arg(video_->height())
                      .arg(host_->width())
                      .arg(host_->height()));
        }
        setItemVisible("overlayLayer", true);
        hideAllOverlays();
        setItemColor("opaquePatch", kOpaqueGreen);
        setItemVisible("opaquePatch", true);
        const QImage resizedOverlay = grabComposite();
        saveCapture(QStringLiteral("12-resized-overlay"), resizedOverlay);
        const QColor resizedOpaque = sampleComposite(
            resizedOverlay, window_->width() * 0.70, window_->height() * 0.21);
        check(QStringLiteral("resize.overlayMapsToNewWindow"),
              colorNear(resizedOpaque, kOpaqueGreen, 6),
              QStringLiteral("sampled %1 at %2x%3 logical")
                  .arg(colorText(resizedOpaque))
                  .arg(window_->width())
                  .arg(window_->height()));
        setItemVisible("overlayLayer", false);
        check(QStringLiteral("resize.sourceEpochStable"),
              session_->sourceEpoch() == epochBeforeResize,
              QStringLiteral("before=%1 after=%2")
                  .arg(epochBeforeResize)
                  .arg(session_->sourceEpoch()));
        if (QQuickItem* list = findItem("trackList")) list->setProperty("contentX", 0.0);
        runPauseScenario();
    }

    if (stageOk && window_ != nullptr) {
        const bool contextBefore = session_->isContextReady();
        const quint64 generationBefore = session_->renderContextGeneration();
        const quint64 epochBeforeHide = session_->sourceEpoch();
        window_->hide();
        processEvents(400);
        const bool contextWhileHidden = session_->isContextReady();
        window_->show();
        const bool exposedAgain = waitFor([&] { return window_->isExposed(); }, 8000);
        QImage afterShow;
        QElapsedTimer showTimer;
        showTimer.start();
        while (showTimer.elapsed() <= budget(8000)) {
            afterShow = grabComposite(2000);
            if (!afterShow.isNull() && !lumaStats(afterShow).black()) break;
            processEvents(50);
        }
        const QImage afterShowFrame = grabVideoFrame();
        const bool contextAfter = session_->isContextReady();
        check(QStringLiteral("hideShow.pictureReturns"),
              exposedAgain && !afterShow.isNull() && !lumaStats(afterShow).black() &&
                  !afterShowFrame.isNull() && !lumaStats(afterShowFrame).black(),
              QStringLiteral("exposed=%1 compositeMaxLuma=%2 frameMaxLuma=%3")
                  .arg(exposedAgain)
                  .arg(lumaStats(afterShow).maximum)
                  .arg(lumaStats(afterShowFrame).maximum));
        check(QStringLiteral("hideShow.contextUsable"),
              contextBefore && contextAfter,
              QStringLiteral("before=%1 hidden=%2 after=%3 generationBefore=%4 generationAfter=%5")
                  .arg(contextBefore)
                  .arg(contextWhileHidden)
                  .arg(contextAfter)
                  .arg(generationBefore)
                  .arg(session_->renderContextGeneration()));
        check(QStringLiteral("hideShow.sourceEpochStable"),
              session_->sourceEpoch() == epochBeforeHide,
              QStringLiteral("before=%1 after=%2")
                  .arg(epochBeforeHide)
                  .arg(session_->sourceEpoch()));
        saveCapture(QStringLiteral("13-after-show"), afterShow);

        if (host_ != nullptr && video_ != nullptr) {
            video_->setParentItem(nullptr);
            processEvents(300);
            video_->setParentItem(host_);
            window_->requestUpdate();
            QImage detachedComposite;
            QElapsedTimer detachTimer;
            detachTimer.start();
            while (detachTimer.elapsed() <= budget(8000)) {
                detachedComposite = grabComposite(2000);
                if (!detachedComposite.isNull() && !lumaStats(detachedComposite).black()) break;
                processEvents(50);
            }
            const QImage detachedFrame = grabVideoFrame();
            check(QStringLiteral("detach.reattachRendersAgain"),
                  !detachedComposite.isNull() && !lumaStats(detachedComposite).black() &&
                      !detachedFrame.isNull() && !lumaStats(detachedFrame).black(),
                  QStringLiteral("compositeMaxLuma=%1 frameMaxLuma=%2 contextReady=%3")
                      .arg(lumaStats(detachedComposite).maximum)
                      .arg(lumaStats(detachedFrame).maximum)
                      .arg(session_->isContextReady()));
            saveCapture(QStringLiteral("14-after-reattach"), detachedComposite);
        }

        runSeekScenario();
    }

    teardownScene();
    if (stageOk) runResourceLifecycleScopes();
    writeEvidence();
    const int exitCode = (!stageOk || setupFailed_) ? 2 : (failureCount_ == 0 ? 0 : 1);
    std::fprintf(stderr, "quick-video-probe: %s (%d failure(s), evidence: %s)\n",
                 exitCode == 0 ? "PASS" : "FAIL", failureCount_,
                 qPrintable(QDir(options_.outputDirectory)
                                .filePath(QStringLiteral("quick-video-probe.json"))));
    return exitCode;
}

}  // namespace

int main(int argc, char** argv) {
    QStringList arguments;
    arguments.reserve(argc);
    for (int index = 0; index < argc; ++index)
        arguments.append(QString::fromLocal8Bit(argv[index]));

    Options options;
    QString parseError;
    if (!parseOptions(arguments, &options, &parseError)) {
        std::fprintf(stderr, "FAIL options - %s\n", qPrintable(parseError));
        printUsage();
        return 2;
    }
    if (!QDir().mkpath(options.outputDirectory)) {
        std::fprintf(stderr, "FAIL options - cannot create output directory %s\n",
                     qPrintable(options.outputDirectory));
        return 2;
    }

    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
#ifdef Q_OS_WIN
        qputenv("QT_QPA_PLATFORM", "windows");
#else
        qputenv("QT_QPA_PLATFORM", "xcb");
#endif
    }
    const QString platform = qEnvironmentVariable("QT_QPA_PLATFORM");
    if (platform.startsWith(QLatin1String("offscreen")) ||
        platform.startsWith(QLatin1String("minimal"))) {
        std::fprintf(stderr,
                     "FAIL platform - a desktop platform with OpenGL is required, not %s\n",
                     qPrintable(platform));
        return 2;
    }

    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setOrganizationName(QStringLiteral("MikaiProbe"));
    app.setApplicationName(QStringLiteral("QuickVideoProbe"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    QTemporaryDir settingsDirectory;
    if (!settingsDirectory.isValid()) {
        std::fprintf(stderr, "FAIL platform - no scratch directory for probe settings\n");
        return 2;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory.path());

    if (!mplayer::probes::selectRuntime()) {
        std::fprintf(stderr, "FAIL runtime - no mpv runtime selected for this probe\n");
        return 2;
    }

    auto session = std::make_unique<VideoSession>();
    Probe probe(options, std::move(session));
    return probe.run();
}
