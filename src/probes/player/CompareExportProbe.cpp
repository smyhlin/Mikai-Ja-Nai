#include "CompareWindow.hpp"
#include "PlayerDialog.hpp"
#include "ProbeIsolation.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSurface.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFile>
#include <QFrame>
#include <QImageReader>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest/QTest>

#include <algorithm>
#include <cstdio>
#include <functional>

namespace {

class PlaybackHost final : public mplayer::shell::PlayerHost {
public:
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int) override {}
    void requestTrackProbe(const QString&) override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override {}
    void playbackFailed() override {}
};

bool waitUntil(const std::function<bool()>& ready, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!ready() && clock.elapsed() < timeoutMs) QTest::qWait(20);
    return ready();
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const auto isolation = mikai::probes::isolation::enter(
        argc, argv, {"compare_export_probe", false, 180000});
    if (isolation != mikai::probes::isolation::Gate::RunHere)
        return mikai::probes::isolation::exitCode();
    if (argc != 3) return 2;
    QTemporaryDir settings;
    if (!settings.isValid()) return 2;
    app.setOrganizationName(QStringLiteral("MikaiCompareExportProbe"));
    app.setApplicationName(QStringLiteral("CompareExportProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    const auto runtime = mplayer::native::selectStandardRuntime(
        qEnvironmentVariable("MIKAI_MPV_MODULE"));
    if (!runtime.loaded) {
        std::fprintf(stderr, "Runtime refused: %s\n", qPrintable(runtime.reason));
        return 2;
    }
    const QString media = QString::fromLocal8Bit(argv[1]);
    const QString output = QString::fromLocal8Bit(argv[2]);
    if (!QDir().mkpath(output)) return 2;
    const QString destination = QDir(output).filePath(QStringLiteral("comparison.gif"));
    if (QFile::exists(destination)) {
        std::fprintf(stderr, "Refusing to overwrite existing probe evidence\n");
        return 2;
    }
    PlaybackHost host;
    QPointer<mplayer::shell::PlayerDialog> player = new mplayer::shell::PlayerDialog(
        QStringLiteral("Compare export smoke"), media, {{1080, media}}, 1080, &host);
    auto* surface = player->findChild<mplayer::VideoSurface*>();
    player->startPlayer();
    player->show();
    if (!waitUntil([&] { return surface->hasVideo() && surface->position() > 0.5; }, 15000)) return 1;
    auto* compare = new mplayer::shell::CompareWindow(surface, QStringLiteral("off"), player);
    compare->show();
    if (!waitUntil([&] {
            return !compare->comparisonPending() && !compare->renderedSideA().isNull() &&
                   !compare->renderedSideB().isNull();
        }, 30000)) {
        std::fprintf(stderr, "No settled comparison pair: %s\n", qPrintable(compare->captureError()));
        return 1;
    }
    auto* mode = compare->findChild<QComboBox*>(QStringLiteral("compareMode"));
    auto* save = compare->findChild<QPushButton*>(QStringLiteral("compareSaveGif"));
    auto* close = compare->findChild<QToolButton*>(QStringLiteral("compareClose"));
    if (mode == nullptr || save == nullptr || close == nullptr) return 1;
    mode->setCurrentIndex(mode->findData(QStringLiteral("flicker")));
    if (!waitUntil([&] { return save->isVisible() && save->isEnabled(); }, 15000)) return 1;
    const QSize expected(std::max(compare->renderedSideA().width(), compare->renderedSideB().width()),
                         std::max(compare->renderedSideA().height(), compare->renderedSideB().height()));
    bool layoutsFit = true;
    for (const int width : {960, 1600}) {
        compare->resize(width, 760);
        QTest::qWait(100);
        auto* toolbar = compare->findChild<QFrame*>(QStringLiteral("compareToolbarCard"));
        for (auto* control : toolbar->findChildren<QWidget*>()) {
            if (!control->isVisible() || !control->isEnabled()) continue;
            const QRect bounds(control->mapTo(toolbar, QPoint()), control->size());
            if (!toolbar->rect().contains(bounds)) {
                layoutsFit = false;
                std::fprintf(stderr, "Clipped control: %s at width %d\n", qPrintable(control->objectName()), width);
            }
        }
        compare->grab().save(QDir(output).filePath(QStringLiteral("compare-%1.png").arg(width)));
    }
    QTimer pickerDriver;
    pickerDriver.setInterval(20);
    QObject::connect(&pickerDriver, &QTimer::timeout, compare, [&] {
        auto* picker = compare->findChild<QFileDialog*>();
        if (picker == nullptr || !picker->isVisible()) return;
        picker->selectFile(destination);
        QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection);
    });
    pickerDriver.start();
    save->click();
    pickerDriver.stop();
    if (!waitUntil([&] { return QFile::exists(destination) && save->isEnabled(); }, 15000)) {
        std::fprintf(stderr, "Actual Save GIF action produced no file\n");
        return 1;
    }
    QImageReader reader(destination);
    const int count = reader.imageCount();
    const QImage a = reader.read();
    const int delayA = reader.nextImageDelay();
    const QImage b = reader.read();
    const int delayB = reader.nextImageDelay();
    const bool passed = layoutsFit && close->text() == QStringLiteral("Close") &&
        count == 2 && a.size() == expected && b.size() == expected &&
        delayA == 1000 && delayB == 1000 && reader.loopCount() == -1;
    close->click();
    const bool closed = !compare->isVisible();
    compare->show();
    if (!waitUntil([&] { return save->isVisible() && save->isEnabled(); }, 15000)) return 1;
    bool pickerWasOpen = false;
    QTimer::singleShot(100, &app, [&] {
        auto* picker = compare->findChild<QFileDialog*>();
        pickerWasOpen = picker != nullptr && picker->isVisible();
        if (player) player->close();
    });
    save->click();
    const bool ownerClosed = waitUntil([&] { return player.isNull(); }, 15000);
    const bool lifetimeSafe = pickerWasOpen && ownerClosed;
    std::fprintf(stderr, "Compare export: %s; frames=%d canvas=%dx%d delays=%d/%d close=%d layout=%d pickerTeardown=%d\n",
                 passed && closed && lifetimeSafe ? "PASS" : "FAIL", count, a.width(), a.height(),
                 delayA, delayB, closed, layoutsFit, lifetimeSafe);
    return passed && closed && lifetimeSafe ? 0 : 1;
}
