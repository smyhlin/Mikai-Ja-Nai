#include "ClipController.hpp"
#include "ClipWindow.hpp"
#include "MpvVideoItem.hpp"
#include "PlayerController.hpp"
#include "PlayerWindow.hpp"
#include "ProbeRuntime.hpp"
#include "ShellTheme.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QInputMethodEvent>
#include <QPointer>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QSGRendererInterface>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>

#include <cmath>
#include <functional>
#include <memory>

using namespace mplayer;
using namespace mplayer::quick;

namespace {
bool waitFor(const std::function<bool()>& predicate, int timeout = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) QTest::qWait(20);
    return predicate();
}
QQuickItem* itemNamed(QQuickItem* root, const QString& name) {
    if (root == nullptr) return nullptr;
    if (root->objectName() == name) return root;
    for (auto* child : root->childItems()) {
        if (auto* found = itemNamed(child, name)) return found;
    }
    return nullptr;
}
QPoint centerOf(QQuickItem* item) {
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}
QQuickItem* findPickerItem(QQuickItem* root, const std::function<bool(QQuickItem*)>& matches) {
    if (root == nullptr) return nullptr;
    if (matches(root)) return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findPickerItem(child, matches)) return found;
    }
    return nullptr;
}

bool driveSavePicker(QQuickWindow* clip, QObject* saveDialog, const QString& path) {
    for (QWindow* candidate : QGuiApplication::allWindows()) {
        auto* window = qobject_cast<QQuickWindow*>(candidate);
        if (window == nullptr || window == clip || !window->isVisible()) continue;
        QQuickItem* root = window->contentItem();
        QQuickItem* field = findPickerItem(root, [](QQuickItem* item) {
            const QString name = item->objectName().toLower();
            const QString type = QString::fromLatin1(item->metaObject()->className()).toLower();
            return item->isVisible() && item->isEnabled() &&
                   (name.contains(QStringLiteral("filename")) ||
                    (type.contains(QStringLiteral("textfield")) &&
                     item->property("text").isValid()));
        });
        if (field == nullptr) continue;
        const QUrl folder = QUrl::fromLocalFile(QFileInfo(path).absolutePath());
        if (!saveDialog->setProperty("currentFolder", folder) ||
            saveDialog->property("currentFolder").toUrl() != folder) continue;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          field->mapToScene(QPointF(field->width() / 2, field->height() / 2)).toPoint());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QInputMethodEvent input;
        input.setCommitString(QFileInfo(path).fileName());
        QCoreApplication::sendEvent(window->focusObject(), &input);
        if (field->property("text").toString() != QFileInfo(path).fileName()) continue;
        QTest::keyClick(window, Qt::Key_Tab);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        QQuickItem* save = findPickerItem(root, [](QQuickItem* item) {
            const QString name = item->objectName().toLower();
            const QString text = item->property("text").toString().toLower();
            const QString type = QString::fromLatin1(item->metaObject()->className()).toLower();
            return item->isVisible() && item->isEnabled() &&
                   (name.contains(QStringLiteral("save")) ||
                    (type.contains(QStringLiteral("button")) &&
                     (text == QStringLiteral("save") || text == QStringLiteral("&save"))));
        });
        if (save == nullptr) continue;
        const QString selectedPath = saveDialog->property("selectedFile").toUrl().toLocalFile();
        if (QDir::cleanPath(selectedPath) != QDir::cleanPath(path)) continue;
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          save->mapToScene(QPointF(save->width() / 2, save->height() / 2)).toPoint());
        return true;
    }
    return false;
}
}

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    if (argc < 3) return 2;
    const QString media = QString::fromLocal8Bit(argv[1]);
    const QString output = QString::fromLocal8Bit(argv[2]);
    if (!QFileInfo(media).isFile() || !QFileInfo(output).isAbsolute() || !QDir().mkpath(output)) return 2;
    QTemporaryDir settings;
    if (!settings.isValid()) return 2;
    app.setOrganizationName(QStringLiteral("MikaiClipProbe"));
    app.setApplicationName(QStringLiteral("ClipProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    if (!probes::selectRuntime()) return 2;
    QJsonArray checks;
    QStringList qmlWarnings;
    int failures = 0;
    const auto require = [&](bool ok, const QString& name) {
        checks.append(QJsonObject{{"name", name}, {"passed", ok}});
        if (!ok) { ++failures; qCritical().noquote() << "FAIL" << name; }
        else qInfo().noquote() << "PASS" << name;
        return ok;
    };
    const auto publish = [&] {
        QFile result(QDir(output).filePath(QStringLiteral("result.json")));
        if (!result.open(QIODevice::WriteOnly)) return 3;
        result.write(QJsonDocument(QJsonObject{{"checks", checks}, {"failures", failures},
            {"platform", QGuiApplication::platformName()},
            {"qmlWarnings", QJsonArray::fromStringList(qmlWarnings)}}).toJson());
        return failures == 0 ? 0 : 1;
    };
    auto player = std::make_unique<PlayerController>(QStringLiteral("QML clip workflow"), media);
    auto* mainWindow = player->window();
    if (!require(mainWindow != nullptr, QStringLiteral("production player window exists"))) return publish();
    mainWindow->showWindowed();
    player->startPlayer();
    auto* source = player->session();
    if (!require(waitFor([&] { return source->hasVideo() && source->duration() > 2.0; }, 20000),
                 QStringLiteral("source has a real seekable video timeline"))) return publish();
    source->setPaused(true);
    const double anchor = std::min(3.0, source->duration() / 3.0);
    source->seekExact(anchor);
    if (!require(waitFor([&] { return std::abs(source->position() - anchor) < 0.2; }),
                 QStringLiteral("main player is paused at a nonzero anchor"))) return publish();
    player->openClipWindow();
    QPointer<ClipWindow> clip;
    if (!require(waitFor([&] {
        for (auto* window : QGuiApplication::topLevelWindows()) {
            if (auto* candidate = qobject_cast<ClipWindow*>(window); candidate && candidate->isVisible()) {
                clip = candidate;
                return true;
            }
        }
        return false;
    }), QStringLiteral("player action opens a real QQuickView clip window"))) return publish();
    auto* editor = clip->controller();
    QObject::connect(clip->engine(), &QQmlEngine::warnings, &app, [&](const QList<QQmlError>& warnings) {
        for (const auto& warning : warnings) qmlWarnings.append(warning.toString());
    });
    require(editor->session() != source, QStringLiteral("preview owns an independent playback session"));
    require(clip->transientParent() == mainWindow, QStringLiteral("clip window belongs to its player"));
    require(waitFor([&] { return !editor->previewSource().isEmpty(); }, 15000),
            QStringLiteral("opening preview contains a decoded still without starting playback"));
    require(waitFor([&] {
        for (int row = 0; row < editor->thumbnails()->rowCount(); ++row) {
            const auto value = editor->thumbnails()->data(editor->thumbnails()->index(row),
                                                         ClipThumbnailModel::ImageSourceRole);
            if (!value.toUrl().isEmpty()) return true;
        }
        return false;
    }), QStringLiteral("filmstrip displays decoded source frames"));
    const double mainPosition = source->position();
    auto* play = itemNamed(clip->rootObject(), QStringLiteral("clipPlay"));
    if (require(play != nullptr, QStringLiteral("preview transport is present"))) {
        QTest::mouseClick(clip, Qt::LeftButton, Qt::NoModifier, centerOf(play));
        require(waitFor([&] { return editor->videoVisible() && editor->position() > 0.1; }, 15000),
                QStringLiteral("QML transport presents real moving preview video"));
        auto* video = qobject_cast<MpvVideoItem*>(itemNamed(clip->rootObject(), QStringLiteral("clipPreviewVideo")));
        require(video != nullptr && !video->grabVideoFrame().isNull(),
                QStringLiteral("preview has its own readable rendered video output"));
        QTest::mouseClick(clip, Qt::LeftButton, Qt::NoModifier, centerOf(play));
        require(!editor->playing(), QStringLiteral("preview transport pauses independently"));
    }
    QTest::keyClick(clip, Qt::Key_M);
    require(editor->muted(), QStringLiteral("M mutes preview audio"));
    QTest::keyClick(clip, Qt::Key_M);
    require(!editor->muted(), QStringLiteral("M restores preview audio"));
    auto* preset = itemNamed(clip->rootObject(), QStringLiteral("clipPreset5"));
    if (require(preset != nullptr, QStringLiteral("duration presets are reachable"))) {
        QTest::mouseClick(clip, Qt::LeftButton, Qt::NoModifier, centerOf(preset));
        require(std::abs(editor->selectionDuration() - std::min(5.0, editor->windowEnd() - editor->windowStart())) < 0.01,
                QStringLiteral("duration preset changes the actual export interval"));
    }
    auto* grip = itemNamed(clip->rootObject(), QStringLiteral("clipMoveSelection"));
    if (require(grip != nullptr, QStringLiteral("short selections retain a separate span-drag grip"))) {
        const double length = editor->selectionDuration();
        const double start = editor->selectionStart();
        const QPoint from = centerOf(grip);
        const int direction = editor->selectionEnd() < editor->windowEnd() - 0.5 ? 1 : -1;
        QTest::mousePress(clip, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(clip, from + QPoint(direction * 45, 0), 50);
        QTest::mouseRelease(clip, Qt::LeftButton, Qt::NoModifier, from + QPoint(direction * 45, 0));
        require(std::abs(editor->selectionDuration() - length) < 0.01 &&
                std::abs(editor->selectionStart() - start) > 0.01,
                QStringLiteral("span dragging moves both boundaries without changing duration"));
    }
    const double rangeStart = editor->selectionStart();
    const double rangeEnd = editor->selectionEnd();
    const double outside = rangeStart > editor->windowStart() + 0.2
                               ? editor->windowStart() : editor->windowEnd();
    editor->seekTo(outside);
    require(editor->selectionStart() == rangeStart && editor->selectionEnd() == rangeEnd,
            QStringLiteral("scrubbing outside selection does not rewrite export boundaries"));
    require(source->playbackPauseIntent() && std::abs(source->position() - mainPosition) < 0.1,
            QStringLiteral("preview playback and editing never seek or unpause the main player"));
    const double originalRadius = shell::cornerRadius();
    shell::setCornerRadius(0);
    auto* previewCard = itemNamed(clip->rootObject(), QStringLiteral("clipPreviewCard"));
    require(previewCard && previewCard->property("radius").toDouble() == 0.0,
            QStringLiteral("zero global radius remains square in the clip editor"));
    shell::setCornerRadius(12);
    require(previewCard && previewCard->property("radius").toDouble() == 12.0,
            QStringLiteral("live global radius updates the existing clip editor"));
    shell::setCornerRadius(originalRadius);
    clip->resize(760, 680);
    QTest::qWait(150);
    require(clip->grabWindow().save(QDir(output).filePath(QStringLiteral("clip-compact.png"))),
            QStringLiteral("compact editor screenshot saved"));
    clip->resize(1060, 820);
    QTest::qWait(150);
    require(clip->grabWindow().save(QDir(output).filePath(QStringLiteral("clip-wide.png"))),
            QStringLiteral("wide editor screenshot saved"));
    if (argc > 3) {
        QTranslator translator;
        require(translator.load(QString::fromLocal8Bit(argv[3])), QStringLiteral("real Ukrainian catalogue loads"));
        app.installTranslator(&translator);
        QTest::qWait(100);
        require(editor->selectionStart() == rangeStart && editor->selectionEnd() == rangeEnd,
                QStringLiteral("retranslation preserves the edited interval"));
        require(clip->grabWindow().save(QDir(output).filePath(QStringLiteral("clip-uk.png"))),
                QStringLiteral("Ukrainian editor screenshot saved"));
        app.removeTranslator(&translator);
    }
    auto* saveDialog = clip->rootObject()->findChild<QObject*>(QStringLiteral("clipSaveDialog"));
    auto* exportButton = itemNamed(clip->rootObject(), QStringLiteral("clipExport"));
    if (require(saveDialog && exportButton, QStringLiteral("QML export action and file picker exist"))) {
        const QString sentinelPath = QDir(output).filePath(QStringLiteral("existing-%1.mp4").arg(app.applicationPid()));
        QFile sentinel(sentinelPath);
        require(sentinel.open(QIODevice::WriteOnly) && sentinel.write("keep existing output") > 0,
                QStringLiteral("existing destination fixture created"));
        sentinel.close();
        QTest::mouseClick(clip, Qt::LeftButton, Qt::NoModifier, centerOf(exportButton));
        require(waitFor([&] { return editor->choosingOutput() && saveDialog->property("visible").toBool(); }),
                QStringLiteral("export action opens the asynchronous QML file picker"));
        if (!require(driveSavePicker(clip, saveDialog, sentinelPath),
                     QStringLiteral("fallback picker filename field and Save button select the owned fixture"))) {
            QMetaObject::invokeMethod(saveDialog, "reject");
            return publish();
        }
        if (!require(waitFor([&] { return editor->overwritePending(); }) &&
                         editor->overwriteFileName() == QFileInfo(sentinelPath).fileName(),
                     QStringLiteral("overwrite confirmation targets the owned fixture, not the suggested path")))
            return publish();
        auto* replace = clip->rootObject()->findChild<QObject*>(QStringLiteral("clipReplaceDialog"));
        if (require(replace != nullptr, QStringLiteral("replacement confirmation is a QML dialog"))) {
            require(QMetaObject::invokeMethod(replace, "reject"), QStringLiteral("replacement refusal is delivered"));
            require(waitFor([&] { return !editor->overwritePending(); }), QStringLiteral("replacement refusal returns to editing"));
        }
        require(sentinel.open(QIODevice::ReadOnly) && sentinel.readAll() == QByteArrayLiteral("keep existing output"),
                QStringLiteral("declining overwrite leaves destination bytes unchanged"));
        sentinel.close();
        const QString exported = QDir(output).filePath(QStringLiteral("exported-%1.mp4").arg(app.applicationPid()));
        QTest::mouseClick(clip, Qt::LeftButton, Qt::NoModifier, centerOf(exportButton));
        require(waitFor([&] { return editor->choosingOutput(); }), QStringLiteral("export can be requested after refusal"));
        if (!require(driveSavePicker(clip, saveDialog, exported),
                     QStringLiteral("fallback picker filename field and Save button select the owned export path"))) {
            QMetaObject::invokeMethod(saveDialog, "reject");
            return publish();
        }
        require(waitFor([&] { return editor->saved() || editor->failed(); }, 60000) && editor->saved(),
                QStringLiteral("selected interval exports successfully through the real encoder: %1").arg(editor->statusText()));
        require(QFileInfo(exported).size() > 1024, QStringLiteral("exported MP4 contains encoded media"));
        require(editor->completedPath().toLocalFile() == exported,
                QStringLiteral("completed export path is the requested owned fixture path"));
        editor->copyOutputPath();
        require(QGuiApplication::clipboard()->text() == exported, QStringLiteral("completed export exposes its actual path"));
        require(clip->grabWindow().save(QDir(output).filePath(QStringLiteral("clip-exported.png"))),
                QStringLiteral("completed export screenshot saved"));
    }
    require(qmlWarnings.isEmpty(), QStringLiteral("clip scene emitted no QML warnings"));
    source->open(media);
    require(waitFor([&] { return clip.isNull(); }), QStringLiteral("source replacement retires its clip window and preview"));
    require(mainWindow->isVisible(), QStringLiteral("closing the clip leaves the player window open"));
    player.reset();
    return publish();
}
