
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
#include <QSaveFile>
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

/// Same verdict line, with the failure's own words. A refused atomic replace
/// is silent at the Qt level, so the reason the operation reported is the only
/// evidence of why the destination could not be replaced.
void check(bool ok, const char* what, const QString& detail) {
    ++checks;
    std::fprintf(stderr, "%s %s%s%s\n", ok ? "PASS" : "FAIL", what,
                 detail.isEmpty() ? "" : ": ", qPrintable(detail));
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
    if (auto* named =
            root.findChild<QTableWidget*>(QStringLiteral("animeJanaiCustomModelsTable"))) {
        return named;
    }
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
struct CatalogueRowView {
    int row = -1;
    QString id;
    QString origin;
    QString state;
    QString path;
    bool candidate = false;
};

CatalogueRowView catalogueRowAt(QTableWidget* table, int row) {
    CatalogueRowView view;
    if (table == nullptr || row < 0 || row >= table->rowCount()) return view;
    const QTableWidgetItem* item = table->item(row, 0);
    if (item == nullptr) return view;
    view.row = row;
    view.id = item->data(Qt::UserRole).toString();
    view.candidate = item->data(Qt::UserRole + 1).toBool();
    view.origin = item->data(Qt::UserRole + 2).toString();
    view.state = item->data(Qt::UserRole + 3).toString();
    view.path = item->data(Qt::UserRole + 4).toString();
    return view;
}

CatalogueRowView catalogueRowForPath(QTableWidget* table, const QString& path) {
    if (table == nullptr) return {};
    const QString cleaned = QDir::cleanPath(path);
    const QString canonical = QFileInfo(path).canonicalFilePath();
    for (int row = 0; row < table->rowCount(); ++row) {
        const CatalogueRowView view = catalogueRowAt(table, row);
        if (view.row < 0) continue;
        if (QDir::cleanPath(view.path) == cleaned) return view;
        if (!canonical.isEmpty() &&
            QFileInfo(view.path).canonicalFilePath().compare(canonical,
                                                             Qt::CaseInsensitive) == 0) {
            return view;
        }
    }
    return {};
}

QByteArray fileBytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

struct AtomicReplaceOutcome {
    bool opened = false;
    bool wrote = false;
    bool replaced = false;
    int attempts = 0;
    QString error;
};

/// Replaces @p path with @p bytes through a QSaveFile, which is the atomic
/// replace an external updater performs. On Windows Qt opens every file handle
/// without FILE_SHARE_DELETE, and QSaveFile commits through
/// SetFileInformationByHandle(FileRenameInfo, ReplaceIfExists), so commit()
/// returns false while *any* other handle holds the destination: a catalogue
/// scan, an integrity check or another process. No fixture handle may be left
/// open on the graph, and a fresh QSaveFile is retried over a bounded window
/// the way a real updater retries a busy file. Success is always verified on
/// disk, so a replacement that never landed is never reported as one. The last
/// refusal is kept even after a later attempt succeeds, so a busy destination
/// stays visible in the log.
AtomicReplaceOutcome replaceFileAtomically(const QString& path, const QByteArray& bytes) {
    AtomicReplaceOutcome outcome;
    QElapsedTimer clock;
    clock.start();
    while (true) {
        ++outcome.attempts;
        {
            QSaveFile replacement(path);
            if (replacement.open(QIODevice::WriteOnly)) {
                outcome.opened = true;
                if (replacement.write(bytes) == bytes.size()) {
                    outcome.wrote = true;
                    if (replacement.commit() && fileBytes(path) == bytes) {
                        outcome.replaced = true;
                        return outcome;
                    }
                    outcome.error = replacement.errorString().isEmpty()
                                        ? QStringLiteral("the replacement could not replace the "
                                                         "destination file")
                                        : replacement.errorString();
                } else {
                    outcome.error = replacement.errorString().isEmpty()
                                        ? QStringLiteral("the replacement could not be written")
                                        : replacement.errorString();
                }
            } else {
                outcome.error = replacement.errorString().isEmpty()
                                    ? QStringLiteral("the destination is not writable")
                                    : replacement.errorString();
            }
        }
        if (clock.elapsed() >= 3000) break;
        QThread::msleep(50);
    }
    return outcome;
}

/// Clicks the manager's model action only when the selected row really is an
/// unregistered candidate. With any other selection the same button opens a
/// modal file picker, which this headless probe cannot drive, so a failed
/// precondition would hang the run instead of reporting it.
bool clickImportForCandidate(QTableWidget* table, QPushButton* button,
                             const char* failedMessage) {
    const CatalogueRowView selected =
        catalogueRowAt(table, table != nullptr ? table->currentRow() : -1);
    if (button == nullptr || selected.row < 0 || !selected.candidate) {
        check(false, failedMessage);
        return false;
    }
    button->click();
    return true;
}

QString customPresetId(const QString& stem) {
    return QStringLiteral("spatial:custom:") + stem;
}

bool presetCatalogueHas(const QString& id) {
    for (const auto& preset : mplayer::native::animeJanaiPresets()) {
        if (preset.id == id) return true;
    }
    return false;
}

mplayer::shell::MenuPopup* enhancementLevelMenu(mplayer::shell::PlayerShell& player,
                                                const QString& level) {
    for (mplayer::shell::MenuPopup* menu :
         player.findChildren<mplayer::shell::MenuPopup*>()) {
        if (menu->property("enhancementLevel").toString() == level) return menu;
    }
    return nullptr;
}

bool menuHasPayload(mplayer::shell::MenuPopup* menu, const QString& payload) {
    if (menu == nullptr) return false;
    for (int index = 0; index < menu->rowCount(); ++index) {
        if (menu->entryAt(index).payload.toString() == payload) return true;
    }
    return false;
}

mplayer::shell::MenuEntry menuEntryForPayload(mplayer::shell::MenuPopup* menu,
                                              const QString& payload) {
    if (menu != nullptr) {
        for (int index = 0; index < menu->rowCount(); ++index) {
            const auto entry = menu->entryAt(index);
            if (entry.payload.toString() == payload) return entry;
        }
    }
    return {};
}

bool clickMenuPayload(mplayer::shell::MenuPopup* menu, const QString& payload) {
    if (menu == nullptr) return false;
    for (int index = 0; index < menu->rowCount(); ++index) {
        if (menu->entryAt(index).payload.toString() != payload) continue;
        QWidget* row = menu->rowWidget(index);
        if (row == nullptr) return false;
        QTest::mouseClick(row, Qt::LeftButton, Qt::NoModifier, row->rect().center());
        QApplication::processEvents();
        return true;
    }
    return false;
}

QTreeWidgetItem* benchmarkItem(QTreeWidget* tree, const QString& id) {
    if (tree == nullptr) return nullptr;
    for (int groupIndex = 0; groupIndex < tree->topLevelItemCount(); ++groupIndex) {
        const QTreeWidgetItem* group = tree->topLevelItem(groupIndex);
        for (int row = 0; row < group->childCount(); ++row) {
            QTreeWidgetItem* item = group->child(row);
            if (item->data(0, Qt::UserRole + 1).toString() == id) return item;
        }
    }
    return nullptr;
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
    bool externalRegistrationClicked = false;
    if (externalCandidateVisible && importModel != nullptr) {
        modelTable->setCurrentCell(
            customModelRow(modelTable, QStringLiteral("external_smoke")), 0);
        QApplication::processEvents();
        externalRegistrationClicked = clickImportForCandidate(
            modelTable, importModel,
            "the external_smoke candidate was selected as the candidate to register");
    }
    const bool externalRegistered = externalRegistrationClicked && waitForUi(
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

/// Filesystem discovery seen through the public surfaces the viewer actually
/// uses: the Settings manager table and actions, Benchmark's catalogue, the
/// player's enhancement menu and Compare's candidate list. Every consumer is
/// constructed and visible before a graph is pasted, so a pass here is a real
/// refresh of an already-built UI and not a fresh dialog reading the disk.
void exerciseLiveModelDiscovery(mikai::backend::MikaiClient& client, Runner& runner) {
    const QString catalog = qEnvironmentVariable("MIKAI_PROBE_TRANSLATION");
    check(mikai::app::applyInterfaceLanguage(QStringLiteral("en"), catalog),
          "the live model discovery scenario runs in English");

    QTemporaryDir workDir;
    QTemporaryDir externalDir;
    check(workDir.isValid() && externalDir.isValid(),
          "the live catalogue scenario has isolated work and external folders");
    if (!workDir.isValid() || !externalDir.isValid()) return;

    const QString customDir = mplayer::native::customAnimeJanaiModelsDir();
    check(QDir().mkpath(customDir), "the isolated owned custom model folder exists");
    const QString rawPath = QDir(customDir).filePath(QStringLiteral("raw_live_model.onnx"));
    const QString decoyPath = QDir(customDir).filePath(QStringLiteral("owned_decoy.onnx"));
    const QString renamedDecoyPath =
        QDir(customDir).filePath(QStringLiteral("owned_decoy_renamed.onnx"));
    const QString externalPath =
        QDir(externalDir.path()).filePath(QStringLiteral("external_live_model.onnx"));
    const QString recreatedExternalPath =
        QDir(externalDir.path()).filePath(QStringLiteral("external_recreated.onnx"));
    const QString rawPreset = customPresetId(QStringLiteral("raw_live_model"));
    const QString externalPreset = customPresetId(QStringLiteral("external_live_model"));
    const QString recreatedPreset = customPresetId(QStringLiteral("external_recreated"));
    const QByteArray modelProto = mikai::tests::fixtures::makeTestIdentityOnnxGraph();
    check(modelProto.size() >= 64, "a real ONNX graph is available for live catalogue paste");
    for (const QString& stale : {rawPath, rawPath + QStringLiteral(".source.json"), decoyPath,
                                 renamedDecoyPath, externalPath, recreatedExternalPath}) {
        QFile::remove(stale);
    }

    SettingsDialog managerDialog(client, runner);
    const QSize managerWindowSize = fitSettingsDialogSize(
        managerDialog,
        QSize(mikai::app::theme::kSettingsDialogMinWidth,
              mikai::app::theme::kSettingsDialogDefaultHeight));
    managerDialog.resize(managerWindowSize);
    managerDialog.showProcessingTab();
    managerDialog.show();
    QApplication::processEvents();
    auto* managerRuntime =
        managerDialog.findChild<QComboBox*>(QStringLiteral("runtimeChoiceCombo"));
    if (managerRuntime != nullptr) {
        managerRuntime->setCurrentIndex(managerRuntime->findData(
            static_cast<int>(mplayer::native::RuntimeChoice::Standard)));
    }
    QApplication::processEvents();
    auto* panelWidget = managerDialog.findChild<mikai::app::AnimeJanaiManagerPanel*>(
        QStringLiteral("animeJanaiManagerPanel"));
    check(panelWidget != nullptr && panelWidget->isVisibleTo(&managerDialog),
          "the manager exists and is visible before the catalogue changes");
    if (panelWidget == nullptr) return;
    mikai::app::AnimeJanaiManagerPanel& panel = *panelWidget;
    mikai::probes::revealWidget(&panel);
    QApplication::processEvents();

    QTableWidget* modelTable = customModelTable(panel);
    QPushButton* importModel =
        panel.findChild<QPushButton*>(QStringLiteral("ajiImportModelButton"));
    QPushButton* removeModel =
        panel.findChild<QPushButton*>(QStringLiteral("ajiRemoveModelButton"));
    auto* slotSelector =
        panel.findChild<QComboBox*>(QStringLiteral("ajiSlotSelectCombo"));
    auto* externalEdit =
        panel.findChild<QLineEdit*>(QStringLiteral("ajiExternalModelsDirEdit"));
    auto* profileName = panel.findChild<QLineEdit*>(QStringLiteral("ajiProfileNameEdit"));
    check(modelTable != nullptr && importModel != nullptr && removeModel != nullptr &&
              slotSelector != nullptr && externalEdit != nullptr && profileName != nullptr,
          "the manager exposes its model table, registration and draft controls");
    if (modelTable == nullptr || importModel == nullptr || removeModel == nullptr ||
        slotSelector == nullptr || externalEdit == nullptr || profileName == nullptr) {
        return;
    }

    externalEdit->setText(externalDir.path());
    externalEdit->setFocus();
    QTest::keyClick(externalEdit, Qt::Key_Tab);
    QApplication::processEvents();
    const QSize managerInitialSize = managerDialog.size();

    const int slotOne = slotSelector->findData(1);
    if (slotOne >= 0) slotSelector->setCurrentIndex(slotOne);
    profileName->setFocus();
    profileName->selectAll();
    QTest::keyClicks(profileName, QStringLiteral("Live Draft Profile"));
    QPushButton* addChain =
        buttonForCaption(panel, mikai::app::AnimeJanaiManagerPanel::tr("Add Chain"));
    QPushButton* addStep =
        panel.findChild<QPushButton*>(QStringLiteral("ajiAddStepButton"));
    check(addChain != nullptr && addStep != nullptr,
          "the manager exposes the draft chain editing actions");
    if (addChain != nullptr) addChain->click();
    if (addStep != nullptr) addStep->click();
    QApplication::processEvents();
    auto* firstStepKind =
        panel.findChild<QComboBox*>(QStringLiteral("animeJanaiStepKind_1"));
    auto* firstStepId =
        panel.findChild<QComboBox*>(QStringLiteral("animeJanaiStepId_1"));
    if (firstStepKind != nullptr) {
        firstStepKind->setCurrentIndex(firstStepKind->findData(
            static_cast<int>(mplayer::native::AnimeJanaiStepKind::Anime4K)));
    }
    QApplication::processEvents();
    firstStepId = panel.findChild<QComboBox*>(QStringLiteral("animeJanaiStepId_1"));
    if (firstStepId != nullptr) {
        firstStepId->setCurrentIndex(firstStepId->findData(QStringLiteral("fast_a")));
    }
    QApplication::processEvents();
    QComboBox* chainCombo =
        comboForLabel(panel, mikai::app::AnimeJanaiManagerPanel::tr("Active chain"));
    check(chainCombo != nullptr, "the manager exposes its active chain selector");
    QApplication::processEvents();

    auto draftConfig = panel.currentConfig();
    const QString draftProfileName = profileName->text();
    const int draftSlot = slotSelector->currentData().toInt();
    int draftChainIndex = chainCombo != nullptr ? chainCombo->currentIndex() : -1;
    QString draftStepId =
        firstStepId != nullptr ? firstStepId->currentData().toString() : QString();
    check(panel.isDirty() && draftProfileName == QStringLiteral("Live Draft Profile") &&
              draftStepId == QStringLiteral("fast_a") &&
              QDir::cleanPath(draftConfig.externalModelsDir) ==
                  QDir::cleanPath(externalDir.path()),
          "the manager holds an unsaved profile, chain and external-folder draft");

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
    benchmarkDialog.resize(managerWindowSize);
    benchmarkDialog.show();
    QApplication::processEvents();
    auto* benchmarkTree =
        benchmarkDialog.findChild<QTreeWidget*>(QStringLiteral("benchmarkModelTree"));
    auto* benchmarkSearch =
        benchmarkDialog.findChild<QLineEdit*>(QStringLiteral("benchmarkModelSearch"));
    check(benchmarkTree != nullptr && benchmarkSearch != nullptr &&
              benchmarkPage != nullptr && benchmarkPage->isVisibleTo(&benchmarkDialog),
          "Benchmark's catalogue exists and is visible before the graph is pasted");
    if (benchmarkTree == nullptr || benchmarkSearch == nullptr) return;
    QTreeWidgetItem* stableBenchmarkRow = benchmarkItem(benchmarkTree, QStringLiteral("builtin1001"));
    check(stableBenchmarkRow != nullptr, "Benchmark offers a stable built-in row to preserve");
    if (stableBenchmarkRow != nullptr) stableBenchmarkRow->setCheckState(0, Qt::Checked);
    benchmarkSearch->setText(QStringLiteral("spatial"));
    QApplication::processEvents();
    const QString benchmarkFilter = benchmarkSearch->text();
    const bool stableBenchmarkChecked =
        stableBenchmarkRow != nullptr && stableBenchmarkRow->checkState(0) == Qt::Checked;
    check(benchmarkFilter == QStringLiteral("spatial") && stableBenchmarkChecked,
          "Benchmark holds a user filter and checked stable model before the change");
    check(benchmarkItem(benchmarkTree, rawPreset) == nullptr,
          "Benchmark does not list an unregistered raw paste");

    const auto selectedRuntime = mplayer::native::selectStandardRuntime(
        qEnvironmentVariable("MIKAI_MPV_MODULE"));
    check(selectedRuntime.loaded &&
              selectedRuntime.kind == mplayer::native::BundleKind::Standard,
          "the real Standard runtime is available for the live player and Compare surfaces");
    mplayer::shell::PlayerShell player;
    player.resize(QSize(900, 640));
    player.show();
    QApplication::processEvents();
    auto* enhancementButton =
        player.findChild<QPushButton*>(QStringLiteral("playerEnhanceButton"));
    auto* enhancementRoot =
        player.findChild<mplayer::shell::MenuPopup*>(QStringLiteral("playerEnhancementsPopup"));
    check(enhancementButton != nullptr && enhancementRoot != nullptr,
          "the player enhancement menu exists before the catalogue changes");
    if (enhancementButton == nullptr || enhancementRoot == nullptr) return;
    enhancementButton->click();
    QApplication::processEvents();
    const bool enteredRoot =
        clickMenuPayload(enhancementRoot, QStringLiteral("level|animejanai"));
    auto* animeJanaiMenu = enhancementLevelMenu(player, QStringLiteral("animejanai"));
    const bool enteredSpatial =
        animeJanaiMenu != nullptr &&
        clickMenuPayload(animeJanaiMenu, QStringLiteral("level|animejanai:spatial"));
    auto* spatialMenu = enhancementLevelMenu(player, QStringLiteral("animejanai:spatial"));
    check(enteredRoot && enteredSpatial && spatialMenu != nullptr && spatialMenu->isVisible(),
          "the player's open Spatial menu is visible before the graph is pasted");
    if (spatialMenu == nullptr) return;
    check(!menuHasPayload(spatialMenu, QStringLiteral("ajipreset|") + rawPreset),
          "the player menu does not offer an unregistered raw paste");
    auto* playerChip =
        player.findChild<QLabel*>(QStringLiteral("playerAnimeJanaiStatus"));
    check(playerChip != nullptr, "the player exposes its AnimeJaNai status chip");

    QImage compareFixture(64, 36, QImage::Format_RGB32);
    for (int y = 0; y < compareFixture.height(); ++y) {
        for (int x = 0; x < compareFixture.width(); ++x) {
            compareFixture.setPixelColor(
                x, y, QColor((x * 4) % 256, (y * 7) % 256, ((x + y) * 3) % 256));
        }
    }
    const QString compareFixturePath =
        QDir(workDir.path()).filePath(QStringLiteral("live-compare-frame.png"));
    check(compareFixture.save(compareFixturePath),
          "a static Compare frame was written for the live catalogue scenario");
    mplayer::VideoSurface* compareSurface = player.videoSurface();
    check(compareSurface != nullptr && compareSurface->open(compareFixturePath) &&
              waitForUi([&] {
                  return compareSurface->hasVideo() && compareSurface->videoWidth() > 0;
              }, 5000),
          "the player surface holds the static Compare frame before the change");
    QStringList shaderCandidates;
    const QString shaderRoot = mplayer::anime4k::findShadersDir();
    for (const auto& mode : mplayer::anime4k::availableModes()) {
        const auto candidate = mplayer::anime4k::candidateForId(mode.id, shaderRoot);
        if (mplayer::anime4k::chainIsComplete(candidate, shaderRoot)) {
            shaderCandidates.append(mode.id);
        }
    }
    check(shaderCandidates.size() >= 2,
          "two shader-only Compare candidates avoid native inference in this scenario");
    if (shaderCandidates.size() < 2) return;
    mplayer::shell::CompareWindow comparison(compareSurface, shaderCandidates.value(0), &player);
    comparison.resize(QSize(900, 600));
    comparison.show();
    const bool compareFrozen = waitForUi(
        [&] { return !comparison.frozenFrame().isNull() && !comparison.comparisonPending(); },
        10000);
    check(compareFrozen, "Compare froze the static frame before the catalogue change");
    if (!compareFrozen) return;
    const QImage frozenBefore = comparison.frozenFrame();
    const QString selectedBefore = comparison.selectedCandidateId(0);
    check(!selectedBefore.isEmpty() && !comparison.candidateAvailable(rawPreset),
          "Compare holds a stable selection and does not offer the raw paste");

    // ── a raw graph pasted into the app-owned folder ─────────────────────────
    {
        QFile raw(rawPath);
        check(raw.open(QIODevice::WriteOnly) && raw.write(modelProto) == modelProto.size(),
              "a raw ONNX graph was pasted into the app-owned custom folder");
        raw.close();
    }
    const bool rawCandidateVisible = waitForUi(
        [&] {
            const auto row = catalogueRowForPath(modelTable, rawPath);
            return row.row >= 0 && row.id == QStringLiteral("raw_live_model") &&
                   row.origin == QStringLiteral("owned") &&
                   row.state == QStringLiteral("unregistered") && row.candidate;
        },
        15000);
    check(rawCandidateVisible,
          "the open manager presents the owned raw paste as an unregistered candidate");
    const auto rawCandidate = catalogueRowForPath(modelTable, rawPath);
    check(rawCandidate.row >= 0 &&
              modelTable->item(rawCandidate.row, 4)->text().startsWith(
                  QStringLiteral("Not registered")),
          "the candidate row names registration as the missing step");
    check(!presetCatalogueHas(rawPreset),
          "the raw owned paste is not a usable preset before explicit registration");
    check(!menuHasPayload(spatialMenu, QStringLiteral("ajipreset|") + rawPreset),
          "the open player menu refuses the unregistered raw paste");
    check(!comparison.candidateAvailable(rawPreset),
          "the open Compare catalogue refuses the unregistered raw paste");
    check(benchmarkItem(benchmarkTree, rawPreset) == nullptr,
          "the open Benchmark catalogue refuses the unregistered raw paste");

    if (rawCandidate.row >= 0) modelTable->setCurrentCell(rawCandidate.row, 0);
    QApplication::processEvents();

    // A second unrelated paste must not move the selected candidate, reload the
    // draft or lose the editor focus state.
    {
        QFile decoy(decoyPath);
        check(decoy.open(QIODevice::WriteOnly) &&
                  decoy.write(modelProto) == modelProto.size(),
              "a second unrelated owned graph was pasted");
        decoy.close();
    }
    check(waitForUi(
              [&] { return catalogueRowForPath(modelTable, decoyPath).row >= 0; }, 15000),
          "the unrelated paste appeared without a manual refresh");
    const auto currentAfterSecondPaste = catalogueRowAt(modelTable, modelTable->currentRow());
    check(currentAfterSecondPaste.row >= 0 &&
              currentAfterSecondPaste.id == QStringLiteral("raw_live_model") &&
              currentAfterSecondPaste.state == QStringLiteral("unregistered") &&
              catalogueRowForPath(modelTable, rawPath).row ==
                  currentAfterSecondPaste.row,
          "the manager keeps the selected candidate across an unrelated paste");
    check(panel.currentConfig() == draftConfig && panel.isDirty() &&
              profileName->text() == draftProfileName &&
              slotSelector->currentData().toInt() == draftSlot &&
              chainCombo != nullptr && chainCombo->currentIndex() == draftChainIndex,
          "the unsaved profile, chain, slot and dirty state survive the paste");

    // ── explicit in-place registration ──────────────────────────────────────
    const auto selectedRaw = catalogueRowForPath(modelTable, rawPath);
    if (selectedRaw.row >= 0) modelTable->setCurrentCell(selectedRaw.row, 0);
    QApplication::processEvents();
    const bool ownedImportClicked = clickImportForCandidate(
        modelTable, importModel,
        "the owned raw paste was selected as the candidate to register");
    QApplication::processEvents();
    check(QApplication::activeModalWidget() == nullptr,
          "registering a selected owned candidate needs no copy chooser");
    const bool ownedRegistered = ownedImportClicked && waitForUi(
        [&] {
            const auto row = catalogueRowForPath(modelTable, rawPath);
            return row.row >= 0 && row.origin == QStringLiteral("owned") &&
                   row.state == QStringLiteral("registered") && !row.candidate;
        },
        20000);
    check(ownedRegistered, "the Import action registered the raw paste in place");
    check(QFileInfo(rawPath).isFile() && QFileInfo(rawPath).size() == modelProto.size() &&
              fileBytes(rawPath) == modelProto &&
              QFileInfo(rawPath + QStringLiteral(".source.json")).isFile(),
          "in-place registration published only the integrity record beside the graph");
    const auto rawRegisteredRow = catalogueRowForPath(modelTable, rawPath);
    check(rawRegisteredRow.row >= 0 && rawRegisteredRow.id == QStringLiteral("raw_live_model") &&
              QFileInfo(rawRegisteredRow.path).size() == modelProto.size(),
          "the registered row keeps the original identifier and file");
    check(waitForUi([&] { return presetCatalogueHas(rawPreset); }, 5000),
          "the registered owned graph became a usable catalogue preset");
    const bool playerSawOwned = waitForUi(
        [&] {
            const auto entry =
                menuEntryForPayload(spatialMenu, QStringLiteral("ajipreset|") + rawPreset);
            return !entry.text.isEmpty() && !entry.disabled;
        },
        15000);
    check(playerSawOwned,
          "the open player Spatial menu gained the registered graph without reopening");
    const bool benchmarkSawOwned = waitForUi(
        [&] { return benchmarkItem(benchmarkTree, rawPreset) != nullptr; }, 15000);
    check(benchmarkSawOwned, "the open Benchmark catalogue gained the registered graph");
    const bool compareSawOwned = waitForUi(
        [&] { return comparison.candidateAvailable(rawPreset); }, 15000);
    check(compareSawOwned, "the open Compare catalogue gained the registered graph");
    QTreeWidgetItem* ownedBenchmarkRow = benchmarkItem(benchmarkTree, rawPreset);
    check(ownedBenchmarkRow != nullptr && ownedBenchmarkRow->checkState(0) == Qt::Unchecked,
          "a newly discovered Benchmark model starts unchecked");
    if (ownedBenchmarkRow != nullptr) ownedBenchmarkRow->setCheckState(0, Qt::Checked);
    QApplication::processEvents();

    player.setAnimeJanaiSelection(rawPreset, QStringLiteral("Configured; live probe"));
    QApplication::processEvents();
    check(playerChip != nullptr && playerChip->isVisibleTo(&player) &&
              !playerChip->accessibleName().contains(QStringLiteral("Unavailable")),
          "the player chip shows the registered selection");

    check(panel.currentConfig() == draftConfig && panel.isDirty() &&
              profileName->text() == draftProfileName,
          "registration preserves the unsaved profile and chain draft");
    QTreeWidgetItem* restoredStableRow =
        benchmarkItem(benchmarkTree, QStringLiteral("builtin1001"));
    check(benchmarkSearch->text() == benchmarkFilter && restoredStableRow != nullptr &&
              restoredStableRow->checkState(0) == Qt::Checked,
          "registration preserves Benchmark's filter and checked stable model");
    check(comparison.frozenFrame() == frozenBefore &&
              comparison.selectedCandidateId(0) == selectedBefore &&
              !comparison.comparisonPending(),
          "the open Compare keeps its frozen frame and selection through registration");

    // Pin the newly registered model into the unsaved chain so a later removal
    // can prove the selected id stays selected instead of being substituted.
    addStep->click();
    QApplication::processEvents();
    auto* registeredModelStep =
        panel.findChild<QComboBox*>(QStringLiteral("animeJanaiStepId_2"));
    const int registeredModelChoice =
        registeredModelStep != nullptr
            ? registeredModelStep->findData(QStringLiteral("raw_live_model"))
            : -1;
    check(registeredModelChoice >= 0,
          "the registered owned model is selectable as an ONNX chain step");
    if (registeredModelChoice >= 0) {
        registeredModelStep->setCurrentIndex(registeredModelChoice);
        QApplication::processEvents();
    }
    draftConfig = panel.currentConfig();
    draftChainIndex = chainCombo != nullptr ? chainCombo->currentIndex() : draftChainIndex;
    draftStepId = registeredModelStep != nullptr
                      ? registeredModelStep->currentData().toString()
                      : draftStepId;
    check(draftStepId == QStringLiteral("raw_live_model") && panel.isDirty(),
          "the unsaved draft now selects the registered model in its chain");
    check(managerDialog.grab().save(QStringLiteral("native-live-model-discovery.png")),
          "the manager's live registered catalogue was captured");
    check(benchmarkDialog.grab().save(QStringLiteral("native-live-benchmark.png")),
          "Benchmark's live catalogue was captured");
    check(player.grab().save(QStringLiteral("native-live-player-menu.png")),
          "the player's live Spatial menu was captured");
    check(comparison.grab().save(QStringLiteral("native-live-compare.png")),
          "Compare's live candidate surface was captured");

    // ── a borrowed external graph stays a candidate until pinned ────────────
    {
        QFile external(externalPath);
        check(external.open(QIODevice::WriteOnly) &&
                  external.write(modelProto) == modelProto.size(),
              "an external-folder ONNX graph was pasted");
        external.close();
    }
    const bool externalCandidateVisible = waitForUi(
        [&] {
            const auto row = catalogueRowForPath(modelTable, externalPath);
            return row.row >= 0 && row.origin == QStringLiteral("external") &&
                   row.state == QStringLiteral("unregistered") && row.candidate;
        },
        15000);
    check(externalCandidateVisible,
          "the open manager presents the external paste as an unregistered candidate");
    check(!presetCatalogueHas(externalPreset),
          "the borrowed external graph is not a preset before explicit registration");
    check(!menuHasPayload(spatialMenu, QStringLiteral("ajipreset|") + externalPreset),
          "the open player menu refuses the unpinned external graph");
    check(!comparison.candidateAvailable(externalPreset) &&
              benchmarkItem(benchmarkTree, externalPreset) == nullptr,
          "Compare and Benchmark refuse the unregistered external graph");
    const auto externalRow = catalogueRowForPath(modelTable, externalPath);
    if (externalRow.row >= 0) modelTable->setCurrentCell(externalRow.row, 0);
    QApplication::processEvents();
    const bool externalImportClicked = clickImportForCandidate(
        modelTable, importModel,
        "the borrowed external paste was selected as the candidate to pin");
    const bool externalRegistered = externalImportClicked && waitForUi(
        [&] {
            const auto row = catalogueRowForPath(modelTable, externalPath);
            return row.row >= 0 && row.origin == QStringLiteral("external") &&
                   row.state == QStringLiteral("registered") && !row.candidate;
        },
        20000);
    check(externalRegistered, "the Import action pinned the external graph explicitly");
    check(QFileInfo(externalPath).isFile() && fileBytes(externalPath) == modelProto &&
              !QFileInfo::exists(externalPath + QStringLiteral(".source.json")),
          "borrowed registration never writes beside the user's external graph");
    check(waitForUi([&] { return presetCatalogueHas(externalPreset); }, 5000),
          "the pinned external graph became a usable preset");
    check(waitForUi(
              [&] { return comparison.candidateAvailable(externalPreset); }, 15000),
          "the open Compare catalogue gained the pinned external graph");
    const bool externalInBenchmark = waitForUi(
        [&] { return benchmarkItem(benchmarkTree, externalPreset) != nullptr; }, 15000);
    check(externalInBenchmark,
          "the open Benchmark catalogue gained the pinned external graph");
    QTreeWidgetItem* externalBenchmarkRow = benchmarkItem(benchmarkTree, externalPreset);
    check(externalBenchmarkRow != nullptr &&
              externalBenchmarkRow->checkState(0) == Qt::Unchecked,
          "a newly discovered external Benchmark model starts unchecked");
    if (externalBenchmarkRow != nullptr)
        externalBenchmarkRow->setCheckState(0, Qt::Checked);
    QApplication::processEvents();

    // ── atomic replacement of the pinned external graph ────────────────────
    const QByteArray externalOriginal = fileBytes(externalPath);
    // A different-sized but still well-framed ModelProto: a duplicate
    // ir_version varint keeps the protobuf stream valid for the bounded
    // validator while making the byte count differ from the existing pin.
    QByteArray externalReplacement = modelProto;
    externalReplacement.append(QByteArrayLiteral("\x08\x01"));
    check(externalOriginal == modelProto &&
              externalReplacement.size() != externalOriginal.size(),
          "a different-sized replacement graph was prepared for the pinned model");

    // The fixture holds no handle on the graph: every paste in this scenario
    // scopes and closes its QFile before the manager is asked to notice it.
    // A QSaveFile still retries a refused commit, because Windows refuses the
    // rename while any other handle holds the destination, and one short-lived
    // reader (a catalogue scan, an integrity check or another process) must not
    // make the scenario report a replacement it never performed.
    const AtomicReplaceOutcome replacement =
        replaceFileAtomically(externalPath, externalReplacement);
    check(replacement.opened,
          "the pinned external graph is writable for an atomic replacement");
    check(replacement.wrote, "the atomic replacement wrote the replacement graph");
    check(replacement.replaced, "the replacement was committed atomically",
          replacement.replaced
              ? QString()
              : QStringLiteral("%1 (after %2 attempts)")
                    .arg(replacement.error)
                    .arg(replacement.attempts));
    if (replacement.replaced && replacement.attempts > 1) {
        std::fprintf(stderr, "note the atomic replacement needed %d attempts: %s\n",
                     replacement.attempts, qPrintable(replacement.error));
    }
    const bool replacementLanded =
        replacement.replaced && fileBytes(externalPath) == externalReplacement &&
        QFileInfo(externalPath).size() == externalReplacement.size();
    check(replacementLanded,
          "the external graph now holds the different-sized replacement");

    // Everything below observes the app's reaction to a replacement that really
    // landed. Without one there is no invalidation to observe and no candidate
    // to re-pin, so the failed replace is reported instead of a re-pin that
    // would silently register the untouched graph or open a modal picker.
    if (replacementLanded) {
        const bool externalReplacementInvalidated = waitForUi(
            [&] {
                const auto row = catalogueRowForPath(modelTable, externalPath);
                return row.row >= 0 && row.origin == QStringLiteral("external") &&
                       row.state == QStringLiteral("unregistered") && row.candidate;
            },
            15000);
        check(externalReplacementInvalidated,
              "the open manager noticed the atomic replacement and invalidated the pin");
        const auto changedExternalRow = catalogueRowForPath(modelTable, externalPath);
        check(changedExternalRow.row >= 0 &&
                  modelTable->item(changedExternalRow.row, 4)->text().startsWith(
                      QStringLiteral("Not registered")),
              "the replaced external model is presented as needing an explicit re-pin");
        check(waitForUi([&] { return !presetCatalogueHas(externalPreset); }, 5000),
              "the replaced external graph leaves the preset catalogue without a fallback");
        check(waitForUi(
                  [&] {
                      const auto entry = menuEntryForPayload(
                          spatialMenu, QStringLiteral("ajipreset|") + externalPreset);
                      return entry.text.isEmpty();
                  },
                  15000),
              "the open player Spatial menu dropped the replaced external graph");
        check(waitForUi([&] { return !comparison.candidateAvailable(externalPreset); },
                        15000),
              "the open Compare catalogue dropped the replaced external graph");
        check(waitForUi(
                  [&] {
                      QTreeWidgetItem* missing = benchmarkItem(benchmarkTree, externalPreset);
                      return missing != nullptr &&
                             !(missing->flags() & Qt::ItemIsUserCheckable) &&
                             missing->checkState(0) == Qt::Checked;
                  },
                  15000),
              "Benchmark keeps the replaced checked external model visible and unrunnable");
        check(panel.currentConfig() == draftConfig && panel.isDirty() &&
                  profileName->text() == draftProfileName &&
                  slotSelector->currentData().toInt() == draftSlot &&
                  chainCombo != nullptr && chainCombo->currentIndex() == draftChainIndex,
              "the atomic replacement preserves the unsaved manager profile and chain");
        QTreeWidgetItem* preservedStableRow =
            benchmarkItem(benchmarkTree, QStringLiteral("builtin1001"));
        check(benchmarkSearch->text() == benchmarkFilter &&
                  preservedStableRow != nullptr &&
                  preservedStableRow->checkState(0) == Qt::Checked &&
                  comparison.frozenFrame() == frozenBefore &&
                  comparison.selectedCandidateId(0) == selectedBefore,
              "the atomic replacement preserves Benchmark's filter and Compare's selection");
        const auto changedExternal = catalogueRowForPath(modelTable, externalPath);
        if (changedExternal.row >= 0)
            modelTable->setCurrentCell(changedExternal.row, 0);
        QApplication::processEvents();
        const bool repinClicked = clickImportForCandidate(
            modelTable, importModel,
            "the replaced external model was selected as the candidate to re-pin");
        const bool externalRepinned = repinClicked && waitForUi(
            [&] {
                const auto row = catalogueRowForPath(modelTable, externalPath);
                return row.row >= 0 && row.origin == QStringLiteral("external") &&
                       row.state == QStringLiteral("registered") && !row.candidate;
            },
            20000);
        check(externalRepinned,
              "the Import action explicitly re-pinned the atomically replaced graph");
        check(fileBytes(externalPath) == externalReplacement &&
                  !QFileInfo::exists(externalPath + QStringLiteral(".source.json")),
              "re-pinning keeps the replaced graph in the user's external folder");
        check(waitForUi([&] { return presetCatalogueHas(externalPreset); }, 5000),
              "the re-pinned replacement graph returned to the preset catalogue");
        check(waitForUi(
                  [&] {
                      const auto entry = menuEntryForPayload(
                          spatialMenu, QStringLiteral("ajipreset|") + externalPreset);
                      return !entry.text.isEmpty() && !entry.disabled;
                  },
                  15000),
              "the open player menu offers the re-pinned replacement graph");
        check(waitForUi([&] { return comparison.candidateAvailable(externalPreset); },
                        15000),
              "the open Compare catalogue offers the re-pinned replacement graph");
        const bool repinnedInBenchmark = waitForUi(
            [&] {
                QTreeWidgetItem* row = benchmarkItem(benchmarkTree, externalPreset);
                return row != nullptr && (row->flags() & Qt::ItemIsUserCheckable) &&
                       row->checkState(0) == Qt::Checked;
            },
            15000);
        check(repinnedInBenchmark,
              "Benchmark restores the re-pinned model with its checked state");
    } else {
        check(false,
              "the pin invalidation, explicit re-pin and restored-catalogue checks ran "
              "against a graph that really changed on disk");
    }

    // ── rename and delete of an unregistered owned paste ────────────────────
    check(QFile::rename(decoyPath, renamedDecoyPath),
          "the unrelated owned paste was renamed on disk");
    const bool decoyRenamed = waitForUi(
        [&] {
            return catalogueRowForPath(modelTable, renamedDecoyPath).row >= 0 &&
                   catalogueRowForPath(modelTable, decoyPath).row < 0;
        },
        15000);
    check(decoyRenamed,
          "the open manager followed the rename without a manual refresh");
    check(catalogueRowForPath(modelTable, renamedDecoyPath).id ==
              QStringLiteral("owned_decoy_renamed") &&
              !presetCatalogueHas(customPresetId(QStringLiteral("owned_decoy_renamed"))),
          "the renamed raw paste is still an unregistered candidate, not a preset");
    check(QFile::remove(renamedDecoyPath), "the renamed candidate was deleted on disk");
    check(waitForUi(
              [&] { return catalogueRowForPath(modelTable, renamedDecoyPath).row < 0; },
              15000),
          "the open manager dropped the deleted candidate without a manual refresh");

    // ── deleting and recreating the external folder ─────────────────────────
    check(QDir(externalDir.path()).removeRecursively(),
          "the external model folder was removed while it was watched");
    const bool externalDisappeared = waitForUi(
        [&] {
            return catalogueRowForPath(modelTable, externalPath).row < 0 &&
                   !presetCatalogueHas(externalPreset);
        },
        15000);
    check(externalDisappeared,
          "removing the external folder removed its borrowed registration and preset");
    check(waitForUi(
              [&] {
                  QTreeWidgetItem* missing = benchmarkItem(benchmarkTree, externalPreset);
                  return missing != nullptr &&
                         !(missing->flags() & Qt::ItemIsUserCheckable) &&
                         missing->checkState(0) == Qt::Checked;
              },
              15000),
          "Benchmark keeps the missing checked external model visible and unrunnable");
    check(QDir().mkpath(externalDir.path()),
          "the external model folder was recreated while it was watched");
    {
        QFile recreated(recreatedExternalPath);
        check(recreated.open(QIODevice::WriteOnly) &&
                  recreated.write(modelProto) == modelProto.size(),
              "a graph was pasted into the recreated external folder");
        recreated.close();
    }
    const bool recreatedCandidateVisible = waitForUi(
        [&] {
            const auto row = catalogueRowForPath(modelTable, recreatedExternalPath);
            return row.row >= 0 && row.origin == QStringLiteral("external") &&
                   row.state == QStringLiteral("unregistered");
        },
        15000);
    check(recreatedCandidateVisible,
          "the recreated external folder presents its new graph as a candidate");
    check(!presetCatalogueHas(recreatedPreset),
          "the recreated folder's graph is not silently promoted to a preset");

    // ── explicit removal and a re-pasted raw graph ──────────────────────────
    const auto registeredRow = catalogueRowForPath(modelTable, rawPath);
    if (registeredRow.row >= 0) modelTable->setCurrentCell(registeredRow.row, 0);
    QApplication::processEvents();
    removeModel->click();
    const bool removedFromManager = waitForUi(
        [&] {
            return catalogueRowForPath(modelTable, rawPath).row < 0 &&
                   !presetCatalogueHas(rawPreset);
        },
        20000);
    check(removedFromManager,
          "Remove Selected deleted the registered owned graph and its preset");
    check(!QFileInfo::exists(rawPath) &&
              !QFileInfo::exists(rawPath + QStringLiteral(".source.json")),
          "owned removal deletes the app-owned graph and integrity record");
    // The retained selection is named in the AnimeJaNai level itself - the level
    // that lists Off, the built-in models and the explicit "Unavailable · id"
    // row. The ENHANCEMENTS root only holds the lead-in row that opens that
    // level, so a lookup of an "ajipreset|" payload there can never find it.
    // Every catalogue pass retires the level's rows with deleteLater(), so the
    // level and the chip are reacquired live rather than trusting the handles
    // recorded before the graph was deleted.
    const bool playerMissingAfterRemoval = waitForUi(
        [&] {
            auto* liveAnimeJanaiLevel =
                enhancementLevelMenu(player, QStringLiteral("animejanai"));
            if (liveAnimeJanaiLevel == nullptr || !liveAnimeJanaiLevel->isVisible()) {
                return false;
            }
            const auto missing = menuEntryForPayload(
                liveAnimeJanaiLevel, QStringLiteral("ajipreset|") + rawPreset);
            auto* liveChip =
                player.findChild<QLabel*>(QStringLiteral("playerAnimeJanaiStatus"));
            return missing.disabled && missing.current && liveChip != nullptr &&
                   liveChip->isVisibleTo(&player) &&
                   liveChip->accessibleName().contains(QStringLiteral("Unavailable"));
        },
        15000);
    check(playerMissingAfterRemoval,
          "removal leaves the player selection named unavailable, never silently replaced");
    check(waitForUi(
              [&] {
                  QTreeWidgetItem* missing = benchmarkItem(benchmarkTree, rawPreset);
                  return missing != nullptr &&
                         !(missing->flags() & Qt::ItemIsUserCheckable) &&
                         missing->checkState(0) == Qt::Checked;
              },
              15000),
          "Benchmark keeps the removed checked model visible but unrunnable");
    QPushButton* modelSelection = nullptr;
    for (QPushButton* button : benchmarkDialog.findChildren<QPushButton*>()) {
        if (button->objectName() == QLatin1String("textAction") &&
            button->property("benchmarkSelectionControl").toString() ==
                QLatin1String("models")) {
            modelSelection = button;
            break;
        }
    }
    check(modelSelection != nullptr,
          "Benchmark exposes its explicit model selection action");
    if (modelSelection != nullptr) {
        for (int attempt = 0; attempt < 3 &&
                              modelSelection->text() == SettingsDialog::tr("Select all");
             ++attempt) {
            modelSelection->click();
            QApplication::processEvents();
        }
        check(modelSelection->text() == SettingsDialog::tr("Clear selection"),
              "the model selection action reaches an explicit Clear selection");
        modelSelection->click();
        QApplication::processEvents();
        auto* benchmarkSummary =
            benchmarkDialog.findChild<QLabel*>(QStringLiteral("benchmarkRunSummary"));
        const bool releasedMissingModel = waitForUi(
            [&] {
                QTreeWidgetItem* missing = benchmarkItem(benchmarkTree, rawPreset);
                return missing == nullptr ||
                       missing->checkState(0) == Qt::Unchecked ||
                       !missing->data(0, Qt::UserRole + 2).toBool();
            },
            5000);
        check(releasedMissingModel && benchmarkSummary != nullptr &&
                  !benchmarkSummary->text().contains(QStringLiteral("unavailable")),
              "Clear selection releases a disappeared checked model instead of trapping it");
    }
    check(waitForUi([&] { return !comparison.candidateAvailable(rawPreset); }, 15000) &&
              comparison.frozenFrame() == frozenBefore &&
              comparison.selectedCandidateId(0) == selectedBefore,
          "Compare drops the removed candidate without moving its frozen selection");
    check(panel.currentConfig() == draftConfig && panel.isDirty() &&
              profileName->text() == draftProfileName &&
              slotSelector->currentData().toInt() == draftSlot &&
              chainCombo != nullptr && chainCombo->currentIndex() == draftChainIndex,
          "removal leaves the unsaved manager profile, chain and selection intact");
    auto* retainedStepCombo = panel.findChild<QComboBox*>(QStringLiteral("animeJanaiStepId_2"));
    check(retainedStepCombo != nullptr &&
              retainedStepCombo->currentData().toString() == draftStepId,
          "the manager chain retains the removed model id instead of substituting one");

    {
        QFile repasted(rawPath);
        check(repasted.open(QIODevice::WriteOnly) &&
                  repasted.write(modelProto) == modelProto.size(),
              "the graph was pasted into the owned folder again after removal");
        repasted.close();
    }
    check(waitForUi(
              [&] {
                  const auto row = catalogueRowForPath(modelTable, rawPath);
                  return row.row >= 0 && row.origin == QStringLiteral("owned") &&
                         row.state == QStringLiteral("unregistered");
              },
              15000),
          "the re-pasted graph returns as an unregistered candidate");
    check(!presetCatalogueHas(rawPreset),
          "the re-pasted graph is not usable until it is registered again");
    check(QFile::remove(rawPath), "the re-pasted candidate was deleted");
    check(waitForUi(
              [&] { return catalogueRowForPath(modelTable, rawPath).row < 0; }, 15000),
          "the open manager dropped the deleted re-paste without a manual refresh");

    check(managerDialog.grab().save(QStringLiteral("native-live-missing-model.png")),
          "the manager's explicit missing-model state was captured");
    check(player.grab().save(QStringLiteral("native-live-player-missing.png")),
          "the player's explicit unavailable selection was captured");
    managerDialog.resize(managerInitialSize);

    comparison.close();
    benchmarkDialog.reject();
    managerDialog.reject();
    QApplication::processEvents();
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

            // ── the selector previews, and every way out restores it ────────
            // Preview is visible before Save: the tab captions and the footer
            // action are the surfaces a user is looking at when they choose a
            // language. Save is the only path that may write the preference.
            check(mikai::app::applyInterfaceLanguage(QStringLiteral("en")),
                  "the preview section starts from the English translation");
            {
                QSettings settings;
                settings.setValue(QStringLiteral("ui/language"), QStringLiteral("en"));
                settings.sync();
            }
            const QString englishProcessing = QCoreApplication::translate(
                "mikai::app::SettingsDialog", "Processing");
            const QString englishSave = QCoreApplication::translate(
                "mikai::app::SettingsDialog", "Save");
            const auto languageSurface = [](SettingsDialog& dialog) {
                auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("settingsTabs"));
                const QString processing =
                    tabs != nullptr && tabs->count() > 2 ? tabs->tabText(2) : QString();
                QPushButton* save = acceptButton(dialog.findChild<QDialogButtonBox*>());
                return qMakePair(processing, save != nullptr ? save->text() : QString());
            };
            const auto storedLanguage = [] {
                QSettings settings;
                return settings.value(QStringLiteral("ui/language")).toString();
            };

            {
                SettingsDialog dialog(client, runner);
                dialog.resize(fitSettingsDialogSize(
                    dialog, mikai::probes::windowSize(QSize(760, 780))));
                if (auto* tabs = dialog.findChild<QTabWidget*>()) tabs->setCurrentIndex(0);
                dialog.show();
                QApplication::processEvents();
                const auto englishSurface = languageSurface(dialog);
                check(englishSurface.first == englishProcessing &&
                          englishSurface.second == englishSave,
                      "the visible Settings surface starts in English");
                auto* language =
                    dialog.findChild<QComboBox*>(QStringLiteral("uiLanguageCombo"));
                const int ukrainian =
                    language != nullptr ? language->findData(QStringLiteral("uk")) : -1;
                check(language != nullptr && ukrainian >= 0,
                      "the visible language selector offers Ukrainian");
                if (language != nullptr && ukrainian >= 0) {
                    language->setCurrentIndex(ukrainian);
                    QApplication::processEvents();
                    const QString ukrainianProcessing = QCoreApplication::translate(
                        "mikai::app::SettingsDialog", "Processing");
                    const QString ukrainianSave = QCoreApplication::translate(
                        "mikai::app::SettingsDialog", "Save");
                    const auto previewSurface = languageSurface(dialog);
                    check(mikai::app::activeInterfaceLanguage() == QStringLiteral("uk"),
                          "choosing Ukrainian previews the live translation without Save");
                    check(previewSurface.first == ukrainianProcessing &&
                              previewSurface.first != englishProcessing &&
                              previewSurface.second == ukrainianSave &&
                              previewSurface.second != englishSave,
                          "the visible tab and Save captions follow the preview immediately");
                    check(storedLanguage() == QStringLiteral("en"),
                          "the preview alone never writes the stored language");
                    const int english = language->findData(QStringLiteral("en"));
                    if (english >= 0) {
                        language->setCurrentIndex(english);
                        QApplication::processEvents();
                        check(mikai::app::activeInterfaceLanguage() == QStringLiteral("en") &&
                                  languageSurface(dialog) == englishSurface,
                              "selecting English previews back immediately as well");
                        language->setCurrentIndex(ukrainian);
                        QApplication::processEvents();
                    }
                    QPushButton* cancel =
                        rejectButton(dialog.findChild<QDialogButtonBox*>());
                    check(cancel != nullptr, "Cancel is visible for the preview round");
                    if (cancel != nullptr) cancel->click();
                    QApplication::processEvents();
                    check(dialog.result() == static_cast<int>(QDialog::Rejected) &&
                              mikai::app::activeInterfaceLanguage() == QStringLiteral("en"),
                          "Cancel restores the language the dialog opened with");
                    check(languageSurface(dialog) == englishSurface &&
                              storedLanguage() == QStringLiteral("en"),
                          "Cancel restores the visible surface and leaves the preference alone");
                }
            }

            {
                SettingsDialog dialog(client, runner);
                dialog.resize(fitSettingsDialogSize(
                    dialog, mikai::probes::windowSize(QSize(760, 780))));
                if (auto* tabs = dialog.findChild<QTabWidget*>()) tabs->setCurrentIndex(0);
                dialog.show();
                QApplication::processEvents();
                const auto englishSurface = languageSurface(dialog);
                auto* language =
                    dialog.findChild<QComboBox*>(QStringLiteral("uiLanguageCombo"));
                const int ukrainian =
                    language != nullptr ? language->findData(QStringLiteral("uk")) : -1;
                if (language != nullptr && ukrainian >= 0) {
                    language->setCurrentIndex(ukrainian);
                    QApplication::processEvents();
                }
                check(mikai::app::activeInterfaceLanguage() == QStringLiteral("uk"),
                      "Escape is tested from the live Ukrainian preview");
                dialog.activateWindow();
                dialog.setFocus();
                QTest::keyClick(&dialog, Qt::Key_Escape);
                QApplication::processEvents();
                check(dialog.result() == static_cast<int>(QDialog::Rejected) &&
                          mikai::app::activeInterfaceLanguage() == QStringLiteral("en"),
                      "Escape restores the opening live language");
                check(languageSurface(dialog) == englishSurface &&
                          storedLanguage() == QStringLiteral("en"),
                      "Escape restores the visible surface and leaves the preference alone");
            }

            {
                SettingsDialog dialog(client, runner);
                dialog.resize(fitSettingsDialogSize(
                    dialog, mikai::probes::windowSize(QSize(760, 780))));
                if (auto* tabs = dialog.findChild<QTabWidget*>()) tabs->setCurrentIndex(0);
                dialog.show();
                QApplication::processEvents();
                const auto englishSurface = languageSurface(dialog);
                auto* language =
                    dialog.findChild<QComboBox*>(QStringLiteral("uiLanguageCombo"));
                const int ukrainian =
                    language != nullptr ? language->findData(QStringLiteral("uk")) : -1;
                if (language != nullptr && ukrainian >= 0) {
                    language->setCurrentIndex(ukrainian);
                    QApplication::processEvents();
                }
                check(mikai::app::activeInterfaceLanguage() == QStringLiteral("uk"),
                      "the window-close round starts from the live Ukrainian preview");
                dialog.close();
                QApplication::processEvents();
                check(dialog.result() == static_cast<int>(QDialog::Rejected) &&
                          mikai::app::activeInterfaceLanguage() == QStringLiteral("en"),
                      "closing Settings restores the opening live language");
                check(languageSurface(dialog) == englishSurface &&
                          storedLanguage() == QStringLiteral("en"),
                      "closing Settings restores the visible surface and leaves the preference alone");
            }

            {
                SettingsDialog dialog(client, runner);
                dialog.resize(fitSettingsDialogSize(
                    dialog, mikai::probes::windowSize(QSize(760, 780))));
                if (auto* tabs = dialog.findChild<QTabWidget*>()) tabs->setCurrentIndex(0);
                dialog.show();
                QApplication::processEvents();
                auto* language =
                    dialog.findChild<QComboBox*>(QStringLiteral("uiLanguageCombo"));
                const int ukrainian =
                    language != nullptr ? language->findData(QStringLiteral("uk")) : -1;
                if (language != nullptr && ukrainian >= 0) {
                    language->setCurrentIndex(ukrainian);
                    QApplication::processEvents();
                }
                QPushButton* save =
                    acceptButton(dialog.findChild<QDialogButtonBox*>());
                check(save != nullptr, "Save is visible for the preview round");
                if (save != nullptr) save->click();
                QApplication::processEvents();
                check(dialog.result() == static_cast<int>(QDialog::Accepted) &&
                          mikai::app::activeInterfaceLanguage() == QStringLiteral("uk"),
                      "Save commits the previewed language and accepts the dialog");
                check(storedLanguage() == QStringLiteral("uk"),
                      "Save is the path that writes the language preference");
                SettingsDialog reopened(client, runner);
                reopened.resize(fitSettingsDialogSize(
                    reopened, mikai::probes::windowSize(QSize(760, 780))));
                if (auto* tabs = reopened.findChild<QTabWidget*>())
                    tabs->setCurrentIndex(0);
                reopened.show();
                QApplication::processEvents();
                const QString ukrainianProcessing = QCoreApplication::translate(
                    "mikai::app::SettingsDialog", "Processing");
                const QString ukrainianSave = QCoreApplication::translate(
                    "mikai::app::SettingsDialog", "Save");
                const auto committedSurface = languageSurface(reopened);
                check(committedSurface.first == ukrainianProcessing &&
                          committedSurface.first != englishProcessing &&
                          committedSurface.second == ukrainianSave,
                      "a reopened Settings shows the committed Ukrainian surface");
                reopened.reject();
                QApplication::processEvents();
            }
            check(mikai::app::applyInterfaceLanguage(QStringLiteral("en")),
                  "the preview section leaves English installed");
            {
                QSettings settings;
                settings.setValue(QStringLiteral("ui/language"), QStringLiteral("en"));
                settings.sync();
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
    exerciseLiveModelDiscovery(client, runner);

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
    screenshots.append(QStringLiteral("native-live-model-discovery.png"));
    screenshots.append(QStringLiteral("native-live-benchmark.png"));
    screenshots.append(QStringLiteral("native-live-player-menu.png"));
    screenshots.append(QStringLiteral("native-live-compare.png"));
    screenshots.append(QStringLiteral("native-live-missing-model.png"));
    screenshots.append(QStringLiteral("native-live-player-missing.png"));
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
