
#include <QAbstractButton>
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
#include <QFontInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHash>
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
#include <QPalette>
#include <QPixmap>
#include <QProcess>
#include <QProgressBar>
#include <QRegion>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSaveFile>
#include <QStackedWidget>
#include <QSettings>
#include <QtCore/QSet>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyleOptionFrame>
#include <QToolButton>
#include <QThread>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QWindow>
#include <QTest>
#include <QQuickItem>
#include "ProbeIsolation.hpp"
#include "ProbeUi.hpp"
#include "IdentityOnnxFixture.hpp"
#include "VideoScalingLiveProbe.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <vector>
#include <cstdio>

#include "I18n.hpp"
#include "backend/MikaiClient.hpp"
#include "theme/Metrics.hpp"
#include "dialogs/SettingsDialog.hpp"
#include "dialogs/AnimeJanaiManagerPanel.hpp"
#include "dialogs/ComponentStoreTab.hpp"
#include "widgets/SwatchButton.hpp"
#include "widgets/ToggleSwitch.hpp"
#include "player/compare/CompareCanvas.hpp"
#include "player/compare/CompareWindow.hpp"
#include "player/core/VideoSession.hpp"
#include "player/quick/PlayerController.hpp"
#include "player/quick/PlayerMenuModel.hpp"
#include "player/quick/PlayerWindow.hpp"
#include "native/bundle/NativePrefs.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "native/components/NativeGpuProbe.hpp"
#include "native/models/NativeRife.hpp"
#include "native/models/NativeSpatialAssets.hpp"
#include "native/models/NativeUpscalers.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"
#include "native/components/NativeComponentProvisioning.hpp"
#include "state/NativeComponentProvisioner.hpp"
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
bool quickPlayerSceneReady(mplayer::quick::PlayerController& controller) {
    return waitForUi([&controller] {
        auto* window = controller.window();
        return window != nullptr && window->status() == QQuickView::Ready &&
               window->videoItem() != nullptr;
    }, 5000);
}
QQuickItem* quickMenuPanel(QQuickItem* root, const QString& menuId) {
    if (root == nullptr) return nullptr;
    for (QQuickItem* item : root->findChildren<QQuickItem*>()) {
        if (item->property("menuId").toString() == menuId &&
            item->property("menuModel").isValid() && item->isVisible()) {
            return item;
        }
    }
    return nullptr;
}

bool openQuickMenuFromScene(mplayer::quick::PlayerController& controller,
                            const QString& buttonName, const QString& menuId) {
    auto* window = controller.window();
    auto* root = window != nullptr ? qobject_cast<QQuickItem*>(window->rootObject()) : nullptr;
    if (window == nullptr || root == nullptr) return false;
    auto* button = root->findChild<QQuickItem*>(buttonName);
    if (button == nullptr || !button->isVisible() || !button->isEnabled()) return false;
    const QPointF sceneCenter =
        button->mapToScene(QPointF(button->width() / 2.0, button->height() / 2.0));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      QPoint(qRound(sceneCenter.x()), qRound(sceneCenter.y())));
    return waitForUi([&] {
        auto* panel = quickMenuPanel(root, menuId);
        return controller.menuStackVisible() && panel != nullptr &&
               panel->property("menuModel").value<QObject*>() ==
                   controller.menuModel(menuId);
    }, 2000);
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

/// The navigation entry a Settings page id owns. `pageIds()` is the public,
/// stable order the entries were built in, so the entry for an id is found
/// without depending on a caption or a widget type the page happens to use.
QPushButton* navEntryFor(SettingsDialog& dialog, const QString& id) {
    const QStringList ids = dialog.pageIds();
    const int index = ids.indexOf(id);
    if (index < 0) return nullptr;
    const QList<QPushButton*> entries =
        dialog.findChildren<QPushButton*>(QStringLiteral("settingsNavItem"));
    return index < entries.size() ? entries.at(index) : nullptr;
}

QKeySequenceEdit* presetField(SettingsDialog& dialog) {
    const QStringList parts =
        SettingsDialog::tr("Shortcut for %1").split(QStringLiteral("%1"));
    if (parts.size() != 2) return nullptr;
    for (QKeySequenceEdit* field : dialog.findChildren<QKeySequenceEdit*>()) {
        const QString name = field->accessibleName();
        if (name.startsWith(parts.at(0)) && name.endsWith(parts.at(1)) &&
            name.size() > parts.at(0).size() + parts.at(1).size()) {
            return field;
        }
    }
    return nullptr;
}

bool showShortcutField(SettingsDialog& dialog, QKeySequenceEdit* field) {
    if (field == nullptr) return false;
    const bool navigated = dialog.showPage(QStringLiteral("shortcuts"));
    dialog.show();
    QApplication::processEvents();
    // The current page is laid out only once it is on screen; settle it before
    // the field's geometry is measured.
    if (auto* stack = dialog.findChild<QStackedWidget*>(QStringLiteral("settingsStack"))) {
        QWidget* page = stack->currentWidget();
        if (page != nullptr && page->layout() != nullptr) page->layout()->activate();
    }
    mikai::probes::revealWidget(field);
    QApplication::processEvents();
    return navigated && dialog.currentPageId() == QStringLiteral("shortcuts") &&
           field->isVisibleTo(&dialog);
}

void shortcutDiagnostics(SettingsDialog& dialog, QKeySequenceEdit& field) {
    QLineEdit* input = field.findChild<QLineEdit*>();
    if (input == nullptr) return;
    QStyleOptionFrame frame;
    frame.initFrom(input);
    const QRect contents =
        input->style()->subElementRect(QStyle::SE_LineEditContents, &frame, input);
    QToolButton* clear = clearActionFor(*input);
    const QFontMetrics metrics(input->font());
    const QString binding = input->text();
    const QString pageId = dialog.currentPageId();
    QPushButton* entry = navEntryFor(dialog, pageId);
    const QString pageCaption =
        entry != nullptr ? entry->text() : QStringLiteral("<no navigation entry>");
    std::fprintf(stderr,
                 "[native_settings_probe] preset shortcut name=%s page=%s caption=%s visible=%d field=%d,%d %dx%d line-edit=%d,%d %dx%d contents=%d,%d %dx%d clear=%s rect=%d,%d %dx%d font=%s text-width=%d capture=native-settings-preset-shortcut-failure.png\n",
                 qPrintable(field.accessibleName()), qPrintable(pageId),
                 qPrintable(pageCaption),
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

/// True when two spellings name the same catalogue file.
bool sameCataloguePath(const QString& left, const QString& right) {
    if (left.isEmpty() || right.isEmpty()) return false;
    if (QDir::cleanPath(left) == QDir::cleanPath(right)) return true;
    const QString leftCanonical = QFileInfo(left).canonicalFilePath();
    const QString rightCanonical = QFileInfo(right).canonicalFilePath();
    return !leftCanonical.isEmpty() &&
           leftCanonical.compare(rightCanonical, Qt::CaseInsensitive) == 0;
}

/// The manager publishes an asynchronous import's progress and its verdict in
/// its own status banner, so that line - and not a clock - is what tells the
/// probe whether a click is still running, was refused, or is done.
QLabel* managerStatusLabel(mikai::app::AnimeJanaiManagerPanel& panel) {
    return panel.findChild<QLabel*>(QStringLiteral("status"));
}

QString managerStatusText(mikai::app::AnimeJanaiManagerPanel& panel) {
    QLabel* label = managerStatusLabel(panel);
    return label != nullptr ? label->text().trimmed() : QString();
}

/// Waits for a model operation to reach its public terminal state.
///
/// The manager has no busy or sensitive control to synchronize on: its model
/// table, Import and Remove actions stay enabled for the whole worker run, and
/// the only in-flight statement is the banner's wording. So the wait is bounded
/// by a hang guard instead of an early budget: a slow worker is never called a
/// refusal, and a genuine failure costs the guard once. The caller's detail
/// still quotes the banner, but only as the failure's cause, never as the
/// signal that the worker ended.
bool waitForTerminalModelState(const auto& settled, int hangGuardMs = 60000) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < hangGuardMs) {
        QApplication::processEvents();
        if (settled()) return true;
        QThread::msleep(10);
    }
    QApplication::processEvents();
    return settled();
}

/// Whether the native registry carries @p id, read straight from the public
/// inventory the manager's own table is built from.
bool registeredModelHasId(const QString& id) {
    if (id.isEmpty()) return false;
    for (const auto& model : mplayer::native::listCustomAnimeJanaiModels()) {
        if (model.id.compare(id, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

/// What the manager itself says about the import of @p path, so a refused or
/// still-running registration is reported with the app's own words, the row it
/// acted on, and whether the underlying registry was written at all, instead of
/// only the verdict the probe expected.
QString importOutcomeDetail(mikai::app::AnimeJanaiManagerPanel& panel, QTableWidget* table,
                            const QString& path) {
    const CatalogueRowView row = catalogueRowForPath(table, path);
    const QString id = row.row >= 0 && !row.id.isEmpty() ? row.id
                                                         : QFileInfo(path).completeBaseName();
    return QStringLiteral("manager status '%1'; row %2; registry %3")
        .arg(managerStatusText(panel),
             row.row < 0
                 ? QStringLiteral("absent")
                 : QStringLiteral("origin %1, state %2, candidate %3, id %4")
                       .arg(row.origin, row.state,
                            row.candidate ? QStringLiteral("yes") : QStringLiteral("no"),
                            row.id),
             registeredModelHasId(id) ? QStringLiteral("holds %1").arg(id)
                                      : QStringLiteral("does not hold %1").arg(id));
}

/// Clicks the manager's model action only when the row the manager would act on
/// really is @p expectedPath as an unregistered candidate. With any other
/// selection the same button opens a modal file picker, which this headless
/// probe cannot drive, so a failed precondition would hang the run instead of
/// reporting it - and a row index that a catalogue refresh has moved under the
/// probe would silently import a different model.
bool clickImportForCandidate(QTableWidget* table, QPushButton* button,
                             const QString& expectedPath, const char* failedMessage) {
    const CatalogueRowView selected =
        catalogueRowAt(table, table != nullptr ? table->currentRow() : -1);
    if (button == nullptr || selected.row < 0 || !selected.candidate ||
        !sameCataloguePath(selected.path, expectedPath)) {
        check(false, failedMessage,
              QStringLiteral("selected row %1 (%2, %3) is not the expected candidate %4")
                  .arg(selected.row)
                  .arg(selected.origin.isEmpty() ? QStringLiteral("none") : selected.origin,
                       selected.state.isEmpty() ? QStringLiteral("none") : selected.state,
                       expectedPath));
        return false;
    }
    button->click();
    return true;
}

/// What the manager says about a removal, and the row and registry it left, so
/// a refusal is reported with the app's own words instead of only the verdict
/// the probe expected.
QString removeOutcomeDetail(mikai::app::AnimeJanaiManagerPanel& panel, QTableWidget* table,
                            const QString& id) {
    const CatalogueRowView row = catalogueRowAt(table, customModelRow(table, id));
    return QStringLiteral("manager status '%1'; row %2; registry %3")
        .arg(managerStatusText(panel),
             row.row < 0
                 ? QStringLiteral("absent")
                 : QStringLiteral("origin %1, state %2, candidate %3, id %4, path %5")
                       .arg(row.origin, row.state,
                            row.candidate ? QStringLiteral("yes") : QStringLiteral("no"),
                            row.id, row.path),
             registeredModelHasId(id) ? QStringLiteral("holds %1").arg(id)
                                      : QStringLiteral("does not hold %1").arg(id));
}

/// Selects the row the manager would act on and proves it is the intended
/// registration before clicking. The action reads `currentRow()`, so the
/// selection uses the same cell operation the consumer reads and the row's id
/// and provenance are checked after that selection, never a stale index.
bool clickRemoveForRow(QTableWidget* table, QPushButton* button, const QString& id,
                       const QString& expectedOrigin, QString* problemOut) {
    const auto problem = [problemOut](const QString& text) {
        if (problemOut != nullptr) *problemOut = text;
        return false;
    };
    if (table == nullptr || button == nullptr) return problem(QStringLiteral("action missing"));
    const int row = customModelRow(table, id);
    if (row < 0) return problem(QStringLiteral("row for '%1' is absent").arg(id));
    table->setCurrentCell(row, 0);
    QApplication::processEvents();
    const CatalogueRowView selected = catalogueRowAt(table, table->currentRow());
    if (selected.id != id) {
        return problem(QStringLiteral("selected row carries id '%1', not '%2'")
                           .arg(selected.id.isEmpty() ? QStringLiteral("(none)") : selected.id, id));
    }
    if (selected.origin != expectedOrigin) {
        return problem(QStringLiteral("selected row origin is '%1', not '%2'")
                           .arg(selected.origin.isEmpty() ? QStringLiteral("(none)")
                                                          : selected.origin,
                                expectedOrigin));
    }
    if (selected.state != QLatin1String("registered")) {
        return problem(QStringLiteral("selected row state is '%1', not registered")
                           .arg(selected.state));
    }
    if (!button->isEnabled()) return problem(QStringLiteral("the removal action is disabled"));
    button->click();
    QApplication::processEvents();
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

mplayer::quick::PlayerMenuModel* enhancementLevelMenu(
    mplayer::quick::PlayerController& player, const QString& level) {
    const QStringList roots{QStringLiteral("enhancements"), QStringLiteral("picture"),
                            QStringLiteral("settings"), QStringLiteral("remedies")};
    QSet<QString> visited;
    const auto find = [&player, &level, &visited](const auto& self, const QString& menuId,
                                                  mplayer::quick::PlayerMenuModel* model)
        -> mplayer::quick::PlayerMenuModel* {
        if (model == nullptr || visited.contains(menuId)) return nullptr;
        visited.insert(menuId);
        if (menuId == level) return model;
        for (int row = 0; row < model->rowCount(); ++row) {
            const auto entry = model->row(row);
            if (entry.action != mplayer::quick::PlayerMenuModel::Action::OpenMenu) continue;
            auto* child = player.submenuModel(menuId, row);
            if (entry.value == level) return child;
            if (auto* nested = self(self, entry.value, child)) return nested;
        }
        return nullptr;
    };
    for (const QString& root : roots) {
        if (auto* model = find(find, root, player.menuModel(root))) return model;
    }
    return nullptr;
}

bool menuHasPayload(mplayer::quick::PlayerMenuModel* menu, const QString& payload) {
    if (menu == nullptr) return false;
    const QString value = payload.startsWith(QLatin1String("ajipreset|"))
                              ? QStringLiteral("animejanai|") +
                                    payload.mid(QStringLiteral("ajipreset|").size())
                              : payload.startsWith(QLatin1String("level|"))
                                    ? payload.mid(QStringLiteral("level|").size())
                                    : payload;
    for (int index = 0; index < menu->rowCount(); ++index) {
        const auto row = menu->row(index);
        if (row.value == value || row.id == value) return true;
    }
    return false;
}

mplayer::quick::PlayerMenuModel::Row menuEntryForPayload(
    mplayer::quick::PlayerMenuModel* menu, const QString& payload) {
    if (menu != nullptr) {
        const QString value = payload.startsWith(QLatin1String("ajipreset|"))
                                  ? QStringLiteral("animejanai|") +
                                        payload.mid(QStringLiteral("ajipreset|").size())
                                  : payload;
        for (int index = 0; index < menu->rowCount(); ++index) {
            const auto row = menu->row(index);
            if (row.value == value || row.id == value) return row;
        }
    }
    return {};
}
bool clickMenuPayload(mplayer::quick::PlayerController& controller,
                      const QString& menuId, const QString& payload) {
    auto* menu = controller.menuModel(menuId);
    if (menu == nullptr) return false;
    const QString value = payload.startsWith(QLatin1String("level|"))
                              ? payload.mid(QStringLiteral("level|").size())
                              : payload.startsWith(QLatin1String("ajipreset|"))
                                    ? QStringLiteral("animejanai|") +
                                          payload.mid(QStringLiteral("ajipreset|").size())
                                    : payload;
    for (int index = 0; index < menu->rowCount(); ++index) {
        const auto row = menu->row(index);
        if (row.value != value && row.id != value) continue;
        if (!row.enabled || row.heading) return false;
        controller.activateMenuRow(menuId, index);
        return true;
    }
    return false;
}
bool compareMenuHasCandidate(mplayer::shell::CompareWindow& comparison,
                             const QString& candidateId, bool enabled) {
    auto* canvas = comparison.findChild<mplayer::shell::CompareCanvas*>(
        QStringLiteral("compareCanvas"));
    if (canvas == nullptr) return false;
    for (const QString& badgeName :
         {QStringLiteral("compareBadgeA"), QStringLiteral("compareBadgeB")}) {
        auto* badge = canvas->findChild<QToolButton*>(badgeName);
        QMenu* root = badge != nullptr ? badge->menu() : nullptr;
        if (root == nullptr) continue;
        const auto findLeaf = [&candidateId, enabled](const auto& self,
                                                       const QMenu* menu) -> bool {
            if (menu == nullptr) return false;
            for (QAction* action : menu->actions()) {
                if (action->menu() != nullptr) {
                    if (self(self, action->menu())) return true;
                } else if (action->data().toString() == candidateId &&
                           action->isEnabled() == enabled) {
                    return true;
                }
            }
            return false;
        };
        if (findLeaf(findLeaf, root)) return true;
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
            modelTable, importModel, externalPath,
            "the external_smoke candidate was selected as the candidate to register");
    }
    const bool externalRegistered = externalRegistrationClicked &&
                                    waitForTerminalModelState([&] {
                                        const int row = customModelRow(
                                            modelTable, QStringLiteral("external_smoke"));
                                        return row >= 0 &&
                                               !modelTable->item(row, 0)
                                                    ->data(Qt::UserRole + 1)
                                                    .toBool();
                                    });
    check(externalRegistered,
          "the external candidate is registered through the real Import action",
          externalRegistered ? QString()
                             : importOutcomeDetail(panel, modelTable, externalPath));
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

    auto* profileName = panel.findChild<QLineEdit*>(QStringLiteral("ajiProfileNameEdit"));
    if (profileName != nullptr) mikai::probes::revealWidget(profileName);
    QApplication::processEvents();
    check(profileName != nullptr && profileName->isVisibleTo(&managerDialog) &&
              profileName->visibleRegion().contains(profileName->rect()),
          "the profile-name header is fully visible in the minimum-size Settings window");
    check(managerDialog.grab().save(QStringLiteral("native-animejanai-manager.png")),
          "the visible manager inside the minimum-size Settings window was captured");
    QPushButton* managerSave = acceptButton(managerDialog.findChild<QDialogButtonBox*>());
    check(managerSave != nullptr, "Settings exposes its real Save action for the manager");
    if (managerSave != nullptr) managerSave->click();
    QApplication::processEvents();
    // Since 25bc266 ("Let Save commit without closing Settings"), Save
    // commits without closing: result stays Rejected, the button shows
    // the "Saved" acknowledgment and is disabled.
    check(managerSave != nullptr &&
              managerDialog.result() == static_cast<int>(QDialog::Rejected) &&
              managerSave->text() ==
                  QCoreApplication::translate("mikai::app::SettingsDialog", "Saved") &&
              !managerSave->isEnabled(),
          "the Settings Save action commits without closing the dialog");

    QFile savedConfig(mplayer::native::AnimeJanaiConfigStore::configFilePath());
    const QByteArray savedBytes =
        savedConfig.open(QIODevice::ReadOnly) ? savedConfig.readAll() : QByteArray{};
    savedConfig.close();
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
    const bool benchmarkNavigated = benchmarkDialog.showPage(QStringLiteral("benchmark"));
    QApplication::processEvents();
    auto* benchmarkTree = benchmarkDialog.findChild<QTreeWidget*>(
        QStringLiteral("benchmarkModelTree"));
    bool benchmarkCustomPresent = false;
    bool benchmarkSlotPresent = false;
    bool benchmarkMixedSlotCategorized = false;
    bool benchmarkCategoriesMatch = false;
    bool benchmarkTemporalStatusAccurate = true;
    bool benchmarkUpscalersPresent = true;
    bool benchmarkUpscalerStatusAccurate = true;
    bool benchmarkSpatialStatusAccurate = true;
    QSet<QString> benchmarkIds;
    const QVector<mplayer::native::UpscalerModel> benchmarkUpscalers =
        mplayer::native::upscalerCatalog();
    const QVector<mplayer::native::StoredSpatialAsset> benchmarkStoredSpatial =
        mplayer::native::storedSpatialAssets();
    const QVector<mplayer::native::AnimeJanaiPreset> benchmarkPresets =
        mplayer::native::animeJanaiPresets();
    const mplayer::native::AnimeJanaiConfig benchmarkConfig =
        mplayer::native::AnimeJanaiConfigStore::load();
    const QVector<mplayer::native::AnimeJanaiSlot> benchmarkDefaultProfiles =
        mplayer::native::AnimeJanaiConfigStore::defaultProfiles(benchmarkConfig);
    const auto benchmarkPresetUsesRife =
        [&benchmarkConfig, &benchmarkDefaultProfiles](
            const mplayer::native::AnimeJanaiPreset& preset) {
            if (preset.rife || preset.id.startsWith(QLatin1String("rife:"))) return true;
            const auto& profiles = preset.id.startsWith(QLatin1String("builtin_slot:"))
                                       ? benchmarkConfig.customSlots
                                       : benchmarkDefaultProfiles;
            for (const mplayer::native::AnimeJanaiSlot& profile : profiles) {
                if (profile.number != preset.slot) continue;
                return std::any_of(
                    profile.chains.cbegin(), profile.chains.cend(),
                    [](const mplayer::native::AnimeJanaiChain& chain) {
                        return chain.rifeEnabled;
                    });
            }
            return false;
        };
    if (benchmarkTree != nullptr) {
        QSet<QString> groupIds;
        for (int groupIndex = 0; groupIndex < benchmarkTree->topLevelItemCount(); ++groupIndex) {
            groupIds.insert(
                benchmarkTree->topLevelItem(groupIndex)->data(0, Qt::UserRole).toString());
        }
        benchmarkCategoriesMatch =
            groupIds.contains(QStringLiteral("processing:restore")) &&
            groupIds.contains(QStringLiteral("processing:upscale")) &&
            groupIds.contains(QStringLiteral("processing:interpolation")) &&
            groupIds.size() == benchmarkTree->topLevelItemCount();
        for (int groupIndex = 0; groupIndex < benchmarkTree->topLevelItemCount(); ++groupIndex) {
            const QTreeWidgetItem* group = benchmarkTree->topLevelItem(groupIndex);
            for (int row = 0; row < group->childCount(); ++row) {
                const QTreeWidgetItem* item = group->child(row);
                const QString id = item->data(0, Qt::UserRole + 1).toString();
                if (!id.isEmpty()) {
                    if (benchmarkIds.contains(id)) benchmarkUpscalersPresent = false;
                    benchmarkIds.insert(id);
                    const QString groupId = group->data(0, Qt::UserRole).toString();
                    QString expectedGroup;
                    if (id.startsWith(QLatin1String("anime4k:"))) {
                        expectedGroup = QStringLiteral("processing:restore");
                    } else if (id.startsWith(QLatin1String("upscaler:")) ||
                               id.startsWith(QLatin1String("spatial:"))) {
                        expectedGroup = QStringLiteral("processing:upscale");
                    } else if (id.startsWith(QLatin1String("rife:"))) {
                        expectedGroup = QStringLiteral("processing:interpolation");
                    } else {
                        const auto preset = std::find_if(
                            benchmarkPresets.cbegin(), benchmarkPresets.cend(),
                            [&id](const mplayer::native::AnimeJanaiPreset& candidate) {
                                return candidate.id == id;
                            });
                        if (preset != benchmarkPresets.cend()) {
                            expectedGroup = benchmarkPresetUsesRife(*preset)
                                                ? QStringLiteral("processing:interpolation")
                                                : QStringLiteral("processing:upscale");
                        }
                    }
                    if (expectedGroup.isEmpty() || groupId != expectedGroup)
                        benchmarkCategoriesMatch = false;
                }
                benchmarkCustomPresent |= id == QStringLiteral("spatial:custom:smoke_model");
                if (id == QStringLiteral("builtin_slot:1")) {
                    benchmarkSlotPresent = true;
                    const auto preset = std::find_if(
                        benchmarkPresets.cbegin(), benchmarkPresets.cend(),
                        [&id](const mplayer::native::AnimeJanaiPreset& candidate) {
                            return candidate.id == id;
                        });
                    benchmarkMixedSlotCategorized =
                        preset != benchmarkPresets.cend() &&
                        group->data(0, Qt::UserRole).toString() ==
                            (benchmarkPresetUsesRife(*preset)
                                 ? QStringLiteral("processing:interpolation")
                                 : QStringLiteral("processing:upscale"));
                }
                if (id.startsWith(QLatin1String("rife:")) ||
                    id.startsWith(QLatin1String("builtin_slot:"))) {
                    const auto preset = std::find_if(
                        benchmarkPresets.cbegin(),
                        benchmarkPresets.cend(),
                        [&id](const mplayer::native::AnimeJanaiPreset& candidate) {
                            return candidate.id == id;
                        });
                    const bool usesRife =
                        preset != benchmarkPresets.cend() &&
                        benchmarkPresetUsesRife(*preset);
                    benchmarkTemporalStatusAccurate &=
                        (id.startsWith(QLatin1String("rife:")) || usesRife
                             ? group->data(0, Qt::UserRole).toString() ==
                                   QStringLiteral("processing:interpolation")
                             : group->data(0, Qt::UserRole).toString() ==
                                   QStringLiteral("processing:upscale")) &&
                        preset != benchmarkPresets.cend() &&
                        item->data(0, Qt::UserRole + 2).toBool() ==
                            (preset != benchmarkPresets.cend() && !preset->supported);
                }
                if (id.startsWith(QLatin1String("upscaler:"))) {
                    benchmarkUpscalersPresent &=
                        group->data(0, Qt::UserRole).toString() ==
                            QStringLiteral("processing:upscale");
                    const auto model = mplayer::native::upscalerForId(id.mid(9));
                    const bool installed =
                        model.has_value() &&
                        !mplayer::native::installedUpscalerPath(*model).isEmpty();
                    benchmarkUpscalerStatusAccurate &=
                        model.has_value() &&
                        item->data(0, Qt::UserRole + 2).toBool() == !installed &&
                        (installed ||
                         item->toolTip(0).contains(QStringLiteral("Not downloaded")));
                }
                if (id.startsWith(QLatin1String("spatial:")) &&
                    !id.startsWith(QLatin1String("spatial:custom:"))) {
                    const QString assetId = id.mid(QStringLiteral("spatial:").size());
                    const mplayer::native::SpatialAsset* asset =
                        mplayer::native::findSpatialAsset(assetId);
                    const auto row = std::find_if(
                        benchmarkStoredSpatial.cbegin(), benchmarkStoredSpatial.cend(),
                        [&assetId](const mplayer::native::StoredSpatialAsset& candidate) {
                            return candidate.asset.id == assetId;
                        });
                    const bool verified =
                        row != benchmarkStoredSpatial.cend() && row->verified;
                    benchmarkSpatialStatusAccurate &=
                        asset != nullptr &&
                        item->data(0, Qt::UserRole + 2).toBool() != verified &&
                        (verified ||
                         (row == benchmarkStoredSpatial.cend()
                              ? item->toolTip(0).contains(QStringLiteral("Not downloaded"))
                              : !item->toolTip(0).isEmpty()));
                }
            }
        }
    }
    check(benchmarkTree != nullptr && benchmarkNavigated &&
              benchmarkDialog.currentPageId() == QStringLiteral("benchmark") &&
              benchmarkCustomPresent && benchmarkSlotPresent && benchmarkCategoriesMatch &&
              benchmarkMixedSlotCategorized && benchmarkTemporalStatusAccurate &&
              benchmarkUpscalersPresent && benchmarkUpscalerStatusAccurate &&
              benchmarkSpatialStatusAccurate &&
              std::all_of(benchmarkUpscalers.cbegin(), benchmarkUpscalers.cend(),
                          [&benchmarkIds](const mplayer::native::UpscalerModel& model) {
                              return benchmarkIds.contains(
                                  QStringLiteral("upscaler:") + model.fileName);
                          }),
          "Benchmark groups Restore, Upscale and Interpolation routes by effect");

    const auto selectedRuntime = mplayer::native::selectStandardRuntime(
        qEnvironmentVariable("MIKAI_MPV_MODULE"));
    check(selectedRuntime.loaded &&
              selectedRuntime.kind == mplayer::native::BundleKind::Standard &&
              !selectedRuntime.restartRequired,
          "the real Standard mpv runtime is selected for player and Compare surfaces");
    std::fprintf(stderr, "[native_settings_probe] runtime=%s loaded=%d reason=%s\n",
                 qPrintable(selectedRuntime.modulePath), selectedRuntime.loaded ? 1 : 0,
                 qPrintable(selectedRuntime.reason));

    mplayer::quick::PlayerController player(QStringLiteral("Native settings probe"),
                                            QString());
    player.showWindow();
    const bool quickSceneReady = quickPlayerSceneReady(player);
    auto* enhancementRoot = player.menuModel(QStringLiteral("enhancements"));
    check(player.isWindowVisible() && quickSceneReady && enhancementRoot != nullptr,
          "the real Quick player scene and enhancement model are available");
    if (enhancementRoot == nullptr) return;
    const bool settingsPanelOpened =
        quickSceneReady && openQuickMenuFromScene(
                               player, QStringLiteral("player.settings.button"),
                               QStringLiteral("settings"));
    check(settingsPanelOpened,
          "the Quick settings control opens the visible QML menu bound to its controller model");
    if (settingsPanelOpened) {
        QTest::keyClick(player.window(), Qt::Key_Escape);
        waitForUi([&player] { return !player.menuStackVisible(); }, 1000);
    }
    bool restoreCategory = false;
    bool upscaleCategory = false;
    bool interpolationCategory = false;
    bool backendCategory = false;
    for (int index = 0; index < enhancementRoot->rowCount(); ++index) {
        const auto row = enhancementRoot->row(index);
        restoreCategory |= row.value == QLatin1String("processing:restore");
        upscaleCategory |= row.value == QLatin1String("processing:upscale");
        interpolationCategory |= row.value == QLatin1String("processing:interpolation");
        backendCategory |= row.value == QLatin1String("animejanai:backend");
    }
    check(restoreCategory && upscaleCategory && interpolationCategory && backendCategory,
          "Restore, Upscale, Interpolation and independent backend menus are available");
    const bool enteredUpscale =
        clickMenuPayload(player, QStringLiteral("enhancements"),
                         QStringLiteral("level|processing:upscale"));
    auto* upscaleMenu =
        enhancementLevelMenu(player, QStringLiteral("processing:upscale"));
    const bool upscaleOpened = enteredUpscale && upscaleMenu != nullptr;
    const bool enteredProfiles =
        upscaleOpened && clickMenuPayload(player, QStringLiteral("processing:upscale"),
                                         QStringLiteral("level|processing:upscale:profiles"));
    auto* profilesMenu =
        enhancementLevelMenu(player, QStringLiteral("processing:upscale:profiles"));
    const bool profilesOpened = enteredProfiles && profilesMenu != nullptr;
    bool customSlotPresent = menuHasPayload(
        profilesMenu, QStringLiteral("ajipreset|builtin_slot:1"));
    QStringList profilePagePayloads;
    if (!customSlotPresent && profilesMenu != nullptr) {
        const QString prefix = QStringLiteral("processing:upscale:profiles:page:");
        for (int index = 0; index < profilesMenu->rowCount(); ++index) {
            const auto row = profilesMenu->row(index);
            if (row.value.startsWith(prefix)) profilePagePayloads.append(row.value);
        }
    }
    for (const QString& pageId : profilePagePayloads) {
        auto* base = enhancementLevelMenu(player, QStringLiteral("processing:upscale:profiles"));
        if (base == nullptr) continue;
        int pageRow = -1;
        for (int index = 0; index < base->rowCount(); ++index) {
            if (base->row(index).value == pageId) {
                pageRow = index;
                break;
            }
        }
        if (pageRow < 0) continue;
        player.activateMenuRow(QStringLiteral("processing:upscale:profiles"), pageRow);
        auto* page = enhancementLevelMenu(player, pageId);
        customSlotPresent |= menuHasPayload(page, QStringLiteral("ajipreset|builtin_slot:1"));
        if (customSlotPresent) {
            profilesMenu = page;
            break;
        }
    }
    check(enteredUpscale && upscaleOpened && enteredProfiles && profilesOpened &&
              profilesMenu != nullptr && customSlotPresent,
          "Upscale profiles expose the custom slot by stable id");

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
    const bool compareSourceOpened = player.session()->open(compareFixturePath);
    check(compareSourceOpened, "the player session opened the static comparison frame");
    const bool compareSourceReady = compareSourceOpened &&
        waitForUi([&] { return player.hasVideo() && player.sourceWidth() > 0; }, 5000);
    check(compareSourceReady, "the static comparison frame reached the player session");
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
        player.openCompare();
        mplayer::shell::CompareWindow* comparison = nullptr;
        const bool compareOpened = waitForUi([&] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* candidate = qobject_cast<mplayer::shell::CompareWindow*>(widget);
                if (candidate != nullptr && candidate->isVisible() &&
                    candidate->frozenSourceEpoch() == player.session()->sourceEpoch()) {
                    comparison = candidate;
                    return true;
                }
            }
            return false;
        }, 10000);
        check(compareOpened, "the controller opened its real Compare window");
        if (comparison != nullptr) comparison->setCurrentCandidateId(shaderCandidate);
        bool hasInstalledFp32Upscaler = false;
        bool installedFp32UpscalerSelectable = false;
        for (const mplayer::native::UpscalerModel& model :
             mplayer::native::upscalerCatalog()) {
            if (model.precision != QLatin1String("fp32") ||
                mplayer::native::installedUpscalerPath(model).isEmpty()) {
                continue;
            }
            hasInstalledFp32Upscaler = true;
            installedFp32UpscalerSelectable |=
                comparison != nullptr &&
                comparison->candidateIds().contains(QStringLiteral("upscaler:") + model.fileName);
        }
        check(!hasInstalledFp32Upscaler || installedFp32UpscalerSelectable,
              "Compare selects an installed fp32 upscaler by its stable catalogue id");
        const bool frameCaptured = comparison != nullptr && waitForUi(
            [&] {
                return !comparison->frozenFrame().isNull() &&
                       !comparison->comparisonPending();
            },
            10000);
        check(frameCaptured, "Compare captured the static frame without an inference backend");
        if (frameCaptured) {
            auto* canvas = comparison->findChild<mplayer::shell::CompareCanvas*>(
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
        if (comparison != nullptr) comparison->close();
    }

    QTableWidget* modelTableForRemoval = customModelTable(*reloadedPanel);
    QPushButton* removeModel = buttonForCaption(
        *reloadedPanel, mikai::app::AnimeJanaiManagerPanel::tr("Remove Selected"));
    check(modelTableForRemoval != nullptr && removeModel != nullptr,
          "the custom model removal action is available");
    QString externalRemovalProblem;
    const bool externalRemovalStarted =
        clickRemoveForRow(modelTableForRemoval, removeModel, QStringLiteral("external_smoke"),
                          QStringLiteral("external"), &externalRemovalProblem);
    // The terminal state is the public synchronisation signal: the registry is
    // committed before either callback or a catalogue refresh can present the
    // borrowed graph as unregistered, so reaching it proves the worker wrote.
    const bool externalRemoved =
        externalRemovalStarted &&
        waitForTerminalModelState([&] {
            // The row must be the borrowed graph itself and present its
            // external provenance as unregistered, not merely carry a flag.
            const CatalogueRowView row = catalogueRowForPath(modelTableForRemoval, externalPath);
            return row.row >= 0 && row.id == QStringLiteral("external_smoke") &&
                   row.origin == QStringLiteral("external") &&
                   row.state == QStringLiteral("unregistered") && row.candidate;
        });
    check(externalRemoved && QFileInfo(externalPath).isFile(),
          "removing an external registration preserves the graph and presents it as unregistered",
          externalRemoved ? QString()
                          : (externalRemovalProblem.isEmpty()
                                 ? removeOutcomeDetail(*reloadedPanel, modelTableForRemoval,
                                                       QStringLiteral("external_smoke"))
                                 : externalRemovalProblem));
    QString ownedRemovalProblem;
    const bool ownedRemovalStarted =
        clickRemoveForRow(modelTableForRemoval, removeModel, QStringLiteral("smoke_model"),
                          QStringLiteral("owned"), &ownedRemovalProblem);
    // Absence is the terminal public state: the managed graph is deleted before
    // the row can disappear, so the inventory below cannot still list it.
    const bool removedInUi =
        ownedRemovalStarted &&
        waitForTerminalModelState([&] {
            return customModelRow(modelTableForRemoval, QStringLiteral("smoke_model")) < 0;
        });
    check(removedInUi, "asynchronous removal cleared the imported model from the manager",
          removedInUi ? QString()
                      : (ownedRemovalProblem.isEmpty()
                             ? removeOutcomeDetail(*reloadedPanel, modelTableForRemoval,
                                                   QStringLiteral("smoke_model"))
                             : ownedRemovalProblem));
    auto removedInventory = std::async(std::launch::async, [] {
        return mplayer::native::listCustomAnimeJanaiModels();
    });
    const bool removalReady = removedInventory.wait_for(std::chrono::seconds(15)) ==
                              std::future_status::ready;
    const auto remainingModels = removalReady
                                     ? removedInventory.get()
                                     : QVector<mplayer::native::CustomAnimeJanaiModel>{};
    QStringList remainingIds;
    for (const auto& model : remainingModels) remainingIds.append(model.id);
    check(removalReady && remainingModels.isEmpty(),
          "background inventory confirms the imported model was removed",
          !removalReady
              ? QStringLiteral("the inventory did not finish within 15 s")
              : (remainingIds.isEmpty()
                     ? QString()
                     : QStringLiteral("still registered: %1")
                           .arg(remainingIds.join(QStringLiteral(", ")))));
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
    const bool benchmarkNavigated = benchmarkDialog.showPage(QStringLiteral("benchmark"));
    benchmarkDialog.resize(managerWindowSize);
    benchmarkDialog.show();
    QApplication::processEvents();
    auto* benchmarkTree =
        benchmarkDialog.findChild<QTreeWidget*>(QStringLiteral("benchmarkModelTree"));
    auto* benchmarkSearch =
        benchmarkDialog.findChild<QLineEdit*>(QStringLiteral("benchmarkModelSearch"));
    check(benchmarkTree != nullptr && benchmarkTree->isVisibleTo(&benchmarkDialog) &&
              benchmarkSearch != nullptr && benchmarkSearch->isVisibleTo(&benchmarkDialog) &&
              benchmarkNavigated &&
              benchmarkDialog.currentPageId() == QStringLiteral("benchmark"),
          "Benchmark's catalogue exists and is visible before the graph is pasted");
    if (benchmarkTree == nullptr || benchmarkSearch == nullptr) return;
    QTreeWidgetItem* stableBenchmarkRow = benchmarkItem(benchmarkTree, QStringLiteral("builtin1001"));
    check(stableBenchmarkRow != nullptr, "Benchmark offers a stable built-in row to preserve");
    if (stableBenchmarkRow != nullptr) stableBenchmarkRow->setCheckState(0, Qt::Checked);
    benchmarkSearch->setText(QStringLiteral("upscale"));
    QApplication::processEvents();
    const QString benchmarkFilter = benchmarkSearch->text();
    const bool stableBenchmarkChecked =
        stableBenchmarkRow != nullptr && stableBenchmarkRow->checkState(0) == Qt::Checked;
    check(benchmarkFilter == QStringLiteral("upscale") && stableBenchmarkChecked,
          "Benchmark holds an Upscale category filter and checked stable model before the change");
    check(benchmarkItem(benchmarkTree, rawPreset) == nullptr,
          "Benchmark does not list an unregistered raw paste");

    const auto selectedRuntime = mplayer::native::selectStandardRuntime(
        qEnvironmentVariable("MIKAI_MPV_MODULE"));
    check(selectedRuntime.loaded &&
              selectedRuntime.kind == mplayer::native::BundleKind::Standard,
          "the real Standard runtime is available for the live player and Compare surfaces");
    mplayer::quick::PlayerController player(QStringLiteral("Native settings probe"),
                                            QString());
    player.showWindow();
    const bool quickSceneReady = quickPlayerSceneReady(player);
    auto* enhancementRoot = player.menuModel(QStringLiteral("enhancements"));
    check(player.isWindowVisible() && quickSceneReady && enhancementRoot != nullptr,
          "the Quick player scene and enhancement model exist before the catalogue changes");
    if (enhancementRoot == nullptr) return;
    const bool enteredUpscale =
        clickMenuPayload(player, QStringLiteral("enhancements"),
                         QStringLiteral("level|processing:upscale"));
    auto* upscaleRootMenu =
        enhancementLevelMenu(player, QStringLiteral("processing:upscale"));
    const bool upscaleOpened = enteredUpscale && upscaleRootMenu != nullptr;
    const bool enteredSpatial =
        upscaleOpened && clickMenuPayload(player, QStringLiteral("processing:upscale"),
                                          QStringLiteral("level|processing:upscale:spatial"));
    auto* spatialMenu =
        enhancementLevelMenu(player, QStringLiteral("processing:upscale:spatial"));
    const bool spatialOpened = enteredSpatial && spatialMenu != nullptr;
    const bool spatialReady = spatialMenu != nullptr && spatialMenu->rowCount() > 0;
    mplayer::quick::PlayerMenuModel* upscaleMenu = nullptr;
    check(enteredUpscale && upscaleOpened && enteredSpatial && spatialOpened && spatialReady &&
              spatialMenu != nullptr,
          "the player's Spatial models menu is open before the graph is pasted");
    if (spatialMenu == nullptr) return;
    check(!menuHasPayload(spatialMenu, QStringLiteral("ajipreset|") + rawPreset),
          "the Spatial models menu does not offer an unregistered raw paste");
    check(player.property("animeJanaiChipText").isValid(),
          "the controller exposes the AnimeJaNai HUD text");

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
    check(player.session()->open(compareFixturePath) &&
              waitForUi([&] { return player.hasVideo() && player.sourceWidth() > 0; }, 5000),
          "the player session holds the static Compare frame before the change");
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
    player.openCompare();
    mplayer::shell::CompareWindow* comparisonWindow = nullptr;
    const bool compareOpened = waitForUi([&] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* candidate = qobject_cast<mplayer::shell::CompareWindow*>(widget);
            if (candidate != nullptr && candidate->isVisible() &&
                candidate->frozenSourceEpoch() == player.session()->sourceEpoch()) {
                comparisonWindow = candidate;
                return true;
            }
        }
        return false;
    }, 10000);
    check(compareOpened, "the controller opened its real Compare window");
    if (!compareOpened || comparisonWindow == nullptr) return;
    mplayer::shell::CompareWindow& comparison = *comparisonWindow;
    comparison.setCurrentCandidateId(shaderCandidates.value(0));
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
          "the open Spatial models menu refuses the unregistered raw paste");
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
        modelTable, importModel, rawPath,
        "the owned raw paste was selected as the candidate to register");
    QApplication::processEvents();
    check(QApplication::activeModalWidget() == nullptr,
          "registering a selected owned candidate needs no copy chooser");
    const bool ownedRegistered = ownedImportClicked &&
                                 waitForTerminalModelState([&] {
                                     const auto row = catalogueRowForPath(modelTable, rawPath);
                                     return row.row >= 0 &&
                                            row.origin == QStringLiteral("owned") &&
                                            row.state == QStringLiteral("registered") &&
                                            !row.candidate;
                                 });
    check(ownedRegistered, "the Import action registered the raw paste in place",
          ownedRegistered ? QString() : importOutcomeDetail(panel, modelTable, rawPath));
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
    const QString customModelsPayload =
        QStringLiteral("level|processing:upscale:spatial:custom");
    const bool customModelsOffered = waitForUi(
        [&] { return menuHasPayload(spatialMenu, customModelsPayload); }, 15000);
    const bool enteredCustomModels =
        customModelsOffered &&
        clickMenuPayload(player, QStringLiteral("processing:upscale:spatial"),
                         customModelsPayload);
    const bool customModelsOpened = waitForUi([&player] {
        auto* menu = enhancementLevelMenu(
            player, QStringLiteral("processing:upscale:spatial:custom"));
        return menu != nullptr && menu->rowCount() > 0;
    }, 3000);
    upscaleMenu = enhancementLevelMenu(
        player, QStringLiteral("processing:upscale:spatial:custom"));
    check(enteredCustomModels && customModelsOpened && upscaleMenu != nullptr,
          "the open Spatial menu exposes registered custom models");
    const bool playerSawOwned = waitForUi(
        [&] {
            const auto entry =
                menuEntryForPayload(upscaleMenu, QStringLiteral("ajipreset|") + rawPreset);
            return !entry.label.isEmpty() && entry.enabled;
        },
        15000);
    check(playerSawOwned,
          "the open player Upscale menu gained the registered graph without reopening");
    const bool benchmarkSawOwned = waitForUi(
        [&] { return benchmarkItem(benchmarkTree, rawPreset) != nullptr; }, 15000);
    check(benchmarkSawOwned, "the open Benchmark catalogue gained the registered graph");
    const bool compareSawOwned = waitForUi(
        [&] {
            return comparison.candidateAvailable(rawPreset) &&
                   compareMenuHasCandidate(comparison, rawPreset, true);
        },
        15000);
    check(compareSawOwned, "the open Compare Upscale menu gained the registered graph");
    QTreeWidgetItem* ownedBenchmarkRow = benchmarkItem(benchmarkTree, rawPreset);
    check(ownedBenchmarkRow != nullptr && ownedBenchmarkRow->checkState(0) == Qt::Unchecked,
          "a newly discovered Benchmark model starts unchecked");
    if (ownedBenchmarkRow != nullptr) ownedBenchmarkRow->setCheckState(0, Qt::Checked);
    QApplication::processEvents();

    const bool selectedRegistered = clickMenuPayload(
        player, QStringLiteral("processing:upscale:spatial:custom"),
        QStringLiteral("ajipreset|") + rawPreset);
    QApplication::processEvents();
    check(selectedRegistered && player.animeJanaiPresetId() == rawPreset,
          "the player applies the registered selection through its live menu");

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
    check(player.window()->grabWindow().save(QStringLiteral("native-live-player-menu.png")),
          "the player's live Upscale menu was captured");
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
    check(!menuHasPayload(upscaleMenu, QStringLiteral("ajipreset|") + externalPreset),
          "the open player menu refuses the unpinned external graph");
    check(!comparison.candidateAvailable(externalPreset) &&
              benchmarkItem(benchmarkTree, externalPreset) == nullptr,
          "Compare and Benchmark refuse the unregistered external graph");
    const auto externalRow = catalogueRowForPath(modelTable, externalPath);
    if (externalRow.row >= 0) modelTable->setCurrentCell(externalRow.row, 0);
    QApplication::processEvents();
    const bool externalImportClicked = clickImportForCandidate(
        modelTable, importModel, externalPath,
        "the borrowed external paste was selected as the candidate to pin");
    // The row precondition above is the click's proof: with no busy control to
    // observe, the terminal registered state - and its full status detail when
    // it never arrives - is what distinguishes a started import from a click
    // that never reached the registration branch.
    const bool externalRegistered =
        externalImportClicked &&
        waitForTerminalModelState([&] {
            const auto row = catalogueRowForPath(modelTable, externalPath);
            return row.row >= 0 && row.origin == QStringLiteral("external") &&
                   row.state == QStringLiteral("registered") && !row.candidate;
        });
    check(externalRegistered, "the Import action pinned the external graph explicitly",
          externalRegistered ? QString()
                             : importOutcomeDetail(panel, modelTable, externalPath));
    check(QFileInfo(externalPath).isFile() && fileBytes(externalPath) == modelProto &&
              !QFileInfo::exists(externalPath + QStringLiteral(".source.json")),
          "borrowed registration never writes beside the user's external graph");
    check(waitForUi([&] { return presetCatalogueHas(externalPreset); }, 5000),
          "the pinned external graph became a usable preset");
    check(waitForUi(
              [&] {
                  return comparison.candidateAvailable(externalPreset) &&
                         compareMenuHasCandidate(comparison, externalPreset, true);
              },
              15000),
          "the open Compare Upscale menu gained the pinned external graph");
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
                          upscaleMenu, QStringLiteral("ajipreset|") + externalPreset);
                      return entry.label.isEmpty();
                  },
                  15000),
              "the open player Upscale menu dropped the replaced external graph");
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
            modelTable, importModel, externalPath,
            "the replaced external model was selected as the candidate to re-pin");
        const bool externalRepinned =
            repinClicked &&
            waitForTerminalModelState([&] {
                const auto row = catalogueRowForPath(modelTable, externalPath);
                return row.row >= 0 && row.origin == QStringLiteral("external") &&
                       row.state == QStringLiteral("registered") && !row.candidate;
            });
        check(externalRepinned,
              "the Import action explicitly re-pinned the atomically replaced graph",
              externalRepinned ? QString()
                               : importOutcomeDetail(panel, modelTable, externalPath));
        check(fileBytes(externalPath) == externalReplacement &&
                  !QFileInfo::exists(externalPath + QStringLiteral(".source.json")),
              "re-pinning keeps the replaced graph in the user's external folder");
        check(waitForUi([&] { return presetCatalogueHas(externalPreset); }, 5000),
              "the re-pinned replacement graph returned to the preset catalogue");
        check(waitForUi(
                  [&] {
                      const auto entry = menuEntryForPayload(
                          upscaleMenu, QStringLiteral("ajipreset|") + externalPreset);
                      return !entry.label.isEmpty() && entry.enabled;
                  },
                  15000),
              "the open player menu offers the re-pinned replacement graph");
        check(waitForUi(
                  [&] {
                      return comparison.candidateAvailable(externalPreset) &&
                             compareMenuHasCandidate(comparison, externalPreset, true);
                  },
                  15000),
              "the open Compare Upscale menu offers the re-pinned replacement graph");
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
    // The retained selection is named by an unavailable stable-id row in the
    // Upscale category. Catalogue refreshes retire old rows with deleteLater(),
    // so reacquire the category and status chip instead of trusting stale handles.
    const bool playerMissingAfterRemoval = waitForUi(
        [&] {
            auto* liveUpscaleMenu = enhancementLevelMenu(
                player, QStringLiteral("processing:upscale:spatial:custom"));
            if (liveUpscaleMenu == nullptr) {
                return false;
            }
            const auto missing = menuEntryForPayload(
                liveUpscaleMenu, QStringLiteral("ajipreset|") + rawPreset);
            return !missing.enabled && missing.selected &&
                   player.animeJanaiStatus().contains(QStringLiteral("Unavailable"),
                                                     Qt::CaseInsensitive);
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
        if (button->objectName() == QLatin1String("benchmarkSelectAllModels") &&
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
    check(player.window()->grabWindow().save(QStringLiteral("native-live-player-missing.png")),
          "the player's explicit unavailable selection was captured");
    managerDialog.resize(managerInitialSize);

    comparison.close();
    benchmarkDialog.reject();
    managerDialog.reject();
    QApplication::processEvents();
}

bool componentCardReachable(QWidget& componentTab, const QString& cardId) {
    QGroupBox* card = nullptr;
    for (QGroupBox* candidate : componentTab.findChildren<QGroupBox*>()) {
        if (candidate->property("componentCardId").toString() == cardId) {
            card = candidate;
            break;
        }
    }
    auto* title = componentTab.findChild<QLabel*>(cardId + QStringLiteral("Title"));
    if (card == nullptr || title == nullptr) return false;

    // Cards continue below the viewport in the compact layout. Scroll the real
    // title into view before testing its visibility; merely being a child of
    // the visible tab is not evidence that the heading is on screen.
    mikai::probes::revealWidget(title);
    QApplication::processEvents();
    if (!card->isVisibleTo(&componentTab) || !title->isVisibleTo(&componentTab)) {
        return false;
    }
    const QRegion visible = title->visibleRegion();
    if (visible.isEmpty() || !visible.contains(title->rect().center())) return false;
    QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(title);
    return accessible != nullptr && !accessible->text(QAccessible::Name).trimmed().isEmpty();
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

void exerciseHdrPresentation(mikai::backend::MikaiClient& client, Runner& runner,
                             QSize requestedSize, const QString& language,
                             const QString& catalog) {
    check(mikai::app::applyInterfaceLanguage(language, catalog),
          "the General HDR presentation language was installed");
    SettingsDialog dialog(client, runner);
    requestedSize = fitSettingsDialogSize(dialog, requestedSize);
    dialog.resize(requestedSize);
    auto* scroll = dialog.findChild<QScrollArea*>(QStringLiteral("nativeGeneralScroll"));
    auto* stack = dialog.findChild<QStackedWidget*>(QStringLiteral("settingsStack"));
    auto* policy = dialog.findChild<QComboBox*>(QStringLiteral("hdrPolicyCombo"));
    auto* overrideCheck =
        dialog.findChild<QCheckBox*>(QStringLiteral("nativeHdrOverrideCheck"));
    QWidget* hdrCard = policy != nullptr ? policy->parentWidget() : nullptr;
    const bool controlsExist = scroll != nullptr && stack != nullptr && policy != nullptr &&
                               overrideCheck != nullptr && hdrCard != nullptr;
    if (!controlsExist) {
        std::fprintf(stderr,
                     "HDR UI lookup: general-scroll=%d settings-stack=%d policy=%d override=%d card=%d\n",
                     scroll != nullptr, stack != nullptr, policy != nullptr, overrideCheck != nullptr,
                     hdrCard != nullptr);
    }
    check(controlsExist,
          "the actual General page exposes both HDR controls and their card");
    if (!controlsExist) {
        dialog.reject();
        return;
    }

    check(dialog.showPage(QStringLiteral("access")),
          "the actual General settings page was selected");
    dialog.show();
    QApplication::processEvents();
    mikai::probes::revealWidget(hdrCard);
    QApplication::processEvents();

    const QRect cardRect(hdrCard->mapTo(scroll->viewport(), QPoint()), hdrCard->size());
    bool captionsFit = true;
    for (QLabel* label : hdrCard->findChildren<QLabel*>()) {
        if (!label->isVisibleTo(&dialog)) continue;
        captionsFit &= label->height() >= label->heightForWidth(label->width());
    }
    const QRect policyRect(policy->mapTo(scroll->viewport(), QPoint()), policy->size());
    const QRect overrideRect(overrideCheck->mapTo(scroll->viewport(), QPoint()),
                             overrideCheck->size());
    QStyleOptionButton overrideOption;
    overrideOption.initFrom(overrideCheck);
    const QRect overrideTextRect = overrideCheck->style()->subElementRect(
        QStyle::SE_CheckBoxContents, &overrideOption, overrideCheck);
    const QRect overrideTextViewportRect(
        overrideCheck->mapTo(scroll->viewport(), overrideTextRect.topLeft()),
        overrideTextRect.size());
    const int overrideTextWidth =
        QFontMetrics(overrideCheck->font()).horizontalAdvance(overrideCheck->text());
    const bool overrideCaptionFits =
        overrideTextRect.width() >= overrideTextWidth &&
        cardRect.contains(overrideTextViewportRect) &&
        scroll->viewport()->rect().contains(overrideTextViewportRect);
    const bool controlsVisible = policy->isVisibleTo(&dialog) &&
                                 overrideCheck->isVisibleTo(&dialog);
    const bool cardFullyVisible = scroll->viewport()->rect().contains(cardRect);
    if (!controlsVisible || !cardFullyVisible || !captionsFit || !overrideCaptionFits ||
        scroll->horizontalScrollBar()->maximum() != 0) {
        const QRect viewportRect = scroll->viewport()->rect();
        const QRect pageRect(scroll->widget()->mapTo(scroll->viewport(), QPoint()),
                             scroll->widget()->size());
        std::fprintf(stderr,
                     "HDR layout geometry: window=%dx%d general-scroll=%d,%d %dx%d viewport=%d,%d %dx%d page=%d,%d %dx%d card=%d,%d %dx%d policy=%d,%d %dx%d override=%d,%d %dx%d caption=%d,%d %dx%d text-width=%d caption-fits=%d hscroll=%d controls-visible=%d captions-fit=%d\n",
                     dialog.width(), dialog.height(), scroll->x(), scroll->y(),
                     scroll->width(), scroll->height(), viewportRect.x(), viewportRect.y(),
                     viewportRect.width(), viewportRect.height(), pageRect.x(), pageRect.y(),
                     pageRect.width(), pageRect.height(), cardRect.x(), cardRect.y(),
                     cardRect.width(), cardRect.height(), policyRect.x(), policyRect.y(),
                     policyRect.width(), policyRect.height(), overrideRect.x(), overrideRect.y(),
                     overrideRect.width(), overrideRect.height(),
                     overrideTextViewportRect.x(), overrideTextViewportRect.y(),
                     overrideTextViewportRect.width(), overrideTextViewportRect.height(),
                     overrideTextWidth, overrideCaptionFits,
                     scroll->horizontalScrollBar()->maximum(), controlsVisible, captionsFit);
        for (QLabel* label : hdrCard->findChildren<QLabel*>()) {
            if (!label->isVisibleTo(&dialog)) continue;
            const QRect labelRect(label->mapTo(scroll->viewport(), QPoint()), label->size());
            std::fprintf(stderr, "HDR label geometry: text=%s rect=%d,%d %dx%d viewport-contained=%d hfw=%d\n",
                         qPrintable(label->text()), labelRect.x(), labelRect.y(),
                         labelRect.width(), labelRect.height(),
                         viewportRect.contains(labelRect), label->heightForWidth(label->width()));
        }
    }
    check(dialog.currentPageId() == QStringLiteral("access") &&
              stack->currentWidget() == scroll && scroll->isVisibleTo(&dialog),
          "the actual General page is selected in the settings stack and its scroll area is visible");
    check(overrideCaptionFits,
          "the Native HDR override caption fits inside the visible card and viewport");
    check(controlsVisible && cardFullyVisible && captionsFit &&
              scroll->horizontalScrollBar()->maximum() == 0,
          "the complete HDR card, labels, and controls fit the real viewport without horizontal clipping");
    check(mikai::probes::dialogActionsFit(dialog, requestedSize),
          "Save and Cancel remain accessible while the HDR card is in view");

    const QString suffix =
        requestedSize.width() <= mikai::app::theme::kSettingsDialogMinWidth
            ? QStringLiteral("compact") : QStringLiteral("wide");
    const QString capture = QStringLiteral("native-general-hdr-%1-%2.png")
                                .arg(language, suffix);
    check(dialog.grab().save(capture),
          "the actual General HDR settings surface was captured");
    dialog.reject();
    QApplication::processEvents();
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

    check(componentCardReachable(*componentTab, QStringLiteral("tensorRtRuntimeCard")),
          "the TensorRT runtime card title is visible and accessible");
    check(componentCardReachable(*componentTab, QStringLiteral("tensorRtKernelsCard")),
          "the TensorRT GPU kernels card title is visible and accessible");
    check(componentCardReachable(*componentTab, QStringLiteral("animeJanaiModelsCard")),
          "the AnimeJaNai upscaling models card title is visible and accessible");
    check(componentCardReachable(*componentTab, QStringLiteral("rifeModelsCard")),
          "the RIFE card title is visible and accessible");
    const QString screenshot =
        QStringLiteral("native-components-%1-%2.png")
            .arg(language, requestedSize.width() <= mikai::app::theme::kSettingsDialogMinWidth
                               ? QStringLiteral("compact")
                               : QStringLiteral("wide"));
    check(dialog.grab().save(screenshot),
          "the Components surface screenshot was saved");
}

QJsonObject exerciseComponentProgress(SettingsDialog& dialog, QSize requestedSize,
                                      const QString& language,
                                      mikai::app::NativeComponentProvisioner& provisioner) {
    using mplayer::native::ComponentProvisionProgress;
    using mplayer::native::ComponentProvisionStage;

    requestedSize = fitSettingsDialogSize(dialog, requestedSize);
    dialog.resize(requestedSize);
    dialog.showComponentsTab();
    dialog.show();
    QApplication::processEvents();

    auto* componentTab = dialog.findChild<mikai::app::ComponentStoreTab*>();
    check(componentTab != nullptr && componentTab->isVisibleTo(&dialog),
          "the real Components page is visible for synthetic progress presentation");
    if (componentTab == nullptr) return {};
    componentTab->setComponentProvisioner(&provisioner);
    componentTab->refresh();
    check(waitForUi([componentTab] {
              return componentTab->inventoryReady() && !componentTab->acquisitionRunning();
          }, 30000),
          "Components inventory is idle before synthetic progress presentation");

    const auto spatialAssets = mplayer::native::spatialAssetCatalog();
    check(!spatialAssets.isEmpty(),
          "the synthetic spatial card identity is available from the pinned catalogue");
    const QString spatialId = spatialAssets.isEmpty() ? QString() : spatialAssets.first().id;

    struct Presentation {
        QString pack;
        QString cardId;
        qint64 received;
        qint64 total;
        /// Negative when the transfer advertises no measured total.
        int downloadedPercent;
        ComponentProvisionStage phaseStage;
        qint64 phaseReceived;
        qint64 phaseTotal;
        /// Negative when the phase must render indeterminate.
        int phasePercent;
        const char* phaseKind;
    };
    QVector<Presentation> presentations{
        {QStringLiteral("trt-runtime"), QStringLiteral("tensorRtRuntimeCard"), 32, 128, 25,
         ComponentProvisionStage::Verifying, 32, 128, -1, "verifying"},
        {QStringLiteral("trt-sm75"), QStringLiteral("tensorRtKernelsCard"), 18, 90, 20,
         ComponentProvisionStage::Extracting, 18, 90, 20, "known-size"},
        // No advertised total: the consumer has no honest fraction, so the only
        // truthful rendering is indeterminate rather than an invented number.
        {QStringLiteral("rife-3.7.0"), QStringLiteral("rifeModelsCard"), 0, 0, -1,
         ComponentProvisionStage::Extracting, 0, 0, -1, "unknown-size"}};
    if (!spatialId.isEmpty()) {
        presentations.append({spatialId, QStringLiteral("animeJanaiModelsCard"), 60, 200, 30,
                              ComponentProvisionStage::Extracting, 60, 200, 30,
                              "known-size"});
    }

    QJsonObject record;
    record.insert(QStringLiteral("languageRequested"), language);
    record.insert(QStringLiteral("languageActive"), mikai::app::activeInterfaceLanguage());
    record.insert(QStringLiteral("width"), requestedSize.width());
    QJsonArray cards;

    const auto progressBar = [componentTab](const Presentation& item) {
        return componentTab->findChild<QProgressBar*>(
            item.cardId + QStringLiteral("Progress"));
    };
    const auto progressLabel = [componentTab](const Presentation& item) {
        return componentTab->findChild<QLabel*>(
            item.cardId + QStringLiteral("ProgressStatus"));
    };
    const auto indeterminate = [](QProgressBar* bar) {
        return bar != nullptr && bar->minimum() == 0 && bar->maximum() == 0;
    };

    // Interleave card events as independent active acquisitions. These are
    // deliberately synthetic UI-only events; no acquisition API is invoked.
    for (const Presentation& item : presentations) {
        const ComponentProvisionProgress event{
            item.pack, item.received, item.total, ComponentProvisionStage::Downloading};
        Q_EMIT provisioner.progress(event);
    }
    QApplication::processEvents();

    QVector<int> downloadedPercents;
    QVector<QString> downloadedTexts;
    for (const Presentation& item : presentations) {
        QProgressBar* bar = progressBar(item);
        QLabel* status = progressLabel(item);
        check(bar != nullptr && bar->isVisibleTo(componentTab),
              "each independently active card exposes its progress bar");
        check(status != nullptr && status->isVisibleTo(componentTab) &&
                  !status->text().isEmpty(),
              "each independently active card exposes a transfer status line");
        if (item.downloadedPercent < 0) {
            check(indeterminate(bar),
                  "an active transfer with no measured total renders indeterminate");
        } else {
            check(bar != nullptr && bar->minimum() == 0 && bar->maximum() == 100 &&
                      bar->value() == item.downloadedPercent,
                  "an active known-size transfer reports its own byte fraction");
        }
        check(bar != nullptr && bar->value() < 100,
              "an active transfer never reports a terminal 100%");
        downloadedPercents.append(indeterminate(bar) ? -1
                                                      : (bar != nullptr ? bar->value() : -1));
        downloadedTexts.append(status != nullptr ? status->text() : QString());
    }

    // A phase change must not reuse the finished transfer's number when the
    // phase is verifying or carries no measured total, and it must not disturb
    // any sibling card's independent fraction.
    for (const Presentation& item : presentations) {
        const ComponentProvisionProgress event{item.pack, item.phaseReceived,
                                              item.phaseTotal, item.phaseStage};
        Q_EMIT provisioner.progress(event);
    }
    QApplication::processEvents();

    bool sawIndeterminate = false;
    bool sawKnownFraction = false;
    bool siblingsUndisturbed = true;
    for (int index = 0; index < presentations.size(); ++index) {
        const Presentation& item = presentations.at(index);
        QProgressBar* bar = progressBar(item);
        QLabel* status = progressLabel(item);
        const bool phaseIndeterminate = indeterminate(bar);
        const QString phaseText = status != nullptr ? status->text() : QString();
        if (item.phasePercent < 0) {
            sawIndeterminate = true;
            check(phaseIndeterminate,
                  "a verifying or unknown-size phase renders indeterminate");
        } else {
            sawKnownFraction = true;
            check(bar != nullptr && bar->minimum() == 0 && bar->maximum() == 100 &&
                      bar->value() == item.phasePercent,
                  "a known-size phase keeps that card's own byte fraction");
            if (downloadedPercents.value(index) != (bar != nullptr ? bar->value() : -1)) {
                siblingsUndisturbed = false;
            }
        }
        check(bar != nullptr && bar->value() < 100,
              "no phase reports a terminal 100% before the store confirms it");
        // The wording is whatever the live language renders: this asserts the
        // line changed, and records the translated text instead of matching an
        // English literal.
        check(!phaseText.isEmpty() && phaseText != downloadedTexts.value(index),
              "the phase line replaces the transferred-byte display");

        QJsonObject card;
        card.insert(QStringLiteral("card"), item.cardId);
        card.insert(QStringLiteral("phaseKind"), QString::fromLatin1(item.phaseKind));
        card.insert(QStringLiteral("downloadingExpectedPercent"),
                    item.downloadedPercent < 0 ? QJsonValue(QJsonValue::Null)
                                               : QJsonValue(item.downloadedPercent));
        card.insert(QStringLiteral("downloadingIndeterminate"),
                    downloadedPercents.value(index) < 0);
        card.insert(QStringLiteral("downloadingValue"), downloadedPercents.value(index));
        card.insert(QStringLiteral("downloadingText"), downloadedTexts.value(index));
        card.insert(QStringLiteral("phaseExpectedPercent"),
                    item.phasePercent < 0 ? QJsonValue(QJsonValue::Null)
                                          : QJsonValue(item.phasePercent));
        card.insert(QStringLiteral("phaseIndeterminate"), phaseIndeterminate);
        card.insert(QStringLiteral("phaseValue"), bar != nullptr ? bar->value() : -1);
        card.insert(QStringLiteral("phaseText"), phaseText);
        cards.append(card);

        QWidget* cardWidget = componentTab->findChild<QWidget*>(
            item.cardId + QStringLiteral("Progress"));
        if (cardWidget != nullptr) mikai::probes::revealWidget(cardWidget);
        check(mikai::probes::dialogActionsFit(dialog, requestedSize),
              "Save and Cancel remain visible in the Components layout");
    }
    check(sawIndeterminate && sawKnownFraction,
          "the synthetic run covers indeterminate and known-size phases");
    check(siblingsUndisturbed,
          "a sibling card's phase never rewrites an independent known fraction");

    dialog.hide();
    QApplication::processEvents();
    dialog.show();
    dialog.showComponentsTab();
    check(waitForUi([&dialog] { return dialog.isVisible(); }, 2000),
          "Settings can hide and reopen with active synthetic presentation");
    for (const Presentation& item : presentations) {
        QProgressBar* bar = progressBar(item);
        if (item.phasePercent < 0) {
            check(indeterminate(bar),
                  "an indeterminate phase stays indeterminate after hide and reopen");
        } else {
            check(bar != nullptr && bar->minimum() == 0 && bar->maximum() == 100 &&
                      bar->value() == item.phasePercent,
                  "a known byte fraction is retained after hide and reopen");
        }
    }

    const QString suffix =
        requestedSize.width() <= mikai::app::theme::kSettingsDialogMinWidth
            ? QStringLiteral("compact")
            : QStringLiteral("wide");
    const QString screenshot =
        QStringLiteral("native-component-progress-%1-%2.png").arg(language, suffix);
    check(dialog.grab().save(screenshot),
          "synthetic component-progress presentation screenshot was saved");
    record.insert(QStringLiteral("cards"), cards);
    record.insert(QStringLiteral("screenshot"), screenshot);
    dialog.reject();
    QApplication::processEvents();
    return record;
}

// The real spatial acquisition is a live network scenario: the probe drives the
// product's own Apply changes action against an empty owned store, lets the
// real fetch/publish/verify workflow run, and records only what it observes.
// Nothing here injects progress, seeds a catalogue or supplies bytes.
constexpr int kSpatialAcquisitionDeadlineMs = 300000;

struct SpatialAcquisitionEvidence {
    QJsonObject initial;
    QJsonObject terminal;
    QJsonArray transitions;
    QJsonArray assets;
    QStringList screenshots;
    int initialStorePresent = -1;
    int terminalStorePresent = -1;
    bool started = false;
    bool timedOut = false;
    bool finished = false;
    bool terminalVerified = false;
    bool reviewPresented = false;
    bool reviewAccepted = false;
};

struct SpatialSurfaceSnapshot {
    int storePresent = 0;
    QString cardState;
    QString status;
    QString severity;
    QString progressLine;
    int barValue = 0;
    bool barVisible = false;
    bool downloadActive = false;
    bool verifyActive = false;
    bool running = false;

    bool sameSurface(const SpatialSurfaceSnapshot& other) const {
        return storePresent == other.storePresent && cardState == other.cardState &&
               status == other.status && severity == other.severity &&
               progressLine == other.progressLine && barValue == other.barValue &&
               barVisible == other.barVisible && downloadActive == other.downloadActive &&
               verifyActive == other.verifyActive && running == other.running;
    }
};

QString spatialStoredDirectory(const mplayer::native::SpatialAsset& asset) {
    const QString token = mplayer::native::SpatialAssetStore::directoryName(asset.id);
    if (token.isEmpty()) return {};
    return QDir(mplayer::native::spatialAssetRoot()).filePath(token);
}

QString spatialStoredGraphPath(const mplayer::native::SpatialAsset& asset) {
    const QString directory = spatialStoredDirectory(asset);
    if (directory.isEmpty()) return {};
    return QDir(directory).filePath(mplayer::native::SpatialAssetStore::graphFileName());
}

QString spatialStoredManifestPath(const mplayer::native::SpatialAsset& asset) {
    const QString directory = spatialStoredDirectory(asset);
    if (directory.isEmpty()) return {};
    return QDir(directory).filePath(mplayer::native::SpatialAssetStore::manifestFileName());
}

/// Presence only: `QFileInfo::isFile` never hashes, so polling the store during
/// a transfer stays off the GUI thread's expensive path. Digest proof stays with
/// the tab's own worker verification.
int spatialStoredGraphCount() {
    int present = 0;
    for (const mplayer::native::SpatialAsset& asset :
         mplayer::native::spatialAssetCatalog()) {
        const QString graph = spatialStoredGraphPath(asset);
        if (!graph.isEmpty() && QFileInfo(graph).isFile()) ++present;
    }
    return present;
}

SpatialSurfaceSnapshot spatialSurfaceState(mikai::app::ComponentStoreTab& tab) {
    SpatialSurfaceSnapshot state;
    state.storePresent = spatialStoredGraphCount();
    if (QLabel* label =
            tab.findChild<QLabel*>(QStringLiteral("animeJanaiModelsCardState"))) {
        state.cardState = label->text();
    }
    if (QProgressBar* bar =
            tab.findChild<QProgressBar*>(QStringLiteral("animeJanaiModelsCardProgress"))) {
        state.barValue = bar->value();
        state.barVisible = bar->isVisibleTo(&tab);
    }
    if (QLabel* line = tab.findChild<QLabel*>(
            QStringLiteral("animeJanaiModelsCardProgressStatus"))) {
        state.progressLine = line->text();
    }
    if (QLabel* status =
            tab.findChild<QLabel*>(QStringLiteral("nativeComponentStatus"))) {
        state.status = status->text();
        state.severity = status->accessibleDescription();
    }
    state.downloadActive = tab.downloadActive();
    state.verifyActive = tab.verifyActive();
    state.running = tab.acquisitionRunning();
    return state;
}

QJsonObject spatialSurfaceJson(qint64 elapsedMs, const SpatialSurfaceSnapshot& state) {
    QJsonObject object;
    object.insert(QStringLiteral("elapsedMs"), static_cast<double>(elapsedMs));
    object.insert(QStringLiteral("storePresent"), state.storePresent);
    object.insert(QStringLiteral("cardState"), state.cardState);
    object.insert(QStringLiteral("status"), state.status);
    object.insert(QStringLiteral("statusSeverity"), state.severity);
    object.insert(QStringLiteral("progressLine"), state.progressLine);
    object.insert(QStringLiteral("barValue"), state.barValue);
    object.insert(QStringLiteral("barVisible"), state.barVisible);
    object.insert(QStringLiteral("downloadActive"), state.downloadActive);
    object.insert(QStringLiteral("verifyActive"), state.verifyActive);
    object.insert(QStringLiteral("acquisitionRunning"), state.running);
    return object;
}

/// One stored graph read back from its own manifest: the pinned identity, the
/// size on disk and the provenance/licence the store wrote beside it. No digest
/// is recomputed here; the tab's worker verification is the digest proof.
QJsonObject spatialStoredRecord(const mplayer::native::SpatialAsset& asset) {
    QJsonObject record;
    record.insert(QStringLiteral("id"), asset.id);
    record.insert(QStringLiteral("name"), asset.name);
    record.insert(QStringLiteral("sha256"), asset.sha256);
    record.insert(QStringLiteral("bytes"), static_cast<double>(asset.bytes));
    record.insert(QStringLiteral("url"), asset.url);
    record.insert(QStringLiteral("commit"), asset.commit);
    const QString graphPath = spatialStoredGraphPath(asset);
    const QString manifestPath = spatialStoredManifestPath(asset);
    record.insert(QStringLiteral("path"), graphPath);
    record.insert(QStringLiteral("manifestPath"), manifestPath);
    const QFileInfo graph(graphPath);
    record.insert(QStringLiteral("present"), graph.isFile());
    record.insert(QStringLiteral("fileBytes"),
                  static_cast<double>(graph.isFile() ? graph.size() : 0));
    QFile manifestFile(manifestPath);
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        record.insert(QStringLiteral("manifestReadable"), false);
        return record;
    }
    const QJsonDocument manifest = QJsonDocument::fromJson(manifestFile.readAll());
    manifestFile.close();
    if (!manifest.isObject()) {
        record.insert(QStringLiteral("manifestReadable"), false);
        return record;
    }
    const QJsonObject object = manifest.object();
    record.insert(QStringLiteral("manifestReadable"), true);
    record.insert(QStringLiteral("manifestId"), object.value(QStringLiteral("id")).toString());
    record.insert(QStringLiteral("manifestName"),
                  object.value(QStringLiteral("name")).toString());
    record.insert(QStringLiteral("manifestSha256"),
                  object.value(QStringLiteral("sha256")).toString());
    record.insert(QStringLiteral("manifestBytes"),
                  object.value(QStringLiteral("bytes")).toDouble());
    record.insert(QStringLiteral("acquiredAt"),
                  object.value(QStringLiteral("acquiredAt")).toString());
    record.insert(QStringLiteral("creator"),
                  object.value(QStringLiteral("creator")).toString());
    const QJsonObject source = object.value(QStringLiteral("source")).toObject();
    record.insert(QStringLiteral("sourceCommit"),
                  source.value(QStringLiteral("commit")).toString());
    record.insert(QStringLiteral("sourceUrl"), source.value(QStringLiteral("url")).toString());
    record.insert(QStringLiteral("sourcePath"), source.value(QStringLiteral("path")).toString());
    const QJsonObject license = object.value(QStringLiteral("license")).toObject();
    record.insert(QStringLiteral("license"), license.value(QStringLiteral("spdx")).toString());
    return record;
}

void exerciseSpatialAcquisition(SettingsDialog& dialog, QSize requestedSize,
                                SpatialAcquisitionEvidence& evidence) {
    using mplayer::native::SpatialAsset;

    requestedSize = fitSettingsDialogSize(dialog, requestedSize);
    dialog.resize(requestedSize);
    dialog.showComponentsTab();
    dialog.show();
    QApplication::processEvents();

    auto* componentTab = dialog.findChild<mikai::app::ComponentStoreTab*>();
    check(componentTab != nullptr && componentTab->isVisibleTo(&dialog),
          "the real Components page is visible for spatial acquisition");
    if (componentTab == nullptr) return;

    const QVector<SpatialAsset> catalogue = mplayer::native::spatialAssetCatalog();
    check(catalogue.size() == 5,
          "the pinned spatial catalogue carries the five acquisition graphs");
    const QString ownedRoot = mikai::probes::isolation::isolatedRoot();
    const QString assetsRoot = mplayer::native::spatialAssetRoot();
    const QString ownedPrefix =
        ownedRoot.isEmpty() ? QString() : QDir::cleanPath(ownedRoot) + QLatin1Char('/');
    const bool insideOwnedScratch =
        !ownedRoot.isEmpty() &&
        QDir::cleanPath(assetsRoot).startsWith(ownedPrefix, Qt::CaseInsensitive);
    check(insideOwnedScratch,
          "the spatial store resolves inside this probe's owned scratch", assetsRoot);

    evidence.initialStorePresent = spatialStoredGraphCount();
    const QStringList existingDirectories =
        QDir(assetsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    check(evidence.initialStorePresent == 0 && existingDirectories.isEmpty(),
          "the owned spatial store holds nothing before acquisition",
          existingDirectories.join(QStringLiteral(", ")));
    // A store outside the owned scratch is never touched: acquiring into it
    // would be the very mutation this isolation exists to prevent.
    if (!insideOwnedScratch || evidence.initialStorePresent != 0 ||
        !existingDirectories.isEmpty()) {
        return;
    }

    check(waitForUi(
              [componentTab] {
                  return componentTab->inventoryReady() && !componentTab->acquisitionRunning();
              },
              60000),
          "the Components inventory settles on the empty store");

    SpatialSurfaceSnapshot initial = spatialSurfaceState(*componentTab);
    evidence.initial = spatialSurfaceJson(0, initial);
    const QString zeroLabel =
        QCoreApplication::translate("mikai::app::ComponentStoreTab", "not installed (0/%1)")
            .arg(catalogue.size());
    check(initial.storePresent == 0,
          "the store is still empty once the Components page has rendered");
    check(initial.cardState == zeroLabel,
          "the spatial card reads not installed (0/5)", initial.cardState);

    if (QWidget* card =
            componentTab->findChild<QWidget*>(QStringLiteral("animeJanaiModelsCardTitle"))) {
        mikai::probes::revealWidget(card);
    }
    const QString initialShot = QStringLiteral("native-spatial-acquisition-initial.png");
    check(dialog.grab().save(initialShot),
          "the empty Components surface screenshot was saved");
    evidence.screenshots.append(initialShot);

    // Select exactly the spatial card through the real checkboxes. Other
    // recommendations are cleared first so Apply cannot start a backend, RIFE
    // or kernel unit.
    QCheckBox* spatialCheck =
        componentTab->findChild<QCheckBox*>(QStringLiteral("animeJanaiModelsCardCheck"));
    QCheckBox* runtimeCheck =
        componentTab->findChild<QCheckBox*>(QStringLiteral("tensorRtRuntimeCardCheck"));
    QCheckBox* kernelsCheck =
        componentTab->findChild<QCheckBox*>(QStringLiteral("tensorRtKernelsCardCheck"));
    QCheckBox* rifeCheck =
        componentTab->findChild<QCheckBox*>(QStringLiteral("rifeModelsCardCheck"));
    check(spatialCheck != nullptr, "the spatial model card exposes its selector");
    if (spatialCheck == nullptr) return;
    for (QCheckBox* box : {runtimeCheck, kernelsCheck, rifeCheck}) {
        if (box != nullptr && box->isChecked()) box->click();
        if (box != nullptr) {
            check(!box->isChecked(), "a non-spatial card is left unselected",
                  box->objectName());
        }
    }
    if (!spatialCheck->isChecked()) spatialCheck->click();
    QApplication::processEvents();
    check(spatialCheck->isChecked(), "the spatial model card is selected");
    if (QWidget* card = spatialCheck->parentWidget()) {
        check(card->property("selected").toBool(),
              "only the spatial card renders an active selection");
    }

    SpatialSurfaceSnapshot beforeApply = spatialSurfaceState(*componentTab);
    evidence.initial.insert(QStringLiteral("selectionCardState"), beforeApply.cardState);

    // The review dialog is the tab's own acquisition review; nothing else is
    // ever accepted. It carries no binding consent and appears only when the
    // product's reviewed path asks for it.
    QTimer reviewTimer;
    reviewTimer.setInterval(50);
    QObject::connect(&reviewTimer, &QTimer::timeout, [componentTab, &evidence] {
        for (QWidget* top : QApplication::topLevelWidgets()) {
            auto* box = qobject_cast<QMessageBox*>(top);
            if (box == nullptr || !box->isVisible()) continue;
            if (box->parentWidget() != componentTab) continue;
            if (box->windowTitle() !=
                QCoreApplication::translate("mikai::app::ComponentStoreTab",
                                            "Review installation")) {
                continue;
            }
            evidence.reviewPresented = true;
            if (QAbstractButton* yes = box->button(QMessageBox::Yes)) {
                evidence.reviewAccepted = true;
                yes->click();
            }
        }
    });
    reviewTimer.start();

    QPushButton* apply = accessibleAction(
        *componentTab,
        QCoreApplication::translate("mikai::app::ComponentStoreTab", "Apply changes"));
    check(apply != nullptr && apply->isEnabled(),
          "the real Apply changes action is available for the acquisition");
    if (apply == nullptr) {
        reviewTimer.stop();
        return;
    }

    QElapsedTimer clock;
    clock.start();
    apply->click();
    QApplication::processEvents();

    evidence.started = waitForUi(
        [componentTab] { return componentTab->acquisitionRunning(); }, 20000);
    check(evidence.started, "Apply started the real acquisition workflow");

    SpatialSurfaceSnapshot last;
    bool haveLast = false;
    bool progressCaptured = false;
    QString stableStatus;
    int stablePolls = 0;
    while (evidence.started) {
        QApplication::processEvents();
        const qint64 elapsed = clock.elapsed();
        const SpatialSurfaceSnapshot state = spatialSurfaceState(*componentTab);
        if (!haveLast || !state.sameSurface(last)) {
            evidence.transitions.append(spatialSurfaceJson(elapsed, state));
            last = state;
            haveLast = true;
        }
        if (!progressCaptured && state.storePresent > 0 &&
            state.storePresent < catalogue.size() && state.cardState != initial.cardState) {
            if (QWidget* card = componentTab->findChild<QWidget*>(
                    QStringLiteral("animeJanaiModelsCardProgress"))) {
                mikai::probes::revealWidget(card);
            }
            const QString progressShot =
                QStringLiteral("native-spatial-acquisition-progress.png");
            if (dialog.grab().save(progressShot)) {
                progressCaptured = true;
                evidence.screenshots.append(progressShot);
            }
        }
        if (!state.running && componentTab->inventoryReady()) {
            if (state.status == stableStatus) {
                if (++stablePolls >= 3) {
                    evidence.finished = true;
                    break;
                }
            } else {
                stableStatus = state.status;
                stablePolls = 0;
            }
        }
        if (elapsed >= kSpatialAcquisitionDeadlineMs) {
            evidence.timedOut = true;
            break;
        }
        QThread::msleep(10);
    }
    reviewTimer.stop();

    if (evidence.timedOut) {
        if (QPushButton* cancel = accessibleAction(
                *componentTab,
                QCoreApplication::translate("mikai::app::ComponentStoreTab", "Cancel"));
            cancel != nullptr && cancel->isEnabled()) {
            cancel->click();
        }
        waitForUi([componentTab] { return !componentTab->acquisitionRunning(); }, 60000);
    }

    SpatialSurfaceSnapshot terminal = spatialSurfaceState(*componentTab);
    evidence.terminalStorePresent = terminal.storePresent;
    evidence.terminal = spatialSurfaceJson(clock.elapsed(), terminal);
    check(evidence.finished, "the acquisition reached an idle terminal state");
    check(!evidence.timedOut, "the acquisition finished inside the probe deadline");
    check(!evidence.reviewPresented || evidence.reviewAccepted,
          "a presented acquisition review was accepted, never bypassed");
    check(terminal.storePresent == catalogue.size(),
          "every pinned graph is present in the owned store",
          QString::number(terminal.storePresent) + QLatin1Char('/') +
              QString::number(catalogue.size()));

    const QString installedLabel =
        QCoreApplication::translate("mikai::app::ComponentStoreTab",
                                    "%1/%2 installed; %3/%2 verified")
            .arg(catalogue.size())
            .arg(catalogue.size())
            .arg(catalogue.size());
    check(terminal.cardState == installedLabel,
          "the spatial card reads five installed and five verified", terminal.cardState);

    const QString countsStatus =
        QCoreApplication::translate("mikai::app::ComponentStoreTab",
                                    "Installed: %1/%2 · Verified: %3/%2")
            .arg(catalogue.size())
            .arg(catalogue.size())
            .arg(catalogue.size());
    check(terminal.status == countsStatus,
          "the Components status reports the installed and verified counts",
          terminal.status);
    check(terminal.severity == QLatin1String("success"),
          "the terminal Components status carries the success severity",
          terminal.severity);
    evidence.terminalVerified =
        terminal.storePresent == catalogue.size() && terminal.cardState == installedLabel &&
        terminal.status == countsStatus && terminal.severity == QLatin1String("success");

    if (QWidget* card =
            componentTab->findChild<QWidget*>(QStringLiteral("animeJanaiModelsCardTitle"))) {
        mikai::probes::revealWidget(card);
    }
    const QString terminalShot = QStringLiteral("native-spatial-acquisition-terminal.png");
    check(dialog.grab().save(terminalShot),
          "the completed Components surface screenshot was saved");
    evidence.screenshots.append(terminalShot);

    QStringList provenanceProblems;
    for (const SpatialAsset& asset : catalogue) {
        const QJsonObject record = spatialStoredRecord(asset);
        evidence.assets.append(record);
        const bool present = record.value(QStringLiteral("present")).toBool();
        const bool readable = record.value(QStringLiteral("manifestReadable")).toBool();
        const bool matches =
            present && readable &&
            record.value(QStringLiteral("fileBytes")).toDouble() ==
                static_cast<double>(asset.bytes) &&
            record.value(QStringLiteral("manifestBytes")).toDouble() ==
                static_cast<double>(asset.bytes) &&
            record.value(QStringLiteral("manifestId")).toString() == asset.id &&
            record.value(QStringLiteral("manifestSha256")).toString() == asset.sha256 &&
            record.value(QStringLiteral("sourceCommit")).toString() == asset.commit &&
            record.value(QStringLiteral("sourceUrl")).toString() == asset.url;
        if (!matches) {
            provenanceProblems.append(QStringLiteral("%1: present=%2 readable=%3 bytes=%4/%5")
                                          .arg(asset.name)
                                          .arg(present ? 1 : 0)
                                          .arg(readable ? 1 : 0)
                                          .arg(record.value(QStringLiteral("fileBytes"))
                                                   .toDouble())
                                          .arg(asset.bytes));
        }
    }
    check(provenanceProblems.isEmpty(),
          "each stored graph carries its pinned size, digest and source provenance",
          provenanceProblems.join(QStringLiteral("; ")));
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

void settlePaint(QWidget* widget, int milliseconds) {
    if (widget != nullptr) widget->update();
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    QApplication::processEvents();
}

void keepWindowOnScreen(QWidget& window) {
    QScreen* screen = window.screen();
    if (screen == nullptr) return;
    const QRect available = screen->availableGeometry();
    const QRect frame = window.frameGeometry();
    QPoint shift;
    if (frame.left() < available.left()) shift.setX(available.left() - frame.left());
    if (frame.top() < available.top()) shift.setY(available.top() - frame.top());
    if (frame.right() > available.right()) {
        shift.setX(shift.x() - (frame.right() - available.right()));
    }
    if (frame.bottom() > available.bottom()) {
        shift.setY(shift.y() - (frame.bottom() - available.bottom()));
    }
    if (shift.isNull()) return;
    window.move(window.pos() + shift);
    settlePaint(&window, 120);
}

QJsonObject rectJson(const QRect& rect) {
    QJsonObject object;
    object.insert(QStringLiteral("x"), rect.x());
    object.insert(QStringLiteral("y"), rect.y());
    object.insert(QStringLiteral("width"), rect.width());
    object.insert(QStringLiteral("height"), rect.height());
    return object;
}

struct TextLabelSnapshot {
    QString objectName;
    QString className;
    QString text;
    QString accessibleName;
    QString stylesheet;
    QString family;
    QString resolvedFamily;
    qreal pointSize = 0.0;
    int pixelSize = 0;
    bool bold = false;
    QRect geometry;
    QRect globalBounds;
    bool autoFillBackground = false;
    QColor windowColor;
    QColor baseColor;
    bool translucent = false;
    bool noSystemBackground = false;
    bool opaquePaint = false;
    bool fullyVisible = false;
};

TextLabelSnapshot snapshotTextLabel(QLabel* label) {
    TextLabelSnapshot snapshot;
    if (label == nullptr) return snapshot;
    const QFont font = label->font();
    const QFontInfo info(font);
    snapshot.objectName = label->objectName();
    snapshot.className = QString::fromLatin1(label->metaObject()->className());
    snapshot.text = label->text();
    snapshot.accessibleName = label->accessibleName();
    snapshot.stylesheet = label->styleSheet();
    snapshot.family = font.family();
    snapshot.resolvedFamily = info.family();
    snapshot.pointSize = font.pointSizeF();
    snapshot.pixelSize = font.pixelSize();
    snapshot.bold = font.bold();
    snapshot.geometry = label->geometry();
    snapshot.globalBounds = QRect(label->mapToGlobal(QPoint()), label->size());
    snapshot.autoFillBackground = label->autoFillBackground();
    snapshot.windowColor = label->palette().color(QPalette::Window);
    snapshot.baseColor = label->palette().color(QPalette::Base);
    snapshot.translucent = label->testAttribute(Qt::WA_TranslucentBackground);
    snapshot.noSystemBackground = label->testAttribute(Qt::WA_NoSystemBackground);
    snapshot.opaquePaint = label->testAttribute(Qt::WA_OpaquePaintEvent);
    snapshot.fullyVisible =
        label->isVisible() && label->visibleRegion().contains(label->rect());
    return snapshot;
}

bool labelIsOnTop(QLabel* label, QWidget& window) {
    if (label == nullptr || !label->isVisible()) return false;
    QWidget* hit = QApplication::widgetAt(label->mapToGlobal(label->rect().center()));
    return hit != nullptr && (hit == &window || window.isAncestorOf(hit));
}

template <typename Widget>
QJsonArray widgetGeometryJson(QWidget& root, const QString& kind) {
    QJsonArray array;
    for (Widget* widget : root.findChildren<Widget*>()) {
        QJsonObject object;
        object.insert(QStringLiteral("kind"), kind);
        object.insert(QStringLiteral("objectName"), widget->objectName());
        object.insert(QStringLiteral("className"),
                      QString::fromLatin1(widget->metaObject()->className()));
        object.insert(QStringLiteral("width"), widget->width());
        object.insert(QStringLiteral("height"), widget->height());
        object.insert(QStringLiteral("physicalWidth"),
                      qRound(widget->width() * widget->devicePixelRatioF()));
        object.insert(QStringLiteral("physicalHeight"),
                      qRound(widget->height() * widget->devicePixelRatioF()));
        object.insert(QStringLiteral("sizeHintWidth"), widget->sizeHint().width());
        object.insert(QStringLiteral("sizeHintHeight"), widget->sizeHint().height());
        object.insert(QStringLiteral("minimumWidth"), widget->minimumWidth());
        object.insert(QStringLiteral("minimumHeight"), widget->minimumHeight());
        object.insert(QStringLiteral("maximumWidth"), widget->maximumWidth());
        object.insert(QStringLiteral("maximumHeight"), widget->maximumHeight());
        object.insert(QStringLiteral("devicePixelRatio"), widget->devicePixelRatioF());
        object.insert(QStringLiteral("visible"), widget->isVisible());
        object.insert(QStringLiteral("fullyVisible"),
                      widget->isVisible() &&
                          widget->visibleRegion().contains(widget->rect()));
        object.insert(QStringLiteral("visibleRegion"),
                      rectJson(widget->visibleRegion().boundingRect()));
        array.append(object);
    }
    return array;
}

bool sameTextLabelConfiguration(const TextLabelSnapshot& left,
                                const TextLabelSnapshot& right) {
    return left.text == right.text && left.family == right.family &&
           left.resolvedFamily == right.resolvedFamily &&
           left.pointSize == right.pointSize && left.pixelSize == right.pixelSize &&
           left.bold == right.bold && left.geometry == right.geometry &&
           left.globalBounds == right.globalBounds &&
           left.autoFillBackground == right.autoFillBackground &&
           left.windowColor == right.windowColor && left.baseColor == right.baseColor &&
           left.stylesheet == right.stylesheet &&
           left.translucent == right.translucent &&
           left.noSystemBackground == right.noSystemBackground &&
           left.opaquePaint == right.opaquePaint;
}

QJsonObject textLabelJson(const TextLabelSnapshot& snapshot) {
    QJsonObject object;
    object.insert(QStringLiteral("objectName"), snapshot.objectName);
    object.insert(QStringLiteral("className"), snapshot.className);
    object.insert(QStringLiteral("text"), snapshot.text);
    object.insert(QStringLiteral("accessibleName"), snapshot.accessibleName);
    object.insert(QStringLiteral("fontFamily"), snapshot.family);
    object.insert(QStringLiteral("fontFamilyResolved"), snapshot.resolvedFamily);
    object.insert(QStringLiteral("fontPointSize"), snapshot.pointSize);
    object.insert(QStringLiteral("fontPixelSize"), snapshot.pixelSize);
    object.insert(QStringLiteral("fontBold"), snapshot.bold);
    object.insert(QStringLiteral("logicalBoundsInWindow"), rectJson(snapshot.geometry));
    object.insert(QStringLiteral("globalLogicalBounds"), rectJson(snapshot.globalBounds));
    object.insert(QStringLiteral("autoFillBackground"), snapshot.autoFillBackground);
    object.insert(QStringLiteral("paletteWindowColor"),
                  snapshot.windowColor.name(QColor::HexArgb));
    object.insert(QStringLiteral("paletteBaseColor"),
                  snapshot.baseColor.name(QColor::HexArgb));
    object.insert(QStringLiteral("stylesheetEmpty"), snapshot.stylesheet.isEmpty());
    object.insert(QStringLiteral("translucentAttribute"), snapshot.translucent);
    object.insert(QStringLiteral("noSystemBackgroundAttribute"),
                  snapshot.noSystemBackground);
    object.insert(QStringLiteral("opaquePaintAttribute"), snapshot.opaquePaint);
    object.insert(QStringLiteral("fullyVisible"), snapshot.fullyVisible);
    return object;
}

struct TextCropMetrics {
    int width = 0;
    int height = 0;
    int uniqueColors = 0;
    int minLuminance = 0;
    int maxLuminance = 0;
    QRgb modalColor = 0;
    double inkFraction = 0.0;
    bool valid = false;
};

TextCropMetrics measureTextCrop(const QImage& crop) {
    TextCropMetrics metrics;
    if (crop.isNull() || crop.width() <= 0 || crop.height() <= 0) return metrics;
    QImage image = crop.convertToFormat(QImage::Format_RGB32);
    image.setDevicePixelRatio(1.0);
    metrics.width = image.width();
    metrics.height = image.height();
    QHash<QRgb, int> histogram;
    int minimum = 255;
    int maximum = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            ++histogram[pixel];
            const int luminance = qGray(pixel);
            minimum = qMin(minimum, luminance);
            maximum = qMax(maximum, luminance);
        }
    }
    int modalCount = 0;
    QRgb modalColor = 0;
    for (auto it = histogram.constBegin(); it != histogram.constEnd(); ++it) {
        if (it.value() > modalCount) {
            modalCount = it.value();
            modalColor = it.key();
        }
    }
    const qint64 total = static_cast<qint64>(image.width()) * image.height();
    metrics.uniqueColors = histogram.size();
    metrics.minLuminance = minimum;
    metrics.maxLuminance = maximum;
    metrics.modalColor = modalColor;
    metrics.inkFraction =
        total > 0 ? 1.0 - static_cast<double>(modalCount) / static_cast<double>(total) : 0.0;
    metrics.valid = metrics.uniqueColors >= 3 && metrics.maxLuminance - metrics.minLuminance >= 16 &&
                    metrics.inkFraction > 0.002 && minimum < 64;
    return metrics;
}

QJsonObject metricsJson(const TextCropMetrics& metrics) {
    QJsonObject object;
    object.insert(QStringLiteral("width"), metrics.width);
    object.insert(QStringLiteral("height"), metrics.height);
    object.insert(QStringLiteral("uniqueColors"), metrics.uniqueColors);
    object.insert(QStringLiteral("minLuminance"), metrics.minLuminance);
    object.insert(QStringLiteral("maxLuminance"), metrics.maxLuminance);
    object.insert(QStringLiteral("luminanceRange"), metrics.maxLuminance - metrics.minLuminance);
    object.insert(QStringLiteral("dominantColor"),
                  QColor::fromRgb(metrics.modalColor).name(QColor::HexRgb));
    object.insert(QStringLiteral("inkFraction"), metrics.inkFraction);
    object.insert(QStringLiteral("valid"), metrics.valid);
    return object;
}

struct TextRasterCapture {
    bool available = false;
    QString error;
    QString surfaceFile;
    QString cropFile;
    QRect logicalGlobal;
    QRect physicalCrop;
    QRect physicalSurface;
    QSize screenImageSize;
    qreal devicePixelRatio = 0.0;
    double scaleX = 0.0;
    double scaleY = 0.0;
    QImage crop;
    TextCropMetrics metrics;
};

struct CropDifference {
    bool comparable = false;
    int totalPixels = 0;
    int differingPixels = 0;
    int totalBytes = 0;
    int differingBytes = 0;
    int maxChannelDelta = 0;
    double meanChannelDelta = 0.0;
    double differingPixelPercent = 0.0;
    double differingBytePercent = 0.0;
};

CropDifference compareTextCrops(const QImage& left, const QImage& right) {
    CropDifference difference;
    if (left.isNull() || right.isNull() || left.size() != right.size()) return difference;
    const QImage first = left.convertToFormat(QImage::Format_RGB32);
    const QImage second = right.convertToFormat(QImage::Format_RGB32);
    if (first.size() != second.size()) return difference;
    difference.comparable = true;
    difference.totalPixels = first.width() * first.height();
    difference.totalBytes = difference.totalPixels * 4;
    qint64 channelSum = 0;
    for (int y = 0; y < first.height(); ++y) {
        const auto* leftLine = reinterpret_cast<const QRgb*>(first.constScanLine(y));
        const auto* rightLine = reinterpret_cast<const QRgb*>(second.constScanLine(y));
        for (int x = 0; x < first.width(); ++x) {
            const QRgb leftPixel = leftLine[x];
            const QRgb rightPixel = rightLine[x];
            if (leftPixel == rightPixel) continue;
            ++difference.differingPixels;
            const int channels[3] = {
                qAbs(qRed(leftPixel) - qRed(rightPixel)),
                qAbs(qGreen(leftPixel) - qGreen(rightPixel)),
                qAbs(qBlue(leftPixel) - qBlue(rightPixel)),
            };
            for (int channel = 0; channel < 3; ++channel) {
                const int delta = channels[channel];
                if (delta > difference.maxChannelDelta) difference.maxChannelDelta = delta;
                channelSum += delta;
                if (delta != 0) ++difference.differingBytes;
            }
        }
    }
    if (difference.totalPixels > 0) {
        difference.differingPixelPercent =
            100.0 * difference.differingPixels / difference.totalPixels;
        difference.meanChannelDelta =
            static_cast<double>(channelSum) /
            (static_cast<double>(difference.totalPixels) * 3.0);
    }
    if (difference.totalBytes > 0) {
        difference.differingBytePercent =
            100.0 * difference.differingBytes / difference.totalBytes;
    }
    return difference;
}

QJsonObject differenceJson(const CropDifference& difference) {
    QJsonObject object;
    object.insert(QStringLiteral("comparable"), difference.comparable);
    object.insert(QStringLiteral("totalPixels"), difference.totalPixels);
    object.insert(QStringLiteral("differingPixels"), difference.differingPixels);
    object.insert(QStringLiteral("differingPixelPercent"), difference.differingPixelPercent);
    object.insert(QStringLiteral("totalBytes"), difference.totalBytes);
    object.insert(QStringLiteral("differingBytes"), difference.differingBytes);
    object.insert(QStringLiteral("differingBytePercent"), difference.differingBytePercent);
    object.insert(QStringLiteral("maxChannelDelta"), difference.maxChannelDelta);
    object.insert(QStringLiteral("meanChannelDelta"), difference.meanChannelDelta);
    return object;
}

bool grabNativeSurface(QScreen* screen, QWidget* window, const QString& file, QImage& surface,
                       QString& error) {
    if (screen == nullptr || window == nullptr) {
        error = QStringLiteral("no screen or window");
        return false;
    }
    QPixmap grab = screen->grabWindow(0);
    if (grab.isNull()) {
        error = QStringLiteral("QScreen::grabWindow(0) returned no pixels");
        return false;
    }
    const QImage desktop = grab.toImage();
    const QSize screenLogical = screen->geometry().size();
    if (desktop.isNull() || screenLogical.isEmpty()) {
        error = QStringLiteral("the screen capture cannot be mapped");
        return false;
    }
    const double scaleX = static_cast<double>(desktop.width()) / screenLogical.width();
    const double scaleY = static_cast<double>(desktop.height()) / screenLogical.height();
    if (qAbs(scaleX - screen->devicePixelRatio()) > 0.02 ||
        qAbs(scaleY - screen->devicePixelRatio()) > 0.02) {
        error = QStringLiteral("the capture scale does not match the screen device pixel ratio");
        return false;
    }
    const QPoint local = window->mapToGlobal(QPoint()) - screen->geometry().topLeft();
    const QRect physical(qRound(local.x() * scaleX), qRound(local.y() * scaleY),
                         qRound(window->width() * scaleX), qRound(window->height() * scaleY));
    if (!QRect(QPoint(), desktop.size()).contains(physical)) {
        error = QStringLiteral("the Settings surface is not fully on screen");
        return false;
    }
    surface = desktop.copy(physical);
    if (surface.isNull() || !surface.save(file)) {
        error = QStringLiteral("the Settings surface could not be saved");
        return false;
    }
    return true;
}

bool grabLabelPair(QScreen* screen, QWidget* window, QLabel* label,
                   const QString& surfaceFile, const QString& cropFile,
                   TextRasterCapture& capture) {
    capture.surfaceFile = surfaceFile;
    capture.cropFile = cropFile;
    if (screen == nullptr || window == nullptr || label == nullptr) {
        capture.error = QStringLiteral("no screen, window or label");
        return false;
    }
    QPixmap grab = screen->grabWindow(0);
    if (grab.isNull()) {
        capture.error = QStringLiteral("QScreen::grabWindow(0) returned no pixels");
        return false;
    }
    const QImage desktop = grab.toImage();
    const QSize screenLogical = screen->geometry().size();
    if (desktop.isNull() || screenLogical.isEmpty()) {
        capture.error = QStringLiteral("the screen capture cannot be mapped");
        return false;
    }
    const double scaleX = static_cast<double>(desktop.width()) / screenLogical.width();
    const double scaleY = static_cast<double>(desktop.height()) / screenLogical.height();
    if (qAbs(scaleX - screen->devicePixelRatio()) > 0.02 ||
        qAbs(scaleY - screen->devicePixelRatio()) > 0.02) {
        capture.error =
            QStringLiteral("the capture scale does not match the screen device pixel ratio");
        return false;
    }
    const QPoint windowLocal = window->mapToGlobal(QPoint()) - screen->geometry().topLeft();
    const QPoint labelLocal = label->mapToGlobal(QPoint()) - screen->geometry().topLeft();
    capture.logicalGlobal = QRect(label->mapToGlobal(QPoint()), label->size());
    capture.screenImageSize = desktop.size();
    capture.devicePixelRatio = label->devicePixelRatioF();
    capture.scaleX = scaleX;
    capture.scaleY = scaleY;
    const QRect physicalSurface(qRound(windowLocal.x() * scaleX),
                                qRound(windowLocal.y() * scaleY),
                                qRound(window->width() * scaleX),
                                qRound(window->height() * scaleY));
    capture.physicalCrop = QRect(qRound(labelLocal.x() * scaleX),
                                 qRound(labelLocal.y() * scaleY),
                                 qRound(label->width() * scaleX),
                                 qRound(label->height() * scaleY));
    capture.physicalSurface = physicalSurface;
    const QRect desktopRect(QPoint(), desktop.size());
    if (!desktopRect.contains(physicalSurface)) {
        capture.error = QStringLiteral("the Settings surface is not fully on screen");
        return false;
    }
    if (!desktopRect.contains(capture.physicalCrop) ||
        !physicalSurface.contains(capture.physicalCrop)) {
        capture.error = QStringLiteral("the label is not fully inside the Settings surface");
        return false;
    }
    const QImage surface = desktop.copy(physicalSurface);
    const QImage crop = desktop.copy(capture.physicalCrop);
    capture.crop = crop;
    capture.metrics = measureTextCrop(crop);
    if (crop.isNull() || crop.width() != capture.physicalCrop.width() ||
        crop.height() != capture.physicalCrop.height()) {
        capture.error = QStringLiteral("the label crop has unexpected dimensions");
        return false;
    }
    if (!capture.metrics.valid) {
        capture.error = QStringLiteral("the label crop is blank or carries no text contrast");
        return false;
    }
    if (!surface.save(surfaceFile)) {
        capture.error = QStringLiteral("the Settings surface could not be saved");
        return false;
    }
    if (!crop.save(cropFile)) {
        capture.error = QStringLiteral("the label crop could not be saved");
        return false;
    }
    capture.available = true;
    return true;
}

QJsonObject textRasterScenario(mikai::backend::MikaiClient& client, Runner& runner,
                               bool& comparable, bool& drained) {
    QJsonObject evidence;
    evidence.insert(QStringLiteral("probe"), QStringLiteral("native_settings_probe"));
    evidence.insert(QStringLiteral("scenario"), QStringLiteral("text_raster_only"));
    evidence.insert(QStringLiteral("networkAccess"), false);
    evidence.insert(QStringLiteral("modelAcquisition"), false);
    evidence.insert(QStringLiteral("runtimeAcquisition"), false);
    evidence.insert(QStringLiteral("inference"), false);
    evidence.insert(QStringLiteral("captureMethod"),
                    QStringLiteral("QScreen::grabWindow(0) after visible paint, cropped by "
                                   "measured physical/logical screen scale"));
    QJsonArray outputs;
    comparable = false;
    drained = false;
    QString appearanceFile = QStringLiteral("native-text-raster-appearance.png");
    {
        SettingsDialog dialog(client, runner);
        const QSize requested = fitSettingsDialogSize(
            dialog, mikai::probes::windowSize(
                        QSize(mikai::app::theme::kSettingsDialogDefaultWidth,
                              mikai::app::theme::kSettingsDialogDefaultHeight)));
        dialog.resize(requested);
        dialog.setWindowFlag(Qt::WindowStaysOnTopHint, true);
        dialog.show();
        dialog.raise();
        dialog.activateWindow();
        keepWindowOnScreen(dialog);
        dialog.raise();
        dialog.activateWindow();
        QApplication::processEvents();
        const bool exposed = waitForUi(
            [&dialog] {
                return dialog.isVisible() && dialog.windowHandle() != nullptr &&
                       dialog.windowHandle()->isExposed();
            },
            10000);
        check(exposed, "the Settings surface is visible and exposed before capture");

        QScreen* screen = dialog.screen();
        QImage appearance;
        QString appearanceError;
        check(dialog.showPage(QStringLiteral("customization/appearance")),
              "the real Appearance page is shown");
        settlePaint(&dialog, 250);
        check(screen != nullptr && grabNativeSurface(screen, &dialog, appearanceFile, appearance,
                                                     appearanceError),
              "the Appearance Settings surface was captured", appearanceError);
        if (!appearance.isNull()) outputs.append(appearanceFile);
        evidence.insert(QStringLiteral("appearanceSurface"), appearanceFile);
        evidence.insert(QStringLiteral("appearanceSwatches"),
                        widgetGeometryJson<mikai::app::SwatchButton>(
                            dialog, QStringLiteral("SwatchButton")));
        evidence.insert(QStringLiteral("appearanceToggles"),
                        widgetGeometryJson<mikai::app::ToggleSwitch>(
                            dialog, QStringLiteral("ToggleSwitch")));

        check(dialog.showPage(QStringLiteral("components")),
              "the real Components page is shown");
        auto* tab = dialog.findChild<mikai::app::ComponentStoreTab*>();
        QLabel* label = tab != nullptr
                            ? tab->findChild<QLabel*>(
                                  QStringLiteral("animeJanaiModelsCardDescription"))
                            : nullptr;
        check(tab != nullptr && label != nullptr,
              "the Components inventory and its model-card text label exist");
        bool settled = false;
        if (tab != nullptr && label != nullptr) {
            QElapsedTimer clock;
            clock.start();
            QString lastText;
            QRect lastRect;
            while (clock.elapsed() < 20000) {
                QApplication::processEvents();
                if (tab->inventoryReady() && !tab->acquisitionRunning() &&
                    label->isVisible() && !label->text().isEmpty()) {
                    if (label->text() == lastText && label->rect() == lastRect) {
                        settled = true;
                        break;
                    }
                    lastText = label->text();
                    lastRect = label->rect();
                } else {
                    lastText.clear();
                }
                QThread::msleep(50);
            }
            QApplication::processEvents();
        }
        check(settled, "the Components inventory and text surface settled before capture");
        evidence.insert(QStringLiteral("inventorySettled"), settled);

        const bool captureReady = settled && tab != nullptr && label != nullptr;
        if (!captureReady) {
            evidence.insert(QStringLiteral("captureAvailable"), false);
            evidence.insert(QStringLiteral("captureUnavailable"),
                            settled ? QStringLiteral("the model-card text label is unavailable")
                                    : QStringLiteral("the Components inventory did not settle"));
        } else {
        if (label != nullptr) {
            mikai::probes::revealWidget(label);
            waitForUi(
                [&label] {
                    return label->isVisible() &&
                           label->visibleRegion().contains(label->rect());
                },
                5000);
        }
        const TextLabelSnapshot before = snapshotTextLabel(label);
        check(before.fullyVisible, "the model-card text label is fully visible");
        const bool unoccluded = labelIsOnTop(label, dialog);
        check(unoccluded, "the model-card text label is not occluded at capture time");
        check(!before.translucent,
              "the model-card text label starts without WA_TranslucentBackground");
        evidence.insert(QStringLiteral("label"), textLabelJson(before));
        evidence.insert(QStringLiteral("actualDpr"), label != nullptr
                                                         ? label->devicePixelRatioF()
                                                         : 0.0);
        if (screen != nullptr) {
            evidence.insert(QStringLiteral("screenDevicePixelRatio"),
                            screen->devicePixelRatio());
            evidence.insert(QStringLiteral("screenLogicalGeometry"),
                            rectJson(screen->geometry()));
        }

        TextRasterCapture normal;
        TextRasterCapture translucent;
        screen = dialog.screen();
        const bool normalOk =
            label != nullptr && screen != nullptr &&
            grabLabelPair(screen, &dialog, label,
                          QStringLiteral("native-text-raster-components-normal.png"),
                          QStringLiteral("native-text-raster-label-normal.png"), normal);
        check(normalOk, "the normal-attribute label capture is comparable", normal.error);
        if (normalOk) outputs.append(normal.surfaceFile);
        if (normalOk) outputs.append(normal.cropFile);
        settlePaint(&dialog, 80);

        bool attributeApplied = false;
        bool attributeRestored = false;
        bool noSystemBackgroundForced = false;
        if (label != nullptr) {
            label->setAttribute(Qt::WA_TranslucentBackground, true);
            label->update();
            settlePaint(&dialog, 150);
            attributeApplied = label->testAttribute(Qt::WA_TranslucentBackground);
            noSystemBackgroundForced =
                label->testAttribute(Qt::WA_NoSystemBackground) &&
                !before.noSystemBackground;
            const bool unoccludedForSecond = labelIsOnTop(label, dialog);
            check(unoccludedForSecond,
                  "the model-card text label stayed unoccluded for the second capture");
            screen = dialog.screen();
            const bool translucentOk =
                screen != nullptr &&
                grabLabelPair(screen, &dialog, label,
                              QStringLiteral("native-text-raster-components-translucent.png"),
                              QStringLiteral("native-text-raster-label-translucent.png"),
                              translucent);
            check(translucentOk, "the translucent-attribute label capture is comparable",
                  translucent.error);
            if (translucentOk) outputs.append(translucent.surfaceFile);
            if (translucentOk) outputs.append(translucent.cropFile);
            label->setAttribute(Qt::WA_TranslucentBackground, before.translucent);
            label->setAttribute(Qt::WA_NoSystemBackground, before.noSystemBackground);
            label->update();
            settlePaint(&dialog, 150);
            attributeRestored =
                label->testAttribute(Qt::WA_TranslucentBackground) == before.translucent &&
                label->testAttribute(Qt::WA_NoSystemBackground) ==
                    before.noSystemBackground;
        }
        check(attributeApplied, "WA_TranslucentBackground was applied for the second capture");
        check(attributeRestored,
              "the original WA_TranslucentBackground and WA_NoSystemBackground were restored");
        evidence.insert(QStringLiteral("translucentForcedNoSystemBackground"),
                        noSystemBackgroundForced);
        const TextLabelSnapshot after = snapshotTextLabel(label);

        QJsonObject normalJson;
        normalJson.insert(QStringLiteral("available"), normal.available);
        normalJson.insert(QStringLiteral("error"), normal.error);
        normalJson.insert(QStringLiteral("surfaceFile"), normal.surfaceFile);
        normalJson.insert(QStringLiteral("cropFile"), normal.cropFile);
        normalJson.insert(QStringLiteral("logicalGlobalBounds"), rectJson(normal.logicalGlobal));
        normalJson.insert(QStringLiteral("physicalCropBounds"), rectJson(normal.physicalCrop));
        normalJson.insert(QStringLiteral("physicalSurfaceBounds"),
                          rectJson(normal.physicalSurface));
        normalJson.insert(QStringLiteral("screenImageSize"), rectJson(
            QRect(QPoint(), normal.screenImageSize)));
        normalJson.insert(QStringLiteral("devicePixelRatio"), normal.devicePixelRatio);
        normalJson.insert(QStringLiteral("measuredScaleX"), normal.scaleX);
        normalJson.insert(QStringLiteral("measuredScaleY"), normal.scaleY);
        normalJson.insert(QStringLiteral("metrics"), metricsJson(normal.metrics));
        QJsonObject translucentJson;
        translucentJson.insert(QStringLiteral("available"), translucent.available);
        translucentJson.insert(QStringLiteral("error"), translucent.error);
        translucentJson.insert(QStringLiteral("surfaceFile"), translucent.surfaceFile);
        translucentJson.insert(QStringLiteral("cropFile"), translucent.cropFile);
        translucentJson.insert(QStringLiteral("logicalGlobalBounds"),
                               rectJson(translucent.logicalGlobal));
        translucentJson.insert(QStringLiteral("physicalCropBounds"),
                               rectJson(translucent.physicalCrop));
        translucentJson.insert(QStringLiteral("physicalSurfaceBounds"),
                               rectJson(translucent.physicalSurface));
        translucentJson.insert(QStringLiteral("screenImageSize"), rectJson(
            QRect(QPoint(), translucent.screenImageSize)));
        translucentJson.insert(QStringLiteral("devicePixelRatio"),
                               translucent.devicePixelRatio);
        translucentJson.insert(QStringLiteral("measuredScaleX"), translucent.scaleX);
        translucentJson.insert(QStringLiteral("measuredScaleY"), translucent.scaleY);
        translucentJson.insert(QStringLiteral("metrics"), metricsJson(translucent.metrics));
        evidence.insert(QStringLiteral("normalCapture"), normalJson);
        evidence.insert(QStringLiteral("translucentCapture"), translucentJson);

        const bool samePosition = normal.logicalGlobal == translucent.logicalGlobal;
        const bool sameCropSize = normal.physicalCrop.size() == translucent.physicalCrop.size();
        const bool configurationHeld = sameTextLabelConfiguration(before, after);
        const bool dominantBackgroundSame =
            normal.metrics.valid && translucent.metrics.valid &&
            normal.metrics.modalColor == translucent.metrics.modalColor;
        check(samePosition, "both captures address the same screen position");
        check(sameCropSize, "both label crops have identical dimensions");
        check(configurationHeld,
              "the label text, font, background and geometry stayed unchanged");
        check(dominantBackgroundSame,
              "both captures show the same dominant painted background");
        evidence.insert(QStringLiteral("labelAfter"), textLabelJson(after));
        evidence.insert(QStringLiteral("labelConfigurationHeld"), configurationHeld);
        evidence.insert(QStringLiteral("dominantBackgroundSame"), dominantBackgroundSame);

        const CropDifference cropDifference =
            compareTextCrops(normal.crop, translucent.crop);
        comparable = normal.available && translucent.available && samePosition &&
                     sameCropSize && configurationHeld && dominantBackgroundSame &&
                     normal.metrics.valid && translucent.metrics.valid &&
                     attributeApplied && attributeRestored && cropDifference.comparable;
        evidence.insert(QStringLiteral("difference"), differenceJson(cropDifference));
        evidence.insert(QStringLiteral("comparable"), comparable);
        }

        dialog.reject();
        QApplication::processEvents();
    }
    runner.cancel();
    drained = runner.waitForDone(30000);
    check(drained, "the text-raster runner drained while its client was still alive");
    evidence.insert(QStringLiteral("runnerDrained"), drained);
    evidence.insert(QStringLiteral("outputs"), outputs);
    return evidence;
}

// ---------------------------------------------------------------------------
// Video scaling persistence (GOAL-H2)
//
// The goal's missing proof is the ordinary user path: edit the General card,
// Cancel, edit again, Save, and see the same policy come back in a freshly
// constructed dialog and in a fresh process. The scenario therefore drives the
// real controls and the real Save/Cancel actions of SettingsDialog; it never
// writes a scale key itself. What it asserts afterwards is what the application
// owns: the production store reader `videoScalePolicyFromSettings()` (the one
// `main.cpp` and a refreshed player use), the process-wide policy Save
// publishes, and the mpv option set built from it.
// ---------------------------------------------------------------------------

struct VideoScaleControls {
    QComboBox* mode = nullptr;
    QSpinBox* zoom = nullptr;
    QComboBox* upscale = nullptr;
    QComboBox* downscale = nullptr;
    QWidget* card = nullptr;
};

VideoScaleControls videoScaleControls(SettingsDialog& dialog) {
    VideoScaleControls controls;
    controls.mode = dialog.findChild<QComboBox*>(QStringLiteral("videoScaleModeCombo"));
    controls.zoom = dialog.findChild<QSpinBox*>(QStringLiteral("videoZoomSpin"));
    controls.upscale = dialog.findChild<QComboBox*>(QStringLiteral("videoUpscalerCombo"));
    controls.downscale = dialog.findChild<QComboBox*>(QStringLiteral("videoDownscalerCombo"));
    controls.card = controls.mode != nullptr ? controls.mode->parentWidget() : nullptr;
    return controls;
}

bool videoScaleControlsPresent(const VideoScaleControls& controls) {
    return controls.mode != nullptr && controls.zoom != nullptr &&
           controls.upscale != nullptr && controls.downscale != nullptr &&
           controls.card != nullptr && controls.zoom->parentWidget() == controls.card &&
           controls.upscale->parentWidget() == controls.card &&
           controls.downscale->parentWidget() == controls.card;
}

mplayer::VideoScalePolicy videoScalePolicyOfControls(const VideoScaleControls& controls) {
    mplayer::VideoScalePolicy policy;
    policy.mode =
        static_cast<mplayer::VideoScaleMode>(controls.mode->currentData().toInt());
    policy.zoomPercent = controls.zoom->value();
    policy.upscale = controls.upscale->currentData().toString();
    policy.downscale = controls.downscale->currentData().toString();
    return policy;
}

/// Selects the option each control offers for the policy's field. Returns false
/// when a control does not offer the id, which is a real failure of the card's
/// catalogue rather than something to coerce.
bool videoScaleSetControls(const VideoScaleControls& controls,
                           const mplayer::VideoScalePolicy& policy) {
    const int modeIndex = controls.mode->findData(static_cast<int>(policy.mode));
    const int upscaleIndex = controls.upscale->findData(policy.upscale);
    const int downscaleIndex = controls.downscale->findData(policy.downscale);
    if (modeIndex < 0 || upscaleIndex < 0 || downscaleIndex < 0) return false;
    controls.mode->setCurrentIndex(modeIndex);
    controls.zoom->setValue(policy.zoomPercent);
    controls.upscale->setCurrentIndex(upscaleIndex);
    controls.downscale->setCurrentIndex(downscaleIndex);
    QApplication::processEvents();
    return true;
}

QJsonObject videoScalePolicyJson(const mplayer::VideoScalePolicy& policy) {
    QJsonObject object;
    object.insert(QStringLiteral("mode"), static_cast<int>(policy.mode));
    object.insert(QStringLiteral("modeLabel"), mplayer::videoScaleModeLabel(policy.mode));
    object.insert(QStringLiteral("zoomPercent"), policy.zoomPercent);
    object.insert(QStringLiteral("upscale"), policy.upscale);
    object.insert(QStringLiteral("downscale"), policy.downscale);
    return object;
}

QJsonArray videoScaleOptionsJson(const mplayer::VideoScalePolicy& policy,
                                 const std::string& downscaleDefault) {
    QJsonArray array;
    for (const mplayer::VideoScaleOption& option :
         mplayer::videoScaleOptions(policy, downscaleDefault)) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), QString::fromStdString(option.name));
        item.insert(QStringLiteral("value"), QString::fromStdString(option.value));
        array.append(item);
    }
    return array;
}

QString videoScaleOptionValue(const std::vector<mplayer::VideoScaleOption>& options,
                              const char* name) {
    for (const mplayer::VideoScaleOption& option : options) {
        if (option.name == name) return QString::fromStdString(option.value);
    }
    return QString();
}

void checkVideoScaleControls(const VideoScaleControls& controls,
                             const mplayer::VideoScalePolicy& expected,
                             const char* what) {
    const mplayer::VideoScalePolicy shown = videoScalePolicyOfControls(controls);
    check(shown.mode == expected.mode, what, QStringLiteral("mode"));
    check(shown.zoomPercent == expected.zoomPercent, what, QStringLiteral("zoomPercent"));
    check(shown.upscale == expected.upscale, what, QStringLiteral("upscale"));
    check(shown.downscale == expected.downscale, what, QStringLiteral("downscale"));
}

QJsonObject videoScalingPersistenceScenario(mikai::backend::MikaiClient& client, Runner& runner,
                                            const QString& storeDir, const QString& selfPath,
                                            bool& restartVerified) {
    using mplayer::VideoScaleMode;
    using mplayer::VideoScalePolicy;
    using mplayer::VideoSession;

    restartVerified = false;
    QJsonObject evidence;
    evidence.insert(QStringLiteral("scenario"), QStringLiteral("video_scaling_persistence"));
    evidence.insert(QStringLiteral("store"), QDir::cleanPath(storeDir));
    evidence.insert(QStringLiteral("platform"), QGuiApplication::platformName());

    const VideoScalePolicy defaults;
    // The store is this run's own empty temporary directory, but the process may
    // already have run other scenarios: normalise the static so "Cancel changed
    // nothing" is measured against a known value rather than an inherited one.
    VideoSession::setVideoScalePolicy(defaults);
    QApplication::processEvents();

    {
        QSettings settings;
        check(mplayer::videoScalePolicyFromSettings(settings) == defaults,
              "the empty isolated store reads the documented default display policy");
    }
    check(VideoSession::videoScalePolicy() == defaults,
          "the process-wide display policy starts at that same default");

    VideoScalePolicy edited;
    edited.mode = VideoScaleMode::Fill;
    edited.zoomPercent = 25;
    edited.upscale = QStringLiteral("bicubic");
    edited.downscale = QStringLiteral("catmull_rom");

    // --- Cancel must write nothing and publish nothing -----------------------
    {
        SettingsDialog dialog(client, runner);
        const VideoScaleControls controls = videoScaleControls(dialog);
        check(videoScaleControlsPresent(controls),
              "the actual General page exposes all four video scaling controls on one card");
        if (!videoScaleControlsPresent(controls)) {
            evidence.insert(QStringLiteral("controlsPresent"), false);
            dialog.reject();
            QApplication::processEvents();
            return evidence;
        }
        evidence.insert(QStringLiteral("controlsPresent"), true);
        // The dialog is never shown: no activation, no global focus, no window
        // on the user's desktop. Every action below is a real widget action.
        check(!dialog.isVisible(),
              "the scaling smoke drives a hidden dialog: no activation or global focus");
        checkVideoScaleControls(controls, defaults,
                                "a freshly opened dialog shows the stored default policy");
        check(controls.zoom->isEnabled(),
              "the zoom control is usable in the default Fit mode");
        check(videoScaleSetControls(controls, edited),
              "the real controls offer Fill, 25% and the bicubic/catmull-rom scalers");
        check(videoScalePolicyOfControls(controls) == edited,
              "the controls hold the edited policy before Cancel");

        QPushButton* cancel = rejectButton(dialog.findChild<QDialogButtonBox*>());
        check(cancel != nullptr, "the actual General page exposes its real Cancel action");
        if (cancel != nullptr) cancel->click();
        QApplication::processEvents();
        check(dialog.result() == static_cast<int>(QDialog::Rejected),
              "the real Cancel action closed the dialog without accepting");
        check(VideoSession::videoScalePolicy() == defaults,
              "Cancel publishes no edited display policy to the live process");
        {
            QSettings settings;
            check(mplayer::videoScalePolicyFromSettings(settings) == defaults,
                  "Cancel leaves every scale field at its stored default");
        }
        evidence.insert(QStringLiteral("editedBeforeCancel"), videoScalePolicyJson(edited));
        dialog.reject();
        QApplication::processEvents();
    }

    // --- Save writes the complete policy; the live static adopts it ----------
    {
        SettingsDialog dialog(client, runner);
        const VideoScaleControls controls = videoScaleControls(dialog);
        check(videoScaleControlsPresent(controls),
              "the second dialog exposes the same four scaling controls");
        if (!videoScaleControlsPresent(controls)) {
            evidence.insert(QStringLiteral("secondDialogControlsPresent"), false);
            dialog.reject();
            QApplication::processEvents();
            return evidence;
        }
        checkVideoScaleControls(controls, defaults,
                                "a dialog reopened after Cancel still shows the stored default policy");
        check(videoScaleSetControls(controls, edited),
              "the saved policy can be entered through the real controls");
        check(VideoSession::videoScalePolicy() == defaults,
              "editing the controls publishes nothing before Save");

        // The seeded foreign store says uk while this probe process runs the
        // language its own startup installed. Save commits the interface
        // language before it reaches the scaling block, so the real selector is
        // aligned to what the process actually speaks: a probe without a
        // Ukrainian catalog must not fail on a catalog it never installed. This
        // touches no scale field and no control under test.
        if (auto* language = dialog.findChild<QComboBox*>(QStringLiteral("uiLanguageCombo"))) {
            const int installed =
                language->findData(mikai::app::activeInterfaceLanguage());
            if (installed >= 0) {
                language->setCurrentIndex(installed);
                QApplication::processEvents();
            }
        }

        QPushButton* save = acceptButton(dialog.findChild<QDialogButtonBox*>());
        check(save != nullptr, "the actual General page exposes its real Save action");
        if (save != nullptr) save->click();
        QApplication::processEvents();
        // Probe correction, not test-weakening: since 25bc266 ("Let Save commit
        // without closing Settings"), SettingsDialog::save() deliberately never
        // calls accept() - it commits, shows the "Saved" acknowledgment and
        // stays open - so result() can never be Accepted here. The contract is
        // result stays Rejected, the button shows "Saved" and is disabled. The
        // text is read through the SettingsDialog translation context so the
        // check holds in every locale, and no visibility is asserted: the
        // dialog is never shown in this scenario.
        check(save != nullptr && dialog.result() == static_cast<int>(QDialog::Rejected) &&
                  save->text() ==
                      QCoreApplication::translate("mikai::app::SettingsDialog", "Saved") &&
                  !save->isEnabled(),
              "the real Save action commits without closing the dialog");

        QSettings storedSettings;
        const VideoScalePolicy stored = mplayer::videoScalePolicyFromSettings(storedSettings);
        check(stored.mode == edited.mode,
              "Save stored the selected display mode");
        check(stored.zoomPercent == edited.zoomPercent,
              "Save stored the selected zoom percentage");
        check(stored.upscale == edited.upscale,
              "Save stored the selected upscaler");
        check(stored.downscale == edited.downscale,
              "Save stored the selected downscaler");
        check(stored == edited,
              "the production reader returns the complete saved policy");
        check(VideoSession::videoScalePolicy() == edited,
              "Save publishes the expected complete policy to the live process");

        // The consumer-visible effect: the saved policy owns all seven options.
        const std::vector<mplayer::VideoScaleOption> options =
            mplayer::videoScaleOptions(stored, {});
        check(videoScaleOptionValue(options, "keepaspect") == QStringLiteral("yes"),
              "the saved Fill policy keeps aspect");
        check(videoScaleOptionValue(options, "panscan") == QStringLiteral("1.000000"),
              "the saved Fill policy crops to fill");
        check(videoScaleOptionValue(options, "video-aspect-override") == QStringLiteral("no"),
              "the saved policy clears any aspect override");
        check(videoScaleOptionValue(options, "video-unscaled") == QStringLiteral("no"),
              "the saved policy does not request unscaled output");
        check(videoScaleOptionValue(options, "video-zoom") == QStringLiteral("0.321928"),
              "the saved 25% zoom reaches mpv as log2(1.25)");
        check(videoScaleOptionValue(options, "scale") == QStringLiteral("bicubic"),
              "the saved upscaler reaches mpv's scale option");
        check(videoScaleOptionValue(options, "dscale") == QStringLiteral("catmull_rom"),
              "the saved downscaler reaches mpv's dscale option");
        evidence.insert(QStringLiteral("saved"), videoScalePolicyJson(stored));
        evidence.insert(QStringLiteral("savedOptions"), videoScaleOptionsJson(stored, {}));
        evidence.insert(QStringLiteral("livePolicy"),
                        videoScalePolicyJson(VideoSession::videoScalePolicy()));
        dialog.reject();
        QApplication::processEvents();
    }

    // --- A fresh dialog in this process restores the controls ----------------
    {
        SettingsDialog dialog(client, runner);
        const VideoScaleControls controls = videoScaleControls(dialog);
        const bool present = videoScaleControlsPresent(controls);
        check(present, "a dialog reopened after Save exposes the four scaling controls");
        if (present) {
            checkVideoScaleControls(controls, edited,
                                    "the reopened dialog restores the saved policy");
            check(controls.zoom->isEnabled(),
                  "the restored Fill mode keeps the zoom control usable");
            evidence.insert(QStringLiteral("reopened"),
                            videoScalePolicyJson(videoScalePolicyOfControls(controls)));
        }
        check(!dialog.isVisible(), "the reopened dialog stayed hidden and unactivated");
        dialog.reject();
        QApplication::processEvents();
    }

    // --- A fresh process reads the same store --------------------------------
    // This is a real process boundary for the settings store and the production
    // reader. It is not the application's player bootstrap: no player is
    // created, so the surface-level startup path remains a separate gate.
    {
        QSettings flush;
        flush.sync();
    }
    const QString expectation = QStringLiteral("%1,%2,%3,%4")
                                    .arg(static_cast<int>(edited.mode))
                                    .arg(edited.zoomPercent)
                                    .arg(edited.upscale, edited.downscale);
    const QString restartEvidencePath =
        QDir(storeDir).filePath(QStringLiteral("video-scaling-restart-evidence.json"));
    QFile::remove(restartEvidencePath);

    QProcess child;
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.setProgram(selfPath);
    child.setWorkingDirectory(storeDir);
    child.setArguments({QStringLiteral("--video-scaling-smoke-read"),
                        QStringLiteral("--video-scaling-store"), storeDir,
                        QStringLiteral("--video-scaling-expect"), expectation,
                        QStringLiteral("--video-scaling-evidence"), restartEvidencePath});
    child.start();
    const bool childStarted = child.waitForStarted(30000);
    check(childStarted, "a fresh process was started from the same probe executable");
    bool childFinished = false;
    int childExit = -1;
    QByteArray childOutput;
    if (childStarted) {
        childFinished = child.waitForFinished(120000);
        if (!childFinished) child.kill();
        childOutput = child.readAllStandardOutput();
        childExit = child.exitCode();
    }
    QJsonObject childEvidence;
    {
        QFile file(restartEvidencePath);
        if (file.open(QIODevice::ReadOnly)) {
            childEvidence = QJsonDocument::fromJson(file.readAll()).object();
        }
    }
    const bool childPassed = childEvidence.value(QStringLiteral("passed")).toBool();
    check(childFinished && childExit == 0,
          "the fresh process exited accepting the policy it read from the store",
          QString::fromLocal8Bit(childOutput.right(2000)));
    check(!childEvidence.isEmpty(),
          "the fresh process wrote its own restart evidence");
    check(childPassed,
          "the fresh process accepted the saved policy through the production reader");
    evidence.insert(QStringLiteral("restart"), childEvidence);
    evidence.insert(QStringLiteral("restartExitCode"), childExit);
    evidence.insert(QStringLiteral("restartOutput"),
                    QString::fromLocal8Bit(childOutput.right(4000)));
    restartVerified = childFinished && childExit == 0 && childPassed;
    evidence.insert(QStringLiteral("restartVerified"), restartVerified);
    return evidence;
}

QJsonObject videoScalingRestartRead(mikai::backend::MikaiClient& client, Runner& runner,
                                    const QString& storeDir, const QString& expectation,
                                    const QString& evidencePath) {
    using mplayer::VideoScaleMode;
    using mplayer::VideoScalePolicy;
    using mplayer::VideoSession;

    QJsonObject evidence;
    evidence.insert(QStringLiteral("scenario"), QStringLiteral("video_scaling_restart_read"));
    evidence.insert(QStringLiteral("store"), QDir::cleanPath(storeDir));
    evidence.insert(QStringLiteral("platform"), QGuiApplication::platformName());

    const QStringList fields = expectation.split(QLatin1Char(','));
    bool expectationValid = fields.size() == 4;
    VideoScalePolicy expected;
    if (expectationValid) {
        bool modeOk = false;
        bool zoomOk = false;
        const int mode = fields.at(0).toInt(&modeOk);
        const int zoom = fields.at(1).toInt(&zoomOk);
        expectationValid =
            modeOk && zoomOk && mode >= static_cast<int>(VideoScaleMode::Fit) &&
            mode <= static_cast<int>(VideoScaleMode::Original) && zoom >= -50 && zoom <= 100;
        if (expectationValid) {
            expected.mode = static_cast<VideoScaleMode>(mode);
            expected.zoomPercent = zoom;
            expected.upscale = fields.at(2);
            expected.downscale = fields.at(3);
        }
    }
    check(expectationValid,
          "the restart expectation names a valid mode, zoom and scaler pair");
    evidence.insert(QStringLiteral("expected"), videoScalePolicyJson(expected));

    check(VideoSession::videoScalePolicy() == VideoScalePolicy{},
          "a fresh process starts at the default policy before any store read");
    QSettings readSettings;
    const VideoScalePolicy read = mplayer::videoScalePolicyFromSettings(readSettings);
    check(read.mode == expected.mode,
          "the fresh process reads the saved display mode from the store");
    check(read.zoomPercent == expected.zoomPercent,
          "the fresh process reads the saved zoom percentage from the store");
    check(read.upscale == expected.upscale,
          "the fresh process reads the saved upscaler from the store");
    check(read.downscale == expected.downscale,
          "the fresh process reads the saved downscaler from the store");
    check(read == expected,
          "the fresh process reads the complete saved policy from the store");
    evidence.insert(QStringLiteral("read"), videoScalePolicyJson(read));

    // Production startup loads the persisted policy before constructing any
    // player or Settings page. Mirror that bootstrap in this probe process.
    VideoSession::setVideoScalePolicy(read);
    check(VideoSession::videoScalePolicy() == expected,
          "startup publishes the persisted policy before the General page opens");

    {
        SettingsDialog dialog(client, runner);
        const VideoScaleControls controls = videoScaleControls(dialog);
        const bool present = videoScaleControlsPresent(controls);
        check(present,
              "the fresh process's General page exposes the four scaling controls");
        if (present) {
            checkVideoScaleControls(controls, expected,
                                    "the fresh process's General page restores the saved policy");
            evidence.insert(QStringLiteral("restored"),
                            videoScalePolicyJson(videoScalePolicyOfControls(controls)));
        }
        check(!dialog.isVisible(),
              "the restart dialog stayed hidden: no activation or global focus");
        dialog.reject();
        QApplication::processEvents();
    }

    const bool passed = failures == 0;
    evidence.insert(QStringLiteral("passed"), passed);
    evidence.insert(QStringLiteral("checks"), checks);
    evidence.insert(QStringLiteral("failures"), failures);
    bool written = false;
    const bool absoluteEvidencePath = !evidencePath.isEmpty() && QFileInfo(evidencePath).isAbsolute();
    if (absoluteEvidencePath) {
        QFile file(evidencePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const QByteArray payload = QJsonDocument(evidence).toJson(QJsonDocument::Indented);
            written = file.write(payload) == payload.size();
            file.close();
        }
    }
    check(written, "the restart read wrote its evidence file");
    return evidence;
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
    // These settings-only scenarios never load mpv, so do not ask isolation to
    // duplicate the potentially large runtime. Keep executable isolation
    // enabled: even direct invocations must not run beside a real native store.
    bool videoScalingSettingsOnly = false;
    for (int i = 1; i < argc; ++i) {
        const QByteArray argument(argv[i]);
        if (argument == "--video-scaling-smoke" ||
            argument == "--video-scaling-smoke-read") {
            videoScalingSettingsOnly = true;
            break;
        }
    }
    const bool hadMpvModule = qEnvironmentVariableIsSet("MIKAI_MPV_MODULE");
    const QByteArray mpvModule = qgetenv("MIKAI_MPV_MODULE");
    if (videoScalingSettingsOnly) qunsetenv("MIKAI_MPV_MODULE");
    const Gate gate = enter(argc, argv, isolationOptions);
    if (videoScalingSettingsOnly && hadMpvModule) {
        qputenv("MIKAI_MPV_MODULE", mpvModule);
    }
    if (gate != Gate::RunHere) return exitCode();

    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QApplication app(argc, argv);
    if (!mikai::probes::initializeUi(app)) return 2;
    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption installRuntimeOption(
        QStringLiteral("install-runtime"),
        QStringLiteral("Opt in to a real reviewed DirectML runtime installation smoke."));
    parser.addOption(installRuntimeOption);
    QCommandLineOption componentProgressOnlyOption(
        QStringLiteral("component-progress-only"),
        QStringLiteral("Run only the isolated synthetic Components progress presentation scenario."));
    parser.addOption(componentProgressOnlyOption);
    QCommandLineOption hdrSettingsOnlyOption(
        QStringLiteral("hdr-settings-only"),
        QStringLiteral("Run only the actual General HDR settings presentation scenario."));
    parser.addOption(hdrSettingsOnlyOption);
    QCommandLineOption spatialAcquisitionOnlyOption(
        QStringLiteral("spatial-acquisition-only"),
        QStringLiteral("Run only the real spatial-model acquisition from the empty owned store."));
    parser.addOption(spatialAcquisitionOnlyOption);
    QCommandLineOption textRasterOnlyOption(
        QStringLiteral("text-raster-only"),
        QStringLiteral("Capture the real Settings Components label twice: normal child "
                       "attributes and WA_TranslucentBackground."));
    parser.addOption(textRasterOnlyOption);
    QCommandLineOption videoScalingSmokeOption(
        QStringLiteral("video-scaling-smoke"),
        QStringLiteral("Run only the actual General video scaling Save/Cancel/reopen "
                       "persistence scenario, including a fresh-process store read."));
    parser.addOption(videoScalingSmokeOption);
    QCommandLineOption videoScalingSmokeReadOption(
        QStringLiteral("video-scaling-smoke-read"),
        QStringLiteral("Read the store a --video-scaling-smoke run saved, in a fresh "
                       "process, through the production settings reader."));
    parser.addOption(videoScalingSmokeReadOption);
    QCommandLineOption videoScalingStoreOption(
        QStringLiteral("video-scaling-store"),
        QStringLiteral("Settings store directory the fresh-process read uses."),
        QStringLiteral("directory"));
    parser.addOption(videoScalingStoreOption);
    QCommandLineOption videoScalingExpectOption(
        QStringLiteral("video-scaling-expect"),
        QStringLiteral("Policy the fresh process must read: mode,zoom,upscale,downscale."),
        QStringLiteral("spec"));
    parser.addOption(videoScalingExpectOption);
    QCommandLineOption videoScalingEvidenceOption(
        QStringLiteral("video-scaling-evidence"),
        QStringLiteral("File the fresh-process read writes its evidence to."),
        QStringLiteral("path"));
    parser.addOption(videoScalingEvidenceOption);
    QCommandLineOption videoScalingLiveOption(
        QStringLiteral("video-scaling-live-smoke"),
        QStringLiteral("Run the visible real-player display scaling smoke: Settings "
                       "Save/Cancel, flyout rows, paused adoption, PiP inheritance."));
    parser.addOption(videoScalingLiveOption);
    QCommandLineOption videoScalingMediaOption(
        QStringLiteral("video-scaling-media"),
        QStringLiteral("Absolute media path for the live smoke (a long clip, >= 60 s)."),
        QStringLiteral("path"));
    parser.addOption(videoScalingMediaOption);
    QCommandLineOption videoScalingHoldOption(
        QStringLiteral("video-scaling-hold-ms"),
        QStringLiteral("Keep the finished live-smoke player visible this long, ms."),
        QStringLiteral("ms"));
    parser.addOption(videoScalingHoldOption);
    parser.process(app);
    const bool installRuntime = parser.isSet(installRuntimeOption);
    const bool componentProgressOnly = parser.isSet(componentProgressOnlyOption);
    const bool hdrSettingsOnly = parser.isSet(hdrSettingsOnlyOption);
    const bool spatialAcquisitionOnly = parser.isSet(spatialAcquisitionOnlyOption);
    const bool textRasterOnly = parser.isSet(textRasterOnlyOption);
    const bool videoScalingSmoke = parser.isSet(videoScalingSmokeOption);
    const bool videoScalingSmokeRead = parser.isSet(videoScalingSmokeReadOption);
    const bool videoScalingLiveSmoke = parser.isSet(videoScalingLiveOption);
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiNativeSettingsProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("NativeSettingsProbe"));

    // The writer run owns a fresh settings directory; the restart read is handed
    // the directory the writer saved into and opens exactly that surface, which
    // is the process boundary the persistence claim rests on.
    QTemporaryDir settingsDir;
    QString settingsRoot;
    if (videoScalingSmokeRead) {
        settingsRoot = QDir::cleanPath(parser.value(videoScalingStoreOption));
        if (settingsRoot.isEmpty() || !QFileInfo(settingsRoot).isDir()) {
            std::fprintf(stderr,
                         "FAIL the restart read needs an existing settings store directory\n");
            return 2;
        }
    } else {
        if (!settingsDir.isValid()) {
            std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
            return 1;
        }
        settingsRoot = settingsDir.path();
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsRoot);

    const QString recipeId = QStringLiteral("janai.unknown.v9");
    if (!videoScalingSmokeRead) {
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
        if (!hdrSettingsOnly) {
            settings.setValue(QLatin1String(mplayer::native::kNativeCustomModelsDirKey),
                              QStringLiteral("foreign-model-store-path"));
        }
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

    if (videoScalingSmokeRead) {
        const QJsonObject evidence = videoScalingRestartRead(
            client, runner, settingsRoot, parser.value(videoScalingExpectOption),
            parser.value(videoScalingEvidenceOption));
        const bool passed =
            failures == 0 && evidence.value(QStringLiteral("passed")).toBool();
        std::fprintf(stderr,
                     "[native_settings_probe video-scaling-smoke-read] %s (%d checks, "
                     "%d failures)\n",
                     passed ? "PASS" : "FAIL", checks, failures);
        return passed ? 0 : 1;
    }

    if (videoScalingSmoke) {
        // The launcher runs the probe in a disposable scratch directory, so a
        // relative evidence file would be deleted with it. An explicit path must
        // be absolute and its directory must already exist; otherwise the
        // default stays beside a direct run's working directory.
        QString evidencePath = parser.value(videoScalingEvidenceOption);
        bool pathAccepted = true;
        if (evidencePath.isEmpty()) {
            evidencePath = QStringLiteral("native-video-scaling-persistence-evidence.json");
        } else {
            const QFileInfo info(evidencePath);
            pathAccepted = info.isAbsolute() && info.absoluteDir().exists();
            if (!pathAccepted) {
                std::fprintf(stderr,
                             "FAIL --video-scaling-evidence must be an absolute path in an "
                             "existing directory\n");
                ++failures;
            }
            evidencePath = info.absoluteFilePath();
        }
        bool restartVerified = false;
        QJsonObject evidence = videoScalingPersistenceScenario(
            client, runner, settingsRoot, QCoreApplication::applicationFilePath(),
            restartVerified);
        const bool passed = failures == 0 && restartVerified && pathAccepted;
        evidence.insert(QStringLiteral("probe"), QStringLiteral("native_settings_probe"));
        evidence.insert(QStringLiteral("evidencePath"), QDir::cleanPath(evidencePath));
        evidence.insert(QStringLiteral("passed"), passed);
        evidence.insert(QStringLiteral("checks"), checks);
        evidence.insert(QStringLiteral("failures"), failures);
        QFile evidenceFile(evidencePath);
        bool evidenceWritten = false;
        if (evidenceFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const QByteArray payload = QJsonDocument(evidence).toJson(QJsonDocument::Indented);
            evidenceWritten = evidenceFile.write(payload) == payload.size();
            evidenceFile.close();
        }
        if (!evidenceWritten) {
            std::fprintf(stderr, "FAIL the video scaling persistence evidence was written to %s\n",
                         qPrintable(evidencePath));
        }
        std::fprintf(stderr,
                     "[native_settings_probe video-scaling-smoke] %s (%d checks, %d failures) "
                     "restart=%d evidence=%d path=%s\n",
                     passed ? "PASS" : "FAIL", checks, failures, restartVerified ? 1 : 0,
                     evidenceWritten ? 1 : 0, qPrintable(evidencePath));
        return passed && evidenceWritten ? 0 : 1;
    }

    if (videoScalingLiveSmoke) {
        mikai::probes::VideoScalingLiveOptions liveOptions;
        liveOptions.mediaPath = parser.value(videoScalingMediaOption);
        liveOptions.settingsRoot = settingsRoot;
        liveOptions.selfPath = QCoreApplication::applicationFilePath();
        liveOptions.evidencePath = parser.value(videoScalingEvidenceOption);
        const QString holdValue = parser.value(videoScalingHoldOption);
        bool holdOk = true;
        liveOptions.holdMs = 0;
        if (!holdValue.isEmpty()) {
            liveOptions.holdMs = holdValue.toInt(&holdOk);
        }
        if (!holdOk) {
            std::fprintf(stderr, "FAIL --video-scaling-hold-ms must be a whole number of ms\n");
            return 2;
        }
        return mikai::probes::runVideoScalingLiveSmoke(client, runner, liveOptions);
    }

    if (textRasterOnly) {
        bool comparable = false;
        bool drained = false;
        QJsonObject evidence = textRasterScenario(client, runner, comparable, drained);
        const QJsonObject normalCapture =
            evidence.value(QStringLiteral("normalCapture")).toObject();
        const QJsonObject translucentCapture =
            evidence.value(QStringLiteral("translucentCapture")).toObject();
        evidence.insert(QStringLiteral("captureAvailable"),
                        normalCapture.value(QStringLiteral("available")).toBool() &&
                            translucentCapture.value(QStringLiteral("available")).toBool());
        const bool passed = failures == 0 && comparable && drained;
        evidence.insert(QStringLiteral("passed"), passed);
        evidence.insert(QStringLiteral("checks"), checks);
        evidence.insert(QStringLiteral("failures"), failures);
        QFile evidenceFile(QStringLiteral("native-text-raster-evidence.json"));
        bool evidenceWritten = false;
        if (evidenceFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const QByteArray payload =
                QJsonDocument(evidence).toJson(QJsonDocument::Indented);
            evidenceWritten = evidenceFile.write(payload) == payload.size();
            evidenceFile.close();
        }
        if (!evidenceWritten) {
            std::fprintf(stderr, "FAIL the text-raster evidence was written\n");
        }
        std::fprintf(stderr,
                     "[native_settings_probe text-raster-only] %s (%d failures) "
                     "comparable=%d drained=%d evidence=%d\n",
                     passed ? "PASS" : "FAIL", failures, comparable ? 1 : 0,
                     drained ? 1 : 0, evidenceWritten ? 1 : 0);
        if (!drained) {
            std::fprintf(stderr,
                         "[native_settings_probe text-raster-only] the runner did not "
                         "drain; exiting without unwinding the borrowed client\n");
            std::fflush(stderr);
            std::_Exit(1);
        }
        return passed && evidenceWritten ? 0 : 1;
    }

    const QString hdrLanguageBefore = mikai::app::activeInterfaceLanguage();
    const QSize hdrCompact(mikai::app::theme::kSettingsDialogMinWidth,
                           mikai::app::theme::kSettingsDialogDefaultHeight);
    const QSize hdrWide(mikai::app::theme::kSettingsDialogDefaultWidth,
                        mikai::app::theme::kSettingsDialogDefaultHeight);
    const QString hdrCatalog = qEnvironmentVariable("MIKAI_PROBE_TRANSLATION");
    if (hdrSettingsOnly ||
        (!componentProgressOnly && !spatialAcquisitionOnly && !textRasterOnly)) {
        for (const QString& language : {QStringLiteral("en"), QStringLiteral("uk")}) {
            exerciseHdrPresentation(client, runner, hdrCompact, language, hdrCatalog);
            exerciseHdrPresentation(client, runner, hdrWide, language, hdrCatalog);
        }
        check(mikai::app::applyInterfaceLanguage(hdrLanguageBefore, hdrCatalog),
              "the probe restores its original interface language after HDR captures");
    }

    if (spatialAcquisitionOnly) {
        check(mikai::app::applyInterfaceLanguage(QStringLiteral("en"), probeCatalog),
              "the acquisition surface is rendered in English for the evidence run");
        // The acquisition needs a real backend client and a real runner. No
        // credential is consulted for the pinned raw.githubusercontent.com URLs,
        // and the environment-file search is pointed at a file this run owns, so
        // a stray .env beside the output directory cannot supply one. The
        // environment variable is cleared too: the default constructor adopts
        // MIKAI_API_KEY, so leaving it set would break the anonymous contract.
        mikai::backend::setEnvFilePathOverride(
            QDir(mikai::probes::isolation::isolatedRoot())
                .filePath(QStringLiteral("probe-anonymous.env")));
        qunsetenv("MIKAI_API_KEY");
        mikai::backend::MikaiClient acquisitionClient;
        check(acquisitionClient.apiKey().isEmpty(),
              "the acquisition client carries no credential");
        Runner acquisitionRunner;
        SpatialAcquisitionEvidence evidence;
        {
            SettingsDialog dialog(acquisitionClient, acquisitionRunner);
            const QSize size(mikai::app::theme::kSettingsDialogDefaultWidth,
                             mikai::app::theme::kSettingsDialogDefaultHeight);
            exerciseSpatialAcquisition(dialog, size, evidence);
            dialog.reject();
            QApplication::processEvents();
        }
        // Drain before the borrowed client leaves scope: every transfer the tab
        // started must be retired while the client it captured is still alive.
        // Cancel first so a stuck unit cannot outlive the finite drain budget.
        acquisitionRunner.cancel();
        const bool drained = acquisitionRunner.waitForDone(30000);
        check(drained, "the acquisition runner drained while its client was still alive");

        const int catalogueSize = mplayer::native::spatialAssetCatalog().size();
        const bool live = evidence.started && !evidence.timedOut &&
                          evidence.terminalStorePresent == catalogueSize;
        const bool proof = live && evidence.finished && evidence.terminalVerified &&
                           evidence.assets.size() == catalogueSize && drained &&
                           failures == 0;

        QJsonObject result;
        result.insert(QStringLiteral("probe"), QStringLiteral("native_settings_probe"));
        result.insert(QStringLiteral("scenario"), QStringLiteral("spatial_acquisition_only"));
        result.insert(QStringLiteral("syntheticProgressPresentation"), false);
        result.insert(QStringLiteral("liveNetworkAcquisition"), live);
        result.insert(QStringLiteral("acquisitionProof"), proof);
        result.insert(QStringLiteral("inferenceProof"), false);
        result.insert(QStringLiteral("store"), mplayer::native::spatialAssetRoot());
        result.insert(QStringLiteral("catalogueSize"), catalogueSize);
        result.insert(QStringLiteral("catalogueCommit"),
                      mplayer::native::spatialAssetCommit());
        result.insert(QStringLiteral("started"), evidence.started);
        result.insert(QStringLiteral("timedOut"), evidence.timedOut);
        result.insert(QStringLiteral("finished"), evidence.finished);
        result.insert(QStringLiteral("terminalVerified"), evidence.terminalVerified);
        result.insert(QStringLiteral("runnerDrained"), drained);
        result.insert(QStringLiteral("reviewDialogPresented"), evidence.reviewPresented);
        result.insert(QStringLiteral("reviewDialogAccepted"), evidence.reviewAccepted);
        result.insert(QStringLiteral("initial"), evidence.initial);
        result.insert(QStringLiteral("terminal"), evidence.terminal);
        result.insert(QStringLiteral("transitions"), evidence.transitions);
        result.insert(QStringLiteral("assets"), evidence.assets);
        QJsonArray screenshots;
        for (const QString& shot : evidence.screenshots) screenshots.append(shot);
        result.insert(QStringLiteral("screenshots"), screenshots);
        // The final verdict is the post-drain one: a run that acquired every
        // graph but could not retire its workers is not a pass.
        const bool passed = failures == 0 && drained;
        result.insert(QStringLiteral("passed"), passed);
        result.insert(QStringLiteral("checks"), checks);
        result.insert(QStringLiteral("failures"), failures);
        QFile evidenceFile(QStringLiteral("native-spatial-acquisition-evidence.json"));
        const bool evidenceWritten = [&result, &evidenceFile] {
            if (!evidenceFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
            const QByteArray payload = QJsonDocument(result).toJson(QJsonDocument::Indented);
            const bool wrote = evidenceFile.write(payload) == payload.size();
            evidenceFile.close();
            return wrote;
        }();
        if (!evidenceWritten) {
            std::fprintf(stderr, "FAIL the spatial acquisition evidence was written\n");
        }
        std::fprintf(stderr,
                     "[native_settings_probe spatial-acquisition-only] %s (%d failures) "
                     "live=%d proof=%d drained=%d transition-records=%d\n",
                     passed ? "PASS" : "FAIL", failures, live ? 1 : 0, proof ? 1 : 0,
                     drained ? 1 : 0, static_cast<int>(evidence.transitions.size()));
        if (!drained) {
            // Returning would destroy the borrowed client while workers it
            // captured may still be running. The evidence is already on disk;
            // leave through the process boundary so the isolation gate removes
            // the owned child and its scratch without unwinding here.
            std::fprintf(stderr,
                         "[native_settings_probe spatial-acquisition-only] the runner did "
                         "not drain; exiting without unwinding the borrowed client\n");
            std::fflush(stderr);
            std::_Exit(1);
        }
        return passed && evidenceWritten ? 0 : 1;
    }

    if (hdrSettingsOnly) {
        QJsonObject evidence;
        evidence.insert(QStringLiteral("probe"), QStringLiteral("native_settings_probe"));
        evidence.insert(QStringLiteral("scenario"), QStringLiteral("hdr_settings_presentation"));
        evidence.insert(QStringLiteral("passed"), failures == 0);
        evidence.insert(QStringLiteral("checks"), checks);
        evidence.insert(QStringLiteral("failures"), failures);
        QJsonArray screenshots;
        screenshots.append(QStringLiteral("native-general-hdr-en-compact.png"));
        screenshots.append(QStringLiteral("native-general-hdr-en-wide.png"));
        screenshots.append(QStringLiteral("native-general-hdr-uk-compact.png"));
        screenshots.append(QStringLiteral("native-general-hdr-uk-wide.png"));
        evidence.insert(QStringLiteral("screenshots"), screenshots);
        QFile evidenceFile(QStringLiteral("native-hdr-settings-evidence.json"));
        if (evidenceFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            evidenceFile.write(QJsonDocument(evidence).toJson(QJsonDocument::Indented));
        } else {
            check(false, "HDR settings presentation evidence was written");
        }
        std::fprintf(stderr, "[native_settings_probe hdr-settings-only] %s (%d failures)\n",
                     failures == 0 ? "PASS" : "FAIL", failures);
        return failures == 0 ? 0 : 1;
    }

    if (componentProgressOnly) {
        mikai::app::NativeComponentProvisioner provisioner(runner);
        const QString catalog = qEnvironmentVariable("MIKAI_PROBE_TRANSLATION");
        const QSize compact(mikai::app::theme::kSettingsDialogMinWidth,
                            mikai::app::theme::kSettingsDialogDefaultHeight);
        const QSize wide(mikai::app::theme::kSettingsDialogDefaultWidth,
                         mikai::app::theme::kSettingsDialogDefaultHeight);
        QJsonArray runs;
        for (const QString& language : {QStringLiteral("en"), QStringLiteral("uk")}) {
            check(mikai::app::applyInterfaceLanguage(language, catalog),
                  "the requested Components presentation language was installed");
            for (const QSize size : {compact, wide}) {
                SettingsDialog dialog(client, runner);
                runs.append(exerciseComponentProgress(dialog, size, language, provisioner));
            }
        }

        QJsonObject evidence;
        evidence.insert(QStringLiteral("probe"), QStringLiteral("native_settings_probe"));
        evidence.insert(QStringLiteral("scenario"),
                        QStringLiteral("component_progress_presentation_only"));
        evidence.insert(QStringLiteral("syntheticProgressPresentation"), true);
        evidence.insert(QStringLiteral("acquisitionProof"), false);
        evidence.insert(QStringLiteral("inferenceProof"), false);
        evidence.insert(QStringLiteral("runs"), runs);
        evidence.insert(QStringLiteral("passed"), failures == 0);
        evidence.insert(QStringLiteral("checks"), checks);
        evidence.insert(QStringLiteral("failures"), failures);
        QJsonArray screenshots;
        screenshots.append(QStringLiteral("native-component-progress-en-compact.png"));
        screenshots.append(QStringLiteral("native-component-progress-en-wide.png"));
        screenshots.append(QStringLiteral("native-component-progress-uk-compact.png"));
        screenshots.append(QStringLiteral("native-component-progress-uk-wide.png"));
        evidence.insert(QStringLiteral("screenshots"), screenshots);
        QFile evidenceFile(QStringLiteral("native-component-progress-evidence.json"));
        if (evidenceFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            evidenceFile.write(QJsonDocument(evidence).toJson(QJsonDocument::Indented));
        } else {
            check(false, "synthetic progress presentation evidence was written");
        }
        std::fprintf(stderr, "[native_settings_probe component-progress-only] %s (%d failures)\\n",
                     failures == 0 ? "PASS" : "FAIL", failures);
        return failures == 0 ? 0 : 1;
    }

    {
        SettingsDialog dialog(client, runner);
        const QSize requestedSize =
            fitSettingsDialogSize(dialog, mikai::probes::windowSize(QSize(760, 780)));
        dialog.resize(requestedSize);
        dialog.showPage(QStringLiteral("benchmark"));
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
        // Since 25bc266 ("Let Save commit without closing Settings"), Save
        // commits without closing: result stays Rejected, the button shows
        // the "Saved" acknowledgment and is disabled.
        check(save != nullptr && dialog.result() == static_cast<int>(QDialog::Rejected) &&
                  save->text() ==
                      QCoreApplication::translate("mikai::app::SettingsDialog", "Saved") &&
                  !save->isEnabled(),
              "the Save button commits without closing the dialog");
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
        // Since 25bc266 ("Let Save commit without closing Settings"), Save
        // commits without closing: result stays Rejected, the button shows
        // the "Saved" acknowledgment and is disabled.
        check(save != nullptr && dialog.result() == static_cast<int>(QDialog::Rejected) &&
                  save->text() ==
                      QCoreApplication::translate("mikai::app::SettingsDialog", "Saved") &&
                  !save->isEnabled(),
              "Save commits backend and recipe edits without closing the dialog");
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
                    // Since 25bc266 ("Let Save commit without closing
                    // Settings"), Save commits without closing: result stays
                    // Rejected, the button shows the "Saved" acknowledgment
                    // and is disabled.
                    check(languageSave != nullptr &&
                              dialog.result() == static_cast<int>(QDialog::Rejected) &&
                              languageSave->text() == QCoreApplication::translate(
                                  "mikai::app::SettingsDialog", "Saved") &&
                              !languageSave->isEnabled(),
                          "the Save that carries the language commits without closing the dialog");
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
                // The navigation entry is the caption a user sees for the page;
                // its id is the stable key, never the translated text.
                QPushButton* processing = navEntryFor(dialog, QStringLiteral("processing"));
                QPushButton* save = acceptButton(dialog.findChild<QDialogButtonBox*>());
                return qMakePair(processing != nullptr ? processing->text() : QString(),
                                 save != nullptr ? save->text() : QString());
            };
            const auto storedLanguage = [] {
                QSettings settings;
                return settings.value(QStringLiteral("ui/language")).toString();
            };

            {
                SettingsDialog dialog(client, runner);
                dialog.resize(fitSettingsDialogSize(
                    dialog, mikai::probes::windowSize(QSize(760, 780))));
                dialog.showPage(QStringLiteral("customization/general"));
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
                dialog.showPage(QStringLiteral("customization/general"));
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
                dialog.showPage(QStringLiteral("customization/general"));
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
                dialog.showPage(QStringLiteral("customization/general"));
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
                // Since 25bc266 ("Let Save commit without closing Settings"),
                // Save commits without closing: result stays Rejected, the
                // button shows the "Saved" acknowledgment and is disabled.
                check(save != nullptr && dialog.result() == static_cast<int>(QDialog::Rejected) &&
                          save->text() ==
                              QCoreApplication::translate("mikai::app::SettingsDialog", "Saved") &&
                          !save->isEnabled(),
                      "Save commits the previewed language without closing the dialog");
                check(mikai::app::activeInterfaceLanguage() == QStringLiteral("uk"),
                      "Save commits the previewed language");
                check(storedLanguage() == QStringLiteral("uk"),
                      "Save is the path that writes the language preference");
                SettingsDialog reopened(client, runner);
                reopened.resize(fitSettingsDialogSize(
                    reopened, mikai::probes::windowSize(QSize(760, 780))));
                reopened.showPage(QStringLiteral("customization/general"));
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
    screenshots.append(QStringLiteral("native-general-hdr-en-compact.png"));
    screenshots.append(QStringLiteral("native-general-hdr-en-wide.png"));
    screenshots.append(QStringLiteral("native-general-hdr-uk-compact.png"));
    screenshots.append(QStringLiteral("native-general-hdr-uk-wide.png"));
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
