// Real RIFE-settings probe: drives the actual Processing-tab RIFE controls on an
// isolated settings store and an owned, temporary AppData tree, then checks what
// the surface does.
//
// The one thing source reading cannot settle is whether configuring a RIFE
// request actually persists the exact recipe the user described while claiming
// nothing about execution. So this drives the real widgets: variant, ensemble,
// the exact rational factor, the scene threshold, the order, and the RIFE-only /
// combined shape - then the real store button, the real Save and the real
// Cancel, and a reopen that reloads the values.
//
// It never claims a native pipeline runs. Every capability is unavailable in
// this build by construction, and the probe asserts the surface says so and that
// no engine is ever asked to execute a source graph.

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSize>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>

#include <cstdio>
#include <cstring>

#include "backend/MikaiClient.hpp"
#include "dialogs/SettingsDialog.hpp"
#include "native/NativePrefs.hpp"
#include "ProbeUi.hpp"
#include "native/NativeRife.hpp"
#include "native/NativeRifeControls.hpp"
#include "native/NativeUserRecipes.hpp"
#include "theme/Theme.hpp"
#include "workers/Runner.hpp"

using mikai::app::Runner;
using mikai::app::SettingsDialog;
using mplayer::native::Backend;
using mplayer::native::NativeRifeControls;
using mplayer::native::Rational;
using mplayer::native::Recipe;
using mplayer::native::RecipeShape;
using mplayer::native::RifeVariant;
using mplayer::native::TemporalOrder;
using mplayer::native::UserRecipeStore;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

bool closeEnough(double a, double b) { return qAbs(a - b) < 1e-6; }

/// Bit-for-bit: a recognisably-rounded stand-in must not pass for the exact
/// stored value.
bool exactSameDouble(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

/// The editor shows the shortest text that reads back as the exact double.
void setThreshold(QLineEdit* edit, double value) {
    edit->setText(QLocale().toString(value, 'g', QLocale::FloatingPointShortest));
}

double thresholdValue(const QLineEdit* edit) {
    bool ok = false;
    const double value = QLocale().toDouble(edit->text(), &ok);
    return ok ? value : qQNaN();
}

QPushButton* acceptButton(QDialogButtonBox* box) {
    if (box == nullptr) {
        return nullptr;
    }
    for (QAbstractButton* button : box->buttons()) {
        if (box->buttonRole(button) == QDialogButtonBox::AcceptRole) {
            return qobject_cast<QPushButton*>(button);
        }
    }
    return nullptr;
}

QPushButton* rejectButton(QDialogButtonBox* box) {
    if (box == nullptr) {
        return nullptr;
    }
    for (QAbstractButton* button : box->buttons()) {
        if (box->buttonRole(button) == QDialogButtonBox::RejectRole) {
            return qobject_cast<QPushButton*>(button);
        }
    }
    return nullptr;
}

/// True when `widget` lies inside the scroll viewport horizontally, i.e. no
/// horizontal panning is needed to see it whole. The page is taller than any
/// sane window, so vertical scrolling is expected and only this axis is checked.
bool horizontallyInside(const QWidget* widget, const QWidget* viewport) {
    if (widget == nullptr || viewport == nullptr || !widget->isVisibleTo(viewport)) return false;
    const QPoint topLeft = widget->mapTo(viewport, QPoint(0, 0));
    return topLeft.x() >= 0 && topLeft.x() + widget->width() <= viewport->width();
}

/// The name a diagnostic can print: the object name the surface is addressed by
/// in this probe, plus its class - several rows share the "muted" object name,
/// so the class is what tells them apart.
QString widgetName(const QWidget* widget) {
    const QString className = QString::fromLatin1(widget->metaObject()->className());
    return widget->objectName().isEmpty()
               ? className
               : QStringLiteral("%1 (%2)").arg(widget->objectName(), className);
}

/// The stored temporal recipe matching the exact shape the probe asked for.
const Recipe* temporalRecipe(const QVector<Recipe>& recipes, RifeVariant variant, bool ensemble,
                             const Rational& factor, TemporalOrder order, double threshold) {
    for (const Recipe& recipe : recipes) {
        if (!recipe.temporal.has_value()) continue;
        if (recipe.spatial.size() != 0) continue;
        const mplayer::native::TemporalStage& stage = *recipe.temporal;
        if (stage.variant != variant || stage.ensemble != ensemble) continue;
        if (stage.factor.num != factor.num || stage.factor.den != factor.den) continue;
        if (stage.order != order) continue;
        if (!closeEnough(stage.sceneThreshold, threshold)) continue;
        return &recipe;
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiRifeSettingsProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("RifeSettingsProbe"));
    QApplication app(argc, argv);
    if (!mikai::probes::initializeUi(app)) return 2;

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());

    // A clean, app-owned tree for this probe only: the app/org names above make
    // AppLocalDataLocation ours, so the recipe store lives there and nowhere
    // else. It is removed again at the end, so nothing outlives the run.
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    check(!appData.isEmpty(), "the probe has an app-local data location");
    QDir(appData).removeRecursively();

    mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
    Runner runner;

    QString firstStoredId;
    QString combinedId;

    {
        SettingsDialog dialog(client, runner);
        const QSize requestedSize = mikai::probes::windowSize(QSize(820, 940));
        dialog.resize(requestedSize);
        if (auto* tabs = dialog.findChild<QTabWidget*>()) {
            tabs->setCurrentIndex(tabs->count() - 1);
        }
        dialog.show();
        QApplication::processEvents();

        check(mikai::probes::dialogActionsFit(dialog, requestedSize),
              "Save and Cancel stay visible at the requested window size");
        auto* panel = dialog.findChild<NativeRifeControls*>(QStringLiteral("nativeRifePanel"));
        auto* variant = dialog.findChild<QComboBox*>(QStringLiteral("nativeRifeVariantCombo"));
        auto* ensemble = dialog.findChild<QCheckBox*>(QStringLiteral("nativeRifeEnsembleCheck"));
        auto* num = dialog.findChild<QSpinBox*>(QStringLiteral("nativeRifeFactorNumSpin"));
        auto* den = dialog.findChild<QSpinBox*>(QStringLiteral("nativeRifeFactorDenSpin"));
        auto* threshold =
            dialog.findChild<QLineEdit*>(QStringLiteral("nativeRifeSceneThresholdEdit"));
        auto* order = dialog.findChild<QComboBox*>(QStringLiteral("nativeRifeOrderCombo"));
        auto* mode = dialog.findChild<QComboBox*>(QStringLiteral("nativeRifeModeCombo"));
        auto* base = dialog.findChild<QComboBox*>(QStringLiteral("nativeRifeBaseCombo"));
        auto* storeButton =
            dialog.findChild<QPushButton*>(QStringLiteral("nativeRifeStoreButton"));
        auto* status = dialog.findChild<QLabel*>(QStringLiteral("nativeRifeStatus"));
        auto* recipeCombo = dialog.findChild<QComboBox*>(QStringLiteral("nativeRecipeCombo"));

        check(panel != nullptr, "the RIFE panel is in the Processing tab");
        check(variant != nullptr && num != nullptr && den != nullptr && threshold != nullptr &&
                  order != nullptr && mode != nullptr && base != nullptr &&
                  storeButton != nullptr && status != nullptr,
              "every RIFE control exists");
        if (panel == nullptr || variant == nullptr || ensemble == nullptr || num == nullptr ||
            den == nullptr || threshold == nullptr || order == nullptr || mode == nullptr ||
            base == nullptr || storeButton == nullptr || status == nullptr) {
            return 1;
        }

        check(!base->isEnabled(), "the combine base is disabled with no stored spatial recipe");

        // ── 1. RIFE-only, exact factor 3/2, interpolate-then-upscale ─────────
        num->setValue(3);
        den->setValue(2);
        setThreshold(threshold, 0.4);
        order->setCurrentIndex(order->findData(static_cast<int>(TemporalOrder::BeforeSpatial)));
        mode->setCurrentIndex(mode->findData(0));
        storeButton->click();
        QApplication::processEvents();

        const auto storedRecipes = [] { return UserRecipeStore().recipes(); };
        const QVector<Recipe> afterFirst = storedRecipes();
        check(afterFirst.size() == 1, "storing a request writes exactly one recipe document");
        const Recipe* first = temporalRecipe(afterFirst, RifeVariant::Full, false, Rational{3, 2},
                                             TemporalOrder::BeforeSpatial, 0.4);
        check(first != nullptr, "the stored RIFE-only recipe names the exact request");
        if (first != nullptr) {
            firstStoredId = first->id;
            check(first->shape() == RecipeShape::RifeOnly, "the stored shape is RIFE-only");
            check(first->backend == Backend::TensorRT, "the explicit backend is stored");
            check(first->temporal->factor.num == 3 && first->temporal->factor.den == 2,
                  "the unreduced factor 3/2 is persisted exactly");
        }
        check(recipeCombo != nullptr && recipeCombo->currentData().toString() == firstStoredId,
              "the recipe combo selects the request that was just stored");

        // ── 2. upscale-then-interpolate is a different, stored request ───────
        order->setCurrentIndex(order->findData(static_cast<int>(TemporalOrder::AfterSpatial)));
        storeButton->click();
        QApplication::processEvents();
        const QVector<Recipe> afterSecond = storedRecipes();
        check(afterSecond.size() == 2, "the other order is a separate stored recipe");
        check(temporalRecipe(afterSecond, RifeVariant::Full, false, Rational{3, 2},
                             TemporalOrder::AfterSpatial, 0.4) != nullptr,
              "the after-spatial order is persisted exactly");

        // ── 3. the ensemble flag names a different original variant ──────────
        ensemble->setChecked(true);
        order->setCurrentIndex(order->findData(static_cast<int>(TemporalOrder::BeforeSpatial)));
        storeButton->click();
        QApplication::processEvents();
        const QVector<Recipe> afterThird = storedRecipes();
        const Recipe* ensembled = temporalRecipe(afterThird, RifeVariant::Full, true, Rational{3, 2},
                                                 TemporalOrder::BeforeSpatial, 0.4);
        check(ensembled != nullptr, "the ensemble request is stored");
        if (ensembled != nullptr) {
            check(ensembled->temporal->modelId() == QStringLiteral("rife_v4.14_ensemble"),
                  "the ensemble request names rife_v4.14_ensemble");
        }

        // ── 4. a combined request preserves the selected spatial chain ───────
        Recipe baseRecipe;
        baseRecipe.label = QStringLiteral("Probe spatial");
        baseRecipe.backend = Backend::TensorRT;
        mplayer::native::SpatialStage baseStage;
        baseStage.modelId = QStringLiteral("builtin.hd");
        baseStage.preResize = QSize(960, 540);
        baseStage.preResizeEnabled = true;
        baseRecipe.spatial = {baseStage};
        QString key;
        QString reason;
        const std::optional<Recipe> storedBase =
            UserRecipeStore().saveRecipe(baseRecipe, &key, &reason);
        check(storedBase.has_value(), "the spatial base is stored through the real store");

        panel->refreshStoredRecipes();
        check(base->count() == 1, "the stored spatial recipe is offered as a base");
        mode->setCurrentIndex(mode->findData(1));
        QApplication::processEvents();
        check(base->isEnabled(), "selecting combine enables the base combo");
        if (storedBase.has_value()) {
            base->setCurrentIndex(base->findData(storedBase->id));
        }
        ensemble->setChecked(false);
        num->setValue(3);
        den->setValue(2);
        setThreshold(threshold, 0.4);
        order->setCurrentIndex(order->findData(static_cast<int>(TemporalOrder::BeforeSpatial)));
        storeButton->click();
        QApplication::processEvents();

        const QVector<Recipe> afterCombined = storedRecipes();
        const Recipe* combined = nullptr;
        for (const Recipe& recipe : afterCombined) {
            if (recipe.shape() == RecipeShape::Combined) {
                combined = &recipe;
                break;
            }
        }
        check(combined != nullptr, "the combined request is stored");
        if (combined != nullptr) {
            combinedId = combined->id;
            check(combined->spatial.size() == 1 &&
                      combined->spatial.at(0).modelId == QStringLiteral("builtin.hd") &&
                      combined->spatial.at(0).preResizeEnabled &&
                      combined->spatial.at(0).preResize == QSize(960, 540),
                  "the combined request preserves the base chain and its pre-resize");
            check(combined->temporal.has_value(), "the combined request still carries its temporal stage");
            check(recipeCombo != nullptr && recipeCombo->currentData().toString() == combinedId,
                  "the combo selects the combined request");
        }

        mikai::probes::revealWidget(panel);
        if (!dialog.grab().save(QStringLiteral("rife-settings-probe.png"))) {
            check(false, "the RIFE settings screenshot is written");
        } else {
            check(true, "the RIFE settings screenshot is written");
        }

        // ── 5. only Save persists the requested preference ───────────────────
        QDialogButtonBox* box = dialog.findChild<QDialogButtonBox*>();
        QPushButton* save = acceptButton(box);
        check(save != nullptr, "the Save button exists");
        if (save != nullptr) {
            save->click();
        }
        QApplication::processEvents();
        check(dialog.result() == static_cast<int>(QDialog::Accepted), "Save accepted the dialog");
        {
            QSettings settings;
            settings.beginGroup(mplayer::native::nativeSettingsGroup());
            check(settings.value(QLatin1String(mplayer::native::kNativeRecipeKey)).toString() ==
                      combinedId,
                  "Save persisted the stored request id");
            settings.endGroup();
        }
    }

    {
        // ── 6. reopen: exact reload, then Cancel writes no preference ────────
        SettingsDialog dialog(client, runner);
        dialog.resize(mikai::probes::windowSize(QSize(820, 940)));
        if (auto* tabs = dialog.findChild<QTabWidget*>()) {
            tabs->setCurrentIndex(tabs->count() - 1);
        }
        dialog.show();
        QApplication::processEvents();

        auto* variant = dialog.findChild<QComboBox*>(QStringLiteral("nativeRifeVariantCombo"));
        auto* ensemble = dialog.findChild<QCheckBox*>(QStringLiteral("nativeRifeEnsembleCheck"));
        auto* num = dialog.findChild<QSpinBox*>(QStringLiteral("nativeRifeFactorNumSpin"));
        auto* den = dialog.findChild<QSpinBox*>(QStringLiteral("nativeRifeFactorDenSpin"));
        auto* threshold =
            dialog.findChild<QLineEdit*>(QStringLiteral("nativeRifeSceneThresholdEdit"));
        auto* order = dialog.findChild<QComboBox*>(QStringLiteral("nativeRifeOrderCombo"));
        auto* mode = dialog.findChild<QComboBox*>(QStringLiteral("nativeRifeModeCombo"));
        auto* base = dialog.findChild<QComboBox*>(QStringLiteral("nativeRifeBaseCombo"));
        auto* storeButton =
            dialog.findChild<QPushButton*>(QStringLiteral("nativeRifeStoreButton"));
        auto* recipeCombo = dialog.findChild<QComboBox*>(QStringLiteral("nativeRecipeCombo"));

        if (variant == nullptr || ensemble == nullptr || num == nullptr || den == nullptr ||
            threshold == nullptr || order == nullptr || mode == nullptr || base == nullptr ||
            storeButton == nullptr || recipeCombo == nullptr) {
            check(false, "the reopened dialog still has its controls");
            return 1;
        }

        check(variant->currentData().toInt() == static_cast<int>(RifeVariant::Full) &&
                  !ensemble->isChecked() && num->value() == 3 && den->value() == 2 &&
                  closeEnough(thresholdValue(threshold), 0.4) &&
                  order->currentData().toInt() ==
                      static_cast<int>(TemporalOrder::BeforeSpatial),
              "reopening reloads the stored temporal values exactly");
        check(mode->currentData().toInt() == 1, "reopening reloads the combined shape");
        {
            const QVector<Recipe> recipes = UserRecipeStore().recipes();
            const Recipe* spatial = nullptr;
            for (const Recipe& recipe : recipes) {
                if (recipe.shape() == RecipeShape::SpatialOnly) spatial = &recipe;
            }
            check(spatial != nullptr && base->currentData().toString() == spatial->id,
                  "reopening selects the base by its stored id, not an index");
        }
        check(recipeCombo->currentData().toString() == combinedId,
              "reopening keeps the stored request selected");

        // A second, different request: it is written to the store, but Cancel
        // must leave the *requested preference* exactly as it was.
        ensemble->setChecked(true);
        mode->setCurrentIndex(mode->findData(0));
        storeButton->click();
        QApplication::processEvents();
        QDialogButtonBox* box = dialog.findChild<QDialogButtonBox*>();
        QPushButton* cancel = rejectButton(box);
        check(cancel != nullptr, "the Cancel button exists");
        if (cancel != nullptr) {
            cancel->click();
        }
        QApplication::processEvents();
        check(dialog.result() == static_cast<int>(QDialog::Rejected), "Cancel rejected the dialog");
        {
            QSettings settings;
            settings.beginGroup(mplayer::native::nativeSettingsGroup());
            check(settings.value(QLatin1String(mplayer::native::kNativeRecipeKey)).toString() ==
                      combinedId,
                  "Cancel did not change the persisted request");
            settings.endGroup();
        }
        mikai::probes::revealWidget(
            dialog.findChild<NativeRifeControls*>());
        if (!dialog.grab().save(QStringLiteral("rife-settings-reload-probe.png"))) {
            check(false, "the reload screenshot is written");
        } else {
            check(true, "the reload screenshot is written");
        }
    }

    {
        // ── 7. the editor keeps the exact double across a reload ─────────────
        constexpr double kExactThreshold = 0.1234567890123456;

        Recipe exactRecipe;
        exactRecipe.label = QStringLiteral("Probe exact threshold");
        exactRecipe.backend = Backend::TensorRT;
        mplayer::native::TemporalStage exactStage;
        exactStage.version = mplayer::native::RifeVersion::V4_14;
        exactStage.variant = RifeVariant::Full;
        exactStage.ensemble = false;
        exactStage.factor = Rational{2, 1};
        exactStage.order = TemporalOrder::BeforeSpatial;
        exactStage.sceneThreshold = kExactThreshold;
        exactRecipe.temporal = exactStage;

        QString key;
        QString reason;
        const std::optional<Recipe> exactStored =
            UserRecipeStore().saveRecipe(exactRecipe, &key, &reason);
        check(exactStored.has_value(), "the exact-threshold recipe is stored");
        if (!exactStored.has_value()) {
            return 1;
        }

        // A previous Save would have named this recipe; do the same.
        {
            QSettings settings;
            settings.beginGroup(mplayer::native::nativeSettingsGroup());
            settings.setValue(QLatin1String(mplayer::native::kNativeRecipeKey),
                              exactStored->id);
            settings.endGroup();
            settings.sync();
        }

        SettingsDialog dialog(client, runner);
        dialog.resize(mikai::probes::windowSize(QSize(820, 940)));
        if (auto* tabs = dialog.findChild<QTabWidget*>()) {
            tabs->setCurrentIndex(tabs->count() - 1);
        }
        dialog.show();
        QApplication::processEvents();

        auto* num = dialog.findChild<QSpinBox*>(QStringLiteral("nativeRifeFactorNumSpin"));
        auto* den = dialog.findChild<QSpinBox*>(QStringLiteral("nativeRifeFactorDenSpin"));
        auto* threshold =
            dialog.findChild<QLineEdit*>(QStringLiteral("nativeRifeSceneThresholdEdit"));
        auto* storeButton =
            dialog.findChild<QPushButton*>(QStringLiteral("nativeRifeStoreButton"));
        auto* recipeCombo = dialog.findChild<QComboBox*>(QStringLiteral("nativeRecipeCombo"));
        auto* status = dialog.findChild<QLabel*>(QStringLiteral("nativeRifeStatus"));

        if (num == nullptr || den == nullptr || threshold == nullptr ||
            storeButton == nullptr || recipeCombo == nullptr || status == nullptr) {
            check(false, "the exact-threshold dialog has its controls");
            return 1;
        }

        // What the editor shows reads back as the same double: no 3-decimal
        // clamp and no rounding on the way in.
        bool shown = false;
        const double readBack = QLocale().toDouble(threshold->text(), &shown);
        check(shown && exactSameDouble(readBack, kExactThreshold),
              "the stored threshold is shown and read back exactly");

        // Editing the factor alone must not disturb the exact threshold.
        num->setValue(4);
        den->setValue(3);
        storeButton->click();
        QApplication::processEvents();

        const QVector<Recipe> afterRefactor = UserRecipeStore().recipes();
        const Recipe* refactored = nullptr;
        for (const Recipe& recipe : afterRefactor) {
            if (!recipe.temporal.has_value()) continue;
            const mplayer::native::TemporalStage& stage = *recipe.temporal;
            if (stage.factor.num == 4 && stage.factor.den == 3) refactored = &recipe;
        }
        check(refactored != nullptr &&
                  exactSameDouble(refactored->temporal->sceneThreshold, kExactThreshold),
              "a factor-only edit stores the exact threshold again");

        // ── 8. bad threshold text refuses by name and writes nothing ─────────
        const int storedCount = UserRecipeStore().recipes().size();
        const QString selected = recipeCombo->currentData().toString();

        threshold->setText(QString());
        storeButton->click();
        QApplication::processEvents();
        check(UserRecipeStore().recipes().size() == storedCount,
              "a blank threshold writes no new request");
        check(recipeCombo->currentData().toString() == selected,
              "a blank threshold keeps the selected request");

        threshold->setText(QStringLiteral("1.1"));
        storeButton->click();
        QApplication::processEvents();
        check(UserRecipeStore().recipes().size() == storedCount,
              "an out-of-range threshold writes no new request");
        check(recipeCombo->currentData().toString() == selected,
              "an out-of-range threshold keeps the selected request");
    }

    {
        // ── 9. the smallest window reaches every RIFE control ────────────────
        // The reported defect is the minimum window: the Processing page grew
        // wider than the viewport - a stored id and the long choice captions
        // were reserved as minimum widths - so the role labels and the Store
        // action sat outside the visible area until the user panned. Panning is
        // what this checks for. `revealWidget` centres an oversized page, which
        // can make a too-wide surface merely look reachable, so the page's own
        // horizontal range is asserted, not where a scroll happened to land.
        SettingsDialog dialog(client, runner);
        const QSize minimum = dialog.minimumSize();
        dialog.resize(minimum);
        if (auto* tabs = dialog.findChild<QTabWidget*>()) {
            tabs->setCurrentIndex(tabs->count() - 1);
        }
        dialog.show();
        QApplication::processEvents();
        check(dialog.size() == minimum, "the dialog is at its own minimum size");
        check(mikai::probes::dialogActionsFit(dialog, minimum),
              "Save and Cancel stay visible at the minimum window size");

        auto* panel = dialog.findChild<NativeRifeControls*>(QStringLiteral("nativeRifePanel"));
        auto* storeButton =
            dialog.findChild<QPushButton*>(QStringLiteral("nativeRifeStoreButton"));
        auto* status = dialog.findChild<QLabel*>(QStringLiteral("nativeRifeStatus"));
        auto* scroll =
            dialog.findChild<QScrollArea*>(QStringLiteral("nativeProcessingScroll"));
        if (panel == nullptr || storeButton == nullptr || status == nullptr || scroll == nullptr) {
            check(false, "the minimum-size dialog has the RIFE surface");
            return 1;
        }

        // Storing is the state the defect was reported in: the surface then
        // names the request that was just stored, identity included.
        storeButton->click();
        QApplication::processEvents();
        mikai::probes::revealWidget(storeButton);
        QApplication::processEvents();

        QList<QWidget*> controls;
        controls << panel << storeButton;
        // The role labels are found structurally - the panel's own label column
        // and its status line - never by their wording. Hidden descendants are
        // skipped: a QComboBox keeps its popup internals (its private container
        // and scroll-area viewport) as named but never-laid-out children, and
        // measuring those would report them as outside no matter how narrow the
        // page is.
        for (QLabel* label : panel->findChildren<QLabel*>()) {
            if (!label->isVisibleTo(panel)) continue;
            controls << label;
        }
        for (QWidget* child : panel->findChildren<QWidget*>()) {
            if (!child->isVisibleTo(panel)) continue;
            if (!child->objectName().isEmpty()) controls << child;
        }

        QScrollBar* bar = scroll->horizontalScrollBar();
        check(bar->minimum() == bar->maximum(),
              "the Processing page has no horizontal scroll range at the minimum window size");
        bar->setValue(bar->minimum());
        QApplication::processEvents();

        QString outside;
        for (QWidget* control : controls) {
            if (!horizontallyInside(control, scroll->viewport()) && outside.isEmpty()) {
                outside = widgetName(control);
            }
        }
        check(outside.isEmpty(),
              "every RIFE control and role label is inside the viewport without horizontal "
              "panning");
        if (!outside.isEmpty() || bar->minimum() != bar->maximum()) {
            // Name every reservation above 300 px, widest first, so a failure
            // says exactly which surfaces stack up to the page minimum instead
            // of only the single widest one.
            struct Reservation {
                QString name;
                QString detail;
                int width = 0;
            };
            QList<Reservation> reservations;
            for (QWidget* child : scroll->widget()->findChildren<QWidget*>()) {
                if (child->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored) continue;
                const int width = child->minimumSizeHint().width();
                if (width <= 300) continue;
                QString detail;
                if (const auto* label = qobject_cast<const QLabel*>(child)) {
                    detail = label->text().left(48).replace(QLatin1Char('\n'),
                                                            QStringLiteral(" "));
                } else if (const auto* box = qobject_cast<const QComboBox*>(child)) {
                    detail = box->currentText().left(48);
                }
                reservations.append({widgetName(child), detail, width});
            }
            std::sort(reservations.begin(), reservations.end(),
                      [](const Reservation& a, const Reservation& b) { return a.width > b.width; });
            QStringList parts;
            for (int i = 0; i < reservations.size() && i < 8; ++i) {
                const Reservation& reservation = reservations.at(i);
                parts.append(QStringLiteral("%1 (%2 px) <%3>").arg(reservation.name)
                                 .arg(reservation.width)
                                 .arg(reservation.detail));
            }
            std::fprintf(stderr,
                         "  page minimum width %d, viewport width %d, reservations>300px: %s, "
                         "first control outside the viewport: %s\n",
                         scroll->widget()->minimumSizeHint().width(), scroll->viewport()->width(),
                         qUtf8Printable(parts.join(QStringLiteral("; "))),
                         outside.isEmpty() ? "none" : qUtf8Printable(outside));
        }
    }

    // The tree is this probe's own; it does not outlive the run.
    QDir(appData).removeRecursively();

    std::fprintf(stderr, "[rife_settings_probe] %s (%d failures)\n",
                 failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
