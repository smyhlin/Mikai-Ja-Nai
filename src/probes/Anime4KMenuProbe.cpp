// Anime4K menu probe: the real popovers, every preset leaf, the planned-model
// pages, persistence across a reopen, and the pixels mpv actually draws.
//
//   - the dock's enhancement button opens a tree of panels standing side by
//     side, one per level, with no Back row: a category opens the level beside
//     the one it sits in, the levels above it stay, the levels below it close
//   - every leaf is only ever reached from a menu opened for it, and each one
//     has to reach its exact catalogue id AND put that preset's ordered chain
//     into mpv's glsl-shaders, stage for stage
//   - the three planned-model pages are informational: every row on them is one
//     the viewer cannot press, and none of them changes the running chain
//   - the experiment that was picked has to survive a close/reopen: the chain
//     id, the info chip, the marked menu row, and the rendered picture
//   - Compare's chooser offers every catalogue preset exactly once, nested under
//     the categories the catalogue gives it, never offers the chain-internal
//     AutoDownscalePre steps, and never rewrites the chain the player is
//     running - checked after driving its own actions
//
// Rows are addressed by the catalogue id the shell hangs on them, never by the
// label printed on them, so nothing here fails because a menu was reworded or a
// translation changed.
//
// mpv hands glsl-shaders back without validating it, so the echo alone cannot
// prove a preset runs. The rendered difference against a measured noise floor
// is the proof, which is why this plays real video instead of reading sources.
//
// Run from the repo root, with the shaders deployed beside the exe:
//   build-cmake/anime4k_menu_probe.exe

#include <QAction>
#include <QApplication>
#include <QHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "Anime4K.hpp"
#include "CompareWindow.hpp"
#include "PlayerDialog.hpp"
#include "PlayerPopups.hpp"
#include "PlayerShell.hpp"
#include "VideoSurface.hpp"
#include "ProbeRuntime.hpp"

using namespace mplayer;
using namespace mplayer::shell;

namespace {

constexpr int kSourceWidth = 480;
constexpr int kSourceHeight = 270;
constexpr auto kExperiment = "exp_3d_graphics";

int failures = 0;

void check(bool ok, const char* what) {
    std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

void pump(int milliseconds) {
    for (int waited = 0; waited < milliseconds; waited += 20) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    QCoreApplication::processEvents();
}

template <typename Done>
bool waitUntil(Done done, int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 40) {
        QCoreApplication::processEvents();
        if (done()) return true;
        QThread::msleep(40);
    }
    return done();
}

// ── the picture, measured rather than assumed ───────────────────────────
//
// mpv fits the video to the widget and centres it, so the band the picture
// occupies follows from the source aspect alone. It is trimmed by an eighth:
// where a letterbox bar meets the picture two frames differ hugely, and that
// edge would swamp the signal.

QRect pictureBand(const QImage& frame) {
    if (frame.isNull() || frame.width() < 64 || frame.height() < 64) return QRect();
    const double scale = std::min(double(frame.width()) / kSourceWidth,
                                  double(frame.height()) / kSourceHeight);
    const int width = int(kSourceWidth * scale);
    const int height = int(kSourceHeight * scale);
    if (width < 64 || height < 64) return QRect();
    return QRect((frame.width() - width) / 2 + width / 8,
                 (frame.height() - height) / 2 + height / 8,
                 width * 3 / 4, height * 3 / 4);
}

/// Mean absolute channel difference over the picture band, 0…255.
double bandDifference(const QImage& before, const QImage& after) {
    const QRect band = pictureBand(before);
    if (band.isEmpty() || after.isNull() || after.size() != before.size()) return -1.0;
    const QImage left = before.convertToFormat(QImage::Format_RGB888);
    const QImage right = after.convertToFormat(QImage::Format_RGB888);
    double sum = 0.0;
    qint64 count = 0;
    for (int y = band.top(); y < band.bottom(); ++y) {
        const uchar* a = left.constScanLine(y);
        const uchar* b = right.constScanLine(y);
        for (int x = band.left(); x < band.right(); ++x) {
            const int at = x * 3;
            sum += std::abs(int(a[at]) - int(b[at])) + std::abs(int(a[at + 1]) - int(b[at + 1])) +
                   std::abs(int(a[at + 2]) - int(b[at + 2]));
            count += 3;
        }
    }
    return count > 0 ? sum / double(count) : -1.0;
}

/// Peak-to-peak luma across the band: a blank or uniform framebuffer reads
/// near zero, a decoded frame of testsrc2 does not.
int bandRange(const QImage& frame) {
    const QRect band = pictureBand(frame);
    if (band.isEmpty()) return -1;
    const QImage image = frame.convertToFormat(QImage::Format_RGB888);
    int low = 255;
    int high = 0;
    for (int y = band.top(); y < band.bottom(); ++y) {
        const uchar* line = image.constScanLine(y);
        for (int x = band.left(); x < band.right(); ++x) {
            const int at = x * 3;
            const int luma = (line[at] * 299 + line[at + 1] * 587 + line[at + 2] * 114) / 1000;
            low = std::min(low, luma);
            high = std::max(high, luma);
        }
    }
    return high - low;
}

/// A framebuffer that has stopped changing: three identical grabs, not two,
/// because the first can still be the picture from before the chain rebuilt.
QImage settledFrame(VideoSurface* surface, int timeoutMs = 6000) {
    QImage previous;
    int stable = 0;
    for (int waited = 0; waited < timeoutMs; waited += 40) {
        QCoreApplication::processEvents();
        QThread::msleep(40);
        QImage current = surface->grabFramebuffer();
        if (!current.isNull()) current = current.convertToFormat(QImage::Format_RGB32);
        if (!current.isNull() && current == previous) {
            if (++stable >= 3) return current;
        } else {
            stable = 0;
        }
        previous = current;
    }
    return QImage();
}

// ── driving the real popovers ───────────────────────────────────────────
//
// Rows are found by the identity the shell hangs on them - a catalogue level or
// a catalogue preset id - and never by the words printed on them. A menu label
// is display text: it is translated, it is reworded, and a probe that hunts for
// a string it once saw calls a working menu broken the moment either happens.
// The panel itself is found by the level the shell records on it, for the same
// reason: every level below the root is the same pooled widget under the same
// name.

/// The visible panel showing catalogue `levelId`, waited for up to `timeoutMs`.
/// A timeout of zero samples once and answers immediately, which is what a
/// caller asking "is this level still up?" wants - waiting there would only
/// turn a closed level into a stall.
MenuPopup* levelPanel(QWidget* host, const QString& levelId, int timeoutMs = 4000) {
    for (int waited = 0; waited == 0 || waited < timeoutMs; waited += 40) {
        QCoreApplication::processEvents();
        for (MenuPopup* panel : host->findChildren<MenuPopup*>()) {
            if (panel->isVisible() && panel->property("enhancementLevel").toString() == levelId) {
                return panel;
            }
        }
        QThread::msleep(40);
    }
    return nullptr;
}

int rowFor(MenuPopup* panel, const QString& id) {
    if (panel == nullptr) return -1;
    for (int spot = 0; spot < panel->rowCount(); ++spot) {
        const QString payload = panel->entryAt(spot).payload.toString();
        if (payload == QLatin1String("level|") + id || payload == QLatin1String("preset|") + id) {
            return spot;
        }
    }
    return -1;
}

QPushButton* rowButton(MenuPopup* panel, const QString& id) {
    const int spot = rowFor(panel, id);
    if (spot < 0) return nullptr;
    return qobject_cast<QPushButton*>(panel->rowWidget(spot));
}

/// The catalogue ids of every preset row on `panel`, in the order the menu
/// shows them. Read off the rows' own payloads, so the answer is the menu's and
/// not a copy of it.
QStringList presetIdsOn(MenuPopup* panel) {
    QStringList ids;
    if (panel == nullptr) return ids;
    for (int spot = 0; spot < panel->rowCount(); ++spot) {
        const QString payload = panel->entryAt(spot).payload.toString();
        if (payload.startsWith(QLatin1String("preset|"))) {
            ids << payload.mid(7);
        }
    }
    return ids;
}

QStringList categoryIdsOn(MenuPopup* panel) {
    QStringList ids;
    if (panel == nullptr) return ids;
    for (int spot = 0; spot < panel->rowCount(); ++spot) {
        const QString payload = panel->entryAt(spot).payload.toString();
        if (payload.startsWith(QLatin1String("level|"))) ids << payload.mid(6);
    }
    return ids;
}

/// Whether every row of `panel` leads nowhere: the shape a page takes when what
/// it holds is not implemented yet.
bool everyRowInert(MenuPopup* panel) {
    if (panel == nullptr || panel->rowCount() == 0) return false;
    for (int spot = 0; spot < panel->rowCount(); ++spot) {
        if (!panel->entryAt(spot).disabled) return false;
    }
    return true;
}

/// A real press-then-release on a visible row, delivered to the widget itself
/// rather than injected through the window system: the probe drives the same
/// Qt controls a viewer clicks, without depending on which window the desktop
/// happens to have in front.
void realClick(QWidget* widget) {
    const QPointF at = widget->rect().center();
    const QPointF global = widget->mapToGlobal(at.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, at, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
}

bool clickRow(MenuPopup* panel, const QString& id) {
    QPushButton* row = rowButton(panel, id);
    if (row == nullptr || !row->isEnabled()) return false;
    realClick(row);
    QCoreApplication::processEvents();
    return true;
}

/// The dock's sparkle button, by the tooltip a viewer hovers. The chrome
/// auto-hides, and a hidden button is not one a viewer could press, so the
/// shell is woken first.
QPushButton* enhanceButton(PlayerShell* shell, QWidget* dialog) {
    shell->wake();
    QCoreApplication::processEvents();
    for (QPushButton* button : dialog->findChildren<QPushButton*>()) {
        if (button->isVisible() && button->toolTip() == QLatin1String("open_enhancements")) {
            return button;
        }
    }
    return nullptr;
}

struct Menus {
    MenuPopup* enhancements = nullptr;
    MenuPopup* anime = nullptr;
};

bool openEnhancements(PlayerShell* shell, QWidget* dialog, Menus& menus) {
    QPushButton* trigger = enhanceButton(shell, dialog);
    if (trigger == nullptr) return false;
    realClick(trigger);
    QCoreApplication::processEvents();
    menus.enhancements = levelPanel(dialog, QString());
    return menus.enhancements != nullptr;
}

bool openAnimeRoot(PlayerShell* shell, QWidget* dialog, Menus& menus) {
    if (!openEnhancements(shell, dialog, menus)) return false;
    if (!clickRow(menus.enhancements, QStringLiteral("anime4k"))) return false;
    menus.anime = levelPanel(dialog, QStringLiteral("anime4k"));
    return menus.anime != nullptr;
}

/// Root, then each catalogue level of `path`, then the leaf. A leaf is only
/// ever reached from a menu opened for it, and every level is reopened from the
/// top: the tree is pooled, so a level left standing is one the next visit
/// would inherit.
bool clickLeaf(const QStringList& path, const QString& presetId, PlayerShell* shell,
               QWidget* dialog, Menus& menus) {
    if (!openAnimeRoot(shell, dialog, menus)) return false;
    for (const QString& levelId : path) {
        if (!clickRow(menus.anime, levelId)) return false;
        menus.anime = levelPanel(dialog, levelId);
        if (menus.anime == nullptr) return false;
    }
    return clickRow(menus.anime, presetId);
}

/// Whether mpv is running exactly the chain the catalogue holds for `id`.
///
/// This is the end-to-end wiring: menu row → stable id → glsl-shaders. The
/// files are looked for in order and the stage count is compared too, so a
/// preset that gained or lost a step cannot pass. The echo is still mpv's own
/// word - the pixel checks below are what prove the chain drew anything.
bool chainMatches(VideoSurface* surface, const QString& id) {
    const QStringList expected = anime4k::filesForId(id);
    const QString live = surface->effectiveShaderChain();
    const int staged = live.isEmpty() ? 0 : live.count(QLatin1String(".glsl"));
    int found = -1;
    for (const QString& file : expected) {
        found = live.indexOf(file, found + 1);
        if (found < 0) return false;
    }
    return staged == expected.size() && anime4k::chainIsComplete(anime4k::candidateForId(id));
}

void shoot(QWidget* widget, const QString& name) {
    check(widget != nullptr, "there is a widget to screenshot");
    if (widget == nullptr) return;
    const QString path = QDir::current().filePath(QStringLiteral("build-cmake/") + name);
    const bool written =
        QDir().mkpath(QFileInfo(path).absolutePath()) && widget->grab().save(path);
    check(written, (QStringLiteral("screenshot written: ") + name).toUtf8().constData());
}

void saveFrame(const QImage& frame, const QString& name) {
    const QString path = QDir::current().filePath(QStringLiteral("build-cmake/") + name);
    const bool written =
        !frame.isNull() && QDir().mkpath(QFileInfo(path).absolutePath()) && frame.save(path);
    check(written, (QStringLiteral("framebuffer written: ") + name).toUtf8().constData());
}

// ── the compare chooser, read as a tree ─────────────────────────────────
//
// A chain-internal downscale is not a complete comparison candidate.

void collectLeafIds(const QList<QAction*>& actions, QStringList* ids) {
    for (QAction* action : actions) {
        if (QMenu* sub = action->menu(); sub != nullptr) {
            collectLeafIds(sub->actions(), ids);
            continue;
        }
        ids->append(action->data().toString());
    }
}

QStringList chooserPathFor(const QString& categoryId) {
    QStringList path;
    QString parent;
    const QStringList parts = categoryId.split(QLatin1Char(47), Qt::SkipEmptyParts);
    for (const QString& component : parts) {
        if (!parent.isEmpty()) parent += QLatin1Char(47);
        parent += component;
        const QString label = anime4k::categoryLabel(parent);
        if (!label.isEmpty()) path.append(label);
    }
    return path;
}

int countActionText(const QList<QAction*>& actions, const QString& text, bool recursive) {
    int matches = 0;
    for (QAction* action : actions) {
        if (action->text() == text) ++matches;
        if (recursive && action->menu() != nullptr) {
            matches += countActionText(action->menu()->actions(), text, true);
        }
    }
    return matches;
}

// ── the dialog ──────────────────────────────────────────────────────────

PlayerDialog* createDialog(const QString& url) {
    auto* dialog = new PlayerDialog(QStringLiteral("Anime4K menu probe"), url);
    dialog->startPlayer();
    dialog->show();
    return dialog;
}

bool waitForVideo(VideoSurface* surface, int timeoutMs = 10000) {
    return waitUntil(
        [surface] {
            return surface != nullptr && surface->hasVideo() && surface->isContextReady() &&
                   surface->videoWidth() > 0 && surface->videoHeight() > 0;
        },
        timeoutMs);
}

/// Closes the dialog and waits for WA_DeleteOnClose to actually destroy it -
/// through a QPointer, because touching a deleted dialog is the bug this is
/// here to avoid.
bool closeDialog(QPointer<PlayerDialog> dialog) {
    if (dialog == nullptr) return false;
    dialog->close();
    return waitUntil([&dialog] { return dialog.isNull(); }, 6000);
}

/// Every preset leaf, with the catalogue levels that lead to it: the category,
/// then the experiment family when the leaf is nested one deeper. Ids only -
/// the labels those rows are painted with are the catalogue's business, and a
/// table of them here would only ever test that nobody reworded a menu.
struct LeafCheck {
    const char* id;
    const char* category;
    const char* family;
};

const LeafCheck kLeaves[] = {
    {"off", "", ""},
    {"fast_a", "anime4k/speed", ""},
    {"fast_b", "anime4k/speed", ""},
    {"fast_c", "anime4k/speed", ""},
    {"hq_a", "anime4k/quality", ""},
    {"hq_b", "anime4k/quality", ""},
    {"hq_c", "anime4k/quality", ""},
    {"hq_aa", "anime4k/maximum", ""},
    {"hq_bb", "anime4k/maximum", ""},
    {"hq_ca", "anime4k/maximum", ""},
    {"exp_3d_graphics", "anime4k/experimental", ""},
    {"exp_old_film", "anime4k/experimental", ""},
    {"exp_deblur_strong", "anime4k/experimental", ""},
    {"exp_noise_bilateral_mean", "anime4k/experimental", "anime4k/experimental/noise"},
    {"exp_noise_bilateral_median", "anime4k/experimental", "anime4k/experimental/noise"},
    {"exp_noise_bilateral_mode", "anime4k/experimental", "anime4k/experimental/noise"},
    {"exp_thin_veryfast", "anime4k/experimental", "anime4k/experimental/thin"},
    {"exp_thin_fast", "anime4k/experimental", "anime4k/experimental/thin"},
    {"exp_thin_hq", "anime4k/experimental", "anime4k/experimental/thin"},
    {"exp_texture_dtd_x2", "anime4k/experimental", "anime4k/experimental/texture"},
    {"exp_texture_dog_x2", "anime4k/experimental", "anime4k/experimental/texture"},
    {"exp_darken_veryfast", "anime4k/experimental", "anime4k/experimental/darken"},
    {"exp_darken_fast", "anime4k/experimental", "anime4k/experimental/darken"},
    {"exp_darken_hq", "anime4k/experimental", "anime4k/experimental/darken"},
};

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // Closing the dialog is not the end of the probe: the reopen follows.
    app.setQuitOnLastWindowClosed(false);

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiAnime4KProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("Anime4KMenuProbe"));
    // Whatever the user picked stays theirs: the player's own setting is read
    // and written inside a temporary directory that dies with the process.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    if (!mplayer::probes::selectStandardRuntime()) {
        return 2;
    }
    QSettings settings;
    settings.clear();
    settings.sync();

    const QString url = QStringLiteral("av://lavfi:testsrc2=size=480x270:rate=25");
    check(!anime4k::findShadersDir().isEmpty(),
          "the Anime4K shader folder is deployed beside the exe");

    QPointer<PlayerDialog> dialog = createDialog(url);
    auto* surface = dialog->findChild<VideoSurface*>();
    auto* shell = dialog->findChild<PlayerShell*>();
    if (!waitForVideo(surface)) {
        std::fprintf(stderr,
                     "FAIL no render context: lavfi testsrc2 did not load or GL never initialised\n");
        delete dialog.data();
        return 2;
    }
    check(shell != nullptr, "the standalone player has a shell");
    if (shell == nullptr) {
        delete dialog.data();
        return 2;
    }
    check(surface->videoWidth() == kSourceWidth && surface->videoHeight() == kSourceHeight,
          "a real 480x270 source is playing through mpv");

    // ── the control picture ─────────────────────────────────────────────
    // Off explicitly, whatever the settings file held, then one pause and two
    // settled grabs with nothing between them: that difference is the noise
    // floor every later comparison has to clear.
    surface->setAnime4KMode(anime4k::Mode::Off);
    check(surface->effectiveShaderChain().isEmpty(),
          "Disabled leaves mpv with no shader chain at all");
    surface->setPaused(true);
    pump(400);
    const QImage offA = settledFrame(surface);
    const QImage offB = settledFrame(surface);
    const double noise = bandDifference(offA, offB);
    std::fprintf(stderr, "     control: two settled Off frames differ by %.4f / 255\n", noise);
    check(!offA.isNull() && noise >= 0.0, "a real framebuffer is readable with the player paused");
    check(noise <= 0.01, "the paused picture is stable between grabs, so the floor is ~zero");
    saveFrame(offA, QStringLiteral("anime4k-menu-off.png"));

    // ── the menu the viewer actually opens ──────────────────────────────
    Menus menus;
    check(openAnimeRoot(shell, dialog, menus),
          "the enhancement button opens ПОКРАЩЕННЯ and the Anime4K row opens its popover");
    if (menus.anime == nullptr) {
        delete dialog.data();
        return 3;
    }
    check(!menus.anime->isWindow() && menus.anime->parentWidget() == dialog,
          "the Anime4K menu is an in-window overlay, not a native popup");
    shoot(dialog, QStringLiteral("anime4k-menu-root.png"));

    // The root lists the four top-level branches of the tree. The ids come from
    // the catalogue, so a branch that is added or renamed is covered without
    // this probe needing to know its name.
    const QStringList rootBranches = categoryIdsOn(menus.enhancements);
    check(rootBranches.contains(QStringLiteral("anime4k")),
          "the enhancements root offers Anime4K");
    for (const QString& page : {QStringLiteral("rife"), QStringLiteral("realesrgan"),
                                QStringLiteral("other-models")}) {
        check(rootBranches.contains(page),
              (QStringLiteral("the enhancements root offers the planned-model branch: ") + page)
                  .toUtf8()
                  .constData());
    }

    // A category row opens the level beside it; it must not select anything on
    // the way.
    check(clickRow(menus.anime, QStringLiteral("anime4k/speed")),
          "a category row is clickable");
    check(menus.anime->isVisible(), "a category keeps the popover open");
    check(surface->effectiveShaderChain().isEmpty() &&
              surface->anime4KChainId() == QLatin1String("off"),
          "opening a category cannot change the applied chain");
    menus.anime = levelPanel(dialog, QStringLiteral("anime4k/speed"));
    check(menus.anime != nullptr, "the category opened its own level beside the root");
    if (menus.anime != nullptr) {
        const QStringList speedPresets = presetIdsOn(menus.anime);
        check(speedPresets.contains(QStringLiteral("fast_a")) &&
                  speedPresets.contains(QStringLiteral("fast_c")),
              "the speed level lists its own presets");
    }
    // No Back row: the tree has no way back because it never left - the root
    // and the level it opened are both still on screen.
    check(levelPanel(dialog, QString()) != nullptr,
          "the root stays on screen beside the level it opened");
    check(clickRow(menus.anime, QStringLiteral("anime4k/experimental")),
          "the experimental category opens");
    shoot(dialog, QStringLiteral("anime4k-menu-experiments.png"));
    check(clickRow(menus.anime, QStringLiteral("anime4k/experimental/noise")),
          "an experiment family opens as a submenu");
    shoot(dialog, QStringLiteral("anime4k-menu-family.png"));
    menus.anime = levelPanel(dialog, QStringLiteral("anime4k/experimental/noise"));
    if (menus.anime != nullptr) {
        const QStringList noisePresets = presetIdsOn(menus.anime);
        check(noisePresets.contains(QStringLiteral("exp_noise_bilateral_mean")) &&
                  noisePresets.contains(QStringLiteral("exp_noise_bilateral_mode")),
              "the family lists its own leaves");
    }

    // ── the planned models ──────────────────────────────────────────────
    for (const QString& page : {QStringLiteral("rife"), QStringLiteral("realesrgan"),
                                QStringLiteral("other-models")}) {
        check(openEnhancements(shell, dialog, menus),
              "the enhancements menu reopens for a planned page");
        check(clickRow(menus.enhancements, page), "a planned model opens its page by click");
        MenuPopup* plannedPage = levelPanel(dialog, page);
        check(plannedPage != nullptr, "the planned page opened in the shared tree");
        check(plannedPage != nullptr && everyRowInert(plannedPage),
              "a planned page holds nothing the viewer can press");
        const QString chainBefore = surface->effectiveShaderChain();
        const QString idBefore = surface->anime4KChainId();
        for (int spot = 0; plannedPage != nullptr && spot < plannedPage->rowCount(); ++spot) {
            if (auto* inert = qobject_cast<QPushButton*>(plannedPage->rowWidget(spot))) {
                realClick(inert);
            }
            QCoreApplication::processEvents();
        }
        check(surface->effectiveShaderChain() == chainBefore &&
                  surface->anime4KChainId() == idBefore,
              (QStringLiteral("pressing any row on the ") + page +
               QStringLiteral(" page changes nothing"))
                  .toUtf8()
                  .constData());
    }

    // ── the rendered proof ─────────────────────────────────────────────
    check(clickLeaf({QStringLiteral("anime4k/experimental")}, QLatin1String(kExperiment), shell,
                    dialog, menus),
          "the experiment is selected through the real menu");
    check(surface->anime4KChainId() == QLatin1String(kExperiment),
          "the experiment is the applied candidate id");
    check(chainMatches(surface, QLatin1String(kExperiment)),
          "mpv is running the experiment's ordered chain, stage for stage");
    check(levelPanel(dialog, QStringLiteral("anime4k/experimental")) == nullptr,
          "choosing a preset takes the level it was reached through off the screen");
    auto* chip = dialog->findChild<QLabel*>(QStringLiteral("playerEnhancementStatus"));
    // The chip names the running preset by the catalogue's own label for it,
    // read from the catalogue rather than typed in here.
    QString experimentLabel;
    for (const anime4k::ModeInfo& info : anime4k::availableModes()) {
        if (info.id == QLatin1String(kExperiment)) experimentLabel = info.label;
    }
    check(chip != nullptr && !chip->isHidden() && !experimentLabel.isEmpty() &&
              chip->text().contains(experimentLabel),
          "the info chip names the running experiment");
    settings.sync();
    check(settings.value(QStringLiteral("player/anime4k")).toString() ==
              QLatin1String(kExperiment),
          "the chosen experiment is persisted by its stable id");

    pump(500);
    const QImage on = settledFrame(surface);
    const double effect = bandDifference(offA, on);
    const double threshold = std::max(0.05, noise * 5.0);
    std::fprintf(stderr,
                 "     experiment vs Off over the picture band: %.3f / 255 "
                 "(floor %.4f, threshold %.4f)\n",
                 effect, noise, threshold);
    check(effect > threshold,
          "the experiment changes the rendered picture beyond the measured noise floor");
    saveFrame(on, QStringLiteral("anime4k-menu-experiment.png"));

    // ── every leaf, each reached from a menu opened for it ─────────────
    for (const LeafCheck& leaf : kLeaves) {
        QStringList path;
        if (*leaf.category) path << QString::fromLatin1(leaf.category);
        if (*leaf.family) path << QString::fromLatin1(leaf.family);
        const QString id = QString::fromLatin1(leaf.id);
        if (!clickLeaf(path, id, shell, dialog, menus)) {
            check(false, (QStringLiteral("leaf is clickable: ") + id).toUtf8().constData());
            continue;
        }
        check(surface->anime4KChainId() == id,
              (QStringLiteral("leaf applies its exact id: ") + id).toUtf8().constData());
        check(chainMatches(surface, id),
              (QStringLiteral("mpv runs that preset's ordered chain: ") + id).toUtf8().constData());
    }

    // ── compare ─────────────────────────────────────────────────────────
    check(clickLeaf({QStringLiteral("anime4k/experimental")}, QLatin1String(kExperiment), shell,
                    dialog, menus),
          "the experiment is selected again for the compare and PiP checks");
    const QString chainBeforeCompare = surface->effectiveShaderChain();
    const QString idBeforeCompare = surface->anime4KChainId();
    shell->openPictureMenu(QPoint(dialog->width() / 2, dialog->height() / 2));
    QWidget* pictureMenu = dialog->findChild<QWidget*>(QStringLiteral("playerPictureMenu"));
    // Found by what the row does rather than by what it says: the picture menu
    // entry that opens Compare is the one whose payload names it.
    bool compareClicked = false;
    if (auto* menu = qobject_cast<MenuPopup*>(pictureMenu);
        menu != nullptr && menu->isVisible()) {
        for (int spot = 0; spot < menu->rowCount() && !compareClicked; ++spot) {
            if (menu->entryAt(spot).payload.toString() != QLatin1String("compare")) continue;
            if (auto* row = qobject_cast<QPushButton*>(menu->rowWidget(spot))) {
                realClick(row);
                compareClicked = true;
            }
        }
    }
    check(compareClicked, "the picture's own menu opens the comparison");
    pump(200);
    auto* compare = dialog->findChild<CompareWindow*>();
    check(compare != nullptr && compare->isVisible(), "the Compare window opened on that action");
    if (compare != nullptr) {
        const QImage frozen = compare->frozenFrame();
        std::fprintf(stderr, "     compare froze %dx%d from a %dx%d surface\n", frozen.width(),
                     frozen.height(), surface->width(), surface->height());
        check(!frozen.isNull(), "Compare froze a real framebuffer");
        check(bandRange(frozen) > 32, "the frozen frame carries a decoded picture, not a blank");
        check(frozen.width() >= surface->width() && frozen.height() >= surface->height(),
              "the frozen frame is the live surface's own framebuffer");
        shoot(compare, QStringLiteral("anime4k-menu-compare.png"));

        QToolButton* badge = nullptr;
        for (QToolButton* button : compare->findChildren<QToolButton*>()) {
            if (button->menu() != nullptr) {
                badge = button;
                break;
            }
        }
        check(badge != nullptr, "the compare badges carry the candidate chooser");
        if (badge != nullptr) {
            QMenu* chooser = badge->menu();
            QStringList offered;
            collectLeafIds(chooser->actions(), &offered);

            // The catalogue is the expectation: whatever it holds, all of it has
            // to be pickable here, and nothing beyond it.
            QStringList catalogueIds;
            QHash<QString, QStringList> expectedPath;
            for (const anime4k::ModeInfo& info : anime4k::availableModes()) {
                catalogueIds << info.id;
                expectedPath.insert(info.id, chooserPathFor(info.category));
            }
            QStringList sortedOffered = offered;
            QStringList sortedCatalogue = catalogueIds;
            sortedOffered.sort();
            sortedCatalogue.sort();
            check(!offered.isEmpty() && sortedOffered == sortedCatalogue,
                  "the chooser offers every catalogue preset exactly once, and nothing else");

            // Each preset has to sit where the catalogue says it sits, at the
            // depth its own category implies: the one preset that hangs directly
            // off the top level is a leaf of the outermost submenu, and the rest
            // are leaves of a category inside it. Reading the expected path out
            // of the catalogue is what keeps this from pinning a nesting shape.
            const auto placedCorrectly = [&expectedPath](const QStringList& walked,
                                                          const QString& presetId) {
                const QStringList want = expectedPath.value(presetId);
                return !want.isEmpty() && walked == want;
            };
            bool everyPresetPlaced = !offered.isEmpty();
            for (QAction* action : chooser->actions()) {
                QMenu* level = action->menu();
                if (level == nullptr) continue;
                const QStringList outer{action->text()};
                for (QAction* nested : level->actions()) {
                    QMenu* deepest = nested->menu();
                    if (deepest == nullptr) {
                        if (!placedCorrectly(outer, nested->data().toString())) {
                            everyPresetPlaced = false;
                        }
                        continue;
                    }
                    const QStringList inner{action->text(), nested->text()};
                    for (QAction* leaf : deepest->actions()) {
                        if (leaf->menu() != nullptr) continue;
                        if (!placedCorrectly(inner, leaf->data().toString())) {
                            everyPresetPlaced = false;
                        }
                    }
                }
            }
            check(everyPresetPlaced,
                  "each preset is nested under the categories the catalogue gives it");
            check(countActionText(chooser->actions(), QStringLiteral("AutoDownscalePre"), true) == 0,
                  "the chain-internal AutoDownscalePre steps are never offered");

            // The badge menu is the only chooser, so a preset is picked the way
            // a viewer's click reaches it: through its own QAction. The action
            // pointer is held across the pick on purpose - the refill runs from
            // the action's own handler, and a rebuild instead of a check-mark
            // move would have deleted it.
            QAction* pick = nullptr;
            QMenu* pickLevel = nullptr;
            for (QAction* action : chooser->actions()) {
                QMenu* level = action->menu();
                if (level == nullptr) continue;
                for (QAction* nested : level->actions()) {
                    QMenu* deepest = nested->menu();
                    if (deepest == nullptr) continue;
                    for (QAction* leaf : deepest->actions()) {
                        if (leaf->data().toString() == QLatin1String("hq_a")) {
                            pick = leaf;
                            pickLevel = deepest;
                        }
                    }
                }
            }
            check(pick != nullptr, "the preset the probe picks is in the chooser");
            if (pick != nullptr) {
                const QList<QAction*> before = pickLevel->actions();
                pick->trigger();
                QCoreApplication::processEvents();
                check(pickLevel->actions() == before,
                      "the chooser's cached action tree survives the pick");
                check(pick->isChecked(), "the picked preset is marked as side A's candidate");
                check(surface->anime4KChainId() == idBeforeCompare &&
                          surface->effectiveShaderChain() == chainBeforeCompare,
                      "Compare's own pick never rewrites the player's running chain");
            }
        }
        compare->close();
        pump(100);
        check(!compare->isVisible(), "the Compare window closes without touching the player");
    }
    check(surface->anime4KChainId() == idBeforeCompare,
          "the running chain is the experiment at the end of the compare run");

    // ── picture in picture ──────────────────────────────────────────────
    // The float decodes in its own core, so it needs the chain separately: the
    // mode alone would drop an experiment the compare window had put in place.
    dialog->togglePip();
    QCoreApplication::processEvents();
    check(dialog->isPipActive(), "PiP opened through the player's own control");
    PipFloat* floating = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (PipFloat* pip = qobject_cast<PipFloat*>(widget); pip != nullptr && pip->isVisible()) {
            floating = pip;
        }
    }
    check(floating != nullptr, "the float is a real top-level window");
    if (floating != nullptr) {
        check(floating->videoSurface()->anime4KChainId() == QLatin1String(kExperiment),
              "the float carries the experiment's chain, not just the mode");
        check(waitForVideo(floating->videoSurface(), 12000),
              "the float decoded the same stream in its own core");
        check(chainMatches(floating->videoSurface(), QLatin1String(kExperiment)),
              "the float runs that chain stage for stage");
    }
    dialog->togglePip();
    pump(300);
    check(!dialog->isPipActive(), "PiP closed through the same control");
    check(dialog->isVisible(), "the player window is back after PiP");

    // ── the smallest window the player allows ───────────────────────────
    // The menus are clamped to the window, so the fit that matters is at the
    // minimum size: a level that overflows it would be unreachable.
    shell->windowModeToggled();
    pump(400);
    check(!dialog->isFullScreen(), "the player left full screen for the fit check");
    dialog->resize(dialog->minimumSize());
    pump(400);
    const QSize minimum = dialog->minimumSize();
    check(dialog->width() <= minimum.width() + 8 && dialog->height() <= minimum.height() + 8,
          "the player window is at its minimum size");
    check(openAnimeRoot(shell, dialog, menus), "the menu opens in the smallest window");
    check(clickRow(menus.anime, QStringLiteral("anime4k/experimental")),
          "the deepest category opens in the smallest window");
    menus.anime = levelPanel(dialog, QStringLiteral("anime4k/experimental"), 0);
    check(clickRow(menus.anime, QStringLiteral("anime4k/experimental/noise")),
          "the deepest family opens in the smallest window");
    MenuPopup* deepestPanel =
        levelPanel(dialog, QStringLiteral("anime4k/experimental/noise"), 0);
    check(deepestPanel != nullptr && dialog->rect().contains(deepestPanel->geometry()),
          "the drilled-down menu fits inside the smallest window");
    // The deepest leaf of the deepest level, taken from the panel rather than
    // from a string: if it is on screen at minimum size, every shallower leaf is.
    const QStringList deepestPresets = presetIdsOn(deepestPanel);
    check(deepestPanel != nullptr && !deepestPresets.isEmpty() &&
              rowButton(deepestPanel, deepestPresets.last()) != nullptr,
          "the last leaf of the deepest level is still on screen");

    // ── the reopen ──────────────────────────────────────────────────────
    check(settings.value(QStringLiteral("player/anime4k")).toString() ==
              QLatin1String(kExperiment),
          "the experiment is still the stored setting before the reopen");
    check(closeDialog(dialog), "the WA_DeleteOnClose dialog is destroyed before the reopen");

    dialog = createDialog(url);
    surface = dialog->findChild<VideoSurface*>();
    shell = dialog->findChild<PlayerShell*>();
    if (!waitForVideo(surface) || shell == nullptr || surface == nullptr) {
        std::fprintf(stderr, "FAIL the reopened player never rendered\n");
        delete dialog.data();
        return 4;
    }
    check(surface->anime4KChainId() == QLatin1String(kExperiment),
          "the reopened player restored the experiment's chain by id");
    check(anime4k::modeId(surface->anime4KMode()) == QLatin1String(kExperiment),
          "the reopened player's mode is the restored experiment");
    check(chainMatches(surface, QLatin1String(kExperiment)),
          "the reopened player applies the experiment's ordered chain");
    chip = dialog->findChild<QLabel*>(QStringLiteral("playerEnhancementStatus"));
    check(chip != nullptr && !chip->isHidden() && chip->text().contains(experimentLabel),
          "the reopened info chip names the restored experiment");

    surface->setPaused(true);
    pump(500);
    const QImage restoredOn = settledFrame(surface);
    const QImage restoredOnAgain = settledFrame(surface);
    const double restoredNoise = bandDifference(restoredOn, restoredOnAgain);
    check(!restoredOn.isNull() && restoredNoise >= 0.0 && restoredNoise <= 0.01,
          "the reopened player holds a stable frame with the restored chain");

    // The restored chain is marked where the viewer would look for it. The
    // comparison is against a sibling that shares its level, taken from the
    // catalogue: a level with nothing beside the current one would make any
    // marking vacuous.
    check(openAnimeRoot(shell, dialog, menus), "the reopened menu opens at the Anime4K root");
    check(clickRow(menus.anime, QStringLiteral("anime4k/experimental")),
          "the reopened experimental category opens");
    MenuPopup* experimentPanel = levelPanel(dialog, QStringLiteral("anime4k/experimental"), 0);
    QStringList siblings;
    for (const anime4k::ModeInfo& info : anime4k::availableModes()) {
        if (info.category == QLatin1String("anime4k/experimental") &&
            info.id != QLatin1String(kExperiment)) {
            siblings << info.id;
        }
    }
    QPushButton* marked = rowButton(experimentPanel, QLatin1String(kExperiment));
    QPushButton* sibling =
        siblings.isEmpty() ? nullptr : rowButton(experimentPanel, siblings.first());
    check(marked != nullptr && sibling != nullptr && siblings.size() > 1,
          "the reopened experiment level lists the restored preset beside its siblings");
    check(marked != nullptr && sibling != nullptr && marked->styleSheet() != sibling->styleSheet(),
          "the restored preset is the one marked as current in its menu");

    // The restored chain has to be drawing, and Off has to take it away - both
    // from the same paused position, so the two frames share one decode.
    check(clickLeaf({}, QStringLiteral("off"), shell, dialog, menus),
          "Disabled is selected through the reopened menu");
    check(surface->effectiveShaderChain().isEmpty(),
          "Disabled clears every active shader path");
    pump(500);
    const QImage restoredOff = settledFrame(surface);
    const double restoredEffect = bandDifference(restoredOn, restoredOff);
    const double restoredThreshold = std::max(0.05, restoredNoise * 5.0);
    std::fprintf(stderr,
                 "     restored experiment vs Off: %.3f / 255 "
                 "(floor %.4f, threshold %.4f)\n",
                 restoredEffect, restoredNoise, restoredThreshold);
    check(restoredEffect > restoredThreshold,
          "the restored experiment was really rendering, not just recorded");

    check(closeDialog(dialog), "the reopened dialog closes cleanly");
    settings.clear();
    settings.sync();
    std::fprintf(stderr, "RESULT: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
