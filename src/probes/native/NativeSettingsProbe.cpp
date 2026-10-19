
#include <QAccessible>
#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QColor>
#include <QCheckBox>
#include <QFileDialog>
#include <QTableWidget>
#include <QTimer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QGroupBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMenu>
#include <QLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QLocale>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QToolButton>
#include <QThread>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QTest>
#include "ProbeIsolation.hpp"
#include "ProbeUi.hpp"
#include "IdentityOnnxFixture.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <cstdio>

#include "I18n.hpp"
#include "backend/MikaiClient.hpp"
#include "theme/Metrics.hpp"
#include "dialogs/SettingsDialog.hpp"
#include "dialogs/AnimeJanaiManagerPanel.hpp"
#include "dialogs/ComponentStoreTab.hpp"
#include "player/compare/CompareCanvas.hpp"
#include "player/compare/CompareWindow.hpp"
#include "player/core/VideoSurface.hpp"
#include "player/shell/PlayerPopups.hpp"
#include "player/shell/PlayerShell.hpp"
#include "native/bundle/NativePrefs.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "native/components/NativeGpuProbe.hpp"
#include "native/models/NativeRife.hpp"
#include "native/models/NativeSpatialAssets.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"
#include "workers/Runner.hpp"

using mikai::app::Runner;
using mikai::app::SettingsDialog;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const char* what) {
    ++checks;
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

QComboBox* comboForLabel(QWidget& root, const QString& labelText) {
    for (QLabel* label : root.findChildren<QLabel*>()) {
        if (label->text() == labelText) {
            if (auto* combo = qobject_cast<QComboBox*>(label->buddy())) return combo;
        }
    }
    return nullptr;
}

QPushButton* buttonForCaption(QWidget& root, const QString& caption) {
    for (QPushButton* button : root.findChildren<QPushButton*>()) {
        if (button->text() == caption) return button;
    }
    return nullptr;
}

struct FileDialogDrive {
    bool appeared = false;
    bool accepted = false;
    bool timedOut = false;
    QString selected;
};

FileDialogDrive driveFileDialog(const QString& path, const std::function<void()>& open) {
    FileDialogDrive result;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer poller;
    poller.setInterval(50);
    QObject::connect(&poller, &QTimer::timeout, &poller, [&] {
        QFileDialog* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr || !dialog->isVisible()) {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* candidate = qobject_cast<QFileDialog*>(widget);
                if (candidate != nullptr && candidate->isVisible()) {
                    dialog = candidate;
                    break;
                }
            }
        }
        if (dialog == nullptr) {
            if (elapsed.elapsed() >= 5000) {
                result.timedOut = true;
                if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                    modal->reject();
                }
            }
            return;
        }

        if (!result.appeared) {
            result.appeared = true;
            QObject::connect(dialog, &QDialog::accepted, &poller,
                             [&result] { result.accepted = true; });
        }
        if (elapsed.elapsed() >= 5000) {
            result.timedOut = true;
            dialog->reject();
            return;
        }
        dialog->selectFile(path);
        if (auto* fileName = dialog->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"));
            fileName != nullptr && fileName->text() != path) {
            fileName->setText(path);
        }
        result.selected = dialog->selectedFiles().value(0);
        static_cast<QDialog*>(dialog)->accept();
    });
    poller.start();
    open();
    poller.stop();
    return result;
}

QTableWidget* customModelTable(QWidget& root) {
    for (QTableWidget* table : root.findChildren<QTableWidget*>()) {
        if (table->columnCount() == 5) return table;
    }
    return nullptr;
}

int customModelRow(QTableWidget* table, const QString& id) {
    if (table == nullptr) return -1;
    for (int row = 0; row < table->rowCount(); ++row) {
        const auto* item = table->item(row, 0);
        if (item != nullptr && item->data(Qt::UserRole).toString() == id) return row;
    }
    return -1;
}

void exerciseAnimeJanaiManager(mikai::backend::MikaiClient& client, Runner& runner) {
    QTemporaryDir fixtureDir;
    check(fixtureDir.isValid(), "an isolated ONNX import fixture directory exists");
    if (!fixtureDir.isValid()) return;

    const QString fixturePath = QDir(fixtureDir.path()).filePath(QStringLiteral("smoke_model.onnx"));
    QFile fixture(fixturePath);
    const QByteArray modelProto = mikai::tests::fixtures::makeTestIdentityOnnxGraph();
    check(modelProto.size() >= 64 &&
              fixture.open(QIODevice::WriteOnly) &&
              fixture.write(modelProto) == modelProto.size(),
          "a real 4D Identity ONNX ModelProto fixture was written");
    fixture.close();

    SettingsDialog managerDialog(client, runner);
    const QSize managerWindowSize = fitSettingsDialogSize(
        managerDialog,
        QSize(mikai::app::theme::kSettingsDialogMinWidth,
              mikai::app::theme::kSettingsDialogDefaultHeight));
    managerDialog.resize(managerWindowSize);
    managerDialog.showProcessingTab();
    managerDialog.show();
    QApplication::processEvents();
    auto* runtimeChoice =
        managerDialog.findChild<QComboBox*>(QStringLiteral("runtimeChoiceCombo"));
    check(runtimeChoice != nullptr, "the Processing runtime choice is available");
    if (runtimeChoice != nullptr) {
        runtimeChoice->setCurrentIndex(
            runtimeChoice->findData(static_cast<int>(mplayer::native::RuntimeChoice::Standard)));
    }
    QApplication::processEvents();
    auto* panelWidget = managerDialog.findChild<mikai::app::AnimeJanaiManagerPanel*>(
        QStringLiteral("animeJanaiManagerPanel"));
    check(panelWidget != nullptr && panelWidget->isVisibleTo(&managerDialog),
          "Standard runtime shows the custom manager in Settings");
    if (runtimeChoice != nullptr) {
        runtimeChoice->setCurrentIndex(
            runtimeChoice->findData(static_cast<int>(mplayer::native::RuntimeChoice::Native)));
    }
    QApplication::processEvents();
    check(panelWidget != nullptr && !panelWidget->isVisibleTo(&managerDialog),
          "Native runtime hides the custom manager while exposing private controls");
    if (runtimeChoice != nullptr) {
        runtimeChoice->setCurrentIndex(
            runtimeChoice->findData(static_cast<int>(mplayer::native::RuntimeChoice::Standard)));
    }
    QApplication::processEvents();
    check(panelWidget != nullptr && panelWidget->isVisibleTo(&managerDialog),
          "returning to Standard restores the custom manager");
    if (panelWidget == nullptr) return;
    mikai::probes::revealWidget(panelWidget);
    QApplication::processEvents();
    mikai::app::AnimeJanaiManagerPanel& panel = *panelWidget;

    QPushButton* importModel = buttonForCaption(
        panel, mikai::app::AnimeJanaiManagerPanel::tr("Import .onnx Model…"));
    check(importModel != nullptr, "the manager exposes its ONNX import action");
    FileDialogDrive importDialog;
    if (importModel != nullptr) {
        importDialog = driveFileDialog(fixturePath, [importModel] { importModel->click(); });
    }
    check(importDialog.appeared && importDialog.accepted && !importDialog.timedOut,
          "the user ONNX import chooser selected the valid fixture");

    QTableWidget* modelTable = customModelTable(panel);
    const bool importedInUi =
        waitForUi([&] { return modelTable != nullptr && modelTable->rowCount() == 1; }, 15000);
    check(importedInUi, "the asynchronous import reached the model inventory table");
    auto modelInventory = std::async(std::launch::async, [] {
        return mplayer::native::listCustomAnimeJanaiModels();
    });
    const bool inventoryReady = modelInventory.wait_for(std::chrono::seconds(15)) ==
                                std::future_status::ready;
    const auto models = inventoryReady
                            ? modelInventory.get()
                            : QVector<mplayer::native::CustomAnimeJanaiModel>{};
    check(inventoryReady && models.size() == 1 && models.front().valid,
          "the background custom-model inventory contains the imported model");

    QTemporaryDir externalDirectory;
    check(externalDirectory.isValid(), "the external model folder is isolated");
    const QString externalPath =
        QDir(externalDirectory.path()).filePath(QStringLiteral("external_smoke.onnx"));
    QFile externalFile(externalPath);
    check(externalFile.open(QIODevice::WriteOnly) &&
              externalFile.write(modelProto) == modelProto.size(),
          "an ordinary external ONNX graph without a sidecar was written");
    externalFile.close();
    auto* externalEdit =
        panel.findChild<QLineEdit*>(QStringLiteral("ajiExternalModelsDirEdit"));
    check(externalEdit != nullptr, "the external model folder can be edited");
    if (externalEdit != nullptr) {
        mikai::probes::revealWidget(externalEdit);
        externalEdit->setFocus();
        externalEdit->setText(externalDirectory.path());
        QTest::keyClick(externalEdit, Qt::Key_Tab);
    }
    const bool externalCandidateVisible = waitForUi(
        [&] { return customModelRow(modelTable, QStringLiteral("external_smoke")) >= 0; },
        5000);
    check(externalCandidateVisible, "the external folder presents its unregistered ONNX candidate");
    if (externalCandidateVisible && importModel != nullptr) {
        modelTable->selectRow(customModelRow(modelTable, QStringLiteral("external_smoke")));
        importModel->click();
    }
    const bool externalRegistered = waitForUi(
        [&] {
            const int row = customModelRow(modelTable, QStringLiteral("external_smoke"));
            return row >= 0 && !modelTable->item(row, 0)->data(Qt::UserRole + 1).toBool();
        },
        15000);
    check(externalRegistered, "the external candidate is registered through the real Import action");
    check(QFileInfo(externalPath).isFile() &&
              !QFileInfo::exists(externalPath + QStringLiteral(".source.json")),
          "external registration leaves the user's graph and directory untouched");

    auto* slotSelector = panel.findChild<QComboBox*>(QStringLiteral("ajiSlotSelectCombo"));
    QPushButton* activate = buttonForCaption(
        panel, mikai::app::AnimeJanaiManagerPanel::tr("Activate / Reset Slot"));
    check(slotSelector != nullptr && activate != nullptr,
          "the manager exposes slot selection and activation actions");
    if (slotSelector != nullptr && activate != nullptr) {
        for (int slot = 1; slot <= 10; ++slot) {
            const int index = slotSelector->findData(slot);
            check(index >= 0, "slot selector lists every profile slot");
            if (index >= 0) {
                slotSelector->setCurrentIndex(index);
                activate->click();
            }
        }
    }

    QComboBox* chain = comboForLabel(
        panel, mikai::app::AnimeJanaiManagerPanel::tr("Active chain"));
    QPushButton* addChain =
        buttonForCaption(panel, mikai::app::AnimeJanaiManagerPanel::tr("Add Chain"));
    QPushButton* addStep =
        panel.findChild<QPushButton*>(QStringLiteral("ajiAddStepButton"));
    check(chain != nullptr && addChain != nullptr && addStep != nullptr,
          "the manager exposes chain and model-step actions");
    if (slotSelector != nullptr && chain != nullptr && addChain != nullptr && addStep != nullptr) {
        const int firstSlot = slotSelector->findData(1);
        if (firstSlot >= 0) slotSelector->setCurrentIndex(firstSlot);
        addChain->click();
        addStep->click();
        addStep->click();
        auto* a4kKind = panel.findChild<QComboBox*>(QStringLiteral("animeJanaiStepKind_2"));
        check(a4kKind != nullptr, "the mixed chain exposes a typed second step");
        if (a4kKind != nullptr) {
            a4kKind->setCurrentIndex(a4kKind->findData(
                static_cast<int>(mplayer::native::AnimeJanaiStepKind::Anime4K)));
        }
        auto* a4kPreset = panel.findChild<QComboBox*>(QStringLiteral("animeJanaiStepId_2"));
        const QStringList expectedA4kIds{
            QStringLiteral("fast_a"), QStringLiteral("fast_b"), QStringLiteral("fast_c"),
            QStringLiteral("hq_a"), QStringLiteral("hq_b"), QStringLiteral("hq_c"),
            QStringLiteral("hq_aa"), QStringLiteral("hq_bb"), QStringLiteral("hq_ca")};
        bool allA4kPresetsAvailable = a4kPreset != nullptr;
        if (a4kPreset != nullptr) {
            for (const QString& id : expectedA4kIds)
                allA4kPresetsAvailable &= a4kPreset->findData(id) >= 0;
        }
        check(allA4kPresetsAvailable,
              "the A4K selector offers A/B/C Fast, A/B/C HQ, and all combined HQ stable ids");
        if (a4kPreset != nullptr) a4kPreset->setCurrentIndex(a4kPreset->findData(QStringLiteral("hq_aa")));
        auto* a4kScale = panel.findChild<QComboBox*>(QStringLiteral("animeJanaiStepScale_2"));
        check(a4kScale != nullptr && a4kScale->findData(4) >= 0,
              "the A4K output scale selector offers exact 1x/2x/4x targets");
        if (a4kScale != nullptr) a4kScale->setCurrentIndex(a4kScale->findData(4));
        auto* inputHeight =
            panel.findChild<QSpinBox*>(QStringLiteral("animeJanaiStepResizeHeight_2"));
        auto* inputPercent =
            panel.findChild<QSpinBox*>(QStringLiteral("animeJanaiStepResizePercent_2"));
        if (inputHeight != nullptr) inputHeight->setValue(720);
        if (inputPercent != nullptr) inputPercent->setValue(80);
        QComboBox* customChoice =
            panel.findChild<QComboBox*>(QStringLiteral("animeJanaiStepId_3"));
        const int customModelIndex =
            customChoice != nullptr ? customChoice->findData(QStringLiteral("smoke_model")) : -1;
        const bool customChoiceFound = customModelIndex >= 0;
        if (customChoiceFound) customChoice->setCurrentIndex(customModelIndex);
        check(customChoiceFound, "the imported custom ONNX model remains distinct and selectable");
        auto* reorderUp =
            panel.findChild<QPushButton*>(QStringLiteral("animeJanaiStepMoveUp_3"));
        check(reorderUp != nullptr, "each step has an explicit move control");
        if (reorderUp != nullptr) reorderUp->click();
        QApplication::processEvents();
    }

    QComboBox* quality = comboForLabel(
        panel, mikai::app::AnimeJanaiManagerPanel::tr("Quality preset (Slot 1001)"));
    QComboBox* balanced = comboForLabel(
        panel, mikai::app::AnimeJanaiManagerPanel::tr("Balanced preset (Slot 1002)"));
    QComboBox* performance = comboForLabel(
        panel, mikai::app::AnimeJanaiManagerPanel::tr("Performance preset (Slot 1003)"));
    check(quality != nullptr && balanced != nullptr && performance != nullptr,
          "all three Sharp preset selectors are available");
    if (quality != nullptr) quality->setCurrentIndex(quality->findData(true));
    if (balanced != nullptr) balanced->setCurrentIndex(balanced->findData(true));
    if (performance != nullptr) performance->setCurrentIndex(performance->findData(true));

    QCheckBox* logging = nullptr;
    for (QCheckBox* candidate : panel.findChildren<QCheckBox*>()) {
        if (candidate->text() ==
            mikai::app::AnimeJanaiManagerPanel::tr("Backend logging")) {
            logging = candidate;
            break;
        }
    }
    check(logging != nullptr, "the backend logging control is available");
    if (logging != nullptr) logging->setChecked(!logging->isChecked());
    QCheckBox* chainRife = nullptr;
    for (QCheckBox* candidate : panel.findChildren<QCheckBox*>()) {
        if (candidate->text() ==
            mikai::app::AnimeJanaiManagerPanel::tr("RIFE interpolation")) {
            chainRife = candidate;
            break;
        }
    }
    check(chainRife != nullptr, "the custom chain exposes RIFE interpolation settings");
    if (chainRife != nullptr) chainRife->setChecked(true);

    QComboBox* trt = comboForLabel(
        panel, mikai::app::AnimeJanaiManagerPanel::tr("TensorRT engine type"));
    QComboBox* dynamicMin = comboForLabel(
        panel, mikai::app::AnimeJanaiManagerPanel::tr("Dynamic minimum resolution"));
    check(trt != nullptr && dynamicMin != nullptr,
          "TensorRT engine type and dynamic-shape controls are available");
    if (trt != nullptr && dynamicMin != nullptr) {
        const QVector<QPair<QString, bool>> modes{
            {QStringLiteral("static"), false},
            {QStringLiteral("static_onnx"), false},
            {QStringLiteral("dynamic"), true}};
        for (const auto& [mode, dynamicVisible] : modes) {
            const int index = trt->findData(mode);
            check(index >= 0, "all TensorRT modes are selectable");
            if (index >= 0) trt->setCurrentIndex(index);
            check(dynamicMin->isVisibleTo(&panel) == dynamicVisible,
                  "TensorRT dynamic shapes follow the selected engine mode");
        }
    }
    auto* rawArgs = panel.findChild<QLineEdit*>(QStringLiteral("ajiTrtEngineSettingsEdit"));
    check(rawArgs != nullptr, "TensorRT raw settings control is available");
    if (rawArgs != nullptr) {
        rawArgs->setFocus();
        rawArgs->selectAll();
        QTest::keyClicks(rawArgs, QStringLiteral("--builderOptimizationLevel=4"));
    }

    check(panel.isDirty(), "editing manager controls marks config dirty");

    const QString profilePath =
        QDir(fixtureDir.path()).filePath(QStringLiteral("smoke_profile.pconf"));
    QPushButton* exportProfile = buttonForCaption(
        panel, mikai::app::AnimeJanaiManagerPanel::tr("Export .pconf…"));
    check(exportProfile != nullptr && slotSelector != nullptr,
          "the custom profile export action is available");
    FileDialogDrive exportDialog;
    if (exportProfile != nullptr && slotSelector != nullptr) {
        exportDialog = driveFileDialog(profilePath, [exportProfile] { exportProfile->click(); });
    }
    check(exportDialog.appeared && exportDialog.accepted && !exportDialog.timedOut &&
              QFileInfo(profilePath).isFile(),
          "the profile export action wrote a .pconf file through its chooser");
    QPushButton* importProfile = buttonForCaption(
        panel, mikai::app::AnimeJanaiManagerPanel::tr("Import .pconf…"));
    FileDialogDrive importProfileDialog;
    if (importProfile != nullptr && slotSelector != nullptr && QFileInfo(profilePath).isFile()) {
        const int secondSlot = slotSelector->findData(2);
        if (secondSlot >= 0) slotSelector->setCurrentIndex(secondSlot);
        importProfileDialog =
            driveFileDialog(profilePath, [importProfile] { importProfile->click(); });
    }
    check(importProfileDialog.appeared && importProfileDialog.accepted &&
              !importProfileDialog.timedOut,
          "the profile import action selected the exported .pconf");
    check(importProfile != nullptr && slotSelector != nullptr &&
              panel.currentConfig().customSlots.size() == 10,
          "the profile import action preserves the complete slot collection");

    const auto edited = panel.currentConfig();
    check(edited.customSlots.size() == 10,
          "user activation created all ten profile slots");
    if (!edited.customSlots.isEmpty()) {
        bool hasMultipleModelSteps = false;
        bool mixedChainRoundTrip = false;
        for (const auto& chainConfig : edited.customSlots.front().chains) {
            hasMultipleModelSteps |= chainConfig.steps.size() >= 2;
            if (chainConfig.steps.size() >= 3 &&
                chainConfig.steps.at(1).kind == mplayer::native::AnimeJanaiStepKind::OnnxModel &&
                chainConfig.steps.at(1).id == QStringLiteral("smoke_model") &&
                chainConfig.steps.at(2).kind == mplayer::native::AnimeJanaiStepKind::Anime4K &&
                chainConfig.steps.at(2).id == QStringLiteral("hq_aa") &&
                chainConfig.steps.at(2).scaleFactor == 4 &&
                chainConfig.steps.at(2).resizeHeightBeforeUpscale == 720 &&
                chainConfig.steps.at(2).resizeFactorBeforeUpscale == 80) {
                mixedChainRoundTrip = true;
            }
        }
        check(edited.customSlots.front().chains.size() >= 2 && hasMultipleModelSteps,
              "slot 1 has multiple chains and multiple ordered steps");
        check(mixedChainRoundTrip,
              "Save/reopen retains ONNX/A4K order, combined HQ id, input resize, and 4x target");
        check(edited.customSlots.size() > 1 &&
                  edited.customSlots.at(1).chains.size() >= 2,
              "importing the .pconf profile populated slot 2");
        bool pconfMixedChainPreserved = false;
        if (edited.customSlots.size() > 1) {
            for (const auto& chainConfig : edited.customSlots.at(1).chains) {
                pconfMixedChainPreserved |= chainConfig.steps.size() >= 3 &&
                    chainConfig.steps.at(1).id == QStringLiteral("smoke_model") &&
                    chainConfig.steps.at(2).kind == mplayer::native::AnimeJanaiStepKind::Anime4K &&
                    chainConfig.steps.at(2).id == QStringLiteral("hq_aa") &&
                    chainConfig.steps.at(2).scaleFactor == 4;
            }
        }
        check(pconfMixedChainPreserved,
              "the .pconf export/import retains the ordered mixed chain and A4K output scale");
    }
    check(edited.qualitySharp && edited.balancedSharp && edited.performanceSharp,
          "Sharp choices are reflected in all built-in presets");
    check(edited.enableLogging == (logging != nullptr && logging->isChecked()) &&
              edited.trtEngineSettings.contains(QStringLiteral("--builderOptimizationLevel=4")),
          "logging and TensorRT settings are retained in the edited config");

    const QString fullConfigPath =
        QDir(fixtureDir.path()).filePath(QStringLiteral("manager.conf"));
    auto* exportConfig = buttonForCaption(
        panel, mikai::app::AnimeJanaiManagerPanel::tr("Export animejanai.conf…"));
    auto* importConfig = buttonForCaption(
        panel, mikai::app::AnimeJanaiManagerPanel::tr("Import animejanai.conf…"));
    check(exportConfig != nullptr && importConfig != nullptr,
          "the complete configuration import and export actions are available");
    if (exportConfig != nullptr) {
        const auto exported =
            driveFileDialog(fullConfigPath, [exportConfig] { exportConfig->click(); });
        check(exported.accepted && QFileInfo::exists(fullConfigPath),
              "the complete manager configuration was exported through its chooser");
    }
    const QString malformedConfig =
        QDir(fixtureDir.path()).filePath(QStringLiteral("invalid.conf"));
    QFile invalidFile(malformedConfig);
    check(invalidFile.open(QIODevice::WriteOnly) &&
              invalidFile.write("<html>not a configuration</html>") > 0,
          "a malformed config import fixture was written");
    invalidFile.close();
    if (importConfig != nullptr) {
        const auto rejected =
            driveFileDialog(malformedConfig, [importConfig] { importConfig->click(); });
        auto* banner = panel.findChild<QLabel*>(QStringLiteral("status"));
        check(rejected.accepted && panel.currentConfig() == edited && panel.isDirty() &&
                  banner != nullptr && banner->isVisibleTo(&panel) && !banner->text().isEmpty(),
              "a malformed configuration import reports an error and preserves unsaved edits");
        const auto imported =
            driveFileDialog(fullConfigPath, [importConfig] { importConfig->click(); });
        check(imported.accepted && panel.currentConfig() == edited,
              "the full configuration round-trips through the real import action");
    }

    check(managerDialog.grab().save(QStringLiteral("native-animejanai-manager.png")),
          "the visible manager inside the minimum-size Settings window was captured");
    QPushButton* managerSave = acceptButton(managerDialog.findChild<QDialogButtonBox*>());
    check(managerSave != nullptr, "Settings exposes its real Save action for the manager");
    if (managerSave != nullptr) managerSave->click();
    QApplication::processEvents();
    check(managerDialog.result() == static_cast<int>(QDialog::Accepted),
          "the Settings Save action accepted the manager changes");

    QFile savedConfig(mplayer::native::AnimeJanaiConfigStore::configFilePath());
    const QByteArray savedBytes =
        savedConfig.open(QIODevice::ReadOnly) ? savedConfig.readAll() : QByteArray{};
    check(savedBytes.contains(QByteArrayLiteral("config_version=3")),
          "the Settings Save action wrote schema version 3");
    check(savedBytes.contains(QByteArrayLiteral("mikai_chain_steps_version=1")),
          "the Settings Save action wrote the ordered-step extension version");

    SettingsDialog reloadedDialog(client, runner);
    reloadedDialog.showProcessingTab();
    auto* reloadedRuntime =
        reloadedDialog.findChild<QComboBox*>(QStringLiteral("runtimeChoiceCombo"));
    if (reloadedRuntime != nullptr) {
        reloadedRuntime->setCurrentIndex(
            reloadedRuntime->findData(static_cast<int>(mplayer::native::RuntimeChoice::Standard)));
    }
    QApplication::processEvents();
    auto* reloadedPanel = reloadedDialog.findChild<mikai::app::AnimeJanaiManagerPanel*>(
        QStringLiteral("animeJanaiManagerPanel"));
    check(reloadedPanel != nullptr && reloadedPanel->currentConfig() == edited &&
              !reloadedPanel->isDirty(),
          "reopening Settings reloads the saved manager configuration cleanly");
    if (reloadedPanel == nullptr) return;
    reloadedDialog.resize(managerWindowSize);
    reloadedDialog.show();
    mikai::probes::revealWidget(reloadedPanel);
    QApplication::processEvents();

    bool customPresetFound = false;
    for (const auto& preset : mplayer::native::animeJanaiPresets()) {
        if (preset.id == QStringLiteral("spatial:custom:smoke_model")) {
            customPresetFound = true;
            break;
        }
    }
    check(customPresetFound,
          "the imported custom model is published through AnimeJaNai preset catalogue");

    SettingsDialog benchmarkDialog(client, runner);
    auto* benchmarkTabs = benchmarkDialog.findChild<QTabWidget*>();
    QWidget* benchmarkPage = nullptr;
    if (benchmarkTabs != nullptr) {
        for (QWidget* page : benchmarkDialog.findChildren<QWidget*>(
                 QStringLiteral("benchmarkPage"))) {
            if (benchmarkTabs->indexOf(page) >= 0) {
                benchmarkPage = page;
                break;
            }
        }
        if (benchmarkPage != nullptr) benchmarkTabs->setCurrentWidget(benchmarkPage);
    }
    QApplication::processEvents();
    auto* benchmarkTree = benchmarkDialog.findChild<QTreeWidget*>(
        QStringLiteral("benchmarkModelTree"));
    bool benchmarkCustomPresent = false;
    bool benchmarkSlotPresent = false;
    if (benchmarkTree != nullptr) {
        for (int groupIndex = 0; groupIndex < benchmarkTree->topLevelItemCount(); ++groupIndex) {
            const QTreeWidgetItem* group = benchmarkTree->topLevelItem(groupIndex);
            for (int row = 0; row < group->childCount(); ++row) {
                const QString id = group->child(row)->data(0, Qt::UserRole + 1).toString();
                benchmarkCustomPresent |=
                    id == QStringLiteral("spatial:custom:smoke_model");
                benchmarkSlotPresent |= id == QStringLiteral("builtin_slot:1");
            }
        }
    }
    check(benchmarkTree != nullptr && benchmarkCustomPresent && benchmarkSlotPresent,
          "Benchmark's public catalogue includes custom spatial models and custom slots");

    const auto selectedRuntime = mplayer::native::selectStandardRuntime(
        qEnvironmentVariable("MIKAI_MPV_MODULE"));
    check(selectedRuntime.loaded &&
              selectedRuntime.kind == mplayer::native::BundleKind::Standard &&
              !selectedRuntime.restartRequired,
          "the real Standard mpv runtime is selected for player and Compare surfaces");
    std::fprintf(stderr, "[native_settings_probe] runtime=%s loaded=%d reason=%s\n",
                 qPrintable(selectedRuntime.modulePath), selectedRuntime.loaded ? 1 : 0,
                 qPrintable(selectedRuntime.reason));

    mplayer::shell::PlayerShell player;
    player.resize(QSize(900, 640));
    player.show();
    QApplication::processEvents();
    auto* enhancementButton =
        player.findChild<QPushButton*>(QStringLiteral("playerEnhanceButton"));
    auto* enhancementRoot =
        player.findChild<mplayer::shell::MenuPopup*>(QStringLiteral("playerEnhancementsPopup"));
    check(enhancementButton != nullptr && enhancementRoot != nullptr,
          "the real player exposes its enhancement menu");
    if (enhancementButton != nullptr && enhancementRoot != nullptr) {
        enhancementButton->click();
        QApplication::processEvents();
        auto clickEntry = [](mplayer::shell::MenuPopup* menu,
                             const std::function<bool(const mplayer::shell::MenuEntry&)>& match) {
            if (menu == nullptr) return false;
            for (int index = 0; index < menu->rowCount(); ++index) {
                if (!match(menu->entryAt(index))) continue;
                QWidget* row = menu->rowWidget(index);
                if (row == nullptr) return false;
                QTest::mouseClick(row, Qt::LeftButton, Qt::NoModifier, row->rect().center());
                QApplication::processEvents();
                return true;
            }
            return false;
        };
        auto levelMenu = [&player](const QString& level) {
            for (mplayer::shell::MenuPopup* menu :
                 player.findChildren<mplayer::shell::MenuPopup*>()) {
                if (menu->property("enhancementLevel").toString() == level) return menu;
            }
            return static_cast<mplayer::shell::MenuPopup*>(nullptr);
        };
        const bool enteredAnimeJanai = clickEntry(
            enhancementRoot, [](const mplayer::shell::MenuEntry& entry) {
                return entry.payload.toString() == QStringLiteral("level|animejanai");
            });
        auto* animeJanaiMenu = levelMenu(QStringLiteral("animejanai"));
        check(enteredAnimeJanai && animeJanaiMenu != nullptr,
              "the player menu opens its AnimeJaNai submenu");
        if (animeJanaiMenu != nullptr) {
            bool customSlotPresent = false;
            for (int index = 0; index < animeJanaiMenu->rowCount(); ++index) {
                customSlotPresent |= animeJanaiMenu->entryAt(index).payload.toString() ==
                                     QStringLiteral("ajipreset|builtin_slot:1");
            }
            check(customSlotPresent, "slot 1 appears as a selectable player-menu preset");
            const bool enteredSpatial = clickEntry(
                animeJanaiMenu, [](const mplayer::shell::MenuEntry& entry) {
                    return entry.payload.toString() ==
                           QStringLiteral("level|animejanai:spatial");
                });
            auto* spatialMenu = levelMenu(QStringLiteral("animejanai:spatial"));
            bool customModelPresent = false;
            if (spatialMenu != nullptr) {
                for (int index = 0; index < spatialMenu->rowCount(); ++index) {
                    customModelPresent |= spatialMenu->entryAt(index).payload.toString() ==
                                          QStringLiteral(
                                              "ajipreset|spatial:custom:smoke_model");
                }
            }
            check(enteredSpatial && customModelPresent,
                  "the custom ONNX model appears in the player's Spatial menu");
        }
    }

    QImage compareFixture(64, 36, QImage::Format_RGB32);
    for (int y = 0; y < compareFixture.height(); ++y) {
        for (int x = 0; x < compareFixture.width(); ++x) {
            compareFixture.setPixelColor(x, y, QColor((x * 4) % 256, (y * 7) % 256,
                                                       ((x + y) * 3) % 256));
        }
    }
    const QString compareFixturePath =
        QDir(fixtureDir.path()).filePath(QStringLiteral("compare-candidate-frame.png"));
    check(compareFixture.save(compareFixturePath),
          "a small static comparison frame fixture was written");
    mplayer::VideoSurface* compareSurface = player.videoSurface();
    const bool compareSourceOpened =
        compareSurface != nullptr && compareSurface->open(compareFixturePath);
    check(compareSourceOpened, "the player surface opened the static comparison frame");
    const bool compareSourceReady =
        compareSourceOpened &&
        waitForUi([&] {
            return compareSurface->hasVideo() && compareSurface->videoWidth() > 0 &&
                   compareSurface->videoHeight() > 0;
        }, 5000);
    check(compareSourceReady, "the static comparison frame reached the player surface");
    QStringList shaderCandidates;
    const QString shaderRoot = mplayer::anime4k::findShadersDir();
    for (const auto& mode : mplayer::anime4k::availableModes()) {
        const auto candidate = mplayer::anime4k::candidateForId(mode.id, shaderRoot);
        if (mplayer::anime4k::chainIsComplete(candidate, shaderRoot)) {
            shaderCandidates.append(mode.id);
        }
    }
    const QString shaderCandidate = shaderCandidates.value(0);
    check(shaderCandidates.size() >= 2,
          "two shader-only Compare candidates are available to avoid native inference");
    if (compareSourceReady && shaderCandidates.size() >= 2) {
        mplayer::shell::CompareWindow comparison(compareSurface, shaderCandidate, &player);
        comparison.resize(QSize(900, 600));
        comparison.show();
        const bool frameCaptured = waitForUi(
            [&] {
                return !comparison.frozenFrame().isNull() &&
                       !comparison.comparisonPending();
            },
            10000);
        check(frameCaptured, "Compare captured the static frame without an inference backend");
        if (frameCaptured) {
            auto* canvas = comparison.findChild<mplayer::shell::CompareCanvas*>(
                QStringLiteral("compareCanvas"));
            auto* badge = canvas != nullptr
                              ? canvas->findChild<QToolButton*>(
                                    QStringLiteral("compareBadgeA"))
                              : nullptr;
            check(badge != nullptr && badge->menu() != nullptr,
                  "the real Compare candidate selector is available");
            if (badge != nullptr && badge->menu() != nullptr) {
                auto compareCustomPresent = std::make_shared<bool>(false);
                QTimer::singleShot(0, badge, [badge, compareCustomPresent] {
                    const auto inspectMenu = [compareCustomPresent](
                                                 const auto& self, const QMenu* menu) -> void {
                        if (menu == nullptr) return;
                        for (QAction* action : menu->actions()) {
                            *compareCustomPresent |=
                                action->data().toString() ==
                                QStringLiteral("spatial:custom:smoke_model");
                            self(self, action->menu());
                        }
                    };
                    inspectMenu(inspectMenu, badge->menu());
                    badge->menu()->hide();
                });
                QTimer::singleShot(5000, badge, [badge] {
                    if (badge->menu() != nullptr && badge->menu()->isVisible()) {
                        badge->menu()->hide();
                    }
                });
                badge->showMenu();
                QApplication::processEvents();
                check(*compareCustomPresent,
                      "Compare's real candidate menu lists the imported custom spatial model");
            }
        }
        comparison.close();
    }

    QTableWidget* modelTableForRemoval = customModelTable(*reloadedPanel);
    QPushButton* removeModel = buttonForCaption(
        *reloadedPanel, mikai::app::AnimeJanaiManagerPanel::tr("Remove Selected"));
    check(modelTableForRemoval != nullptr && removeModel != nullptr,
          "the custom model removal action is available");
    if (removeModel != nullptr) {
        const int externalRow =
            customModelRow(modelTableForRemoval, QStringLiteral("external_smoke"));
        if (externalRow >= 0) {
            modelTableForRemoval->selectRow(externalRow);
            removeModel->click();
        }
    }
    const bool externalRemoved = waitForUi(
        [&] {
            const int row =
                customModelRow(modelTableForRemoval, QStringLiteral("external_smoke"));
            return row >= 0 &&
                   modelTableForRemoval->item(row, 0)->data(Qt::UserRole + 1).toBool();
        },
        15000);
    check(externalRemoved && QFileInfo(externalPath).isFile(),
          "removing an external registration preserves the graph and presents it as unregistered");
    if (removeModel != nullptr) {
        const int importedRow = customModelRow(modelTableForRemoval, QStringLiteral("smoke_model"));
        if (importedRow >= 0) {
            modelTableForRemoval->selectRow(importedRow);
            removeModel->click();
        }
    }
    const bool removedInUi = waitForUi(
        [&] {
            return customModelRow(modelTableForRemoval, QStringLiteral("smoke_model")) < 0;
        },
        15000);
    check(removedInUi, "asynchronous removal cleared the imported model from the manager");
    auto removedInventory = std::async(std::launch::async, [] {
        return mplayer::native::listCustomAnimeJanaiModels();
    });
    const bool removalReady = removedInventory.wait_for(std::chrono::seconds(15)) ==
                              std::future_status::ready;
    const auto remainingModels = removalReady
                                     ? removedInventory.get()
                                     : QVector<mplayer::native::CustomAnimeJanaiModel>{};
    check(removalReady && remainingModels.isEmpty(),
          "background inventory confirms the imported model was removed");
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
    const QString suffix =
        requestedSize.width() <= mikai::app::theme::kSettingsDialogMinWidth
            ? QStringLiteral("compact")
            : QStringLiteral("wide");
    if (processing != nullptr) {
        check(dialog.grab().save(
                  QStringLiteral("native-processing-%1-%2.png").arg(language, suffix)),
              "the visible Processing page screenshot was saved");
    }

    auto* manager = dialog.findChild<mikai::app::AnimeJanaiManagerPanel*>(
        QStringLiteral("animeJanaiManagerPanel"));
    auto* scroll = qobject_cast<QScrollArea*>(processing);
    check(manager != nullptr && manager->isVisibleTo(&dialog) && scroll != nullptr,
          "the visible Standard form is available for layout inspection");
    if (manager != nullptr && scroll != nullptr) {
        bool captionsFit = true;
        for (QLabel* label : manager->findChildren<QLabel*>()) {
            QWidget* field = label->buddy();
            if (field == nullptr || !label->isVisibleTo(&dialog)) continue;
            const QRect caption(label->mapTo(&dialog, QPoint()), label->size());
            const QRect editor(field->mapTo(&dialog, QPoint()), field->size());
            const bool fits = !caption.intersects(editor) &&
                              label->height() >= label->heightForWidth(label->width());
            if (!fits) {
                std::fprintf(stderr, "Clipped or overlapping Processing caption: %s\n",
                             qPrintable(label->text()));
            }
            captionsFit &= fits;
        }
        check(captionsFit && scroll->widget()->width() <= scroll->viewport()->width() &&
                  scroll->horizontalScrollBar()->maximum() == 0,
              "Processing captions fit without editor overlap or horizontal page scrolling");
        auto* rawArgs = manager->findChild<QLineEdit*>(QStringLiteral("ajiTrtEngineSettingsEdit"));
        check(rawArgs != nullptr, "the TensorRT section is available for visual inspection");
        if (rawArgs != nullptr) {
            mikai::probes::revealWidget(rawArgs);
            check(dialog.grab().save(
                      QStringLiteral("native-processing-%1-%2-trt.png").arg(language, suffix)),
                  "the visible preset and TensorRT form was captured");
        }
    }
    if (language == QLatin1String("uk") && suffix == QLatin1String("wide")) {
        dialog.showMaximized();
        check(waitForUi([&dialog] { return dialog.isMaximized(); }, 2000),
              "Settings can maximize on the native desktop");
        check(mikai::probes::dialogActionsFit(dialog, dialog.size()),
              "maximized Settings keeps Save and Cancel visible");
        check(dialog.grab().save(QStringLiteral("native-settings-uk-maximized.png")),
              "the maximized Settings window was captured");
        dialog.showNormal();
        check(waitForUi([&dialog, requestedSize] {
                  return !dialog.isMaximized() && dialog.size() == requestedSize;
              }, 2000),
              "Settings restores its resizable normal window dimensions");
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
    if (privateAssets != nullptr) mikai::probes::revealWidget(privateAssets);
    QApplication::processEvents();
    check(dialog.grab().save(
              QStringLiteral("native-processing-%1-%2-native.png").arg(language, suffix)),
          "the Native Processing surface screenshot was saved");
    if (runtime != nullptr) {
        runtime->setCurrentIndex(
            runtime->findData(static_cast<int>(mplayer::native::RuntimeChoice::Standard)));
    }
    QApplication::processEvents();
    check(privateAssets != nullptr && !privateAssets->isVisibleTo(&dialog),
          "returning from Native to Standard hides private controls again");
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
                                          "AnimeJaNai upscaling models")),
          "the AnimeJaNai upscaling models card title is reachable");
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
    check(verifyCompleted, "Verify completion was observed through the Components status surface");
    check(backend->currentData().toInt() == static_cast<int>(selected) &&
              !mplayer::native::describeAnimeJanaiInstallation(selected, false).installed,
          "Verify preserves the selected missing backend without installing it");

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
            const auto requestedBackend = selected;
            const bool installCompleted = waitForUi([&] {
                QLabel* currentStatus = statusSeverityLabel(*componentTab);
                const QString severity = currentStatus != nullptr
                    ? currentStatus->accessibleDescription() : QString();
                return mplayer::native::describeAnimeJanaiInstallation(requestedBackend, true).installed ||
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
                mplayer::native::describeAnimeJanaiInstallation(requestedBackend, true);
            const auto inactiveInstallation =
                mplayer::native::describeAnimeJanaiInstallation(inactive, true);
            const QString recordPath =
                QDir(installed.root).filePath(QStringLiteral("install.json"));
            const bool recordExists = QFileInfo(recordPath).isFile();
            const QString recordSha256 = recordExists ? fileSha256(recordPath) : QString();
            const QString actualBackend = installed.installed
                ? backendIdentity(requestedBackend)
                : (inactiveInstallation.installed
                       ? backendIdentity(inactive)
                       : QStringLiteral("none"));
            check(backend->currentData().toInt() == static_cast<int>(requestedBackend),
                  "Apply changes preserves the explicitly selected backend");
            check(installCompleted && installed.installed && recordExists &&
                      !recordSha256.isEmpty(),
                  "the real Apply changes install produced a verified GPU-backend record");
            QLabel* finalStatus = statusSeverityLabel(*componentTab);
            const QString severity = finalStatus != nullptr
                ? finalStatus->accessibleDescription() : QStringLiteral("unavailable");
            std::fprintf(stderr,
                         "[native_settings_probe] apply-changes requested-backend=%s actual-backend=%s completed=%d status-reason=%s severity=%s cancel-clicked=%d installed=%d root=%s record=%s record-sha256=%s\\n",
                         qPrintable(backendIdentity(requestedBackend)),
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
    // This probe runs scenarios that write an app-owned native store. Production
    // `nativeRoot()` is `<applicationDirPath>/native`, so an org name, an app
    // name and a redirected QSettings path buy exactly nothing: the only real
    // boundary is the executable directory. The physical isolation helper stakes
    // that boundary, so the whole probe - the mutating Verify fixture included -
    // runs from an owned copy of this executable in its own empty native store.
    using mikai::probes::isolation::Gate;
    using mikai::probes::isolation::enter;
    using mikai::probes::isolation::exitCode;
    using mikai::probes::isolation::Options;
    Options isolationOptions;
    isolationOptions.probe = "native_settings_probe";
    const Gate gate = enter(argc, argv, isolationOptions);
    if (gate != Gate::RunHere) return exitCode();

    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
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
            for (const QSize size : {
                     QSize(mikai::app::theme::kSettingsDialogMinWidth,
                           mikai::app::theme::kSettingsDialogDefaultHeight),
                     QSize(mikai::app::theme::kSettingsDialogDefaultWidth,
                           mikai::app::theme::kSettingsDialogDefaultHeight)}) {
                const bool applied = mikai::app::applyInterfaceLanguage(language, catalog);
                check(applied, "the Processing surface language was installed");
                if (!applied) continue;
                SettingsDialog dialog(client, runner);
                exerciseProcessing(dialog, client, runner, size, language);
            }
        }
    }
    {
        const QString catalog = qEnvironmentVariable("MIKAI_PROBE_TRANSLATION");
        for (const QString& language : {QStringLiteral("en"), QStringLiteral("uk")}) {
            for (const QSize size : {
                     QSize(mikai::app::theme::kSettingsDialogMinWidth,
                           mikai::app::theme::kSettingsDialogDefaultHeight),
                     QSize(mikai::app::theme::kSettingsDialogDefaultWidth,
                           mikai::app::theme::kSettingsDialogDefaultHeight)}) {
                const bool applied = mikai::app::applyInterfaceLanguage(language, catalog);
                check(applied, "the Components surface language was installed");
                if (!applied) continue;
                SettingsDialog dialog(client, runner);
                exerciseComponents(dialog, size, language);
            }
        }
    }
    {
        // This scenario writes a deliberately corrupt backend record and, on
        // request, installs a real runtime. Both mutate app-owned state, and
        // `nativeRoot()` is `<applicationDirPath>/native`: an org name, an app
        // name and a redirected `AppLocalDataLocation` all still resolve to the
        // real adjacent store, so none of them is isolation. `isolation::enter`
        // at the top of main put this whole process on an owned copy of the
        // executable, whose own native store is empty scratch, so the write
        // below lands there and nowhere else.
        const QString isolatedRoot = mikai::probes::isolation::isolatedRoot();
        check(!isolatedRoot.isEmpty() &&
                  mplayer::native::pathWithinRoot(isolatedRoot,
                                                  mplayer::native::nativeRoot()),
              "the mutating fixture's native store is inside the owned isolate");
        const auto selectedBackend = mplayer::native::Backend::DirectML;
        const auto inactiveBackend = mplayer::native::Backend::TensorRT;
        const auto selectedBefore =
            mplayer::native::describeAnimeJanaiInstallation(selectedBackend);
        const auto inactiveBefore =
            mplayer::native::describeAnimeJanaiInstallation(inactiveBackend);
        bool corruptFixtureReady = false;
        if (!selectedBefore.root.isEmpty() && !inactiveBefore.root.isEmpty() &&
            selectedBefore.root != inactiveBefore.root &&
            selectedBefore.reasonKey ==
                QLatin1String("animejanai.installation.recordMissing") &&
            inactiveBefore.reasonKey ==
                QLatin1String("animejanai.installation.recordMissing") &&
            QDir().mkpath(inactiveBefore.root)) {
            const QString corruptRecord =
                QDir(inactiveBefore.root).filePath(QStringLiteral("install.json"));
            QFile record(corruptRecord);
            const QByteArray invalidJson = QByteArrayLiteral("{ deliberately invalid");
            corruptFixtureReady = record.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                                  record.write(invalidJson) == invalidJson.size();
            record.close();
        }
        check(corruptFixtureReady,
              "the isolated stage starts with no existing backend records");
        if (corruptFixtureReady) {
            SettingsDialog dialog(client, runner);
            exerciseVerifyBackendIsolation(dialog, selectedBackend, inactiveBackend,
                                            installRuntime);
        }
    }

    exerciseAnimeJanaiManager(client, runner);

    QJsonObject evidence;
    evidence.insert(QStringLiteral("probe"), QStringLiteral("native_settings_probe"));
    evidence.insert(QStringLiteral("passed"), failures == 0);
    evidence.insert(QStringLiteral("checks"), checks);
    evidence.insert(QStringLiteral("failures"), failures);
    QJsonArray screenshots;
    screenshots.append(QStringLiteral("native-animejanai-manager.png"));
    screenshots.append(QStringLiteral("native-processing-en-compact.png"));
    screenshots.append(QStringLiteral("native-processing-en-wide.png"));
    screenshots.append(QStringLiteral("native-processing-en-compact-native.png"));
    screenshots.append(QStringLiteral("native-processing-en-wide-native.png"));
    screenshots.append(QStringLiteral("native-processing-uk-compact.png"));
    screenshots.append(QStringLiteral("native-processing-uk-compact-native.png"));
    screenshots.append(QStringLiteral("native-processing-uk-wide.png"));
    screenshots.append(QStringLiteral("native-processing-uk-wide-native.png"));
    screenshots.append(QStringLiteral("native-processing-en-compact-trt.png"));
    screenshots.append(QStringLiteral("native-processing-en-wide-trt.png"));
    screenshots.append(QStringLiteral("native-processing-uk-compact-trt.png"));
    screenshots.append(QStringLiteral("native-processing-uk-wide-trt.png"));
    screenshots.append(QStringLiteral("native-settings-uk-maximized.png"));
    evidence.insert(QStringLiteral("screenshots"), screenshots);
    QFile evidenceFile(QStringLiteral("native-animejanai-smoke-evidence.json"));
    if (evidenceFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        evidenceFile.write(QJsonDocument(evidence).toJson(QJsonDocument::Indented));
        evidenceFile.close();
    } else {
        check(false, "smoke evidence JSON was written");
    }

    std::fprintf(stderr, "[native_settings_probe] %s (%d failures)\n",
                 failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
