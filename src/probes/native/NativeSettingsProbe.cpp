
#include <QAccessible>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QGroupBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QStandardPaths>
#include <QThread>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QToolButton>
#include <QUuid>
#include "ProbeUi.hpp"

#include <cstdio>

#include "I18n.hpp"
#include "backend/MikaiClient.hpp"
#include "theme/Metrics.hpp"
#include "dialogs/SettingsDialog.hpp"
#include "dialogs/ComponentStoreTab.hpp"
#include "native/bundle/NativePrefs.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "native/components/NativeGpuProbe.hpp"
#include "native/models/NativeRife.hpp"
#include "native/models/NativeSpatialAssets.hpp"
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
QString fileSha256(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}
QSize fitSettingsDialogSize(const SettingsDialog& dialog, QSize requestedSize) {
    const QScreen* screen = dialog.screen();
    return screen == nullptr ? requestedSize
                             : requestedSize.boundedTo(screen->availableGeometry().size());
}


bool waitForUi(const auto& predicate, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        QApplication::processEvents();
        if (predicate()) return true;
        QThread::msleep(10);
    }
    QApplication::processEvents();
    return predicate();
}

QLabel* statusSeverityLabel(QWidget& root) {
    for (QLabel* label : root.findChildren<QLabel*>()) {
        const QString severity = label->accessibleDescription();
        if (severity == QLatin1String("success") || severity == QLatin1String("failure") ||
            severity == QLatin1String("cancelled") || severity == QLatin1String("neutral"))
            return label;
    }
    return nullptr;
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

QKeySequenceEdit* presetField(SettingsDialog& dialog) {
    auto* tabs = dialog.findChild<QTabWidget*>();
    if (tabs == nullptr) return nullptr;
    const QStringList parts =
        SettingsDialog::tr("Shortcut for %1").split(QStringLiteral("%1"));
    if (parts.size() != 2) return nullptr;
    for (int index = 0; index < tabs->count(); ++index) {
        if (tabs->tabText(index) != SettingsDialog::tr("Presets")) continue;
        for (QKeySequenceEdit* field : tabs->widget(index)->findChildren<QKeySequenceEdit*>()) {
            const QString name = field->accessibleName();
            if (name.startsWith(parts.at(0)) && name.endsWith(parts.at(1)) &&
                name.size() > parts.at(0).size() + parts.at(1).size()) {
                return field;
            }
        }
    }
    return nullptr;
}

QWidget* shortcutTabPage(QTabWidget& tabs, QWidget& field) {
    for (int index = 0; index < tabs.count(); ++index) {
        QWidget* page = tabs.widget(index);
        if (page == &field || page->isAncestorOf(&field)) return page;
    }
    return nullptr;
}

bool showShortcutField(SettingsDialog& dialog, QKeySequenceEdit* field) {
    auto* tabs = dialog.findChild<QTabWidget*>();
    if (tabs == nullptr || field == nullptr) return false;
    QWidget* page = shortcutTabPage(*tabs, *field);
    if (page == nullptr) return false;
    tabs->setCurrentWidget(page);
    dialog.show();
    QApplication::processEvents();
    if (page->layout() != nullptr) page->layout()->activate();
    mikai::probes::revealWidget(field);
    QApplication::processEvents();
    return tabs->currentWidget() == page && page->isVisibleTo(&dialog) &&
           field->isVisibleTo(&dialog);
}

void shortcutDiagnostics(SettingsDialog& dialog, QKeySequenceEdit& field) {
    auto* tabs = dialog.findChild<QTabWidget*>();
    QLineEdit* input = field.findChild<QLineEdit*>();
    if (input == nullptr) return;
    QStyleOptionFrame frame;
    frame.initFrom(input);
    const QRect contents =
        input->style()->subElementRect(QStyle::SE_LineEditContents, &frame, input);
    QToolButton* clear = clearActionFor(*input);
    const QFontMetrics metrics(input->font());
    const QString binding = input->text();
    const QString tabCaption = tabs != nullptr
        ? tabs->tabText(tabs->currentIndex()) : QStringLiteral("<no tabs>");
    std::fprintf(stderr,
                 "[native_settings_probe] preset shortcut name=%s tab=%s visible=%d field=%d,%d %dx%d line-edit=%d,%d %dx%d contents=%d,%d %dx%d clear=%s rect=%d,%d %dx%d font=%s text-width=%d capture=native-settings-preset-shortcut-failure.png\n",
                 qPrintable(field.accessibleName()), qPrintable(tabCaption),
                 field.isVisibleTo(&dialog) ? 1 : 0, field.x(), field.y(),
                 field.width(), field.height(), input->x(), input->y(),
                 input->width(), input->height(), contents.x(), contents.y(),
                 contents.width(), contents.height(), clear != nullptr ? "yes" : "no",
                 clear != nullptr ? clear->x() : -1, clear != nullptr ? clear->y() : -1,
                 clear != nullptr ? clear->width() : 0, clear != nullptr ? clear->height() : 0,
                 qPrintable(input->font().toString()), metrics.horizontalAdvance(binding));
    dialog.grab().save(QStringLiteral("native-settings-preset-shortcut-failure.png"));
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

bool componentCardReachable(QWidget& componentTab, const QString& translatedTitle) {
    for (QLabel* label : componentTab.findChildren<QLabel*>()) {
        if (label->text() == translatedTitle && label->isVisibleTo(&componentTab))
            return true;
    }
    return false;
}

QComboBox* backendSelector(QWidget& componentTab) {
    return componentTab.findChild<QComboBox*>(QStringLiteral("animeJanaiRuntimeBackend"));
}

QString backendIdentity(mplayer::native::Backend backend) {
    return backend == mplayer::native::Backend::TensorRT
        ? QStringLiteral("tensorrt") : QStringLiteral("directml");
}

QPushButton* accessibleAction(QWidget& root, const QString& actionName) {
    for (QPushButton* button : root.findChildren<QPushButton*>()) {
        QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(button);
        if (button->isVisibleTo(&root) && accessible != nullptr &&
            (accessible->text(QAccessible::Name) == actionName ||
             button->text() == actionName))
            return button;
    }
    return nullptr;
}

void exerciseProcessing(SettingsDialog& dialog, mikai::backend::MikaiClient& client,
                        Runner& runner, QSize requestedSize, const QString& language) {
    QSettings before;
    const QString nativeEnabledBefore =
        before.value(QStringLiteral("player/native/enabled")).toString();
    const QString nativeBackendBefore =
        before.value(QStringLiteral("player/native/backend")).toString();
    const QString nativeRecipeBefore =
        before.value(QStringLiteral("player/native/recipe")).toString();
    const QString runtimeBefore =
        before.value(QStringLiteral("player/native/runtimeChoice")).toString();
    const QString customModelsDirBefore =
        before.value(QStringLiteral("player/native/customModelsDir")).toString();
    const QString shortcutBefore =
        before.value(QStringLiteral("player/presetShortcuts/hd2x")).toString();
    const QString cacheBefore = before.value(QStringLiteral("quality/cache")).toString();

    auto* runtime = dialog.findChild<QComboBox*>(QStringLiteral("runtimeChoiceCombo"));
    auto* privateAssets = dialog.findChild<QWidget*>(QStringLiteral("nativeAssetsPanel"));
    if (runtime != nullptr) {
        runtime->setCurrentIndex(
            runtime->findData(static_cast<int>(mplayer::native::RuntimeChoice::Standard)));
    }
    requestedSize = fitSettingsDialogSize(dialog, requestedSize);
    dialog.resize(requestedSize);
    dialog.showProcessingTab();
    dialog.show();
    QApplication::processEvents();
    check(mikai::probes::dialogActionsFit(dialog, requestedSize),
          "Processing Save and Cancel stay visible at the minimum window");
    auto* processing = dialog.findChild<QWidget*>(QStringLiteral("nativeProcessingScroll"));
    check(processing != nullptr && processing->isVisibleTo(&dialog),
          "the real Processing page is visible");
    if (processing != nullptr) {
        const QString suffix =
            requestedSize.width() <= mikai::app::theme::kSettingsDialogMinWidth
                ? QStringLiteral("compact")
                : QStringLiteral("wide");
        check(dialog.grab().save(
                  QStringLiteral("native-processing-%1-%2.png").arg(language, suffix)),
              "the visible Processing page screenshot was saved");
    }

    QApplication::processEvents();
    check(privateAssets != nullptr && !privateAssets->isVisibleTo(&dialog),
          "the Standard Processing screenshot contains no private Native editor");
    if (runtime != nullptr) {
        runtime->setCurrentIndex(
            runtime->findData(static_cast<int>(mplayer::native::RuntimeChoice::Native)));
    }
    QApplication::processEvents();
    check(privateAssets != nullptr && privateAssets->isVisibleTo(&dialog),
          "choosing Native deliberately exposes the private workflow");
    dialog.reject();
    QApplication::processEvents();

    QSettings after;
    check(after.value(QStringLiteral("player/native/enabled")).toString() == nativeEnabledBefore &&
              after.value(QStringLiteral("player/native/backend")).toString() == nativeBackendBefore &&
              after.value(QStringLiteral("player/native/recipe")).toString() == nativeRecipeBefore &&
              after.value(QStringLiteral("player/native/runtimeChoice")).toString() == runtimeBefore &&
              after.value(QStringLiteral("player/native/customModelsDir")).toString() ==
                  customModelsDirBefore,
          "Cancel preserves saved Native requests and the custom model directory");
    check(after.value(QStringLiteral("player/presetShortcuts/hd2x")).toString() ==
                  shortcutBefore &&
              after.value(QStringLiteral("quality/cache")).toString() == cacheBefore,
          "Cancel preserves foreign settings data");

    {
        QString runtimeReason;
        if (!mplayer::native::storeRuntimeChoice(
                mplayer::native::RuntimeChoice::Standard, &runtimeReason)) {
            std::fprintf(stderr,
                         "[native_settings_probe] could not seed Standard runtime choice: %s\n",
                         qPrintable(runtimeReason));
        }
    }
    {
        SettingsDialog dialog(client, runner);
        dialog.showProcessingTab();
        dialog.show();
        QApplication::processEvents();
        auto* runtime = dialog.findChild<QComboBox*>(QStringLiteral("runtimeChoiceCombo"));
        auto* privateAssets = dialog.findChild<QWidget*>(QStringLiteral("nativeAssetsPanel"));
        check(runtime != nullptr &&
                  runtime->currentData().toInt() ==
                      static_cast<int>(mplayer::native::RuntimeChoice::Standard),
              "Standard remains the selected runtime for the hidden-form Save check");
        check(privateAssets != nullptr && !privateAssets->isVisibleTo(&dialog),
              "private Native controls remain hidden for a Standard Save");
        QPushButton* save = acceptButton(dialog.findChild<QDialogButtonBox*>());
        if (save != nullptr) save->click();
        QApplication::processEvents();
    }
    QSettings afterStandardSave;
    check(afterStandardSave.value(QStringLiteral("player/native/enabled")).toString() ==
                  nativeEnabledBefore &&
              afterStandardSave.value(QStringLiteral("player/native/backend")).toString() ==
                  nativeBackendBefore &&
              afterStandardSave.value(QStringLiteral("player/native/recipe")).toString() ==
                  nativeRecipeBefore &&
              afterStandardSave.value(QStringLiteral("player/native/customModelsDir")).toString() ==
                  customModelsDirBefore,
          "Standard Save preserves hidden private Native preferences");
}

void exerciseComponents(SettingsDialog& dialog, QSize requestedSize, const QString& language) {
    requestedSize = fitSettingsDialogSize(dialog, requestedSize);
    dialog.resize(requestedSize);
    dialog.showComponentsTab();
    dialog.show();
    QApplication::processEvents();

    auto* componentTab = dialog.findChild<mikai::app::ComponentStoreTab*>();
    check(componentTab != nullptr && componentTab->isVisibleTo(&dialog),
          "the real Components tab is reachable");
    if (componentTab == nullptr) return;

    componentTab->refresh();
    QApplication::processEvents();
    QComboBox* backend = backendSelector(*componentTab);
    QPushButton* install = accessibleAction(
        *componentTab,
        QCoreApplication::translate("mikai::app::ComponentStoreTab", "Apply changes"));
    check(backend != nullptr && backend->isVisibleTo(componentTab),
          "the backend selector is reachable");
    check(install != nullptr,
          "the Apply changes action is reachable without starting a download");
    if (backend == nullptr) return;
    const int directMl = backend->findData(
        static_cast<int>(mplayer::native::Backend::DirectML));
    check(directMl >= 0, "the backend selector includes DirectML");
    if (directMl >= 0) backend->setCurrentIndex(directMl);

    check(componentCardReachable(
              *componentTab,
              QCoreApplication::translate("mikai::app::ComponentStoreTab",
                                          "TensorRT runtime")),
          "the TensorRT runtime card title is reachable");
    check(componentCardReachable(
              *componentTab,
              QCoreApplication::translate("mikai::app::ComponentStoreTab",
                                          "TensorRT kernels for this GPU")),
          "the TensorRT GPU kernels card title is reachable");
    check(componentCardReachable(
              *componentTab,
              QCoreApplication::translate("mikai::app::ComponentStoreTab",
                                          "RIFE interpolation models")),
          "the RIFE card title is reachable");
    const QString screenshot =
        QStringLiteral("native-components-%1-%2.png")
            .arg(language, requestedSize.width() <= mikai::app::theme::kSettingsDialogMinWidth
                               ? QStringLiteral("compact")
                               : QStringLiteral("wide"));
    check(dialog.grab().save(screenshot),
          "the Components surface screenshot was saved");
}

void exerciseVerifyBackendIsolation(SettingsDialog& dialog,
                                    mplayer::native::Backend selected,
                                    mplayer::native::Backend inactive,
                                    bool installRuntime) {
    dialog.showComponentsTab();
    dialog.show();
    QApplication::processEvents();

    auto* componentTab = dialog.findChild<mikai::app::ComponentStoreTab*>();
    if (componentTab == nullptr) {
        check(false, "the real Components tab is reachable for Verify");
        return;
    }
    componentTab->refresh();
    QApplication::processEvents();
    QComboBox* backend = backendSelector(*componentTab);
    QPushButton* verify = accessibleAction(
        *componentTab, QCoreApplication::translate("mikai::app::ComponentStoreTab", "Verify"));
    check(backend != nullptr && verify != nullptr,
          "the public Components surface exposes backend choice and Verify");
    if (backend == nullptr || verify == nullptr) return;
    const QString statusSeverityBefore =
        statusSeverityLabel(*componentTab) != nullptr
            ? statusSeverityLabel(*componentTab)->accessibleDescription()
            : QString();
    const QString statusReasonBefore = componentTab->statusReason();
    const int selectedIndex = backend->findData(static_cast<int>(selected));
    check(selectedIndex >= 0, "the absent backend is selectable through the public control");
    if (selectedIndex < 0) return;
    backend->setCurrentIndex(selectedIndex);
    QApplication::processEvents();
    check(backend->currentData().toInt() == static_cast<int>(selected),
          "the selected backend survives Components refresh");
    verify->click();
    const bool verifyCompleted = waitForUi([&] {
        QLabel* currentStatus = statusSeverityLabel(*componentTab);
        return verify->isEnabled() && currentStatus != nullptr &&
               (currentStatus->accessibleDescription() != statusSeverityBefore ||
                componentTab->statusReason() != statusReasonBefore);
    }, 5000);
    QLabel* status = statusSeverityLabel(*componentTab);
    check(verifyCompleted, "Verify completion was observed through the Components status surface");
    check(status != nullptr &&
              status->accessibleDescription() == QLatin1String("success"),
          "Verify reports success for the selected backend despite an inactive corrupt record");

    if (installRuntime) {
        QPushButton* install = accessibleAction(
            *componentTab,
            QCoreApplication::translate("mikai::app::ComponentStoreTab", "Apply changes"));
        check(install != nullptr,
              "the public Apply changes action is available for the opt-in smoke");
        if (install != nullptr) {
            check(backend->currentData().toInt() == static_cast<int>(selected),
                  "Verify preserves the selected backend before Apply changes");
            install->click();
            const auto gpuBackend = mplayer::native::observeComputeCapability().nvidia
                                        ? mplayer::native::Backend::TensorRT
                                        : mplayer::native::Backend::DirectML;
            const bool installCompleted = waitForUi([&] {
                QLabel* currentStatus = statusSeverityLabel(*componentTab);
                const QString severity = currentStatus != nullptr
                    ? currentStatus->accessibleDescription() : QString();
                return mplayer::native::describeAnimeJanaiInstallation(gpuBackend, true).installed ||
                       severity == QLatin1String("failure");
            }, 300000);
            bool cancelClicked = false;
            if (!installCompleted) {
                QPushButton* cancel = accessibleAction(
                    *componentTab,
                    QCoreApplication::translate(
                        "mikai::app::ComponentStoreTab", "Cancel the running download"));
                if (cancel != nullptr && cancel->isEnabled()) {
                    cancel->click();
                    cancelClicked = true;
                }
                waitForUi([&] {
                    QLabel* currentStatus = statusSeverityLabel(*componentTab);
                    return currentStatus == nullptr ||
                           currentStatus->accessibleDescription() ==
                               QLatin1String("cancelled") ||
                           currentStatus->accessibleDescription() ==
                               QLatin1String("failure");
                }, 60000);
            }
            const auto installed =
                mplayer::native::describeAnimeJanaiInstallation(gpuBackend, true);
            const auto inactiveInstallation =
                mplayer::native::describeAnimeJanaiInstallation(inactive, true);
            const QString recordPath =
                QDir(installed.root).filePath(QStringLiteral("install.json"));
            const bool recordExists = QFileInfo(recordPath).isFile();
            const QString recordSha256 = recordExists ? fileSha256(recordPath) : QString();
            const QString actualBackend = installed.installed
                ? backendIdentity(gpuBackend)
                : (inactiveInstallation.installed
                       ? backendIdentity(inactive)
                       : QStringLiteral("none"));
            check(backend->currentData().toInt() == static_cast<int>(gpuBackend),
                  "Apply changes selects the backend matched to this GPU");
            check(installCompleted && installed.installed && recordExists &&
                      !recordSha256.isEmpty(),
                  "the real Apply changes install produced a verified GPU-backend record");
            QLabel* finalStatus = statusSeverityLabel(*componentTab);
            const QString severity = finalStatus != nullptr
                ? finalStatus->accessibleDescription() : QStringLiteral("unavailable");
            std::fprintf(stderr,
                         "[native_settings_probe] apply-changes requested-backend=%s actual-backend=%s completed=%d status-reason=%s severity=%s cancel-clicked=%d installed=%d root=%s record=%s record-sha256=%s\\n",
                         qPrintable(backendIdentity(gpuBackend)),
                         qPrintable(actualBackend), installCompleted ? 1 : 0,
                         qPrintable(componentTab->statusReason()), qPrintable(severity),
                         cancelClicked ? 1 : 0, installed.installed ? 1 : 0,
                         qPrintable(installed.root), qPrintable(recordPath),
                         qPrintable(recordSha256));
        }
    }

    const QString screenshot = QStringLiteral("native-components-verify-isolation.png");
    check(dialog.grab().save(screenshot),
          "the selected-backend Verify result was captured");
}

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
    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption installRuntimeOption(
        QStringLiteral("install-runtime"),
        QStringLiteral("Opt in to a real reviewed DirectML runtime installation smoke."));
    parser.addOption(installRuntimeOption);
    parser.process(app);
    const bool installRuntime = parser.isSet(installRuntimeOption);
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
        // The stored native request: an unavailable recipe the (empty)
        // catalogue cannot offer, and a backend this build does not run.
        settings.beginGroup(mplayer::native::nativeSettingsGroup());
        settings.setValue(QLatin1String(mplayer::native::kNativeSchemaKey),
                          mplayer::native::kNativePreferencesSchema);
        settings.setValue(QLatin1String(mplayer::native::kNativeEnabledKey), true);
        settings.setValue(QLatin1String(mplayer::native::kNativeBackendKey),
                          QStringLiteral("directml"));
        settings.setValue(QLatin1String(mplayer::native::kNativeRecipeKey), recipeId);
        settings.setValue(QLatin1String(mplayer::native::kNativeCustomModelsDirKey),
                          QStringLiteral("foreign-model-store-path"));
        settings.setValue(QStringLiteral("runtimeChoice"), QStringLiteral("standard"));
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

    const QString probeCatalog = qEnvironmentVariable("MIKAI_PROBE_TRANSLATION");
    if (!probeCatalog.isEmpty() && QFileInfo::exists(probeCatalog)) {
        check(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), probeCatalog),
              "the supplied catalog is installed through the public language API");
    }

    mikai::backend::MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
    Runner runner;

    {
        SettingsDialog dialog(client, runner);
        const QSize requestedSize =
            fitSettingsDialogSize(dialog, mikai::probes::windowSize(QSize(760, 780)));
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
        auto* backend = dialog.findChild<QComboBox*>(QStringLiteral("nativeBackendCombo"));
        auto* recipe = dialog.findChild<QComboBox*>(QStringLiteral("nativeRecipeCombo"));
        auto* runtime = dialog.findChild<QComboBox*>(QStringLiteral("runtimeChoiceCombo"));
        auto* privateAssets = dialog.findChild<QWidget*>(QStringLiteral("nativeAssetsPanel"));
        dialog.showProcessingTab();
        QApplication::processEvents();
        check(privateAssets != nullptr && !privateAssets->isVisibleTo(&dialog),
              "Standard Processing hides the private Native editor and assets");
        check(runtime != nullptr &&
                  runtime->currentData().toInt() ==
                      static_cast<int>(mplayer::native::RuntimeChoice::Standard),
              "the stored runtime choice is shown");
        if (runtime != nullptr) {
            runtime->setCurrentIndex(
                runtime->findData(static_cast<int>(mplayer::native::RuntimeChoice::Native)));
        }
        QApplication::processEvents();
        check(privateAssets != nullptr && privateAssets->isVisibleTo(&dialog),
              "deliberately selecting Native exposes its private workflow");

        check(backend != nullptr &&
                  backend->currentData().toInt() ==
                      static_cast<int>(mplayer::native::Backend::DirectML),
              "the stored backend is shown");
        check(recipe != nullptr && recipe->currentData().toString() == recipeId,
              "an unknown recipe stays requested");

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
              "Save preserved the stored enabled request");
        check(settings.value(QLatin1String(mplayer::native::kNativeBackendKey)).toString() ==
                  QStringLiteral("directml"),
              "Save persisted the backend");
        check(settings.value(QLatin1String(mplayer::native::kNativeRecipeKey)).toString() ==
                  recipeId,
              "Save persisted the unknown recipe");
        check(settings.value(QStringLiteral("runtimeChoice")).toString() ==
                  QStringLiteral("native"),
              "Save persisted the runtime choice");
        check(settings.value(QLatin1String(mplayer::native::kNativeCustomModelsDirKey)).toString() ==
                  QStringLiteral("foreign-model-store-path"),
              "Save preserved the stored custom model directory");
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
        auto* recipe = dialog.findChild<QComboBox*>(QStringLiteral("nativeRecipeCombo"));
        auto* runtime = dialog.findChild<QComboBox*>(QStringLiteral("runtimeChoiceCombo"));
        check(recipe != nullptr && recipe->currentData().toString() == recipeId,
              "reopening keeps the requested recipe");
        check(runtime != nullptr &&
                  runtime->currentData().toInt() ==
                      static_cast<int>(mplayer::native::RuntimeChoice::Native),
              "reopening shows the stored runtime choice");
    }

    {
        QSettings settings;
        settings.setValue(QStringLiteral("player/native/enabled"), false);
        settings.setValue(QStringLiteral("quality/cache"), QStringLiteral("preserved-cache"));
        settings.sync();
    }
    {
        SettingsDialog dialog(client, runner);
        dialog.reject();
        QCoreApplication::processEvents();
    }
    {
        QSettings settings;
        check(!settings.value(QStringLiteral("player/native/enabled")).toBool(),
              "Cancel preserves the stored disabled request");
        check(settings.value(QStringLiteral("quality/cache")).toString() ==
                  QStringLiteral("preserved-cache"),
              "Cancel preserves unrelated settings with Native disabled");
    }
    {
        SettingsDialog dialog(client, runner);
        auto* backend = dialog.findChild<QComboBox*>(QStringLiteral("nativeBackendCombo"));
        auto* recipe = dialog.findChild<QComboBox*>(QStringLiteral("nativeRecipeCombo"));
        auto* runtime = dialog.findChild<QComboBox*>(QStringLiteral("runtimeChoiceCombo"));
        if (runtime != nullptr) {
            runtime->setCurrentIndex(
                runtime->findData(static_cast<int>(mplayer::native::RuntimeChoice::Native)));
        }
        if (recipe != nullptr) {
            const int noneIndex = recipe->findData(QString());
            if (noneIndex >= 0) recipe->setCurrentIndex(noneIndex);
        }
        if (backend != nullptr) {
            const int tensorRtIndex =
                backend->findData(static_cast<int>(mplayer::native::Backend::TensorRT));
            if (tensorRtIndex >= 0) backend->setCurrentIndex(tensorRtIndex);
        }

        auto* box = dialog.findChild<QDialogButtonBox*>();
        QPushButton* save = acceptButton(box);
        check(recipe != nullptr && recipe->currentData().toString().isEmpty(),
              "the stored-disabled preservation case changes the recipe");
        check(save != nullptr, "Save exists for the persisted-disabled preservation case");
        if (save != nullptr) save->click();
        QApplication::processEvents();
        check(dialog.result() == static_cast<int>(QDialog::Accepted),
              "Save accepts backend and recipe edits with Native disabled");
    }
    {
        QSettings settings;
        check(!settings.value(QStringLiteral("player/native/enabled")).toBool(),
              "backend and recipe edits preserve the saved disabled request");
        check(settings.value(QStringLiteral("quality/cache")).toString() ==
                  QStringLiteral("preserved-cache"),
              "backend and recipe edits preserve unrelated settings");
    }

    // The preset shortcut field is driven to the longest combination the dialog
    // accepts, so a field that cannot show it says so here.
    {
        SettingsDialog dialog(client, runner);
        dialog.resize(fitSettingsDialogSize(dialog, mikai::probes::windowSize(QSize(760, 780))));
        QKeySequenceEdit* preset = presetField(dialog);
        check(preset != nullptr,
              "a preset shortcut field is listed by its translated accessible-name pattern");
        if (preset != nullptr) {
            preset->setKeySequence(longestBinding());
        }
        const bool presetPageVisible = showShortcutField(dialog, preset);
        check(presetPageVisible,
              "the preset shortcut's owning tab and field are visible");
        if (preset != nullptr) {
            check(preset->keySequence() == longestBinding(),
                  "the preset field holds the longest binding");
            const bool readable = bindingFullyVisible(*preset);
            if (!readable) shortcutDiagnostics(dialog, *preset);
            check(readable,
                  "the visible preset field shows every character of the longest binding");
            const bool clearInside = clearActionInside(*preset);
            if (!clearInside) shortcutDiagnostics(dialog, *preset);
            check(clearInside, "the preset clear action is inside the field");
        }
    }

    {
        // ── the language applies live, and a failed save is a named refusal ──
        // The old behaviour stored the choice and asked for a restart. What has
        // to be true now is that the running translator follows the saved
        // choice, and that a catalog which cannot be loaded leaves both the
        // working translation and the stored preference exactly as they were.
        const QString catalog = qEnvironmentVariable("MIKAI_PROBE_TRANSLATION");
        if (catalog.isEmpty() || !QFileInfo::exists(catalog)) {
            std::fprintf(stderr,
                         "SKIP the live language section: MIKAI_PROBE_TRANSLATION must name the "
                         "built catalog for this probe to switch into Ukrainian\n");
        } else {
            check(mikai::app::applyInterfaceLanguage(QStringLiteral("uk"), catalog),
                  "the probe loads the supplied catalog through the public language API");
            check(mikai::app::applyInterfaceLanguage(QStringLiteral("en")),
                  "the probe starts in English with its catalog available for Save");

            {
                SettingsDialog dialog(client, runner);
                auto* language =
                    dialog.findChild<QComboBox*>(QStringLiteral("uiLanguageCombo"));
                check(language != nullptr, "the language choice exists");
                const int ukrainian = language != nullptr ? language->findData(QStringLiteral("uk")) : -1;
                check(ukrainian >= 0, "the language choice offers Ukrainian");
                if (language != nullptr && ukrainian >= 0) {
                    language->setCurrentIndex(ukrainian);
                    QDialogButtonBox* languageBox = dialog.findChild<QDialogButtonBox*>();
                    QPushButton* languageSave = acceptButton(languageBox);
                    if (languageSave != nullptr) {
                        languageSave->click();
                    }
                    QApplication::processEvents();
                    check(dialog.result() == static_cast<int>(QDialog::Accepted),
                          "the Save that carries the language accepted the dialog");
                }
            }
            check(mikai::app::activeInterfaceLanguage() == QStringLiteral("uk"),
                  "saving the language applied it to the running application without a restart");
            {
                QSettings settings;
                check(settings.value(QStringLiteral("ui/language")).toString() ==
                          QStringLiteral("uk"),
                      "the saved language preference is the one that was applied");
            }

            // An unavailable catalog: the working translation and the stored
            // preference both survive, and the refusal is named.
            const QString workingError = mikai::app::localizedApiError(QStringLiteral("not_found"));
            check(!mikai::app::applyInterfaceLanguage(
                      QStringLiteral("uk"),
                      QDir(QCoreApplication::applicationDirPath())
                          .absoluteFilePath(QStringLiteral("mikai_absent_probe_catalog.qm"))),
                  "an absent catalog is refused");
            check(mikai::app::activeInterfaceLanguage() == QStringLiteral("uk"),
                  "a refused language left the working one installed");
            check(QLocale().language() == QLocale::Ukrainian,
                  "a refused language left the default locale alone");
            check(mikai::app::localizedApiError(QStringLiteral("not_found")) == workingError,
                  "a refused language left the working translation's texts alone");
            {
                QSettings settings;
                check(settings.value(QStringLiteral("ui/language")).toString() ==
                          QStringLiteral("uk"),
                      "a refused language left the stored preference alone");
            }

            check(mikai::app::applyInterfaceLanguage(QStringLiteral("en")),
                  "the probe leaves English installed");
        }
    }

    {
        const QString catalog = qEnvironmentVariable("MIKAI_PROBE_TRANSLATION");
        for (const QString& language : {QStringLiteral("en"), QStringLiteral("uk")}) {
            const bool applied = mikai::app::applyInterfaceLanguage(language, catalog);
            check(applied, "the Processing surface language was installed");
            if (!applied) continue;
            for (const QSize size : {
                     QSize(mikai::app::theme::kSettingsDialogMinWidth,
                           mikai::app::theme::kSettingsDialogDefaultHeight),
                     QSize(mikai::app::theme::kSettingsDialogDefaultWidth,
                           mikai::app::theme::kSettingsDialogDefaultHeight)}) {
                SettingsDialog dialog(client, runner);
                exerciseProcessing(dialog, client, runner, size, language);
            }
        }
    }
    {
        const QString catalog = qEnvironmentVariable("MIKAI_PROBE_TRANSLATION");
        for (const QString& language : {QStringLiteral("en"), QStringLiteral("uk")}) {
            const bool applied = mikai::app::applyInterfaceLanguage(language, catalog);
            check(applied, "the Components surface language was installed");
            if (!applied) continue;
            for (const QSize size : {
                     QSize(mikai::app::theme::kSettingsDialogMinWidth,
                           mikai::app::theme::kSettingsDialogDefaultHeight),
                     QSize(mikai::app::theme::kSettingsDialogDefaultWidth,
                           mikai::app::theme::kSettingsDialogDefaultHeight)}) {
                SettingsDialog dialog(client, runner);
                exerciseComponents(dialog, size, language);
            }
        }
    }
    {
        const QString previousApplicationName = QCoreApplication::applicationName();
        const QString isolatedName =
            QStringLiteral("MikaiNativeSettingsProbeVerify_%1")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QCoreApplication::setApplicationName(isolatedName);
        const QString isolatedAppData =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        const auto selectedBackend = mplayer::native::Backend::DirectML;
        const auto inactiveBackend = mplayer::native::Backend::TensorRT;
        const auto selectedBefore =
            mplayer::native::describeAnimeJanaiInstallation(selectedBackend);
        const auto inactiveBefore =
            mplayer::native::describeAnimeJanaiInstallation(inactiveBackend);
        bool corruptFixtureReady = false;
        if (!selectedBefore.root.isEmpty() && !inactiveBefore.root.isEmpty() &&
            !selectedBefore.installed && QDir().mkpath(inactiveBefore.root)) {
            const QString corruptRecord =
                QDir(inactiveBefore.root).filePath(QStringLiteral("install.json"));
            QFile record(corruptRecord);
            const QByteArray invalidJson = QByteArrayLiteral("{ deliberately invalid");
            corruptFixtureReady = record.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                                  record.write(invalidJson) == invalidJson.size();
            record.close();
        }
        check(corruptFixtureReady, "the isolated Verify fixture has a missing selected record and corrupt inactive record");
        if (corruptFixtureReady) {
            SettingsDialog dialog(client, runner);
            exerciseVerifyBackendIsolation(dialog, selectedBackend, inactiveBackend,
                                            installRuntime);
        }
        if (!isolatedAppData.isEmpty()) QDir(isolatedAppData).removeRecursively();
        QCoreApplication::setApplicationName(previousApplicationName);
    }

    std::fprintf(stderr, "[native_settings_probe] %s (%d failures)\n",
                 failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
