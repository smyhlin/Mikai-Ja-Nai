// Real geometry and accessibility probe for the settings dialog at its own
// minimum supported size, in the installed interface language and at the
// platform device pixel ratio.
//
// Only the running widgets can settle whether every actionable control on
// every tab is reachable: source reading cannot show that a page is taller than
// the viewport with no scrollbar, that a translated caption pushed a row out of
// reach, or that a check box is under the 24 px desktop hit target. So this
// builds the real dialog, drives it to `minimumSize()`, walks each tab and
// checks three things a user can observe:
//
//  * reachability - every actionable control, after the page it lives on is
//    scrolled as a user would, sits wholly inside its viewport and the dialog;
//    no page needs horizontal panning;
//  * accessibility - every actionable control on the Processing tab, the owned
//    assets panel included, and every shortcut binding on the bindings tab
//    exposes a non-empty accessible name, and each labelled row binds its label
//    to its field;
//  * targets - every actionable control is at least 24 logical pixels wide and
//    tall. An action the store truthfully refuses (disabled) is not a defect;
//    a control that is hidden, clipped or unreachable is.
//
// It writes one screenshot per tab so the presentation can be inspected beside
// the numbers. It is run once per language and per device pixel ratio by its
// caller; it never changes a preference or claims the native engine runs.

#include <cstdio>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QAccessible>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QGuiApplication>
#include <QScreen>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QSize>
#include <QThread>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWidget>

#include "ProbeUi.hpp"
#include "PlayerController.hpp"
#include "PlayerWindow.hpp"
#include "VideoSession.hpp"
#include "backend/MikaiClient.hpp"
#include "dialogs/NativeAssetsPanel.hpp"
#include "dialogs/SettingsDialog.hpp"
#include "theme/Metrics.hpp"
#include "workers/Runner.hpp"

using mikai::app::NativeAssetsPanel;
using mikai::app::Runner;
using mikai::app::SettingsDialog;

namespace {

/// The smallest hit target a desktop control may have. The design system sets
/// this as a minimum in the theme; the probe states it as the contract it
/// guards, never as a width this probe invented.
constexpr int kMinHitTarget = 24;

int failures = 0;

void check(bool ok, const QString& what) {
    const QByteArray text = what.toUtf8();
    std::fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL", text.constData());
    if (!ok) {
        ++failures;
    }
}

/// A widget a user can operate. Item views are the inventory, the cache and the
/// scratch list, which are themselves selectable surfaces; labels and progress
/// bars are not actions and are excluded.
bool isInteractive(const QWidget* widget) {
    return qobject_cast<const QAbstractButton*>(widget) != nullptr ||
           qobject_cast<const QComboBox*>(widget) != nullptr ||
           qobject_cast<const QLineEdit*>(widget) != nullptr ||
           qobject_cast<const QAbstractSpinBox*>(widget) != nullptr ||
           qobject_cast<const QKeySequenceEdit*>(widget) != nullptr ||
           qobject_cast<const QAbstractItemView*>(widget) != nullptr;
}

/// A field's private part - the line edit inside a spin box or a key-sequence
/// field, the clear button inside a line edit - is not a separate action: the
/// control that owns it is the one the user operates.
bool hasInteractiveAncestor(const QWidget* widget, const QWidget* stop) {
    for (const QWidget* parent = widget->parentWidget(); parent != nullptr && parent != stop;
         parent = parent->parentWidget()) {
        if (isInteractive(parent)) {
            return true;
        }
    }
    return false;
}

QString describe(const QWidget* widget) {
    QString text;
    if (const auto* button = qobject_cast<const QAbstractButton*>(widget)) {
        text = button->text();
    } else if (const auto* edit = qobject_cast<const QLineEdit*>(widget)) {
        text = edit->text();
    } else if (const auto* box = qobject_cast<const QComboBox*>(widget)) {
        text = box->currentText();
    }
    text = text.simplified();
    if (text.size() > 40) {
        text = text.left(37) + QStringLiteral("...");
    }
    const QString name = widget->objectName();
    return QStringLiteral("%1[%2]<%3>")
        .arg(QString::fromLatin1(widget->metaObject()->className()),
             name.isEmpty() ? QStringLiteral("-") : name, text);
}

/// The closest scroll area a control is inside, or nullptr. The control is
/// revealed through this one, which is the scroll a user would turn.
QScrollArea* scrollAncestor(QWidget* widget) {
    for (QWidget* parent = widget->parentWidget(); parent != nullptr;
         parent = parent->parentWidget()) {
        if (auto* scroll = qobject_cast<QScrollArea*>(parent)) {
            return scroll;
        }
    }
    return nullptr;
}

QList<QWidget*> interactiveDescendants(QWidget* page) {
    QList<QWidget*> controls;
    for (QWidget* widget : page->findChildren<QWidget*>()) {
        if (!isInteractive(widget) || !widget->isVisibleTo(page)) {
            continue;
        }
        if (hasInteractiveAncestor(widget, page)) {
            continue;
        }
        controls.append(widget);
    }
    return controls;
}

/// Polishes and lays out a whole page with the style applied. An offscreen run
/// never paints, so a widget far down a page keeps the geometry of the
/// pre-style layout: its size hint says one height and its rectangle another.
/// Activating every layout again, from the deepest widget out, replaces that
/// geometry with the polished one, so a later reveal and the final containment
/// are judged against the same, settled rectangles.
void settleLayout(QWidget* page) {
    page->ensurePolished();
    const QList<QWidget*> widgets = page->findChildren<QWidget*>();
    for (QWidget* widget : widgets) {
        widget->ensurePolished();
    }
    for (int round = 0; round < 3; ++round) {
        for (QWidget* widget : widgets) {
            if (QLayout* layout = widget->layout()) {
                layout->activate();
            }
        }
        if (QLayout* layout = page->layout()) {
            layout->activate();
        }
        QApplication::processEvents();
    }
}

/// Brings `control` wholly inside `scroll`'s viewport by moving the scrollbars
/// by the exact overflow the control has, repeating until it is contained or
/// the bars cannot move further. Qt's own ensureWidgetVisible computes its
/// offset from the geometry it sees at that instant, and a widget that is laid
/// out again while it is being revealed can be left a few pixels short of the
/// edge; driving the bars from the measured overflow is exact and does not
/// assume any particular internal geometry. Returns false only when the range
/// itself cannot reach the control.
bool revealFully(QScrollArea* scroll, QWidget* control) {
    QWidget* viewport = scroll->viewport();
    for (int pass = 0; pass < 8; ++pass) {
        const QRect view(viewport->mapToGlobal(QPoint(0, 0)), viewport->size());
        const QRect rect(control->mapToGlobal(QPoint(0, 0)), control->size());
        int vertical = 0;
        int horizontal = 0;
        if (rect.top() < view.top()) {
            vertical = rect.top() - view.top();
        } else if (rect.bottom() > view.bottom()) {
            vertical = rect.bottom() - view.bottom();
        }
        if (rect.left() < view.left()) {
            horizontal = rect.left() - view.left();
        } else if (rect.right() > view.right()) {
            horizontal = rect.right() - view.right();
        }
        if (vertical == 0 && horizontal == 0) {
            return true;
        }
        QScrollBar* vbar = scroll->verticalScrollBar();
        QScrollBar* hbar = scroll->horizontalScrollBar();
        const int nextV = qBound(vbar->minimum(), vbar->value() + vertical, vbar->maximum());
        const int nextH = qBound(hbar->minimum(), hbar->value() + horizontal, hbar->maximum());
        if (nextV == vbar->value() && nextH == hbar->value()) {
            return false;
        }
        vbar->setValue(nextV);
        hbar->setValue(nextH);
        QApplication::processEvents();
    }
    return false;
}

/// Scrolls the control's page as a user would and reports whether it then sits
/// wholly inside its viewport and the dialog. A control a page cannot bring
/// into view is the clipped or unreachable case this probe exists to catch.
bool reachable(QWidget* dialog, QWidget* control, QString* why) {
    QScrollArea* scroll = scrollAncestor(control);
    if (scroll != nullptr) {
        // Polish first: an offscreen run does not paint, so a widget far down a
        // page can still carry pre-style geometry when it is first scrolled to.
        // Then reveal it by the overflow it actually has. Neither step relaxes
        // the containment judged below: a control the page cannot show fails.
        control->ensurePolished();
        QApplication::processEvents();
        revealFully(scroll, control);
    }
    QWidget* viewport = scroll != nullptr ? static_cast<QWidget*>(scroll->viewport()) : dialog;
    const QRect viewRect(viewport->mapToGlobal(QPoint(0, 0)), viewport->size());
    const QRect controlRect(control->mapToGlobal(QPoint(0, 0)), control->size());
    if (!viewRect.contains(controlRect)) {
        *why = QStringLiteral("%1 at %2,%3 %4x%5 is outside its viewport %6,%7 %8x%9")
                   .arg(describe(control))
                   .arg(controlRect.x())
                   .arg(controlRect.y())
                   .arg(controlRect.width())
                   .arg(controlRect.height())
                   .arg(viewRect.x())
                   .arg(viewRect.y())
                   .arg(viewRect.width())
                   .arg(viewRect.height());
        return false;
    }
    const QRect dialogRect(dialog->mapToGlobal(QPoint(0, 0)), dialog->size());
    if (!dialogRect.contains(controlRect)) {
        *why = QStringLiteral("%1 at %2,%3 is outside the dialog window")
                   .arg(describe(control))
                   .arg(controlRect.x())
                   .arg(controlRect.y());
        return false;
    }
    return true;
}

QString accessibleName(QWidget* widget) {
    QAccessibleInterface* face = QAccessible::queryAccessibleInterface(widget);
    if (face != nullptr) {
        const QString name = face->text(QAccessible::Name).trimmed();
        if (!name.isEmpty()) {
            return name;
        }
    }
    return widget->accessibleName().trimmed();
}

/// The page a tab shows: the scrolled page when the tab is itself a scroll
/// area, the tab's own widget otherwise.
QWidget* scrollPage(QWidget* host) {
    if (auto* scroll = qobject_cast<QScrollArea*>(host)) {
        return scroll->widget();
    }
    return host;
}

/// The widgets a page's rows are declared on: the page itself plus every
/// scrolled list inside it. The rows of a list live in the list's own content
/// widget, so a page alone would miss them.
QList<QWidget*> rowSurfaces(QWidget* page) {
    QList<QWidget*> surfaces = {page};
    for (QScrollArea* scroll : page->findChildren<QScrollArea*>()) {
        if (QWidget* content = scroll->widget()) {
            surfaces.append(content);
        }
    }
    return surfaces;
}

/// The row labels that name a field and so must be bound to it. A label carrying
/// an object name is a heading, a hint or a status line, and an empty one is
/// neither a heading nor a name.
QList<QLabel*> rowLabels(QWidget* page) {
    QList<QLabel*> labels;
    for (QWidget* surface : rowSurfaces(page)) {
        for (QLabel* label : surface->findChildren<QLabel*>(Qt::FindDirectChildrenOnly)) {
            if (!label->objectName().isEmpty() || label->text().trimmed().isEmpty()) {
                continue;
            }
            labels.append(label);
        }
    }
    return labels;
}

/// The controls that expose no accessible name, and those below the hit target.
/// Both are measured, so a failure is a reading of the running dialog rather
/// than a claim about the source.
void auditControls(const QList<QWidget*>& controls, QStringList* unnamed,
                   QStringList* shortTargets) {
    for (QWidget* control : controls) {
        if (accessibleName(control).isEmpty()) {
            unnamed->append(describe(control));
        }
        const QSize size = control->size();
        if (size.width() < kMinHitTarget || size.height() < kMinHitTarget) {
            shortTargets->append(QStringLiteral("%1 is %2x%3")
                                     .arg(describe(control))
                                     .arg(size.width())
                                     .arg(size.height()));
        }
    }
}

void report(const char* prefix, const QStringList& lines) {
    for (const QString& line : lines) {
        std::fprintf(stderr, "  %s: %s\n", prefix, qUtf8Printable(line));
    }
}

void reportBounds(const char* heading, const QSize& minimum, qreal dpr,
                  const QList<QWidget*>& controls) {
    std::fprintf(stderr, "%s at %dx%d DPR %.2f:\n", heading, minimum.width(), minimum.height(), dpr);
    for (QWidget* control : controls) {
        const QRect rect(control->mapToGlobal(QPoint(0, 0)), control->size());
        std::fprintf(stderr, "  %-56s %4d x %-4d at %4d,%4d  name=\"%s\"\n",
                     qUtf8Printable(describe(control)), rect.width(), rect.height(), rect.x(),
                     rect.y(), qUtf8Printable(accessibleName(control)));
    }
}

/// A one-shot reading of what sets a page's width floor, so a horizontal
/// overflow names the exact control that votes for it rather than only that it
/// exists. Captions that have already declared they yield - the widgets whose
/// horizontal policy is Ignored - are not a floor and are not listed.
void reportWidthFloor(QWidget* page, QScrollArea* scroll) {
    if (scroll == nullptr) {
        return;
    }
    std::fprintf(stderr, "width floor: page minimum=%d laid out=%d viewport=%d\n",
                 page->minimumSizeHint().width(), page->width(), scroll->viewport()->width());
    for (QWidget* child : page->findChildren<QWidget*>()) {
        if (!child->isVisibleTo(page) ||
            child->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored) {
            continue;
        }
        const int width = child->minimumSizeHint().width();
        if (width <= 300) {
            continue;
        }
        std::fprintf(stderr, "  reserves %4d px: %s\n", width, qUtf8Printable(describe(child)));
    }
}

void auditPlayerHostGeometry() {
    mplayer::quick::PlayerController controller(QStringLiteral("Native geometry host"),
                                                QString());
    mplayer::quick::PlayerWindow* window = controller.window();
    mplayer::VideoSession* session = controller.session();
    QString sceneError;
    QObject::connect(window, &mplayer::quick::PlayerWindow::sceneLoadFailed, &controller,
                     [&sceneError](const QString& error) { sceneError = error; });
    window->setFullscreenIntent(false);
    controller.showWindow();
    QElapsedTimer sceneWait;
    sceneWait.start();
    while (window->rootObject() == nullptr && sceneError.isEmpty() &&
           sceneWait.elapsed() < 10000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }

    check(window->rootObject() != nullptr,
          QStringLiteral("the canonical PlayerWindow loads its production Quick scene (%1)")
              .arg(sceneError));
    check(window->size().width() > 0 && window->size().height() > 0,
          QStringLiteral("the player host has non-empty window geometry"));

    QScreen* screen = QGuiApplication::primaryScreen();
    check(screen != nullptr, QStringLiteral("the player geometry has a target screen"));
    if (screen != nullptr) {
        const QRect available = screen->availableGeometry();
        const QRect requested(available.topLeft() - QPoint(1000, 1000), QSize(640, 360));
        const QRect clamped = window->clampedGeometry(requested, screen);
        check(clamped.isValid() && available.contains(clamped),
              QStringLiteral("the production host clamps restored geometry into the screen"));
    }

    const QRect windowed = window->geometry();
    window->setRememberedGeometry(windowed);
    window->setCompactGeometry(QRect(windowed.topLeft(),
                                     mplayer::quick::PlayerWindow::kCompactSize));
    controller.togglePip();
    QApplication::processEvents();
    check(window->compactPip() && controller.window() == window &&
              controller.session() == session,
          QStringLiteral("PiP keeps the same canonical window and playback session"));
    const QRect expectedCompact = screen != nullptr
                                      ? window->clampedGeometry(
                                            QRect(windowed.topLeft(),
                                                  mplayer::quick::PlayerWindow::kCompactSize),
                                            screen)
                                      : QRect(windowed.topLeft(),
                                              mplayer::quick::PlayerWindow::kCompactSize);
    check(window->topmostIntent(),
          QStringLiteral("the compact presentation retains topmost intent"));
    check(window->size() == expectedCompact.size(),
          QStringLiteral("the compact presentation applies its bounded geometry (%1x%2, expected "
                         "%3x%4)")
              .arg(window->width())
              .arg(window->height())
              .arg(expectedCompact.width())
              .arg(expectedCompact.height()));
    controller.leavePip();
    QApplication::processEvents();
    check(!window->compactPip() && window->rememberedGeometry() == windowed,
          QStringLiteral("leaving PiP restores the remembered windowed geometry"));
    check(!window->topmostIntent(),
          QStringLiteral("leaving PiP releases compact topmost intent"));
    controller.requestClose();
}

}  // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiNativeGeometryProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("NativeGeometryProbe"));
    QApplication app(argc, argv);
    if (!mikai::probes::initializeUi(app)) {
        return 2;
    }

    const QString language = qEnvironmentVariable("MIKAI_PROBE_LANGUAGE", "en");

    // Screenshots land in the caller's directory: the physical launcher stages
    // its own tree and deletes it, so the default working directory is a trap.
    QString outputDir;
    const QStringList probeArgs = QCoreApplication::arguments();
    for (int index = 1; index < probeArgs.size() - 1; ++index) {
        if (probeArgs.at(index) == QLatin1String("--output-dir")) {
            outputDir = probeArgs.at(index + 1);
        }
    }
    if (outputDir.isEmpty()) outputDir = qEnvironmentVariable("MIKAI_PROBE_OUTPUT_DIR");
    if (outputDir.isEmpty()) outputDir = QDir::currentPath();
    if (!QDir::isAbsolutePath(outputDir) || !QDir().mkpath(outputDir)) {
        std::fprintf(stderr, "FAIL the output directory is not an absolute, creatable path: %s\n",
                     qUtf8Printable(outputDir));
        return 1;
    }
    const auto shotPath = [&outputDir](const QString& name) {
        return QDir(outputDir).filePath(name);
    };

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());

    auditPlayerHostGeometry();

    mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
    Runner runner;

    SettingsDialog dialog(client, runner);
    const QSize minimum = dialog.minimumSize();
    dialog.resize(minimum);
    dialog.show();
    QApplication::processEvents();

    std::fprintf(stderr,
                 "native geometry probe: language=%s minimum=%dx%d actual=%dx%d DPR=%.2f\n",
                 qPrintable(language), minimum.width(), minimum.height(), dialog.width(),
                 dialog.height(), dialog.devicePixelRatioF());
    check(dialog.size() == minimum, QStringLiteral("the dialog holds its minimum supported size"));
    check(minimum.width() > 0 && minimum.height() > 0,
          QStringLiteral("the minimum supported size is a real, non-empty contract"));
    check(mikai::probes::dialogActionsFit(dialog, minimum),
          QStringLiteral("Save and Cancel stay visible at the minimum size"));

    // The navigation the dialog actually ships: stable page ids over the
    // settings stack, selected by id rather than by a translated caption.
    const QStringList pageIds = dialog.pageIds();
    check(!pageIds.isEmpty(), QStringLiteral("the settings navigation lists pages"));
    auto* stack = dialog.findChild<QStackedWidget*>(QStringLiteral("settingsStack"));
    check(stack != nullptr, QStringLiteral("the settings content stack exists"));
    if (stack == nullptr) {
        return 1;
    }
    const auto navEntries = dialog.findChildren<QPushButton*>(QStringLiteral("settingsNavItem"));
    check(navEntries.size() == pageIds.size(),
          QStringLiteral("each settings page has one navigation entry"));

    for (const QString& pageId : pageIds) {
        check(dialog.showPage(pageId),
              QStringLiteral("the “%1” page is selectable by id").arg(pageId));
        QApplication::processEvents();
        check(dialog.currentPageId() == pageId,
              QStringLiteral("the “%1” page is the current page").arg(pageId));

        QWidget* host = stack->currentWidget();
        QWidget* page = scrollPage(host);

        // Settle the page before anything is measured: an offscreen run never
        // paints, so a widget far down the page still carries the pre-style
        // layout until every layout is activated again.
        settleLayout(page);

        // No page may require horizontal panning: a control wider than the
        // viewport is a layout defect, not something a scroll wheel fixes. The
        // check runs while the page is current, so its own scroll range is the
        // one that was just laid out.
        const auto checkNoHorizontalRange = [&](QWidget* scrolled, const QString& label) {
            for (QScrollArea* scroll : scrolled->findChildren<QScrollArea*>()) {
                if (!scroll->isVisibleTo(scrolled)) {
                    continue;
                }
                QScrollBar* horizontal = scroll->horizontalScrollBar();
                check(horizontal->minimum() == horizontal->maximum(),
                      QStringLiteral("the “%1” tab has no horizontal scroll range (%2)")
                          .arg(label, scroll->objectName().isEmpty() ? QStringLiteral("(unnamed)")
                                                                    : scroll->objectName()));
            }
        };
        checkNoHorizontalRange(page, pageId);
        if (auto* pageScroll = qobject_cast<QScrollArea*>(host)) {
            QScrollBar* horizontal = pageScroll->horizontalScrollBar();
            check(horizontal->minimum() == horizontal->maximum(),
                  QStringLiteral("the “%1” page has no horizontal scroll range").arg(pageId));
        }
        if (qEnvironmentVariableIsSet("MIKAI_PROBE_WIDTH_FLOOR")) {
            reportWidthFloor(page, qobject_cast<QScrollArea*>(host));
        }

        const QList<QWidget*> controls = interactiveDescendants(page);
        check(!controls.isEmpty(),
              QStringLiteral("the “%1” page lists actionable controls").arg(pageId));

        QStringList unreachable;
        for (QWidget* control : controls) {
            QString why;
            if (!reachable(&dialog, control, &why)) {
                unreachable.append(why);
            }
        }
        check(unreachable.isEmpty(),
              QStringLiteral("every actionable control on the “%1” page is reachable by scrolling")
                  .arg(pageId));
        report("unreachable", unreachable);

        // Leave the page at its top so the screenshot shows its heading rather
        // than wherever the reachability walk happened to scroll it.
        if (QScrollArea* scroll = qobject_cast<QScrollArea*>(host)) {
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->minimum());
        } else if (QScrollArea* inner = scrollAncestor(page)) {
            inner->verticalScrollBar()->setValue(inner->verticalScrollBar()->minimum());
        }
        QApplication::processEvents();

        QString shotName = pageId;
        shotName.replace(QLatin1Char('/'), QLatin1Char('-'));
        const QString file = shotPath(QStringLiteral("native-geometry-%1-%2.png")
                                          .arg(language, shotName));
        check(dialog.grab().save(file),
              QStringLiteral("the “%1” page screenshot is written to %2").arg(pageId, file));
    }

    auto* benchmarkScroll = dialog.findChild<QScrollArea*>(QStringLiteral("benchmarkScroll"));
    check(benchmarkScroll != nullptr,
          QStringLiteral("the Benchmark page exposes its scroll as benchmarkScroll"));
    if (benchmarkScroll != nullptr) {
        check(dialog.showPage(QStringLiteral("benchmark")),
              QStringLiteral("the Benchmark page is selectable by id"));
        QApplication::processEvents();
        benchmarkScroll->verticalScrollBar()->setValue(
            benchmarkScroll->verticalScrollBar()->minimum());
        QApplication::processEvents();
        QString file = shotPath(QStringLiteral("native-geometry-%1-benchmark-top.png").arg(language));
        check(dialog.grab().save(file),
              QStringLiteral("the Benchmark top screenshot is written to %1").arg(file));

        benchmarkScroll->verticalScrollBar()->setValue(
            benchmarkScroll->verticalScrollBar()->maximum());
        QApplication::processEvents();
        file = shotPath(QStringLiteral("native-geometry-%1-benchmark-bottom.png").arg(language));
        check(dialog.grab().save(file),
              QStringLiteral("the Benchmark bottom screenshot is written to %1").arg(file));
    }

    // The bindings are audited on their own, in the same four combinations as
    // the walk, because a binding that is present but clipped, unnamed, under a
    // hit target or unbound from its label still reads as a finished settings
    // page. The tab is the one the catalogue was found on.
    if (pageIds.contains(QStringLiteral("shortcuts"))) {
        check(dialog.showPage(QStringLiteral("shortcuts")),
              QStringLiteral("the shortcut bindings page is selectable by id"));
        QApplication::processEvents();
        QWidget* shortcutsPage = scrollPage(stack->currentWidget());
        settleLayout(shortcutsPage);

        for (QScrollArea* scroll : shortcutsPage->findChildren<QScrollArea*>()) {
            if (!scroll->isVisibleTo(shortcutsPage)) {
                continue;
            }
            QScrollBar* horizontal = scroll->horizontalScrollBar();
            check(horizontal->minimum() == horizontal->maximum(),
                  QStringLiteral("the shortcut bindings need no horizontal scrolling (%1)")
                      .arg(scroll->objectName().isEmpty() ? QStringLiteral("(unnamed)")
                                                          : scroll->objectName()));
        }
        if (qEnvironmentVariableIsSet("MIKAI_PROBE_WIDTH_FLOOR")) {
            reportWidthFloor(shortcutsPage, scrollAncestor(shortcutsPage));
        }

        const QList<QWidget*> bindings = interactiveDescendants(shortcutsPage);
        check(!bindings.isEmpty(), QStringLiteral("the shortcut bindings are listed"));

        QStringList unreachableBindings;
        for (QWidget* binding : bindings) {
            QString why;
            if (!reachable(&dialog, binding, &why)) {
                unreachableBindings.append(why);
            }
        }
        check(unreachableBindings.isEmpty(),
              QStringLiteral("every shortcut binding is reachable by scrolling"));
        report("unreachable", unreachableBindings);

        QStringList unnamedBindings;
        QStringList shortBindings;
        auditControls(bindings, &unnamedBindings, &shortBindings);
        check(unnamedBindings.isEmpty(),
              QStringLiteral("every shortcut binding exposes an accessible name"));
        report("unnamed", unnamedBindings);
        check(shortBindings.isEmpty(),
              QStringLiteral("every shortcut binding meets the 24 px hit target"));
        report("short", shortBindings);

        QStringList unboundBindings;
        for (QLabel* label : rowLabels(shortcutsPage)) {
            if (label->buddy() == nullptr) {
                unboundBindings.append(label->text().simplified());
            }
        }
        check(unboundBindings.isEmpty(),
              QStringLiteral("each shortcut row label is bound to its field"));
        report("no buddy", unboundBindings);

        reportBounds("shortcut bindings", minimum, dialog.devicePixelRatioF(), bindings);
    }

    // The Processing tab carries the native controls this task owns. Its
    // accessibility and target contract is checked there and only there: the
    // borrowed assets panel is another surface with its own contract.
    const bool hasProcessing = dialog.showPage(QStringLiteral("processing"));
    check(hasProcessing, QStringLiteral("the Processing settings page is selectable by id"));
    if (!hasProcessing) {
        return 1;
    }
    QApplication::processEvents();
    QWidget* processingPage = scrollPage(stack->currentWidget());

    const QList<QWidget*> nativeControls = interactiveDescendants(processingPage);
    QStringList unnamed;
    QStringList targets;
    auditControls(nativeControls, &unnamed, &targets);
    check(unnamed.isEmpty(),
          QStringLiteral("every actionable native control exposes an accessible name"));
    report("unnamed", unnamed);
    check(targets.isEmpty(),
          QStringLiteral("every actionable native control meets the 24 px hit target"));
    report("short", targets);

    // Each labelled native row binds its label to its field: the label the user
    // reads is the name assistive technology announces. Headings, hints and
    // status lines carry an object name and are not row labels. The page's own
    // rows and the assets panel's rows are both checked; the RIFE panel's rows
    // are kept by its form layout, whose fields are checked above by name.
    QStringList unbuddy;
    QList<QWidget*> labelledSurfaces = {processingPage};
    for (NativeAssetsPanel* panel : processingPage->findChildren<NativeAssetsPanel*>()) {
        labelledSurfaces.append(panel);
    }
    for (QWidget* surface : labelledSurfaces) {
        for (QLabel* label : surface->findChildren<QLabel*>(Qt::FindDirectChildrenOnly)) {
            if (!label->objectName().isEmpty() || label->text().trimmed().isEmpty()) {
                continue;
            }
            if (label->buddy() == nullptr) {
                unbuddy.append(label->text().simplified());
            }
        }
    }
    check(unbuddy.isEmpty(), QStringLiteral("each native row label is bound to its field"));
    report("no buddy", unbuddy);

    // Record the observed bounds so a failure is a measurement, not a claim.
    reportBounds("native controls", minimum, dialog.devicePixelRatioF(), nativeControls);

    // GOAL-C: the AnimeJaNai manager's action rows are measured on the running
    // dialog, one width per row and the one control height. A row whose members
    // paint at different widths is the defect the user reported as neighbouring
    // buttons of unequal size.
    struct ManagerRow {
        const char* label;
        QStringList names;
    };
    const QList<ManagerRow> managerRows{
        {"onnx models",
         {QStringLiteral("ajiImportModelButton"), QStringLiteral("ajiRemoveModelButton"),
          QStringLiteral("ajiOpenModelsFolderButton")}},
        {"external folder",
         {QStringLiteral("ajiBrowseExternalDirButton"), QStringLiteral("ajiClearExternalDirButton")}},
        {"configuration file",
         {QStringLiteral("ajiImportConfButton"), QStringLiteral("ajiExportConfButton"),
          QStringLiteral("ajiResetDefaultsButton")}},
        {"profile slot",
         {QStringLiteral("ajiCreateSlotButton"), QStringLiteral("ajiCloneSlotButton"),
          QStringLiteral("ajiDeleteSlotButton"), QStringLiteral("ajiImportPconfButton"),
          QStringLiteral("ajiExportPconfButton")}},
        {"profile chain",
         {QStringLiteral("ajiAddChainButton"), QStringLiteral("ajiRemoveChainButton"),
          QStringLiteral("ajiMoveChainUpButton"), QStringLiteral("ajiMoveChainDownButton")}},
    };
    for (const ManagerRow& row : managerRows) {
        QList<QWidget*> members;
        for (const QString& name : row.names) {
            auto* control = dialog.findChild<QPushButton*>(name);
            if (control != nullptr && control->isVisibleTo(processingPage)) members.append(control);
        }
        if (members.size() < 2) {
            std::fprintf(stderr, "  manager row %s: not measurable on this page\n", row.label);
            continue;
        }
        int rowWidth = -1;
        bool equal = true;
        bool oneHeight = true;
        for (QWidget* member : members) {
            if (rowWidth < 0) rowWidth = member->width();
            equal = equal && member->width() == rowWidth;
            oneHeight = oneHeight && member->height() == mikai::app::theme::kControlHeight;
        }
        const QByteArray label = QByteArray("manager actions: ") + row.label;
        check(equal, QStringLiteral("the %1 row shares one width").arg(QString::fromLatin1(row.label)));
        check(oneHeight,
              QStringLiteral("the %1 row is one control height").arg(QString::fromLatin1(row.label)));
        reportBounds(label.constData(), minimum, dialog.devicePixelRatioF(), members);
    }

    // GOAL-C: the components page's action row and card headers at the dialog's
    // own minimum width. Bounds are printed so a failure is a measurement.
    if (dialog.showPage(QStringLiteral("components"))) {
        QApplication::processEvents();
        QWidget* componentsPage = dialog.findChild<QWidget*>(QStringLiteral("componentsPage"));
        if (componentsPage == nullptr) {
            check(false, QStringLiteral("the components page exists"));
        } else {
            settleLayout(componentsPage);
            const QStringList actionNames{
                QStringLiteral("componentDownloadButton"),
                QStringLiteral("refreshComponentsButton"),
                QStringLiteral("nativeComponentVerifyButton"),
                QStringLiteral("nativeComponentImportAnimeJanaiButton"),
                QStringLiteral("nativeComponentTabCancelButton")};
            QList<QWidget*> geometryTargets;
            int actionWidth = -1;
            bool equalWidths = true;
            bool controlHeight = true;
            for (const QString& name : actionNames) {
                auto* action = dialog.findChild<QPushButton*>(name);
                if (action == nullptr) {
                    equalWidths = false;
                    controlHeight = false;
                    continue;
                }
                geometryTargets.append(action);
                if (actionWidth < 0) actionWidth = action->width();
                equalWidths = equalWidths && action->width() == actionWidth;
                controlHeight =
                    controlHeight && action->height() == mikai::app::theme::kControlHeight;
            }
            check(equalWidths, QStringLiteral("the components actions share one width"));
            check(controlHeight, QStringLiteral("every components action is one control height"));

            bool badgeLines = true;
            bool countLines = true;
            bool headingsLegible = true;
            QStringList crampedHeadings;
            for (const QString& card :
                 {QStringLiteral("tensorRtRuntimeCard"), QStringLiteral("animeJanaiModelsCard"),
                  QStringLiteral("rifeModelsCard"), QStringLiteral("tensorRtKernelsCard")}) {
                auto* badge = dialog.findChild<QLabel*>(card + QStringLiteral("State"));
                auto* title = dialog.findChild<QLabel*>(card + QStringLiteral("Title"));
                auto* counts = dialog.findChild<QLabel*>(card + QStringLiteral("Count"));
                if (badge == nullptr || title == nullptr || counts == nullptr) {
                    badgeLines = false;
                    countLines = false;
                    headingsLegible = false;
                    continue;
                }
                geometryTargets.append(badge);
                geometryTargets.append(counts);
                const QRect badgeRect(badge->mapTo(componentsPage, QPoint(0, 0)), badge->size());
                const QRect titleRect(title->mapTo(componentsPage, QPoint(0, 0)), title->size());
                badgeLines = badgeLines &&
                             badge->height() <= mikai::app::theme::kBadgeHeight &&
                             badge->width() <= mikai::app::theme::kComponentBadgeWidth &&
                             !badgeRect.intersects(titleRect);
                // A heading is a label, not an action, so the actionable audit
                // never sees it: it must still show its widest word and hold
                // every wrapped line it needs.
                const bool legible = title->width() >= title->minimumSizeHint().width() &&
                                     title->height() >= title->heightForWidth(title->width());
                if (!legible) {
                    headingsLegible = false;
                    crampedHeadings.append(
                        QStringLiteral("%1 heading %2x%3 needs %4x%5")
                            .arg(card)
                            .arg(title->width())
                            .arg(title->height())
                            .arg(title->minimumSizeHint().width())
                            .arg(title->heightForWidth(title->width())));
                }
                geometryTargets.append(title);
                auto* description =
                    dialog.findChild<QLabel*>(card + QStringLiteral("Description"));
                const bool belowDescription =
                    !counts->isVisible() || description == nullptr ||
                    counts->mapTo(componentsPage, QPoint(0, 0)).y() >=
                        description->mapTo(componentsPage,
                                            QPoint(0, description->height())).y();
                countLines = countLines && counts != badge && belowDescription;
            }
            check(badgeLines, QStringLiteral("every component badge is one 24 px line"));
            check(countLines, QStringLiteral("component counts have their own line"));
            check(headingsLegible,
                  QStringLiteral("every component heading shows its widest word and every line"));
            report("cramped headings", crampedHeadings);
            reportBounds("goals geometry", minimum, dialog.devicePixelRatioF(), geometryTargets);
        }
    }

    std::fprintf(stderr, "[native_geometry_probe] %s (%d failures)\n",
                 failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
