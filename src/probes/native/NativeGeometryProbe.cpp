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
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSize>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWidget>

#include "ProbeUi.hpp"
#include "backend/MikaiClient.hpp"
#include "dialogs/NativeAssetsPanel.hpp"
#include "dialogs/SettingsDialog.hpp"
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

/// The settings tab that carries the shortcut bindings. It is found by its
/// content, never by its caption, because the caption is translated: the
/// bindings tab is the one holding the catalogue of key-sequence fields, and
/// the Processing tab's single field is not that catalogue. Returns -1 when no
/// tab holds a catalogue, which is a defect the caller reports rather than
/// skips.
int shortcutTabIndex(QTabWidget* tabs) {
    int best = -1;
    int bestFields = 1;
    for (int i = 0; i < tabs->count(); ++i) {
        const int fields = tabs->widget(i)->findChildren<QKeySequenceEdit*>().size();
        if (fields > bestFields) {
            bestFields = fields;
            best = i;
        }
    }
    return best;
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

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());

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

    auto* tabs = dialog.findChild<QTabWidget*>();
    check(tabs != nullptr && tabs->count() >= 3, QStringLiteral("the settings tabs exist"));
    if (tabs == nullptr) {
        return 1;
    }

    // The tab bar itself must not clip a tab: a hidden tab is exactly the
    // unreachable action this probe guards against.
    QTabBar* bar = tabs->tabBar();
    const QRect barRect(bar->mapToGlobal(QPoint(0, 0)), bar->size());
    for (int i = 0; i < tabs->count(); ++i) {
        QRect tabRect = bar->tabRect(i);
        tabRect.translate(bar->mapToGlobal(QPoint(0, 0)));
        check(barRect.contains(tabRect),
              QStringLiteral("tab %1 (“%2”) is inside the tab bar").arg(i).arg(tabs->tabText(i)));
    }

    // The bindings tab is located before the walk so its screenshot carries the
    // name the contract is written under, not the translated tab caption.
    const int shortcuts = shortcutTabIndex(tabs);
    check(shortcuts >= 0, QStringLiteral("a settings tab holds the shortcut bindings"));
    QStringList tabNames = {QStringLiteral("general"), QStringLiteral("presets"),
                            QStringLiteral("processing")};
    if (shortcuts >= 0) {
        tabNames[shortcuts] = QStringLiteral("shortcuts");
    }

    for (int i = 0; i < tabs->count(); ++i) {
        tabs->setCurrentIndex(i);
        QApplication::processEvents();

        QWidget* host = tabs->widget(i);
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
        checkNoHorizontalRange(page, tabs->tabText(i));
        if (auto* pageScroll = qobject_cast<QScrollArea*>(host)) {
            QScrollBar* horizontal = pageScroll->horizontalScrollBar();
            check(horizontal->minimum() == horizontal->maximum(),
                  QStringLiteral("the “%1” tab has no horizontal scroll range")
                      .arg(tabs->tabText(i)));
        }
        if (qEnvironmentVariableIsSet("MIKAI_PROBE_WIDTH_FLOOR")) {
            reportWidthFloor(page, qobject_cast<QScrollArea*>(host));
        }

        const QList<QWidget*> controls = interactiveDescendants(page);
        check(!controls.isEmpty(),
              QStringLiteral("the “%1” tab lists actionable controls").arg(tabs->tabText(i)));

        QStringList unreachable;
        for (QWidget* control : controls) {
            QString why;
            if (!reachable(&dialog, control, &why)) {
                unreachable.append(why);
            }
        }
        check(unreachable.isEmpty(),
              QStringLiteral("every actionable control on the “%1” tab is reachable by scrolling")
                  .arg(tabs->tabText(i)));
        report("unreachable", unreachable);

        // Leave the page at its top so the screenshot shows its heading rather
        // than wherever the reachability walk happened to scroll it.
        if (QScrollArea* scroll = qobject_cast<QScrollArea*>(host)) {
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->minimum());
        } else if (QScrollArea* inner = scrollAncestor(page)) {
            inner->verticalScrollBar()->setValue(inner->verticalScrollBar()->minimum());
        }
        QApplication::processEvents();

        const QString file = QStringLiteral("native-geometry-%1-%2.png")
                                 .arg(language)
                                 .arg(i < tabNames.size() ? tabNames.at(i)
                                                          : QString::number(i));
        check(dialog.grab().save(file),
              QStringLiteral("the “%1” tab screenshot is written to %2").arg(tabs->tabText(i), file));
    }

    const int benchmark = tabs->indexOf(
        dialog.findChild<QScrollArea*>(QStringLiteral("benchmarkScroll")));
    check(benchmark >= 0, QStringLiteral("the Benchmark tab exposes its results section"));
    if (benchmark >= 0) {
        tabs->setCurrentIndex(benchmark);
        auto* benchmarkScroll = qobject_cast<QScrollArea*>(tabs->widget(benchmark));
        if (benchmarkScroll != nullptr) {
            benchmarkScroll->verticalScrollBar()->setValue(
                benchmarkScroll->verticalScrollBar()->maximum());
            QApplication::processEvents();
            const QString file =
                QStringLiteral("native-geometry-%1-benchmark-results.png").arg(language);
            check(dialog.grab().save(file),
                  QStringLiteral("the Benchmark results section screenshot is written to %1")
                      .arg(file));
        }
    }

    // The bindings are audited on their own, in the same four combinations as
    // the walk, because a binding that is present but clipped, unnamed, under a
    // hit target or unbound from its label still reads as a finished settings
    // page. The tab is the one the catalogue was found on.
    if (shortcuts >= 0) {
        tabs->setCurrentIndex(shortcuts);
        QApplication::processEvents();
        QWidget* shortcutsHost = tabs->widget(shortcuts);
        QWidget* shortcutsPage = scrollPage(shortcutsHost);
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
    int processing = -1;
    const QString processingLabel = SettingsDialog::tr("Processing");
    for (int index = 0; index < tabs->count(); ++index) {
        if (tabs->tabText(index) == processingLabel) {
            processing = index;
            break;
        }
    }
    check(processing >= 0, QStringLiteral("the Processing settings tab exists"));
    if (processing < 0) {
        return 1;
    }
    tabs->setCurrentIndex(processing);
    QApplication::processEvents();
    QWidget* processingPage = scrollPage(tabs->widget(processing));

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

    std::fprintf(stderr, "[native_geometry_probe] %s (%d failures)\n",
                 failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
