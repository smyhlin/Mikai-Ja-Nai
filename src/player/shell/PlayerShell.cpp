#include <utility>
#include "PlayerShell.hpp"

#include "PlayerPopups.hpp"
#include "gui/VideoDropTarget.hpp"
#include "gui/SystemDefaultPlayer.hpp"

#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>
#include <QMap>
#include <QMessageBox>
#include <QWindow>
#include <QPainterPath>
#include <QRegion>
#include <QShowEvent>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include "ShellSliders.hpp"
#include "ShellWidgets.hpp"
#include "VideoSurface.hpp"
#include "native/components/AnimeJanaiCatalogWatcher.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"

namespace mplayer {
namespace shell {

namespace {

void skipForwardGlyph(QPainter& painter, qreal x, qreal y, qreal size, const QColor& color) {
    painter.save();
    painter.setPen(color);
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(qMax(8, qRound(size * 0.48)));
    painter.setFont(font);
    painter.drawText(QRectF(x, y, size, size), Qt::AlignCenter, QStringLiteral("+85"));
    painter.restore();
}

QString stateName(PlayerShell::State state) {
    switch (state) {
        case PlayerShell::State::DefaultFit: return QStringLiteral("DEFAULT_FIT");
        case PlayerShell::State::FullscreenActive: return QStringLiteral("FULLSCREEN_ACTIVE");
        case PlayerShell::State::FullscreenAutoHidden:
            return QStringLiteral("FULLSCREEN_AUTO_HIDDEN");
    }
    return QStringLiteral("DEFAULT_FIT");
}

/// One panel per level of the enhancement tree: the root, the Anime4K level, a
/// category, an experiment family, and the deepest one a preset hangs in.
constexpr int kEnhancementTreeDepth = 6;
constexpr auto kAnimeJanaiRoot = "animejanai";
constexpr auto kAnimeJanaiRife = "animejanai:rife";
constexpr auto kAnimeJanaiSpatial = "animejanai:spatial";
constexpr auto kAnimeJanaiBackend = "animejanai:backend";

QString rifeVersion(const native::AnimeJanaiPreset& preset) {
    return QFileInfo(preset.modelFile).completeBaseName().section(QLatin1Char('_'), 0, 1)
        .remove(QStringLiteral("rife_v"));
}

int rifeMinor(const QString& version) {
    bool ok = false;
    const int minor = version.section(QLatin1Char('.'), 1, 1).toInt(&ok);
    return ok ? minor : -1;
}

int rifeRange(const QString& version) {
    const int minor = rifeMinor(version);
    return minor >= 7 ? (minor - 7) / 5 : -1;
}

QString rifeRangeLabel(int range) {
    const int first = 7 + range * 5;
    return QObject::tr("RIFE v4.%1–v4.%2").arg(first).arg(first + 4);
}
/// Whether `levelId` is a RIFE level whose existence is derived from the
/// catalogue: one version range, or one version. The root, SPATIAL, RIFE and
/// BACKEND levels are structural and always stand, so only these two can go
/// stale when the last pin that named them is removed.
bool isCatalogueRifeLevel(const QString& levelId) {
    const QString rife = QString::fromLatin1(kAnimeJanaiRife);
    return levelId.startsWith(rife + QStringLiteral(":range:")) ||
           levelId.startsWith(rife + QStringLiteral(":version:"));
}

/// Whether at least one catalogue preset still hangs in a RIFE range or
/// version level. A level nobody hangs in is a dead panel.
bool rifeLevelHasPreset(const QString& levelId,
                        const QVector<native::AnimeJanaiPreset>& presets) {
    const QString rife = QString::fromLatin1(kAnimeJanaiRife);
    if (levelId.startsWith(rife + QStringLiteral(":range:"))) {
        const int range = levelId.section(QLatin1Char(':'), -1).toInt();
        for (const native::AnimeJanaiPreset& preset : presets) {
            if (preset.rife && rifeRange(rifeVersion(preset)) == range) {
                return true;
            }
        }
        return false;
    }
    if (levelId.startsWith(rife + QStringLiteral(":version:"))) {
        const QString version = levelId.section(QLatin1Char(':'), -1);
        for (const native::AnimeJanaiPreset& preset : presets) {
            if (preset.rife && rifeVersion(preset) == version) {
                return true;
            }
        }
        return false;
    }
    // Not a catalogue-derived level: nothing to lose.
    return true;
}

QString animeJanaiTitle(const QString& levelId) {
    if (levelId == QLatin1String(kAnimeJanaiRoot)) return QStringLiteral("ANIMEJANAI");
    if (levelId == QLatin1String(kAnimeJanaiSpatial)) return QObject::tr("SPATIAL MODELS");
    if (levelId == QLatin1String(kAnimeJanaiRife)) return QObject::tr("RIFE MODELS");
    if (levelId == QLatin1String(kAnimeJanaiBackend)) return QObject::tr("BACKEND");
    if (levelId.startsWith(QString::fromLatin1(kAnimeJanaiRife) + QStringLiteral(":range:"))) {
        return rifeRangeLabel(levelId.section(QLatin1Char(':'), -1).toInt());
    }
    if (levelId.startsWith(QString::fromLatin1(kAnimeJanaiRife) + QStringLiteral(":version:"))) {
        return QObject::tr("RIFE v%1").arg(levelId.section(QLatin1Char(':'), -1));
    }
    return levelId.section(QLatin1Char(':'), -1).toUpper();
}

QVector<MenuEntry> animeJanaiEntries(const QString& levelId, const QString& currentId) {
    QVector<MenuEntry> entries;
    const QVector<native::AnimeJanaiPreset> presets = native::animeJanaiPresets();
    auto addSubmenu = [&entries](const QString& label, const QString& id) {
        MenuEntry entry;
        entry.text = label + QStringLiteral("  ›");
        entry.payload = QStringLiteral("level|") + id;
        entry.submenu = true;
        entries.append(entry);
    };
    if (levelId == QLatin1String(kAnimeJanaiRoot)) {
        MenuEntry off;
        off.text = QObject::tr("Off");
        off.tooltip = QObject::tr("Disable only AnimeJaNai; Anime4K is unchanged.");
        off.payload = QStringLiteral("ajipreset|");
        off.current = currentId.isEmpty();
        entries.append(off);
        for (const native::AnimeJanaiPreset& preset : presets) {
            if (preset.id.startsWith(QLatin1String("builtin"))) {
                MenuEntry entry;
                entry.text = preset.label;
                entry.tooltip = preset.reason;
                entry.payload = QStringLiteral("ajipreset|") + preset.id;
                entry.current = preset.id == currentId;
                entries.append(entry);
            }
        }
        // A selection whose graph is gone is still the selection: the filter
        // was asked for that id. It is shown as an explicit unavailable row
        // rather than falling back to Off or to a model nobody chose, and the
        // id itself is what the row names, because the catalogue has no label
        // for it any more.
        bool selectionInCatalogue = currentId.isEmpty();
        for (const native::AnimeJanaiPreset& preset : presets) {
            if (preset.id.compare(currentId, Qt::CaseInsensitive) == 0) {
                selectionInCatalogue = true;
                break;
            }
        }
        if (!selectionInCatalogue) {
            MenuEntry missing;
            missing.text = QObject::tr("Unavailable · %1").arg(currentId);
            missing.tooltip = QObject::tr(
                "This model is no longer in the catalogue. The selection is kept until you choose "
                "another.");
            missing.payload = QStringLiteral("ajipreset|") + currentId;
            missing.current = true;
            missing.disabled = true;
            entries.append(missing);
        }
        addSubmenu(QObject::tr("Spatial models"), QString::fromLatin1(kAnimeJanaiSpatial));
        addSubmenu(QObject::tr("RIFE models"), QString::fromLatin1(kAnimeJanaiRife));
        addSubmenu(QObject::tr("Backend"), QString::fromLatin1(kAnimeJanaiBackend));
    } else if (levelId == QLatin1String(kAnimeJanaiSpatial)) {
        for (const native::AnimeJanaiPreset& preset : presets) {
            if (!preset.id.startsWith(QLatin1String("spatial:")) || preset.rife) {
                continue;
            }
            MenuEntry entry;
            entry.text = preset.label;
            entry.tooltip = preset.supported ? preset.reason
                                            : QObject::tr("Unavailable: %1").arg(preset.reason);
            entry.payload = QStringLiteral("ajipreset|") + preset.id;
            entry.current = preset.id == currentId;
            entry.disabled = !preset.supported;
            entries.append(entry);
        }
    } else if (levelId == QLatin1String(kAnimeJanaiRife)) {
        QMap<int, bool> ranges;
        for (const native::AnimeJanaiPreset& preset : presets) {
            if (preset.rife) ranges.insert(rifeRange(rifeVersion(preset)), true);
        }
        for (int range : ranges.keys()) {
            if (range >= 0) {
                addSubmenu(rifeRangeLabel(range),
                           QString::fromLatin1(kAnimeJanaiRife) +
                               QStringLiteral(":range:") + QString::number(range));
            }
        }
    } else if (levelId.startsWith(
                   QString::fromLatin1(kAnimeJanaiRife) + QStringLiteral(":range:"))) {
        const int range = levelId.section(QLatin1Char(':'), -1).toInt();
        QMap<int, QString> versions;
        for (const native::AnimeJanaiPreset& preset : presets) {
            const QString version = rifeVersion(preset);
            if (preset.rife && rifeRange(version) == range) {
                versions.insert(rifeMinor(version), version);
            }
        }
        for (const QString& version : versions.values()) {
            addSubmenu(QObject::tr("RIFE v%1").arg(version),
                       QString::fromLatin1(kAnimeJanaiRife) +
                           QStringLiteral(":version:") + version);
        }
    } else if (levelId.startsWith(
                   QString::fromLatin1(kAnimeJanaiRife) + QStringLiteral(":version:"))) {
        const QString version = levelId.section(QLatin1Char(':'), -1);
        for (const native::AnimeJanaiPreset& preset : presets) {
            if (!preset.rife || rifeVersion(preset) != version) continue;
            MenuEntry entry;
            entry.text = preset.label;
            entry.tooltip = preset.supported ? preset.reason
                                            : QObject::tr("Unavailable: %1").arg(preset.reason);
            entry.payload = QStringLiteral("ajipreset|") + preset.id;
            entry.current = preset.id == currentId;
            entry.disabled = !preset.supported;
            entries.append(entry);
        }
    } else if (levelId == QLatin1String(kAnimeJanaiBackend)) {
        for (const auto& item : {std::make_pair(native::Backend::TensorRT, QObject::tr("TensorRT")),
                                 std::make_pair(native::Backend::DirectML, QObject::tr("DirectML"))}) {
            MenuEntry entry;
            entry.text = item.second;
            const QString backend = item.first == native::Backend::TensorRT
                                        ? QStringLiteral("tensorrt")
                                        : QStringLiteral("directml");
            entry.payload = QStringLiteral("ajbackend|") + backend;
            entry.current = backend == currentId;
            entries.append(entry);
        }
    }
    return entries;
}

/// Whether a panel already shows exactly these rows. A rebuild retires every
/// row widget, so a refresh that finds nothing new must not do it: the rows
/// under the pointer, the keyboard highlight and the widget focus are state
/// that a catalogue event nobody can see has no reason to reset.
bool sameEntries(const QVector<MenuEntry>& entries, const MenuPopup* panel) {
    if (panel == nullptr) {
        return false;
    }
    int spot = 0;
    for (const MenuEntry& next : entries) {
        if (!next.heading.isEmpty()) {
            continue;
        }
        if (spot >= panel->rowCount()) {
            return false;
        }
        const MenuEntry shown = panel->entryAt(spot);
        if (next.text != shown.text || next.tooltip != shown.tooltip ||
            next.payload != shown.payload || next.current != shown.current ||
            next.submenu != shown.submenu || next.disabled != shown.disabled) {
            return false;
        }
        ++spot;
    }
    return spot == panel->rowCount();
}

QVector<MenuEntry> enhancementEntries(const QString& levelId, const QString& currentId,
                                      const native::menu::Inputs& native) {
    QVector<MenuEntry> entries;
    if (levelId == QLatin1String(kAnimeJanaiRoot) ||
        levelId == QLatin1String(kAnimeJanaiSpatial) ||
        levelId == QLatin1String(kAnimeJanaiRife) ||
        levelId == QLatin1String(kAnimeJanaiBackend) ||
        levelId.startsWith(QLatin1String(kAnimeJanaiRife) + QStringLiteral(":"))) {
        return animeJanaiEntries(levelId, currentId);
    }
    const native::RuntimeSelection& runtime = native::currentSelection();
    const bool privateNativeRuntime =
        runtime.loaded && runtime.kind == native::BundleKind::Native;
    if (privateNativeRuntime && native::menu::ownsLevel(levelId)) {
        for (const native::menu::Row& row : native::menu::levelRows(levelId, native)) {
            MenuEntry entry;
            entry.text = row.label;
            entry.tooltip = row.tooltip;
            entry.disabled = row.disabled;
            entry.submenu = row.submenu;
            entry.current = row.current;
            if (row.action != native::menu::Action::None) {
                // The action and its value are spelled into the payload, never
                // taken from the row index: a reordered branch must resolve to
                // the same request.
                entry.payload = QStringLiteral("native|") +
                                native::menu::actionName(row.action) + QLatin1Char('|') +
                                row.value;
            }
            entries.append(entry);
        }
        return entries;
    }
    for (const anime4k::MenuRow& row : anime4k::menuRows(levelId)) {
        MenuEntry entry;
        entry.text = row.label;
        entry.tooltip = row.tooltip;
        switch (row.role) {
            case anime4k::MenuRow::Role::Preset:
                entry.payload = QStringLiteral("preset|") + row.id;
                entry.current = row.id == currentId;
                break;
            case anime4k::MenuRow::Role::Category:
                // The chevron is a promise that something opens: it is only
                // added for a category that actually has children.
                if (row.hasChildren) {
                    entry.text = row.label + QStringLiteral("  ›");
                }
                entry.payload = QStringLiteral("level|") + row.id;
                entry.submenu = row.hasChildren;
                entry.current = currentId != QLatin1String("off") &&
                                anime4k::categoryContainsPreset(row.id, currentId);
                break;
        }
        entries.append(entry);
    }
    if (levelId.isEmpty()) {
        MenuEntry aj;
        aj.text = QObject::tr("AnimeJaNai  ›");
        aj.tooltip = QObject::tr("AnimeJaNai official filter");
        aj.payload = QStringLiteral("level|") + QString::fromLatin1(kAnimeJanaiRoot);
        aj.submenu = true;
        entries.append(aj);
        if (privateNativeRuntime) {
            for (const native::menu::Row& row : native::menu::rootRows(native)) {
                MenuEntry entry;
                entry.text = row.label + (row.submenu ? QStringLiteral("  ›") : QString());
                entry.tooltip = row.tooltip;
                entry.disabled = row.disabled;
                entry.submenu = row.submenu;
                entry.current = row.current;
                entry.payload = QStringLiteral("level|") + row.value;
                entries.append(entry);
            }
        }
    }
    return entries;
}

}  // namespace

PlayerShell::PlayerShell(QWidget* parent, bool frameless) : QWidget(parent) {
    setWindowTitle(tr("Media Lightbox"));
    setMinimumSize(kMinWindowWidth, kMinWindowHeight);
    if (parent != nullptr) {
        resize(parent->size());
    } else {
        resize(1280, 760);
    }
    setMouseTracking(true);
    setAutoFillBackground(false);

    // The native branch is built before anyone pushes real inputs at it, so its
    // starting state has to be the truth of this build: no engine, and no
    // status snapshot read yet. Nothing here claims a capability. No core exists
    // yet, so no factory observation is passed - the owning player supplies the
    // per-core ledger later through setNativeMenuInputs().
    nativeInputs_.availability = native::currentNativeAvailability(nullptr);
    nativeInputs_.status.availability = native::unavailableAvailability(
        QStringLiteral("native.statusAbsent"), tr("No native status has been read."));

    frameless_ = frameless && parent == nullptr;
    if (frameless_) {
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    } else {
        setWindowFlags(Qt::Widget);
    }

    quality_ = QStringLiteral("HD");
    origin_ = QStringLiteral("UDream");

    hideTimer_.setSingleShot(true);
    connect(&hideTimer_, &QTimer::timeout, this, &PlayerShell::enterAutoHidden);
    enhancementHoverTimer_.setSingleShot(true);
    connect(&enhancementHoverTimer_, &QTimer::timeout, this, [this] {
        MenuPopup* panel = pendingEnhancementHoverPanel_.data();
        QWidget* row = pendingEnhancementHoverRow_.data();
        const int spot = pendingEnhancementHoverSpot_;
        cancelEnhancementHover();
        if (panel == nullptr || row == nullptr || !panel->isVisible() ||
            !row->isVisible() || spot < 0 || spot >= panel->rowCount() ||
            panel->rowWidget(spot) != row) {
            return;
        }
        const QRect rowGlobal(row->mapToGlobal(QPoint(0, 0)), row->size());
        if (!rowGlobal.contains(QCursor::pos())) {
            return;
        }
        onEnhancementRow(panel, spot, false);
    });

    buildSurface();
    buildWindowControls();
    buildTitleBar();
    buildChevrons();
    buildDock();
    buildFooter();
    buildOverlays();
    buildPictureMenu();
    // The catalogue the enhancement tree draws from is observed for as long as
    // the shell lives: a graph pasted, registered or removed while the player is
    // open must reach the menu without a reopen. Lifetime-owned, and it starts
    // from the saved config, which is the catalogue this menu shows.
    animeJanaiCatalogWatcher_ = new native::AnimeJanaiCatalogWatcher(this);
    connect(animeJanaiCatalogWatcher_, &native::AnimeJanaiCatalogWatcher::catalogChanged,
            this, &PlayerShell::onAnimeJanaiCatalogueChanged);
    // Not z-order over the picture any more, but the child order ChromeList
    // walks - stated once, because a raise() per layout pass is a child-order
    // mutation that invalidates the region it covers.
    for (QWidget* widget : {static_cast<QWidget*>(windowControls_), static_cast<QWidget*>(titleBar_),
                            static_cast<QWidget*>(btnPrev_), static_cast<QWidget*>(btnNext_),
                            static_cast<QWidget*>(dock_), static_cast<QWidget*>(footerLeft_),
                            static_cast<QWidget*>(center_)}) {
        widget->raise();
    }

    // Adopts the whole subtree - the popovers skip themselves - and records it
    // in paint order, so a frame walks that list.
    chrome_.adopt(this, this);

    applyState();
    layoutChrome();

    // The application filter can dismiss popups and schedule either paint
    // device, so install it only after all chrome and overlays exist.
    videoDrop_ = new mikai::gui::VideoDropTarget(
        this, mikai::gui::VideoDropTarget::OverlayMode::HostPainted);
    connect(videoDrop_, &mikai::gui::VideoDropTarget::videoDropped,
            this, &PlayerShell::mediaDropped);
    connect(videoDrop_, &mikai::gui::VideoDropTarget::indicatorChanged, this, [this] {
        hideTimer_.stop();
        if (videoDrop_->active()) {
            dismissOverlay();
        } else if (isVisible()) {
            hideTimer_.start(kAutohideMs);
        }
        chromeDirtyAll();
    });
}

PlayerShell::~PlayerShell() {
    delete std::exchange(videoDrop_, nullptr);
    if (surface_ != nullptr) {
        disconnect(surface_, &VideoSurface::paintOverlay, this, &PlayerShell::paintSurfaceOverlay);
        surface_->removeEventFilter(this);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  BUILDERS
// ═════════════════════════════════════════════════════════════════════════════

void PlayerShell::buildSurface() {
    surface_ = new VideoSurface(this);
    surface_->setObjectName(QStringLiteral("videoSurface"));
    surface_->setMouseTracking(true);
    surface_->setFocusPolicy(Qt::NoFocus);
    // Input over the picture belongs to the shell, not the surface: a click
    // toggles playback and a drag is not a click. The Python needed a
    // _ReposedEvent shim for this because the surface was a foreign native
    // window; here it is an ordinary child, so an event filter is enough.
    surface_->installEventFilter(this);
    // Every paint, not just the ones carrying a new frame: the chrome survives
    // a resize, a re-show and a paused repaint. Direct - the painter is only
    // valid for the length of the call.
    connect(surface_, &VideoSurface::paintOverlay, this, &PlayerShell::paintSurfaceOverlay,
            Qt::DirectConnection);
}

void PlayerShell::buildWindowControls() {
    windowControls_ = new Panel(blackSoft(), 8.0, w15(), this);
    auto* lay = new QHBoxLayout(windowControls_);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(2);

    btnMin_ = new IconButton(IconPainter::minimize, tr("Minimize the window"), 28, 15,
                             windowControls_);
    connect(btnMin_, &QPushButton::clicked, this, &QWidget::showMinimized);
    lay->addWidget(btnMin_);

    btnClose_ = new IconButton(IconPainter::close, tr("Exit the player  (Esc)"), 28, 15,
                               windowControls_, QColor(237, 66, 69, 190));
    connect(btnClose_, &QPushButton::clicked, this, &PlayerShell::exitViewer);
    lay->addWidget(btnClose_);
}

void PlayerShell::buildTitleBar() {
    // A frameless window has no OS title bar, so a windowed player had no
    // way to be moved: every pixel belonged either to the picture or to a
    // control. This is the grab handle that a decorated window would give.
    titleBar_ = new Panel(blackSoft(), 0.0, w15(), this);
    titleBar_->setObjectName(QStringLiteral("playerTitleBar"));
    titleBar_->setCursor(Qt::OpenHandCursor);
    auto* lay = new QHBoxLayout(titleBar_);
    lay->setContentsMargins(12, 0, 6, 0);
    lay->setSpacing(8);

    auto* caption = new ChromeLabel(QStringLiteral("MIKAI"), QFont(), w70(), titleBar_);
    caption->setObjectName(QStringLiteral("playerTitleCaption"));
    lay->addWidget(caption);
    lay->addStretch(1);

    titleMin_ = new IconButton(IconPainter::minimize, tr("Minimize the window"), 28, 15,
                               titleBar_, w70());
    connect(titleMin_, &QPushButton::clicked, this, &QWidget::showMinimized);
    lay->addWidget(titleMin_);

    titleMax_ = new IconButton(IconPainter::restore, tr("Toggle fullscreen / windowed"), 28, 15,
                               titleBar_, w70());
    connect(titleMax_, &QPushButton::clicked, this, &PlayerShell::windowModeToggled);
    lay->addWidget(titleMax_);

    titleClose_ = new IconButton(IconPainter::close, tr("Exit the player  (Esc)"), 28, 15,
                                 titleBar_, w70());
    connect(titleClose_, &QPushButton::clicked, this, &PlayerShell::exitViewer);
    lay->addWidget(titleClose_);
}
void PlayerShell::buildChevrons() {
    btnPrev_ = new IconButton(IconPainter::chevronLeft, tr("Previous episode  (←)"),
                              42, 18, this, QColor(0, 0, 0, 165));
    btnPrev_->setTint(w70());
    connect(btnPrev_, &QPushButton::clicked, this, &PlayerShell::prevRequested);

    btnNext_ = new IconButton(IconPainter::chevronRight, tr("Next episode  (→)"),
                              42, 18, this, QColor(0, 0, 0, 165));
    btnNext_->setTint(w70());
    connect(btnNext_, &QPushButton::clicked, this, &PlayerShell::nextRequested);
}

void PlayerShell::buildDock() {
    dock_ = new Panel(dock(), 16.0, w15(), this);
    auto* dockLayout = new QVBoxLayout(dock_);
    dockLayout->setContentsMargins(16, 8, 16, 8);
    dockLayout->setSpacing(3);

    auto* topRow = new QHBoxLayout();
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(4);

    // A layout host, not a surface: nothing of it is painted, so the dock's own
    // fill is what shows through over the picture.
    auto* volumeHost = new QWidget(dock_);
    volumeHost->setFixedWidth(kDockSideWidth);
    volumeHost->setAttribute(Qt::WA_StyledBackground, false);
    volumeHost->setAttribute(Qt::WA_TranslucentBackground, true);
    auto* volumeLayout = new QHBoxLayout(volumeHost);
    volumeLayout->setContentsMargins(0, 0, 0, 0);
    vol_ = new VolumeGroup(volumeHost);
    vol_->setObjectName(QStringLiteral("playerVolumeGroup"));
    vol_->setAccessibleName(tr("Volume"));
    connect(vol_, &VolumeGroup::volumeChanged, this, &PlayerShell::onVolume);
    connect(vol_, &VolumeGroup::muteClicked, this, &PlayerShell::toggleMute);
    volumeLayout->addWidget(vol_);
    volumeLayout->addStretch();
    topRow->addWidget(volumeHost);

    topRow->addStretch();
    btnPlay_ = new IconButton(IconPainter::play, tr("Play / pause  (Space)"),
                              kDockPlaySize, kDockPlayGlyph, dock_);
    btnPlay_->setObjectName(QStringLiteral("playerPlayToggle"));
    // Checkable so `checked` reports playback state to assistive technology and
    // to a probe by identity; the engine rewrites it on every state change, so
    // the auto-toggle a click performs is never left standing.
    btnPlay_->setCheckable(true);
    btnPlay_->setAccessibleName(tr("Play"));
    connect(btnPlay_, &QPushButton::clicked, this, &PlayerShell::onPlayClicked);
    topRow->addWidget(btnPlay_);
    btnSkipForward_ = new IconButton(skipForwardGlyph, tr("Skip forward 85 seconds"),
                                     kDockActionSize, kDockActionSize - 8, dock_);
    btnSkipForward_->setObjectName(QStringLiteral("playerSkipForward85"));
    btnSkipForward_->setAccessibleName(tr("Skip forward 85 seconds"));
    btnSkipForward_->setToolTip(tr("Skip forward 85 seconds"));
    btnSkipForward_->setEnabled(false);
    connect(btnSkipForward_, &QPushButton::clicked, this, &PlayerShell::onSkipForward);
    topRow->addWidget(btnSkipForward_);
    topRow->addStretch();

    auto* actionHost = new QWidget(dock_);
    actionHost->setFixedWidth(kDockSideWidth);
    actionHost->setAttribute(Qt::WA_StyledBackground, false);
    actionHost->setAttribute(Qt::WA_TranslucentBackground, true);
    auto* actions = new QHBoxLayout(actionHost);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(4);
    actions->setAlignment(Qt::AlignRight);

    btnPip_ = new IconButton(IconPainter::pip, tr("Picture in picture"),
                             kDockActionSize, kDockActionGlyph, actionHost);
    connect(btnPip_, &QPushButton::clicked, this, &PlayerShell::pipRequested);
    actions->addWidget(btnPip_);

    btnWindowMode_ = new IconButton(IconPainter::contract, tr("Windowed mode"),
                                    kDockActionSize, kDockActionGlyph, actionHost);
    connect(btnWindowMode_, &QPushButton::clicked, this, &PlayerShell::windowModeToggled);
    actions->addWidget(btnWindowMode_);

    btnAudio_ = new IconButton(IconPainter::microphone,
                               tr("Audio — choose a track  (T)"),
                               kDockActionSize, kDockActionGlyph, actionHost);
    btnAudio_->setObjectName(QStringLiteral("playerAudioButton"));
    connect(btnAudio_, &QPushButton::clicked, this, &PlayerShell::audioRequested);
    btnAudio_->hide();
    actions->addWidget(btnAudio_);

    btnSubs_ = new IconButton(IconPainter::captions,
                              tr("Subtitles — choose a track  (C)"),
                              kDockActionSize, kDockActionGlyph, actionHost);
    btnSubs_->setObjectName(QStringLiteral("playerSubtitlesButton"));
    connect(btnSubs_, &QPushButton::clicked, this, &PlayerShell::subtitlesRequested);
    btnSubs_->hide();
    actions->addWidget(btnSubs_);

    btnEnhance_ = new IconButton(IconPainter::spark, tr("Open enhancements"),
                                 kDockActionSize, kDockActionGlyph, actionHost);
    btnEnhance_->setObjectName(QStringLiteral("playerEnhanceButton"));
    connect(btnEnhance_, &QPushButton::clicked, this, &PlayerShell::openEnhancements);
    actions->addWidget(btnEnhance_);
    btnSettings_ = new IconButton(IconPainter::gear, tr("Quality and speed"),
                                  kDockActionSize, kDockActionGlyph, actionHost);
    btnSettings_->setObjectName(QStringLiteral("playerSettingsButton"));
    connect(btnSettings_, &QPushButton::clicked, this, &PlayerShell::openSettings);
    actions->addWidget(btnSettings_);
    topRow->addWidget(actionHost);
    dockLayout->addLayout(topRow);

    auto* timeline = new QHBoxLayout();
    timeline->setContentsMargins(0, 0, 0, 0);
    timeline->setSpacing(8);
    lblElapsed_ = new ChromeLabel(QStringLiteral("00:00"), monoFont(9), white(230), dock_);
    lblElapsed_->setFixedWidth(38);
    timeline->addWidget(lblElapsed_);

    scrub_ = new ScrubBar(dock_);
    scrub_->setObjectName(QStringLiteral("playerScrubBar"));
    scrub_->setAccessibleName(tr("Timeline"));
    scrub_->setMinimumWidth(120);
    connect(scrub_, &RangeSlider::valueChanged, this, &PlayerShell::onScrub);
    timeline->addWidget(scrub_, 1);

    lblRemaining_ = new ChromeLabel(QStringLiteral("-00:00"), monoFont(9), white(140), dock_);
    lblRemaining_->setFixedWidth(42);
    timeline->addWidget(lblRemaining_);
    dockLayout->addLayout(timeline);

    QWidget* overlayHost = parentWidget() != nullptr ? parentWidget() : this;
    flyout_ = new SettingsFlyout(overlayHost);
    connect(flyout_, &SettingsFlyout::speedSelected, this, &PlayerShell::onRate);
    connect(flyout_, &SettingsFlyout::qualitySelected, this, &PlayerShell::qualityRequested);
    connect(flyout_, &SettingsFlyout::defaultPlayerRequested, this, [this] {
        QString error;
        if (!mikai::gui::requestSystemDefaultPlayer(&error)) {
            auto* message = new QMessageBox(QMessageBox::Warning,
                tr("Default video player"), error, QMessageBox::Ok, this);
            message->setTextFormat(Qt::PlainText);
            message->setAttribute(Qt::WA_DeleteOnClose);
            message->open();
            return;
        }
        toast_->flash(tr("Choose Mikai in Windows Default Apps. Your defaults have not been changed."));
    });

    // The enhancement tree is one panel per level, standing side by side:
    // the root is ENHANCEMENTS, and every panel after it is the submenu of a row
    // in the panel before. They are pooled rather than built per level, because
    // a level is reopened every time the pointer crosses the row that leads to
    // it - and a hidden panel is a panel that is not on screen, which is the
    // whole of the tree's state.
    for (int depth = 0; depth < kEnhancementTreeDepth; ++depth) {
        const bool root = depth == 0;
        auto* panel = new MenuPopup(root ? tr("ENHANCEMENTS") : QString(), overlayHost);
        panel->setObjectName(root ? QStringLiteral("playerEnhancementsPopup")
                                 : QStringLiteral("playerEnhancementLevelPopup"));
        connect(panel, &MenuPopup::picked, this, [this, panel](const QVariant& payload) {
            cancelEnhancementHover();
            // A click names a row; the panel it came from is what says which
            // level of the tree that row belongs to, which the payload alone
            // cannot - the same preset id is reachable from the panel it hangs
            // in and from nowhere else.
            for (int spot = 0; spot < panel->rowCount(); ++spot) {
                if (panel->entryAt(spot).payload == payload) {
                    onEnhancementRow(panel, spot, true);
                    return;
                }
            }
        });
        enhancementPanels_.append(panel);
        enhancementLevels_.append(QString());
        registerOverlay(panel);
    }
    // The enhancement tree also contains AnimeJaNai, which is independent of
    // the local Anime4K shader directory.
    btnEnhance_->setVisible(true);

    registerOverlay(flyout_);
}

void PlayerShell::buildFooter() {
    footerLeft_ = new Panel(pill(), 16.0, w15(), this);
    footerLeft_->setObjectName(QStringLiteral("playerInfoChip"));
    auto* info = new QHBoxLayout(footerLeft_);
    info->setContentsMargins(12, 5, 12, 5);
    info->setSpacing(8);

    lblEpisode_ = new ChromeLabel(
        episodeLabel_.isEmpty() ? QStringLiteral("MIKAI") : episodeLabel_, monoFont(9, true),
        white(255), footerLeft_);
    lblEpisode_->setObjectName(QStringLiteral("playerEpisodeLabel"));
    info->addWidget(lblEpisode_);

    auto* divider = new ChromeLabel(QStringLiteral("│"), QFont(), white(70), footerLeft_);
    info->addWidget(divider);

    lblNetworkStats_ = new ChromeLabel(QStringLiteral("0.00 Mbps × — FPS"), monoFont(8),
                                       white(185), footerLeft_);
    lblNetworkStats_->setObjectName(QStringLiteral("playerNetworkStats"));
    lblNetworkStats_->setToolTip(tr("FPS unknown"));
    lblNetworkStats_->setAccessibleDescription(tr("FPS unknown"));
    info->addWidget(lblNetworkStats_);

    lblEnhancement_ = new ChromeLabel(QString(), monoFont(8), QColor(QStringLiteral("#79dcf2")),
                                      footerLeft_);
    lblEnhancement_->setObjectName(QStringLiteral("playerEnhancementStatus"));
    lblEnhancement_->hide();
    info->addWidget(lblEnhancement_);

    // The native status chip. It is a sibling of the Anime4K chip rather than a
    // replacement: the shader chain and the native pipeline are separate, and
    // one must never be read as the other. It is shown only for the loaded
    // Native runtime, when processing is requested or the filter reports state.
    lblNative_ = new ChromeLabel(QString(), monoFont(8), QColor(QStringLiteral("#ffcf7a")),
                                 footerLeft_);
    lblNative_->setObjectName(QStringLiteral("playerNativeStatus"));
    lblNative_->hide();
    info->addWidget(lblNative_);
    lblAnimeJanai_ = new ChromeLabel(QString(), monoFont(8), QColor(QStringLiteral("#79dcf2")),
                                     footerLeft_);
    lblAnimeJanai_->setObjectName(QStringLiteral("playerAnimeJanaiStatus"));
    lblAnimeJanai_->hide();
    info->addWidget(lblAnimeJanai_);
    footerLeft_->setFixedHeight(kInfoChipHeight);

    QWidget* overlayHost = parentWidget() != nullptr ? parentWidget() : this;
    keys_ = new KeyHints(overlayHost);
    registerOverlay(keys_);
}

void PlayerShell::buildPictureMenu() {
    // Built once and reused: a menu rebuilt on every right-click allocates a
    // popover and its rows each time, and this one is opened often.
    QWidget* overlayHost = parentWidget() != nullptr ? parentWidget() : this;
    pictureMenu_ = new MenuPopup(QString(), overlayHost);
    pictureMenu_->setObjectName(QStringLiteral("playerPictureMenu"));
    pictureMenu_->hide();
    registerOverlay(pictureMenu_);
    connect(pictureMenu_, &MenuPopup::picked, this, [this](const QVariant& payload) {
        pictureMenu_->close();
        const QString action = payload.toString();
        if (action == QStringLiteral("frame")) {
            emit frameRequested();
        } else if (action == QStringLiteral("compare")) {
            emit compareRequested();
        } else if (action == QStringLiteral("copy")) {
            emit copyLinkRequested();
        } else if (action == QStringLiteral("folder")) {
            emit openFolderRequested();
        } else if (action == QStringLiteral("information")) {
            emit mediaInformationRequested();
        } else if (action == QStringLiteral("keys")) {
            openKeys();
        } else if (action == QStringLiteral("exit")) {
            exitViewer();
        }
    });
    updatePictureMenu();
}

void PlayerShell::updatePictureMenu() {
    if (pictureMenu_ == nullptr) {
        return;
    }
    QVector<MenuEntry> entries;
    const auto add = [&entries](const QString& text, const QString& action,
                               const QString& tooltip = QString()) {
        MenuEntry entry;
        entry.text = text;
        entry.payload = action;
        entry.tooltip = tooltip;
        entries.append(entry);
    };
    add(tr("Copy frame"), QStringLiteral("frame"));
    add(tr("Compare quality"), QStringLiteral("compare"));
    add(tr("Copy link"), QStringLiteral("copy"));
    const QString folder = localMediaFolder();
    if (!folder.isEmpty()) {
        add(tr("Open containing folder"), QStringLiteral("folder"), folder);
    }
    if (!path_.isEmpty() && (QDir::isAbsolutePath(path_) ||
                             QUrl::fromUserInput(path_).isLocalFile())) {
        add(tr("Media information"), QStringLiteral("information"));
    }
    add(tr("Keyboard shortcuts  (?)"), QStringLiteral("keys"));
    add(tr("Exit  (Esc)"), QStringLiteral("exit"));
    pictureMenu_->setEntries(entries);
}

QString PlayerShell::localMediaFolder() const {
    if (path_.isEmpty()) {
        return QString();
    }
    QString local = path_;
    // Native filenames can contain '#', which URL parsing treats as a fragment.
    if (!QDir::isAbsolutePath(local)) {
        const QUrl url = QUrl::fromUserInput(local);
        if (!url.isLocalFile()) {
            return QString();
        }
        local = url.toLocalFile();
    }
    const QFileInfo info(local);
    if (!info.isFile()) {
        return QString();
    }
    const QString folder = info.absolutePath();
    return QFileInfo(folder).isDir() ? folder : QString();
}

void PlayerShell::openPictureMenu(const QPoint& position) {
    if (pictureMenu_ == nullptr) {
        return;
    }
    pictureMenuAnchor_ = position;
    pictureMenu_->popupNear(mapToGlobal(position));
}
void PlayerShell::buildOverlays() {
    toast_ = new Toast(this);
    center_ = new CenterGlyph(this);
    center_->setKind(CenterGlyph::Kind::None);
}

// ═════════════════════════════════════════════════════════════════════════════
//  STATE MACHINE
// ═════════════════════════════════════════════════════════════════════════════

PlayerShell::State PlayerShell::state() const {
    if (hudHidden_) return State::FullscreenAutoHidden;
    if (fullscreen_) return State::FullscreenActive;
    return State::DefaultFit;
}

QString PlayerShell::stateName() const { return ::mplayer::shell::stateName(state()); }

void PlayerShell::applyState() {
    const State st = state();
    const QString name = ::mplayer::shell::stateName(st);
    const bool hidden = hudHidden_;

    // Fullscreen does NOT suppress the chrome. The shell is hosted in a dialog
    // that is itself fullscreen, so gating on "not fullscreen" meant half the
    // UI could never appear. Idle auto-hide is what hides them.
    //
    // The painted min/max/close are a substitute for a missing OS frame, so
    // they exist only where there is no frame: over a full monitor, or in a
    // standalone frameless window. Inside the hosted decorated window the real
    // title bar owns those actions, and showing both gives the viewer two
    // close buttons.
    windowControls_->setVisible(!hidden && (fullscreen_ || frameless_));
    titleBar_->setVisible(!hidden && !fullscreen_ && frameless_);
    // The glyph states the action the press would take, so the dock never
    // offers a mode the player is already in.
    if (btnWindowMode_ != nullptr) {
        btnWindowMode_->setIcon(fullscreen_ ? IconPainter::contract : IconPainter::expand);
        btnWindowMode_->setToolTip(fullscreen_ ? tr("Windowed mode")
                                               : tr("Fullscreen mode"));
    }
    btnPrev_->setVisible(!hidden && hasPreviousEpisode_);
    btnNext_->setVisible(!hidden && hasNextEpisode_);
    dock_->setVisible(!hidden);
    footerLeft_->setVisible(!hidden);
    if (center_ != nullptr) {
        center_->setKind((hidden || playing_ || !hasMedia_) ? CenterGlyph::Kind::None
                                                            : CenterGlyph::Kind::Play);
    }
    if (hidden) {
        dismissOverlay();
    }

    setStateCursor();


    // QOpenGLWidget ignores stylesheets, so corner rounding is a real mask.
    const int radius = (rounded_ && !fullscreen_ && frameless_) ? 12 : 0;
    if (radius > 0) {
        QPainterPath path;
        path.addRoundedRect(QRectF(surface_->geometry()), radius, radius);
        surface_->setMask(QRegion(path.toFillPolygon().toPolygon()));
        hasMask_ = true;
    } else if (hasMask_) {
        // Tracked rather than asked of the widget: QWidget::mask() hands back a
        // QRegion by value, and this branch runs on every input event.
        surface_->clearMask();
        hasMask_ = false;
    }

    if (name != lastState_) {
        lastState_ = name;
        emit stateChanged(name);
    }
    layoutChrome();
    updateEmptyState();
    update();
}

void PlayerShell::updateEmptyState() {
    const bool shown = !hasMedia_ && !hudHidden_;
    if (shown == emptyStateShown_) {
        return;
    }
    emptyStateShown_ = shown;
    // The hint is painted, not a widget - and with no core running there may be
    // no other frame to carry the change.
    chromeDirtyAll();
}

void PlayerShell::setStateCursor() {
    setCursor(hudHidden_ ? Qt::BlankCursor : Qt::ArrowCursor);
}

// ── fullscreen / autohide ──────────────────────────────────────────────

// The host owns the window, so these setters sync composition and emit
// nothing; windowModeToggled() is the only place an intent leaves the shell.
void PlayerShell::enterFullscreen() {
    fullscreen_ = true;
    hudHidden_ = false;
    applyState();
    hideTimer_.start(kAutohideMs);
}

void PlayerShell::leaveFullscreen() {
    fullscreen_ = false;
    hudHidden_ = false;
    applyState();
    hideTimer_.start(kAutohideMs);
}

void PlayerShell::windowModeToggled() {
    const bool desired = !fullscreen_;
    if (desired) {
        enterFullscreen();
    } else {
        leaveFullscreen();
    }
    emit fullscreenChanged(desired);
}

void PlayerShell::registerOverlay(QWidget* overlay) {
    if (overlay == nullptr || overlays_.contains(QPointer<QWidget>(overlay))) {
        return;
    }
    overlays_.append(overlay);
}

bool PlayerShell::dismissOverlay() {
    cancelEnhancementHover();
    bool dismissed = false;
    for (const QPointer<QWidget>& overlay : overlays_) {
        if (overlay != nullptr && overlay->isVisible()) {
            overlay->close();
            dismissed = true;
        }
    }
    return dismissed;
}

bool PlayerShell::hasVisibleOverlay() const {
    for (const QPointer<QWidget>& overlay : overlays_) {
        if (overlay != nullptr && overlay->isVisible()) {
            return true;
        }
    }
    return false;
}

void PlayerShell::enterAutoHidden() {
    // T-317: chrome auto-hide is a fullscreen affordance. In windowed mode the
    // viewer still has window chrome and a taskbar entry to see the player by,
    // and an auto-hidden HUD would also misreport FULLSCREEN_AUTO_HIDDEN to
    // every consumer that gates on the state name (strip and drawer hiding),
    // so the request is refused instead of honored wrongly.
    if (!fullscreen_) return;
    // Never yank the chrome away mid-gesture: an open overlay or a live drag
    // means the user is interacting even though no move crossed the shell.
    if ((videoDrop_ != nullptr && videoDrop_->active()) || hasVisibleOverlay()) {
        hideTimer_.start(kAutohideMs);
        return;
    }
    if (scrub_->isDragging() || vol_->isDragging() || resizingLeft_ || resizingTop_ ||
        resizingRight_ || resizingBottom_) {
        hideTimer_.start(kAutohideMs);
        return;
    }
    hudHidden_ = true;
    applyState();
}

void PlayerShell::wake() {
    if (hudHidden_) {
        hudHidden_ = false;
        applyState();
    }
    hideTimer_.start(kAutohideMs);
}

// ═════════════════════════════════════════════════════════════════════════════
//  LAYOUT
// ═════════════════════════════════════════════════════════════════════════════

QSize PlayerShell::fitted() const {
    if (fullscreen_) {
        // Fullscreen is the whole monitor: the surface takes the window edge to
        // edge, no letterboxing, no design margin, no aspect bars.
        return {width(), height()};
    }
    const int pad = kFrameMargin;
    const int chromeHeight = 150 + titleBarOffset_;
    const int vw = std::max(160, width() - 2 * (pad + 24));
    const int vh = std::max(100, height() - chromeHeight);
    if (static_cast<double>(vw) / vh > aspect_) {
        return {static_cast<int>(vh * aspect_), vh};
    }
    return {vw, static_cast<int>(vw / aspect_)};
}

void PlayerShell::layoutChrome() {
    const int w = width();
    const int h = height();

    // ── canvas ────────────────────────────────────────────────────
    const QSize fit = fitted();
    const int sw = fit.width();
    const int sh = fit.height();
    const int sx = (w - sw) / 2;
    const int sy = titleBarOffset_ + (h - titleBarOffset_ - sh) / 2;
    // Guarded: a geometry write on a QOpenGLWidget schedules a repaint, and
    // layoutChrome runs from applyState, which runs on every input event. The
    // common case is a layout pass that computes the rectangle it already has.
    const QRect canvas(sx, sy, sw, sh);
    if (surface_->geometry() != canvas) {
        surface_->setGeometry(canvas);
    }
    // devicePixelRatioF walks widget -> window -> screen, and both uses below
    // want the same number.
    const qreal ratio = surface_->devicePixelRatioF();
    const int outputWidth = std::max(1, static_cast<int>(std::round(sw * ratio)));
    const int outputHeight = std::max(1, static_cast<int>(std::round(sh * ratio)));
    if (outputWidth_ != outputWidth || outputHeight_ != outputHeight) {
        outputWidth_ = outputWidth;
        outputHeight_ = outputHeight;
        updateInfoChip();
    }

    // ── window controls (top-right) ────────────────────────────────
    // sizeHint() forces a full QHBoxLayout measure over three children, and
    // the ternary used to ask for it twice.
    const int hintedWidth = windowControls_->sizeHint().width();
    const int wcWidth = hintedWidth > 0 ? hintedWidth : 96;
    const int wcX = std::clamp(static_cast<int>(w * 0.945) + 4, 0, std::max(0, w - wcWidth - 4));
    windowControls_->setGeometry(wcX, std::max(6, static_cast<int>(h * 0.008)), wcWidth, 36);

    // ── title bar (windowed mode) ──────────────────────────────────
    if (titleBar_ != nullptr) {
        const int barHeight = 36;
        titleBar_->setGeometry(0, 0, w, barHeight);
        // No painted bar means no reserved strip.
        titleBarOffset_ = (fullscreen_ || !frameless_) ? 0 : barHeight;
    }

    // ── flank chevrons ─────────────────────────────────────────────
    const int cy = static_cast<int>(h * 0.5) - 21;
    btnPrev_->move(static_cast<int>(w * 0.012), cy);
    btnNext_->move(w - static_cast<int>(w * 0.012) - 42, cy);

    // ── dock (bottom-centre) ───────────────────────────────────────
    const int dw = std::min(kDockMaxWidth, w - 2 * kDockHorizontalInset);
    dock_->setGeometry((w - dw) / 2, h - kDockBottomInset - kDockHeight, dw, kDockHeight);

    // ── status chip ───────────────────────────────────────────────
    footerLeft_->adjustSize();
    footerLeft_->move(12, 8);

    // ── overlays ───────────────────────────────────────────────────
    center_->move((w - center_->width()) / 2, (h - center_->height()) / 2);
    if (toast_->isVisible()) {
        toast_->move((w - toast_->width()) / 2, static_cast<int>(h * 0.80));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  HOST → SHELL
// ═════════════════════════════════════════════════════════════════════════════

void PlayerShell::setMedia(const QString& path, const QString& origin, const QString& quality) {
    path_ = path;
    hasMedia_ = !path.isEmpty();
    btnSkipForward_->setEnabled(hasMedia_ && duration_ > 0.0);
    if (!origin.isEmpty()) origin_ = origin;
    if (!quality.isEmpty()) setQuality(quality);

    const QString name =
        path.isEmpty() ? tr("no media loaded") : QFileInfo(path).fileName();
    setWindowTitle(tr("%1 · Media Lightbox").arg(name));
    position_ = 0.0;
    networkBytesPerSecond_ = 0.0;
    streamFramesPerSecond_ = 0.0;
    updateInfoChip();
    // The folder row is only offered for a source that is a file on disk, so
    // the rows follow the media rather than being frozen at build time.
    updatePictureMenu();
    updateEmptyState();
    layoutChrome();
}

void PlayerShell::setDuration(double seconds) {
    duration_ = std::max(0.0, seconds);
    btnSkipForward_->setEnabled(hasMedia_ && duration_ > 0.0);
    scrub_->setDuration(duration_);
    updateTimeLabels();
}

void PlayerShell::setPosition(double seconds) {
    position_ = std::max(0.0, seconds);
    updateTimeLabels();
}

void PlayerShell::setPlaying(bool playing) {
    playing_ = playing;
    updatePlayIcon();
    center_->setKind((playing || hudHidden_ || !hasMedia_) ? CenterGlyph::Kind::None
                                                           : CenterGlyph::Kind::Play);
    if (!hudHidden_) {
        hideTimer_.start(kAutohideMs);
    }
}

void PlayerShell::updatePlayIcon() {
    // The shell caches the pause state, but the engine owns the truth: the
    // first playingChanged lands before the cache is primed, and after a PiP
    // hand-back the cache and the core can disagree. One property read, and a
    // play control stuck on pause while the video runs is the one icon a
    // viewer trusts absolutely.
    const bool running = surface_ != nullptr && surface_->isPlaying();
    btnPlay_->setIcon(running ? IconPainter::pause : IconPainter::play);
    // Checked mirrors the same single fact, so the state is readable by identity
    // instead of only by glyph. The engine still owns it: onPlayClicked writes
    // through togglePause, and the next playingChanged rewrites this from the
    // core, so the button never becomes a second source of truth.
    btnPlay_->setChecked(running);
}

void PlayerShell::setEpisodeStepping(bool hasPrevious, bool hasNext) {
    if (hasPreviousEpisode_ == hasPrevious && hasNextEpisode_ == hasNext) {
        return;
    }
    hasPreviousEpisode_ = hasPrevious;
    hasNextEpisode_ = hasNext;
    // An arrow with nothing to load is a dead control, so it goes away rather
    // than inviting a click that cannot do anything.
    btnPrev_->setVisible(hasPrevious && !hudHidden_);
    btnNext_->setVisible(hasNext && !hudHidden_);
    update();
}

void PlayerShell::setEpisodeLabel(const QString& label) {
    episodeLabel_ = label;
    if (lblEpisode_ != nullptr) {
        lblEpisode_->setText(label.isEmpty() ? QStringLiteral("MIKAI") : label);
        footerLeft_->adjustSize();
    }
}

void PlayerShell::setVolume(double volume, bool fromHost) {
    volume_ = std::clamp(volume, 0.0, 1.0);
    vol_->setVolume(volume_);
    if (volume_ > 0 && muted_) {
        muted_ = false;
        vol_->setMuted(false);
    }
    if (fromHost) {
        toast_->flash(tr("Volume %1%").arg(static_cast<int>(volume_ * 100)));
    }
}

void PlayerShell::setMuted(bool muted) {
    muted_ = muted;
    vol_->setMuted(muted);
}

void PlayerShell::setRate(double rate) {
    rate_ = std::clamp(rate, 0.25, 2.0);
    flyout_->setRate(rate_);
}

void PlayerShell::setQuality(const QString& quality) {
    quality_ = quality.isEmpty() ? QStringLiteral("HD") : quality;
    flyout_->setQuality(quality_);
}

void PlayerShell::setTrackAvailability(int audioTracks, int subtitleTracks, bool localMedia) {
    btnAudio_->setVisible(localMedia || audioTracks > 1);
    btnAudio_->setEnabled(audioTracks > 0);
    btnSubs_->setVisible(localMedia || subtitleTracks > 0);
    btnSubs_->setEnabled(subtitleTracks > 0);
    dock_->adjustSize();
    layoutChrome();
}

void PlayerShell::setAspectRatio(double ratio) {
    if (ratio > 0.05 && ratio < 20.0) {
        aspect_ = ratio;
        layoutChrome();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  INTERNAL CONTROLS
// ═════════════════════════════════════════════════════════════════════════════

void PlayerShell::updateTimeLabels() {
    const QString elapsed = formatTime(position_);
    if (elapsed != lblElapsed_->text()) lblElapsed_->setText(elapsed);
    const QString remaining = formatSigned(std::max(0.0, duration_ - position_));
    if (remaining != lblRemaining_->text()) lblRemaining_->setText(remaining);
    if (duration_ > 0 && !scrub_->isDragging()) {
        // The bar is painted at pixel resolution, so a fraction finer than one
        // pixel moves the handle to where it already is. Snapping first keeps
        // 59 of every 60 emissions from repainting the whole timeline.
        const double pixels = std::max(1.0, static_cast<double>(scrub_->width()));
        scrub_->setValueSilent(std::round(position_ / duration_ * pixels) / pixels);
    }
}

void PlayerShell::onSkipForward() {
    if (!hasMedia_ || duration_ <= 0.0) {
        return;
    }
    const double target = std::min(duration_, position_ + 85.0);
    if (target <= position_) {
        return;
    }
    position_ = target;
    updateTimeLabels();
    emit seekRequested(target);
}

void PlayerShell::onPlayClicked() {
    if (!hasMedia_) {
        toast_->flash(tr("No media loaded — drop a file to begin"));
        return;
    }
    emit playPauseRequested();
}

void PlayerShell::onScrub(double fraction) {
    const double t = fraction * duration_;
    position_ = t;
    lblElapsed_->setText(formatTime(t));
    lblRemaining_->setText(formatSigned(std::max(0.0, duration_ - t)));
    emit seekRequested(t);
}

void PlayerShell::onVolume(double volume) {
    setVolume(volume);
    emit volumeChanged(volume_);
    toast_->flash(tr("Volume %1%").arg(static_cast<int>(volume_ * 100)));
}

void PlayerShell::toggleMute() {
    muted_ = !muted_;
    vol_->setMuted(muted_);
    emit muteChanged(muted_);
    toast_->flash(muted_ ? tr("Muted")
                         : tr("Volume %1%").arg(static_cast<int>(volume_ * 100)));
}

void PlayerShell::onRate(double rate) {
    setRate(rate);
    emit rateChanged(rate_);
    toast_->flash(tr("Speed %1x").arg(rate_, 0, 'g', 3));
}

void PlayerShell::setQualityOptions(const QVector<SettingsFlyout::QualityOption>& options) {
    qualityOptions_ = options;
}

void PlayerShell::setAnimeJanaiSelection(const QString& id, const QString& status) {
    const bool selectionChanged = animeJanaiPresetId_ != id;
    animeJanaiPresetId_ = id;
    // Retained as well as rendered: a language change re-derives this chip from
    // the same selection and status instead of asking the host for it again.
    animeJanaiStatus_ = status;
    updateAnimeJanaiChip();
    if (selectionChanged) {
        refreshEnhancementTree();
    }
    updateInfoChip();
}

/// The chip's own text, tooltip and accessible name, from the retained
/// selection and status. The status line is partly ours - "Preparing…" and the
/// backend/model form - so it has to be rebuilt on a language change; the
/// status text itself is the host's and is never invented here.
void PlayerShell::updateAnimeJanaiChip() {
    const QString& id = animeJanaiPresetId_;
    const QString& status = animeJanaiStatus_;
    if (lblAnimeJanai_ != nullptr) {
        if (id.isEmpty()) {
            lblAnimeJanai_->hide();
        } else {
            const QString backendShort =
                animeJanaiBackend_.compare(QLatin1String("directml"), Qt::CaseInsensitive) == 0
                    ? QStringLiteral("DML")
                    : QStringLiteral("TRT");
            QString presetLabel = id;
            bool presetInCatalogue = false;
            for (const native::AnimeJanaiPreset& preset : native::animeJanaiPresets()) {
                if (preset.id.compare(id, Qt::CaseInsensitive) == 0) {
                    presetLabel = preset.label;
                    presetInCatalogue = true;
                    break;
                }
            }
            if (!presetInCatalogue) {
                // The retained selection outlived its graph. It is still the
                // selection - the filter was asked for that id - so the chip
                // says out loud that the catalogue cannot resolve it instead of
                // falling back to Off or to a model the viewer never chose. The
                // host's status stays in the tooltip, word for word.
                const QString missing = tr("Unavailable · %1").arg(id);
                lblAnimeJanai_->setText(
                    QStringLiteral("│ %1 · %2").arg(backendShort, missing));
                lblAnimeJanai_->setToolTip(status.isEmpty() ? missing : status);
                lblAnimeJanai_->setAccessibleName(missing);
                lblAnimeJanai_->show();
                footerLeft_->adjustSize();
                return;
            }
            QString visibleStatus = status;
            if (status.startsWith(QLatin1String("Configured;"))) {
                if (id.startsWith(QLatin1String("rife:"), Qt::CaseInsensitive)) {
                    const double srcFps = streamFramesPerSecond_ > 0.0 ? streamFramesPerSecond_ : 23.98;
                    const double outFps = srcFps * 2.0;
                    if (sourceHeight_ > 0) {
                        visibleStatus = QStringLiteral("%1 · %2 · %3p · %4 → %5 FPS")
                                            .arg(backendShort, presetLabel)
                                            .arg(sourceHeight_)
                                            .arg(srcFps, 0, 'f', 2)
                                            .arg(outFps, 0, 'f', 2);
                    } else {
                        visibleStatus = QStringLiteral("%1 · %2 · %3")
                                            .arg(backendShort, presetLabel, tr("Preparing…"));
                    }
                } else if (sourceHeight_ > 0) {
                    const int outH = sourceHeight_ * 2;
                    const int outW = (sourceWidth_ > 0 ? sourceWidth_ : (sourceHeight_ * 16 / 9)) * 2;
                    visibleStatus = QStringLiteral("%1 · %2 · %3p → %4p (%5×%4)")
                                        .arg(backendShort, presetLabel)
                                        .arg(sourceHeight_)
                                        .arg(outH)
                                        .arg(outW);
                } else {
                    visibleStatus = QStringLiteral("%1 · %2 · %3")
                                        .arg(backendShort, presetLabel, tr("Preparing…"));
                }
            }
            const bool alreadyPrefixed =
                visibleStatus.startsWith(QLatin1String("TRT ")) ||
                visibleStatus.startsWith(QLatin1String("DML ")) ||
                visibleStatus.startsWith(QLatin1String("AnimeJaNai"), Qt::CaseInsensitive);
            lblAnimeJanai_->setText(
                alreadyPrefixed
                    ? QStringLiteral("│ %1").arg(visibleStatus)
                    : QStringLiteral("│ %1 · %2").arg(backendShort, visibleStatus));
            lblAnimeJanai_->setToolTip(status);
            lblAnimeJanai_->setAccessibleName(visibleStatus);
            lblAnimeJanai_->show();
        }
        footerLeft_->adjustSize();
    }
}

void PlayerShell::setAnimeJanaiBackend(const QString& backend) {
    if (animeJanaiBackend_ == backend) {
        return;
    }
    animeJanaiBackend_ = backend;
    refreshEnhancementTree();
}

void PlayerShell::setAnime4KMode(anime4k::Mode mode) {
    if (anime4kMode_ == mode) {
        return;
    }
    anime4kMode_ = mode;
    refreshEnhancementTree();
    updateInfoChip();
}

void PlayerShell::setNativeMenuInputs(const native::menu::Inputs& inputs) {
    nativeInputs_ = inputs;
    updateNativeHud();
    refreshEnhancementTree();
}

void PlayerShell::setStreamStats(double bytesPerSecond, double framesPerSecond,
                                 StreamFpsSource fpsSource) {
    networkBytesPerSecond_ = std::max(0.0, bytesPerSecond);
    streamFramesPerSecond_ = std::max(0.0, framesPerSecond);
    streamFpsSource_ = fpsSource;
    updateInfoChip();
}

void PlayerShell::setSourceSize(int width, int height) {
    sourceWidth_ = std::max(0, width);
    sourceHeight_ = std::max(0, height);
    updateInfoChip();
}

void PlayerShell::onAnime4KMode(anime4k::Mode mode) {
    setAnime4KMode(mode);
    QString label = anime4k::displayLabel(mode);
    if (label.isEmpty()) {
        label = anime4k::modeId(mode);
    }
    toast_->flash(tr("Anime4K: %1").arg(label));
    emit anime4KModeChanged(mode);
}

void PlayerShell::applyAnime4KPreset(const QString& presetId) {
    // Guarded, because this entry point is fed by stored settings: a key can
    // outlive the preset it was bound to. onAnime4KPreset() maps an unknown id
    // onto Off, which would silently switch the chain off on a typo.
    const QVector<anime4k::ModeInfo> modes = anime4k::availableModes();
    for (const anime4k::ModeInfo& info : modes) {
        if (info.id == presetId) {
            onAnime4KPreset(presetId);
            return;
        }
    }
}

void PlayerShell::onAnime4KPreset(const QString& presetId) {
    // The id is the catalogue's own key, so a menu row can never select a
    // preset the rewrite of the menu no longer points at.
    onAnime4KMode(anime4k::modeForId(presetId));
}

void PlayerShell::refreshEnhancementTree() {
    cancelEnhancementHover();
    const QString currentId = anime4k::modeId(anime4kMode_);
    for (int depth = 0; depth < enhancementPanels_.size(); ++depth) {
        MenuPopup* panel = enhancementPanels_.at(depth);
        if (!panel->isVisible()) {
            continue;
        }
        const QString& level = enhancementLevels_.at(depth);
        if (level.isEmpty()) {
            panel->setTitle(tr("ENHANCEMENTS"));
        } else if (level.startsWith(QLatin1String("animejanai"))) {
            panel->setTitle(animeJanaiTitle(level));
        } else if (level == QLatin1String(native::menu::kNativeLevelId)) {
            panel->setTitle(tr("NATIVE PROCESSING"));
        } else if (level == QLatin1String(native::menu::kRifeLevelId)) {
            panel->setTitle(QStringLiteral("RIFE"));
        } else {
            const QString label = anime4k::categoryLabel(level);
            panel->setTitle(label.isEmpty() ? QStringLiteral("ANIME4K") : label.toUpper());
        }
        const QVector<MenuEntry> entries = enhancementEntries(
            enhancementLevels_.at(depth),
            enhancementLevels_.at(depth) == QLatin1String(kAnimeJanaiBackend)
                ? animeJanaiBackend_
                : (enhancementLevels_.at(depth).startsWith(QLatin1String("animejanai"))
                       ? animeJanaiPresetId_ : currentId),
            nativeInputs_);
        if (!sameEntries(entries, panel)) {
            panel->setEntries(entries);
        }
        watchEnhancementRows(panel);
    }
}

void PlayerShell::onAnimeJanaiCatalogueChanged() {
    // A catalogue change is not a choice: the retained preset id and backend
    // stay exactly as they are, no preset is requested, no chain is reloaded
    // and no status text is rewritten. Only what the catalogue can offer is
    // redrawn - the open levels, their labels and tooltips, and the chip's
    // label. Rows are replaced through MenuPopup::setEntries(), which retires
    // the old widgets with deleteLater(), so no click handler is ever running
    // inside a row that this pass destroys.
    pruneStaleEnhancementLevels();
    refreshEnhancementTree();
    updateAnimeJanaiChip();
    // A level keeps the place it was opened at. Only a card that grew past the
    // window that parents it is pulled back inside, because a row nobody can
    // click is worse than a card that moved a few pixels.
    for (MenuPopup* panel : enhancementPanels_) {
        if (!panel->isVisible()) {
            continue;
        }
        QWidget* host = panel->parentWidget();
        if (host == nullptr) {
            continue;
        }
        const QRect bounds = host->rect().adjusted(4, 4, -4, -4);
        panel->move(qBound(bounds.left(), panel->x(), bounds.right() - panel->width() + 1),
                    qBound(bounds.top(), panel->y(), bounds.bottom() - panel->height() + 1));
    }
}

void PlayerShell::pruneStaleEnhancementLevels() {
    if (enhancementPanels_.isEmpty()) {
        return;
    }
    const QVector<native::AnimeJanaiPreset> presets = native::animeJanaiPresets();
    // Deepest first: a range that lost its last version takes the version level
    // under it, and closing the child before the parent leaves no stale panel
    // behind for the refresh to draw.
    for (int depth = enhancementPanels_.size() - 1; depth > 0; --depth) {
        MenuPopup* panel = enhancementPanels_.at(depth);
        if (!panel->isVisible()) {
            continue;
        }
        const QString& level = enhancementLevels_.at(depth);
        if (!isCatalogueRifeLevel(level) || rifeLevelHasPreset(level, presets)) {
            continue;
        }
        // Closing a level is not a selection change: the retained id remains
        // selected and, if its graph is gone, is drawn as an explicit
        // unavailable row in the level that is still standing.
        cancelEnhancementHover();
        panel->close();
        enhancementLevels_[depth].clear();
        panel->setProperty("enhancementLevel", QString());
    }
}

void PlayerShell::collapseEnhancementTree(int depth) {
    cancelEnhancementHover();
    for (int deeper = depth + 1; deeper < enhancementPanels_.size(); ++deeper) {
        MenuPopup* panel = enhancementPanels_.at(deeper);
        if (panel->isVisible()) {
            panel->close();
        }
        enhancementLevels_[deeper].clear();
    }
}

void PlayerShell::watchEnhancementRows(MenuPopup* panel) {
    for (int spot = 0; spot < panel->rowCount(); ++spot) {
        if (QWidget* row = panel->rowWidget(spot)) {
            row->installEventFilter(this);
        }
    }
}

void PlayerShell::cancelEnhancementHover() {
    enhancementHoverTimer_.stop();
    pendingEnhancementHoverPanel_.clear();
    pendingEnhancementHoverRow_.clear();
    pendingEnhancementHoverSpot_ = -1;
}

bool PlayerShell::hasVisibleEnhancementDescendant(int depth) const {
    for (int deeper = depth + 1; deeper < enhancementPanels_.size(); ++deeper) {
        if (enhancementPanels_.at(deeper)->isVisible()) {
            return true;
        }
    }
    return false;
}

void PlayerShell::showEnhancementLevel(int depth, const QString& levelId, QWidget* anchor) {
    if (depth <= 0 || depth >= enhancementPanels_.size() || anchor == nullptr) {
        return;
    }
    MenuPopup* panel = enhancementPanels_.at(depth);
    if (levelId.startsWith(QLatin1String("animejanai"))) {
        panel->setTitle(animeJanaiTitle(levelId));
    } else if (levelId == QLatin1String(native::menu::kNativeLevelId)) {
        panel->setTitle(tr("NATIVE PROCESSING"));
    } else if (levelId == QLatin1String(native::menu::kRifeLevelId)) {
        panel->setTitle(QStringLiteral("RIFE"));
    } else {
        const QString label = anime4k::categoryLabel(levelId);
        panel->setTitle(label.isEmpty() ? QStringLiteral("ANIME4K") : label.toUpper());
    }
    panel->setEntries(enhancementEntries(
        levelId, levelId == QLatin1String(kAnimeJanaiBackend)
                     ? animeJanaiBackend_
                     : (levelId.startsWith(QLatin1String("animejanai"))
                            ? animeJanaiPresetId_ : anime4k::modeId(anime4kMode_)),
        nativeInputs_));
    enhancementLevels_[depth] = levelId;
    panel->setProperty("enhancementLevel", levelId);
    watchEnhancementRows(panel);
    collapseEnhancementTree(depth);
    QWidget* host = panel->parentWidget();
    if (host == nullptr) {
        return;
    }
    panel->popupBeside(QRect(anchor->mapTo(host, QPoint(0, 0)), anchor->size()),
                       enhancementGrowLeft_);
}

void PlayerShell::onEnhancementRow(MenuPopup* panel, int spot, bool activated) {
    const int depth = enhancementPanels_.indexOf(panel);
    if (depth < 0 || spot < 0 || spot >= panel->rowCount()) {
        return;
    }
    const MenuEntry entry = panel->entryAt(spot);
    if (entry.disabled) {
        return;
    }
    if (entry.submenu) {
        const QString submenuId = entry.payload.toString().mid(6);
        const native::RuntimeSelection& runtime = native::currentSelection();
        if (native::menu::ownsLevel(submenuId) &&
            (!runtime.loaded || runtime.kind != native::BundleKind::Native)) {
            return;
        }
        if (depth == 0) {
            // Which way the tree grows is decided once, from the room beside
            // the root, and then held for every level below it: a cascade that
            // re-decided per level would flip back over its own panels.
            QWidget* host = panel->parentWidget();
            if (host != nullptr) {
                const QRect rect(panel->mapTo(host, QPoint(0, 0)), panel->size());
                enhancementGrowLeft_ = rect.left() > host->width() - rect.right();
            }
        }
        showEnhancementLevel(depth + 1, submenuId, panel->rowWidget(spot));
        return;
    }
    // A terminal row: the levels below it are no longer about anything.
    collapseEnhancementTree(depth);
    if (!activated) {
        return;
    }
    const QString payload = entry.payload.toString();
    if (payload.startsWith(QLatin1String("ajipreset|"))) {
        const QString id = payload.mid(10);
        emit animeJanaiPresetRequested(id);
        dismissOverlay();
        return;
    }
    if (payload.startsWith(QLatin1String("ajbackend|"))) {
        emit animeJanaiBackendRequested(payload.mid(10));
        dismissOverlay();
        return;
    }
    if (payload.startsWith(QLatin1String("native|"))) {
        const native::RuntimeSelection& runtime = native::currentSelection();
        if (!runtime.loaded || runtime.kind != native::BundleKind::Native) {
            return;
        }
        // native|<action>|<value>. The value keeps any '|' it carries, so a
        // recipe id is never truncated into a different id. A row that names no
        // action emits nothing - and a native payload never reaches the shader
        // chain, so it can never set a shader id.
        const QStringList parts = payload.split(QLatin1Char('|'));
        const native::menu::Action action =
            native::menu::actionFromName(parts.size() > 1 ? parts.at(1) : QString());
        if (action == native::menu::Action::None) {
            return;
        }
        const QString value =
            parts.size() > 2 ? parts.mid(2).join(QLatin1Char('|')) : QString();
        emit nativeActionRequested(action, value);
        return;
    }
    onAnime4KPreset(payload.mid(7));
    dismissOverlay();
}

void PlayerShell::updateInfoChip() {
    const QString throughput =
        QStringLiteral("%1 Mbps").arg(networkBytesPerSecond_ * 8.0 / 1'000'000.0, 0, 'f', 2);
    const QString fps =
        streamFramesPerSecond_ > 0.0
            ? QStringLiteral("%1 FPS").arg(streamFramesPerSecond_, 0, 'f', 2)
            : QStringLiteral("— FPS");
    const bool hasEnhancement =
        anime4kMode_ != anime4k::Mode::Off || !animeJanaiPresetId_.isEmpty() ||
        (lblNative_ != nullptr && lblNative_->isVisible());
    if (networkBytesPerSecond_ > 0.0) {
        if (!hasEnhancement && sourceHeight_ > 0) {
            lblNetworkStats_->setText(
                QStringLiteral("%1p · %2 × %3").arg(sourceHeight_).arg(throughput, fps));
        } else {
            lblNetworkStats_->setText(QStringLiteral("%1 × %2").arg(throughput, fps));
        }
    } else if (streamFramesPerSecond_ > 0.0) {
        if (!hasEnhancement && sourceHeight_ > 0) {
            lblNetworkStats_->setText(QStringLiteral("%1p · %2").arg(sourceHeight_).arg(fps));
        } else {
            lblNetworkStats_->setText(fps);
        }
    } else {
        lblNetworkStats_->setText(
            !hasEnhancement && sourceHeight_ > 0
                ? QStringLiteral("%1p · %2").arg(sourceHeight_).arg(fps)
                : fps);
    }
    QString fpsProvenance;
    switch (streamFpsSource_) {
        case StreamFpsSource::MpvOutput:
            fpsProvenance = tr("mpv output FPS");
            break;
        case StreamFpsSource::Container:
            fpsProvenance = tr("container source FPS");
            break;
        case StreamFpsSource::Unknown:
            fpsProvenance = tr("FPS unknown");
            break;
    }
    lblNetworkStats_->setToolTip(fpsProvenance);
    lblNetworkStats_->setAccessibleDescription(fpsProvenance);

    if (anime4kMode_ == anime4k::Mode::Off) {
        lblEnhancement_->hide();
    } else {
        // Qualified, because an experiment's own label ("HQ") only means
        // something inside the category it sits in.
        QString label = anime4k::displayLabel(anime4kMode_);
        if (label.isEmpty()) {
            label = QStringLiteral("Anime4K");
        }
        const int sourceWidth = sourceWidth_ > 0 ? sourceWidth_ : outputWidth_;
        const int sourceHeight = sourceHeight_ > 0 ? sourceHeight_ : outputHeight_;
        if (sourceHeight > 0 && outputHeight_ > 0) {
            lblEnhancement_->setText(
                QStringLiteral("│ Anime4K · %1 · %2p → %3p (%4×%2 → %5×%3)")
                    .arg(label)
                    .arg(sourceHeight)
                    .arg(outputHeight_)
                    .arg(sourceWidth)
                    .arg(outputWidth_));
        } else {
            lblEnhancement_->setText(
                QStringLiteral("│ Anime4K · %1 · %2×%3 → %4×%5")
                    .arg(label)
                    .arg(sourceWidth)
                    .arg(sourceHeight)
                    .arg(outputWidth_)
                    .arg(outputHeight_));
        }
        lblEnhancement_->show();
    }
    // The native chip is a separate line of truth, and it is re-derived with
    // the shader chip so the two can never disagree about what is running.
    updateNativeHud();
    footerLeft_->adjustSize();
}

void PlayerShell::updateNativeHud() {
    if (lblNative_ == nullptr) {
        return;
    }
    const native::menu::StatusLines lines = native::menu::statusLines(nativeInputs_);
    if (!lines.visible || lines.headline.isEmpty()) {
        // No private Native runtime state belongs on a Standard player.
        lblNative_->hide();
    } else {
        lblNative_->setText(QStringLiteral("│ ") + lines.headline);
        // The tooltip and the accessible name carry the fact, not a claim: a
        // requested-but-not-applied selection reads as exactly that.
        lblNative_->setToolTip(lines.reason.isEmpty() ? lines.headline : lines.reason);
        lblNative_->setAccessibleName(lines.headline);
        lblNative_->show();
    }
    footerLeft_->adjustSize();
}

void PlayerShell::openEnhancements() {
    dismissOverlay();
    wake();
    // Always the root: a tree left standing halfway down by the last visit
    // would open on a level nobody asked for.
    collapseEnhancementTree(0);
    MenuPopup* root = enhancementPanels_.first();
    root->setTitle(tr("ENHANCEMENTS"));
    root->setEntries(
        enhancementEntries(QString(), anime4k::modeId(anime4kMode_), nativeInputs_));
    root->setProperty("enhancementLevel", QString());
    watchEnhancementRows(root);
    root->popupNear(btnEnhance_->mapToGlobal(QPoint(btnEnhance_->width(), 0)));
}

void PlayerShell::openSettings() {
    dismissOverlay();
    wake();
    flyout_->setRate(rate_);
    flyout_->setQuality(quality_);
    flyout_->setQualityOptions(qualityOptions_);
    flyout_->popupNear(btnSettings_->mapToGlobal(QPoint(btnSettings_->width(), 0)));
}

void PlayerShell::openKeys() {
    dismissOverlay();
    wake();
    keys_->popupNear(mapToGlobal(QPoint(width() / 2, height() / 2)));
}

void PlayerShell::exitViewer() {
    emit closeRequested();
    close();
}

// ═════════════════════════════════════════════════════════════════════════════
//  KEYBOARD
// ═════════════════════════════════════════════════════════════════════════════

void PlayerShell::keyPressEvent(QKeyEvent* event) {
    wake();
    switch (event->key()) {
        case Qt::Key_Space:
            onPlayClicked();
            break;
        case Qt::Key_F:
        case Qt::Key_F11:
            windowModeToggled();
            break;
        case Qt::Key_M:
            toggleMute();
            break;
        case Qt::Key_Escape:
            if (dismissOverlay()) {
                break;
            }
            exitViewer();
            break;
        case Qt::Key_Right:
            if (duration_ > 0) {
                const double t = std::min(duration_, position_ + 5);
                position_ = t;
                emit seekRequested(t);
                updateTimeLabels();
            } else {
                emit nextRequested();
            }
            break;
        case Qt::Key_Left:
            if (duration_ > 0) {
                const double t = std::max(0.0, position_ - 5);
                position_ = t;
                emit seekRequested(t);
                updateTimeLabels();
            } else {
                emit prevRequested();
            }
            break;
        case Qt::Key_Up:
        case Qt::Key_Down: {
            const double v = std::clamp(volume_ + (event->key() == Qt::Key_Up ? 0.1 : -0.1), 0.0, 1.0);
            setVolume(v);
            emit volumeChanged(v);
            toast_->flash(tr("Volume %1%").arg(static_cast<int>(v * 100)));
            break;
        }
        case Qt::Key_Question:
        case Qt::Key_Slash:
            openKeys();
            break;
        default:
            QWidget::keyPressEvent(event);
            return;
    }
    event->accept();
}

// ═════════════════════════════════════════════════════════════════════════════
//  POINTER
// ═════════════════════════════════════════════════════════════════════════════

void PlayerShell::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    windowModeToggled();
    event->accept();
}

bool PlayerShell::hitEdges(const QPointF& pos) {
    if (!frameless_ || fullscreen_) return false;
    const int grip = kEdgeGrip;
    resizingLeft_ = pos.x() < grip;
    resizingTop_ = pos.y() < grip;
    resizingRight_ = pos.x() > width() - grip;
    resizingBottom_ = pos.y() > height() - grip;
    return resizingLeft_ || resizingTop_ || resizingRight_ || resizingBottom_;
}

void PlayerShell::mousePressEvent(QMouseEvent* event) {
    wake();
    if (event->button() == Qt::RightButton) {
        // The right button is the pointer, not a control: a click on the dock
        // or a popover is handled by those widgets, and this only sees what
        // reaches the picture.
        openPictureMenu(event->position().toPoint());
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    if (dismissOverlay()) {
        event->accept();
        return;
    }

    // The bar is the window's grab handle. startSystemMove hands the drag to
    // the window manager, which is what makes the player snap to edges and
    // land on another monitor the way any other window does; a hand-rolled
    // move() fights all of that.
    if (titleBar_ != nullptr && titleBar_->isVisible() && !fullscreen_
        && titleBar_->geometry().contains(static_cast<int>(event->position().x()),
                                          static_cast<int>(event->position().y()))) {
        if (QWindow* handle = windowHandle(); handle != nullptr) {
            handle->startSystemMove();
            event->accept();
            return;
        }
    }

    if (hitEdges(event->position())) {
        dragGeometry_ = geometry();
        dragOrigin_ = event->globalPosition().toPoint();
        grabPointer();
        event->accept();
        return;
    }

    press_ = event->position();
    moved_ = false;
    grabPointer();
    event->accept();
}

void PlayerShell::mouseMoveEvent(QMouseEvent* event) {
    wake();

    if (resizingLeft_ || resizingTop_ || resizingRight_ || resizingBottom_) {
        QRect g = dragGeometry_;
        const QPoint delta = event->globalPosition().toPoint() - dragOrigin_;
        if (resizingRight_) g.setWidth(std::max(kMinWindowWidth, g.width() + delta.x()));
        if (resizingBottom_) g.setHeight(std::max(kMinWindowHeight, g.height() + delta.y()));
        if (resizingLeft_) {
            g.setLeft(std::min(dragGeometry_.right() - kMinWindowWidth, dragGeometry_.x() + delta.x()));
        }
        if (resizingTop_) {
            g.setTop(std::min(dragGeometry_.bottom() - kMinWindowHeight, dragGeometry_.y() + delta.y()));
        }
        setGeometry(g);
        event->accept();
        return;
    }

    if (!(event->buttons() & Qt::LeftButton)) {
        // Edge cursor feedback; it must not fight the state cursor.
        if (!hudHidden_ && hitEdges(event->position())) {
            if ((resizingLeft_ && resizingTop_) || (resizingRight_ && resizingBottom_)) {
                setCursor(Qt::SizeFDiagCursor);
            } else if ((resizingLeft_ && resizingBottom_) || (resizingRight_ && resizingTop_)) {
                setCursor(Qt::SizeBDiagCursor);
            } else if (resizingLeft_ || resizingRight_) {
                setCursor(Qt::SizeHorCursor);
            } else {
                setCursor(Qt::SizeVerCursor);
            }
            event->accept();
            return;
        }
        if (hudHidden_ == false) setStateCursor();
        return;
    }

    const double dx = event->position().x() - press_.x();
    const double dy = event->position().y() - press_.y();
    if (!moved_ && std::hypot(dx, dy) < kDragThresholdPx) return;
    moved_ = true;
    event->accept();
}

void PlayerShell::mouseReleaseEvent(QMouseEvent* event) {
    if (!grabbed_) return;
    releasePointer();
    if (resizingLeft_ || resizingTop_ || resizingRight_ || resizingBottom_) {
        resizingLeft_ = resizingTop_ = resizingRight_ = resizingBottom_ = false;
        setStateCursor();
        event->accept();
        return;
    }
    setStateCursor();
    if (!moved_ && event->button() == Qt::LeftButton) onPlayClicked();
    event->accept();
}

void PlayerShell::grabPointer() {
    if (!grabbed_) {
        grabbed_ = true;
        grabMouse();
    }
}

void PlayerShell::releasePointer() {
    if (grabbed_) {
        grabbed_ = false;
        releaseMouse();
    }
}

bool PlayerShell::eventFilter(QObject* watched, QEvent* event) {
    // First thing, and before the branches below: a chrome widget the surface
    // adopted reports its own change here - a label that was retexted, a panel
    // that was reshown - and an update() on a widget whose pixels come from the
    // surface is only half a repaint. The event is never consumed.
    chromeRepaintEvent(this, watched, event);
    // Resting on a row of the tree opens the level that hangs under it, which
    // is what makes the tree a tree: with a click per level, four levels would
    // be four choices and three Back rows. Only the rows are watched - a panel
    // that opened the moment the pointer touched it could not be approached.
    if (event->type() == QEvent::Enter) {
        for (int depth = 0; depth < enhancementPanels_.size(); ++depth) {
            MenuPopup* panel = enhancementPanels_.at(depth);
            if (!panel->isVisible()) {
                continue;
            }
            for (int spot = 0; spot < panel->rowCount(); ++spot) {
                if (panel->rowWidget(spot) == watched) {
                    cancelEnhancementHover();
                    if (hasVisibleEnhancementDescendant(depth)) {
                        pendingEnhancementHoverPanel_ = panel;
                        pendingEnhancementHoverRow_ = qobject_cast<QWidget*>(watched);
                        pendingEnhancementHoverSpot_ = spot;
                        const int delay =
                            panel->style()->styleHint(QStyle::SH_Menu_SubMenuPopupDelay,
                                                      nullptr, panel);
                        enhancementHoverTimer_.start(qMax(0, delay));
                    } else {
                        onEnhancementRow(panel, spot, false);
                    }
                    return false;
                }
            }
        }
    }
    if (watched == surface_) {
        // The surface is a real child widget, so re-targeting is a matter of
        // translating the position into shell coordinates. The Python needed a
        // whole shim class because its surface was a foreign native window.
        switch (event->type()) {
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonDblClick:
            case QEvent::MouseMove:
            case QEvent::MouseButtonRelease:
            case QEvent::Wheel: {
                QMouseEvent* mouse = static_cast<QMouseEvent*>(event);
                // A QOpenGLWidget emits a synthetic mouse move on every repaint.
                // Forwarding those would restart the auto-hide countdown on
                // every decoded frame and the chrome would never disappear.
                if (event->type() == QEvent::MouseMove) {
                    const QPoint at = mouse->globalPosition().toPoint();
                    if (at == lastMousePos_) return true;
                    lastMousePos_ = at;
                }
                if (event->type() == QEvent::MouseButtonPress) {
                    mousePressEvent(mouse);
                } else if (event->type() == QEvent::MouseButtonDblClick) {
                    mouseDoubleClickEvent(mouse);
                } else if (event->type() == QEvent::MouseMove) {
                    mouseMoveEvent(mouse);
                } else if (event->type() == QEvent::MouseButtonRelease) {
                    mouseReleaseEvent(mouse);
                }
                return true;
            }
            default:
                break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ═════════════════════════════════════════════════════════════════════════════
//  PAINT & LIFECYCLE
// ═════════════════════════════════════════════════════════════════════════════

bool PlayerShell::event(QEvent* event) {
    // Prevent QWidget::event from calling update() on the full-screen shell
    // whenever focus moves between the shell and a child popup/button.
    if (event->type() == QEvent::FocusIn) {
        focusInEvent(static_cast<QFocusEvent*>(event));
        event->accept();
        return true;
    }
    if (event->type() == QEvent::FocusOut) {
        focusOutEvent(static_cast<QFocusEvent*>(event));
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

void PlayerShell::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void PlayerShell::retranslateUi() {
    const QString name = path_.isEmpty() ? tr("no media loaded") : QFileInfo(path_).fileName();
    setWindowTitle(tr("%1 · Media Lightbox").arg(name));
    // An IconButton copies its tooltip into an accessible name when it is built,
    // so restating the tooltip alone would leave the announced name - what a
    // screen reader reads out - in the language the window opened in. Both come
    // from one string here.
    const auto relabel = [](IconButton* button, const QString& text) {
        button->setToolTip(text);
        button->setAccessibleName(text);
    };
    relabel(titleMin_, tr("Minimize the window"));
    relabel(titleMax_, tr("Toggle fullscreen / windowed"));
    relabel(titleClose_, tr("Exit the player  (Esc)"));
    relabel(btnMin_, tr("Minimize the window"));
    relabel(btnClose_, tr("Exit the player  (Esc)"));
    relabel(btnPrev_, tr("Previous episode  (←)"));
    relabel(btnNext_, tr("Next episode  (→)"));
    // The play control is built with a short spoken name ("Play") while its
    // tooltip carries the full gesture hint. A relabel would overwrite the
    // accessible name with the tooltip, so a language round trip would leave
    // assistive technology reading a different (and longer) name than the one
    // the control was built with. Restate them separately.
    btnPlay_->setToolTip(tr("Play / pause  (Space)"));
    btnPlay_->setAccessibleName(tr("Play"));
    relabel(btnSkipForward_, tr("Skip forward 85 seconds"));
    vol_->setAccessibleName(tr("Volume"));
    scrub_->setAccessibleName(tr("Timeline"));
    relabel(btnPip_, tr("Picture in picture"));
    relabel(btnAudio_, tr("Audio — choose a track  (T)"));
    relabel(btnSubs_, tr("Subtitles — choose a track  (C)"));
    relabel(btnEnhance_, tr("Open enhancements"));
    relabel(btnSettings_, tr("Quality and speed"));
    relabel(btnWindowMode_, fullscreen_ ? tr("Windowed mode") : tr("Fullscreen mode"));

    updatePictureMenu();
    // Re-placed, not rebuilt: the rows above changed width, and a card left at
    // its old top-left can now run past the window that parents it. The anchor
    // is the point it was opened from, so it lands where it was, clamped.
    if (pictureMenu_->isVisible()) {
        pictureMenu_->popupNear(mapToGlobal(pictureMenuAnchor_));
    }
    refreshEnhancementTree();
    // The AnimeJaNai chip carries our own wording around the host's status
    // text, so it is re-derived here; the status itself is not re-requested.
    updateAnimeJanaiChip();
    updateInfoChip();
    updateTimeLabels();
    layoutChrome();
    // A translated string is new chrome pixels even where a counter's text was
    // unchanged - the empty-state hint is painted, not a label - so this is a
    // picture repaint rather than a window repaint.
    chromeDirtyAll();
}

void PlayerShell::chromeDirty(QWidget* widget, const QRect& rect) {
    if (widget == nullptr || surface_ == nullptr) {
        return;
    }
    const QRect local = rect.isNull() ? widget->rect() : rect;
    const QRect dirty(widget->mapTo(this, local.topLeft()), local.size());
    const QRect picture = surface_->geometry();
    // The picture carries the chrome that stands over it; this window carries
    // what falls beside it - and the dock straddles the two, which is why the
    // rect is split rather than sent whole to either side.
    const QRect over = dirty.intersected(picture);
    if (!over.isEmpty()) {
        surface_->update(over.translated(-surface_->pos()));
    }
    const QRegion beside = QRegion(dirty) - QRegion(picture);
    if (!beside.isEmpty()) {
        update(beside);
    }
}

void PlayerShell::chromeDirtyAll() {
    // A move or a resize leaves stale pixels where the widget was, and no
    // widget-shaped rect describes those.
    if (surface_ != nullptr) {
        surface_->update();
    }
    update();
}

void PlayerShell::paintSurfaceOverlay(QPainter* painter) {
    if (painter == nullptr || surface_ == nullptr) {
        return;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    // The painter is in the surface's coordinates and the chrome is laid out in
    // the shell's, of which the surface is a child at a known offset. The clip
    // is the whole shell: the picture is already the edge of this device.
    painter->translate(-surface_->x(), -surface_->y());
    paintChrome(*painter, QRect(QPoint(0, 0), size()));
    painter->restore();
}

void PlayerShell::paintChrome(QPainter& painter, const QRect& clip) {
    if (videoDrop_ != nullptr && videoDrop_->active()) {
        // Both devices use shell coordinates: the card stays whole across the
        // picture edge without a translucent widget above the GL surface.
        videoDrop_->paintIndicator(painter, rect());
        return;
    }
    // The discs are the shell's, not the buttons', so they are painted under
    // every control.
    for (IconButton* button : {btnPrev_, btnNext_}) {
        if (button->isVisible() && QRect(button->pos(), button->size()).intersects(clip)) {
            paintRounded(painter, QRectF(button->geometry()), 21, QColor(0, 0, 0, 110), w15());
        }
    }

    chrome_.paint(painter, this, clip);

    // Empty-state hint: over the whole window rather than in a widget.
    if (!hasMedia_ && !hudHidden_) {
        // The band the two hint lines are drawn into.
        const QRectF band(0.0, height() / 2.0 - 30.0, width(), 44.0);
        if (band.intersects(QRectF(clip))) {
            const QFont f = uiFont(11);
            const QStringList lines = {tr("no media loaded"),
                                       tr("drop a video file here  ·  or pass one as argv[1]")};
            painter.setFont(f);
            for (int i = 0; i < lines.size(); ++i) {
                painter.setPen(i == 0 ? w55() : w35());
                painter.drawText(QRectF(0, height() / 2.0 - 30 + i * 22, width(), 22),
                                 Qt::AlignHCenter, lines[i]);
            }
        }
    }
}

void PlayerShell::paintEvent(QPaintEvent* event) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    if (fullscreen_) {
        p.fillRect(rect(), bg0());
    } else {
        QRadialGradient g(QPointF(width() / 2.0, height() * 0.40),
                          std::max(width(), height()) * 0.75);
        g.setColorAt(0.0, QColor(5, 6, 10, 165));
        g.setColorAt(1.0, QColor(5, 6, 10, 250));
        p.fillRect(rect(), g);
    }

    // The half of the chrome the surface cannot carry: same routine, one clip
    // per paint device, so nothing is painted twice and nothing is left out.
    p.setClipRegion(event->region().intersected(QRegion(rect()) - QRegion(surface_->geometry())));
    paintChrome(p, QRect(QPoint(0, 0), size()));
}

void PlayerShell::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layoutChrome();
}

void PlayerShell::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    applyState();
    layoutChrome();
    hideTimer_.start(kAutohideMs);
}

void PlayerShell::closeEvent(QCloseEvent* event) {
    releasePointer();
    resizingLeft_ = resizingTop_ = resizingRight_ = resizingBottom_ = false;
    hideTimer_.stop();
    dismissOverlay();
    QWidget::closeEvent(event);
}

}  // namespace shell
}  // namespace mplayer
