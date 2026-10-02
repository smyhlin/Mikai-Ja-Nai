// Real settings-dialog probe: builds the actual Processing tab on an isolated
// settings store and checks what it does with a stored, currently-unavailable
// native request.
//
// The one thing source reading cannot settle is whether the dialog shows the
// persisted request as *requested* rather than as applied, and whether a native
// save leaves the rest of the store alone. So this drives the real widgets: the
// tick, the backend, the recipe, the toggle and the runtime choice, then the real
// Save button, then a reopen.
//
// It never claims the native engine works. The request is unavailable in this
// build by construction, and the probe asserts the dialog says so.

#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QToolButton>
#include "ProbeUi.hpp"

#include <cstdio>

#include "backend/MikaiClient.hpp"
#include "dialogs/SettingsDialog.hpp"
#include "native/NativePrefs.hpp"
#include "native/NativeRuntimeSelection.hpp"
#include "workers/Runner.hpp"

using mikai::app::Runner;
using mikai::app::SettingsDialog;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
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

/// The longest binding a one-key field should be able to show: a full modifier
/// stack in front of a two-digit function key.
QKeySequence longestBinding() {
    return QKeySequence::fromString(QStringLiteral("Ctrl+Shift+Alt+F12"),
                                    QKeySequence::PortableText);
}

/// The clear action a line edit shows while it holds text, or nullptr.
QToolButton* clearActionFor(QLineEdit& input) {
    QToolButton* action = input.findChild<QToolButton*>();
    return action != nullptr && action->isVisibleTo(&input) ? action : nullptr;
}

/// The first preset row's field. Every key-sequence field but the named native
/// toggle is a preset row.
QKeySequenceEdit* presetField(SettingsDialog& dialog) {
    for (QKeySequenceEdit* field : dialog.findChildren<QKeySequenceEdit*>()) {
        if (field->objectName() != QLatin1String("nativeToggleShortcutEdit")) {
            return field;
        }
    }
    return nullptr;
}

/// Whether a sighted user can read the binding the field holds. Qt's own
/// accessible text interface reports the cell each character is drawn in, so the
/// check is what that interface and the field's styled area agree on, not a
/// width this probe wrote down.
bool bindingFullyVisible(QKeySequenceEdit& field) {
    QLineEdit* input = field.findChild<QLineEdit*>();
    if (input == nullptr) {
        std::fprintf(stderr, "FAIL the binding field holds no line edit\n");
        return false;
    }
    QAccessibleInterface* face = QAccessible::queryAccessibleInterface(input);
    QAccessibleTextInterface* text = face != nullptr ? face->textInterface() : nullptr;
    if (text == nullptr) {
        std::fprintf(stderr, "FAIL the binding field exposes no accessible text interface\n");
        return false;
    }
    const QString binding = input->text();
    const int count = text->characterCount();
    if (binding.isEmpty() || count != static_cast<int>(binding.size())) {
        std::fprintf(stderr, "FAIL the field holds %d characters, its text interface %d\n",
                     static_cast<int>(binding.size()), count);
        return false;
    }
    QStyleOptionFrame frame;
    frame.initFrom(input);
    const QRect drawable =
        input->style()->subElementRect(QStyle::SE_LineEditContents, &frame, input);
    int right = input->mapToGlobal(drawable.topRight()).x();
    if (const QToolButton* clear = clearActionFor(*input)) {
        right = qMin(right, clear->mapToGlobal(QPoint()).x() - 1);
    }
    const int left = input->mapToGlobal(QPoint()).x();
    for (int i = 0; i < count; ++i) {
        const QRect cell = text->characterRect(i);
        if (cell.left() < left || cell.right() > right) {
            std::fprintf(stderr, "FAIL character %d of '%s' is drawn at %d..%d, outside %d..%d\n",
                         i, qPrintable(binding), cell.left(), cell.right(), left, right);
            return false;
        }
    }
    return true;
}

/// Whether the clear action the field shows while it holds text sits inside the
/// field itself.
bool clearActionInside(QKeySequenceEdit& field) {
    QLineEdit* input = field.findChild<QLineEdit*>();
    QToolButton* clear = input != nullptr ? clearActionFor(*input) : nullptr;
    if (clear == nullptr) {
        std::fprintf(stderr, "FAIL the binding field shows no clear action\n");
        return false;
    }
    const QRect bounds(input->mapToGlobal(QPoint()), input->size());
    const QRect action(clear->mapToGlobal(QPoint()), clear->size());
    if (!bounds.contains(action)) {
        std::fprintf(stderr, "FAIL the clear action at %d..%d is outside the field %d..%d\n",
                     action.left(), action.right(), bounds.left(), bounds.right());
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    if (!mikai::probes::initializeUi(app)) return 2;
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiNativeSettingsProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("NativeSettingsProbe"));

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());

    const QString recipeId = QStringLiteral("janai.unknown.v9");
    {
        QSettings settings;
        // Foreign settings a native save must not disturb.
        settings.setValue(QStringLiteral("player/presetShortcuts/hd2x"), QStringLiteral("Ctrl+1"));
        settings.setValue(QStringLiteral("ui/language"), QStringLiteral("uk"));
        settings.setValue(QStringLiteral("quality/cache"), QStringLiteral("cache-body"));
        settings.setValue(QStringLiteral("home/feeds"), QStringLiteral("ongoing"));
        // The stored native request: an unavailable tick, a recipe the (empty)
        // catalogue cannot offer, and a backend this build does not run.
        settings.beginGroup(mplayer::native::nativeSettingsGroup());
        settings.setValue(QLatin1String(mplayer::native::kNativeSchemaKey),
                          mplayer::native::kNativePreferencesSchema);
        settings.setValue(QLatin1String(mplayer::native::kNativeEnabledKey), true);
        settings.setValue(QLatin1String(mplayer::native::kNativeBackendKey),
                          QStringLiteral("directml"));
        settings.setValue(QLatin1String(mplayer::native::kNativeRecipeKey), recipeId);
        settings.setValue(QStringLiteral("runtimeChoice"), QStringLiteral("standard"));
        settings.endGroup();
        settings.beginGroup(mplayer::native::nativeShortcutsSettingsGroup());
        settings.setValue(QLatin1String(mplayer::native::kNativeToggleProcessingKey),
                          QStringLiteral("Ctrl+Shift+J"));
        settings.endGroup();
        settings.sync();
    }

    const QString envFixturePath = qEnvironmentVariable("MIKAI_PROBE_ENV_FILE");
    QByteArray envFixtureBytes;
    if (!envFixturePath.isEmpty()) {
        const QFileInfo expected(QCoreApplication::applicationDirPath() + QStringLiteral("/.env"));
        const QFileInfo provided(envFixturePath);
        if (!provided.isFile() || provided.canonicalFilePath() != expected.canonicalFilePath()) {
            std::fprintf(stderr, "FAIL environment fixture must be .env beside isolated probe\n");
            return 2;
        }
        QFile fixture(envFixturePath);
        if (!fixture.open(QIODevice::ReadOnly)) return 2;
        envFixtureBytes = fixture.readAll();
    }

    mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
    Runner runner;

    {
        SettingsDialog dialog(client, runner);
        const QSize requestedSize = mikai::probes::windowSize(QSize(760, 780));
        dialog.resize(requestedSize);
        if (auto* tabs = dialog.findChild<QTabWidget*>()) {
            tabs->setCurrentIndex(tabs->count() - 1);
        }
        dialog.show();
        QApplication::processEvents();
        check(mikai::probes::dialogActionsFit(dialog, requestedSize),
              "Save and Cancel stay visible at the requested window size");
        if (!dialog.grab().save(QStringLiteral("native-settings-probe.png"))) {
            return 2;
        }
        auto* enabled = dialog.findChild<QCheckBox*>(QStringLiteral("nativeEnabledCheck"));
        auto* backend = dialog.findChild<QComboBox*>(QStringLiteral("nativeBackendCombo"));
        auto* recipe = dialog.findChild<QComboBox*>(QStringLiteral("nativeRecipeCombo"));
        auto* runtime = dialog.findChild<QComboBox*>(QStringLiteral("runtimeChoiceCombo"));
        auto* toggle =
            dialog.findChild<QKeySequenceEdit*>(QStringLiteral("nativeToggleShortcutEdit"));

        check(enabled != nullptr && enabled->isChecked(),
              "the stored unavailable tick is shown");
        check(backend != nullptr &&
                  backend->currentData().toInt() ==
                      static_cast<int>(mplayer::native::Backend::DirectML),
              "the stored backend is shown");
        check(recipe != nullptr && recipe->currentData().toString() == recipeId,
              "an unknown recipe stays requested");
        check(toggle != nullptr &&
                  toggle->keySequence().toString(QKeySequence::PortableText) ==
                      QStringLiteral("Ctrl+Shift+J"),
              "the stored toggle binding is shown");
        check(runtime != nullptr &&
                  runtime->currentData().toInt() ==
                      static_cast<int>(mplayer::native::RuntimeChoice::Standard),
              "the stored runtime choice is shown");

        // Changing the combo must not write anything until Save.
        if (runtime != nullptr) {
            runtime->setCurrentIndex(
                runtime->findData(static_cast<int>(mplayer::native::RuntimeChoice::Native)));
        }
        QSettings before;
        before.beginGroup(mplayer::native::nativeSettingsGroup());
        const QString choiceBefore = before.value(QStringLiteral("runtimeChoice")).toString();
        before.endGroup();
        check(choiceBefore != QStringLiteral("native"),
              "the runtime choice is not written before Save");

        auto* box = dialog.findChild<QDialogButtonBox*>();
        QPushButton* save = acceptButton(box);
        check(save != nullptr, "the Save button exists");
        if (save != nullptr) {
            save->click();
        }
        QApplication::processEvents();
        check(dialog.result() == static_cast<int>(QDialog::Accepted),
              "the Save button accepted the dialog");
    }

    if (!envFixturePath.isEmpty()) {
        QFile fixture(envFixturePath);
        const bool unchanged =
            fixture.open(QIODevice::ReadOnly) && fixture.readAll() == envFixtureBytes;
        check(unchanged, "saving native settings leaves the environment file byte-identical");
    }

    {
        QSettings settings;
        settings.beginGroup(mplayer::native::nativeSettingsGroup());
        check(settings.value(QLatin1String(mplayer::native::kNativeEnabledKey)).toBool(),
              "Save persisted the tick");
        check(settings.value(QLatin1String(mplayer::native::kNativeBackendKey)).toString() ==
                  QStringLiteral("directml"),
              "Save persisted the backend");
        check(settings.value(QLatin1String(mplayer::native::kNativeRecipeKey)).toString() ==
                  recipeId,
              "Save persisted the unknown recipe");
        check(settings.value(QStringLiteral("runtimeChoice")).toString() ==
                  QStringLiteral("native"),
              "Save persisted the runtime choice");
        settings.endGroup();
        settings.beginGroup(mplayer::native::nativeShortcutsSettingsGroup());
        check(settings.value(QLatin1String(mplayer::native::kNativeToggleProcessingKey))
                      .toString() == QStringLiteral("Ctrl+Shift+J"),
              "Save persisted the toggle binding");
        settings.endGroup();

        check(settings.value(QStringLiteral("player/presetShortcuts/hd2x")).toString() ==
                  QStringLiteral("Ctrl+1"),
              "the preset binding is untouched");
        check(settings.value(QStringLiteral("ui/language")).toString() == QStringLiteral("uk"),
              "the language is untouched");
        check(settings.value(QStringLiteral("quality/cache")).toString() ==
                  QStringLiteral("cache-body"),
              "the quality cache is untouched");
        check(settings.value(QStringLiteral("home/feeds")).toString() ==
                  QStringLiteral("ongoing"),
              "the feed selection is untouched");
    }

    {
        SettingsDialog dialog(client, runner);
        auto* enabled = dialog.findChild<QCheckBox*>(QStringLiteral("nativeEnabledCheck"));
        auto* recipe = dialog.findChild<QComboBox*>(QStringLiteral("nativeRecipeCombo"));
        auto* runtime = dialog.findChild<QComboBox*>(QStringLiteral("runtimeChoiceCombo"));
        check(enabled != nullptr && enabled->isChecked(),
              "reopening shows the stored tick again");
        check(recipe != nullptr && recipe->currentData().toString() == recipeId,
              "reopening keeps the requested recipe");
        check(runtime != nullptr &&
                  runtime->currentData().toInt() ==
                      static_cast<int>(mplayer::native::RuntimeChoice::Native),
              "reopening shows the stored runtime choice");
    }

    // The shared shortcut field width, on the two kinds of field that use it. Each
    // is driven to the longest combination the dialog accepts, so a field that
    // cannot show it says so here.
    {
        SettingsDialog dialog(client, runner);
        dialog.resize(mikai::probes::windowSize(QSize(760, 780)));
        auto* toggle =
            dialog.findChild<QKeySequenceEdit*>(QStringLiteral("nativeToggleShortcutEdit"));
        if (auto* tabs = dialog.findChild<QTabWidget*>()) {
            tabs->setCurrentIndex(tabs->count() - 1);
        }
        if (toggle != nullptr) {
            toggle->setKeySequence(longestBinding());
        }
        dialog.show();
        QApplication::processEvents();
        mikai::probes::revealWidget(toggle);
        check(toggle != nullptr, "the native shortcut field is listed");
        if (toggle != nullptr) {
            check(toggle->keySequence() == longestBinding(),
                  "the native field holds the longest binding");
            check(bindingFullyVisible(*toggle),
                  "the native field shows every character of the longest binding");
            check(clearActionInside(*toggle), "the native clear action is inside the field");
        }
    }

    {
        SettingsDialog dialog(client, runner);
        dialog.resize(mikai::probes::windowSize(QSize(760, 780)));
        if (auto* tabs = dialog.findChild<QTabWidget*>()) {
            tabs->setCurrentIndex(1);  // the Presets tab, where the preset rows live
        }
        QKeySequenceEdit* preset = presetField(dialog);
        if (preset != nullptr) {
            preset->setKeySequence(longestBinding());
        }
        dialog.show();
        QApplication::processEvents();
        mikai::probes::revealWidget(preset);
        check(preset != nullptr, "a preset shortcut field is listed");
        if (preset != nullptr) {
            check(preset->keySequence() == longestBinding(),
                  "the preset field holds the longest binding");
            check(bindingFullyVisible(*preset),
                  "the preset field shows every character of the longest binding");
            check(clearActionInside(*preset), "the preset clear action is inside the field");
        }
    }

    std::fprintf(stderr, "[native_settings_probe] %s (%d failures)\n",
                 failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
