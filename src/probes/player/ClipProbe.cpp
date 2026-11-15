#include "ClipController.hpp"
#include "ClipWindow.hpp"
#include "MpvVideoItem.hpp"
#include "PlayerController.hpp"
#include "PlayerWindow.hpp"
#include "ProbeRuntime.hpp"
#include "ShellTheme.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
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
#include <QScreen>
#include <QSettings>
#include <QSGRendererInterface>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>

#include <algorithm>
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
QQuickItem* topmostItemAt(QQuickItem* root, const QPointF& scenePoint) {
    if (root == nullptr || !root->isVisible()) return nullptr;
    const auto children = root->childItems();
    for (auto it = children.crbegin(); it != children.crend(); ++it) {
        if (QQuickItem* found = topmostItemAt(*it, scenePoint)) return found;
    }
    return root->contains(root->mapFromScene(scenePoint)) ? root : nullptr;
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
    QJsonObject estimateResult;
    QJsonObject tooltipDiagnostics;
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
            {"platform", QGuiApplication::platformName()}, {"clipSizeEstimate", estimateResult},
            {"tooltipDiagnostics", tooltipDiagnostics},
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
    const auto waitForEstimatePresentation = [&] {
        return waitFor([&] {
            if (!editor->estimatedSizeReady() || editor->estimatedSize().isEmpty()) return false;
            QQuickItem* label = itemNamed(clip->rootObject(), QStringLiteral("clipEstimatedSize"));
            return label != nullptr && label->isVisible() &&
                   label->property("text").toString() == editor->estimatedSize();
        }, 30000);
    };
    if (require(waitFor([&] { return editor->estimatedSizeReady(); }, 30000),
                QStringLiteral("initial media analysis completes with known video metadata"))) {
        const QString initialEstimate = editor->estimatedSize();
        const QString initialDetails = editor->estimatedSizeDetails();
        estimateResult = QJsonObject{{"ready", editor->estimatedSizeReady()},
                                     {"estimate", initialEstimate},
                                     {"details", initialDetails},
                                     {"qualityHeight", editor->qualityHeight()}};
        QQuickItem* estimateLabel = itemNamed(clip->rootObject(), QStringLiteral("clipEstimatedSize"));
        require(!initialEstimate.isEmpty() && !initialDetails.isEmpty() &&
                    estimateLabel != nullptr && estimateLabel->isVisible() &&
                    estimateLabel->property("text").toString() == initialEstimate,
                QStringLiteral("completed estimate is visible in the real clip window"));
        require(clip->grabWindow().save(QDir(output).filePath(QStringLiteral("clip-estimate.png"))),
                QStringLiteral("completed estimate screenshot saved"));
        const bool initiallyMuted = editor->muted();
        editor->toggleMute();
        require(editor->muted() != initiallyMuted &&
                    editor->estimatedSize() == initialEstimate,
                QStringLiteral("preview mute does not change the export-size estimate"));
        editor->toggleMute();
        const int previousQuality = editor->qualityHeight();
        const int changedQuality = previousQuality == 1080 ? 720 : 1080;
        const bool qualityChangesGeometry = source->videoHeight() > std::min(previousQuality, changedQuality);
        editor->setQualityHeight(changedQuality);
        require(waitFor([&] {
            return editor->estimatedSizeReady() &&
                   (qualityChangesGeometry ? editor->estimatedSize() != initialEstimate
                                           : editor->estimatedSize() == initialEstimate);
        }, 30000) && editor->qualityHeight() == changedQuality,
                qualityChangesGeometry
                    ? QStringLiteral("output quality changes the displayed size estimate")
                    : QStringLiteral("non-upscaled output quality preserves the estimate"));
        estimateResult.insert(QStringLiteral("changedQualityHeight"), editor->qualityHeight());
        estimateResult.insert(QStringLiteral("changedEstimate"), editor->estimatedSize());
        estimateResult.insert(QStringLiteral("changedDetails"), editor->estimatedSizeDetails());
        require(estimateLabel != nullptr &&
                    estimateLabel->property("text").toString() == editor->estimatedSize(),
                QStringLiteral("updated quality estimate reaches the visible label"));
        editor->setQualityHeight(1080);
        require(waitFor([&] {
            return editor->estimatedSizeReady() && editor->estimatedSize() == initialEstimate;
        }, 30000) && editor->qualityHeight() == 1080,
                QStringLiteral("restoring output quality restores the estimate"));
        estimateResult.insert(QStringLiteral("restoredEstimate"), editor->estimatedSize());
    }
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
    const double originalSelectionStart = editor->selectionStart();
    const double originalSelectionEnd = editor->selectionEnd();
    editor->setSelection(0.2, 0.3);
    require(std::abs(editor->selectionStart() - 0.2) < 0.01 &&
                std::abs(editor->selectionEnd() - 0.3) < 0.01,
            QStringLiteral("a 100 ms interval between thumbnail buckets is accepted"));
    const bool shortEstimateReady = waitForEstimatePresentation();
    require(shortEstimateReady,
            QStringLiteral("a 100 ms interval between cached thumbnail buckets has a ready visible estimate"));
    estimateResult.insert(QStringLiteral("shortInterval"), QJsonObject{
        {"start", editor->selectionStart()}, {"end", editor->selectionEnd()},
        {"ready", editor->estimatedSizeReady()}, {"estimate", editor->estimatedSize()},
        {"details", editor->estimatedSizeDetails()}});
    editor->setSelection(originalSelectionStart, originalSelectionEnd);
    require(waitForEstimatePresentation(),
            QStringLiteral("restoring the original interval restores a ready visible estimate"));
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
        const double oldDuration = editor->selectionDuration();
        const QString oldEstimate = editor->estimatedSize();
        QTest::mouseClick(clip, Qt::LeftButton, Qt::NoModifier, centerOf(preset));
        require(std::abs(editor->selectionDuration() - std::min(5.0, editor->windowEnd() - editor->windowStart())) < 0.01,
                QStringLiteral("duration preset changes the actual export interval"));
        if (std::abs(editor->selectionDuration() - oldDuration) > 0.01) {
            require(waitFor([&] {
                return editor->estimatedSizeReady() && editor->estimatedSize() != oldEstimate;
            }, 30000), QStringLiteral("duration changes the displayed size estimate"));
            QQuickItem* estimateLabel = itemNamed(clip->rootObject(), QStringLiteral("clipEstimatedSize"));
            require(estimateLabel != nullptr &&
                        estimateLabel->property("text").toString() == editor->estimatedSize(),
                    QStringLiteral("duration estimate reaches the visible label"));
        }
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
    if (!require(waitForEstimatePresentation(),
                 QStringLiteral("estimate is ready and visible after range movement and scrubbing")))
        return publish();
    require(waitFor([&] { return source->mediaTracks().ready; }, 10000),
            QStringLiteral("source media-track metadata is ready for the audio estimate check"));
    const auto originalAudioTrack = source->mediaTrackSelection().audio;
    const auto& sourceTracks = source->mediaTracks();
    if (sourceTracks.ready && originalAudioTrack && *originalAudioTrack > 0) {
        const QString beforeAudioOff = editor->estimatedSize();
        QJsonObject audioEstimate{{"readyBefore", editor->estimatedSizeReady()},
                                  {"estimateBefore", beforeAudioOff},
                                  {"selectedAudioTrack", qlonglong(*originalAudioTrack)}};
        const bool turnedOff = source->selectMediaTrack(MediaTrackType::Audio, 0,
                                                        sourceTracks.coreIdentity,
                                                        sourceTracks.sourceEpoch);
        require(turnedOff, QStringLiteral("source audio can be disabled through its public track API"));
        if (turnedOff) {
            require(waitFor([&] {
                return clip.isNull() || (source->mediaTrackSelection().audio.value_or(0) == 0 &&
                       editor->estimatedSizeReady() && editor->estimatedSize() != beforeAudioOff);
            }, 30000), QStringLiteral("export-audio-off changes the completed size estimate"));
            if (clip.isNull()) {
                require(false, QStringLiteral("audio selection changes preserve the clip editor"));
                return publish();
            }
            audioEstimate.insert(QStringLiteral("audioOffEstimate"), editor->estimatedSize());
            const auto& offTracks = source->mediaTracks();
            const bool restored = source->selectMediaTrack(MediaTrackType::Audio,
                                                            *originalAudioTrack,
                                                            offTracks.coreIdentity,
                                                            offTracks.sourceEpoch);
            require(restored, QStringLiteral("source audio track can be restored"));
            if (restored) {
                require(waitFor([&] {
                    return clip.isNull() || (source->mediaTrackSelection().audio == originalAudioTrack &&
                           editor->estimatedSizeReady() &&
                           editor->estimatedSize() == beforeAudioOff);
                }, 30000), QStringLiteral("restoring export audio restores the estimate"));
                if (clip.isNull()) {
                    require(false, QStringLiteral("audio restoration preserves the clip editor"));
                    return publish();
                }
                audioEstimate.insert(QStringLiteral("restoredEstimate"), editor->estimatedSize());
            }
        }
        estimateResult.insert(QStringLiteral("audioSelection"), audioEstimate);
        require(!clip.isNull(), QStringLiteral("audio selection changes preserve the clip editor"));
    }
    require(source->playbackPauseIntent() && std::abs(source->position() - mainPosition) < 0.1,
            QStringLiteral("preview playback and editing never seek or unpause the main player"));
    auto* moveGrip = itemNamed(clip->rootObject(), QStringLiteral("clipMoveSelection"));
    QScreen* screen = clip->screen() != nullptr ? clip->screen() : QGuiApplication::primaryScreen();
    const QRect available = screen != nullptr ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    const auto fitWindow = [&](int width, int height) {
        const QSize maximum(std::max(320, available.width() - 48),
                            std::max(320, available.height() - 112));
        clip->resize(std::min(width, maximum.width()), std::min(height, maximum.height()));
        clip->setPosition(available.topLeft() + QPoint(16, 16));
        QTest::qWait(100);
        const QRect frame = clip->frameGeometry();
        int dx = 0;
        int dy = 0;
        if (frame.right() > available.right() - 8) dx = available.right() - 8 - frame.right();
        if (frame.left() < available.left() + 8) dx = available.left() + 8 - frame.left();
        if (frame.bottom() > available.bottom() - 8) dy = available.bottom() - 8 - frame.bottom();
        if (frame.top() < available.top() + 8) dy = available.top() + 8 - frame.top();
        if (dx != 0 || dy != 0) clip->setPosition(clip->position() + QPoint(dx, dy));
    };
    fitWindow(1060, 820);
    auto* selectedQuality = itemNamed(
        clip->rootObject(), QStringLiteral("clipQuality%1").arg(editor->qualityHeight()));
    if (require(selectedQuality != nullptr && selectedQuality->isVisible() &&
                    selectedQuality->isEnabled(),
                QStringLiteral("selected output-quality button is available to transfer focus"))) {
        QTest::mouseClick(clip, Qt::LeftButton, Qt::NoModifier, centerOf(selectedQuality));
        require(moveGrip == nullptr || !moveGrip->property("activeFocus").toBool(),
                QStringLiteral("clicking selected output quality naturally moves focus off the span grip"));
    }
    const double originalRadius = shell::cornerRadius();
    shell::setCornerRadius(0);
    auto* previewCard = itemNamed(clip->rootObject(), QStringLiteral("clipPreviewCard"));
    require(previewCard && previewCard->property("radius").toDouble() == 0.0,
            QStringLiteral("zero global radius remains square in the clip editor"));
    shell::setCornerRadius(12);
    require(previewCard && previewCard->property("radius").toDouble() == 12.0,
            QStringLiteral("live global radius updates the existing clip editor"));
    shell::setCornerRadius(originalRadius);
    if (!require(waitForEstimatePresentation(),
                 QStringLiteral("estimate is ready and visible before layout screenshots")))
        return publish();
    fitWindow(760, 680);
    QTest::qWait(150);
    require(clip->grabWindow().save(QDir(output).filePath(QStringLiteral("clip-compact.png"))),
            QStringLiteral("compact editor screenshot saved"));
    fitWindow(1060, 820);
    QTest::qWait(150);
    require(clip->grabWindow().save(QDir(output).filePath(QStringLiteral("clip-wide.png"))),
            QStringLiteral("wide editor screenshot saved"));
    QQuickItem* estimateLabel = itemNamed(clip->rootObject(), QStringLiteral("clipEstimatedSize"));
    if (require(estimateLabel != nullptr && estimateLabel->isVisible(),
                QStringLiteral("estimate label is available for its details tooltip"))) {
        clip->raise();
        clip->requestActivate();
        QCoreApplication::processEvents();
        const double renderedTextWidth = estimateLabel->property("contentWidth").toDouble();
        const QPointF labelTextPoint = estimateLabel->mapToScene(
            QPointF(std::min(renderedTextWidth / 2.0, estimateLabel->width() / 2.0),
                    estimateLabel->height() / 2.0));
        const QPoint labelGlobalCenter = clip->mapToGlobal(labelTextPoint.toPoint());
        QCursor::setPos(labelGlobalCenter);
        QTest::qWait(100);
        QTest::mouseMove(clip, labelTextPoint.toPoint());
        QTest::qWait(50);
        const bool pointerReachedLabel = QCursor::pos() == labelGlobalCenter;
        require(pointerReachedLabel, QStringLiteral("native pointer reaches the estimate label"));
        QObject* hoverHandler = nullptr;
        for (QObject* child : estimateLabel->findChildren<QObject*>()) {
            if (child->metaObject()->indexOfProperty("hovered") >= 0) {
                hoverHandler = child;
                break;
            }
        }
        bool qualityButtonHovered = false;
        if (hoverHandler == nullptr || !hoverHandler->property("hovered").toBool()) {
            QQuickItem* hoverButton = itemNamed(clip->rootObject(),
                QStringLiteral("clipQuality%1").arg(editor->qualityHeight()));
            if (hoverButton != nullptr) {
                const QPoint buttonCenter = centerOf(hoverButton);
                QCursor::setPos(clip->mapToGlobal(buttonCenter));
                QTest::mouseMove(clip, buttonCenter);
                QTest::qWait(50);
                qualityButtonHovered = hoverButton->property("hovered").toBool();
                QCursor::setPos(labelGlobalCenter);
                QTest::mouseMove(clip, labelTextPoint.toPoint());
                QTest::qWait(50);
            }
        }
        const QString tooltipDetails = editor->estimatedSizeDetails();
        QObject* tooltipObject =
            clip->rootObject()->findChild<QObject*>(QStringLiteral("clipEstimateDetailsTip"));
        QQuickItem* tooltipContent =
            tooltipObject != nullptr
                ? qobject_cast<QQuickItem*>(tooltipObject->property("contentItem").value<QObject*>())
                : nullptr;
        QQuickWindow* tooltipWindow = nullptr;
        const bool tooltipVisible = waitFor([&] {
            if (tooltipDetails.isEmpty()) return false;
            tooltipWindow = nullptr;
            if (tooltipObject != nullptr && tooltipObject->property("visible").toBool() &&
                tooltipObject->property("opened").toBool() && tooltipContent != nullptr &&
                tooltipContent->isVisible() &&
                tooltipContent->property("text").toString() == tooltipDetails) {
                tooltipWindow = tooltipContent->window();
                return tooltipWindow != nullptr && tooltipWindow->isVisible();
            }
            for (QWindow* topLevel : QGuiApplication::topLevelWindows()) {
                auto* quickWindow = qobject_cast<QQuickWindow*>(topLevel);
                if (quickWindow == nullptr || !quickWindow->isVisible()) continue;
                QQuickItem* root = quickWindow == clip ? clip->rootObject() : quickWindow->contentItem();
                QQuickItem* tooltip = findPickerItem(root, [&](QQuickItem* item) {
                    return item->isVisible() && item->property("text").toString() == tooltipDetails;
                });
                if (tooltip != nullptr) {
                    tooltipWindow = quickWindow;
                    return true;
                }
            }
            return false;
        }, 3000);
        QJsonArray windows;
        int windowIndex = 0;
        for (QWindow* topLevel : QGuiApplication::topLevelWindows()) {
            const QRect geometry = topLevel->geometry();
            QString screenshot;
            if (!tooltipVisible) {
                if (auto* quickWindow = qobject_cast<QQuickWindow*>(topLevel);
                    quickWindow != nullptr && quickWindow->isVisible()) {
                    screenshot = QStringLiteral("clip-estimate-details-failure-window-%1.png").arg(windowIndex);
                    quickWindow->grabWindow().save(QDir(output).filePath(screenshot));
                }
            }
            windows.append(QJsonObject{
                {"class", QString::fromLatin1(topLevel->metaObject()->className())},
                {"title", topLevel->title()},
                {"visible", topLevel->isVisible()},
                {"geometry", QStringLiteral("%1,%2 %3x%4")
                                 .arg(geometry.x()).arg(geometry.y())
                                 .arg(geometry.width()).arg(geometry.height())},
                {"failureScreenshot", screenshot}});
            ++windowIndex;
        }
        if (!tooltipVisible) {
            clip->grabWindow().save(
                QDir(output).filePath(QStringLiteral("clip-estimate-details-failure.png")));
        }
        const QRect labelRect(estimateLabel->mapToScene(QPointF(0, 0)).toPoint(),
                              estimateLabel->size().toSize());
        const QPointF labelLocalPoint = estimateLabel->mapFromScene(labelTextPoint);
        const bool parentTextContainsPoint = estimateLabel->contains(labelLocalPoint);
        QWindow* pointerWindow = QGuiApplication::topLevelAt(QCursor::pos());
        QWindow* focusWindow = QGuiApplication::focusWindow();
        QQuickItem* visualHit = topmostItemAt(clip->contentItem(), labelTextPoint);
        tooltipDiagnostics = QJsonObject{
            {"availableGeometry", QStringLiteral("%1,%2 %3x%4")
                                     .arg(available.x()).arg(available.y())
                                     .arg(available.width()).arg(available.height())},
            {"clipGeometry", QStringLiteral("%1,%2 %3x%4")
                                 .arg(clip->geometry().x()).arg(clip->geometry().y())
                                 .arg(clip->width()).arg(clip->height())},
            {"clipFrameGeometry", QStringLiteral("%1,%2 %3x%4")
                                      .arg(clip->frameGeometry().x()).arg(clip->frameGeometry().y())
                                      .arg(clip->frameGeometry().width()).arg(clip->frameGeometry().height())},
            {"labelWindowRect", QStringLiteral("%1,%2 %3x%4")
                                    .arg(labelRect.x()).arg(labelRect.y())
                                    .arg(labelRect.width()).arg(labelRect.height())},
            {"labelRenderedTextWidth", renderedTextWidth},
            {"labelParentContainsPointer", parentTextContainsPoint},
            {"clipActive", clip->isActive()},
            {"clipExposed", clip->isExposed()},
            {"focusWindow", focusWindow != nullptr ? focusWindow->title() : QString()},
            {"pointerWindow", pointerWindow != nullptr ? pointerWindow->title() : QString()},
            {"pointerWindowClass", pointerWindow != nullptr
                                      ? QString::fromLatin1(pointerWindow->metaObject()->className())
                                      : QString()},
            {"visualHitObjectName", visualHit != nullptr ? visualHit->objectName() : QString()},
            {"visualHitClass", visualHit != nullptr
                                   ? QString::fromLatin1(visualHit->metaObject()->className())
                                   : QString()},
            {"qualityButtonHovered", qualityButtonHovered},
            {"hoverHandlerClass", hoverHandler != nullptr
                                      ? QString::fromLatin1(hoverHandler->metaObject()->className())
                                      : QString()},
            {"hoverHandlerEnabled", hoverHandler != nullptr &&
                                        hoverHandler->property("enabled").toBool()},
            {"hoverHandlerHovered", hoverHandler != nullptr &&
                                        hoverHandler->property("hovered").toBool()},
            {"labelGlobalCenter", QStringLiteral("%1,%2")
                                      .arg(labelGlobalCenter.x()).arg(labelGlobalCenter.y())},
            {"cursorGlobal", QStringLiteral("%1,%2").arg(QCursor::pos().x()).arg(QCursor::pos().y())},
            {"tooltipVisible", tooltipVisible},
            {"tooltipDetails", tooltipDetails},
            {"tooltipObjectFound", tooltipObject != nullptr},
            {"tooltipObjectVisible", tooltipObject != nullptr &&
                                         tooltipObject->property("visible").toBool()},
            {"tooltipObjectOpened", tooltipObject != nullptr &&
                                        tooltipObject->property("opened").toBool()},
            {"tooltipObjectText", tooltipObject != nullptr
                                      ? tooltipObject->property("text").toString()
                                      : QString()},
            {"tooltipContentFound", tooltipContent != nullptr},
            {"tooltipContentVisible", tooltipContent != nullptr && tooltipContent->isVisible()},
            {"tooltipContentText", tooltipContent != nullptr
                                       ? tooltipContent->property("text").toString()
                                       : QString()},
            {"tooltipContentGeometry", tooltipContent != nullptr
                                           ? QStringLiteral("%1,%2 %3x%4")
                                                 .arg(tooltipContent->x()).arg(tooltipContent->y())
                                                 .arg(tooltipContent->width()).arg(tooltipContent->height())
                                           : QString()},
            {"topLevelWindows", windows}};
        require(tooltipVisible, QStringLiteral("hovering the estimate displays its assumptions tooltip"));
        if (tooltipVisible) {
            QQuickWindow* capture = tooltipWindow != nullptr ? tooltipWindow : clip;
            require(capture->grabWindow().save(
                        QDir(output).filePath(QStringLiteral("clip-estimate-details.png"))),
                    QStringLiteral("estimate details tooltip screenshot saved"));
        }
    }
    if (argc > 3) {
        QTranslator translator;
        require(translator.load(QString::fromLocal8Bit(argv[3])), QStringLiteral("real Ukrainian catalogue loads"));
        app.installTranslator(&translator);
        QTest::qWait(100);
        require(editor->selectionStart() == rangeStart && editor->selectionEnd() == rangeEnd,
                QStringLiteral("retranslation preserves the edited interval"));
        require(waitForEstimatePresentation(),
                QStringLiteral("retranslated estimate remains ready and visible"));
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
