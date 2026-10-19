// Interaction probe: the dock's dub/subtles buttons, the gear's quality list,
// and the things that only break in use.
//
//   - a seek crashed the process: mpv_command wants a NULL-terminated array
//   - the quality and track popovers closed on every shell state change
//   - returning from Alt+Tab left nothing focused
//   - quality and dub/sub selection sat in a top strip, above the picture
//
// The stages run one at a time, in the order the player was designed to be
// walked, and each waits on the state it needs rather than on a clock. Two
// stages sharing the one settings popover or the one surface measured each
// other's work - the pixel proof of the shader chain read a framebuffer the
// quality walk had just reloaded - and a stage still on the stack when the
// terminal Esc freed the dialog wrote through a dangling pointer. A stage that
// finds the window gone is skipped, and a font or size the probe cannot name is
// never asserted: the checks state the behaviour, not the theme's numbers.

#include <QAbstractButton>
#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLibraryInfo>
#include <QLocale>
#include <QPointer>
#include <QTest>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>

#include "CompareWindow.hpp"
#include "PlayerDialog.hpp"
#include "PlayerPopups.hpp"
#include "PlayerShell.hpp"
#include "ShellSliders.hpp"
#include "ShellWidgets.hpp"
#include "VideoSurface.hpp"
#include "ProbeRuntime.hpp"

using namespace mplayer;
using namespace mplayer::shell;

namespace {

class StubHost : public PlayerHost {
public:
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int) override {}
    void requestTrackProbe(const QString&) override {}
    void requestOpenOriginal() override {}
    void requestCopyStreamUrl() override {}
    // The dialog carries WA_DeleteOnClose, so once Esc closes it the pointer is
    // dangling and isVisible() would read freed memory. The host is told, and
    // it outlives the dialog: that is the only safe way to observe the close.
    void playerClosed() override { closed = true; }
    void playbackFailed() override {}

    bool closed = false;
};

/// The dock button painted with `glyph`, or null. The glyph is the button's
/// identity here: a tooltip is display text that changes with the wording and
/// the locale, and a probe that hunts for a string it once saw reports a
/// missing control the moment somebody rewords it - which is exactly the class
/// of false failure this probe exists to catch.
QPushButton* buttonWithGlyph(QWidget* root, IconFn glyph) {
    for (QPushButton* button : root->findChildren<QPushButton*>()) {
        auto* icon = qobject_cast<IconButton*>(button);
        if (icon != nullptr && icon->icon() == glyph) return button;
    }
    return nullptr;
}

/// How many buttons under `root` are painted with one of `glyphs`. Used to say
/// a control is not there: absent means no button wears that glyph at all.
int countButtonsWithGlyphs(QWidget* root, std::initializer_list<IconFn> glyphs) {
    int found = 0;
    for (QPushButton* button : root->findChildren<QPushButton*>()) {
        auto* icon = qobject_cast<IconButton*>(button);
        if (icon == nullptr) continue;
        for (IconFn glyph : glyphs) {
            if (icon->icon() == glyph) {
                ++found;
                break;
            }
        }
    }
    return found;
}

/// Waits, bounded, for the player's own window to be back - and to stay back
/// for a beat rather than only for the instant it is sampled.
///
/// The dock's popovers are children of that window, and the probe's own later
/// stages share this event loop: the PiP stage hides the window for about ten
/// seconds and hands it back only when the float is restored, and the shell
/// re-runs its state pass on the way back, which is free to dismiss whatever
/// overlay was up. A menu step taken across that hand-over would drive a dock
/// that is not on screen yet. Requiring the window to be visible twice, about
/// half a second apart, also catches a stage that is about to take it away
/// again - which is this probe's own PiP stage, arriving while the step before
/// it is still running.
bool playerWindowIsBack(const QPointer<QWidget>& dialog, int timeoutMs = 14000) {
    int heldVisibleFor = 0;
    for (int waited = 0; waited < timeoutMs; waited += 50) {
        if (dialog.isNull()) return false;
        QCoreApplication::processEvents();
        if (dialog->isVisible()) {
            heldVisibleFor += 50;
            if (heldVisibleFor >= 800) return true;
        } else {
            heldVisibleFor = 0;
        }
        QThread::msleep(50);
    }
    return false;
}

/// The pooled panel currently showing catalogue level `levelId`. Every level
/// below the root is the same widget class under the same name, so the level is
/// read off the property the shell records on the panel it is showing - a level
/// the probe cannot name is a level it cannot open.
MenuPopup* levelPanelShowing(QWidget* host, const QString& levelId) {
    for (MenuPopup* panel : host->findChildren<MenuPopup*>()) {
        if (!panel->isVisible()) continue;
        if (panel->property("enhancementLevel").toString() == levelId) return panel;
    }
    return nullptr;
}

/// The spot of the row that stands for catalogue `id`, or -1. Rows are matched
/// by the identity the shell hangs on them, not by the words on them: a
/// reworded or re-translated label leaves the row where it was, while a
/// reworded *string in the probe* would make a working menu look broken.
int rowForCatalogId(MenuPopup* panel, const QString& id) {
    if (panel == nullptr) return -1;
    for (int spot = 0; spot < panel->rowCount(); ++spot) {
        const QString payload = panel->entryAt(spot).payload.toString();
        if (payload == QLatin1String("level|") + id || payload == QLatin1String("preset|") + id) {
            return spot;
        }
    }
    return -1;
}

/// The pressable widget behind the row that stands for catalogue `id`, or null.
/// Null covers both "the menu does not offer it" and "the row is not a button",
/// which the caller tells apart by asking for the spot as well.
QPushButton* pressableRowFor(MenuPopup* panel, const QString& id) {
    const int spot = rowForCatalogId(panel, id);
    if (spot < 0) return nullptr;
    return qobject_cast<QPushButton*>(panel->rowWidget(spot));
}

bool buttonsFitWithoutOverlap(QWidget* popup) {
    QVector<QRect> occupied;
    for (QPushButton* button :
         popup->findChildren<QPushButton*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!button->isVisible()) continue;
        const QRect bounds(button->mapTo(popup, QPoint()), button->size());
        if (!popup->rect().contains(bounds)) return false;
        for (const QRect& other : occupied) {
            if (bounds.intersects(other)) return false;
        }
        occupied.append(bounds);
    }
    return !occupied.isEmpty();
}

/// The row the probe drives: Mode A (Fast) is the chain Anime4K documents for
/// 720p input, which is exactly the clip the probe plays.
constexpr auto kAnime4KProbeMode = anime4k::Mode::FastA;
constexpr double kAnime4KProbeSeek = 2.0;

/// Where the picture actually sits inside a framebuffer: mpv fits the video to
/// the widget and centres it, so the band is recoverable from the source
/// aspect alone. A square at the centre of that band cannot reach a bar.
QRect pictureArea(const QImage& frame, int sourceWidth, int sourceHeight) {
    if (frame.isNull() || sourceWidth <= 0 || sourceHeight <= 0) {
        return QRect();
    }
    const double scale = std::min(static_cast<double>(frame.width()) / sourceWidth,
                                  static_cast<double>(frame.height()) / sourceHeight);
    const int side = static_cast<int>(std::min(sourceWidth, sourceHeight) * scale);
    if (side < 32) return QRect();
    return QRect((frame.width() - side) / 2, (frame.height() - side) / 2, side, side);
}

/// Why two frames cannot be compared pixel for pixel, or empty when they can.
///
/// mpv's vertex shader maps the frame to the widget, so enabling a chain does
/// not change the output SIZE - it changes the picture drawn into the same
/// rectangle. Two stable frames of a paused clip therefore differ only if the
/// chain rebuilt itself, which is exactly the question - while two frames of
/// different sizes are two different pictures and have no answer between them.
///
/// The band both helpers use is derived from the source aspect so it stays
/// clear of the letterbox bars. A bar edge reads as a very large difference and
/// would swamp the signal; the centre of a portrait framebuffer is the only
/// place guaranteed to be picture.
///
/// A sentinel from `pictureDifference` says the measurement did not happen, not
/// that the chain did nothing: mpv re-fits the picture when the widget or the
/// output geometry changes, and a band taken from one size is not the same part
/// of the picture at another. The reason is handed back so a caller can report
/// the geometry it actually saw instead of reading a missing number as an
/// effect that is missing.
QString comparisonRefusal(const QImage& before, const QImage& after) {
    if (before.isNull()) return QStringLiteral("the reference frame is empty");
    if (after.isNull()) return QStringLiteral("the compared frame is empty");
    const QRect band = pictureArea(before, 1280, 720);
    if (band.width() < 32 || band.height() < 32) {
        return QStringLiteral("the reference framebuffer %1x%2 has no picture band")
            .arg(before.width())
            .arg(before.height());
    }
    if (band.intersected(QRect(QPoint(0, 0), after.size())) != band) {
        return QStringLiteral("the framebuffer changed size, %1x%2 to %3x%4")
            .arg(before.width())
            .arg(before.height())
            .arg(after.width())
            .arg(after.height());
    }
    return QString();
}

/// Mean absolute difference between two frames over the picture band, or a
/// negative sentinel when `comparisonRefusal` says they cannot be compared.
double pictureDifference(const QImage& before, const QImage& after) {
    if (!comparisonRefusal(before, after).isEmpty()) return -1.0;
    const QRect area = pictureArea(before, 1280, 720);
    const QImage a = before.convertToFormat(QImage::Format_RGB888);
    const QImage b = after.convertToFormat(QImage::Format_RGB888);
    const int step = std::max(1, std::min(area.width(), area.height()) / 160);
    double total = 0.0;
    int counted = 0;
    for (int y = area.top(); y + step < area.bottom(); y += step) {
        for (int x = area.left(); x + step < area.right(); x += step) {
            const QRgb pa = a.pixel(x, y);
            const QRgb pb = b.pixel(x, y);
            total += std::abs(qRed(pa) - qRed(pb)) + std::abs(qGreen(pa) - qGreen(pb)) +
                     std::abs(qBlue(pa) - qBlue(pb));
            counted += 3;
        }
    }
    return counted > 0 ? total / static_cast<double>(counted) : -1.0;
}

/// Pauses on one source position, so every later frame is the same picture.
void freezeOn(VideoSurface* surface) {
    if (surface == nullptr) return;
    if (surface->isPlaying()) {
        surface->togglePause();
    }
    surface->seek(kAnime4KProbeSeek);
    for (int i = 0; i < 40; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(40);
    }
}

/// A framebuffer that has stopped changing, WITHOUT moving the playhead.
///
/// mpv re-renders on every exposure and the downscaler is not bit-exact, so
/// two grabs of one still frame differ slightly. Polling until two
/// consecutive grabs match makes the frame a fixed reference.
///
/// This must not seek: seeking to the same timestamp twice does not reproduce
/// the same pixels, and a helper that re-seeks turns the "noise floor"
/// measurement into a frame-to-frame difference instead - which was measured
/// at ~5/255 here, the same order as the effect, and made every comparison
/// uninterpretable.
QImage settledFrame(VideoSurface* surface) {
    if (surface == nullptr) return QImage();
    QImage previous;
    for (int i = 0; i < 80; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(40);
        const QImage current = surface->grabVideoFramebuffer();
        if (!current.isNull() && current == previous) {
            return current;
        }
        previous = current;
    }
    return QImage();
}

/// The float, kept here so the ownership checks can run after the step that
/// opened it has returned. A QPointer because the player's destructor is what
/// finally releases the pooled float: the checks after it read through this
/// guard, never a raw pointer, or the read is a use-after-free.
QPointer<PipFloat> pipFloat;

/// Where the float's core was when the viewer pressed restore.
double floatExitPosition = 0.0;
bool floatExitPlaying = false;

int failures = 0;
int checks = 0;

void check(bool ok, const char* what) {
    ++checks;
    std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

/// Pumps the loop until `ready` holds or the budget runs out, then answers with
/// one last look. Bounded, so a widget that never appears is a reported failure
/// and not a probe that sits forever.
template <typename Ready>
bool waitFor(Ready ready, int timeoutMs, int stepMs = 40) {
    for (int waited = 0; waited < timeoutMs; waited += stepMs) {
        if (ready()) return true;
        QCoreApplication::processEvents();
        QThread::msleep(stepMs);
    }
    return ready();
}

bool pipPointerGesture(const QPointer<QWidget>& overlay, const QPoint& grab,
                       const QPoint& drop) {
    auto send = [&](QEvent::Type type) {
        if (overlay.isNull()) return false;
        const QPoint global = type == QEvent::MouseButtonPress ? grab : drop;
        const Qt::MouseButton button =
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        const Qt::MouseButtons buttons =
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event(type, overlay->mapFromGlobal(global), global, button, buttons,
                          Qt::NoModifier);
        QApplication::sendEvent(overlay, &event);
        return !overlay.isNull();
    };
    return send(QEvent::MouseButtonPress) && send(QEvent::MouseMove) &&
           send(QEvent::MouseButtonRelease);
}

bool dragPipTo(const QPointer<PipFloat>& pip, const QPoint& target, const char* description) {
    if (pip.isNull()) return false;
    QPointer<QWidget> overlay = pip->findChild<QWidget*>(QStringLiteral("pipOverlay"));
    check(overlay != nullptr, "the float has an overlay for pointer placement");
    if (overlay.isNull()) return false;

    QPoint grabLocal;
    bool foundBlank = false;
    for (const QPoint& candidate :
         {QPoint(overlay->width() / 2, overlay->height() / 4),
          QPoint(overlay->width() / 3, overlay->height() / 2),
          QPoint(overlay->width() * 2 / 3, overlay->height() / 2)}) {
        if (overlay->rect().contains(candidate) && overlay->childAt(candidate) == nullptr) {
            grabLocal = candidate;
            foundBlank = true;
            break;
        }
    }
    check(foundBlank, "the drag starts on exposed picture, not a playback control");
    if (!foundBlank) return false;
    const QPoint before = pip->pos();
    const QPoint grab = overlay->mapToGlobal(grabLocal);
    const QPoint drop = grab + target - before;
    if (!pipPointerGesture(overlay, grab, drop)) {
        check(false, "the float's pointer receiver survived the placement gesture");
        return false;
    }
    QTest::qWait(500);
    if (pip.isNull()) {
        check(false, "the float survived the placement gesture");
        return false;
    }
    const QPoint landed = pip->pos();
    std::fprintf(stderr,
                 "     PiP drop: before=%d,%d pointer=%d,%d expected=%d,%d landed=%d,%d\n",
                 before.x(), before.y(), drop.x(), drop.y(), target.x(), target.y(),
                 landed.x(), landed.y());
    const bool placed = landed == target && landed != before;
    check(placed, description);
    return placed;
}

bool resizePipRightEdge(const QPointer<PipFloat>& pip, int delta) {
    if (pip.isNull()) return false;
    QPointer<QWidget> overlay = pip->findChild<QWidget*>(QStringLiteral("pipOverlay"));
    check(overlay != nullptr, "the float has an overlay for edge resizing");
    if (overlay.isNull()) return false;
    const QPoint edge(overlay->rect().right(), overlay->height() / 2);
    check(overlay->childAt(edge) == nullptr, "the resize starts on an exposed right edge");
    if (overlay->childAt(edge) != nullptr) return false;
    const QPoint before = pip->pos();
    const QSize beforeSize = pip->size();
    const QSize expected(beforeSize.width() + delta, beforeSize.height());
    const QPoint grab = overlay->mapToGlobal(edge);
    if (!pipPointerGesture(overlay, grab, grab + QPoint(delta, 0))) {
        check(false, "the float's pointer receiver survived the resize gesture");
        return false;
    }
    QTest::qWait(500);
    if (pip.isNull()) {
        check(false, "the float survived the resize gesture");
        return false;
    }
    std::fprintf(stderr,
                 "     PiP right-edge resize: before=%dx%d expected=%dx%d actual=%dx%d "
                 "pos=%d,%d -> %d,%d\n",
                 beforeSize.width(), beforeSize.height(), expected.width(), expected.height(),
                 pip->width(), pip->height(), before.x(), before.y(), pip->x(), pip->y());
    const bool resized = pip->size() == expected && pip->size() != beforeSize &&
                         pip->pos() == before;
    check(resized, "a right-edge pointer drag changes the exact size without moving the float");
    return resized;
}

/// The float that is on screen, or null.
///
/// Every player pools its float instead of destroying it - tearing down a
/// second QOpenGLWidget beside the player's crashes the display driver - so the
/// top-level widgets also hold the hidden floats of players that were put away
/// earlier. Only a visible one is the float under test.
PipFloat* visiblePipFloat() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* pip = qobject_cast<PipFloat*>(widget);
        if (pip != nullptr && pip->isVisible()) return pip;
    }
    return nullptr;
}

/// Waits, bounded, for a float to come on screen and hands it back.
PipFloat* awaitVisiblePipFloat(int timeoutMs = 6000) {
    PipFloat* pip = nullptr;
    waitFor([&pip] { return (pip = visiblePipFloat()) != nullptr; }, timeoutMs);
    return pip;
}

/// Reveals the float's chrome the way a viewer does.
///
/// The float keeps its own hold timer, and that timer reads the physical
/// pointer when it expires. A synthetic move alone would reveal for an instant
/// and then fade on the next tick while the real pointer is still elsewhere, so
/// the pointer is really moved onto the float first. QTest::mouseMove only puts
/// the pointer there (Qt 6.3+ asks the test to use QCursor::setPos for that when
/// no button is held), so the move that reaches the float's own event filter is
/// delivered explicitly afterwards. Returns whether the pointer really is
/// inside the float.
bool hoverPipControls(const QPointer<PipFloat>& pip) {
    if (pip.isNull() || !pip->isVisible()) return false;
    // The centre is inside the float whether the chrome is faded out - the
    // whole rect is exposed picture then - or up, where a control under the
    // pointer is still part of the float.
    const QPoint local = pip->rect().center();
    QTest::mouseMove(pip, local);
    if (pip.isNull()) return false;
    const QPoint global = pip->mapToGlobal(local);
    QMouseEvent move(QEvent::MouseMove, pip->mapFromGlobal(global), global, Qt::NoButton,
                     Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(pip.data(), &move);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return !pip.isNull() && pip->isVisible() &&
           pip->rect().contains(pip->mapFromGlobal(QCursor::pos()));
}

/// What the owning player actually did with one parentless float.
///
/// A QPointer answers "is it alive" and stops there. It cannot answer the two
/// questions the pooled release has to get right: destroyed ONCE, and not
/// destroyed while the player's own QOpenGLWidget is still on screen - the
/// second is the DrvPresentBuffers fault the pool exists to avoid, and it is
/// invisible from a pointer that is merely null afterwards. A parentless
/// top-level widget gives exactly one notice, `destroyed`, so that is what is
/// counted, and the main surface is read inside the handler: it is alive there
/// (the dialog owns it and outlives its own destructor body), which is exactly
/// the read that decides whether the release is ordered.
struct FloatRelease {
    int destroyed = 0;
    bool mainAliveAtDestroy = false;
    bool mainVisibleAtDestroy = false;
};

/// Starts counting one float's destruction against the player's own surface.
std::shared_ptr<FloatRelease> watchFloatRelease(PipFloat* floatWindow, VideoSurface* mainSurface) {
    auto release = std::make_shared<FloatRelease>();
    // Guards, not raw pointers: the read runs from the float's destruction,
    // and the surface it reads belongs to a dialog freed moments later.
    QPointer<VideoSurface> main(mainSurface);
    QObject::connect(floatWindow, &QObject::destroyed, [release, main] {
        ++release->destroyed;
        release->mainAliveAtDestroy = !main.isNull();
        release->mainVisibleAtDestroy = !main.isNull() && main->isVisible();
    });
    return release;
}

/// The player's own surface. The float is parentless, so every VideoSurface
/// under the dialog belongs to the main player.
VideoSurface* playerSurface(PlayerDialog* dialog) {
    return dialog != nullptr ? dialog->findChild<VideoSurface*>() : nullptr;
}

/// How many floats are still parentless top-level windows. Once every player
/// is gone this has to be zero: a float outliving its owner is a whole
/// QOpenGLWidget and mpv core that nothing is left to release.
int livePipFloats() {
    int live = 0;
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (qobject_cast<PipFloat*>(w) != nullptr) ++live;
    }
    return live;
}

/// Runs the probe's stages one at a time, in order.
///
/// The stages used to be keyed to absolute clock times, so a stage that ran
/// long - the quality walk waits on a stream reload for each height - was still
/// on the stack when the next one fired. Two stages then drove the one settings
/// popover and the one surface at once: the pixel measurement read a framebuffer
/// the quality walk had just reloaded, and the terminal Esc could close and free
/// the dialog while a stage was still inside it, which is the use-after-free the
/// old driver kept tripping over. Here a stage runs to completion before the
/// next is even started, and `guard` ends the run the moment the widget the
/// stages drive is gone.
class PhaseRunner : public QObject {
public:
    using Stage = std::function<void()>;

    void add(int order, Stage stage) { stages_.append(Entry{order, std::move(stage)}); }

    void setGuard(std::function<bool()> guard) { guard_ = std::move(guard); }
    void setFinish(std::function<void()> finish) { finish_ = std::move(finish); }

    void start() {
        std::stable_sort(stages_.begin(), stages_.end(),
                         [](const Entry& a, const Entry& b) { return a.order < b.order; });
        QTimer::singleShot(0, this, &PhaseRunner::advance);
    }

private:
    struct Entry {
        int order = 0;
        Stage stage;
    };

    void advance() {
        if (index_ >= stages_.size() || (guard_ && !guard_())) {
            if (finish_) finish_();
            return;
        }
        Stage stage = std::move(stages_[index_++].stage);
        stage();
        QTimer::singleShot(0, this, &PhaseRunner::advance);
    }

    QVector<Entry> stages_;
    std::function<bool()> guard_;
    std::function<void()> finish_;
    int index_ = 0;
};

// ═══════════════════════════════════════════════════════════════════════════
//  Live interface language
//
// This probe links mikai_player, not mikai_app, so it cannot call the app's
// applyInterfaceLanguage. What it does here is that function's contract, not a
// second language policy: an application catalog is loaded before anything
// installed is removed, a failed load leaves the working translation intact,
// and installing or removing a QTranslator posts QEvent::LanguageChange to
// every open widget. That event is what the shell, the float, the popovers and
// the Compare window answer - the live switch under test. The Settings
// selector that writes the preference is covered by native_settings_probe;
// this scenario applies the real catalog directly to an already-open player.
//
// The real manager switches QLocale::setDefault together with its
// translators, so the default locale is part of the same contract: number,
// date and measurement text drawn by Qt follows it. This helper does the same
// and restores whatever locale the process had before the scenario, so the
// focused smoke proves the pair (translators and locale) moves as one unit.
// ═══════════════════════════════════════════════════════════════════════════

class ProbeLanguage {
public:
    ProbeLanguage() : priorLocale_(QLocale()) {}

    ~ProbeLanguage() {
        // The application holds the pointer, so a translator that goes out of
        // scope while installed would leave a dangling entry. Removed the way
        // it was installed; the locale is a process default and is put back
        // even if the application is already gone.
        auto* app = QCoreApplication::instance();
        if (app != nullptr) {
            if (application_) app->removeTranslator(application_.get());
            if (platform_) app->removeTranslator(platform_.get());
        }
        QLocale::setDefault(priorLocale_);
    }

    bool apply(const QString& preference, const QString& catalog, QString* reason) {
        if (preference == QLatin1String("uk")) {
            return applyUkrainian(catalog, reason);
        }
        return applyEnglish();
    }

    QString active() const { return active_; }
    QString catalog() const { return catalog_; }

    // The pair under test: the translators carry the words, the default
    // locale carries the Qt-drawn numbers and dates. The application changes
    // both together, so a switch that moved only one of them is a regression
    // this scenario has to see.
    bool localeMatches(const QString& code) const {
        const QLocale current = QLocale();
        if (code == QLatin1String("uk")) {
            return current.language() == QLocale::Ukrainian &&
                   current.territory() == QLocale::Ukraine;
        }
        if (code == QLatin1String("en")) {
            return current.language() == QLocale::English &&
                   current.territory() == QLocale::UnitedStates;
        }
        return false;
    }

    // Number formatting is what makes the locale observable in a running
    // player rather than a bookkeeping value: Ukrainian writes decimals with
    // a comma, English with a point.
    bool usesUkrainianNumbers() const {
        return QLocale().decimalPoint() == QLatin1String(",");
    }

    QString localeTag() const { return QLocale().name(); }
    QLocale priorLocale() const { return priorLocale_; }

private:
    bool applyEnglish() {
        auto* app = QCoreApplication::instance();
        if (app == nullptr) return false;
        if (application_) {
            app->removeTranslator(application_.get());
            application_.reset();
        }
        if (platform_) {
            app->removeTranslator(platform_.get());
            platform_.reset();
        }
        active_ = QStringLiteral("en");
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        return true;
    }

    bool applyUkrainian(const QString& suppliedCatalog, QString* reason) {
        auto* app = QCoreApplication::instance();
        if (app == nullptr) return false;
        const QString path = suppliedCatalog.isEmpty() ? catalog_ : suppliedCatalog;
        auto application = std::make_unique<QTranslator>();
        if (path.isEmpty() || !application->load(path) || application->isEmpty()) {
            if (reason != nullptr) {
                *reason = QStringLiteral("the application catalog could not be loaded: %1").arg(path);
            }
            return false;
        }

        // The Qt platform catalog is optional; the application catalog decides
        // success. The search follows the packaged layout first, then the
        // installed Qt prefix, the same order the application uses.
        const QLocale locale(QLocale::Ukrainian, QLocale::Ukraine);
        std::unique_ptr<QTranslator> platform;
        const QStringList searchPaths{
            QCoreApplication::applicationDirPath() + QStringLiteral("/translations"),
            QLibraryInfo::path(QLibraryInfo::TranslationsPath)};
        for (const QString& directory : searchPaths) {
            auto candidate = std::make_unique<QTranslator>();
            if (candidate->load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                                directory) &&
                !candidate->isEmpty()) {
                platform = std::move(candidate);
                break;
            }
        }

        // Loaded before installed, and both installed before the working pair is
        // removed, so no refusal can leave the application translator-less.
        if (!app->installTranslator(application.get())) {
            if (reason != nullptr) {
                *reason = QStringLiteral("the application rejected the Ukrainian translator");
            }
            return false;
        }
        if (platform && !app->installTranslator(platform.get())) {
            app->removeTranslator(application.get());
            if (reason != nullptr) {
                *reason = QStringLiteral("the application rejected the Qt platform translator");
            }
            return false;
        }
        if (application_) app->removeTranslator(application_.get());
        if (platform_) app->removeTranslator(platform_.get());
        application_ = std::move(application);
        platform_ = std::move(platform);
        catalog_ = path;
        active_ = QStringLiteral("uk");
        QLocale::setDefault(locale);
        return true;
    }

    std::unique_ptr<QTranslator> application_;
    std::unique_ptr<QTranslator> platform_;
    QString active_ = QStringLiteral("en");
    QString catalog_;
    QLocale priorLocale_;
};

void checkText(bool ok, const QString& what) {
    check(ok, what.toUtf8().constData());
}

QStringList widgetTexts(QWidget* widget) {
    QStringList values;
    if (widget == nullptr) return values;
    values << widget->windowTitle() << widget->toolTip() << widget->accessibleName();
    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        values << button->text();
    }
    if (auto* label = qobject_cast<QLabel*>(widget)) {
        values << label->text();
    }
    return values;
}

QStringList textsForName(QWidget* root, const QString& name) {
    return widgetTexts(root != nullptr ? root->findChild<QWidget*>(name) : nullptr);
}

QStringList namedTexts(QWidget* root, const QStringList& names) {
    QStringList values;
    for (const QString& name : names) {
        values << name << textsForName(root, name);
    }
    return values;
}

QStringList labelsUnder(QWidget* root) {
    QStringList values;
    if (root == nullptr) return values;
    for (QLabel* label : root->findChildren<QLabel*>()) {
        values << label->text();
    }
    return values;
}

QStringList visibleButtonsUnder(QWidget* root) {
    QStringList values;
    if (root == nullptr) return values;
    for (QAbstractButton* button : root->findChildren<QAbstractButton*>()) {
        if (!button->isVisible()) continue;
        values << button->text() << button->toolTip() << button->accessibleName();
    }
    return values;
}

QStringList menuTexts(MenuPopup* menu) {
    QStringList values;
    if (menu == nullptr) return values;
    for (int spot = 0; spot < menu->rowCount(); ++spot) {
        const MenuEntry entry = menu->entryAt(spot);
        values << entry.heading << entry.text << entry.tooltip;
    }
    return values;
}

QStringList compareTexts(CompareWindow* compare) {
    QStringList values;
    if (compare == nullptr) return values;
    values << QStringLiteral("windowTitle") << compare->windowTitle();
    values << namedTexts(compare, {QStringLiteral("compareSwap"),
                                   QStringLiteral("compareRecapture"),
                                   QStringLiteral("comparePinBtn"),
                                   QStringLiteral("compareFit"),
                                   QStringLiteral("compareZoom")});
    for (const char* name : {"compareMode", "compareFilter"}) {
        if (auto* box = compare->findChild<QComboBox*>(QString::fromLatin1(name))) {
            for (int index = 0; index < box->count(); ++index) {
                values << box->itemText(index);
            }
        }
    }
    // The toolbar's fixed captions (Mode:, Filter:, Split:, Flicker/Gain) are
    // unnamed labels; the pixel readout and the zoom badge are state, not
    // captions, so they are left out of the corpus.
    if (auto* card = compare->findChild<QWidget*>(QStringLiteral("compareToolbarCard"))) {
        for (QLabel* label : card->findChildren<QLabel*>()) {
            if (label->objectName() == QLatin1String("compareHover") ||
                label->objectName() == QLatin1String("compareHoverSwatch")) {
                continue;
            }
            values << label->text();
        }
    }
    return values;
}

QStringList compareSelections(CompareWindow* compare) {
    QStringList values;
    if (compare == nullptr) return values;
    for (QWidget* bar : compare->findChildren<QWidget*>(QStringLiteral("compareSelectorBar"))) {
        for (QAbstractButton* button : bar->findChildren<QAbstractButton*>()) {
            values << button->text() << button->toolTip() << button->accessibleName();
        }
    }
    return values;
}

int compareSelectorCount(CompareWindow* compare) {
    return compare != nullptr
               ? compare->findChildren<QWidget*>(QStringLiteral("compareSelectorBar")).size()
               : 0;
}

struct PlaybackFacts {
    QString sourceUrl;
    quint64 sourceEpoch = 0;
    int height = 0;
    int preset = -1;
    double speed = 0.0;
    bool playing = false;
    quint64 pauseGeneration = 0;
    double position = 0.0;
    bool dialogVisible = false;
    bool pipActive = false;
};

PlaybackFacts captureFacts(PlayerDialog* dialog, VideoSurface* surface, RangeSlider* speed) {
    PlaybackFacts facts;
    if (surface != nullptr) {
        facts.sourceUrl = surface->sourceUrl();
        facts.sourceEpoch = surface->sourceEpoch();
        facts.preset = static_cast<int>(surface->anime4KMode());
        facts.playing = surface->isPlaying();
        facts.pauseGeneration = surface->pauseRequestGeneration();
        facts.position = surface->position();
    }
    if (dialog != nullptr) {
        facts.height = dialog->currentHeight();
        facts.dialogVisible = dialog->isVisible();
        facts.pipActive = dialog->isPipActive();
    }
    if (speed != nullptr) facts.speed = speed->value();
    return facts;
}

void checkStableFacts(const PlaybackFacts& before, const PlaybackFacts& after,
                      const QString& when) {
    std::fprintf(stderr,
                 "     facts %s: url=%s epoch=%llu height=%d preset=%d speed=%.3f "
                 "pauseGen=%llu playing=%d\n",
                 qPrintable(when), qPrintable(after.sourceUrl),
                 static_cast<unsigned long long>(after.sourceEpoch), after.height,
                 after.preset, after.speed,
                 static_cast<unsigned long long>(after.pauseGeneration), int(after.playing));
    checkText(after.sourceUrl == before.sourceUrl && !after.sourceUrl.isEmpty(),
              when + QStringLiteral(": the source URL is untouched"));
    checkText(after.sourceEpoch == before.sourceEpoch && after.sourceEpoch > 0,
              when + QStringLiteral(": the source epoch shows no reopen"));
    checkText(after.height == before.height && after.height > 0,
              when + QStringLiteral(": the chosen quality is untouched"));
    checkText(after.preset == before.preset && after.preset >= 0,
              when + QStringLiteral(": the chosen preset is untouched"));
    checkText(qFuzzyCompare(after.speed, before.speed),
              when + QStringLiteral(": the chosen speed is untouched"));
}

bool waitPositionBeyond(VideoSurface* surface, double baseline, int timeoutMs = 5000) {
    return waitFor(
        [surface, baseline] { return surface != nullptr && surface->position() > baseline + 0.4; },
        timeoutMs);
}

bool widgetInside(QWidget* parent, QWidget* child) {
    if (parent == nullptr || child == nullptr || !child->isVisible()) return false;
    const QRect geometry(child->mapTo(parent, QPoint(0, 0)), child->size());
    return parent->rect().contains(geometry);
}

QJsonObject evidenceFromFacts(const PlaybackFacts& facts) {
    QJsonObject object;
    object.insert(QStringLiteral("sourceUrl"), facts.sourceUrl);
    object.insert(QStringLiteral("sourceEpoch"), static_cast<double>(facts.sourceEpoch));
    object.insert(QStringLiteral("height"), facts.height);
    object.insert(QStringLiteral("preset"), facts.preset);
    object.insert(QStringLiteral("speed"), facts.speed);
    object.insert(QStringLiteral("pauseGeneration"), static_cast<double>(facts.pauseGeneration));
    object.insert(QStringLiteral("playing"), facts.playing);
    object.insert(QStringLiteral("position"), facts.position);
    object.insert(QStringLiteral("dialogVisible"), facts.dialogVisible);
    object.insert(QStringLiteral("pipActive"), facts.pipActive);
    return object;
}

void saveEvidenceJson(const QJsonObject& evidence) {
    QFile file(QStringLiteral("interaction-language-evidence.json"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "FAIL the language evidence JSON was written\n");
        ++failures;
        return;
    }
    file.write(QJsonDocument(evidence).toJson(QJsonDocument::Indented));
    file.close();
}

bool saveShot(QWidget* widget, const QString& name, QJsonArray* screenshots) {
    if (widget == nullptr) return false;
    const bool saved = widget->grab().save(name);
    if (saved && screenshots != nullptr) screenshots->append(name);
    return saved;
}

// The focused case is selected by `--language-smoke`, so a live-language run
// does not have to pay for (or depend on) the interaction stages that drive
// quality reloads, pixels, PiP ownership and the Esc ladder. It requires real
// motion media: a language switch is proven against a picture that is still
// being delivered.
int runLanguageSmoke(const QString& url, PlayerHost* host) {
    const QString catalog = qEnvironmentVariable("MIKAI_PROBE_TRANSLATION");
    QJsonObject evidence;
    QJsonArray screenshots;
    evidence.insert(QStringLiteral("probe"), QStringLiteral("interaction_probe"));
    evidence.insert(QStringLiteral("case"), QStringLiteral("language-smoke"));
    evidence.insert(QStringLiteral("media"), url);
    evidence.insert(QStringLiteral("catalog"), catalog);
    QJsonArray languages;
    languages.append(QStringLiteral("en"));
    languages.append(QStringLiteral("uk"));
    languages.append(QStringLiteral("en"));
    evidence.insert(QStringLiteral("languages"), languages);

    ProbeLanguage language;
    if (catalog.isEmpty() || !QFileInfo(catalog).isFile()) {
        check(false, "MIKAI_PROBE_TRANSLATION names the built application catalog");
        evidence.insert(QStringLiteral("passed"), false);
        evidence.insert(QStringLiteral("checks"), checks);
        evidence.insert(QStringLiteral("failures"), failures);
        saveEvidenceJson(evidence);
        std::fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    QString reason;
    evidence.insert(QStringLiteral("localePrior"), language.priorLocale().name());
    check(language.apply(QStringLiteral("en"), catalog, &reason),
          "the focused scenario starts in English with the catalog available");
    check(language.active() == QLatin1String("en"), "the probe knows its working language");
    checkText(language.localeMatches(QStringLiteral("en")),
              QStringLiteral("the English baseline also sets the default locale"));
    checkText(!language.usesUkrainianNumbers(),
              QStringLiteral("the English baseline formats numbers with the English decimal mark"));
    evidence.insert(QStringLiteral("localeBaseline"), language.localeTag());
    std::fprintf(stderr, "case: language-smoke catalog=%s media=%s\n", qPrintable(catalog),
                 qPrintable(url));

    const QList<QualityEntry> qualities = {{480, url}, {720, url}, {1080, url}};
    auto* dialog = new PlayerDialog(QStringLiteral("probe language smoke"), url, qualities, 720,
                                    host);
    QPointer<PlayerDialog> dialogGuard(dialog);
    QVector<QVariantMap> playlist;
    for (int episode = 1; episode <= 5; ++episode) {
        playlist.append(QVariantMap{{"label", QStringLiteral("Серія %1").arg(episode)},
                                    {"embed", QStringLiteral("e%1").arg(episode)}});
    }
    dialog->setPlaylist(playlist, 2);
    dialog->setVariants({QVariantMap{{"label", QStringLiteral("Ashdi 720p")},
                                     {"embed", QStringLiteral("e1")},
                                     {"kind", QStringLiteral("voice")}},
                         QVariantMap{{"label", QStringLiteral("Суб 1080p")},
                                     {"embed", QStringLiteral("e4")},
                                     {"kind", QStringLiteral("sub")}}},
                        QStringLiteral("e1"));
    dialog->startPlayer();
    dialog->showFullScreen();

    auto* shell = dialog->findChild<PlayerShell*>();
    auto* surface = dialog->findChild<VideoSurface*>();
    auto* flyout = dialog->findChild<SettingsFlyout*>();
    auto* speed = dialog->findChild<RangeSlider*>(QStringLiteral("playerSpeedSlider"));
    check(shell != nullptr && surface != nullptr,
          "the focused case built a real player with its surface");
    if (shell == nullptr || surface == nullptr) {
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        evidence.insert(QStringLiteral("passed"), false);
        evidence.insert(QStringLiteral("checks"), checks);
        evidence.insert(QStringLiteral("failures"), failures);
        saveEvidenceJson(evidence);
        std::fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
        return 1;
    }

    check(waitFor([surface] { return surface->hasVideo() && surface->duration() > 0.0; }, 20000),
          "the motion fixture loaded before the language switch");

    // A real quality choice, then a real preset and a real speed. These are the
    // viewer-owned values the language switch must not disturb, and each one is
    // set through the same control a viewer uses.
    shell->openSettings();
    check(waitFor([flyout] { return flyout != nullptr && flyout->isVisible(); }, 4000),
          "the quality-and-speed popover opened for the chosen state");
    QPushButton* qualityRow = nullptr;
    if (flyout != nullptr) {
        for (QPushButton* button : flyout->findChildren<QPushButton*>()) {
            if (button->isVisible() && button->text().contains(QStringLiteral("1080p"))) {
                qualityRow = button;
                break;
            }
        }
    }
    check(qualityRow != nullptr, "the popover offers the 1080p stream");
    const quint64 epochBeforeQuality = surface->sourceEpoch();
    if (qualityRow != nullptr) qualityRow->click();
    check(waitFor([dialog] { return dialog->currentHeight() == 1080; }, 10000),
          "the chosen quality becomes the running stream");
    check(waitFor([surface, epochBeforeQuality] {
              return surface->sourceEpoch() != epochBeforeQuality;
          }, 12000),
          "the chosen quality reopened the stream before the language baseline");
    check(waitFor([surface] { return surface->hasVideo() && surface->duration() > 0.0; }, 12000),
          "the chosen stream is loaded before the language switch");
    QTest::qWait(500);
    shell->dismissOverlay();

    shell->applyAnime4KPreset(anime4k::modeId(anime4k::Mode::FastA));
    check(waitFor([surface] { return surface->anime4KMode() == anime4k::Mode::FastA; }, 5000),
          "the chosen preset is applied");
    shell->openSettings();
    check(waitFor([flyout] { return flyout != nullptr && flyout->isVisible(); }, 4000),
          "the popover reopened for the chosen speed");
    if (speed == nullptr) {
        speed = dialog->findChild<RangeSlider*>(QStringLiteral("playerSpeedSlider"));
    }
    check(speed != nullptr, "the speed selector exists");
    if (speed != nullptr) speed->setValue(0.5);
    QTest::qWait(200);
    check(speed != nullptr && std::abs(speed->value() - 0.5) < 0.01,
          "the speed selector holds the chosen rate");
    shell->dismissOverlay();

    surface->setPaused(false);
    check(waitFor([surface] { return surface->isPlaying(); }, 8000),
          "playback runs before the language scenario");
    surface->seek(3.0);
    check(waitFor([surface] { return surface->position() > 1.0; }, 6000),
          "the player is past the start before the language switch");

    const auto switchTo = [&](const QString& code, const QString& label) {
        QString switchReason;
        const bool applied = language.apply(code, catalog, &switchReason);
        checkText(applied, label);
        if (!applied) {
            std::fprintf(stderr, "     %s\n", qPrintable(switchReason));
        } else {
            // The application moves the default locale in the same call that
            // swaps the translators; the player has to keep running with
            // Qt-drawn numbers under the Ukrainian locale, not just Ukrainian
            // words.
            checkText(language.localeMatches(code),
                      QStringLiteral("%1: the default locale follows the translators").arg(code));
            checkText(language.usesUkrainianNumbers() == (code == QLatin1String("uk")),
                      QStringLiteral("%1: Qt number formatting follows the default locale").arg(code));
        }
        QTest::qWait(300);
        QCoreApplication::processEvents();
        return applied;
    };

    const QStringList shellNames{QStringLiteral("playerSettingsButton"),
                                 QStringLiteral("playerEnhanceButton"),
                                 QStringLiteral("playerPlayToggle"),
                                 QStringLiteral("originalFileButton"),
                                 QStringLiteral("playerSeriesToggle")};
    const auto harvestShell = [&] {
        QStringList values{shell->windowTitle()};
        values += namedTexts(dialog, shellNames);
        return values;
    };

    // ── shell chrome and the open settings popover ─────────────────────────
    check(language.active() == QLatin1String("en"), "the shell baseline is English");
    shell->openSettings();
    check(waitFor([flyout] { return flyout != nullptr && flyout->isVisible(); }, 4000),
          "the settings popover is open across the language switch");
    const QStringList shellBefore = harvestShell();
    const QStringList settingsButtonBefore =
        textsForName(dialog, QStringLiteral("playerSettingsButton"));
    const QStringList flyoutBefore =
        namedTexts(flyout, {QStringLiteral("playerSettingsFlyout"),
                            QStringLiteral("playerSpeedSlider"),
                            QStringLiteral("playerQualityCaption"),
                            QStringLiteral("playerQualitySection"),
                            QStringLiteral("playerQualityLabel")}) +
        visibleButtonsUnder(flyout);
    const PlaybackFacts shellFacts = captureFacts(dialog, surface, speed);
    const double shellPosition = surface->position();
    if (switchTo(QStringLiteral("uk"), QStringLiteral("the real catalog switches the player to Ukrainian"))) {
        evidence.insert(QStringLiteral("localeUkrainian"), language.localeTag());
        const QStringList shellUkrainian = harvestShell();
        const QStringList flyoutUkrainian =
            namedTexts(flyout, {QStringLiteral("playerSettingsFlyout"),
                                QStringLiteral("playerSpeedSlider"),
                                QStringLiteral("playerQualityCaption"),
                                QStringLiteral("playerQualitySection"),
                                QStringLiteral("playerQualityLabel")}) +
            visibleButtonsUnder(flyout);
        checkText(shellUkrainian != shellBefore,
                  QStringLiteral("uk: the open shell restates its chrome in place"));
        checkText(textsForName(dialog, QStringLiteral("playerSettingsButton")) !=
                      settingsButtonBefore,
                  QStringLiteral("uk: the settings control's tooltip and spoken name are restated"));
        bool chromeWordingComplete = true;
        for (const char* name : {"playerSettingsButton", "playerEnhanceButton",
                                 "playerPlayToggle"}) {
            QWidget* widget = dialog->findChild<QWidget*>(QString::fromLatin1(name));
            if (widget == nullptr || widget->toolTip().isEmpty() ||
                widget->accessibleName().isEmpty()) {
                chromeWordingComplete = false;
            }
        }
        checkText(chromeWordingComplete,
                  QStringLiteral("uk: the dock chrome keeps a tooltip and an accessible name"));
        checkText(flyoutUkrainian != flyoutBefore,
                  QStringLiteral("uk: the open settings popover restates its captions in place"));
        checkText(flyout != nullptr && flyout->isVisible(),
                  QStringLiteral("uk: the settings popover stays open across the switch"));
        checkText(flyout == nullptr || buttonsFitWithoutOverlap(flyout),
                  QStringLiteral("uk: the restated settings rows still fit without overlap"));
        checkText(dialog->isVisible(),
                  QStringLiteral("uk: the player window stays open across the switch"));

        // A replacement catalog that cannot be loaded is refused before
        // anything installed is touched: the working translators keep
        // answering LanguageChange and the default locale keeps formatting
        // numbers as Ukrainian. This is the branch a stale or missing catalog
        // drives in Settings, and it is checked in both halves because the
        // application switches both together.
        const QStringList shellBeforeRefusal = harvestShell();
        const QString missingCatalog = QCoreApplication::applicationDirPath() +
                                       QStringLiteral("/probe-missing-uk-catalog.qm");
        QString refusalReason;
        const bool refused =
            language.apply(QStringLiteral("uk"), missingCatalog, &refusalReason);
        QTest::qWait(100);
        QCoreApplication::processEvents();
        checkText(!refused, QStringLiteral("uk: a catalog that does not load is refused"));
        checkText(!refusalReason.isEmpty(),
                  QStringLiteral("uk: the refusal explains why the catalog was not applied"));
        checkText(language.active() == QLatin1String("uk") &&
                      language.catalog() != missingCatalog,
                  QStringLiteral("uk: the refused catalog does not replace the working one"));
        checkText(language.localeMatches(QStringLiteral("uk")) &&
                      language.usesUkrainianNumbers(),
                  QStringLiteral("uk: the refused catalog keeps the Ukrainian locale and numbers"));
        checkText(harvestShell() == shellBeforeRefusal,
                  QStringLiteral("uk: the refused catalog keeps the installed translation"));
        evidence.insert(QStringLiteral("refusedCatalog"), missingCatalog);
        evidence.insert(QStringLiteral("localeAfterRefusal"), language.localeTag());

        const PlaybackFacts after = captureFacts(dialog, surface, speed);
        checkStableFacts(shellFacts, after, QStringLiteral("uk shell"));
        checkText(shellFacts.playing && after.playing,
                  QStringLiteral("uk: playback keeps running without a pause or restart"));
        checkText(waitPositionBeyond(surface, shellPosition),
                  QStringLiteral("uk: the playhead keeps moving while the language changes"));
        saveShot(dialog, QStringLiteral("interaction-language-shell-flyout-uk.png"),
                 &screenshots);

        if (switchTo(QStringLiteral("en"), QStringLiteral("the catalog switches the player back to English"))) {
            evidence.insert(QStringLiteral("localeEnglish"), language.localeTag());
            const QStringList shellEnglish = harvestShell();
            const QStringList flyoutEnglish =
                namedTexts(flyout, {QStringLiteral("playerSettingsFlyout"),
                                    QStringLiteral("playerSpeedSlider"),
                                    QStringLiteral("playerQualityCaption"),
                                    QStringLiteral("playerQualitySection"),
                                    QStringLiteral("playerQualityLabel")}) +
                visibleButtonsUnder(flyout);
            checkText(shellEnglish == shellBefore,
                      QStringLiteral("en: the shell's chrome returns to the opening words"));
            checkText(flyoutEnglish == flyoutBefore,
                      QStringLiteral("en: the settings captions return to the opening words"));
            checkText(flyout != nullptr && flyout->isVisible(),
                      QStringLiteral("en: the settings popover stayed open for the whole cycle"));
            const PlaybackFacts after = captureFacts(dialog, surface, speed);
            checkStableFacts(shellFacts, after, QStringLiteral("en shell"));
            checkText(shellFacts.playing && after.playing,
                      QStringLiteral("en: playback still runs after the round trip"));
            checkText(waitPositionBeyond(surface, after.position),
                      QStringLiteral("en: the playhead kept moving through the round trip"));
        }
    }
    shell->dismissOverlay();

    // ── key hints ──────────────────────────────────────────────────────────
    QKeyEvent question(QEvent::KeyPress, Qt::Key_Question, Qt::NoModifier);
    QCoreApplication::sendEvent(shell, &question);
    auto* hints = dialog->findChild<KeyHints*>(QStringLiteral("playerKeyHints"));
    check(waitFor([hints] { return hints != nullptr && hints->isVisible(); }, 3000),
          "the keyboard hints are open across the language switch");
    if (hints != nullptr) {
        const QStringList hintsBefore = namedTexts(hints, {QStringLiteral("playerKeyHints")}) +
                                        labelsUnder(hints);
        const PlaybackFacts hintsFacts = captureFacts(dialog, surface, speed);
        const double hintsPosition = surface->position();
        if (switchTo(QStringLiteral("uk"), QStringLiteral("Ukrainian reaches the open keyboard hints"))) {
            const QStringList hintsUkrainian = namedTexts(hints, {QStringLiteral("playerKeyHints")}) +
                                               labelsUnder(hints);
            checkText(hintsUkrainian != hintsBefore,
                      QStringLiteral("uk: the open hints restate their rows in place"));
            checkText(hints->isVisible(), QStringLiteral("uk: the hints stay open"));
            checkText(widgetInside(dialog, hints),
                      QStringLiteral("uk: the restated hints stay inside the player window"));
            const PlaybackFacts after = captureFacts(dialog, surface, speed);
            checkStableFacts(hintsFacts, after, QStringLiteral("uk hints"));
            checkText(after.playing && waitPositionBeyond(surface, hintsPosition),
                      QStringLiteral("uk: the picture keeps playing while the hints restate"));
            saveShot(hints, QStringLiteral("interaction-language-hints-uk.png"), &screenshots);
            if (switchTo(QStringLiteral("en"), QStringLiteral("English returns to the open keyboard hints"))) {
                const QStringList hintsEnglish = namedTexts(hints, {QStringLiteral("playerKeyHints")}) +
                                                 labelsUnder(hints);
                checkText(hintsEnglish == hintsBefore,
                          QStringLiteral("en: the hints return to their opening rows"));
                checkText(hints->isVisible(), QStringLiteral("en: the hints stayed open"));
                const PlaybackFacts after = captureFacts(dialog, surface, speed);
                checkStableFacts(hintsFacts, after, QStringLiteral("en hints"));
                checkText(after.playing && waitPositionBeyond(surface, after.position),
                          QStringLiteral("en: playback continued through the hints cycle"));
            }
        }
        shell->dismissOverlay();
    }

    // ── the picture menu ───────────────────────────────────────────────────
    shell->openPictureMenu(QPoint(60, 60));
    auto* menu = dialog->findChild<MenuPopup*>(QStringLiteral("playerPictureMenu"));
    check(waitFor([menu] { return menu != nullptr && menu->isVisible(); }, 3000),
          "the picture menu is open across the language switch");
    if (menu != nullptr) {
        const QStringList menuBefore = menuTexts(menu);
        const int rowsBefore = menu->rowCount();
        const PlaybackFacts menuFacts = captureFacts(dialog, surface, speed);
        const double menuPosition = surface->position();
        if (switchTo(QStringLiteral("uk"), QStringLiteral("Ukrainian reaches the open picture menu"))) {
            const QStringList menuUkrainian = menuTexts(menu);
            checkText(menuUkrainian != menuBefore,
                      QStringLiteral("uk: the open picture menu restates its rows in place"));
            checkText(menu->isVisible() && menu->rowCount() == rowsBefore,
                      QStringLiteral("uk: the picture menu stays open with the same rows"));
            checkText(buttonsFitWithoutOverlap(menu),
                      QStringLiteral("uk: the restated menu rows still fit without overlap"));
            checkText(widgetInside(dialog, menu),
                      QStringLiteral("uk: the restated menu stays inside the player window"));
            const PlaybackFacts after = captureFacts(dialog, surface, speed);
            checkStableFacts(menuFacts, after, QStringLiteral("uk menu"));
            checkText(after.playing && waitPositionBeyond(surface, menuPosition),
                      QStringLiteral("uk: the picture keeps playing behind the restated menu"));
            saveShot(menu, QStringLiteral("interaction-language-menu-uk.png"), &screenshots);
            if (switchTo(QStringLiteral("en"), QStringLiteral("English returns to the open picture menu"))) {
                checkText(menuTexts(menu) == menuBefore,
                          QStringLiteral("en: the picture menu returns to its opening rows"));
                checkText(menu->isVisible(), QStringLiteral("en: the picture menu stayed open"));
                const PlaybackFacts after = captureFacts(dialog, surface, speed);
                checkStableFacts(menuFacts, after, QStringLiteral("en menu"));
                checkText(after.playing && waitPositionBeyond(surface, after.position),
                          QStringLiteral("en: playback continued through the menu cycle"));
            }
        }
        shell->dismissOverlay();
    }

    // ── Compare over a real frozen frame ───────────────────────────────────
    // The candidates are Original and an Anime4K chain: CompareWindow opens
    // side A on the id it is handed and side B on the first other candidate in
    // the catalogue, so "off" pairs the original with Anime4K and nothing here
    // asks an AnimeJaNai model to infer.
    // Compare takes its hold from what the core is observed to hold, not from
    // the announced intent: a pause write still being confirmed can leave the
    // two disagreeing for one pump, and a hold acquired in that window is
    // reported as resumed before the freeze is taken.
    check(waitFor([surface] { return surface->isPlaying() && !surface->pauseObserved(); }, 5000),
          "playback is running before Compare freezes it");
    QPointer<CompareWindow> compare = new CompareWindow(surface, QStringLiteral("off"), nullptr);
    compare->resize(1100, 700);
    compare->show();
    const bool frozen = waitFor(
        [&compare, surface] {
            if (compare.isNull()) return false;
            const QImage frame = compare->frozenFrame();
            // The freeze is taken from a stream reopened at the start, so the
            // first frame's timestamp is legitimately 0.000: what this stage
            // needs is a real captured picture with a real non-negative
            // timestamp - `>= 0.0` also rejects a NaN - and it is the value
            // staying *unchanged* across the language switches below that the
            // frozen window is held to, never a non-zero one.
            return !frame.isNull() && compare->frozenPlaybackTimestamp().has_value() &&
                   *compare->frozenPlaybackTimestamp() >= 0.0 &&
                   compare->frozenSourceEpoch() > 0 &&
                   compare->frozenSourceEpoch() == surface->sourceEpoch() &&
                   compare->playbackHeld() && !compare->comparisonPending();
        },
        30000);
    check(frozen, "Compare froze a real frame from the motion fixture and holds playback");
    // A refused freeze has to say which half of it is missing: the frame, the
    // timestamp, the source epoch, the hold, a pending build or the plan lock -
    // otherwise an empty capture error reads as "nothing happened" instead of the
    // one state the window was actually left in.
    const QByteArray frozenStamp =
        compare.isNull() || !compare->frozenPlaybackTimestamp().has_value()
            ? QByteArray("(none)")
            : QByteArray::number(*compare->frozenPlaybackTimestamp(), 'f', 3);
    const QByteArray candidateError =
        compare.isNull() ? QByteArray("<gone>")
                         : compare->candidateError(QStringLiteral("off")).toUtf8();
    const QByteArray captureError =
        compare.isNull() ? QByteArray("<gone>") : compare->captureError().toUtf8();
    const QByteArray lockError =
        compare.isNull() ? QByteArray("<gone>") : compare->lockError().toUtf8();
    std::fprintf(stderr,
                 "     compare: frozen=%d epoch=%llu sourceEpoch=%llu timestamp=%s held=%d "
                 "pending=%d capturing=%d queued=%d frozenNull=%d playing=%d pauseGen=%llu "
                 "pauseObserved=%d error=%s capture=%s lock=%s\n",
                 int(frozen),
                 static_cast<unsigned long long>(compare.isNull() ? 0 : compare->frozenSourceEpoch()),
                 static_cast<unsigned long long>(surface->sourceEpoch()),
                 frozenStamp.constData(), compare.isNull() ? 0 : int(compare->playbackHeld()),
                 compare.isNull() ? 0 : int(compare->comparisonPending()),
                 compare.isNull() ? 1 : int(compare->captureInProgress()),
                 compare.isNull() ? -1 : int(compare->queuedNativeChanges()),
                 compare.isNull() ? 1 : int(compare->frozenFrame().isNull()),
                 int(surface->isPlaying()),
                 static_cast<unsigned long long>(surface->pauseRequestGeneration()),
                 int(surface->pauseObserved()),
                 candidateError.constData(), captureError.constData(), lockError.constData());
    if (!frozen) {
        // The refusal belongs in the run's evidence, not only on stderr: a bare
        // "capture=" with nothing in it cannot say which half of the freeze was
        // missing, and this is the artifact the next reader gets.
        QJsonObject refusal;
        refusal.insert(QStringLiteral("frozen"), false);
        refusal.insert(QStringLiteral("candidateId"), QStringLiteral("off"));
        refusal.insert(QStringLiteral("candidateError"), QString::fromUtf8(candidateError));
        refusal.insert(QStringLiteral("captureError"), QString::fromUtf8(captureError));
        refusal.insert(QStringLiteral("lockError"), QString::fromUtf8(lockError));
        refusal.insert(QStringLiteral("timestamp"), QString::fromUtf8(frozenStamp));
        refusal.insert(QStringLiteral("sourceEpoch"),
                       static_cast<double>(surface->sourceEpoch()));
        refusal.insert(QStringLiteral("frozenSourceEpoch"),
                       compare.isNull() ? 0.0
                                        : static_cast<double>(compare->frozenSourceEpoch()));
        refusal.insert(QStringLiteral("frozenFrameNull"),
                       compare.isNull() ? true : compare->frozenFrame().isNull());
        refusal.insert(QStringLiteral("playbackHeld"),
                       compare.isNull() ? false : compare->playbackHeld());
        refusal.insert(QStringLiteral("comparisonPending"),
                       compare.isNull() ? false : compare->comparisonPending());
        refusal.insert(QStringLiteral("captureInProgress"),
                       compare.isNull() ? false : compare->captureInProgress());
        refusal.insert(QStringLiteral("queuedNativeChanges"),
                       compare.isNull() ? -1 : compare->queuedNativeChanges());
        // The hold is taken against what the core is observed to hold, so a
        // refusal has to carry that observation and the pause request it was
        // compared with; without them "resumed while holding" cannot be told
        // apart from a hold that was never acquired.
        refusal.insert(QStringLiteral("intentPlaying"), surface->isPlaying());
        refusal.insert(QStringLiteral("pauseRequestGeneration"),
                       static_cast<double>(surface->pauseRequestGeneration()));
        refusal.insert(QStringLiteral("pauseObserved"), surface->pauseObserved());
        evidence.insert(QStringLiteral("compare"), refusal);
    }
    if (!frozen && !compare.isNull()) {
        compare->close();
        compare->deleteLater();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    if (!compare.isNull() && frozen) {
        const QImage frozenBefore = compare->frozenFrame();
        const std::optional<double> timestampBefore = compare->frozenPlaybackTimestamp();
        const quint64 epochBefore = compare->frozenSourceEpoch();
        const QString modeBefore =
            compare->findChild<QComboBox*>(QStringLiteral("compareMode")) != nullptr
                ? compare->findChild<QComboBox*>(QStringLiteral("compareMode"))
                      ->currentData()
                      .toString()
                : QString();
        auto* splitBox = compare->findChild<QDoubleSpinBox*>(QStringLiteral("compareSplit"));
        const double splitBefore = splitBox != nullptr ? splitBox->value() : -1.0;
        const QStringList selectionBefore = compareSelections(compare);
        const int selectorCountBefore = compareSelectorCount(compare);
        const QImage renderedABefore = compare->renderedSideA();
        const QImage renderedBBefore = compare->renderedSideB();
        const QStringList captionsBefore = compareTexts(compare);
        const PlaybackFacts compareFacts = captureFacts(dialog, surface, speed);
        const double comparePosition = surface->position();
        check(!compare->renderedSideA().isNull() && !compare->renderedSideB().isNull(),
              "Compare rendered both sides of the frozen frame");

        if (switchTo(QStringLiteral("uk"), QStringLiteral("Ukrainian reaches the open Compare window"))) {
            check(!compare.isNull() && compare->isVisible(),
                  "uk: the Compare window stays open across the switch");
            check(!compare.isNull() && compare->frozenFrame() == frozenBefore,
                  "uk: the frozen frame is byte-identical after the switch");
            check(!compare.isNull() && compare->frozenPlaybackTimestamp() == timestampBefore,
                  "uk: the frozen timestamp is unchanged");
            check(!compare.isNull() && compare->frozenSourceEpoch() == epochBefore,
                  "uk: the frozen source epoch is unchanged");
            check(!compare.isNull() && compare->playbackHeld(),
                  "uk: Compare still owns its playback hold");
            check(!compare.isNull() && !compare->comparisonPending(),
                  "uk: restating captions starts no capture or rebuild");
            auto* modeAfter = compare->findChild<QComboBox*>(QStringLiteral("compareMode"));
            auto* splitAfter = compare->findChild<QDoubleSpinBox*>(QStringLiteral("compareSplit"));
            checkText(modeAfter != nullptr && modeAfter->currentData().toString() == modeBefore,
                      QStringLiteral("uk: the comparison mode is unchanged"));
            checkText(splitAfter != nullptr && splitAfter->value() == splitBefore,
                      QStringLiteral("uk: the split position is unchanged"));
            checkText(compareSelectorCount(compare) == selectorCountBefore,
                      QStringLiteral("uk: both side selectors are still on screen"));
            checkText(compare->renderedSideA() == renderedABefore &&
                          compare->renderedSideB() == renderedBBefore,
                      QStringLiteral("uk: the selected sides still render the same pictures"));
            const QStringList captionsUkrainian = compareTexts(compare);
            checkText(captionsUkrainian != captionsBefore,
                      QStringLiteral("uk: the Compare captions restate in place"));
            checkText(compare->windowTitle() != captionsBefore.value(1),
                      QStringLiteral("uk: the Compare window title is retranslated"));
            const PlaybackFacts after = captureFacts(dialog, surface, speed);
            checkStableFacts(compareFacts, after, QStringLiteral("uk compare"));
            checkText(!after.playing && std::abs(after.position - comparePosition) < 0.2,
                      QStringLiteral("uk: Compare's hold keeps the player frozen"));
            saveShot(compare, QStringLiteral("interaction-language-compare-uk.png"), &screenshots);
            QJsonObject compareEvidence;
            compareEvidence.insert(QStringLiteral("frozenWidth"), frozenBefore.width());
            compareEvidence.insert(QStringLiteral("frozenHeight"), frozenBefore.height());
            compareEvidence.insert(QStringLiteral("timestamp"),
                                   timestampBefore.has_value() ? *timestampBefore : -1.0);
            compareEvidence.insert(QStringLiteral("sourceEpoch"),
                                   static_cast<double>(epochBefore));
            compareEvidence.insert(QStringLiteral("mode"), modeBefore);
            compareEvidence.insert(QStringLiteral("split"), splitBefore);
            compareEvidence.insert(QStringLiteral("selection"),
                                   QJsonArray::fromStringList(selectionBefore));
            compareEvidence.insert(QStringLiteral("selectionUkrainian"),
                                   QJsonArray::fromStringList(compareSelections(compare)));
            compareEvidence.insert(QStringLiteral("selectorCount"), selectorCountBefore);
            compareEvidence.insert(QStringLiteral("held"), true);
            compareEvidence.insert(QStringLiteral("renderedAWidth"),
                                   compare->renderedSideA().width());
            compareEvidence.insert(QStringLiteral("renderedAHeight"),
                                   compare->renderedSideA().height());
            compareEvidence.insert(QStringLiteral("renderedBWidth"),
                                   compare->renderedSideB().width());
            compareEvidence.insert(QStringLiteral("renderedBHeight"),
                                   compare->renderedSideB().height());
            compareEvidence.insert(QStringLiteral("captureError"), compare->captureError());
            evidence.insert(QStringLiteral("compare"), compareEvidence);

            if (switchTo(QStringLiteral("en"), QStringLiteral("English returns to the open Compare window"))) {
                check(!compare.isNull() && compare->isVisible(),
                      "en: the Compare window stayed open");
                check(!compare.isNull() && compare->frozenFrame() == frozenBefore,
                      "en: the frozen frame survived the round trip");
                check(!compare.isNull() && compare->frozenPlaybackTimestamp() == timestampBefore,
                      "en: the frozen timestamp survived the round trip");
                check(!compare.isNull() && compare->frozenSourceEpoch() == epochBefore,
                      "en: the frozen source epoch survived the round trip");
                check(!compare.isNull() && compare->playbackHeld(),
                      "en: the playback hold survived the round trip");
                checkText(compareSelectorCount(compare) == selectorCountBefore &&
                              compare->renderedSideA() == renderedABefore &&
                              compare->renderedSideB() == renderedBBefore,
                          QStringLiteral("en: the selected sides survive the round trip"));
                checkText(compareTexts(compare) == captionsBefore,
                          QStringLiteral("en: the Compare captions return to the opening words"));
                const PlaybackFacts after = captureFacts(dialog, surface, speed);
                checkStableFacts(compareFacts, after, QStringLiteral("en compare"));
                checkText(!after.playing && std::abs(after.position - comparePosition) < 0.2,
                          QStringLiteral("en: Compare still holds playback frozen"));
                saveShot(compare, QStringLiteral("interaction-language-compare-en.png"),
                         &screenshots);
            }
        }

        // Closing Compare must release the hold it took. The player was
        // playing when Compare opened, so the original pause intent was "run"
        // and that is what the release has to restore.
        if (!compare.isNull()) compare->close();
        check(waitFor(
                  [&compare, surface] {
                      return compare.isNull() ||
                             (!compare->isVisible() && !compare->playbackHeld() &&
                              !compare->comparisonPending() && !surface->nativePlanLocked());
                  },
                  8000),
              "closing Compare releases its hold and leaves no pending capture");
        check(waitFor([surface] { return surface->isPlaying(); }, 5000),
              "closing Compare restores the original pause intent (playing)");
        check(waitPositionBeyond(surface, surface->position()),
              "closing Compare resumes frame delivery without reopening the media");
        if (!compare.isNull()) {
            compare->deleteLater();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }

    // ── PiP over the same running player ───────────────────────────────────
    // Back to a known early position: the language cycles above advance real
    // motion, and the fixture is short enough that a slow Compare render could
    // otherwise leave the float with nothing left to deliver.
    surface->seek(2.0);
    check(waitFor([surface] { return surface->position() < 4.0; }, 5000),
          "the player returns to an early position for the PiP cycle");
    if (surface != nullptr) {
        for (const QPointF& at : {QPointF(40, 40), QPointF(60, 60)}) {
            QMouseEvent move(QEvent::MouseMove, at, surface->mapToGlobal(at), Qt::NoButton,
                             Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(surface, &move);
        }
    }
    check(waitFor([surface] { return surface->isPlaying(); }, 5000),
          "playback runs before PiP opens");
    shell->wake();
    QTest::qWait(150);
    QPushButton* pipButton = buttonWithGlyph(dialog, IconPainter::pip);
    check(pipButton != nullptr && pipButton->isVisible(), "the PiP control is on the dock");
    if (pipButton != nullptr) pipButton->click();
    PipFloat* pip = awaitVisiblePipFloat(8000);
    check(pip != nullptr, "the float opened for the live-language PiP cycle");
    if (pip != nullptr) {
        QPointer<PipFloat> pipGuard(pip);
        VideoSurface* floatSurface = pip->videoSurface();
        check(waitFor(
                  [floatSurface] {
                      return floatSurface != nullptr && floatSurface->hasVideo();
                  },
                  15000),
              "the float loaded the same motion fixture");
        floatSurface->setPaused(false);
        check(waitFor([floatSurface] { return floatSurface->isPlaying(); }, 6000),
              "the float is playing before the language switch");
        QTest::qWait(500);
        // The float fades its chrome out on its own timer. A restatement is
        // checked on visible controls, so the pointer is put back on the float
        // for each reading rather than racing the fade.
        hoverPipControls(pipGuard);
        QTest::qWait(100);

        const QStringList pipNames{QStringLiteral("playerPip"), QStringLiteral("pipRestore"),
                                   QStringLiteral("pipClose"), QStringLiteral("pipPlay"),
                                   QStringLiteral("pipMute"), QStringLiteral("pipVolume"),
                                   QStringLiteral("pipVolumeChip")};
        const QStringList pipBefore = [&] {
            QStringList values{pip->windowTitle()};
            values += namedTexts(pip, pipNames);
            return values;
        }();
        QWidget* chipBefore = pip->findChild<QWidget*>(QStringLiteral("pipVolumeChip"));
        const int chipWidthBefore = chipBefore != nullptr ? chipBefore->width() : -1;
        const auto controlRect = [](QWidget* parent, const char* name) {
            QWidget* control = parent != nullptr
                                   ? parent->findChild<QWidget*>(QString::fromLatin1(name))
                                   : nullptr;
            if (control == nullptr) return QRect();
            return QRect(control->mapTo(parent, QPoint(0, 0)), control->size());
        };
        const QRect restoreRectBefore = controlRect(pip, "pipRestore");
        const QRect closeRectBefore = controlRect(pip, "pipClose");
        const PlaybackFacts mainFacts = captureFacts(dialog, surface, speed);
        const PlaybackFacts floatFacts = captureFacts(dialog, floatSurface, speed);
        const double floatPosition = floatSurface->position();
        auto boundsHold = [&]() {
            if (pipGuard.isNull()) return false;
            QWidget* restore = pipGuard->findChild<QWidget*>(QStringLiteral("pipRestore"));
            QWidget* close = pipGuard->findChild<QWidget*>(QStringLiteral("pipClose"));
            QWidget* mute = pipGuard->findChild<QWidget*>(QStringLiteral("pipMute"));
            QWidget* volume = pipGuard->findChild<QWidget*>(QStringLiteral("pipVolume"));
            QWidget* chip = pipGuard->findChild<QWidget*>(QStringLiteral("pipVolumeChip"));
            QWidget* play = pipGuard->findChild<QWidget*>(QStringLiteral("pipPlay"));
            if (restore == nullptr || close == nullptr || mute == nullptr || volume == nullptr ||
                chip == nullptr || play == nullptr) {
                return false;
            }
            const bool inside = widgetInside(pipGuard, restore) && widgetInside(pipGuard, close) &&
                                widgetInside(pipGuard, mute) && widgetInside(pipGuard, volume) &&
                                widgetInside(pipGuard, chip) && widgetInside(pipGuard, play);
            const QRect restoreRect(restore->mapTo(pipGuard, QPoint(0, 0)), restore->size());
            const QRect closeRect(close->mapTo(pipGuard, QPoint(0, 0)), close->size());
            const QRect chipRect(chip->mapTo(pipGuard, QPoint(0, 0)), chip->size());
            const bool pillWide = chipRect.width() >= mute->width() + volume->width();
            return inside && !restoreRect.intersects(closeRect) && pillWide &&
                   restore->isEnabled() && close->isEnabled();
        };
        check(boundsHold(), "the PiP restore and close bounds hold in English");

        if (switchTo(QStringLiteral("uk"), QStringLiteral("Ukrainian reaches the open PiP float"))) {
            const QStringList pipUkrainian = [&] {
                QStringList values{pipGuard.isNull() ? QString() : pipGuard->windowTitle()};
                values += namedTexts(pipGuard, pipNames);
                return values;
            }();
            checkText(!pipGuard.isNull() && pipGuard->isVisible(),
                      QStringLiteral("uk: the float stays open across the switch"));
            checkText(dialog->isPipActive(),
                      QStringLiteral("uk: the player still reports PiP active"));
            checkText(pipUkrainian != pipBefore,
                      QStringLiteral("uk: the float restates its window title, hints and names in place"));
            bool wordingComplete = true;
            for (const char* name : {"pipRestore", "pipClose", "pipMute", "pipPlay"}) {
                QWidget* widget = pipGuard->findChild<QWidget*>(QString::fromLatin1(name));
                if (widget == nullptr || widget->toolTip().isEmpty() ||
                    widget->accessibleName().isEmpty()) {
                    wordingComplete = false;
                }
            }
            checkText(wordingComplete,
                      QStringLiteral("uk: PiP controls keep a tooltip and an accessible name"));
            hoverPipControls(pipGuard);
            checkText(boundsHold(),
                      QStringLiteral("uk: the longer pill wording leaves restore and close inside the float"));
            checkText(controlRect(pipGuard, "pipRestore") == restoreRectBefore &&
                          controlRect(pipGuard, "pipClose") == closeRectBefore,
                      QStringLiteral("uk: the restated wording does not move restore or close"));
            QWidget* chip = pipGuard->findChild<QWidget*>(QStringLiteral("pipVolumeChip"));
            QWidget* mute = pipGuard->findChild<QWidget*>(QStringLiteral("pipMute"));
            QWidget* volume = pipGuard->findChild<QWidget*>(QStringLiteral("pipVolume"));
            if (chip != nullptr && mute != nullptr && volume != nullptr) {
                checkText(chip->width() == chipWidthBefore &&
                              chip->width() >= mute->width() + volume->width(),
                          QStringLiteral("uk: the pill keeps one fixed width for both controls"));
            }
            const PlaybackFacts mainAfter = captureFacts(dialog, surface, speed);
            const PlaybackFacts floatAfter = captureFacts(dialog, floatSurface, speed);
            checkStableFacts(mainFacts, mainAfter, QStringLiteral("uk PiP player"));
            checkStableFacts(floatFacts, floatAfter, QStringLiteral("uk PiP float"));
            checkText(floatAfter.playing &&
                          waitPositionBeyond(floatSurface, floatPosition, 5000),
                      QStringLiteral("uk: the float keeps delivering frames while restating"));
            saveShot(pipGuard, QStringLiteral("interaction-language-pip-uk.png"), &screenshots);
            QJsonObject pipEvidence;
            pipEvidence.insert(QStringLiteral("restoreInside"), boundsHold());
            pipEvidence.insert(QStringLiteral("sourceEpoch"),
                               static_cast<double>(floatAfter.sourceEpoch));
            pipEvidence.insert(QStringLiteral("playing"), floatAfter.playing);
            pipEvidence.insert(QStringLiteral("position"), floatAfter.position);
            evidence.insert(QStringLiteral("pip"), pipEvidence);

            if (switchTo(QStringLiteral("en"), QStringLiteral("English returns to the open PiP float"))) {
                const QStringList pipEnglish = [&] {
                    QStringList values{pipGuard.isNull() ? QString() : pipGuard->windowTitle()};
                    values += namedTexts(pipGuard, pipNames);
                    return values;
                }();
                checkText(pipEnglish == pipBefore,
                          QStringLiteral("en: the float returns to its opening words"));
                checkText(!pipGuard.isNull() && pipGuard->isVisible(),
                          QStringLiteral("en: the float stayed open for the whole cycle"));
                hoverPipControls(pipGuard);
                checkText(boundsHold(),
                          QStringLiteral("en: restore and close still sit inside the float"));
                checkText(controlRect(pipGuard, "pipRestore") == restoreRectBefore &&
                              controlRect(pipGuard, "pipClose") == closeRectBefore,
                          QStringLiteral("en: restore and close return to their opening bounds"));
                const PlaybackFacts floatAfter = captureFacts(dialog, floatSurface, speed);
                checkText(floatAfter.playing &&
                              waitPositionBeyond(floatSurface, floatPosition + 0.4, 6000),
                          QStringLiteral("en: the float kept playing through the round trip"));
                checkText(floatAfter.sourceEpoch == floatFacts.sourceEpoch &&
                              floatAfter.sourceUrl == floatFacts.sourceUrl,
                          QStringLiteral("en: no media reopen happened under the float"));
            }
        }

        if (!pipGuard.isNull()) {
            auto* restore = pipGuard->findChild<QPushButton*>(QStringLiteral("pipRestore"));
            check(restore != nullptr, "the float still has a restore control after the language cycle");
            if (restore != nullptr) {
                hoverPipControls(pipGuard);
                restore->click();
            }
        }
        check(waitFor(
                  [&pipGuard, &dialogGuard] {
                      return pipGuard.isNull() ||
                             (!pipGuard->isVisible() && !dialogGuard.isNull() &&
                              dialogGuard->isVisible());
                  },
                  8000),
              "the restore returns to the player window without a restart");
        check(waitFor([surface] { return surface->isPlaying(); }, 5000),
              "playback continues in the returned player");
        checkText(!surface->sourceUrl().isEmpty() &&
                      surface->sourceEpoch() == mainFacts.sourceEpoch,
                  QStringLiteral("the PiP round trip did not reopen the media"));
    }

    if (!dialogGuard.isNull()) {
        const PlaybackFacts finalFacts = captureFacts(dialog, surface, speed);
        evidence.insert(QStringLiteral("final"), evidenceFromFacts(finalFacts));
        evidence.insert(QStringLiteral("activeLanguage"), language.active());
        checkText(language.localeMatches(language.active()),
                  QStringLiteral("the closing default locale matches the working language"));
        evidence.insert(QStringLiteral("localeFinal"), language.localeTag());
        evidence.insert(QStringLiteral("screenshots"), screenshots);
        evidence.insert(QStringLiteral("passed"), failures == 0);
        evidence.insert(QStringLiteral("checks"), checks);
        evidence.insert(QStringLiteral("failures"), failures);
        saveEvidenceJson(evidence);
        dialogGuard->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    } else {
        evidence.insert(QStringLiteral("passed"), failures == 0);
        evidence.insert(QStringLiteral("checks"), checks);
        evidence.insert(QStringLiteral("failures"), failures);
        saveEvidenceJson(evidence);
    }
    std::fprintf(stderr, failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // Two ways in: the full interaction walk (the historical argument), and a
    // narrowly named live-language case. The second exists because proving the
    // language contract does not need the quality reloads, pixel comparisons,
    // PiP ownership ladder or Esc walk - and a smoke that spends a minute on
    // them is a smoke nobody runs before touching a translation.
    QString videoArgument;
    bool languageSmoke = false;
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QLatin1String("--language-smoke")) {
            languageSmoke = true;
        } else if (videoArgument.isEmpty()) {
            videoArgument = argument;
        }
    }
    if (videoArgument.isEmpty() || !QFileInfo::exists(videoArgument)) {
        std::fprintf(stderr,
                     "Usage: InteractionProbe <known-duration-video-file> "
                     "[--language-smoke]\n");
        return 3;
    }
    // The last stage closes the last window, and Qt quits the loop when that
    // happens - which returned 0 from app.exec() before the runner could print
    // its verdict, so a run with failures exited as if it had passed.
    app.setQuitOnLastWindowClosed(false);

    // Both scopes are redirected into a directory that dies with this process
    // before the first PlayerDialog, whose constructor reads player/anime4k and
    // player/presetShortcuts. Without it a probe inherits a real session's
    // chosen chain and shortcut map.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiInteractionProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("InteractionProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }

    static StubHost host;
    const QString url = QFileInfo(videoArgument).absoluteFilePath();
    if (languageSmoke) {
        return runLanguageSmoke(url, &host);
    }
    const QList<QualityEntry> qualities = {{480, url}, {720, url}, {1080, url}};

    auto* dialog = new PlayerDialog(QStringLiteral("probe 720p"), url, qualities, 720, &host);
    // The dialog carries WA_DeleteOnClose, so the moment Esc closes it the
    // pointer is freed: every later look at the window goes through this guard.
    QPointer<PlayerDialog> dialogGuard(dialog);
    // Two dubs and two subtitle tracks, so both dock buttons have to appear.
    // A playlist, the way TitlePage::openPlayer hands one over. Without it
    // seriesButton_ stays hidden and the series drawer is never built, so the
    // whole "a populated view empties itself a beat later" class went untested.
    QVector<QVariantMap> playlist;
    for (int episode = 1; episode <= 5; ++episode) {
        const QString label = QStringLiteral("Серія %1").arg(episode);
        playlist.append(QVariantMap{{"label", label},
                                    {"embed", QStringLiteral("e%1").arg(episode)}});
    }
    dialog->setPlaylist(playlist, 2);
    // A viewer's list, not just distinct labels: the same voice variant opens
    // in several qualities, and the dock has to offer that picker.
    dialog->setVariants({QVariantMap{{"label", QStringLiteral("Ashdi 480p")},
                                     {"embed", QStringLiteral("e1")},
                                     {"kind", QStringLiteral("voice")}},
                         QVariantMap{{"label", QStringLiteral("Ashdi 720p")},
                                     {"embed", QStringLiteral("e1")},
                                     {"kind", QStringLiteral("voice")}},
                         QVariantMap{{"label", QStringLiteral("Ashdi 1080p")},
                                     {"embed", QStringLiteral("e1")},
                                     {"kind", QStringLiteral("voice")}},
                         QVariantMap{{"label", QStringLiteral("Цукіхана 720p")},
                                     {"embed", QStringLiteral("e2")},
                                     {"kind", QStringLiteral("voice")}},
                         QVariantMap{{"label", QStringLiteral("Суб 480p")},
                                     {"embed", QStringLiteral("e3")},
                                     {"kind", QStringLiteral("sub")}},
                         QVariantMap{{"label", QStringLiteral("Суб 1080p")},
                                     {"embed", QStringLiteral("e4")},
                                     {"kind", QStringLiteral("sub")}}},
                        QStringLiteral("e1"));
    dialog->startPlayer();
    // The real player opens fullscreen, and a fullscreen owner is a different
    // stacking case: a popover can sit behind it. Reproduce that, not a
    // normal window that happens to agree.
    dialog->showFullScreen();

    QPointer<PlayerShell> shell = dialog->findChild<PlayerShell*>();
    QPointer<VideoSurface> surface = dialog->findChild<VideoSurface*>();
    QPointer<QWidget> audio = dialog->findChild<QWidget*>(QStringLiteral("playerAudioPopup"));
    QPointer<QWidget> subs = dialog->findChild<QWidget*>(QStringLiteral("playerSubtitlesPopup"));
    QPointer<QWidget> strip = dialog->findChild<QWidget*>(QStringLiteral("playerFeatureStrip"));

    // One stage at a time, in the order the stages were designed for. The
    // guard ends the run if the player has been closed, and the finish prints
    // the verdict exactly once, however the run ended.
    PhaseRunner runner;
    runner.setGuard([&] { return !dialogGuard.isNull(); });
    runner.setFinish([&] {
        std::fprintf(stderr, failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL (%d)\n", failures);
        app.exit(failures == 0 ? 0 : 1);
    });

    runner.add(2500, [&] {
        if (surface == nullptr) {
            check(false, "surface exists");
            app.exit(1);
            return;
        }
        surface->seek(1.0);
        surface->seek(0.5);
        check(true, "two seeks survived (no crash)");
    });

    // Both dock buttons are there, and each lists only its own kind.
    runner.add(6000, [&] {
        if (surface != nullptr) {
            for (const QPointF& at : {QPointF(40, 40), QPointF(60, 60)}) {
                QMouseEvent move(QEvent::MouseMove, at, surface->mapToGlobal(at), Qt::NoButton,
                                 Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(surface, &move);
            }
        }
        // Both track pickers, found by the glyph each is painted with rather
        // than by a tooltip: a playlist with two of each kind makes both
        // appear, and that is the state under test.
        QPushButton* dub = buttonWithGlyph(dialog, IconPainter::microphone);
        QPushButton* subtitles = buttonWithGlyph(dialog, IconPainter::captions);
        check(dub != nullptr && dub->isVisible(), "the dub button is on the dock");
        check(subtitles != nullptr && subtitles->isVisible(),
              "the subtitle button is on the dock");
        // The window is already draggable and resizable from its edges, so a
        // second fullscreen control and a separate canvas reset would both be
        // a control that duplicates something the chrome already does.
        check(countButtonsWithGlyphs(dialog, {IconPainter::expand, IconPainter::contract}) == 1,
              "the window has one fullscreen control, not two");
        check(countButtonsWithGlyphs(dialog, {IconPainter::reset}) == 0,
              "there is no separate canvas-reset control");

        auto* volume = shell->findChild<VolumeGroup*>();
        auto* scrub = shell->findChild<ScrubBar*>();
        QPushButton* play = buttonWithGlyph(shell, IconPainter::pause);
        if (play == nullptr) play = buttonWithGlyph(shell, IconPainter::play);
        // "Expanded" is the shape of a slider and not of the icon that replaces
        // it; the span itself is the theme's to choose.
        check(volume != nullptr && volume->width() > volume->height(),
              "the volume slider is permanently expanded");
        // "Large" is the relationship the dock states: the primary control is
        // the biggest button on it. A number pinned here went red the day the
        // theme changed size, and the theme is free to change.
        int widestOther = 0;
        int tallestOther = 0;
        if (shell != nullptr) {
            for (QPushButton* other : shell->findChildren<QPushButton*>()) {
                if (other == play || other->isHidden()) continue;
                widestOther = std::max(widestOther, other->width());
                tallestOther = std::max(tallestOther, other->height());
            }
        }
        check(play != nullptr && play->width() > widestOther && play->height() > tallestOther,
              "the dock's primary play control is the largest control on it");
        check(play != nullptr && scrub != nullptr &&
                  play->mapTo(shell, play->rect().center()).y() <
                      scrub->mapTo(shell, scrub->rect().center()).y(),
              "the playback controls sit above the timeline");
    });

    runner.add(6200, [&] {
        auto* mainSurface = dialog->findChild<VideoSurface*>();
        auto* shell = dialog->findChild<PlayerShell*>();
        auto* skip = shell != nullptr
            ? shell->findChild<IconButton*>(QStringLiteral("playerSkipForward85")) : nullptr;
        check(skip != nullptr && skip->isVisible() && skip->isEnabled(),
              "the +85 control is visible and enabled for seekable media");
        if (mainSurface == nullptr || skip == nullptr || !mainSurface->hasVideo() ||
            mainSurface->duration() <= 89.0) {
            check(false, "the +85 transport scenario has a known duration over 89 seconds");
            return;
        }

        mainSurface->setPaused(true);
        mainSurface->seek(2.0);
        const bool parked = waitFor([&] {
            return mainSurface->pauseObserved() && !mainSurface->isPlaying() &&
                   std::abs(mainSurface->position() - 2.0) < 0.25;
        }, 5000);
        check(parked, "the +85 test begins paused at two seconds");
        if (!parked) return;
        skip->click();
        for (int i = 0; i < 60; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
            if (mainSurface->position() >= 86.0) break;
        }
        check(mainSurface->position() >= 86.0 && mainSurface->position() <= 89.5,
              "the +85 button seeks the active core from 2 to about 87 seconds");
        check(!mainSurface->isPlaying(), "the +85 seek preserves the paused state");

        mainSurface->seek(mainSurface->duration() - 2.0);
        for (int i = 0; i < 40; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
            if (mainSurface->position() >= mainSurface->duration() - 12.0) break;
        }
        skip->click();
        for (int i = 0; i < 40; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }
        check(mainSurface->position() <= mainSurface->duration() + 1.0,
              "the +85 seek is clamped to the known media end");
        mainSurface->seek(5.0);
        mainSurface->setPaused(false);
    });

    // The play control has to follow the engine, and it used to be frozen on
    // the glyph it was built with: the surface recorded a pause without ever
    // announcing it, so the state pump had nothing to report and the button
    // showed pause over a video that had stopped. Read at this point in the
    // sequence because the PiP section below asserts the pause state it finds.
    runner.add(6500, [&] {
        auto* mainSurface = dialog->findChild<VideoSurface*>();
        auto* play = dialog->findChild<IconButton*>(QStringLiteral("playerPlayToggle"));
        // The engine loads asynchronously, and this stage is about the stream
        // the button governs; wait for it rather than sampling a player that
        // has not started.
        if (mainSurface != nullptr) {
            waitFor([&] { return mainSurface->hasVideo(); }, 8000);
        }
        check(play != nullptr, "the dock has a play control");
        if (play == nullptr || mainSurface == nullptr || !mainSurface->hasVideo()) {
            check(play != nullptr && mainSurface != nullptr,
                  "a stream was loaded for the play-glyph check");
            return;
        }
        // The glyph is read off the button, not off a grab: play and pause are
        // painted by the same class at the same size.
        if (mainSurface->isPlaying()) {
            check(play->icon() == &IconPainter::pause,
                  "the play control shows pause while the video runs");
            play->click();
            QTest::qWait(700);
            check(!mainSurface->isPlaying(), "the control paused the engine");
            check(play->icon() == &IconPainter::play,
                  "the play control shows play while the video is paused");
        } else {
            check(play->icon() == &IconPainter::play,
                  "the play control shows play while the video is paused");
            play->click();
            QTest::qWait(700);
            check(mainSurface->isPlaying(), "the control resumed the engine");
            check(play->icon() == &IconPainter::pause,
                  "the play control shows pause while the video runs");
        }
        // Left exactly as found: the PiP ownership checks further down expect
        // to enter from a paused player.
        if (mainSurface->isPlaying()) {
            play->click();
            QTest::qWait(700);
        }
        check(!mainSurface->isPlaying(), "the player is left paused for the PiP section");
    });
    // An episode swap from a paused player is where the play control went
    // stale: the hand-back told the shell it was paused while the core had
    // already resumed, so the button showed "play" over a running video for
    // as long as the viewer looked at it. applyEpisode is the public path
    // that swaps in place and carries the pause across.
    runner.add(6300, [&] {
        auto* mainSurface = dialog->findChild<VideoSurface*>();
        auto* play = dialog->findChild<IconButton*>(QStringLiteral("playerPlayToggle"));
        // The engine loads asynchronously; wait for the stream instead of
        // sampling a player that has not started yet.
        if (mainSurface != nullptr) {
            waitFor([&] { return mainSurface->hasVideo(); }, 8000);
        }
        if (mainSurface == nullptr || play == nullptr || !mainSurface->hasVideo()) {
            std::fprintf(stderr,
                         "SKIP  the episode-swap check: no stream loaded in this session\n");
            return;
        }
        if (mainSurface->isPlaying()) {
            mainSurface->setPaused(true);
            QTest::qWait(500);
        }
        check(!mainSurface->isPlaying(), "the player is paused before the swap");
        check(play->icon() == &IconPainter::play, "the control reads play while paused");
        // Real variants carry a kind: the dock's two buttons filter on it, so a
        // fixture without one makes both pickers look broken.
        QVector<QVariantMap> swap;
        for (int episode = 1; episode <= 5; ++episode) {
            swap.append(QVariantMap{{"label", QStringLiteral("Серія %1").arg(episode)},
                                     {"embed", QStringLiteral("e%1").arg(episode)},
                                     {"kind", episode <= 3 ? QStringLiteral("voice")
                                                           : QStringLiteral("sub")}});
        }
        mainSurface->setPaused(false);
        QTest::qWait(900);
        check(mainSurface->position() > 2.0, "the player is past the start before the swap");
        const double beforeSwap = mainSurface->position();
        mainSurface->setPaused(true);
        QTest::qWait(400);
        dialog->applyEpisode(QStringLiteral("Серія 4"), QStringLiteral("e4"), 3, qualities, swap);
        for (int i = 0; i < 80; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
            if (!mainSurface->isPlaying() && play->icon() == &IconPainter::play) break;
        }
        check(!mainSurface->isPlaying(), "the swapped stream came back paused");
        check(play->icon() == &IconPainter::play,
              "an episode swap from a paused core leaves the control on play");
        // Wait for the swap to land. hasVideo() is no use as a signal here: it
        // stays true across a load, and VideoSurface keeps its cached position
        // until the pump reads a new time-pos - so sampling straight after the
        // call reads the position of the stream that was just replaced.
        for (int i = 0; i < 60; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
            if (mainSurface->position() < 2.0) break;
        }
        // A new episode starts at its beginning whatever the old one was doing,
        // and it keeps the pause state - a viewer who paused to step forward
        // wants to choose when the new one starts. Position is what the fix is
        // about, so that is what is asserted here.
        std::fprintf(stderr, "     position after swap: %.2fs (was %.2fs before)\n",
                     mainSurface->position(), beforeSwap);
        check(mainSurface->position() < 2.0,
              "a swapped episode starts at 00:00, not at the old offset");
        // Left paused, because the PiP ownership checks further down enter from
        // a paused player and assert that they return to it. A freshly loaded
        // core comes up playing, and the pause has to be re-asserted once the
        // load has actually landed - asking a core that has not started yet is
        // asking a core that will ignore the answer.
        for (int i = 0; i < 40; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
            if (mainSurface->duration() > 0.0) break;
        }
        mainSurface->setPaused(true);
        QTest::qWait(600);
        check(!mainSurface->isPlaying(), "the player is left paused for the PiP section");
    });
    // The gear carries quality, using mikai's own values.
    runner.add(7000, [&] {
        if (surface != nullptr) {
            for (const QPointF& at : {QPointF(40, 40), QPointF(60, 60)}) {
                QMouseEvent move(QEvent::MouseMove, at, surface->mapToGlobal(at), Qt::NoButton,
                                 Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(surface, &move);
            }
        }
    });

    runner.add(7500, [&] {
        // The engine loads asynchronously, and every height below is proved by
        // what happens to the stream; wait for it rather than racing the load.
        if (surface != nullptr) {
            waitFor([&] { return surface->hasVideo(); }, 8000);
        }
        if (shell != nullptr) shell->openSettings();
        auto* flyout = dialog->findChild<SettingsFlyout*>();
        check(flyout != nullptr && flyout->isVisible(), "the gear opened its popover");
        if (flyout == nullptr) return;
        check(!flyout->isWindow() && flyout->parentWidget() == dialog,
              "the gear popover uses the fullscreen overlay plane");
        // Every height the host offered has to be reachable, and the popover is
        // the only place a viewer reaches it. Each row is proved by what
        // pressing it does - it reloads the stream at that height - so this
        // holds whatever the rows end up saying.
        const QVector<int> offeredHeights{480, 720, 1080};
        for (const int height : offeredHeights) {
            // Play on past a second first, so a fall in position can only mean
            // the file restarted. A reload from 0.2s would look the same as a
            // stream that never moved.
            surface->setPaused(false);
            for (int waited = 0; waited < 200 && surface->position() < 1.0; ++waited) {
                QCoreApplication::processEvents();
                QThread::msleep(50);
            }
            const double before = surface->position();
            // Only the row that is on screen: setQualityOptions() rebuilds the
            // list, and the retired rows stay children until they are deleted,
            // so a search that ignores visibility can press a row the popover
            // has already replaced.
            QPushButton* qualityRow = nullptr;
            for (QPushButton* button : flyout->findChildren<QPushButton*>()) {
                if (button->isVisible() &&
                    button->text().contains(QStringLiteral("%1p").arg(height))) {
                    qualityRow = button;
                    break;
                }
            }
            check(qualityRow != nullptr,
                  (QStringLiteral("the popover offers the %1p stream").arg(height))
                      .toUtf8()
                      .constData());
            if (qualityRow == nullptr) continue;
            qualityRow->click();
            // The height the player says it is running is what the press is
            // about, and it is set on the click itself - a reload is only how
            // it gets there.
            if (dialog->currentHeight() != height) {
                int visibleRows = 0;
                for (QPushButton* b : flyout->findChildren<QPushButton*>()) {
                    if (b->isVisible()) visibleRows++;
                }
                std::fprintf(stderr,
                             "     height after pressing %dp: %dp (flyout visible=%d rows=%d)\n",
                             height, dialog->currentHeight(), int(flyout->isVisible()),
                             visibleRows);
            }
            check(dialog->currentHeight() == height,
                  (QStringLiteral("picking %1p makes it the running stream").arg(height))
                      .toUtf8()
                      .constData());
            // A reload restarts the file, so the position falls back to the
            // beginning: that is the second externally visible thing a quality
            // switch does, and it says the press reached the stream rather
            // than only closing a popover.
            for (int waited = 0; waited < 80; ++waited) {
                QCoreApplication::processEvents();
                QThread::msleep(50);
                if (surface->position() < before) break;
            }
            check(surface->position() < before,
                  (QStringLiteral("picking %1p reloads the stream at that height").arg(height))
                      .toUtf8()
                      .constData());
            shell->openSettings();
            QTest::qWait(150);
        }
        check(buttonsFitWithoutOverlap(flyout), "the settings rows fit without overlap");
        auto* infoChip = shell->findChild<QWidget*>(QStringLiteral("playerInfoChip"));
        auto* networkStats =
            shell->findChild<QLabel*>(QStringLiteral("playerNetworkStats"));
        check(infoChip != nullptr && infoChip->isVisible() && shell != nullptr &&
                  infoChip->x() < shell->width() / 2 && infoChip->y() < shell->height() / 2,
              "the MIKAI status chip sits in the top-left of the player");
        check(networkStats != nullptr && !networkStats->isHidden() &&
                  !networkStats->text().isEmpty(),
              "the status chip is on screen with a readout in it");
        flyout->grab().toImage().save(QStringLiteral("flyout_probe.png"));
        auto* series = dialog->findChild<QPushButton*>(QStringLiteral("playerSeriesToggle"));
        check(series != nullptr && series->isVisible(),
              "the series toggle is still on the strip");
        // Keep a full-screen artifact: this caught the old native popup
        // rendering correctly in isolation while disappearing from the display.
        QTest::qWait(100);
        QApplication::processEvents();
        dialog->grab().save(QStringLiteral("dialog_flyout.png"));
        if (auto* screen = QGuiApplication::primaryScreen()) {
            screen->grabWindow(0).save(QStringLiteral("screen_flyout.png"));
        }
    });

    // Anime4K is only real if libmpv actually runs the chain, and mpv's own
    // glsl-shaders property cannot be used to find that out: it hands back the
    // string it was given without validating it, so a preset naming a deleted
    // file still reads back whole - confirmed by deleting one shader and
    // watching the comparison pass anyway. The proof has to be the pixels.
    runner.add(8000, [&] {
        const QString shadersDir = anime4k::findShadersDir();
        check(!shadersDir.isEmpty(), "the shader folder sits beside the exe");
        check(anime4k::shaderStringForCandidate(QStringLiteral("off"), shadersDir).isEmpty(),
              "Off resolves to no chain at all");
        // Every file a preset references has to exist on disk. Deleting one is
        // what proves this can fail: the presets naming it drop out and the
        // suite says exactly which.
        int missingPresets = 0;
        int checkedPresets = 0;
        for (const anime4k::ModeInfo& info : anime4k::availableModes()) {
            if (info.mode == anime4k::Mode::Off) continue;
            ++checkedPresets;
            if (!anime4k::chainIsComplete(anime4k::candidateForId(info.id, shadersDir),
                                          shadersDir)) {
                std::fprintf(stderr, "FAIL preset %s is missing a file\n", qPrintable(info.id));
                ++missingPresets;
            }
        }
        std::fprintf(stderr, "     presets checked: %d\n", checkedPresets);
        check(missingPresets == 0,
              "every preset resolves to files that exist");

        QPushButton* enhancementsButton = buttonWithGlyph(shell, IconPainter::spark);
        check(enhancementsButton != nullptr && enhancementsButton->isVisible(),
              "the enhancements button is on the dock");
        if (enhancementsButton == nullptr) return;
        enhancementsButton->click();

        auto* enhancements =
            dialog->findChild<MenuPopup*>(QStringLiteral("playerEnhancementsPopup"));
        check(enhancements != nullptr && enhancements->isVisible(),
              "the enhancements menu opened");
        check(enhancements != nullptr && !enhancements->isWindow() &&
                  enhancements->parentWidget() == dialog,
              "the enhancements menu uses the fullscreen overlay plane");
        // Official AnimeJaNai lives beside Anime4K and exposes its supported
        // spatial and temporal choices through its own typed menu payloads.
        QPushButton* animeJanaiEntry =
            pressableRowFor(enhancements, QStringLiteral("animejanai"));
        check(animeJanaiEntry != nullptr && animeJanaiEntry->isEnabled(),
              "the enhancements root offers selectable official AnimeJaNai");
        if (animeJanaiEntry == nullptr) {
            shell->dismissOverlay();
            return;
        }
        animeJanaiEntry->click();
        QTest::qWait(60);
        MenuPopup* animeJanaiPage =
            levelPanelShowing(dialog, QStringLiteral("animejanai"));
        check(animeJanaiPage != nullptr && animeJanaiPage != enhancements,
              "the official AnimeJaNai choices open beside the root");
        check(enhancements->isVisible(),
              "the enhancement root stays visible beside AnimeJaNai");
        check(pressableRowFor(animeJanaiPage, QStringLiteral("animejanai:spatial")) != nullptr,
              "official spatial choices have a real submenu");
        check(pressableRowFor(animeJanaiPage, QStringLiteral("animejanai:rife")) != nullptr,
              "official RIFE choices have a real submenu");
        QPushButton* anime4kEntry =
            pressableRowFor(enhancements, QStringLiteral("anime4k"));
        check(anime4kEntry != nullptr, "the enhancements menu offers Anime4K");
        if (anime4kEntry == nullptr) {
            shell->dismissOverlay();
            return;
        }
        anime4kEntry->click();
        QTest::qWait(60);

        MenuPopup* anime4kPopup = levelPanelShowing(dialog, QStringLiteral("anime4k"));
        check(anime4kPopup != nullptr && anime4kPopup->isVisible(),
              "the Anime4K level opened beside the root");

        // The presets hang under categories, so the row is two clicks in - and
        // the category itself must not change the chain on the way.
        const anime4k::Mode beforeCategory = surface->anime4KMode();
        QPushButton* speedRow =
            pressableRowFor(anime4kPopup, QStringLiteral("anime4k/speed"));
        check(speedRow != nullptr, "the Anime4K level offers its speed category");
        if (speedRow == nullptr) {
            shell->dismissOverlay();
            return;
        }
        speedRow->click();
        QTest::qWait(60);
        check(surface->anime4KMode() == beforeCategory,
              "opening a category does not change the applied chain");
        MenuPopup* speedPanel = levelPanelShowing(dialog, QStringLiteral("anime4k/speed"));
        check(speedPanel != nullptr && speedPanel != anime4kPopup,
              "a category opens the level beside the one it sits in");
        check(anime4kPopup->isVisible(),
              "the level a category was opened from stays on screen");

        QPushButton* row = pressableRowFor(speedPanel, anime4k::modeId(kAnime4KProbeMode));
        check(row != nullptr, "the speed category lists its presets");
        if (row == nullptr) {
            shell->dismissOverlay();
            return;
        }

        // A framebuffer only exists once the window has been exposed, which
        // does not happen in a headless session. That is not a defect, so the
        // pixel comparison is skipped rather than failed - and skipping says
        // so, because a probe that silently reports PASS while proving
        // nothing is worse than one that admits it could not run.
        if (!surface->waitForContext(4000)) {
            std::fprintf(stderr, "SKIP  the pixel comparison: no render context in this session\n");
            shell->dismissOverlay();
            return;
        }

        // Freeze once. Everything after this reads the same source position,
        // so the only variable left is the shader chain.
        freezeOn(surface);

        // The noise floor: two settled frames with nothing touched between
        // them. mpv re-renders on exposure, so this is not zero - and a
        // before/after comparison only means something against it.
        const QImage noiseA = settledFrame(surface);
        const QImage noiseB = settledFrame(surface);
        const double noise = pictureDifference(noiseA, noiseB);
        std::fprintf(stderr, "     control: two frames, nothing changed  %.3f / 255\n", noise);
        if (noise < 0.0) {
            std::fprintf(stderr, "FAIL  the pixel comparison: %s\n",
                         qPrintable(comparisonRefusal(noiseA, noiseB)));
        }
        check(noise >= 0.0, "the two control frames are comparable");

        const QImage plainFrame = settledFrame(surface);
        row->click();
        check(surface->anime4KMode() == kAnime4KProbeMode, "the press reached the surface");
        // vo=libmpv rebuilds the chain on the next rendered frame and the
        // shaders compile on load, so this is not instant.
        QTest::qWait(3000);

        const QImage chainFrame = settledFrame(surface);
        if (plainFrame.isNull() || chainFrame.isNull() || noise < 0.0) {
            check(false, "the frozen shader comparison has valid settled frames");
            shell->dismissOverlay();
            return;
        }
        check(noise == 0.0, "the frozen frame is bit-stable between grabs");
        const QString refusal = comparisonRefusal(plainFrame, chainFrame);
        if (!refusal.isEmpty()) {
            std::fprintf(stderr, "FAIL  the chain comparison: %s\n", qPrintable(refusal));
            check(false, "the frozen shader comparison measures the same picture geometry");
        } else {
            const double delta = pictureDifference(plainFrame, chainFrame);
            const QRect band = pictureArea(plainFrame, 1280, 720);
            std::fprintf(stderr,
                         "     chain on vs off %.3f / 255 over a %dx%d band of %dx%d, "
                         "against %.3f noise\n",
                         delta, band.width(), band.height(), plainFrame.width(),
                         plainFrame.height(), noise);
            check(delta > 0.1, "enabling the preset changed the picture");
        }

        // The other half of the tree's contract, on a fresh open and with a
        // chain actually running now: a preset row applies its preset and takes
        // the whole tree off the screen. Official AnimeJaNai remains a separate
        // selectable branch and does not replace or turn off the shader chain.
        //
        // Every row is taken from the panel as it stands at that moment, and
        // only once that panel is really on screen: each level is rebuilt the
        // moment its row is pressed, so a row kept from the step before is one
        // the rebuild has already retired. The panels are children of the
        // player's dialog and the probe's later stages share this loop - the PiP
        // stage hides that dialog and the shell re-runs its state pass on the
        // way back - so the window is waited for first, and then every row.
        // Bounded wait for a level to come up, so a row is never read out of a
        // panel the shell has not finished placing.
        const auto awaitLevel = [dialog](const QString& levelId) -> MenuPopup* {
            for (int waited = 0; waited < 4000; waited += 40) {
                QCoreApplication::processEvents();
                if (MenuPopup* panel = levelPanelShowing(dialog, levelId)) return panel;
                QThread::msleep(40);
            }
            return nullptr;
        };
        playerWindowIsBack(dialog);
        enhancementsButton->click();
        anime4kEntry = pressableRowFor(awaitLevel(QString()), QStringLiteral("anime4k"));
        check(anime4kEntry != nullptr, "the tree reopens at its root after the pixel stage");
        if (anime4kEntry == nullptr) {
            shell->dismissOverlay();
            return;
        }
        anime4kEntry->click();
        QPushButton* reopen = pressableRowFor(awaitLevel(QStringLiteral("anime4k")),
                                              QStringLiteral("anime4k/speed"));
        check(reopen != nullptr, "the reopened Anime4K level still leads to its speed category");
        if (reopen != nullptr) {
            reopen->click();
            MenuPopup* reopenedSpeed = awaitLevel(QStringLiteral("anime4k/speed"));
            // The contract is that a reopened category lists pressable presets
            // and that each row applies the preset its own payload names. Which
            // preset sits where is the catalogue's business: a probe that pins
            // one id goes red the day the catalogue is reordered, while saying
            // nothing about whether the rows still work.
            const anime4k::Mode running = surface->anime4KMode();
            QString chosenId;
            for (int spot = 0; reopenedSpeed != nullptr && spot < reopenedSpeed->rowCount();
                 ++spot) {
                const MenuEntry entry = reopenedSpeed->entryAt(spot);
                const QString payload = entry.payload.toString();
                if (!payload.startsWith(QLatin1String("preset|"))) continue;
                if (entry.disabled) {
                    check(false, "a reopened speed preset row is one the viewer can press");
                    continue;
                }
                const QString id = payload.mid(7);
                if (id == anime4k::modeId(running)) continue;
                if (auto* presetRow = qobject_cast<QPushButton*>(reopenedSpeed->rowWidget(spot))) {
                    chosenId = id;
                    presetRow->click();
                    QTest::qWait(80);
                    break;
                }
            }
            check(!chosenId.isEmpty(),
                  "the reopened speed level lists a preset that is not the running one");
            if (!chosenId.isEmpty()) {
                check(surface->anime4KMode() == anime4k::modeForId(chosenId) &&
                          surface->anime4KMode() != running,
                      "a preset row applies the preset it names, and not the one already running");
                // Sampled straight, not through the wait: a level that has gone
                // must not make the probe sit out its whole timeout.
                check(levelPanelShowing(dialog, QStringLiteral("anime4k")) == nullptr &&
                          levelPanelShowing(dialog, QStringLiteral("anime4k/speed")) == nullptr,
                      "choosing a preset takes the whole tree off the screen");
            }
        }
        shell->dismissOverlay();
        enhancementsButton->click();
        MenuPopup* publicRoot = awaitLevel(QString());
        const anime4k::Mode beforeAnimeJanai = surface->anime4KMode();
        QPushButton* animeJanaiRoot =
            pressableRowFor(publicRoot, QStringLiteral("animejanai"));
        check(animeJanaiRoot != nullptr && animeJanaiRoot->isEnabled(),
              "official AnimeJaNai remains a selectable public branch");
        if (animeJanaiRoot != nullptr) animeJanaiRoot->click();
        MenuPopup* animeJanaiChoices = awaitLevel(QStringLiteral("animejanai"));
        check(animeJanaiChoices != nullptr &&
                  pressableRowFor(animeJanaiChoices, QStringLiteral("animejanai:spatial")) != nullptr &&
                  pressableRowFor(animeJanaiChoices, QStringLiteral("animejanai:rife")) != nullptr,
              "the official spatial and RIFE choices remain on their own public menu");
        check(surface->anime4KMode() == beforeAnimeJanai,
              "opening official AnimeJaNai choices leaves the running Anime4K chain unchanged");
        shell->dismissOverlay();
    });

    // And the pickers still work, split by kind.
    runner.add(9000, [&] {
        if (shell != nullptr) shell->openSettings();
        // Use a real press and release so this follows the same button path a
        // viewer uses.
        if (QPushButton* dubButton = buttonWithGlyph(dialog, IconPainter::microphone)) {
            const QPointF at(12, 12);
            const QPointF g = dubButton->mapToGlobal(dubButton->rect().center());
            QMouseEvent press(QEvent::MouseButtonPress, at, g, Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
            QApplication::sendEvent(dubButton, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, at, g, Qt::LeftButton, Qt::NoButton,
                                Qt::NoModifier);
            QApplication::sendEvent(dubButton, &release);
        }
        check(audio != nullptr && audio->isVisible(), "the dub picker opened");
        if (audio != nullptr) {
            check(!audio->isWindow() && audio->parentWidget() == dialog,
                  "the dub picker uses the fullscreen overlay plane");
            check(buttonsFitWithoutOverlap(audio), "the dub rows fit without overlap");
        }
        if (strip != nullptr) {
            std::fprintf(stderr, "     strip: visible=%d size=%dx%d at %d,%d\n",
                         int(strip->isVisible()), strip->width(), strip->height(), strip->x(),
                         strip->y());
        }
        if (audio != nullptr) check(audio->findChildren<QPushButton*>().size() > 0, "dub rows exist");
    });

    runner.add(10500, [&] {
        if (QPushButton* track = buttonWithGlyph(dialog, IconPainter::captions)) {
            track->click();
        }
        check(subs != nullptr && subs->isVisible(), "the subtitle picker opened");
        if (subs != nullptr) {
            check(!subs->isWindow() && subs->parentWidget() == dialog,
                  "the subtitle picker uses the fullscreen overlay plane");
            check(buttonsFitWithoutOverlap(subs), "the subtitle rows fit without overlap");
        }
    });

    runner.add(11000, [&] {
        if (shell == nullptr) return;
        shell->dismissOverlay();

        check(buttonWithGlyph(shell, IconPainter::keyboard) == nullptr,
              "the footer keyboard button is gone");
        check(buttonWithGlyph(shell, IconPainter::more) == nullptr,
              "the footer ellipsis button is gone");

        QPoint picture;
        if (surface != nullptr) {
            for (const QPoint& candidate :
                 {QPoint(surface->width() / 4, surface->height() / 2),
                  QPoint(surface->width() / 2, surface->height() / 3), QPoint(60, 60)}) {
                if (!surface->rect().contains(candidate)) continue;
                if (shell->childAt(candidate) == surface) {
                    picture = candidate;
                    break;
                }
            }
        }
        check(!picture.isNull(), "the right-click has a spot on the picture to land on");
        if (picture.isNull()) return;
        auto* hints = dialog->findChild<KeyHints*>(QStringLiteral("playerKeyHints"));
        QTest::mouseClick(surface, Qt::RightButton, Qt::NoModifier, picture);
        auto* menu = dialog->findChild<MenuPopup*>(QStringLiteral("playerPictureMenu"));
        waitFor([&menu] { return menu != nullptr && menu->isVisible(); }, 2000);
        check(menu != nullptr && menu->isVisible(),
              "a right-click on the picture opens the picture menu");
        if (menu == nullptr) {
            shell->dismissOverlay();
            return;
        }
        check(!menu->isWindow() && menu->parentWidget() == dialog,
              "the picture menu uses the fullscreen overlay plane");
        check(buttonsFitWithoutOverlap(menu), "the picture menu rows fit without overlap");

        QPushButton* keysRow = nullptr;
        for (int spot = 0; spot < menu->rowCount() && keysRow == nullptr; ++spot) {
            if (menu->entryAt(spot).payload.toString() != QLatin1String("keys")) continue;
            keysRow = qobject_cast<QPushButton*>(menu->rowWidget(spot));
        }
        check(keysRow != nullptr && keysRow->isEnabled(),
              "the picture menu carries the keyboard shortcuts action");
        if (keysRow == nullptr) {
            shell->dismissOverlay();
            return;
        }
        {
            const QPointF at = keysRow->rect().center();
            const QPointF global = keysRow->mapToGlobal(at.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, at, global, Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
            QApplication::sendEvent(keysRow, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, at, global, Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(keysRow, &release);
        }
        check(hints != nullptr && hints->isVisible(),
              "the picture menu's keyboard row opens the hints");
        if (hints == nullptr) {
            shell->dismissOverlay();
            return;
        }
        check(!hints->isWindow() && hints->parentWidget() == dialog,
              "the keyboard hints use the fullscreen overlay plane");
        QKeyEvent escapePress(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(hints, &escapePress);
        check(!hints->isVisible(), "Esc closes the hints the menu opened");
        QKeyEvent questionPress(QEvent::KeyPress, Qt::Key_Question, Qt::NoModifier);
        QCoreApplication::sendEvent(shell, &questionPress);
        check(hints->isVisible(), "the ? shortcut still opens the hints");
        shell->dismissOverlay();

        QTemporaryDir folderFixture;
        if (!folderFixture.isValid()) {
            check(false, "the folder fixture directory was created");
            return;
        }
        const QString clip = folderFixture.filePath(QStringLiteral("probe clip #1.mp4"));
        QFile seed(clip);
        if (!seed.open(QIODevice::WriteOnly) || seed.write("0", 1) != 1) {
            check(false, "the fixture clip was written");
            return;
        }
        seed.close();
        auto folderRowOffered = [](PlayerShell* shell) {
            if (shell == nullptr) return false;
            auto* panel = shell->findChild<MenuPopup*>(QStringLiteral("playerPictureMenu"));
            if (panel == nullptr) return false;
            for (int spot = 0; spot < panel->rowCount(); ++spot) {
                if (panel->entryAt(spot).payload.toString() == QLatin1String("folder")) {
                    return true;
                }
            }
            return false;
        };
        QPointer<PlayerShell> isolated(new PlayerShell(nullptr, /*frameless=*/false));
        isolated->setMedia(clip);
        check(folderRowOffered(isolated.data()), "a local file offers its containing folder");
        isolated->setMedia(QStringLiteral("https://example.invalid/video.mkv"));
        check(!folderRowOffered(isolated.data()), "a streamed source offers no folder");
        delete isolated.data();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    });

    runner.add(11500, [&] {
        if (shell != nullptr) shell->dismissOverlay();
        if (surface != nullptr) {
            QTest::mouseMove(surface, QPoint(40, 40));
            QTest::mouseMove(surface, QPoint(60, 60));
        }
        QApplication::processEvents();

        QWidget* original = dialog->findChild<QWidget*>(QStringLiteral("originalFileButton"));
        check(original != nullptr, "the original-file button exists");
        if (original == nullptr) {
            return;
        }
        const QPoint at = original->mapTo(dialog, QPoint(0, 0));
        check(at.x() > dialog->width() / 2 && at.y() < dialog->height() / 2,
              "the original-file button is in the top-right corner");
        check(original->isVisible(), "the original-file button is visible while the chrome is up");

        check(waitFor([&] { return !original->isVisible(); }, 4000),
              "the window controls auto-hide once the pointer goes idle");
    });

    // PiP: open the float, check the spec'd layout, then prove the auto-hide
    // stands down while the pointer is on it.
    runner.add(14600, [&] {
        if (dialogGuard.isNull()) return;
        // Pointer activity first: the chrome auto-hides after a couple of idle
        // seconds, and the dock button the stage presses lives on it.
        if (surface != nullptr) {
            for (const QPointF& at : {QPointF(40, 40), QPointF(60, 60)}) {
                QMouseEvent move(QEvent::MouseMove, at, surface->mapToGlobal(at), Qt::NoButton,
                                 Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(surface, &move);
            }
        }
        if (QPushButton* pipButton = buttonWithGlyph(dialog, IconPainter::pip)) {
            pipButton->click();
        }
        // The float builds its own core a beat after it is shown, so the wait
        // is on the float actually being on screen, not on a guessed latency.
        PipFloat* pip = awaitVisiblePipFloat(6000);
        check(pip != nullptr, "the PiP float opened");
        if (pip == nullptr) {
            return;
        }
        pipFloat = pip;
        // The float lays its chrome out when it is first shown, so give the
        // show a beat before reading geometry off it.
        QTest::qWait(400);
        {
            auto child = [pip](const char* name) -> QWidget* {
                return pip->findChild<QWidget*>(QString::fromLatin1(name));
            };
            QWidget* mute = child("pipMute");
            QWidget* restore = child("pipRestore");
            QWidget* close = child("pipClose");
            QWidget* play = child("pipPlay");
            check(mute != nullptr && mute->isVisible(), "PiP mute is visible");
            check(restore != nullptr && restore->isVisible(), "PiP restore is visible");
            check(close != nullptr && close->isVisible(), "PiP close is visible");
            check(play != nullptr && play->isVisible(), "PiP play button is visible");
            // One fixed pill, always the same size. It used to grow on hover,
            // which resized the float's content under the pointer on every
            // enter and leave - the lag. A constant width is the fix.
            QWidget* volume = child("pipVolume");
            check(volume != nullptr && volume->isVisible(),
                  "the float shows a volume level");
            check(mute != nullptr && volume != nullptr && mute->parentWidget() != nullptr
                      && volume->parentWidget() == mute->parentWidget(),
                  "the level and the mute button share one chip");
            if (volume != nullptr && mute != nullptr) {
                const int pillWidth = volume->parentWidget()->width();
                // The pill must be as wide as its two children plus the gap,
                // and no wider: a strip stretched to the window is what made
                // the control look lost in the corner.
                check(pillWidth >= mute->width() + volume->width(),
                      "the chip is wide enough for the button and the level");
                // Containment, in one coordinate space: the button must lie
                // inside the pill's rect. Comparing two mapTo results across
                // different parents is how the earlier version of this check
                // reported a failure that was never there.
                const QWidget* pill = volume->parentWidget();
                const QRect pillRect = pill->geometry();
                const QRect buttonRect =
                    QRect(mute->mapTo(pill, QPoint(0, 0)), mute->size());
                std::fprintf(stderr,
                             "     pill=%d,%d %dx%d  button=%d,%d %dx%d\n",
                             pillRect.x(), pillRect.y(), pillRect.width(), pillRect.height(),
                             buttonRect.x(), buttonRect.y(), buttonRect.width(),
                             buttonRect.height());
                // buttonRect is already relative to the pill, so containment is a
                // plain size comparison - not a position one.
                check(buttonRect.left() >= 0 && buttonRect.top() >= 0 &&
                          buttonRect.right() < pillRect.width() &&
                          buttonRect.bottom() < pillRect.height(),
                      "the mute button sits inside the pill, not beside it");
                check(volume->toolTip().isEmpty(),
                      "the level has no tooltip to chase the pointer");
            }
            if (mute != nullptr && restore != nullptr) {
                const QPoint m = mute->mapTo(pip, QPoint(0, 0));
                const QPoint r = restore->mapTo(pip, QPoint(0, 0));
                check(m.x() < r.x(), "PiP mute sits left of restore");
                check(m.y() < pip->height() / 2, "PiP mute sits near the top");
            }
            if (play != nullptr) {
                const QPoint c = play->mapTo(pip, QPoint(0, 0));
                check(std::abs(c.x() + play->width() / 2 - pip->width() / 2) < 4,
                      "PiP play button is horizontally centred");
                check(std::abs(c.y() + play->height() / 2 - pip->height() / 2) < 4,
                      "PiP play button is vertically centred");
                // "Large" is a relationship inside the float, not a number
                // copied out of the theme: the centre control is bigger than
                // the corner chrome whatever size the chrome has.
                int cornerSize = 0;
                for (QWidget* corner : {mute, restore, close}) {
                    if (corner != nullptr) cornerSize = std::max(cornerSize, corner->width());
                }
                check(play->width() > cornerSize && play->height() > cornerSize,
                      "PiP play button is the largest control on the float");
            }
            QPointer<QWidget> playGuard(play);
            const auto controlFrame = [&](bool videoOnly) {
                if (pipFloat.isNull() || playGuard.isNull()) return QImage();
                VideoSurface* surface = pipFloat->videoSurface();
                if (surface == nullptr) return QImage();
                const QImage frame = videoOnly ? surface->grabVideoFramebuffer()
                                               : surface->grabFramebuffer();
                QRect logical(surface->mapFromGlobal(playGuard->mapToGlobal(QPoint())),
                              playGuard->size());
                logical.adjust(-16, -16, 16, 16);
                const qreal ratio = surface->devicePixelRatioF();
                return frame.copy(QRect(logical.topLeft() * ratio, logical.size() * ratio)
                                      .intersected(frame.rect()));
            };
            pip->videoSurface()->setPaused(true);
            check(waitFor([&] {
                      return pip->videoSurface()->pauseObserved() &&
                             !pip->videoSurface()->isPlaying();
                  }, 3000),
                  "PiP acknowledges pause before the visual hover check");
            QTest::qWait(600);
            const QImage videoOnly = controlFrame(true);
            QTest::mouseMove(pip, QPoint(pip->width() / 4, pip->height() / 3));
            check(waitFor([&] {
                      return pip->rect().contains(pip->mapFromGlobal(QCursor::pos()));
                  }, 1000),
                  "the actual pointer entered the float");
            QTest::qWait(400);
            const QImage hovered = controlFrame(false);
            const double visibleDifference = pictureDifference(videoOnly, hovered);
            check(visibleDifference > 5.0,
                  "the hovered PiP play control is visibly painted over the paused video");
            QTest::qWait(2400);
            const QImage hoverHeld = controlFrame(false);
            const double heldDifference = pictureDifference(hovered, hoverHeld);
            std::fprintf(stderr, "     PiP held difference=%.3f paused=%d position=%.3f\n",
                         heldDifference, int(pip->videoSurface()->pauseObserved()),
                         pip->videoSurface()->position());
            if (heldDifference >= 1.0) {
                hovered.save(QStringLiteral("pip-hover-before.png"));
                hoverHeld.save(QStringLiteral("pip-hover-after.png"));
            }
            check(heldDifference >= 0.0 && heldDifference < 1.0,
                  "PiP controls stay visibly unchanged while the pointer is on the float");

            QTest::mouseMove(pip, QPoint(-32, pip->height() / 2));
            check(waitFor([&] {
                      return !pip->rect().contains(pip->mapFromGlobal(QCursor::pos()));
                  }, 1000),
                  "the actual pointer left the float");
            const bool fadedOut = waitFor([&] {
                const double difference = pictureDifference(videoOnly, controlFrame(false));
                return difference >= 0.0 && difference < 1.0;
            }, 4000);
            check(fadedOut,
                  "PiP controls fade back to the unobstructed paused video after the pointer leaves");
            if (!fadedOut) {
                const QImage afterLeave = controlFrame(false);
                std::fprintf(stderr, "     PiP leave difference=%.3f pointer=%d,%d\n",
                             pictureDifference(videoOnly, afterLeave),
                             pip->mapFromGlobal(QCursor::pos()).x(),
                             pip->mapFromGlobal(QCursor::pos()).y());
                videoOnly.save(QStringLiteral("pip-leave-baseline.png"));
                afterLeave.save(QStringLiteral("pip-leave-after.png"));
            }
        }
    });

    // THE REGRESSION CLASS. Every other check in this file asserts a view
    // right after it was built, which is why "it renders, then a beat later it
    // is empty" survived: nothing here ever looked twice. A populated view is
    // opened, the event loop is allowed to turn for 1.5s, and the same view is
    // asserted again. Any timer, resize pass, state handler or async callback
    // that empties a populated widget shows up here and nowhere else.
    runner.add(28000, [&] {
        // The PiP stage closed the float and the dialog reports itself visible
        // again at that point, but it is not reliably still visible 100ms later,
        // so no assertion is made about it here: the checks below are about the
        // controls, and they make their own state explicit. Only restore it if
        // it really is down, so a hidden window cannot be mistaken for a broken
        // control.
        if (!dialog->isVisible()) {
            dialog->show();
            QApplication::processEvents();
        }
        for (QWidget* w : dialog->findChildren<QWidget*>()) {
            if (QString::fromLatin1(w->metaObject()->className())
                    .contains(QStringLiteral("VideoSurface"))) {
                for (const QPointF& at : {QPointF(40, 40), QPointF(60, 60)}) {
                    QMouseEvent move(QEvent::MouseMove, at, w->mapToGlobal(at), Qt::NoButton,
                                     Qt::NoButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(w, &move);
                }
            }
        }
        QApplication::processEvents();
        QPushButton* series = dialog->findChild<QPushButton*>(QStringLiteral("playerSeriesToggle"));
        // isHidden() is this control's own state. isVisible() would also be
        // false whenever the strip's idle timer has hidden the chrome, which
        // says nothing about whether the toggle is there or enabled.
        check(series != nullptr && !series->isHidden() && series->isEnabled(),
              "the series toggle is on the strip with a playlist loaded");

        auto* drawer = dialog->findChild<QWidget*>(QStringLiteral("playerSeriesDrawer"));
        // Click only to OPEN. The stage runs after earlier stages that may have
        // left the drawer expanded, and clicking an expanded drawer collapses
        // it - so a blind click tested the toggle, not the drawer.
        if (drawer != nullptr && !drawer->isVisible() && series != nullptr) {
            series->click();
            QApplication::processEvents();
        }

        check(drawer != nullptr && drawer->isVisible(), "the series drawer opened");
        const int before =
            drawer != nullptr ? drawer->findChildren<QPushButton*>(
                                   QStringLiteral("playerSeriesItemCurrent")).size() +
                                   drawer->findChildren<QPushButton*>(QStringLiteral("playerSeriesItem"))
                                       .size()
                             : 0;
        check(before == 5, "the drawer lists every episode of the playlist");

        // Let the loop turn. Shell state, hide timers, layout passes and any
        // queued callback all get their chance to run against a populated view.
        QTest::qWait(1500);
        QApplication::processEvents();

        const int after =
            drawer != nullptr ? drawer->findChildren<QPushButton*>(
                                   QStringLiteral("playerSeriesItemCurrent")).size() +
                                   drawer->findChildren<QPushButton*>(QStringLiteral("playerSeriesItem"))
                                       .size()
                             : 0;
        check(drawer != nullptr && drawer->isVisible(),
              "the series drawer is still open 1.5s after it opened");
        check(after == before && after == 5,
              "the series drawer still lists every episode 1.5s later");
        check(series != nullptr && series->isVisible(),
              "the series toggle survived the drawer's own refresh");
    });

    runner.add(21000, [&] {
        if (pipFloat == nullptr || !pipFloat->isVisible()) {
            check(false, "the float is up for the placement checks");
            return;
        }
        QScreen* under = QGuiApplication::screenAt(pipFloat->frameGeometry().center());
        check(under != nullptr, "the float sits on a screen it can be clicked on");
        if (under == nullptr) return;
        const QRect work = under->availableGeometry();
        const QSize room = work.size() - pipFloat->size();
        check(room.width() > 0 && room.height() > 0,
              "the work area has room for two distinct noncorner placements");
        if (room.width() <= 0 || room.height() <= 0) return;
        const QPoint first = work.topLeft() + QPoint(room.width() / 3, room.height() / 3);
        const QPoint second =
            work.topLeft() + QPoint(room.width() * 2 / 3, room.height() / 2);
        dragPipTo(pipFloat, first,
                  "the first arbitrary drop stays at the exact released position after settling");
        if (pipFloat.isNull()) return;
        dragPipTo(pipFloat, second,
                  "the second distinct arbitrary drop stays exactly where released, without snap");
        if (pipFloat.isNull()) return;
        check(pipFloat->frameGeometry().intersects(work),
              "the freely-placed float is still reachable on its work area");
    });
    runner.add(21200, [&] {
        if (pipFloat == nullptr) {
            return;
        }
        auto* pipSurface = pipFloat->videoSurface();
        // NOT findChild: the float owns a VideoSurface too, and which one
        // comes first depends on construction order - comparing a surface
        // against itself passes every check here.
        auto* mainSurface = shell->videoSurface();
        if (mainSurface == nullptr || pipSurface == nullptr
            || mainSurface == pipSurface) {
            check(false, "the two surfaces are distinct and both reachable");
            return;
        }
        // The float loads its own core when the window is first exposed, a beat
        // after it opens. Everything below needs a live stream.
        for (int i = 0; i < 100 && !pipSurface->hasVideo(); ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }
        if (!pipSurface->hasVideo()) {
            // Same as above: no stream in a headless session is an environment,
            // not a defect, and the checks below all need one.
            std::fprintf(stderr, "SKIP  the float ownership checks: no stream in this session\n");
            return;
        }
        check(true, "the float's own core loaded the stream");
        // The main core has to be stopped: the float has its own mpv handle,
        // so a still-playing main core decodes a second copy of the same
        // stream behind a hidden window.
        check(!mainSurface->isPlaying(), "the hidden main player is not decoding while PiP is up");
        check(mainSurface->hasVideo(), "the main player keeps its stream loaded for the return");
        if (!pipSurface->isPlaying()) {
            pipSurface->setPaused(false);
        }
        QTest::qWait(1000);
        check(pipSurface->isPlaying(), "the float is the one playing");

        // Each core is compared against ITSELF, not against the other: the
        // float has been playing while the main one is parked, so only a
        // before/after on the same core says anything.
        const double floatBefore = pipSurface->position();
        const double mainBefore = mainSurface->position();
        // The float's scrubber used to seek the main surface, so dragging it
        // moved a video nobody could see.
        pipSurface->seek(floatBefore + 5.0);
        QTest::qWait(400);
        check(pipSurface->position() > floatBefore + 1.0, "seeking the float moves the float");
        check(std::abs(mainSurface->position() - mainBefore) < 0.5,
              "seeking the float leaves the hidden main player alone");

        // The counter follows the float's core; it froze on the moment PiP
        // opened when it was fed from the parked one.
        // The counter is driven from the float's own positionChanged, so the
        // label must track that core. A seek is the trigger: the elapsed label
        // is only rewritten when a position arrives, and with the chain
        // enabled mpv re-emits, so this is the cheapest moment to prove the
        // label is live rather than the placeholder it used to freeze on.
        auto* elapsed = pipFloat->findChild<QLabel*>(QString::fromLatin1("pipElapsed"));
        check(elapsed != nullptr, "the float has a position label");
        if (elapsed != nullptr) {
            const QString before = elapsed->text();
            pipSurface->seek(pipSurface->position() + 30.0);
            QTest::qWait(900);
            std::fprintf(stderr, "     label: \"%s\" -> \"%s\"\n", qPrintable(before),
                         qPrintable(elapsed->text()));
            check(elapsed->text() != before, "the float's clock follows its own core");
        }
    });

    // Leaving PiP brings the main player back to where the viewer actually
    // watched, which is the float's position and not where the main core was
    // parked on entry.
    runner.add(24000, [&] {
        if (pipFloat == nullptr) {
            check(false, "the float was still up for the return check");
            return;
        }
        auto* floatSurface = pipFloat->videoSurface();
        if (!floatSurface->isPlaying()) {
            floatSurface->setPaused(false);
        }
        QTest::qWait(700);
        floatSurface->setPaused(true);
        check(waitFor([&] { return !floatSurface->isPlaying(); }, 3000),
              "the viewer's paused float state is acknowledged before restore");
        floatExitPosition = floatSurface->position();
        floatExitPlaying = floatSurface->isPlaying();
        auto* restore = pipFloat->findChild<QPushButton*>(QString::fromLatin1("pipRestore"));
        check(restore != nullptr, "the float has a restore button");
        if (restore == nullptr) {
            return;
        }
        restore->click();
        // The hand-back is deferred by a tick on purpose, so wait for the float
        // to be gone and the window to be back rather than sampling
        // mid-transition.
        waitFor([&] {
            return dialogGuard.isNull() ||
                   (pipFloat != nullptr && !pipFloat->isVisible() && dialog->isVisible());
        }, 8000);
        // The float is pooled rather than destroyed - each owns a
        // QOpenGLWidget and tearing one down beside the player's crashes
        // the display driver - so "closed" means hidden, not gone.
        check(pipFloat != nullptr && !pipFloat->isVisible(),
              "the restore button put the float away");
        check(dialogGuard != nullptr && dialog->isVisible(), "the player window is back");
        auto* mainSurface = dialog->findChild<VideoSurface*>();
        if (mainSurface == nullptr) {
            return;
        }
        waitFor([&] {
            return dialogGuard.isNull() ||
                   (std::abs(mainSurface->position() - floatExitPosition) < 1.5 &&
                    mainSurface->isPlaying() == floatExitPlaying);
        }, 5000);
        std::fprintf(stderr, "     returned to %.2fs, the float left it at %.2fs\n",
                     mainSurface->position(), floatExitPosition);
        check(std::abs(mainSurface->position() - floatExitPosition) < 1.5,
              "the main player resumes where the float left off");
        check(mainSurface->isPlaying() == floatExitPlaying,
              "the main core continues the float's playback state");
    });

    // The float's X and its expand button mean opposite things, and they were
    // wired to the same slot: X put the full window back instead of closing.
    // A second player, because the first one is closed by the Esc ladder below
    // and the dialog is destroyed with it.
    runner.add(300, [&] {
        StubHost secondHost;
        auto* second = new PlayerDialog(QStringLiteral("probe pip close"), url, qualities, 720,
                                        &secondHost);
        QVector<QVariantMap> entries;
        for (int episode = 1; episode <= 3; ++episode) {
            entries.append(QVariantMap{{"label", QStringLiteral("Серія %1").arg(episode)},
                                       {"embed", QStringLiteral("e%1").arg(episode)}});
        }
        second->setPlaylist(entries, 0);
        second->setVariants({QVariantMap{{"label", QStringLiteral("Ashdi 1080p")},
                                      {"embed", QStringLiteral("e1")},
                                      {"kind", QStringLiteral("voice")}}},
                            QStringLiteral("e1"));
        second->show();
        auto* secondShell = second->findChild<PlayerShell*>();
        check(secondShell != nullptr, "the second player has a shell");
        if (secondShell == nullptr) {
            // A window left on screen would skew every stage after this one.
            second->close();
            return;
        }
        // The float is parentless, so it is not in the dialog's child tree:
        // top-level widgets, the way the PiP stage finds it too. Only a float
        // that was not already there can be the one this player just opened,
        // which keeps a pooled float belonging to another player out of reach.
        QVector<PipFloat*> preexisting;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (auto* f = qobject_cast<PipFloat*>(w)) preexisting.append(f);
        }
        emit secondShell->pipRequested();
        PipFloat* floatWindow = nullptr;
        waitFor([&] {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                auto* f = qobject_cast<PipFloat*>(w);
                if (f != nullptr && f->isVisible() && !preexisting.contains(f)) {
                    floatWindow = f;
                    return true;
                }
            }
            return false;
        }, 6000);
        check(floatWindow != nullptr, "the second player opened a float");
        auto* close = floatWindow != nullptr
                          ? floatWindow->findChild<QPushButton*>(QStringLiteral("pipClose"))
                          : nullptr;
        check(close != nullptr, "the float has a close button");
        check(!secondHost.closed, "the player is still open while the float is up");
        // The float is parentless on purpose, so the player that opened it is
        // the only thing that can release it. Watch it through a guard: the
        // player is WA_DeleteOnClose, so a raw look at the float once the
        // player is freed is the use-after-free this probe has to avoid.
        QPointer<PipFloat> activeFloatGuard(floatWindow);
        // A null guard afterwards only says the float is gone. This counts HOW
        // MANY times it was destroyed, and reads the player's own surface at
        // that instant: the float here was UP, so a release that ran while the
        // main surface was still visible is the DrvPresentBuffers fault, and a
        // second destroy of the same window is a double free. Neither is
        // visible from the guard alone.
        auto activeRelease = watchFloatRelease(floatWindow, playerSurface(second));
        const int floatsBefore = livePipFloats();
        if (close != nullptr) {
            close->click();
            QTest::qWait(2500);
            check(secondHost.closed, "the float's X closed the player");
            // No isVisible() here: the dialog is WA_DeleteOnClose, so the host
            // being told is the only safe evidence. Reading the window after
            // playerClosed() is the use-after-free this probe already guards
            // the first dialog against.
        } else {
            // The host is told on close, and the dialog is destroyed with it, so
            // a check that never runs would leave a second player on screen for
            // the rest of the run and skew every check after it.
            second->close();
        }
        // If the X did not close the player, close it here rather than leave a
        // window on screen for the rest of the run. `second` is only reachable
        // while the host has not been told the dialog closed, which is what
        // playerClosed() means and when the dialog is still alive.
        if (!secondHost.closed) {
            second->close();
        }
        // The float was up when the player closed, and releasing it is the
        // owner's job. Wait for that deferred delete to land and read through
        // the guard: before the destructor released the pool this still named a
        // live top-level float, its QOpenGLWidget and its mpv core.
        waitFor([&] { return activeFloatGuard.isNull(); }, 4000);
        check(activeFloatGuard.isNull(), "closing the player destroyed the float it had up");
        std::fprintf(stderr,
                     "     active float release: destroyed=%d mainAliveAtDestroy=%d "
                     "mainVisibleAtDestroy=%d floatsBefore=%d floatsAfter=%d\n",
                     activeRelease->destroyed, int(activeRelease->mainAliveAtDestroy),
                     int(activeRelease->mainVisibleAtDestroy), floatsBefore, livePipFloats());
        check(activeRelease->destroyed == 1,
              "closing the player destroyed the float it had up exactly once");
        check(activeRelease->mainAliveAtDestroy,
              "the float was released while the player still owned its surface");
        // The order, stated as the fault it prevents: the parked/up float's GL
        // widget must not be torn down while the main one is on screen.
        check(!activeRelease->mainVisibleAtDestroy,
              "the float's GL widget was torn down with the player's surface off screen");
        check(livePipFloats() == floatsBefore - 1,
              "closing the player left no parentless float behind");
    });

    // Custom geometry survives pooled reentry; closing the owner releases the hidden float.
    runner.add(310, [&] {
        StubHost parkedHost;
        auto* parked = new PlayerDialog(QStringLiteral("probe pip parked"), url, qualities, 720,
                                        &parkedHost);
        QPointer<PlayerDialog> parkedGuard(parked);
        QVector<QVariantMap> entries;
        for (int episode = 1; episode <= 3; ++episode) {
            entries.append(QVariantMap{{"label", QStringLiteral("Серія %1").arg(episode)},
                                       {"embed", QStringLiteral("e%1").arg(episode)}});
        }
        parked->setPlaylist(entries, 0);
        parked->setVariants({QVariantMap{{"label", QStringLiteral("Ashdi 1080p")},
                                         {"embed", QStringLiteral("e1")},
                                         {"kind", QStringLiteral("voice")}}},
                            QStringLiteral("e1"));
        parked->show();
        auto* parkedShell = parked->findChild<PlayerShell*>();
        check(parkedShell != nullptr, "the parked-case player has a shell");
        if (parkedShell == nullptr) {
            parked->close();
            return;
        }
        // Hidden floats belong to other pools, not this entry.
        QVector<PipFloat*> preexisting;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (auto* f = qobject_cast<PipFloat*>(w)) preexisting.append(f);
        }
        auto awaitNewFloat = [&]() -> PipFloat* {
            PipFloat* found = nullptr;
            waitFor([&] {
                for (QWidget* w : QApplication::topLevelWidgets()) {
                    auto* f = qobject_cast<PipFloat*>(w);
                    if (f != nullptr && f->isVisible() && !preexisting.contains(f)) {
                        found = f;
                        return true;
                    }
                }
                return false;
            }, 6000);
            return found;
        };
        emit parkedShell->pipRequested();
        PipFloat* parkedFloat = awaitNewFloat();
        check(parkedFloat != nullptr, "the parked-case player opened a float");
        if (parkedFloat == nullptr) {
            parked->close();
            return;
        }
        QPointer<PipFloat> parkedFloatGuard(parkedFloat);
        auto parkedRelease = watchFloatRelease(parkedFloat, playerSurface(parked));
        const int floatsBefore = livePipFloats();
        auto clickRestore = [](const QPointer<PipFloat>& pip) -> bool {
            if (pip.isNull()) return false;
            auto* restore = pip->findChild<QPushButton*>(QStringLiteral("pipRestore"));
            if (restore == nullptr) return false;
            restore->click();
            return true;
        };
        QTest::qWait(300);
        if (parkedGuard.isNull() || parkedFloatGuard.isNull()) {
            check(false, "the parked-case player and float survived initial layout");
            if (parkedGuard != nullptr) parkedGuard->close();
            return;
        }
        QScreen* screen = QGuiApplication::screenAt(parkedFloatGuard->frameGeometry().center());
        check(screen != nullptr, "the parked-case float has a work area for custom geometry");
        if (screen == nullptr) {
            parkedGuard->close();
            return;
        }
        const QRect work = screen->availableGeometry();
        const QSize defaultSize = parkedFloatGuard->size();
        const QPoint defaultPosition = parkedFloatGuard->pos();
        const QSize room = work.size() - defaultSize;
        const int resizeDelta = std::max(1, work.width() / 24);
        const bool hasRoom =
            room.width() * 3 / 5 > resizeDelta && room.height() > 0;
        check(hasRoom, "the work area fits arbitrary placement and a wider custom float");
        if (!hasRoom) {
            parkedGuard->close();
            return;
        }
        const QPoint customPosition =
            work.topLeft() + QPoint(room.width() * 2 / 5, room.height() / 3);
        const QSize customSize(defaultSize.width() + resizeDelta, defaultSize.height());
        dragPipTo(parkedFloatGuard, customPosition,
                  "the parked-case float is placed at an exact arbitrary noncorner position");
        resizePipRightEdge(parkedFloatGuard, resizeDelta);
        if (parkedGuard.isNull() || parkedFloatGuard.isNull()) {
            if (parkedGuard != nullptr) parkedGuard->close();
            return;
        }
        check(parkedFloatGuard->pos() != defaultPosition &&
                  parkedFloatGuard->size() != defaultSize,
              "the float has a real custom position and size before leaving PiP");
        check(clickRestore(parkedFloatGuard), "the parked-case float has a restore button");
        waitFor([&] {
            return parkedGuard.isNull() || parkedFloatGuard.isNull() ||
                   (!parkedFloatGuard->isVisible() && parkedGuard->isVisible());
        }, 6000);
        check(parkedGuard != nullptr && parkedFloatGuard != nullptr &&
                  !parkedFloatGuard->isVisible(),
              "the restore parked the float instead of destroying it");
        check(parkedRelease->destroyed == 0,
              "leaving PiP destroyed nothing - the float is pooled, not freed");
        if (parkedGuard == nullptr || parkedFloatGuard == nullptr) {
            if (parkedGuard != nullptr) parkedGuard->close();
            return;
        }

        emit parkedShell->pipRequested();
        QPointer<PipFloat> reopened = awaitNewFloat();
        check(reopened != nullptr, "the second PiP entry opened a float");
        check(reopened != nullptr && reopened == parkedFloatGuard,
              "re-entering PiP reuses the parked float instead of building another");
        if (reopened != nullptr) {
            QTest::qWait(500);
            if (reopened != nullptr) {
                std::fprintf(stderr,
                             "     PiP reentry: expected=%d,%d %dx%d actual=%d,%d %dx%d "
                             "default=%d,%d %dx%d\n",
                             customPosition.x(), customPosition.y(), customSize.width(),
                             customSize.height(), reopened->x(), reopened->y(),
                             reopened->width(), reopened->height(), defaultPosition.x(),
                             defaultPosition.y(), defaultSize.width(), defaultSize.height());
                check(reopened->pos() == customPosition,
                      "pooled reentry retains the exact arbitrary position, not the default corner");
                check(reopened->size() == customSize,
                      "pooled reentry retains the exact edge-resized size, not the default size");
                check(clickRestore(reopened), "the re-entered float has a restore button");
                waitFor([&] {
                    return parkedGuard.isNull() || parkedFloatGuard.isNull() ||
                           !parkedFloatGuard->isVisible();
                }, 6000);
            }
        }
        check(parkedGuard != nullptr && parkedFloatGuard != nullptr &&
                  !parkedFloatGuard->isVisible(),
              "the float is parked again before its player is closed");
        check(parkedRelease->destroyed == 0,
              "the second leave parked the reused float instead of destroying it");

        if (parkedGuard != nullptr) parkedGuard->close();
        waitFor([&] { return parkedGuard.isNull(); }, 6000);
        check(parkedGuard.isNull(), "closing the parked-case player was processed");
        if (!parkedFloatGuard.isNull()) {
            int floats = 0;
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (qobject_cast<PipFloat*>(w) != nullptr) floats++;
            }
            std::fprintf(stderr,
                         "     parked float alive: visible=%d topLevelFloats=%d owned=%d\n",
                         int(parkedFloatGuard->isVisible()), floats,
                         int(parkedFloatGuard->parent() != nullptr));
        }
        check(parkedFloatGuard.isNull(),
              "closing the player destroyed the float it had parked");
        std::fprintf(stderr,
                     "     parked float release: destroyed=%d mainAliveAtDestroy=%d "
                     "mainVisibleAtDestroy=%d floatsBefore=%d floatsAfter=%d\n",
                     parkedRelease->destroyed, int(parkedRelease->mainAliveAtDestroy),
                     int(parkedRelease->mainVisibleAtDestroy), floatsBefore, livePipFloats());
        check(parkedRelease->destroyed == 1,
              "closing the player released the parked float exactly once");
        check(parkedRelease->mainAliveAtDestroy,
              "the parked float was released with the dialog that owned it");
        check(!parkedRelease->mainVisibleAtDestroy,
              "the parked float's GL widget went with the player's surface off screen");
        check(livePipFloats() == floatsBefore - 1,
              "closing the parked-case player left no parentless float behind");
    });

    // Esc unwinds one layer per press - picker, then the shell's own popover,
    // then the series drawer, then the player - so what matters is that the
    // ladder terminates, not that one press out of a busy UI closes it. Each
    // press gets its own beat on the loop, and every look at the window goes
    // through the guard: the press that closes it is the one that frees it.
    runner.add(30000, [&] {
        for (int press = 0; press < 12 && !host.closed; ++press) {
            QTest::qWait(300);
            if (dialogGuard.isNull()) break;
            QTest::keyClick(dialogGuard, Qt::Key_Escape);
        }
        check(host.closed, "Esc closed the player once nothing was open");
        // host.closed is set from the close event, which runs before the
        // WA_DeleteOnClose deletion. The float is released by the destructor,
        // so the deletion has to land before the float can be read as gone.
        waitFor([&] { return dialogGuard.isNull(); }, 6000);
        check(dialogGuard.isNull(), "closing the player processed its deletion");
        // This player entered PiP and restored, so its float is the parked
        // case for the window every earlier stage drove. The dialog is the only
        // thing that can release a parentless float, and its destruction is
        // exactly what the Esc press just ran: before the destructor released
        // the pool this guard still named a hidden top-level float, complete
        // with its QOpenGLWidget and mpv core.
        if (!pipFloat.isNull()) {
            std::fprintf(stderr, "     parked float survived the player: visible=%d\n",
                         int(pipFloat->isVisible()));
        }
        check(pipFloat.isNull(), "closing the player destroyed the float it had parked");
    });

    runner.start();
    return app.exec();
}
