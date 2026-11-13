// UI actions stay within owned windows; no desktop input is synthesized.
#include "VideoScalingLiveProbe.hpp"

#include "backend/MikaiClient.hpp"
#include "dialogs/SettingsDialog.hpp"
#include "quick/PlayerController.hpp"
#include "quick/PlayerMenuModel.hpp"
#include "quick/PlayerWindow.hpp"
#include "quick/MpvVideoItem.hpp"
#include "player/core/VideoSession.hpp"
#include "support/I18n.hpp"
#include "workers/Runner.hpp"

#include "ProbeRuntime.hpp"
#include "ProbeUi.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStringList>
#include <QThread>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

using mikai::app::SettingsDialog;
using mplayer::VideoScaleMode;
using mplayer::VideoScalePolicy;
using mplayer::VideoSession;
using mplayer::quick::PlayerController;
using mplayer::quick::PlayerWindow;

namespace mikai::probes {

namespace {
int failures = 0;
int checks = 0;
constexpr double kPi = 3.14159265358979323846;

void check(bool ok, const QString& what, const QString& detail) {
    ++checks;
    const QString line =
        what + (detail.isEmpty() ? QString() : QStringLiteral(": ") + detail);
    std::fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL", line.toUtf8().constData());
    if (!ok) ++failures;
}

void check(bool ok, const QString& what) { check(ok, what, QString()); }

bool pumpUntil(const std::function<bool()>& done, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(10);
    }
    return done();
}

double pixelLuma(const QColor& color) {
    return 0.2126 * color.red() + 0.7152 * color.green() + 0.0722 * color.blue();
}

/// The lit width across the middle row. mpv paints the letterbox bars black, so
/// this is the displayed picture's horizontal span inside its own surface.
int litWidth(const QImage& image) {
    if (image.isNull() || image.height() == 0) return 0;
    const int y = image.height() / 2;
    int lit = 0;
    for (int x = 0; x < image.width(); ++x) {
        if (pixelLuma(image.pixelColor(x, y)) > 16.0) ++lit;
    }
    return lit;
}

int litHeight(const QImage& image) {
    if (image.isNull() || image.width() == 0) return 0;
    const int x = image.width() / 2;
    int lit = 0;
    for (int y = 0; y < image.height(); ++y) {
        if (pixelLuma(image.pixelColor(x, y)) > 16.0) ++lit;
    }
    return lit;
}

double meanLuma(const QImage& image, const QRect& rect) {
    const QRect area = rect.intersected(image.rect());
    if (area.isEmpty()) return -1.0;
    double total = 0.0;
    int count = 0;
    for (int y = area.top(); y < area.bottom(); y += 2) {
        for (int x = area.left(); x < area.right(); x += 2) {
            total += pixelLuma(image.pixelColor(x, y));
            ++count;
        }
    }
    return count > 0 ? total / count : -1.0;
}

double centerLuma(const QImage& image) {
    if (image.isNull()) return -1.0;
    return meanLuma(image, QRect(image.width() / 2 - 24, image.height() / 2 - 16, 48, 32));
}

/// Mean absolute per-channel difference between two same-sized frames. A stale
/// grab compares equal, which is what makes it useful as a settle predicate.
double meanAbsDiff(const QImage& a, const QImage& b) {
    if (a.isNull() || b.isNull() || a.size() != b.size()) return -1.0;
    double total = 0.0;
    int count = 0;
    for (int y = 0; y < a.height(); y += 3) {
        for (int x = 0; x < a.width(); x += 3) {
            const QColor left = a.pixelColor(x, y);
            const QColor right = b.pixelColor(x, y);
            total += std::abs(left.red() - right.red()) + std::abs(left.green() - right.green()) +
                     std::abs(left.blue() - right.blue());
            count += 3;
        }
    }
    return count > 0 ? total / count : -1.0;
}

QSize fitBoxFor(double aspect, const QSize& area) {
    if (area.isEmpty() || aspect <= 0.05) return {};
    if (double(area.width()) / area.height() > aspect) {
        return {static_cast<int>(area.height() * aspect), area.height()};
    }
    return {area.width(), static_cast<int>(area.width() / aspect)};
}

bool withinPixelTolerance(int measured, int expected, int tolerance) {
    return expected > 0 && std::abs(measured - expected) <= tolerance;
}

QSize itemSize(const QQuickItem& item) {
    return QSize(qRound(item.width()), qRound(item.height()));
}

QJsonObject rectJson(const QRect& rect) {
    QJsonObject object;
    object.insert(QStringLiteral("x"), rect.x());
    object.insert(QStringLiteral("y"), rect.y());
    object.insert(QStringLiteral("width"), rect.width());
    object.insert(QStringLiteral("height"), rect.height());
    return object;
}

QJsonObject sizeJson(const QSize& size) {
    QJsonObject object;
    object.insert(QStringLiteral("width"), size.width());
    object.insert(QStringLiteral("height"), size.height());
    return object;
}

QJsonObject policyJson(const VideoScalePolicy& policy) {
    QJsonObject object;
    object.insert(QStringLiteral("mode"), static_cast<int>(policy.mode));
    object.insert(QStringLiteral("modeLabel"), mplayer::videoScaleModeLabel(policy.mode));
    object.insert(QStringLiteral("zoomPercent"), policy.zoomPercent);
    object.insert(QStringLiteral("upscale"), policy.upscale);
    object.insert(QStringLiteral("downscale"), policy.downscale);
    return object;
}

VideoScalePolicy policyFor(VideoScaleMode mode, int zoomPercent,
                           const QString& upscale, const QString& downscale) {
    VideoScalePolicy policy;
    policy.mode = mode;
    policy.zoomPercent = zoomPercent;
    policy.upscale = upscale;
    policy.downscale = downscale;
    return policy;
}

struct RefusalLog {
    int count = 0;
    QStringList reasons;
};

/// One observed state of the live player. Geometry comes from the real shell and
/// its Quick video item; the span comes from the rendered framebuffer.
struct Observation {
    QString label;
    QSize shellSize;
    QRect surfaceRect;
    QSize video;
    QSize grab;
    int span = -1;
    int spanHeight = -1;
    double center = -1.0;
    double position = -1.0;
    bool playing = false;
    bool pausedObserved = false;
    QJsonObject extra;

    QJsonObject json() const {
        QJsonObject object;
        object.insert(QStringLiteral("label"), label);
        object.insert(QStringLiteral("shellSize"), sizeJson(shellSize));
        object.insert(QStringLiteral("surfaceRect"), rectJson(surfaceRect));
        object.insert(QStringLiteral("video"), sizeJson(video));
        object.insert(QStringLiteral("grab"), sizeJson(grab));
        object.insert(QStringLiteral("span"), span);
        object.insert(QStringLiteral("spanHeight"), spanHeight);
        object.insert(QStringLiteral("centerLuma"), center);
        object.insert(QStringLiteral("position"), position);
        object.insert(QStringLiteral("playing"), playing);
        object.insert(QStringLiteral("pausedObserved"), pausedObserved);
        for (auto it = extra.constBegin(); it != extra.constEnd(); ++it) {
            object.insert(it.key(), it.value());
        }
        return object;
    }
};

Observation observe(const PlayerWindow& window, const VideoSession& session,
                    const QQuickItem& video, const QString& label) {
    Observation observation;
    observation.label = label;
    observation.shellSize = window.size();
    const QRectF geometry = video.mapRectToScene(video.boundingRect());
    observation.surfaceRect = geometry.toRect();
    observation.video = QSize(session.videoWidth(), session.videoHeight());
    observation.position = session.position();
    observation.playing = session.isPlaying();
    observation.pausedObserved = session.pauseObserved();
    return observation;
}

void measure(Observation& observation, const QImage& image) {
    observation.grab = image.size();
    observation.span = litWidth(image);
    observation.spanHeight = litHeight(image);
    observation.center = centerLuma(image);
}

/// Grabs until the frame satisfies `accepts`, so a mode change that is still in
/// flight cannot be measured from the previous mode's pixels.
QImage grabMatching(PlayerWindow& window, const std::function<bool(const QImage&)>& accepts,
                    int timeoutMs = 3000) {
    QImage image;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        image = window.grabVideoFrame();
        if (!image.isNull() && accepts(image)) return image;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(15);
    }
    return window.grabVideoFrame();
}

bool saveImage(const QImage& image, const QString& path, QStringList* written) {
    if (image.isNull()) return false;
    if (!image.save(path)) return false;
    if (written != nullptr) written->append(path);
    return true;
}

QPushButton* acceptButton(QDialog& dialog) {
    for (QAbstractButton* button : dialog.findChildren<QAbstractButton*>()) {
        if (auto* box = qobject_cast<QDialogButtonBox*>(button->parentWidget())) {
            if (box->buttonRole(button) == QDialogButtonBox::AcceptRole) {
                return qobject_cast<QPushButton*>(button);
            }
        }
    }
    return nullptr;
}

QPushButton* rejectButton(QDialog& dialog) {
    for (QAbstractButton* button : dialog.findChildren<QAbstractButton*>()) {
        if (auto* box = qobject_cast<QDialogButtonBox*>(button->parentWidget())) {
            if (box->buttonRole(button) == QDialogButtonBox::RejectRole) {
                return qobject_cast<QPushButton*>(button);
            }
        }
    }
    return nullptr;
}

bool setScalingControls(SettingsDialog& dialog, const VideoScalePolicy& policy) {
    auto* mode = dialog.findChild<QComboBox*>(QStringLiteral("videoScaleModeCombo"));
    auto* zoom = dialog.findChild<QSpinBox*>(QStringLiteral("videoZoomSpin"));
    auto* upscale = dialog.findChild<QComboBox*>(QStringLiteral("videoUpscalerCombo"));
    auto* downscale = dialog.findChild<QComboBox*>(QStringLiteral("videoDownscalerCombo"));
    if (mode == nullptr || zoom == nullptr || upscale == nullptr || downscale == nullptr) {
        return false;
    }
    const int modeIndex = mode->findData(static_cast<int>(policy.mode));
    const int upIndex = upscale->findData(policy.upscale);
    const int downIndex = downscale->findData(policy.downscale);
    if (modeIndex < 0 || upIndex < 0 || downIndex < 0) return false;
    mode->setCurrentIndex(modeIndex);
    zoom->setValue(policy.zoomPercent);
    upscale->setCurrentIndex(upIndex);
    downscale->setCurrentIndex(downIndex);
    QApplication::processEvents();
    return true;
}

struct LiveContext {
    PlayerController* controller = nullptr;
    PlayerWindow* window = nullptr;
    VideoSession* session = nullptr;
    QQuickItem* videoItem = nullptr;
    QSize video;
    QSize pictureArea;
    QSize fitSize;
    double dpr = 1.0;
    QString outputDir;
    QString language;
    QJsonArray settingsCard;
    RefusalLog refusals;
    QJsonArray observations;
    QJsonArray artifacts;
    QStringList written;
};

QString shotPath(const LiveContext& context, const QString& stem) {
    return QDir(context.outputDir)
        .filePath(QStringLiteral("live-video-scaling-") + stem + QStringLiteral(".png"));
}

/// Saves the rendered framebuffer (authoritative, occlusion-free) and the
/// window's own visible pixels through the screen's window grab. The screen grab
/// is tagged with the window's exposure so an occluded capture cannot read as
/// visible evidence.
QJsonObject captureArtifacts(LiveContext& context, const QString& stem) {
    const QImage frame = context.window->grabVideoFrame();
    saveImage(frame, shotPath(context, stem + QStringLiteral("-surface")), &context.written);

    QJsonObject record;
    record.insert(QStringLiteral("stem"), stem);
    record.insert(QStringLiteral("frame"), sizeJson(frame.size()));
    record.insert(QStringLiteral("windowExposed"), context.window->isExposed());
    const QImage asImage = context.window->grabWindow();
    record.insert(QStringLiteral("windowGrab"), sizeJson(asImage.size()));
    record.insert(QStringLiteral("windowGrabSaved"),
                  saveImage(asImage, shotPath(context, stem + QStringLiteral("-window")),
                            &context.written));
    context.artifacts.append(record);
    return record;
}

/// Opens the real Settings dialog, drives the actual scaling controls and then
/// clicks its real Save or Cancel button. Never calls a slot directly.
bool settingsDecision(LiveContext& context, mikai::backend::MikaiClient& client,
                      mikai::app::Runner& runner, const VideoScalePolicy& policy,
                      bool accept, const QString& label) {
    SettingsDialog dialog(client, runner, nullptr);
    dialog.setAttribute(Qt::WA_ShowWithoutActivating, true);
    dialog.showPage(QStringLiteral("access"));
    dialog.show();
    QApplication::processEvents();
    check(dialog.isVisible(), label + QStringLiteral(": the real Settings dialog is visible"));
    const bool set = setScalingControls(dialog, policy);
    check(set, label + QStringLiteral(": the real scaling controls offer every requested value"));
    // The probe's seeded store carries a foreign interface-language preference;
    // Save commits the language before it reaches the scaling block, so point the
    // real selector at the language this process actually installed. No scale
    // field and no control under test is involved.
    if (auto* language = dialog.findChild<QComboBox*>(QStringLiteral("uiLanguageCombo"))) {
        const int installed = language->findData(mikai::app::activeInterfaceLanguage());
        if (installed >= 0) {
            language->setCurrentIndex(installed);
            QApplication::processEvents();
        }
    }
    {
        QJsonObject dialogRecord;
        dialogRecord.insert(QStringLiteral("stem"), label + QStringLiteral("-dialog"));
        dialogRecord.insert(QStringLiteral("width"), dialog.width());
        dialogRecord.insert(QStringLiteral("height"), dialog.height());
        dialogRecord.insert(QStringLiteral("devicePixelRatio"), dialog.devicePixelRatioF());
        const bool saved = saveImage(
            dialog.grab().toImage(), shotPath(context, label + QStringLiteral("-dialog")),
            &context.written);
        dialogRecord.insert(QStringLiteral("screenshot"), saved);
        context.artifacts.append(dialogRecord);
    }
    QPushButton* button = accept ? acceptButton(dialog) : rejectButton(dialog);
    check(button != nullptr, label + (accept ? QStringLiteral(": the real Save button exists")
                                             : QStringLiteral(": the real Cancel button exists")));
    if (button != nullptr) button->click();
    QApplication::processEvents();
    // Since 25bc266 ("Let Save commit without closing Settings"), Save commits
    // without closing: the dialog stays Rejected and the button shows the
    // "Saved" acknowledgment while disabled. The text is read through the
    // SettingsDialog translation context so the check holds in every locale.
    // The Cancel path is unchanged: it rejects the dialog.
    const bool committed =
        accept ? dialog.result() == static_cast<int>(QDialog::Rejected) && button != nullptr &&
                     button->text() ==
                         QCoreApplication::translate("mikai::app::SettingsDialog", "Saved") &&
                     !button->isEnabled()
               : dialog.result() == static_cast<int>(QDialog::Rejected);
    check(committed, label + (accept ? QStringLiteral(": Save commits without closing the dialog")
                                      : QStringLiteral(": Cancel rejected the dialog")));
    captureArtifacts(context,
                     label + (accept ? QStringLiteral("-save") : QStringLiteral("-cancel")));
    dialog.reject();
    QApplication::processEvents();
    return committed;
}

bool clickFlyoutMode(LiveContext& context, VideoScaleMode mode, const QString& label) {
    context.controller->noteUserActivity();
    Q_EMIT context.controller->menuOpenRequested(QStringLiteral("settings"));
    QQuickItem* panel = nullptr;
    const bool visible = pumpUntil([&] {
        if (context.window->rootObject() == nullptr) return false;
        for (QQuickItem* item : context.window->rootObject()->findChildren<QQuickItem*>()) {
            if (item->property("menuId").toString() == QLatin1String("settings") &&
                item->isVisible()) {
                panel = item;
                return true;
            }
        }
        return false;
    }, 3000);
    check(visible, label + QStringLiteral(": the in-scene settings flyout is visible"));
    mplayer::quick::PlayerMenuModel* model =
        context.controller->menuModel(QStringLiteral("settings"));
    const auto rowFor = [model](VideoScaleMode wanted) {
        if (model == nullptr) return -1;
        for (int i = 0; i < model->rowCount(); ++i) {
            const auto entry = model->row(i);
            if (entry.action == mplayer::quick::PlayerMenuModel::Action::SetVideoScaleMode &&
                entry.value.toInt() == static_cast<int>(wanted)) {
                return i;
            }
        }
        return -1;
    };
    const int row = rowFor(mode);
    check(visible && row >= 0,
          label + QStringLiteral(": the flyout model offers this display mode row"));
    if (!visible || row < 0) return false;

    // A Settings Save publishes the full policy through VideoSession but does
    // not rewrite the controller's menu cache. If its cached mode already
    // equals the requested one while the live policy differs, take one real
    // row through another mode before the requested row so the same typed
    // action path adopts the policy that Settings just published.
    const auto activateFlyoutRow = [panel](int rowIndex, int action) {
        return QMetaObject::invokeMethod(
            panel, "activateRow", Qt::DirectConnection,
            Q_ARG(QVariant, QVariant(rowIndex)), Q_ARG(QVariant, QVariant(action)));
    };
    if (VideoSession::videoScalePolicy().mode != mode &&
        context.controller->videoScaleMode() == static_cast<int>(mode)) {
        const VideoScaleMode intermediate =
            mode == VideoScaleMode::Fit ? VideoScaleMode::Fill : VideoScaleMode::Fit;
        const int intermediateRow = rowFor(intermediate);
        if (intermediateRow < 0) return false;
        const auto intermediateAction = model->row(intermediateRow).action;
        check(activateFlyoutRow(intermediateRow, static_cast<int>(intermediateAction)),
              label + QStringLiteral(": the QML flyout applied its intermediate row"));
        QCoreApplication::processEvents();
    }
    const auto action = model->row(row).action;
    const bool activated = activateFlyoutRow(row, static_cast<int>(action));
    check(activated, label + QStringLiteral(": the visible QML flyout action was invoked"));
    QCoreApplication::processEvents();
    return activated;
}

/// Checks one display mode: the shell surface box and the rendered span. The
/// expectations are derived from the observed picture area and video geometry,
/// never from the bare-surface test's absolute numbers.
bool verifyMode(LiveContext& context, const QString& label, VideoScaleMode mode,
                const QSize& expectedSurface, const std::function<bool(int)>& spanAccepts,
                int* spanOut = nullptr) {
    const bool applied = pumpUntil(
        [&] {
            return VideoSession::videoScalePolicy().mode == mode &&
                   itemSize(*context.videoItem) == expectedSurface;
        },
        5000);
    check(applied, label + QStringLiteral(": the real shell surface reaches this mode's box"));
    if (!applied) {
        Observation observation =
            observe(*context.window, *context.session, *context.videoItem, label);
        observation.extra.insert(QStringLiteral("expectedSurface"), sizeJson(expectedSurface));
        context.observations.append(observation.json());
        return false;
    }
    QImage image;
    const bool matched = pumpUntil(
        [&] {
            image = context.window->grabVideoFrame();
            if (image.isNull()) return false;
            const int span = litWidth(image);
            return spanAccepts(span);
        },
        4000);
    Observation observation =
        observe(*context.window, *context.session, *context.videoItem, label);
    measure(observation, image);
    observation.extra.insert(QStringLiteral("expectedSurface"), sizeJson(expectedSurface));
    observation.extra.insert(QStringLiteral("policy"), policyJson(VideoSession::videoScalePolicy()));
    context.observations.append(observation.json());
    if (spanOut != nullptr) *spanOut = observation.span;
    check(matched, label + QStringLiteral(": the rendered picture reaches this mode's span"),
          QStringLiteral("span=%1 grab=%2x%3").arg(observation.span).arg(observation.grab.width()).arg(observation.grab.height()));
    // Keep our own flyout out of the window grab; the rendered framebuffer above
    // is the authoritative picture either way.
    context.controller->closeMenus();
    QApplication::processEvents();
    captureArtifacts(context, label);
    return matched;
}

/// Measures the real General card at the three acceptance widths GOAL-Z names.
/// The language is whatever this process installed from MIKAI_PROBE_LANGUAGE, so
/// one run is single-language evidence and says which language it covers.
void measureSettingsCard(LiveContext& context, mikai::backend::MikaiClient& client,
                         mikai::app::Runner& runner) {
    SettingsDialog dialog(client, runner, nullptr);
    dialog.setAttribute(Qt::WA_ShowWithoutActivating, true);
    dialog.showPage(QStringLiteral("access"));
    dialog.show();
    QApplication::processEvents();

    auto* scroll = dialog.findChild<QScrollArea*>(QStringLiteral("nativeGeneralScroll"));
    auto* mode = dialog.findChild<QComboBox*>(QStringLiteral("videoScaleModeCombo"));
    auto* zoom = dialog.findChild<QSpinBox*>(QStringLiteral("videoZoomSpin"));
    auto* upscale = dialog.findChild<QComboBox*>(QStringLiteral("videoUpscalerCombo"));
    auto* downscale = dialog.findChild<QComboBox*>(QStringLiteral("videoDownscalerCombo"));
    const bool controlsExist = scroll != nullptr && mode != nullptr && zoom != nullptr &&
                               upscale != nullptr && downscale != nullptr;
    check(controlsExist,
          QStringLiteral("the real General page exposes the scaling card for the width matrix"));
    if (!controlsExist) {
        dialog.reject();
        QApplication::processEvents();
        return;
    }
    QWidget* card = mode->parentWidget();
    check(dialog.currentPageId() == QStringLiteral("access"),
          QStringLiteral("the General page is the selected page for the width matrix"));

    for (const int width : {860, 1000, 1280}) {
        dialog.resize(width, dialog.height());
        QApplication::processEvents();
        mikai::probes::revealWidget(card);
        QApplication::processEvents();

        const QRect cardRect(card->mapTo(scroll->viewport(), QPoint()), card->size());
        bool captionsFit = true;
        for (QLabel* label : card->findChildren<QLabel*>()) {
            if (!label->isVisibleTo(&dialog)) continue;
            captionsFit = captionsFit && label->height() >= label->heightForWidth(label->width());
        }
        const bool controlsVisible = mode->isVisibleTo(&dialog) && zoom->isVisibleTo(&dialog) &&
                                     upscale->isVisibleTo(&dialog) &&
                                     downscale->isVisibleTo(&dialog);
        const bool cardFullyVisible = scroll->viewport()->rect().contains(cardRect);
        const bool noHorizontalScroll = scroll->horizontalScrollBar()->maximum() == 0;
        const bool actionsFit =
            mikai::probes::dialogActionsFit(dialog, QSize(dialog.width(), dialog.height()));

        const QString stem = QStringLiteral("settings-%1-%2").arg(context.language).arg(width);
        bool saved = false;
        if (QImage shot = dialog.grab().toImage(); !shot.isNull()) {
            saved = saveImage(shot, shotPath(context, stem), &context.written);
        }
        QJsonObject record;
        record.insert(QStringLiteral("requestedWidth"), width);
        record.insert(QStringLiteral("width"), dialog.width());
        record.insert(QStringLiteral("height"), dialog.height());
        record.insert(QStringLiteral("devicePixelRatio"), dialog.devicePixelRatioF());
        record.insert(QStringLiteral("language"), context.language);
        record.insert(QStringLiteral("card"), rectJson(cardRect));
        record.insert(QStringLiteral("modeControl"), rectJson(QRect(mode->mapTo(scroll->viewport(), QPoint()), mode->size())));
        record.insert(QStringLiteral("zoomControl"), rectJson(QRect(zoom->mapTo(scroll->viewport(), QPoint()), zoom->size())));
        record.insert(QStringLiteral("upscalerControl"), rectJson(QRect(upscale->mapTo(scroll->viewport(), QPoint()), upscale->size())));
        record.insert(QStringLiteral("downscalerControl"), rectJson(QRect(downscale->mapTo(scroll->viewport(), QPoint()), downscale->size())));
        record.insert(QStringLiteral("controlsVisible"), controlsVisible);
        record.insert(QStringLiteral("cardFullyVisible"), cardFullyVisible);
        record.insert(QStringLiteral("captionsFit"), captionsFit);
        record.insert(QStringLiteral("horizontalScrollMaximum"),
                      scroll->horizontalScrollBar()->maximum());
        record.insert(QStringLiteral("actionsFit"), actionsFit);
        record.insert(QStringLiteral("screenshot"), saved);
        context.settingsCard.append(record);
        check(captionsFit && controlsVisible && cardFullyVisible && noHorizontalScroll &&
                  actionsFit,
              QStringLiteral("the scaling card fits the %1 px General page in %2")
                  .arg(width)
                  .arg(context.language),
              QStringLiteral("card=%1,%2 %3x%4 hscroll=%5")
                  .arg(cardRect.x()).arg(cardRect.y())
                  .arg(cardRect.width()).arg(cardRect.height())
                  .arg(scroll->horizontalScrollBar()->maximum()));
    }
    dialog.reject();
    QApplication::processEvents();
}

QJsonObject liveEvidence(const LiveContext& context, const VideoScalingLiveOptions& options,
                         bool restartPassed, int restartExit,
                         const QString& restartOutput) {
    QJsonObject evidence;
    evidence.insert(QStringLiteral("scenario"), QStringLiteral("video_scaling_live"));
    evidence.insert(QStringLiteral("platform"), QGuiApplication::platformName());
    evidence.insert(QStringLiteral("language"), context.language);
    evidence.insert(QStringLiteral("media"), options.mediaPath);
    evidence.insert(QStringLiteral("evidencePath"), QDir::cleanPath(options.evidencePath));
    evidence.insert(QStringLiteral("video"), sizeJson(context.video));
    evidence.insert(QStringLiteral("devicePixelRatio"), context.dpr);
    evidence.insert(QStringLiteral("pictureArea"), sizeJson(context.pictureArea));
    evidence.insert(QStringLiteral("fitBox"), sizeJson(context.fitSize));
    evidence.insert(QStringLiteral("settingsCard"), context.settingsCard);
    evidence.insert(
        QStringLiteral("languageScope"),
        QStringLiteral("The Settings card matrix covers only the interface language this "
                       "run installed (MIKAI_PROBE_LANGUAGE); the other language needs its "
                       "own run with its catalog."));
    evidence.insert(QStringLiteral("displayScalingRefusals"), context.refusals.count);
    evidence.insert(QStringLiteral("refusalReasons"),
                    QJsonArray::fromStringList(context.refusals.reasons));
    evidence.insert(QStringLiteral("observations"), context.observations);
    evidence.insert(QStringLiteral("artifacts"), context.artifacts);
    evidence.insert(QStringLiteral("screenshots"), QJsonArray::fromStringList(context.written));
    evidence.insert(QStringLiteral("restartReaderVerified"), restartPassed);
    evidence.insert(QStringLiteral("restartReaderExitCode"), restartExit);
    evidence.insert(QStringLiteral("restartReaderOutput"), restartOutput);
    evidence.insert(QStringLiteral("holdMs"), options.holdMs);
    evidence.insert(QStringLiteral("checks"), checks);
    evidence.insert(QStringLiteral("failures"), failures);
    evidence.insert(QStringLiteral("passed"), failures == 0 && restartPassed);
    return evidence;
}

bool writeEvidence(const QJsonObject& evidence, const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = file.errorString();
        return false;
    }
    const QByteArray payload = QJsonDocument(evidence).toJson(QJsonDocument::Indented);
    const bool wrote = file.write(payload) == payload.size();
    file.close();
    if (!wrote) *error = QStringLiteral("short write");
    return wrote;
}

}  // namespace

int runVideoScalingLiveSmoke(mikai::backend::MikaiClient& client,
                             mikai::app::Runner& runner,
                             const VideoScalingLiveOptions& options) {
    failures = 0;
    checks = 0;

    const QString platform = QGuiApplication::platformName();
    check(platform != QLatin1String("offscreen") && platform != QLatin1String("minimal"),
          QStringLiteral("a native Qt platform is active, so the run shows real windows"),
          platform);
    const QFileInfo media(options.mediaPath);
    check(media.isAbsolute() && media.isFile(),
          QStringLiteral("the media argument is an absolute existing file"), options.mediaPath);
    check(options.holdMs >= 0 && options.holdMs <= 600000,
          QStringLiteral("the optional visible hold is within 0..600000 ms"));
    const QFileInfo evidenceFileInfo(options.evidencePath);
    check(evidenceFileInfo.isAbsolute() && evidenceFileInfo.absoluteDir().exists(),
          QStringLiteral("--video-scaling-evidence is an absolute file in an existing directory"),
          options.evidencePath);
    if (failures > 0) {
        LiveContext empty;
        empty.outputDir = evidenceFileInfo.absolutePath();
        QJsonObject evidence = liveEvidence(empty, options, false, -1, QString());
        QString error;
        if (!writeEvidence(evidence, options.evidencePath, &error)) {
            std::fprintf(stderr, "FAIL the live evidence was written: %s\n", qPrintable(error));
            return 2;
        }
        return 2;
    }
    // Installed-bundle discovery cannot find the launcher's disposable module copy.
    bool runtimeReady = false;
    if (!qEnvironmentVariableIsEmpty("MIKAI_PROBE_BUNDLE")) {
        runtimeReady = mplayer::probes::selectRuntime();
        check(runtimeReady, QStringLiteral("the named MIKAI_PROBE_BUNDLE selects the runtime"));
    } else {
        const QString stagedModule = qEnvironmentVariable("MIKAI_MPV_MODULE");
        const QFileInfo stagedInfo(stagedModule);
        if (stagedInfo.isAbsolute() && stagedInfo.isFile()) {
            const mplayer::native::RuntimeSelection runtime =
                mplayer::native::selectStandardRuntime(stagedModule);
            mplayer::probes::detail::reportSelected(runtime, "launcher-staged module");
            runtimeReady = runtime.loaded;
            check(runtimeReady, QStringLiteral("the launcher-staged pinned mpv module loads"),
                  runtime.reason.isEmpty() ? stagedModule : runtime.reason);
        } else {
            check(false, QStringLiteral("the launcher staged an absolute mpv module"),
                  stagedModule.isEmpty() ? QStringLiteral("MIKAI_MPV_MODULE is unset")
                                         : stagedModule);
        }
    }
    if (!runtimeReady) {
        LiveContext empty;
        empty.outputDir = evidenceFileInfo.absolutePath();
        QJsonObject evidence = liveEvidence(empty, options, false, -1, QString());
        QString error;
        if (!writeEvidence(evidence, options.evidencePath, &error)) {
            std::fprintf(stderr, "FAIL the live evidence was written: %s\n", qPrintable(error));
        }
        return 2;
    }

    // The scale keys are the subject; a default Anime4K chain would change the
    // pixels the span checks read without changing the geometry under test. The
    // same handle then reads the opening scaling policy, so this adds no second
    // store instance.
    VideoScalePolicy opening;
    {
        QSettings settings;
        settings.setValue(QString::fromLatin1(mikai::app::kUiLanguageSettingKey),
                          mikai::app::activeInterfaceLanguage());
        settings.setValue(QStringLiteral("player/anime4k"), QStringLiteral("off"));
        settings.sync();
        opening = mplayer::videoScalePolicyFromSettings(settings);
    }
    check(opening == VideoScalePolicy{},
          QStringLiteral("the owned, empty store starts on the documented default policy"));
    VideoSession::setVideoScalePolicy(opening);

    // The probe's existing size convention, not a parallel one: MIKAI_PROBE_WIDTH
    // / MIKAI_PROBE_HEIGHT size the real player window, and the same width is
    // what the Settings card matrix records.
    const QSize liveWindow = mikai::probes::windowSize(QSize(1280, 760));
    LiveContext context;
    auto controller = std::make_unique<PlayerController>(
        QStringLiteral("Video scaling live smoke"), media.absoluteFilePath());
    context.controller = controller.get();
    context.window = controller->window();
    // The window is the presentation host in both windowed and PiP modes;
    // obtain the session it actually presents instead of independently
    // resolving two controller-owned pointers that could disagree.
    context.session = context.window == nullptr ? nullptr : context.window->session();
    context.outputDir = evidenceFileInfo.absolutePath();
    context.language = mikai::app::activeInterfaceLanguage();
    const bool canonicalHost =
        context.window != nullptr && context.session != nullptr &&
        controller->window() == context.window && controller->session() == context.session;
    check(canonicalHost,
          QStringLiteral("the real Quick controller and PlayerWindow resolve one VideoSession"));
    if (!canonicalHost) {
        LiveContext empty;
        QJsonObject evidence = liveEvidence(empty, options, false, -1, QString());
        QString error;
        writeEvidence(evidence, options.evidencePath, &error);
        return 1;
    }
    context.window->resize(liveWindow);
    controller->showWindow();
    controller->startPlayer();
    context.videoItem = context.window->videoItem();
    check(context.videoItem != nullptr,
          QStringLiteral("the real PlayerWindow exposes its in-scene video item"));
    QObject::connect(context.session, &VideoSession::displayScalingRefused, context.session,
                     [&context](const QString& reason) {
                         ++context.refusals.count;
                         context.refusals.reasons.append(reason);
                     });
    if (context.videoItem == nullptr) {
        controller->requestClose();
        LiveContext empty;
        QJsonObject evidence = liveEvidence(empty, options, false, -1, QString());
        QString error;
        writeEvidence(evidence, options.evidencePath, &error);
        return 1;
    }

    const bool framed = pumpUntil(
        [&] {
            if (!context.session->hasVideo() || context.session->videoWidth() <= 0) return false;
            const QImage image = context.window->grabVideoFrame();
            return !image.isNull();
        },
        30000);
    context.video = QSize(context.session->videoWidth(), context.session->videoHeight());
    const double duration = context.session->duration();
    check(framed, QStringLiteral("the real player decodes and renders the media fixture"));
    // A short clip reaches EOS in the middle of the matrix; refuse it instead of
    // letting a half-run read as a pass.
    check(duration >= 60.0,
          QStringLiteral("the fixture is long enough for the whole matrix without reaching EOS"),
          QStringLiteral("duration=%1 s").arg(duration));
    if (!framed || duration < 60.0 || context.video.isEmpty()) {
        controller->requestClose();
        QJsonObject evidence = liveEvidence(context, options, false, -1, QString());
        QString error;
        writeEvidence(evidence, options.evidencePath, &error);
        return 1;
    }
    const bool startupReady = pumpUntil(
        [&] { return context.window->isVisible() && (context.window->visibility() == QWindow::FullScreen); }, 5000);
    check(startupReady,
          QStringLiteral("the player completed its deferred fullscreen startup"));
    if (startupReady) controller->toggleFullscreen();
    const QSize requestedWindow = liveWindow;
    context.window->resize(requestedWindow);
    const bool windowedReady = startupReady && pumpUntil(
        [&] {
            return context.window->isVisible() && !(context.window->visibility() == QWindow::FullScreen) &&
                   context.window->size() == requestedWindow;
        },
        5000);
    check(windowedReady,
          QStringLiteral("the real window-mode action established the requested windowed matrix"),
          QStringLiteral("requested=%1x%2 actual=%3x%4 fullscreen=%5")
              .arg(requestedWindow.width()).arg(requestedWindow.height())
              .arg(context.window->width()).arg(context.window->height())
              .arg((context.window->visibility() == QWindow::FullScreen)));
    if (!windowedReady) {
        controller->requestClose();
        QJsonObject evidence = liveEvidence(context, options, false, -1, QString());
        QString error;
        if (!writeEvidence(evidence, options.evidencePath, &error)) {
            std::fprintf(stderr, "FAIL the live evidence was written: %s\n", qPrintable(error));
        }
        return 1;
    }
    context.dpr = context.window->devicePixelRatio();
    const double videoAspect = double(context.video.width()) / context.video.height();
    check(videoAspect > 0.5 && videoAspect < 3.0,
          QStringLiteral("the fixture reports a picture aspect the shell can fit"), QString::number(videoAspect));

    if (!context.session->isPlaying()) context.session->setPaused(false);
    check(pumpUntil([&] { return context.session->isPlaying(); }, 5000),
          QStringLiteral("the source is playing before the live matrix starts"));

    // ── Settings Save on a playing surface; Fill also reveals the picture area ──
    const double positionBeforeSave = context.session->position();
    const QSize boxBeforeSave = itemSize(*context.videoItem);
    const VideoScalePolicy fillPolicy =
        policyFor(VideoScaleMode::Fill, 0, QStringLiteral("bicubic"), QStringLiteral("catmull_rom"));
    settingsDecision(context, client, runner, fillPolicy, true, QStringLiteral("live-save-fill"));
    check(VideoSession::videoScalePolicy().mode == VideoScaleMode::Fill,
          QStringLiteral("Save published Fill to the running process"));
    {
        QSettings storedSettings;
        check(mplayer::videoScalePolicyFromSettings(storedSettings) == fillPolicy,
              QStringLiteral("Save stored the complete Fill policy the production reader returns"));
    }
    const bool relaid = pumpUntil(
        [&] { return itemSize(*context.videoItem) != boxBeforeSave; }, 5000);
    check(relaid, QStringLiteral("the running shell re-laid its surface for Fill without a restart"));
    context.pictureArea = itemSize(*context.videoItem);

    Observation fillPlaying = observe(*context.window, *context.session, *context.videoItem, QStringLiteral("fill-playing-save"));
    fillPlaying.extra.insert(QStringLiteral("policy"), policyJson(fillPolicy));
    context.observations.append(fillPlaying.json());

    const double positionAfterSave = context.session->position();
    check(context.session->isPlaying(), QStringLiteral("the live Save did not stop playback"));
    check(positionAfterSave + 0.75 >= positionBeforeSave,
          QStringLiteral("the live Save did not rewind playback"),
          QStringLiteral("before=%1 after=%2").arg(positionBeforeSave).arg(positionAfterSave));

    context.fitSize = fitBoxFor(videoAspect, context.pictureArea);
    check(!context.fitSize.isEmpty() && context.fitSize != context.pictureArea,
          QStringLiteral("this window makes Fit and Fill geometrically distinguishable"),
          QStringLiteral("fit=%1x%2 area=%3x%4")
              .arg(context.fitSize.width()).arg(context.fitSize.height())
              .arg(context.pictureArea.width()).arg(context.pictureArea.height()));

    // ── Flyout rows, still playing ───────────────────────────────────────────

    // Fit: the surface is the aspect box and the picture fills its own surface.
    if (clickFlyoutMode(context, VideoScaleMode::Fit, QStringLiteral("live-flyout-fit"))) {
        const int expectedSpan =
            static_cast<int>(std::lround(context.fitSize.width() * context.dpr));
        verifyMode(context, QStringLiteral("fit"), VideoScaleMode::Fit, context.fitSize,
                   [expectedSpan](int span) {
                       return std::abs(span - expectedSpan) <= std::max(8, expectedSpan / 100);
                   });
    }
    // Stretch: the surface is the whole picture area and the picture fills it.
    if (clickFlyoutMode(context, VideoScaleMode::Stretch, QStringLiteral("live-flyout-stretch"))) {
        verifyMode(context, QStringLiteral("stretch"), VideoScaleMode::Stretch, context.pictureArea,
                   [](int span) { return span > 0; });
    }
    // 4:3 override: the surface keeps the video-aspect box, so mpv pillarboxes
    // the 4:3 picture inside it — the span is the override's width.
    if (clickFlyoutMode(context, VideoScaleMode::Aspect4x3, QStringLiteral("live-flyout-4x3"))) {
        const int expectedFourThree = static_cast<int>(
            std::lround(context.fitSize.height() * context.dpr * 4.0 / 3.0));
        verifyMode(context, QStringLiteral("aspect-4x3"), VideoScaleMode::Aspect4x3,
                   context.fitSize, [expectedFourThree](int span) {
                       return withinPixelTolerance(span, expectedFourThree,
                                                   std::max(10, expectedFourThree / 50));
                   });
    }
    // 16:9 override on a 16:9 source is pixel-identical to Fit; the option still
    // has to be accepted and the box unchanged. Recorded explicitly, not claimed
    // as a visible change.
    if (clickFlyoutMode(context, VideoScaleMode::Aspect16x9, QStringLiteral("live-flyout-16x9"))) {
        verifyMode(context, QStringLiteral("aspect-16x9"), VideoScaleMode::Aspect16x9,
                   context.fitSize, [](int span) { return span > 0; });
    }

    const bool stillPlaying = context.session->isPlaying();
    check(stillPlaying, QStringLiteral("the flyout matrix left playback running"));
    check(context.session->position() > 0.0, QStringLiteral("playback advanced across the flyout matrix"));

    // ── Paused adoption ──────────────────────────────────────────────────────
    context.session->setPaused(true);
    check(pumpUntil([&] { return context.session->pauseObserved(); }, 5000),
          QStringLiteral("the source actually paused before the paused matrix"));
    const double pausedPosition = context.session->position();
    const QSize fitBeforePause = context.fitSize;

    if (clickFlyoutMode(context, VideoScaleMode::Fit, QStringLiteral("live-paused-fit"))) {
        pumpUntil([&] { return itemSize(*context.videoItem) == fitBeforePause; }, 5000);
    }
    const QImage fitPaused = grabMatching(*context.window, [](const QImage& image) {
        return !image.isNull();
    });
    check(!fitPaused.isNull(), QStringLiteral("the paused Fit frame rendered"));

    // A mode change on a paused surface must relayout and repaint, and must not
    // resume or seek.
    const bool pausedFillApplied =
        clickFlyoutMode(context, VideoScaleMode::Fill, QStringLiteral("live-paused-fill"));
    if (pausedFillApplied) {
        const bool relaid = pumpUntil(
            [&] { return itemSize(*context.videoItem) == context.pictureArea; }, 5000);
        check(relaid, QStringLiteral("a paused surface adopts Fill's shell box"));
        const int expectedFillSpan =
            static_cast<int>(std::lround(context.pictureArea.width() * context.dpr));
        const QImage pausedFill = grabMatching(*context.window, [expectedFillSpan](const QImage& image) {
            return std::abs(litWidth(image) - expectedFillSpan) < 16;
        });
        Observation observation = observe(*context.window, *context.session, *context.videoItem, QStringLiteral("fill-paused"));
        measure(observation, pausedFill);
        context.observations.append(observation.json());
        // The box changed, so the two paused frames have different sizes and a
        // direct difference is not meaningful; the repaint evidence is the new
        // box plus a picture that fills it.
        check(!pausedFill.isNull() && withinPixelTolerance(litWidth(pausedFill), expectedFillSpan, 16),
              QStringLiteral("a paused surface repaints the picture after a live mode change"),
              QStringLiteral("span=%1 expected=%2").arg(litWidth(pausedFill)).arg(expectedFillSpan));
        check(!context.session->isPlaying() && context.session->pauseObserved(),
              QStringLiteral("the paused mode change did not resume playback"));
        check(std::abs(context.session->position() - pausedPosition) < 0.35,
              QStringLiteral("the paused mode change did not seek"));
        captureArtifacts(context, QStringLiteral("paused-fill"));
    }

    // ── Original 1:1 (Settings-only) on the paused frame ─────────────────────
    const VideoScalePolicy originalPolicy = policyFor(
        VideoScaleMode::Original, 0, QStringLiteral("bicubic"), QStringLiteral("catmull_rom"));
    settingsDecision(context, client, runner, originalPolicy, true, QStringLiteral("live-save-original"));
    const bool originalBox = pumpUntil(
        [&] { return itemSize(*context.videoItem) == context.fitSize; }, 5000);
    check(originalBox, QStringLiteral("Original keeps the shell's video-aspect box"));
    const QImage originalPaused = grabMatching(*context.window, [&](const QImage& image) {
        return meanAbsDiff(image, fitPaused) > 3.0;
    });
    Observation originalObservation =
        observe(*context.window, *context.session, *context.videoItem, QStringLiteral("original-paused"));
    measure(originalObservation, originalPaused);
    {
        const int grabWidth = originalPaused.width();
        const int expected = std::min(grabWidth, context.video.width());
        originalObservation.extra.insert(QStringLiteral("expectedUnscaledSpan"), expected);
        context.observations.append(originalObservation.json());
        if (expected < grabWidth - 8) {
            check(withinPixelTolerance(originalObservation.span, expected,
                                       std::max(10, expected / 50)),
                  QStringLiteral("Original renders the picture at its own device-pixel width"),
                  QStringLiteral("span=%1 expected=%2 grab=%3")
                      .arg(originalObservation.span).arg(expected).arg(grabWidth));
        } else {
            // The window is narrower than the video, so 1:1 is a crop and its
            // span equals the surface: recorded, never claimed as a size proof.
            check(originalObservation.span > 0,
                  QStringLiteral("Original rendered (1:1 span is not separable in this window)"),
                  QStringLiteral("span=%1 grab=%2 video=%3")
                      .arg(originalObservation.span).arg(grabWidth).arg(context.video.width()));
        }
    }
    check(meanAbsDiff(fitPaused, originalPaused) > 3.0,
          QStringLiteral("Original changed the rendered picture while geometry stayed"));
    check(!context.session->isPlaying() && std::abs(context.session->position() - pausedPosition) < 0.35,
          QStringLiteral("Original did not resume or seek the paused source"));
    captureArtifacts(context, QStringLiteral("original-paused"));

    // ── Zoom on the paused frame ─────────────────────────────────────────────
    settingsDecision(context, client, runner,
                     policyFor(VideoScaleMode::Fit, 0, QStringLiteral("bicubic"),
                               QStringLiteral("catmull_rom")),
                     true, QStringLiteral("live-save-fit"));
    check(pumpUntil([&] { return itemSize(*context.videoItem) == context.fitSize; }, 5000),
          QStringLiteral("Fit restored the aspect box"));
    // The core dithers, so two renders of the same paused frame are not
    // byte-identical; 12 is far below the difference a scaling change makes.
    const QImage fitRestored = grabMatching(*context.window, [&](const QImage& image) {
        return !fitPaused.isNull() && meanAbsDiff(image, fitPaused) < 12.0;
    });

    settingsDecision(context, client, runner,
                     policyFor(VideoScaleMode::Fit, 50, QStringLiteral("bicubic"),
                               QStringLiteral("catmull_rom")),
                     true, QStringLiteral("live-save-zoom"));
    const QImage zoomedPaused = grabMatching(*context.window, [&](const QImage& image) {
        return !fitRestored.isNull() && meanAbsDiff(image, fitRestored) > 3.0;
    });
    Observation zoomObservation = observe(*context.window, *context.session, *context.videoItem, QStringLiteral("fit-zoom50-paused"));
    measure(zoomObservation, zoomedPaused);
    {
        // Expected: a point r from the centre in the zoomed frame is the point
        // r/1.5 in the unzoomed one. Nearest-match within 2 px absorbs rounding
        // and GL filtering without accepting an unscaled frame.
        int matches = 0;
        int samples = 0;
        int identity = 0;
        const double factor = 1.5;
        const int cx = zoomedPaused.width() / 2;
        const int cy = zoomedPaused.height() / 2;
        const int maxRadius = std::min(cx, cy) / 2;
        for (int radius = maxRadius / 3; radius >= maxRadius / 8; radius -= 6) {
            for (int step = 0; step < 16; ++step) {
                const double angle = 2.0 * kPi * step / 16.0;
                const int zx = cx + int(std::lround(radius * std::cos(angle)));
                const int zy = cy + int(std::lround(radius * std::sin(angle)));
                const int sx = cx + int(std::lround(radius / factor * std::cos(angle)));
                const int sy = cy + int(std::lround(radius / factor * std::sin(angle)));
                if (zx < 0 || zy < 0 || zx >= zoomedPaused.width() || zy >= zoomedPaused.height()) continue;
                if (sx < 0 || sy < 0 || sx >= fitRestored.width() || sy >= fitRestored.height()) continue;
                const QColor zoomed = zoomedPaused.pixelColor(zx, zy);
                const QColor straight = fitRestored.pixelColor(zx, zy);
                ++samples;
                double best = 1e9;
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int x = std::clamp(sx + dx, 0, fitRestored.width() - 1);
                        const int y = std::clamp(sy + dy, 0, fitRestored.height() - 1);
                        const QColor reference = fitRestored.pixelColor(x, y);
                        best = std::min(best, double(std::abs(zoomed.red() - reference.red()) +
                                                    std::abs(zoomed.green() - reference.green()) +
                                                    std::abs(zoomed.blue() - reference.blue())));
                    }
                }
                if (best <= 45.0) ++matches;
                if (std::abs(zoomed.red() - straight.red()) +
                        std::abs(zoomed.green() - straight.green()) +
                        std::abs(zoomed.blue() - straight.blue()) <=
                    45) {
                    ++identity;
                }
            }
        }
        const double matchRate = samples > 0 ? double(matches) / samples : 0.0;
        const double identityRate = samples > 0 ? double(identity) / samples : 1.0;
        zoomObservation.extra.insert(QStringLiteral("zoomMatchRate"), matchRate);
        zoomObservation.extra.insert(QStringLiteral("zoomIdentityRate"), identityRate);
        zoomObservation.extra.insert(QStringLiteral("samples"), samples);
        check(samples >= 24, QStringLiteral("the paused zoom frame offered enough samples"));
        check(matchRate >= 0.5 && matchRate > identityRate + 0.10,
              QStringLiteral("the paused +50% zoom enlarged the picture by the expected factor"),
              QStringLiteral("match=%1 identity=%2 samples=%3")
                  .arg(matchRate, 0, 'f', 3).arg(identityRate, 0, 'f', 3).arg(samples));
    }
    context.observations.append(zoomObservation.json());
    check(!context.session->isPlaying() && std::abs(context.session->position() - pausedPosition) < 0.35,
          QStringLiteral("the zoom change did not resume or seek the paused source"));
    captureArtifacts(context, QStringLiteral("fit-zoom50-paused"));

    settingsDecision(context, client, runner,
                     policyFor(VideoScaleMode::Fit, 0, QStringLiteral("bicubic"),
                               QStringLiteral("catmull_rom")),
                     true, QStringLiteral("live-save-zoom-reset"));
    const QImage zoomReset = grabMatching(*context.window, [&](const QImage& image) {
        return !fitRestored.isNull() && meanAbsDiff(image, fitRestored) < 12.0;
    });
    check(!fitRestored.isNull() && !zoomReset.isNull() &&
              meanAbsDiff(zoomReset, fitRestored) < 12.0,
          QStringLiteral("resetting zoom restored the paused picture"));

    context.session->setPaused(false);
    check(pumpUntil([&] { return context.session->isPlaying(); }, 5000),
          QStringLiteral("playback resumed explicitly after the paused matrix"));

    // ── PiP must inherit the current policy ──────────────────────────────────
    if (clickFlyoutMode(context, VideoScaleMode::Aspect4x3, QStringLiteral("live-pip-4x3"))) {
        pumpUntil([&] {
            return VideoSession::videoScalePolicy().mode == VideoScaleMode::Aspect4x3 &&
                   itemSize(*context.videoItem) == context.fitSize;
        }, 5000);
    }
    const QPointer<VideoSession> pipSession(context.session);
    const double positionBeforePip = context.session->position();
    context.controller->togglePip();
    const bool active = pumpUntil([&] {
        return context.controller->isPipActive() && context.window->compactPip();
    }, 10000);
    check(active, QStringLiteral("the real controller entered same-window PiP"));
    const bool retainedOwner = context.controller->session() == pipSession &&
                               context.controller->window() == context.window &&
                               context.window->videoItem() == context.videoItem;
    check(retainedOwner,
          QStringLiteral("PiP retains the same VideoSession, PlayerWindow and video item"));
    if (active && retainedOwner) {
        const bool pipFramed = pumpUntil([&] {
            const QImage image = context.window->grabVideoFrame();
            return context.session->hasVideo() && !image.isNull();
        }, 15000);
        check(pipFramed, QStringLiteral("same-window PiP continues rendering the source"));
        const QImage pipFrame = context.window->grabVideoFrame();
        const QSize pipBox = itemSize(*context.videoItem);
        const int expected = pipFrame.isNull()
                                 ? -1
                                 : static_cast<int>(std::lround(pipFrame.height() * 4.0 / 3.0));
        const int span = litWidth(pipFrame);
        check(withinPixelTolerance(span, expected, std::max(12, expected / 40)),
              QStringLiteral("same-window PiP renders the current 4:3 policy"),
              QStringLiteral("span=%1 expected=%2 grab=%3x%4")
                  .arg(span).arg(expected).arg(pipFrame.width()).arg(pipFrame.height()));
        check(pipBox == context.window->size(),
              QStringLiteral("same-window PiP keeps the video item fitted to its window"),
              QStringLiteral("item=%1x%2 window=%3x%4")
                  .arg(pipBox.width()).arg(pipBox.height())
                  .arg(context.window->width()).arg(context.window->height()));
        Observation pipObservation =
            observe(*context.window, *context.session, *context.videoItem, QStringLiteral("pip-4x3"));
        measure(pipObservation, pipFrame);
        pipObservation.extra.insert(QStringLiteral("expectedSpan"), expected);
        pipObservation.extra.insert(QStringLiteral("compactSize"), sizeJson(context.window->size()));
        pipObservation.extra.insert(QStringLiteral("policy"),
                                    policyJson(VideoSession::videoScalePolicy()));
        pipObservation.extra.insert(QStringLiteral("sameSession"),
                                    context.controller->session() == pipSession);
        context.observations.append(pipObservation.json());
        saveImage(pipFrame, shotPath(context, QStringLiteral("pip-4x3-surface")),
                  &context.written);
        saveImage(context.window->grabWindow(),
                  shotPath(context, QStringLiteral("pip-4x3-float")), &context.written);
        QJsonObject artifact;
        artifact.insert(QStringLiteral("stem"), QStringLiteral("pip-4x3"));
        artifact.insert(QStringLiteral("windowExposed"), context.window->isExposed());
        context.artifacts.append(artifact);
    }
    if (active) context.controller->togglePip();
    check(pumpUntil([&] {
        return !context.controller->isPipActive() && !context.window->compactPip();
    }, 10000), QStringLiteral("PiP restores the same player window"));
    check(context.window->isVisible(),
          QStringLiteral("the player window remains visible after PiP"));
    check(context.controller->window() == context.window &&
              context.controller->session() == pipSession &&
              context.window->session() == pipSession &&
              context.window->videoItem() == context.videoItem,
          QStringLiteral("PiP restoration resolves to the original Quick window and session"));
    check(context.session->position() > positionBeforePip,
          QStringLiteral("playback sampling advanced across the same-session PiP transition"));
    check(VideoSession::videoScalePolicy().mode == VideoScaleMode::Aspect4x3,
          QStringLiteral("returning from PiP did not change the display policy"));

    // ── Cancel must not publish ──────────────────────────────────────────────
    const VideoScalePolicy beforeCancel = VideoSession::videoScalePolicy();
    const QSize boxBeforeCancel = itemSize(*context.videoItem);
    settingsDecision(context, client, runner,
                     policyFor(VideoScaleMode::Stretch, 40, QStringLiteral("bilinear"),
                               QStringLiteral("mitchell")),
                     false, QStringLiteral("live-cancel"));
    check(VideoSession::videoScalePolicy() == beforeCancel,
          QStringLiteral("the real Cancel published no display policy"));
    {
        QSettings storedSettings;
        check(mplayer::videoScalePolicyFromSettings(storedSettings) == beforeCancel,
              QStringLiteral("the real Cancel stored no display policy"));
    }
    check(itemSize(*context.videoItem) == boxBeforeCancel,
          QStringLiteral("the real Cancel left the running shell's surface box unchanged"));

    // ── Final Save and a fresh-process store read ────────────────────────────
    const VideoScalePolicy finalPolicy = policyFor(
        VideoScaleMode::Fill, 0, QStringLiteral("catmull_rom"), QStringLiteral("bicubic"));
    settingsDecision(context, client, runner, finalPolicy, true, QStringLiteral("live-save-final"));
    {
        QSettings storedSettings;
        check(mplayer::videoScalePolicyFromSettings(storedSettings) == finalPolicy,
              QStringLiteral("the final Save is what the production reader returns"));
    }
    check(pumpUntil([&] { return itemSize(*context.videoItem) == context.pictureArea; }, 5000),
          QStringLiteral("the final Fill policy reached the live shell"));

    const QString expectation =
        QStringLiteral("%1,%2,%3,%4")
            .arg(static_cast<int>(finalPolicy.mode))
            .arg(finalPolicy.zoomPercent)
            .arg(finalPolicy.upscale, finalPolicy.downscale);
    const QString restartEvidence =
        QDir(options.settingsRoot).filePath(QStringLiteral("video-scaling-restart-evidence.json"));
    QFile::remove(restartEvidence);
    QProcess reader;
    reader.setProcessChannelMode(QProcess::MergedChannels);
    reader.setProgram(options.selfPath);
    reader.setWorkingDirectory(options.settingsRoot);
    reader.setArguments({QStringLiteral("--video-scaling-smoke-read"),
                         QStringLiteral("--video-scaling-store"), options.settingsRoot,
                         QStringLiteral("--video-scaling-expect"), expectation,
                         QStringLiteral("--video-scaling-evidence"), restartEvidence});
    reader.start();
    const bool readerStarted = reader.waitForStarted(30000);
    check(readerStarted, QStringLiteral("a fresh process was started to read the Saved policy"));
    bool readerFinished = false;
    int readerExit = -1;
    QByteArray readerOutput;
    if (readerStarted) {
        readerFinished = reader.waitForFinished(120000);
        if (!readerFinished) reader.kill();
        readerOutput = reader.readAllStandardOutput();
        readerExit = reader.exitCode();
    }
    QJsonObject readerEvidence;
    {
        QFile file(restartEvidence);
        if (file.open(QIODevice::ReadOnly)) {
            readerEvidence = QJsonDocument::fromJson(file.readAll()).object();
        }
    }
    const bool restartPassed =
        readerFinished && readerExit == 0 &&
        readerEvidence.value(QStringLiteral("passed")).toBool();
    check(restartPassed,
          QStringLiteral("a fresh process accepted the final Saved policy through the production reader"),
          QString::fromLocal8Bit(readerOutput.right(1500)));

    // ── The new card at the three acceptance widths, in this run's language ──
    measureSettingsCard(context, client, runner);

    // ── Evidence, visible hold ───────────────────────────────────────────────
    QJsonObject evidence = liveEvidence(context, options, restartPassed, readerExit,
                                        QString::fromLocal8Bit(readerOutput.right(4000)));
    evidence.insert(QStringLiteral("restartEvidence"), readerEvidence);
    evidence.insert(QStringLiteral("finalPolicy"), policyJson(finalPolicy));
    {
        QSettings storedSettings;
        evidence.insert(QStringLiteral("storeReaderFinal"),
                        policyJson(mplayer::videoScalePolicyFromSettings(storedSettings)));
    }
    evidence.insert(QStringLiteral("mediaDurationSeconds"), duration);
    QString error;
    bool evidenceWritten = writeEvidence(evidence, options.evidencePath, &error);
    if (!evidenceWritten) {
        std::fprintf(stderr, "FAIL the live evidence was written: %s\n", qPrintable(error));
    }

    if (options.holdMs > 0) {
        // Leave a stable, visible state for a human or a read-only desktop
        // capture: the final Fill player plus its open in-scene scaling flyout.
        context.controller->noteUserActivity();
        Q_EMIT context.controller->menuOpenRequested(QStringLiteral("settings"));
        pumpUntil([&] {
            if (context.window->rootObject() == nullptr) return false;
            for (QQuickItem* item : context.window->rootObject()->findChildren<QQuickItem*>()) {
                if (item->property("menuId").toString() == QLatin1String("settings") &&
                    item->isVisible()) return true;
            }
            return false;
        }, 3000);
        captureArtifacts(context, QStringLiteral("hold"));
        std::fprintf(stderr,
                     "LIVE-SMOKE-HOLD-READY ms=%d window=%dx%d surface=%.0fx%.0f mode=%s evidence=%s\n",
                     options.holdMs, context.window->width(), context.window->height(),
                     context.videoItem->width(), context.videoItem->height(),
                     qPrintable(mplayer::videoScaleModeLabel(
                         VideoSession::videoScalePolicy().mode)),
                     qPrintable(QDir::cleanPath(options.evidencePath)));
        std::fflush(stderr);
        pumpUntil([] { return false; }, options.holdMs);
        std::fprintf(stderr, "LIVE-SMOKE-HOLD-DONE\n");
        std::fflush(stderr);
    }

    controller->requestClose();
    QApplication::processEvents();
    VideoSession::setVideoScalePolicy(opening);

    std::fprintf(stderr,
                 "[native_settings_probe video-scaling-live-smoke] %s (%d checks, %d failures) "
                 "restart=%d evidence=%d refusals=%d language=%s path=%s\n",
                 failures == 0 && restartPassed && evidenceWritten ? "PASS" : "FAIL", checks,
                 failures, restartPassed ? 1 : 0, evidenceWritten ? 1 : 0,
                 context.refusals.count, qPrintable(context.language),
                 qPrintable(QDir::cleanPath(options.evidencePath)));
    return failures == 0 && restartPassed && evidenceWritten ? 0 : 1;
}

}  // namespace mikai::probes
