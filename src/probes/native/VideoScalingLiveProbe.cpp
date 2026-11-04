// UI actions stay within owned windows; no desktop input is synthesized.
#include "VideoScalingLiveProbe.hpp"

#include "PlayerDialog.hpp"
#include "PlayerShell.hpp"
#include "PipFloat.hpp"
#include "ShellWidgets.hpp"
#include "backend/MikaiClient.hpp"
#include "dialogs/SettingsDialog.hpp"
#include "player/core/VideoSurface.hpp"
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
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStringList>
#include <QTest>
#include <QThread>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <functional>

using mikai::app::SettingsDialog;
using mplayer::VideoScaleMode;
using mplayer::VideoScalePolicy;
using mplayer::VideoSurface;
using mplayer::shell::PipFloat;
using mplayer::shell::PlayerDialog;
using mplayer::shell::PlayerShell;
using mplayer::shell::SettingsFlyout;

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
/// surface widget; the span comes from the rendered framebuffer.
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

Observation observe(const PlayerShell& shell, const VideoSurface& surface, const QString& label) {
    Observation observation;
    observation.label = label;
    observation.shellSize = shell.size();
    observation.surfaceRect = surface.geometry();
    observation.video = QSize(surface.videoWidth(), surface.videoHeight());
    observation.position = surface.position();
    observation.playing = surface.isPlaying();
    observation.pausedObserved = surface.pauseObserved();
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
QImage grabMatching(VideoSurface& surface, const std::function<bool(const QImage&)>& accepts,
                    int timeoutMs = 3000) {
    QImage image;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        image = surface.grabVideoFramebuffer();
        if (!image.isNull() && accepts(image)) return image;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(15);
    }
    image = surface.grabVideoFramebuffer();
    return image;
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
    PlayerDialog* player = nullptr;
    PlayerShell* shell = nullptr;
    VideoSurface* surface = nullptr;
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
    const QImage frame = context.surface->grabVideoFramebuffer();
    saveImage(frame, shotPath(context, stem + QStringLiteral("-surface")), &context.written);

    QJsonObject record;
    record.insert(QStringLiteral("stem"), stem);
    record.insert(QStringLiteral("frame"), sizeJson(frame.size()));
    const QWindow* window = context.player->windowHandle();
    record.insert(QStringLiteral("windowExposed"),
                  window != nullptr && window->isExposed());
    if (QScreen* screen = context.player->screen()) {
        const QImage asImage = screen->grabWindow(context.player->winId()).toImage();
        record.insert(QStringLiteral("windowGrab"), sizeJson(asImage.size()));
        record.insert(QStringLiteral("windowGrabSaved"),
                      saveImage(asImage, shotPath(context, stem + QStringLiteral("-window")),
                                &context.written));
    }
    context.artifacts.append(record);
    return record;
}

/// Opens the real Settings dialog, drives the actual scaling controls and then
/// clicks its real Save or Cancel button. Never calls a slot directly.
bool settingsDecision(LiveContext& context, mikai::backend::MikaiClient& client,
                      mikai::app::Runner& runner, const VideoScalePolicy& policy,
                      bool accept, const QString& label) {
    SettingsDialog dialog(client, runner, context.player);
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
    const bool accepted = dialog.result() == static_cast<int>(QDialog::Accepted);
    check(accepted == accept, label + (accept ? QStringLiteral(": Save accepted the dialog")
                                              : QStringLiteral(": Cancel rejected the dialog")));
    captureArtifacts(context,
                     label + (accept ? QStringLiteral("-save") : QStringLiteral("-cancel")));
    dialog.reject();
    QApplication::processEvents();
    return accepted == accept;
}

bool clickFlyoutMode(LiveContext& context, VideoScaleMode mode, const QString& label) {
    context.shell->wake();
    context.shell->openSettings();
    SettingsFlyout* flyout = context.player->findChild<SettingsFlyout*>();
    if (flyout == nullptr) {
        check(false, label + QStringLiteral(": the player exposes its settings flyout"));
        return false;
    }
    const bool visible = pumpUntil([flyout] { return flyout->isVisible(); }, 3000);
    check(visible, label + QStringLiteral(": the player settings flyout is visible"));
    QPushButton* row = nullptr;
    if (visible) {
        for (QPushButton* button : flyout->findChildren<QPushButton*>()) {
            if (button->property("videoScaleMode").toInt() == static_cast<int>(mode) &&
                button->isVisible()) {
                row = button;
                break;
            }
        }
    }
    check(row != nullptr, label + QStringLiteral(": the flyout offers this display mode row"));
    if (row == nullptr) return false;
    row->click();
    QApplication::processEvents();
    return true;
}

QPushButton* pipButton(PlayerShell& shell) {
    const QString expected =
        QCoreApplication::translate("mplayer::shell::PlayerShell", "Picture in picture");
    for (QPushButton* button : shell.findChildren<QPushButton*>()) {
        if (button->accessibleName() == expected || button->toolTip() == expected) {
            return button;
        }
    }
    return nullptr;
}

PipFloat* visiblePip() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* pip = qobject_cast<PipFloat*>(widget); pip != nullptr && pip->isVisible()) {
            return pip;
        }
    }
    return nullptr;
}

/// Checks one display mode: the shell surface box and the rendered span. The
/// expectations are derived from the observed picture area and video geometry,
/// never from the bare-surface test's absolute numbers.
bool verifyMode(LiveContext& context, const QString& label, VideoScaleMode mode,
                const QSize& expectedSurface, const std::function<bool(int)>& spanAccepts,
                int* spanOut = nullptr) {
    const bool applied = pumpUntil(
        [&] {
            return VideoSurface::videoScalePolicy().mode == mode &&
                   context.surface->size() == expectedSurface;
        },
        5000);
    check(applied, label + QStringLiteral(": the real shell surface reaches this mode's box"));
    if (!applied) {
        Observation observation = observe(*context.shell, *context.surface, label);
        observation.extra.insert(QStringLiteral("expectedSurface"), sizeJson(expectedSurface));
        context.observations.append(observation.json());
        return false;
    }
    QImage image;
    const bool matched = pumpUntil(
        [&] {
            image = context.surface->grabVideoFramebuffer();
            if (image.isNull()) return false;
            const int span = litWidth(image);
            return spanAccepts(span) && centerLuma(image) > 20.0;
        },
        4000);
    Observation observation = observe(*context.shell, *context.surface, label);
    measure(observation, image);
    observation.extra.insert(QStringLiteral("expectedSurface"), sizeJson(expectedSurface));
    observation.extra.insert(QStringLiteral("policy"), policyJson(VideoSurface::videoScalePolicy()));
    context.observations.append(observation.json());
    if (spanOut != nullptr) *spanOut = observation.span;
    check(matched, label + QStringLiteral(": the rendered picture reaches this mode's span"),
          QStringLiteral("span=%1 grab=%2x%3").arg(observation.span).arg(observation.grab.width()).arg(observation.grab.height()));
    // Keep our own flyout out of the window grab; the rendered framebuffer above
    // is the authoritative picture either way.
    context.shell->dismissOverlay();
    QApplication::processEvents();
    captureArtifacts(context, label);
    return matched;
}

/// Measures the real General card at the three acceptance widths GOAL-Z names.
/// The language is whatever this process installed from MIKAI_PROBE_LANGUAGE, so
/// one run is single-language evidence and says which language it covers.
void measureSettingsCard(LiveContext& context, mikai::backend::MikaiClient& client,
                         mikai::app::Runner& runner) {
    SettingsDialog dialog(client, runner, context.player);
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
        settings.setValue(QStringLiteral("player/anime4k"), QStringLiteral("off"));
        settings.sync();
        opening = mplayer::videoScalePolicyFromSettings(settings);
    }
    check(opening == VideoScalePolicy{},
          QStringLiteral("the owned, empty store starts on the documented default policy"));
    VideoSurface::setVideoScalePolicy(opening);

    // The probe's existing size convention, not a parallel one: MIKAI_PROBE_WIDTH
    // / MIKAI_PROBE_HEIGHT size the real player window, and the same width is
    // what the Settings card matrix records.
    const QSize liveWindow = mikai::probes::windowSize(QSize(1280, 760));
    PlayerDialog player(QStringLiteral("Video scaling live smoke"), media.absoluteFilePath());
    player.setAttribute(Qt::WA_ShowWithoutActivating, true);
    player.resize(liveWindow);
    player.show();
    player.startPlayer();

    LiveContext context;
    context.player = &player;
    context.outputDir = evidenceFileInfo.absolutePath();
    context.language = mikai::app::activeInterfaceLanguage();
    context.shell = player.findChild<PlayerShell*>();
    context.surface = player.findChild<VideoSurface*>();
    check(context.shell != nullptr && context.surface != nullptr,
          QStringLiteral("the real player window exposes its shell and video surface"));
    if (context.shell == nullptr || context.surface == nullptr) {
        player.close();
        LiveContext empty;
        QJsonObject evidence = liveEvidence(empty, options, false, -1, QString());
        QString error;
        writeEvidence(evidence, options.evidencePath, &error);
        return 1;
    }
    QObject::connect(context.surface, &VideoSurface::displayScalingRefused, context.surface,
                     [&context](const QString& reason) {
                         ++context.refusals.count;
                         context.refusals.reasons.append(reason);
                     });

    const bool framed = pumpUntil(
        [&] {
            if (!context.surface->hasVideo() || context.surface->videoWidth() <= 0) return false;
            const QImage image = context.surface->grabVideoFramebuffer();
            return !image.isNull() && centerLuma(image) > 20.0;
        },
        30000);
    context.video = QSize(context.surface->videoWidth(), context.surface->videoHeight());
    const double duration = context.surface->duration();
    check(framed, QStringLiteral("the real player decodes and renders the media fixture"));
    // A short clip reaches EOS in the middle of the matrix; refuse it instead of
    // letting a half-run read as a pass.
    check(duration >= 60.0,
          QStringLiteral("the fixture is long enough for the whole matrix without reaching EOS"),
          QStringLiteral("duration=%1 s").arg(duration));
    if (!framed || duration < 60.0 || context.video.isEmpty()) {
        player.close();
        QJsonObject evidence = liveEvidence(context, options, false, -1, QString());
        QString error;
        writeEvidence(evidence, options.evidencePath, &error);
        return 1;
    }
    context.dpr = context.surface->devicePixelRatioF();
    const double videoAspect = double(context.video.width()) / context.video.height();
    check(videoAspect > 0.5 && videoAspect < 3.0,
          QStringLiteral("the fixture reports a picture aspect the shell can fit"), QString::number(videoAspect));

    if (!context.surface->isPlaying()) context.surface->setPaused(false);
    check(pumpUntil([&] { return context.surface->isPlaying(); }, 5000),
          QStringLiteral("the source is playing before the live matrix starts"));

    // ── Settings Save on a playing surface; Fill also reveals the picture area ──
    const double positionBeforeSave = context.surface->position();
    const QSize boxBeforeSave = context.surface->geometry().size();
    const VideoScalePolicy fillPolicy =
        policyFor(VideoScaleMode::Fill, 0, QStringLiteral("bicubic"), QStringLiteral("catmull_rom"));
    settingsDecision(context, client, runner, fillPolicy, true, QStringLiteral("live-save-fill"));
    check(VideoSurface::videoScalePolicy().mode == VideoScaleMode::Fill,
          QStringLiteral("Save published Fill to the running process"));
    {
        QSettings storedSettings;
        check(mplayer::videoScalePolicyFromSettings(storedSettings) == fillPolicy,
              QStringLiteral("Save stored the complete Fill policy the production reader returns"));
    }
    const bool relaid = pumpUntil(
        [&] { return context.surface->size() != boxBeforeSave; }, 5000);
    check(relaid, QStringLiteral("the running shell re-laid its surface for Fill without a restart"));
    context.pictureArea = context.surface->size();

    Observation fillPlaying = observe(*context.shell, *context.surface, QStringLiteral("fill-playing-save"));
    fillPlaying.extra.insert(QStringLiteral("policy"), policyJson(fillPolicy));
    context.observations.append(fillPlaying.json());

    const double positionAfterSave = context.surface->position();
    check(context.surface->isPlaying(), QStringLiteral("the live Save did not stop playback"));
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

    const bool stillPlaying = context.surface->isPlaying();
    check(stillPlaying, QStringLiteral("the flyout matrix left playback running"));
    check(context.surface->position() > 0.0, QStringLiteral("playback advanced across the flyout matrix"));

    // ── Paused adoption ──────────────────────────────────────────────────────
    context.surface->setPaused(true);
    check(pumpUntil([&] { return context.surface->pauseObserved(); }, 5000),
          QStringLiteral("the source actually paused before the paused matrix"));
    const double pausedPosition = context.surface->position();
    const QSize fitBeforePause = context.fitSize;

    if (clickFlyoutMode(context, VideoScaleMode::Fit, QStringLiteral("live-paused-fit"))) {
        pumpUntil([&] { return context.surface->size() == fitBeforePause; }, 5000);
    }
    const QImage fitPaused = grabMatching(*context.surface, [](const QImage& image) {
        return !image.isNull() && centerLuma(image) > 20.0;
    });
    check(!fitPaused.isNull(), QStringLiteral("the paused Fit frame rendered"));

    // A mode change on a paused surface must relayout and repaint, and must not
    // resume or seek.
    const bool pausedFillApplied =
        clickFlyoutMode(context, VideoScaleMode::Fill, QStringLiteral("live-paused-fill"));
    if (pausedFillApplied) {
        const bool relaid = pumpUntil(
            [&] { return context.surface->size() == context.pictureArea; }, 5000);
        check(relaid, QStringLiteral("a paused surface adopts Fill's shell box"));
        const int expectedFillSpan =
            static_cast<int>(std::lround(context.pictureArea.width() * context.dpr));
        const QImage pausedFill = grabMatching(*context.surface, [expectedFillSpan](const QImage& image) {
            return std::abs(litWidth(image) - expectedFillSpan) < 16;
        });
        Observation observation = observe(*context.shell, *context.surface, QStringLiteral("fill-paused"));
        measure(observation, pausedFill);
        context.observations.append(observation.json());
        // The box changed, so the two paused frames have different sizes and a
        // direct difference is not meaningful; the repaint evidence is the new
        // box plus a picture that fills it.
        check(!pausedFill.isNull() && withinPixelTolerance(litWidth(pausedFill), expectedFillSpan, 16),
              QStringLiteral("a paused surface repaints the picture after a live mode change"),
              QStringLiteral("span=%1 expected=%2").arg(litWidth(pausedFill)).arg(expectedFillSpan));
        check(!context.surface->isPlaying() && context.surface->pauseObserved(),
              QStringLiteral("the paused mode change did not resume playback"));
        check(std::abs(context.surface->position() - pausedPosition) < 0.35,
              QStringLiteral("the paused mode change did not seek"));
        captureArtifacts(context, QStringLiteral("paused-fill"));
    }

    // ── Original 1:1 (Settings-only) on the paused frame ─────────────────────
    const VideoScalePolicy originalPolicy = policyFor(
        VideoScaleMode::Original, 0, QStringLiteral("bicubic"), QStringLiteral("catmull_rom"));
    settingsDecision(context, client, runner, originalPolicy, true, QStringLiteral("live-save-original"));
    const bool originalBox = pumpUntil(
        [&] { return context.surface->size() == context.fitSize; }, 5000);
    check(originalBox, QStringLiteral("Original keeps the shell's video-aspect box"));
    const QImage originalPaused = grabMatching(*context.surface, [&](const QImage& image) {
        return meanAbsDiff(image, fitPaused) > 3.0;
    });
    Observation originalObservation =
        observe(*context.shell, *context.surface, QStringLiteral("original-paused"));
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
    check(!context.surface->isPlaying() && std::abs(context.surface->position() - pausedPosition) < 0.35,
          QStringLiteral("Original did not resume or seek the paused source"));
    captureArtifacts(context, QStringLiteral("original-paused"));

    // ── Zoom on the paused frame ─────────────────────────────────────────────
    settingsDecision(context, client, runner,
                     policyFor(VideoScaleMode::Fit, 0, QStringLiteral("bicubic"),
                               QStringLiteral("catmull_rom")),
                     true, QStringLiteral("live-save-fit"));
    check(pumpUntil([&] { return context.surface->size() == context.fitSize; }, 5000),
          QStringLiteral("Fit restored the aspect box"));
    // The core dithers, so two renders of the same paused frame are not
    // byte-identical; 12 is far below the difference a scaling change makes.
    const QImage fitRestored = grabMatching(*context.surface, [&](const QImage& image) {
        return !fitPaused.isNull() && meanAbsDiff(image, fitPaused) < 12.0;
    });

    settingsDecision(context, client, runner,
                     policyFor(VideoScaleMode::Fit, 50, QStringLiteral("bicubic"),
                               QStringLiteral("catmull_rom")),
                     true, QStringLiteral("live-save-zoom"));
    const QImage zoomedPaused = grabMatching(*context.surface, [&](const QImage& image) {
        return !fitRestored.isNull() && meanAbsDiff(image, fitRestored) > 3.0;
    });
    Observation zoomObservation = observe(*context.shell, *context.surface, QStringLiteral("fit-zoom50-paused"));
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
    check(!context.surface->isPlaying() && std::abs(context.surface->position() - pausedPosition) < 0.35,
          QStringLiteral("the zoom change did not resume or seek the paused source"));
    captureArtifacts(context, QStringLiteral("fit-zoom50-paused"));

    settingsDecision(context, client, runner,
                     policyFor(VideoScaleMode::Fit, 0, QStringLiteral("bicubic"),
                               QStringLiteral("catmull_rom")),
                     true, QStringLiteral("live-save-zoom-reset"));
    const QImage zoomReset = grabMatching(*context.surface, [&](const QImage& image) {
        return !fitRestored.isNull() && meanAbsDiff(image, fitRestored) < 12.0;
    });
    check(!fitRestored.isNull() && !zoomReset.isNull() &&
              meanAbsDiff(zoomReset, fitRestored) < 12.0,
          QStringLiteral("resetting zoom restored the paused picture"));

    context.surface->setPaused(false);
    check(pumpUntil([&] { return context.surface->isPlaying(); }, 5000),
          QStringLiteral("playback resumed explicitly after the paused matrix"));

    // ── PiP must inherit the current policy ──────────────────────────────────
    if (clickFlyoutMode(context, VideoScaleMode::Aspect4x3, QStringLiteral("live-pip-4x3"))) {
        pumpUntil([&] {
            return VideoSurface::videoScalePolicy().mode == VideoScaleMode::Aspect4x3 &&
                   context.surface->size() == context.fitSize;
        }, 5000);
    }
    QPushButton* pip = pipButton(*context.shell);
    check(pip != nullptr, QStringLiteral("the real dock exposes its Picture-in-picture button"));
    if (pip != nullptr) {
        context.shell->wake();
        pip->click();
        const bool active = pumpUntil([&] { return context.player->isPipActive(); }, 10000);
        check(active, QStringLiteral("the real PiP button started the float"));
        PipFloat* pipFloat = active ? visiblePip() : nullptr;
        check(pipFloat != nullptr, QStringLiteral("the PiP float is visible on this process's own windows"));
        if (pipFloat != nullptr && pipFloat->videoSurface() != nullptr) {
            VideoSurface* floatSurface = pipFloat->videoSurface();
            const bool pipFramed = pumpUntil(
                [&] {
                    const QImage image = floatSurface->grabVideoFramebuffer();
                    return floatSurface->hasVideo() && !image.isNull() && centerLuma(image) > 20.0;
                },
                15000);
            check(pipFramed, QStringLiteral("the PiP float decodes and renders the source"));
            const QImage pipFrame = floatSurface->grabVideoFramebuffer();
            const int expected = pipFrame.isNull()
                                     ? -1
                                     : static_cast<int>(std::lround(pipFrame.height() * 4.0 / 3.0));
            const int span = litWidth(pipFrame);
            check(withinPixelTolerance(span, expected, std::max(12, expected / 40)),
                  QStringLiteral("the PiP float renders the current 4:3 policy"),
                  QStringLiteral("span=%1 expected=%2 grab=%3x%4")
                      .arg(span).arg(expected).arg(pipFrame.width()).arg(pipFrame.height()));
            check(floatSurface->geometry().size() == pipFloat->size(),
                  QStringLiteral("the PiP surface fills the float window"),
                  QStringLiteral("surface=%1x%2 float=%3x%4")
                      .arg(floatSurface->geometry().width()).arg(floatSurface->geometry().height())
                      .arg(pipFloat->width()).arg(pipFloat->height()));
            Observation pipObservation = observe(*context.shell, *floatSurface, QStringLiteral("pip-4x3"));
            measure(pipObservation, pipFrame);
            pipObservation.extra.insert(QStringLiteral("expectedSpan"), expected);
            pipObservation.extra.insert(QStringLiteral("floatSize"), sizeJson(pipFloat->size()));
            pipObservation.extra.insert(QStringLiteral("policy"),
                                       policyJson(VideoSurface::videoScalePolicy()));
            context.observations.append(pipObservation.json());
            saveImage(pipFrame, shotPath(context, QStringLiteral("pip-4x3-surface")),
                      &context.written);
            saveImage(pipFloat->grab().toImage(),
                      shotPath(context, QStringLiteral("pip-4x3-float")), &context.written);
            QJsonObject artifact;
            artifact.insert(QStringLiteral("stem"), QStringLiteral("pip-4x3"));
            artifact.insert(QStringLiteral("windowExposed"),
                            pipFloat->windowHandle() != nullptr &&
                                pipFloat->windowHandle()->isExposed());
            context.artifacts.append(artifact);

            QPushButton* restore = pipFloat->findChild<QPushButton*>(QStringLiteral("pipRestore"));
            check(restore != nullptr, QStringLiteral("the float exposes its real restore button"));
            if (restore != nullptr) restore->click();
            check(pumpUntil([&] { return !context.player->isPipActive(); }, 10000),
                  QStringLiteral("the restore button returned the float to the player"));
        }
    }
    check(pumpUntil([&] { return context.player->isVisible(); }, 5000),
          QStringLiteral("the player window is visible again after PiP"));
    check(VideoSurface::videoScalePolicy().mode == VideoScaleMode::Aspect4x3,
          QStringLiteral("returning from PiP did not change the display policy"));

    // ── Cancel must not publish ──────────────────────────────────────────────
    const VideoScalePolicy beforeCancel = VideoSurface::videoScalePolicy();
    const QSize boxBeforeCancel = context.surface->size();
    settingsDecision(context, client, runner,
                     policyFor(VideoScaleMode::Stretch, 40, QStringLiteral("bilinear"),
                               QStringLiteral("mitchell")),
                     false, QStringLiteral("live-cancel"));
    check(VideoSurface::videoScalePolicy() == beforeCancel,
          QStringLiteral("the real Cancel published no display policy"));
    {
        QSettings storedSettings;
        check(mplayer::videoScalePolicyFromSettings(storedSettings) == beforeCancel,
              QStringLiteral("the real Cancel stored no display policy"));
    }
    check(context.surface->size() == boxBeforeCancel,
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
    check(pumpUntil([&] { return context.surface->size() == context.pictureArea; }, 5000),
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
        // capture: the final Fill player plus its open scaling flyout.
        context.shell->wake();
        context.shell->openSettings();
        pumpUntil([&] { return context.shell->hasVisibleOverlay(); }, 3000);
        captureArtifacts(context, QStringLiteral("hold"));
        std::fprintf(stderr,
                     "LIVE-SMOKE-HOLD-READY ms=%d window=%dx%d surface=%dx%d mode=%s evidence=%s\n",
                     options.holdMs, player.width(), player.height(),
                     context.surface->width(), context.surface->height(),
                     qPrintable(mplayer::videoScaleModeLabel(
                         VideoSurface::videoScalePolicy().mode)),
                     qPrintable(QDir::cleanPath(options.evidencePath)));
        std::fflush(stderr);
        pumpUntil([] { return false; }, options.holdMs);
        std::fprintf(stderr, "LIVE-SMOKE-HOLD-DONE\n");
        std::fflush(stderr);
    }

    player.close();
    QApplication::processEvents();
    VideoSurface::setVideoScalePolicy(opening);

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
