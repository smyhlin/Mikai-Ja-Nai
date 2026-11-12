// The native Quick player probe judges the production scene and its
// overlay-free video target independently.

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QQmlError>
#include <QQuickItem>
#include <QScopedValueRollback>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTest>

#include <algorithm>
#include <cstdio>
#include <functional>

#include "PlayerController.hpp"
#include "PlayerMenuModel.hpp"
#include "PlayerIconProvider.hpp"
#include "CompareWindow.hpp"
#include "MpvVideoItem.hpp"
#include "PlayerWindow.hpp"
#include "PreparationWindow.hpp"
#include "ProbePicture.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSession.hpp"

using namespace mplayer;
using namespace mplayer::quick;

namespace {

class EpisodeFixtureHost final : public shell::PlayerHost {
public:
    int requestedEpisode = -1;
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int index) override { requestedEpisode = index; }
    void requestTrackProbe(const QString&) override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override {}
    void playbackFailed() override {}
};

EpisodeFixtureHost episodeFixtureHost;

QString evidenceDir() {
    const QString requested = qEnvironmentVariable("MIKAI_DIALOG_PROBE_OUTPUT_DIR");
    if (requested.isEmpty()) return QDir::current().absolutePath();
    if (!QFileInfo(requested).isAbsolute() || !QDir().mkpath(requested)) return {};
    return QDir(requested).absolutePath();
}

QString evidencePath(const QString& name) {
    return QDir(evidenceDir()).filePath(name);
}

struct PlaybackSample {
    QImage frame;
    double position = -1.0;
    int videoWidth = 0;
    int videoHeight = 0;
};

PlaybackSample capture(PlayerWindow* window, VideoSession* session) {
    PlaybackSample sample;
    if (window == nullptr || session == nullptr) return sample;
    sample.frame = window->grabVideoFrame();
    sample.position = session->position();
    sample.videoWidth = session->videoWidth();
    sample.videoHeight = session->videoHeight();
    return sample;
}

bool sameGeometry(const PlaybackSample& a, const PlaybackSample& b) {
    return !a.frame.isNull() && a.frame.size() == b.frame.size() &&
           a.videoWidth == b.videoWidth && a.videoHeight == b.videoHeight;
}

void requireMovingPicture(mplayer::probes::Verdict& verdict, const PlaybackSample& earlier,
                          const PlaybackSample& later) {
    verdict.require(later.videoWidth > 0 && later.videoHeight > 0,
                    QStringLiteral("production session reports video dimensions (%1x%2)")
                        .arg(later.videoWidth)
                        .arg(later.videoHeight));
    verdict.require(!later.frame.isNull(),
                    QStringLiteral("native Quick window produced its overlay-free video frame"));

    const mplayer::probes::Picture picture =
        mplayer::probes::measurePicture(later.frame, later.videoWidth, later.videoHeight);
    verdict.metric(QStringLiteral("overlay-free video band %1x%2 at %3,%4: lit=%5/%6 (%7%)")
                       .arg(picture.rect.width())
                       .arg(picture.rect.height())
                       .arg(picture.rect.x())
                       .arg(picture.rect.y())
                       .arg(picture.lit)
                       .arg(picture.sampled)
                       .arg(100.0 * picture.litFraction(), 0, 'f', 1));
    if (picture.sampled == 0) {
        verdict.require(false, QStringLiteral("video picture band has pixels to judge"));
        return;
    }
    if (picture.isMostlyBlack()) {
        verdict.inconclusive(
            QStringLiteral("video band is mostly black: valid black content and failed rendering "
                           "are indistinguishable"));
        return;
    }
    verdict.require(true, QStringLiteral("video picture is non-black (> 50% lit pixels)"));

    verdict.require(earlier.position >= 0.0 && later.position > earlier.position,
                    QStringLiteral("source clock advanced (%1 to %2 s)")
                        .arg(earlier.position, 0, 'f', 2)
                        .arg(later.position, 0, 'f', 2));
    if (!sameGeometry(earlier, later)) {
        verdict.inconclusive(QStringLiteral("video sample geometry changed; motion cannot be "
                                            "compared"));
        return;
    }

    long changed = 0;
    long compared = 0;
    for (int y = picture.rect.top(); y <= picture.rect.bottom(); y += 2) {
        for (int x = picture.rect.left(); x <= picture.rect.right(); x += 2) {
            ++compared;
            const QColor before = earlier.frame.pixelColor(x, y);
            const QColor after = later.frame.pixelColor(x, y);
            if (qAbs(after.red() - before.red()) + qAbs(after.green() - before.green()) +
                    qAbs(after.blue() - before.blue()) >
                mplayer::probes::kMotionPixelDelta) {
                ++changed;
            }
        }
    }
    verdict.metric(QStringLiteral("motion band changed=%1/%2 (%3%)")
                       .arg(changed)
                       .arg(compared)
                       .arg(compared > 0 ? 100.0 * double(changed) / double(compared) : 0.0, 0, 'f',
                            2));
    if (compared == 0) {
        verdict.inconclusive(QStringLiteral("no pixels were available for a motion comparison"));
    } else if (double(changed) >= mplayer::probes::kMinMotionChangedFraction * compared) {
        verdict.require(true, QStringLiteral("picture moved across source-time samples (>= 1%)"));
    } else {
        verdict.inconclusive(QStringLiteral("picture pixels did not move: static media and a "
                                            "frozen output are indistinguishable"));
    }
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs,
             const std::function<void()>& sample = {}) {
    QElapsedTimer timer;
    timer.start();
    qint64 nextSampleAt = 0;
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (predicate()) return true;
        if (sample && timer.elapsed() >= nextSampleAt) {
            sample();
            nextSampleAt = timer.elapsed() + 100;
        }
        QThread::msleep(10);
    }
    return predicate();
}

QQuickItem* findVisualItem(QQuickItem* root,
                           const std::function<bool(QQuickItem*)>& matches) {
    if (root == nullptr) return nullptr;
    if (matches(root)) return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* match = findVisualItem(child, matches)) return match;
    }
    return nullptr;
}

QQuickItem* menuPanel(QQuickItem* root, const QString& menuId) {
    if (root == nullptr) return nullptr;
    return findVisualItem(root, [&menuId](QQuickItem* item) {
        return item->property("menuId").toString() == menuId &&
               item->property("menuModel").isValid() && item->isVisible();
    });
}

QQuickItem* submenuRow(QQuickItem* root, const QString& menuId, const QString& rowValue) {
    QQuickItem* panel = menuPanel(root, menuId);
    if (panel == nullptr) return nullptr;
    return findVisualItem(panel, [&rowValue](QQuickItem* item) {
        return item->property("rowSubmenu").toBool() &&
               item->property("rowValue").toString() == rowValue && item->isVisible();
    });
}

bool saveImage(const QImage& image, const QString& name) {
    if (image.isNull()) return false;
    return image.save(evidencePath(name), "PNG");
}

bool exerciseEnhancementCascade(PlayerController* controller, PlayerWindow* window,
                                mplayer::probes::Verdict& verdict,
                                const std::function<void()>& sample) {
    if (controller == nullptr || window == nullptr || window->rootObject() == nullptr) return false;

    // Open through the visible production transport control; menu requests,
    // cascade delegates, and row input all stay on the real PlayerEntry scene.
    QQuickItem* enhanceButton = nullptr;
    for (QQuickItem* item : window->rootObject()->findChildren<QQuickItem*>()) {
        if (item->property("iconName").toString() == QLatin1String("spark") &&
            item->isVisible() && item->isEnabled()) {
            enhanceButton = item;
            break;
        }
    }
    verdict.require(enhanceButton != nullptr,
                    QStringLiteral("visible production enhancement button is available"));
    if (enhanceButton == nullptr) return false;
    const QPoint enhancePoint =
        enhanceButton->mapToScene(QPointF(enhanceButton->width() / 2.0,
                                          enhanceButton->height() / 2.0)).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, enhancePoint);
    const bool rootOpened = waitFor(
        [&] { return menuPanel(window->rootObject(), QStringLiteral("enhancements")) != nullptr; },
        1000, sample);
    verdict.require(rootOpened, QStringLiteral("real enhancement control opened its menu"));
    if (!rootOpened) return false;
    PlayerMenuModel* rootModel = controller->menuModel(QStringLiteral("enhancements"));
    if (rootModel == nullptr || rootModel->rowCount() == 0) {
        verdict.require(false, QStringLiteral("production enhancement menu model is populated"));
        return false;
    }

    QString upscaleLevel;
    int upscaleRow = -1;
    for (int row = 0; row < rootModel->rowCount(); ++row) {
        const PlayerMenuModel::Row entry = rootModel->row(row);
        if (entry.submenu && entry.value == QLatin1String("processing:upscale")) {
            upscaleLevel = entry.value;
            upscaleRow = row;
            break;
        }
    }
    if (upscaleLevel.isEmpty()) {
        verdict.require(false, QStringLiteral("production enhancement menu exposes its Upscale "
                                              "cascade"));
        return false;
    }

    QQuickItem* upscale = submenuRow(window->rootObject(), QStringLiteral("enhancements"),
                                    upscaleLevel);
    verdict.require(upscale != nullptr,
                    QStringLiteral("real enhancement menu exposes the Upscale row above controls"));
    if (upscale == nullptr) return false;

    QElapsedTimer hoverClock;
    quint64 frameSwaps = 0;
    quint64 firstVisibleFrame = 0;
    qint64 firstVisibleFrameMs = -1;
    QMetaObject::Connection frameConnection = QObject::connect(
        window, &QQuickWindow::frameSwapped, window, [&] {
            ++frameSwaps;
            if (firstVisibleFrame != 0 ||
                menuPanel(window->rootObject(), upscaleLevel) == nullptr) {
                return;
            }
            firstVisibleFrame = frameSwaps;
            firstVisibleFrameMs = hoverClock.elapsed();
        }, Qt::AutoConnection);
    const QPoint scenePoint = upscale->mapToScene(QPointF(upscale->width() / 2.0,
                                                           upscale->height() / 2.0)).toPoint();
    hoverClock.start();
    QTest::mouseMove(window, scenePoint);
    const bool expanded = waitFor(
        [&] { return menuPanel(window->rootObject(), upscaleLevel) != nullptr &&
                     firstVisibleFrame != 0; },
        1000, sample);
    QObject::disconnect(frameConnection);

    verdict.require(expanded, QStringLiteral("real pointer hover expands the production Upscale "
                                              "submenu"));
    if (firstVisibleFrame != 0) {
        const quint64 swapsBeforeVisible = firstVisibleFrame - 1;
        verdict.metric(QStringLiteral("hover-to-expanded-presentation=%1 ms; frameSwapsBefore=%2")
                           .arg(firstVisibleFrameMs)
                           .arg(swapsBeforeVisible));
        verdict.require(firstVisibleFrame <= 2 && firstVisibleFrameMs <= 250,
                        QStringLiteral("expanded cascade presented within two Quick frames and "
                                       "250 ms (frames=%1, time=%2 ms)")
                            .arg(firstVisibleFrame)
                            .arg(firstVisibleFrameMs));
    } else {
        verdict.require(false, QStringLiteral("no expanded-menu Quick frame arrived within "
                                              "the bounded 1000 ms observation"));
    }

    PlayerMenuModel* childModel = controller->submenuModel(QStringLiteral("enhancements"),
                                                            upscaleRow);
    verdict.require(childModel != nullptr && childModel->rowCount() > 0,
                    QStringLiteral("expanded Upscale panel uses its production model"));
    if (!expanded || childModel == nullptr) return false;

    upscale = submenuRow(window->rootObject(), QStringLiteral("enhancements"), upscaleLevel);
    if (upscale == nullptr) {
        verdict.require(false, QStringLiteral("parent Upscale row remains available after expansion"));
        return false;
    }
    const QPoint upscaleClick =
        upscale->mapToScene(QPointF(upscale->width() / 2.0, upscale->height() / 2.0)).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, upscaleClick);
    const bool rowInputContained = waitFor(
        [&] { return menuPanel(window->rootObject(), upscaleLevel) != nullptr &&
                     controller->menuStackVisible(); },
        500, sample);
    verdict.require(rowInputContained,
                    QStringLiteral("clicking the real submenu row stays inside the menu stack"));
    verdict.require(controller->session()->isPlaying(),
                    QStringLiteral("submenu-row click did not reach the underlying transport"));

    int anime4kRow = -1;
    for (int row = 0; row < rootModel->rowCount(); ++row) {
        const PlayerMenuModel::Row entry = rootModel->row(row);
        if (entry.submenu && entry.value == QLatin1String("anime4k")) {
            anime4kRow = row;
            break;
        }
    }
    QQuickItem* anime4k = submenuRow(window->rootObject(), QStringLiteral("enhancements"),
                                     QStringLiteral("anime4k"));
    verdict.require(anime4kRow >= 0 && anime4k != nullptr,
                    QStringLiteral("production enhancement menu exposes built-in Anime4K"));
    if (anime4kRow < 0 || anime4k == nullptr) return false;

    const QPoint anime4kPoint =
        anime4k->mapToScene(QPointF(anime4k->width() / 2.0, anime4k->height() / 2.0)).toPoint();
    QTest::mouseMove(window, anime4kPoint);
    const QString anime4kLevel = QStringLiteral("anime4k");
    const bool anime4kExpanded = waitFor(
        [&] { return menuPanel(window->rootObject(), anime4kLevel) != nullptr; }, 700, sample);
    verdict.require(anime4kExpanded,
                    QStringLiteral("pointer hover opens the actual built-in Anime4K model list"));
    if (!anime4kExpanded) return false;

    PlayerMenuModel* anime4kModel =
        controller->submenuModel(QStringLiteral("enhancements"), anime4kRow);
    int categoryRow = -1;
    QString categoryLevel;
    if (anime4kModel != nullptr) {
        for (int row = 0; row < anime4kModel->rowCount(); ++row) {
            const PlayerMenuModel::Row entry = anime4kModel->row(row);
            if (entry.submenu && entry.value.startsWith(QLatin1String("anime4k/"))) {
                categoryRow = row;
                categoryLevel = entry.value;
                break;
            }
        }
    }
    QQuickItem* category = submenuRow(window->rootObject(), anime4kLevel, categoryLevel);
    verdict.require(categoryRow >= 0 && category != nullptr,
                    QStringLiteral("built-in Anime4K list exposes a nested model category"));
    if (categoryRow < 0 || category == nullptr) return false;

    const QPoint categoryPoint =
        category->mapToScene(QPointF(category->width() / 2.0, category->height() / 2.0))
            .toPoint();
    QTest::mouseMove(window, categoryPoint);
    const bool nestedExpanded = waitFor(
        [&] { return menuPanel(window->rootObject(), categoryLevel) != nullptr; }, 700, sample);
    verdict.require(nestedExpanded,
                    QStringLiteral("real hover expands a third-level Anime4K cascade"));
    if (!nestedExpanded) return false;

    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, categoryPoint);
    const bool nestedClickContained = controller->menuStackVisible() &&
                                      menuPanel(window->rootObject(), categoryLevel) != nullptr;
    verdict.require(nestedClickContained,
                    QStringLiteral("nested submenu-row click does not dismiss or pass through"));
    verdict.require(controller->session()->isPlaying(),
                    QStringLiteral("nested row click did not toggle the underlying transport"));

    QQuickItem* nestedPanel = menuPanel(window->rootObject(), categoryLevel);
    if (nestedPanel == nullptr) return false;
    const QPoint paddingPoint =
        nestedPanel->mapToScene(QPointF(nestedPanel->width() - 3.0,
                                        nestedPanel->height() / 2.0)).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, paddingPoint);
    const bool paddingClickContained = controller->menuStackVisible() &&
                                       menuPanel(window->rootObject(), categoryLevel) != nullptr;
    verdict.require(paddingClickContained,
                    QStringLiteral("cascade panel padding click remains inside the menu stack"));
    verdict.require(controller->session()->isPlaying(),
                    QStringLiteral("panel-padding click did not toggle the underlying transport"));
    return rowInputContained && anime4kExpanded && nestedExpanded && nestedClickContained &&
           paddingClickContained && controller->session()->isPlaying();
}

struct RunResult {
    bool completed = false;
    QPointer<PlayerController> controller;
    QPointer<PlayerWindow> window;
};

RunResult runPlayer(const QString& url, const QString& videoName, const QString& screenName,
                    bool exerciseMenus, mplayer::probes::Verdict& verdict) {
    RunResult result;
    const bool episodeFixture = qEnvironmentVariableIntValue("MIKAI_DIALOG_EPISODE_FIXTURE") == 1;
    auto* controller = episodeFixture
                           ? new PlayerController(QStringLiteral("Episode 1"), url,
                                                  {{720, url}}, 720, &episodeFixtureHost)
                           : new PlayerController(QStringLiteral("dialog probe"), url);
    if (episodeFixture) {
        QVector<QVariantMap> playlist;
        for (int i = 1; i <= 6; ++i) {
            playlist.append({{QStringLiteral("label"), QStringLiteral("Episode %1").arg(i)},
                             {QStringLiteral("number"), QString::number(i)}});
        }
        controller->setPlaylist(playlist, 0);
    }
    result.controller = controller;
    result.window = controller->window();

    QString loadError;
    const QMetaObject::Connection sceneErrorConnection =
        QObject::connect(result.window, &PlayerWindow::sceneLoadFailed, controller,
                         [&loadError](const QString& error) { loadError = error; });
    const QMetaObject::Connection mediaErrorConnection =
        QObject::connect(controller->session(), &VideoSession::errorOccurred, controller,
                         [&loadError](const QString& error) { loadError = error; });

    controller->showWindow();
    controller->startPlayer();

    const auto closeWindow = [&] {
        QObject::disconnect(sceneErrorConnection);
        QObject::disconnect(mediaErrorConnection);
        if (controller == nullptr) return false;
        const bool accepted = controller->requestClose();
        if (accepted) waitFor([&] { return !controller->isWindowVisible(); }, 2500);
        return accepted;
    };

    const bool visible = waitFor([&] {
        return controller->isWindowVisible() && result.window->rootObject() != nullptr;
    }, 10000);
    verdict.require(visible, QStringLiteral("actual PlayerWindow loaded PlayerEntry and is visible"));
    const bool fullscreen =
        visible && result.window->windowStates().testFlag(Qt::WindowFullScreen) &&
        controller->fullscreen();
    verdict.require(fullscreen,
                    QStringLiteral("production chrome hide/show test is in actual fullscreen"));
    if (!visible) {
        QStringList viewErrors;
        for (const QQmlError& error : result.window->errors()) {
            viewErrors.append(error.toString());
        }
        verdict.metric(
            QStringLiteral("production window timeout: status=%1 controllerVisible=%2 "
                           "windowVisible=%3 rootObject=%4 fullscreenState=%5 loadError=%6 "
                           "QQuickView.errors=%7")
                .arg(static_cast<int>(result.window->status()))
                .arg(int(controller->isWindowVisible()))
                .arg(int(result.window->isVisible()))
                .arg(int(result.window->rootObject() != nullptr))
                .arg(int(result.window->windowStates().testFlag(Qt::WindowFullScreen)))
                .arg(loadError.isEmpty() ? QStringLiteral("(none)") : loadError)
                .arg(viewErrors.isEmpty() ? QStringLiteral("(none)")
                                          : viewErrors.join(QLatin1Char('\n'))));
        closeWindow();
        return result;
    }

    VideoSession* session = controller->session();
    const bool sourceReady = waitFor([&] {
        return !loadError.isEmpty() ||
               (session->hasVideo() && result.window->hasVisibleOutputFrame());
    }, 12000);
    verdict.require(sourceReady && loadError.isEmpty(),
                    QStringLiteral("explicit media source reached a video-ready native window%1")
                        .arg(loadError.isEmpty() ? QString()
                                                 : QStringLiteral(" (%1)").arg(loadError)));
    if (!sourceReady || !loadError.isEmpty()) {
        closeWindow();
        return result;
    }

    waitFor([&] { return session->duration() > 0.0; }, 5000);
    if (qEnvironmentVariableIntValue("MIKAI_DIALOG_PREPARATION_FIXTURE") == 1) {
        controller->beginEnginePreparation(QStringLiteral("Preparing engine"));
        QPointer<PreparationWindow> preparation;
        const bool opened = waitFor([&] {
            for (QWindow* window : QGuiApplication::topLevelWindows()) {
                auto* candidate = qobject_cast<PreparationWindow*>(window);
                if (candidate != nullptr && candidate->isVisible() && candidate->rootObject()) {
                    preparation = candidate;
                    return true;
                }
            }
            return false;
        }, 5000);
        verdict.require(opened, QStringLiteral("engine preparation window is visible"));
        if (preparation != nullptr) {
            auto* elapsed = preparation->rootObject()->findChild<QObject*>(
                QStringLiteral("player.preparation.elapsed"));
            const QRegularExpression pattern(QStringLiteral("\\d{2,}:\\d{2}:\\d{2}$"));
            verdict.require(elapsed != nullptr && pattern.match(
                                elapsed->property("text").toString()).hasMatch(),
                            QStringLiteral("preparation elapsed time has padded fields"));
            saveImage(preparation->grabWindow(), QStringLiteral("dialog_probe_preparation.png"));
        }
        controller->endEnginePreparation(true, QStringLiteral("Complete"));
    }
    if (qEnvironmentVariableIntValue("MIKAI_DIALOG_HOVER_FIXTURE") == 1) {
        auto* timeline = qobject_cast<QQuickItem*>(
            result.window->rootObject()->property("timeline").value<QObject*>());
        verdict.require(timeline != nullptr && timeline->isVisible(),
                        QStringLiteral("timeline is visible for preview"));
        if (timeline != nullptr) {
            const auto pointAt = [timeline](qreal fraction) {
                return timeline->mapToScene(QPointF(timeline->width() * fraction,
                                                     timeline->height() / 2)).toPoint();
            };
            const QPoint point = pointAt(0.35);
            QTest::mouseMove(result.window, point);
            QQuickItem* preview = nullptr;
            const bool visiblePreview = waitFor([&] {
                preview = findVisualItem(result.window->rootObject(), [](QQuickItem* item) {
                    return item->objectName() == QLatin1String("player.timeline.preview")
                           && item->isVisible();
                });
                return preview != nullptr;
            }, 10000);
            verdict.require(visiblePreview, QStringLiteral("timeline preview opens"));
            if (visiblePreview) {
                verdict.require(waitFor([&] {
                    auto* frame = preview->findChild<QQuickItem*>(
                        QStringLiteral("player.timeline.preview.frame"));
                    return frame != nullptr && frame->property("status").toInt() == 1;
                }, 5000), QStringLiteral("preview frame is decoded and displayed"));
                const QPointF top = preview->mapToScene(QPointF(0, 0));
                const QPointF timelineTop = timeline->mapToScene(QPointF(0, 0));
                verdict.require(qAbs(top.y() + preview->height() - timelineTop.y() + 10.0) < 2.0,
                                QStringLiteral("preview sits immediately above the timeline"));
                verdict.require(qAbs(top.x() + preview->width() / 2.0 - point.x()) < 2.0,
                                QStringLiteral("preview is centered at the hovered position"));
                saveImage(result.window->grabWindow(), QStringLiteral("dialog_probe_hover.png"));
                const QPoint moved = pointAt(0.65);
                QTest::mouseMove(result.window, moved);
                verdict.require(waitFor([&] {
                    const QPointF position = preview->mapToScene(QPointF(0, 0));
                    return preview->isVisible() &&
                           qAbs(position.x() + preview->width() / 2.0 - moved.x()) < 2.0;
                }, 2000), QStringLiteral("preview follows the pointer along the timeline"));
                QTest::mouseMove(result.window, QPoint(8, 8));
                verdict.require(waitFor([&] { return !preview->isVisible(); }, 2000),
                                QStringLiteral("preview closes when leaving the timeline"));
            }
        }
    }
    if (episodeFixture) {
        QQuickItem* button = findVisualItem(result.window->rootObject(), [](QQuickItem* item) {
            return item->objectName() == QLatin1String("player.episodes.button") && item->isVisible();
        });
        verdict.require(button != nullptr, QStringLiteral("episode list button is visible"));
        if (button != nullptr) {
            QTest::mouseClick(result.window, Qt::LeftButton, Qt::NoModifier,
                              button->mapToScene(QPointF(button->width() / 2,
                                                         button->height() / 2)).toPoint());
            QQuickItem* drawer = nullptr;
            const bool opened = waitFor([&] {
                drawer = findVisualItem(result.window->rootObject(), [](QQuickItem* item) {
                    return item->objectName() == QLatin1String("player.episodes.drawer")
                           && item->isVisible();
                });
                return drawer != nullptr;
            }, 3000);
            verdict.require(opened, QStringLiteral("episode drawer opens"));
            if (opened) {
                QQuickItem* firstRow = findVisualItem(drawer, [](QQuickItem* item) {
                    return item->objectName() == QLatin1String("player.episodes.row.episode.0")
                           && item->isVisible();
                });
                verdict.require(firstRow != nullptr, QStringLiteral("current episode is visible"));
                verdict.require(drawer->height() < result.window->height(),
                                QStringLiteral("episode drawer fits its content"));
                saveImage(result.window->grabWindow(), QStringLiteral("dialog_probe_episodes.png"));
                QQuickItem* secondRow = findVisualItem(drawer, [](QQuickItem* item) {
                    return item->objectName() == QLatin1String("player.episodes.row.episode.1")
                           && item->isVisible();
                });
                verdict.require(secondRow != nullptr, QStringLiteral("next episode is visible"));
                if (secondRow != nullptr) {
                    episodeFixtureHost.requestedEpisode = -1;
                    QTest::mouseClick(result.window, Qt::LeftButton, Qt::NoModifier,
                                      secondRow->mapToScene(QPointF(secondRow->width() / 2,
                                                                   secondRow->height() / 2)).toPoint());
                    verdict.require(waitFor([&] { return episodeFixtureHost.requestedEpisode == 1; }, 2000),
                                    QStringLiteral("episode selection reaches its host"));
                }
                verdict.require(waitFor([&] { return !drawer->isVisible(); }, 2000),
                                QStringLiteral("episode drawer dismisses after selection"));
            }
        }
    }
    QQuickItem* clipButton = nullptr;
    waitFor([&] {
        clipButton = findVisualItem(result.window->rootObject(), [](QQuickItem* item) {
            return item->property("iconName").toString() == QLatin1String("clapperboard")
                   && item->isVisible() && item->isEnabled();
        });
        return clipButton != nullptr;
    }, 2000);
    verdict.require(clipButton != nullptr, QStringLiteral("clip button is visible"));
    if (clipButton != nullptr) {
        QTest::mouseClick(result.window, Qt::LeftButton, Qt::NoModifier,
                          clipButton->mapToScene(QPointF(clipButton->width() / 2,
                                                        clipButton->height() / 2)).toPoint());
        const bool clipVisible = waitFor([] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (widget->isVisible() && widget->inherits("mplayer::shell::ClipDialog"))
                    return true;
            }
            return false;
        }, 5000);
        verdict.require(clipVisible, QStringLiteral("clip dialog is visible"));
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->inherits("mplayer::shell::ClipDialog")) widget->close();
        }
        result.window->requestActivate();
    }
    QTest::mouseClick(result.window, Qt::RightButton, Qt::NoModifier,
                      QPoint(result.window->width() / 2, result.window->height() / 2));
    const bool contextOpened = waitFor([&] {
        return menuPanel(result.window->rootObject(), QStringLiteral("picture")) != nullptr;
    }, 2000);
    verdict.require(contextOpened, QStringLiteral("right click opens picture menu"));
    if (contextOpened) {
        PlayerIconProvider iconProvider;
        for (const QString& menuId : {QStringLiteral("picture"), QStringLiteral("picture.scale"),
                                      QStringLiteral("picture.subtitles")}) {
            PlayerMenuModel* model = controller->menuModel(menuId);
            bool iconsVisible = model != nullptr && model->rowCount() > 0;
            if (model != nullptr) {
                for (const PlayerMenuModel::Row& row : model->rows()) {
                    if (row.heading) continue;
                    QSize size;
                    const QImage image = iconProvider.requestImage(
                        row.icon + QStringLiteral("?size=16&dpr=1&color=%23ffffff"), &size, {});
                    bool hasInk = false;
                    for (int y = 0; y < image.height() && !hasInk; ++y) {
                        for (int x = 0; x < image.width(); ++x) {
                            if (image.pixelColor(x, y).alpha() > 0) {
                                hasInk = true;
                                break;
                            }
                        }
                    }
                    if (row.icon.isEmpty() || !hasInk) {
                        iconsVisible = false;
                        verdict.metric(QStringLiteral("missing icon for %1").arg(row.id));
                    }
                }
            }
            verdict.require(iconsVisible, QStringLiteral("all %1 actions have visible icons").arg(menuId));
        }
        saveImage(result.window->grabWindow(), QStringLiteral("dialog_probe_picture_menu.png"));
        PlayerMenuModel* scaleModel = controller->menuModel(QStringLiteral("picture.scale"));
        QQuickItem* scaleOpener = submenuRow(result.window->rootObject(),
                                             QStringLiteral("picture"),
                                             QStringLiteral("picture.scale"));
        verdict.require(scaleOpener != nullptr && scaleModel != nullptr,
                        QStringLiteral("video scaling submenu is available"));
        if (scaleOpener != nullptr && scaleModel != nullptr) {
            QTest::mouseMove(result.window,
                             scaleOpener->mapToScene(QPointF(scaleOpener->width() / 2,
                                                             scaleOpener->height() / 2)).toPoint());
            QQuickItem* scalePanel = nullptr;
            const bool scaleOpened = waitFor([&] {
                scalePanel = menuPanel(result.window->rootObject(), QStringLiteral("picture.scale"));
                return scalePanel != nullptr;
            }, 2000);
            verdict.require(scaleOpened, QStringLiteral("video scaling submenu opens"));
            if (scaleOpened && scaleModel->rowCount() >= 2) {
                const auto chooseScale = [&](int index) {
                    const PlayerMenuModel::Row choice = scaleModel->row(index);
                    QQuickItem* row = findVisualItem(scalePanel, [&choice](QQuickItem* item) {
                        return item->isVisible() && item->property("rowValue").toString() == choice.value
                               && item->property("rowAction").toInt() ==
                                      int(PlayerMenuModel::Action::SetVideoScaleMode);
                    });
                    if (row == nullptr) return false;
                    QTest::mouseClick(result.window, Qt::LeftButton, Qt::NoModifier,
                                      row->mapToScene(QPointF(row->width() / 2,
                                                               row->height() / 2)).toPoint());
                    return waitFor([&] {
                        int selected = 0;
                        for (const auto& candidate : scaleModel->rows())
                            if (candidate.selected) ++selected;
                        return controller->videoScaleMode() == choice.value.toInt() &&
                               scaleModel->row(index).selected && selected == 1;
                    }, 2000);
                };
                verdict.require(chooseScale(scaleModel->rowCount() - 1),
                                QStringLiteral("last scale option becomes the only selected option"));
                verdict.require(chooseScale(0),
                                QStringLiteral("first scale option replaces the previous selection"));
                saveImage(result.window->grabWindow(), QStringLiteral("dialog_probe_scaling_menu.png"));
            }
        }
        QQuickItem* compareRow = findVisualItem(
            menuPanel(result.window->rootObject(), QStringLiteral("picture")), [](QQuickItem* item) {
                return item->property("rowAction").toInt() == int(PlayerMenuModel::Action::OpenCompare)
                       && item->isVisible();
            });
        verdict.require(compareRow != nullptr, QStringLiteral("compare action is visible"));
        if (compareRow != nullptr) {
            QTest::mouseClick(result.window, Qt::LeftButton, Qt::NoModifier,
                              compareRow->mapToScene(QPointF(compareRow->width() / 2,
                                                              compareRow->height() / 2)).toPoint());
            QPointer<mplayer::shell::CompareWindow> compare;
            const bool compareVisible = waitFor([&] {
                for (QWidget* widget : QApplication::topLevelWidgets()) {
                    auto* candidate = qobject_cast<mplayer::shell::CompareWindow*>(widget);
                    if (candidate != nullptr && candidate->isVisible()) {
                        compare = candidate;
                        return true;
                    }
                }
                return false;
            }, 10000);
            verdict.require(compareVisible, QStringLiteral("compare window is visible"));
            if (compare != nullptr) {
                const bool captured = waitFor([&] {
                    return compare == nullptr || (!compare->comparisonPending() &&
                           (!compare->frozenFrame().isNull() || !compare->captureError().isEmpty()));
                }, 30000);
                verdict.require(captured && compare != nullptr && !compare->frozenFrame().isNull(),
                                QStringLiteral("compare freezes a real playback frame%1")
                                    .arg(compare != nullptr && !compare->captureError().isEmpty()
                                             ? QStringLiteral(" (%1)").arg(compare->captureError())
                                             : QString()));
                if (compare != nullptr) {
                    verdict.require(!compare->renderedSideA().isNull() &&
                                        !compare->renderedSideB().isNull(),
                                    QStringLiteral("compare displays both candidate pictures"));
                    verdict.require(compare->playbackHeld(),
                                    QStringLiteral("compare holds playback at the captured frame"));
                    saveImage(compare->grab().toImage(), QStringLiteral("dialog_probe_compare.png"));
                    compare->close();
                }
                verdict.require(waitFor([&] { return session->isPlaying(); }, 5000),
                                QStringLiteral("closing compare resumes playing video"));
            }
        }
    }
    if (!session->isPlaying()) session->setPaused(false);
    result.window->requestActivate();

    QQuickItem* settingsButton = findVisualItem(result.window->rootObject(), [](QQuickItem* item) {
        return item->objectName() == QLatin1String("player.settings.button") && item->isVisible();
    });
    verdict.require(settingsButton != nullptr, QStringLiteral("settings button is visible"));
    if (settingsButton != nullptr) {
        const QPoint point = settingsButton->mapToScene(
            QPointF(settingsButton->width() / 2, settingsButton->height() / 2)).toPoint();
        QTest::mouseClick(result.window, Qt::LeftButton, Qt::NoModifier, point);
        const bool opened = waitFor([&] {
            QQuickItem* item = findVisualItem(result.window->rootObject(), [](QQuickItem* child) {
                return child->objectName() == QLatin1String("player.settings") && child->isVisible();
            });
            return item != nullptr;
        }, 2000);
        verdict.require(opened, QStringLiteral("settings flyout opens"));
        if (opened) {
            QQuickItem* scaleChoice = findVisualItem(result.window->rootObject(), [](QQuickItem* item) {
                return item->objectName() == QLatin1String("player.settings.row.scale.0");
            });
            verdict.require(scaleChoice != nullptr && scaleChoice->property("isSelected").toBool(),
                            QStringLiteral("settings shows the active scaling choice"));
            QQuickItem* preset = findVisualItem(result.window->rootObject(), [](QQuickItem* item) {
                return item->objectName() == QLatin1String("player.speed.1.2") && item->isVisible();
            });
            verdict.require(preset != nullptr, QStringLiteral("speed preset is visible"));
            if (preset != nullptr) {
                const QPoint presetPoint = preset->mapToScene(
                    QPointF(preset->width() / 2, preset->height() / 2)).toPoint();
                QTest::mouseClick(result.window, Qt::LeftButton, Qt::NoModifier, presetPoint);
                verdict.require(waitFor([&] { return std::abs(controller->rate() - 1.2) < 0.01; }, 2000),
                                QStringLiteral("speed preset changes playback rate"));
                controller->setRate(1.0);
            }
            saveImage(result.window->grabWindow(), QStringLiteral("dialog_probe_settings.png"));
            QTest::keyClick(result.window, Qt::Key_Escape);
        }
    }

    PlaybackSample earlier;
    PlaybackSample later;
    int validSamples = 0;
    int blackSamples = 0;
    bool samplingPlayback = false;
    int backwardJumps = 0;
    int forwardJumps = 0;
    double previousPosition = -1.0;
    qint64 previousSampleMs = -1;
    QElapsedTimer sampleClock;
    sampleClock.start();
    const bool knownBrightFixture =
        qEnvironmentVariable("MIKAI_DIALOG_PROBE_BRIGHT_FIXTURE") == QLatin1String("1");
    const double duration = session->duration();
    const double firstAt = duration > 0.0 ? std::min(0.5, std::max(0.1, duration * 0.15)) : 0.5;
    const auto samplePlayback = [&] {
        if (samplingPlayback || !session->isPlaying() ||
            !result.window->hasVisibleOutputFrame()) {
            return;
        }
        QScopedValueRollback<bool> samplingGuard(samplingPlayback, true);
        const PlaybackSample sample = capture(result.window, session);
        if (sample.frame.isNull() || sample.videoWidth <= 0 || sample.videoHeight <= 0 ||
            sample.position < 0.0) {
            return;
        }
        ++validSamples;
        if (knownBrightFixture) {
            const mplayer::probes::Picture picture =
                mplayer::probes::measurePicture(sample.frame, sample.videoWidth,
                                                 sample.videoHeight);
            if (picture.sampled > 0 && picture.isMostlyBlack()) ++blackSamples;
        }
        const qint64 sampleMs = sampleClock.elapsed();
        if (previousPosition >= 0.0) {
            if (sample.position < previousPosition - 0.35) ++backwardJumps;
            const double wallDelta = double(sampleMs - previousSampleMs) / 1000.0;
            if (sample.position - previousPosition > wallDelta + 0.75) ++forwardJumps;
        }
        previousPosition = sample.position;
        previousSampleMs = sampleMs;
        if (earlier.position < 0.0 && sample.position >= firstAt) earlier = sample;
        if (earlier.position >= 0.0) later = sample;
    };

    // PlayerWindow emits this only after a fresh output frame was composited.
    // The Basic loop keeps this callback on the GUI thread, where readback is legal.
    const QMetaObject::Connection frameSampleConnection =
        QObject::connect(result.window, &PlayerWindow::framePresented, result.window,
                         samplePlayback, Qt::AutoConnection);

    const bool beganPlaying = waitFor([&] { return earlier.position >= 0.0; }, 4000,
                                      samplePlayback);
    verdict.require(beganPlaying && session->isPlaying(),
                    QStringLiteral("production session is playing before UI transitions"));
    if (knownBrightFixture) {
        verdict.metric(QStringLiteral("bright-fixture guard=enabled; black samples are failures"));
    } else {
        verdict.instrumentation(
            false, QStringLiteral("black-frame detection requires explicit "
                                  "MIKAI_DIALOG_PROBE_BRIGHT_FIXTURE=1 for the known-bright FFV1 fixture"));
    }

    const bool sceneShot = saveImage(result.window->grabWindow(), screenName);
    verdict.require(sceneShot,
                    QStringLiteral("actual production QQuickWindow screenshot saved to %1")
                        .arg(evidencePath(screenName)));

    const int beforeAutoHideSamples = validSamples;
    QTest::mouseMove(result.window, QPoint(result.window->width() / 2,
                                           result.window->height() / 2));
    controller->closeMenus();
    const bool autoHidden =
        waitFor([&] { return controller->autoHidden(); }, 5000, samplePlayback);
    verdict.require(autoHidden, QStringLiteral("playing production chrome auto-hid while frames "
                                               "continued to be sampled"));
    QElapsedTimer hiddenPeriod;
    hiddenPeriod.start();
    waitFor([&] { return hiddenPeriod.elapsed() >= 350; }, 500, samplePlayback);
    verdict.require(validSamples > beforeAutoHideSamples,
                    QStringLiteral("overlay-free output was sampled while chrome was hidden"));
    const int beforeWakeSamples = validSamples;
    controller->noteUserActivity();
    const bool woke =
        waitFor([&] { return !controller->autoHidden(); }, 1500, samplePlayback);
    verdict.require(woke, QStringLiteral("real player activity restored the hidden chrome"));
    QElapsedTimer awakePeriod;
    awakePeriod.start();
    waitFor([&] { return awakePeriod.elapsed() >= 350; }, 500, samplePlayback);
    verdict.require(validSamples > beforeWakeSamples,
                    QStringLiteral("overlay-free output was sampled while chrome was visible again"));

    const bool playingForMenus = session->isPlaying();
    verdict.require(playingForMenus, QStringLiteral("session remained playing before menu hover"));
    const int beforeMenuSamples = validSamples;

    if (exerciseMenus) {
        const bool menu = exerciseEnhancementCascade(controller, result.window, verdict,
                                                     samplePlayback);
        if (menu) {
            const bool menuShot = saveImage(result.window->grabWindow(),
                                            QStringLiteral("dialog_probe_enhancements.png"));
            verdict.require(menuShot, QStringLiteral("actual enhancement cascade screenshot saved"));
        }
        controller->closeMenus();
        QElapsedTimer menuPeriod;
        menuPeriod.start();
        waitFor([&] { return menuPeriod.elapsed() >= 300; }, 400, samplePlayback);
    }
    verdict.require(session->isPlaying(),
                    QStringLiteral("production session stayed playing through menu and list changes"));
    verdict.require(validSamples > beforeMenuSamples,
                    QStringLiteral("overlay-free frames were sampled during enhancement menu hover"));
    verdict.require(backwardJumps == 0,
                    QStringLiteral("source position did not jump backward during UI transitions "
                                   "(observed %1 jumps)").arg(backwardJumps));
    verdict.require(forwardJumps == 0,
                    QStringLiteral("source position did not jump forward during UI transitions "
                                   "(observed %1 jumps)").arg(forwardJumps));
    if (knownBrightFixture) {
        verdict.require(blackSamples == 0,
                        QStringLiteral("known-bright FFV1 fixture produced no black overlay-free "
                                       "samples (%1 black of %2)")
                            .arg(blackSamples)
                            .arg(validSamples));
        verdict.metric(QStringLiteral("bright-frame samples=%1, black=%2")
                           .arg(validSamples)
                           .arg(blackSamples));
    }
    const double requiredAdvance = duration > 0.0
                                       ? std::min(2.0, std::max(0.3, duration * 0.5))
                                       : 2.0;
    const bool paired =
        waitFor([&] { return earlier.position >= 0.0 &&
                             later.position >= earlier.position + requiredAdvance; },
                8000, samplePlayback);
    verdict.metric(QStringLiteral("session duration=%1 s, position=%2 s, playing=%3, video=%4x%5")
                       .arg(session->duration(), 0, 'f', 2)
                       .arg(session->position(), 0, 'f', 2)
                       .arg(int(session->isPlaying()))
                       .arg(session->videoWidth())
                       .arg(session->videoHeight()));
    verdict.require(loadError.isEmpty(),
                    QStringLiteral("no playback error reported (%1)")
                        .arg(loadError.isEmpty() ? QStringLiteral("none") : loadError));
    verdict.require(paired,
                    QStringLiteral("paired source-time samples advanced by %1 s during the live "
                                   "production UI walk")
                        .arg(requiredAdvance, 0, 'f', 2));
    if (paired) {
        const bool videoShot = saveImage(later.frame, videoName);
        verdict.require(videoShot, QStringLiteral("overlay-free video evidence saved to %1")
                                       .arg(evidencePath(videoName)));
        requireMovingPicture(verdict, earlier, later);
        QQuickItem* videoItem = result.window->videoItem();
        const QSize expected = videoItem == nullptr
                                   ? QSize()
                                   : QSize(qRound(videoItem->width() * result.window->devicePixelRatio()),
                                           qRound(videoItem->height() * result.window->devicePixelRatio()));
        const bool ownTarget = !expected.isEmpty() &&
                               qAbs(later.frame.width() - expected.width()) <= 1 &&
                               qAbs(later.frame.height() - expected.height()) <= 1;
        verdict.require(ownTarget,
                        QStringLiteral("overlay-free capture matches production video item's "
                                       "device-pixel geometry (%1x%2 vs %3x%4)")
                            .arg(later.frame.width())
                            .arg(later.frame.height())
                            .arg(expected.width())
                            .arg(expected.height()));
    }
    verdict.metric(QStringLiteral("live UI samples=%1, backward jumps=%2, forward jumps=%3")
                       .arg(validSamples)
                       .arg(backwardJumps)
                       .arg(forwardJumps));

    QObject::disconnect(frameSampleConnection);
    const bool closeAccepted = closeWindow();
    verdict.require(closeAccepted, QStringLiteral("production requestClose acknowledged teardown"));
    if (closeAccepted) {
        result.completed = !controller->isWindowVisible();
        verdict.require(result.completed, QStringLiteral("acknowledged production window closed"));
    }
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    // Select the production OpenGL surface before QApplication creates the
    // GUI platform and scene graph. QSG_RENDER_LOOP is intentionally untouched:
    // the shipping default is tested unless the caller explicitly overrides it.
    const QByteArray requestedRenderLoop = qgetenv("QSG_RENDER_LOOP");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiDialogProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("DialogProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());

    if (evidenceDir().isEmpty()) {
        std::fprintf(stderr, "FAIL MIKAI_DIALOG_PROBE_OUTPUT_DIR must name an absolute writable directory\n");
        return 2;
    }
    if (!mplayer::probes::selectRuntime()) return 2;

    // The first positional argument remains the media URL/path convention.
    const QString url = argc > 1
                            ? QString::fromLocal8Bit(argv[1])
                            : QStringLiteral("av://lavfi:testsrc2=size=1280x720:rate=25");
    mplayer::probes::Verdict verdict;
    verdict.metric(QStringLiteral("QSG_RENDER_LOOP request: %1")
                       .arg(requestedRenderLoop.isEmpty()
                                ? QStringLiteral("unset; Qt production default")
                                : QString::fromLocal8Bit(requestedRenderLoop)));

    RunResult first = runPlayer(url, QStringLiteral("dialog_gl.png"),
                                QStringLiteral("dialog_probe.png"), true, verdict);
    if (!first.completed) {
        verdict.require(false, QStringLiteral("first player completed its acknowledged close"));
        return verdict.report("dialog_probe");
    }
    if (first.controller != nullptr) {
        delete first.controller.data();
    }

    RunResult reopened = runPlayer(url, QStringLiteral("dialog_gl_reopen.png"),
                                   QStringLiteral("dialog_probe_reopen.png"), true, verdict);
    if (reopened.controller != nullptr) delete reopened.controller.data();
    verdict.require(reopened.completed,
                    QStringLiteral("player closed and reopened with a new production session"));
    return verdict.report("dialog_probe");
}
