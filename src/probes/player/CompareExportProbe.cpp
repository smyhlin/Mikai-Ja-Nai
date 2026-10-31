#include "CompareExportDialog.hpp"
#include "CompareExportComplete.hpp"
#include "CompareWindow.hpp"
#include "PlayerDialog.hpp"
#include "ProbeIsolation.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSurface.hpp"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QImageReader>
#include <QLineEdit>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
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

QDialog* dialogNamed(const QString& title) {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* dialog = qobject_cast<QDialog*>(widget);
        if (dialog != nullptr && dialog->windowTitle() == title && dialog->isVisible())
            return dialog;
    }
    return nullptr;
}
class ClipboardRestore final {
public:
    explicit ClipboardRestore(QClipboard* clipboard) : clipboard_(clipboard), saved_(new QMimeData) {
        const QMimeData* original = clipboard_->mimeData();
        if (original == nullptr) return;
        for (const QString& format : original->formats())
            saved_->setData(format, original->data(format));
    }

    ~ClipboardRestore() { clipboard_->setMimeData(saved_); }

    ClipboardRestore(const ClipboardRestore&) = delete;
    ClipboardRestore& operator=(const ClipboardRestore&) = delete;

private:
    QClipboard* clipboard_;
    QMimeData* saved_;
};

bool exportThroughDialog(mplayer::shell::CompareWindow* compare, int formatIndex,
                         const QString& outputDirectory, const QString& fileName,
                         const QString& destination, bool verifyAutoDismiss = false) {
    auto* exportButton = compare->findChild<QPushButton*>(QStringLiteral("compareExport"));
    if (exportButton == nullptr || !exportButton->isEnabled()) return false;

    bool exportDialogSeen = false;
    bool controlsFound = false;
    bool browseClicked = false;
    bool chooserSeen = false;
    bool directorySelected = false;
    bool directoryMatches = false;
    bool saveClicked = false;
    QTimer driver;
    driver.setInterval(20);
    QObject::connect(&driver, &QTimer::timeout, compare, [&] {
        QDialog* dialog = dialogNamed(QStringLiteral("Export comparison"));
        if (dialog == nullptr) return;
        exportDialogSeen = true;
        auto* format = dialog->findChild<QComboBox*>(QStringLiteral("compareExportFormat"));
        auto* directory = dialog->findChild<QLineEdit*>(QStringLiteral("compareExportDirectory"));
        auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("compareExportFileName"));
        auto* browse = dialog->findChild<QPushButton*>(QStringLiteral("compareExportBrowse"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (format == nullptr || directory == nullptr || name == nullptr ||
            browse == nullptr || buttons == nullptr) return;
        controlsFound = true;
        format->setCurrentIndex(formatIndex);
        name->setText(fileName);
        directoryMatches = directory->text() == outputDirectory;
        if (!browseClicked && !directoryMatches) {
            browseClicked = true;
            auto* chooserDriver = new QTimer(compare);
            chooserDriver->setInterval(20);
            QObject::connect(chooserDriver, &QTimer::timeout, compare,
                [chooserDriver, compare, outputDirectory, &chooserSeen, &directorySelected] {
                    for (QWidget* widget : QApplication::topLevelWidgets()) {
                        auto* chooser = qobject_cast<QFileDialog*>(widget);
                        if (chooser == nullptr || !chooser->isVisible()) continue;
                        chooserSeen = true;
                        chooser->setDirectory(outputDirectory);
                        QMetaObject::invokeMethod(chooser, "accept", Qt::QueuedConnection);
                        directorySelected = true;
                        chooserDriver->stop();
                        chooserDriver->deleteLater();
                        return;
                    }
                });
            chooserDriver->start();
            browse->click();
            return;
        }
        if (!saveClicked && directoryMatches) {
            auto* save = buttons->button(QDialogButtonBox::Save);
            if (save == nullptr) return;
            saveClicked = true;
            save->click();
        }
    });
    driver.start();
    exportButton->click();
    const bool popupShown = waitUntil([&] {
        return dialogNamed(QStringLiteral("Export complete")) != nullptr;
    }, 30000);
    driver.stop();
    auto* popup = qobject_cast<mplayer::shell::CompareExportComplete*>(
        dialogNamed(QStringLiteral("Export complete")));
    if (!popupShown || popup == nullptr || !QFile::exists(destination)) {
        std::fprintf(stderr,
            "Export UI format=%d dialogSeen=%d controlsFound=%d browseClicked=%d chooserSeen=%d "
            "directorySelected=%d directoryMatches=%d saveClicked=%d popup=%d destination=%d "
            "output=%s destinationPath=%s\\n",
            formatIndex, exportDialogSeen, controlsFound, browseClicked, chooserSeen,
            directorySelected, directoryMatches, saveClicked, popupShown,
            QFile::exists(destination), qPrintable(outputDirectory), qPrintable(destination));
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->isVisible())
                std::fprintf(stderr, "Visible window: title=%s class=%s\\n",
                    qPrintable(widget->windowTitle()), widget->metaObject()->className());
        }
        return false;
    }
    const auto buttons = popup->findChildren<QPushButton*>();
    bool openFile = false;
    bool openFolder = false;
    bool copyFile = false;
    for (const auto* button : buttons) {
        openFile |= button->text() == QStringLiteral("Open file") && button->isVisible() && button->isEnabled();
        openFolder |= button->text() == QStringLiteral("Open folder") && button->isVisible() && button->isEnabled();
        copyFile |= button->text() == QStringLiteral("Copy file") && button->isVisible() && button->isEnabled();
    }
    popup->grab().save(QDir(outputDirectory).filePath(
        QFileInfo(destination).completeBaseName() + QStringLiteral("-complete.png")));
    const bool controlsPresent = openFile && openFolder && copyFile;
    if (!(directorySelected || directoryMatches) || !controlsPresent) {
        std::fprintf(stderr,
            "Export completion format=%d directorySelected=%d directoryMatches=%d "
            "openFile=%d openFolder=%d copyFile=%d\\n",
            formatIndex, directorySelected, directoryMatches, openFile, openFolder, copyFile);
    }
    bool copiedUrl = false;
    bool dismissed = false;
    if (controlsPresent) {
        ClipboardRestore restore(QApplication::clipboard());
        QPushButton* copyButton = nullptr;
        for (auto* button : buttons) {
            if (button->text() == QStringLiteral("Copy file")) {
                copyButton = button;
                break;
            }
        }
        if (copyButton != nullptr) {
            QPointer<mplayer::shell::CompareExportComplete> guardedPopup(popup);
            copyButton->click();
            copiedUrl = waitUntil([&] {
                const QMimeData* data = QApplication::clipboard()->mimeData();
                if (data == nullptr || !data->hasUrls()) return false;
                const QString expected = QFileInfo(destination).absoluteFilePath();
                for (const QUrl& url : data->urls()) {
                    if (url.isLocalFile() &&
                        QFileInfo(url.toLocalFile()).absoluteFilePath() == expected)
                        return true;
                }
                return false;
            }, 1000);
            if (verifyAutoDismiss) {
                dismissed = waitUntil([&] {
                    return guardedPopup.isNull() || !guardedPopup->isVisible();
                }, 7000);
            }
        }
    }
    if (!verifyAutoDismiss && popup->isVisible()) popup->close();
    QTest::qWait(20);
    const bool complete = (directorySelected || directoryMatches) && controlsPresent &&
                          copiedUrl && (!verifyAutoDismiss || dismissed);
    if (!complete) {
        std::fprintf(stderr,
            "Export completion format=%d directorySelected=%d directoryMatches=%d "
            "copyUrl=%d autoDismiss=%d\\n",
            formatIndex, directorySelected, directoryMatches, copiedUrl, dismissed);
    }
    return complete;
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
    const QString output = QDir(QString::fromLocal8Bit(argv[2])).absolutePath();
    if (!QDir().mkpath(output)) return 2;
    for (const QString& extension : {QStringLiteral("gif"), QStringLiteral("mp4"),
                                     QStringLiteral("webp")}) {
        if (QFile::exists(QDir(output).filePath(QStringLiteral("comparison.") + extension))) {
            std::fprintf(stderr, "Refusing to overwrite existing probe evidence\n");
            return 2;
        }
    }
    const QString destination = QDir(output).filePath(QStringLiteral("comparison.gif"));
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
    auto* exportControl = compare->findChild<QPushButton*>(QStringLiteral("compareExport"));
    auto* close = compare->findChild<QToolButton*>(QStringLiteral("compareClose"));
    if (mode == nullptr || exportControl == nullptr || close == nullptr) return 1;
    mode->setCurrentIndex(mode->findData(QStringLiteral("flicker")));
    if (!waitUntil([&] { return exportControl->isVisible() && exportControl->isEnabled(); }, 15000)) return 1;
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
    if (!exportThroughDialog(compare, 0, output, QStringLiteral("comparison.gif"), destination)) {
        std::fprintf(stderr, "GIF export dialog or completion controls failed\n");
        return 1;
    }
    QImageReader reader(destination);
    const int count = reader.imageCount();
    const QImage a = reader.read();
    const int delayA = reader.nextImageDelay();
    const QImage b = reader.read();
    const int delayB = reader.nextImageDelay();
    const bool gifPassed = layoutsFit && close->text() == QStringLiteral("Close") &&
        count == 2 && a.size() == expected && b.size() == expected &&
        delayA == 1000 && delayB == 1000 && reader.loopCount() == -1;
    const QString mp4Path = QDir(output).filePath(QStringLiteral("comparison.mp4"));
    const QString webpPath = QDir(output).filePath(QStringLiteral("comparison.webp"));
    const bool mp4Passed = exportThroughDialog(compare, 1, output, QStringLiteral("comparison.mp4"), mp4Path) &&
        QFileInfo(mp4Path).size() > 0;
    const bool webpPassed = exportThroughDialog(compare, 2, output, QStringLiteral("comparison.webp"),
                                                 webpPath, true) &&
        QFileInfo(webpPath).size() > 0;
    const bool passed = gifPassed && mp4Passed && webpPassed;
    close->click();
    const bool closed = !compare->isVisible();
    compare->show();
    auto* exportButton = compare->findChild<QPushButton*>(QStringLiteral("compareExport"));
    if (exportButton == nullptr || !exportButton->isEnabled()) return 1;
    bool pickerWasOpen = false;
    QTimer::singleShot(100, &app, [&] {
        QDialog* dialog = dialogNamed(QStringLiteral("Export comparison"));
        auto* browse = dialog == nullptr ? nullptr
            : dialog->findChild<QPushButton*>(QStringLiteral("compareExportBrowse"));
        if (browse == nullptr) return;
        QTimer::singleShot(100, &app, [&] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* picker = qobject_cast<QFileDialog*>(widget);
                if (picker != nullptr && picker->isVisible()) pickerWasOpen = true;
            }
            if (player) player->close();
        });
        browse->click();
    });
    exportButton->click();
    const bool ownerClosed = waitUntil([&] { return player.isNull(); }, 15000);
    const bool lifetimeSafe = pickerWasOpen && ownerClosed;
    std::fprintf(stderr, "Compare export: %s; frames=%d canvas=%dx%d delays=%d/%d close=%d layout=%d pickerTeardown=%d\n",
                 passed && closed && lifetimeSafe ? "PASS" : "FAIL", count, a.width(), a.height(),
                 delayA, delayB, closed, layoutsFit, lifetimeSafe);
    return passed && closed && lifetimeSafe ? 0 : 1;
}
