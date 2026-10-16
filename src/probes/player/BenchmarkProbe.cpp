#include <QApplication>
#include <QCheckBox>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>

#include <QTimer>
#include <QDir>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

#include "ProbeUi.hpp"
#include "ProbeRuntime.hpp"

#include "backend/MikaiClient.hpp"
#include "dialogs/SettingsDialog.hpp"
#include "workers/Runner.hpp"

using mikai::app::Runner;
using mikai::app::SettingsDialog;
using mikai::backend::MikaiClient;

namespace {

int failures = 0;

struct ModelEntry {
    QString id;
    QString label;
    QString group;
};

struct ClipEntry {
    QString file;
    QString label;
};

// Use sampler budgets so first-use engine builds are not truncated.
constexpr qint64 kEngineBuildBudgetMs = 600000;
constexpr qint64 kStartupBudgetMs = 20000;
constexpr qint64 kSampleBudgetMs = 3000;
constexpr qint64 kPairBudgetMs = kEngineBuildBudgetMs + kStartupBudgetMs + kSampleBudgetMs;

void check(bool ok, const char* what) {
    std::fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

bool waitForUi(const std::function<bool()>& predicate, qint64 timeoutMs) {
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

double parseFps(const QString& text);

// Table labels must map unambiguously to the selected stable model IDs.
bool writeEvidence(const QString& path, bool complete, const QVector<ModelEntry>& models,
                   const QVector<ClipEntry>& clips, const QHash<QString, ModelEntry>& modelByLabel,
                   const QTableWidget* results) {
    const QString baselineLabel = SettingsDialog::tr("Baseline");
    QHash<QString, QString> clipFileByLabel;
    for (const ClipEntry& clip : clips) {
        if (clipFileByLabel.contains(clip.label)) {
            std::fprintf(stderr, "FAIL ambiguous clip label \"%s\"\n", qPrintable(clip.label));
            return false;
        }
        clipFileByLabel.insert(clip.label, clip.file);
    }

    QJsonArray modelArray;
    for (const ModelEntry& model : models) {
        modelArray.append(QJsonObject{{QStringLiteral("id"), model.id},
                                      {QStringLiteral("label"), model.label},
                                      {QStringLiteral("group"), model.group}});
    }
    QJsonArray clipArray;
    for (const ClipEntry& clip : clips) {
        clipArray.append(QJsonObject{{QStringLiteral("file"), clip.file},
                                     {QStringLiteral("label"), clip.label}});
    }

    const int rowCount = results != nullptr ? results->rowCount() : 0;
    QJsonArray resultArray;
    for (int row = 0; row < rowCount; ++row) {
        const QTableWidgetItem* modelItem = results->item(row, 0);
        const QTableWidgetItem* clipItem = results->item(row, 1);
        const QTableWidgetItem* resultItem = results->item(row, 2);
        const QString modelLabel = modelItem != nullptr ? modelItem->text() : QString();
        const QString clipLabel = clipItem != nullptr ? clipItem->text() : QString();
        ModelEntry model;
        if (modelLabel == baselineLabel) {
            model = ModelEntry{QStringLiteral("baseline"), modelLabel, QStringLiteral("Baseline")};
        } else if (modelByLabel.contains(modelLabel)) {
            model = modelByLabel.value(modelLabel);
        } else {
            std::fprintf(stderr,
                         "FAIL result row %d names model \"%s\", which no selected tree entry maps\n",
                         row, qPrintable(modelLabel));
            return false;
        }
        if (!clipFileByLabel.contains(clipLabel)) {
            std::fprintf(stderr,
                         "FAIL result row %d names clip \"%s\", which no selected clip maps\n", row,
                         qPrintable(clipLabel));
            return false;
        }
        const QString reason =
            resultItem != nullptr ? resultItem->data(Qt::UserRole).toString() : QString();
        const double fps = resultItem != nullptr ? parseFps(resultItem->text()) : -1.0;
        const bool measured = reason.isEmpty() && std::isfinite(fps) && fps > 0.0;
        resultArray.append(
            QJsonObject{{QStringLiteral("modelId"), model.id},
                        {QStringLiteral("model"), model.label},
                        {QStringLiteral("group"), model.group},
                        {QStringLiteral("clip"), clipLabel},
                        {QStringLiteral("clipFile"), clipFileByLabel.value(clipLabel)},
                        {QStringLiteral("fps"), measured ? QJsonValue(fps) : QJsonValue()},
                        {QStringLiteral("reason"), reason}});
    }

    QJsonObject root{{QStringLiteral("complete"), complete},
                     {QStringLiteral("models"), modelArray},
                     {QStringLiteral("clips"), clipArray},
                     {QStringLiteral("results"), resultArray}};

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "FAIL --output %s cannot be opened: %s\n", qPrintable(path),
                     qPrintable(file.errorString()));
        return false;
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) {
        file.cancelWriting();
        std::fprintf(stderr, "FAIL --output %s could not be written in full: %s\n",
                     qPrintable(path), qPrintable(file.errorString()));
        return false;
    }
    if (!file.commit()) {
        std::fprintf(stderr, "FAIL --output %s could not be committed: %s\n", qPrintable(path),
                     qPrintable(file.errorString()));
        return false;
    }
    std::printf("benchmark evidence written to %s\n", qPrintable(path));
    std::fflush(stdout);
    return true;
}

double parseFps(const QString& text) {
    static const QRegularExpression numRe(QStringLiteral("^[-+]?[0-9]*[.,]?[0-9]+"));
    auto m = numRe.match(text);
    if (m.hasMatch()) {
        QString num = m.captured(0).replace(',', '.');
        bool ok = false;
        double v = num.toDouble(&ok);
        if (ok) return v;
    }
    bool ok = false;
    double v = text.toDouble(&ok);
    return ok ? v : -1.0;
}

}  // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QApplication app(argc, argv);
    if (!mikai::probes::initializeUi(app)) {
        return 2;
    }

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption modelOpt(QStringLiteral("model"),
                                QStringLiteral("catalogue id, comma separated for several"),
                                QStringLiteral("id"));
    parser.addOption(modelOpt);
    QCommandLineOption sizeOpt(QStringLiteral("size"), QStringLiteral("WIDTHxHEIGHT e.g. 1024x768"), QStringLiteral("size"));
    parser.addOption(sizeOpt);
    QCommandLineOption clipOpt(QStringLiteral("clip"), QStringLiteral("clip file"), QStringLiteral("file"));
    parser.addOption(clipOpt);
    QCommandLineOption expectFailOpt(QStringLiteral("expect-failure"), QStringLiteral("expect explicit unacquired model row to show Failed with UserRole reason (baseline still positive)"));
    parser.addOption(expectFailOpt);
    QCommandLineOption listOpt(QStringLiteral("list-models"),
                               QStringLiteral("print every catalogue model as group<TAB>id<TAB>label and exit"));
    parser.addOption(listOpt);
    QCommandLineOption allModelsOpt(
        QStringLiteral("all-models"),
        QStringLiteral("check every catalogue model, baseline included in the run"));
    parser.addOption(allModelsOpt);
    QCommandLineOption allClipsOpt(QStringLiteral("all-clips"),
                                   QStringLiteral("check every bundled clip"));
    parser.addOption(allClipsOpt);
    QCommandLineOption outputOpt(QStringLiteral("output"),
                                 QStringLiteral("write the measured matrix to this path as UTF-8 JSON"),
                                 QStringLiteral("file"));
    parser.addOption(outputOpt);
    parser.process(app);

    QStringList requestedModels;
    if (parser.isSet(modelOpt)) {
        requestedModels = parser.value(modelOpt).split(QLatin1Char(','), Qt::SkipEmptyParts);
    }
    const bool expectFailure = parser.isSet(expectFailOpt);
    const bool allModels = parser.isSet(allModelsOpt);
    const bool allClips = parser.isSet(allClipsOpt);
    if (allModels && parser.isSet(modelOpt)) {
        std::fprintf(stderr, "FAIL --all-models cannot be combined with --model\n");
        return 2;
    }
    if (allClips && parser.isSet(clipOpt)) {
        std::fprintf(stderr, "FAIL --all-clips cannot be combined with --clip\n");
        return 2;
    }
    QString outputPath;
    if (parser.isSet(outputOpt)) {
        outputPath = parser.value(outputOpt);
        // Fail before a long matrix if its evidence cannot be saved.
        QSaveFile destination(outputPath);
        if (!destination.open(QIODevice::WriteOnly)) {
            std::fprintf(stderr, "FAIL --output %s cannot be opened: %s\n", qPrintable(outputPath),
                         qPrintable(destination.errorString()));
            return 2;
        }
        destination.cancelWriting();
    }

    QSize requestedSize;
    if (parser.isSet(sizeOpt)) {
        const QString s = parser.value(sizeOpt).trimmed();
        const int x = s.indexOf('x');
        if (x > 0) {
            bool wOk=false, hOk=false;
            int w = s.left(x).toInt(&wOk);
            int h = s.mid(x+1).toInt(&hOk);
            if (wOk && hOk && w>0 && h>0) requestedSize = QSize(w, h);
        }
    }
    if (parser.isSet(sizeOpt) && !requestedSize.isValid()) {
        std::fprintf(stderr, "FAIL invalid --size\n");
        return 2;
    }

    QString targetClip = QStringLiteral("480x360.mp4");
    if (parser.isSet(clipOpt)) targetClip = parser.value(clipOpt);

    QCoreApplication::setOrganizationName(QStringLiteral("MikaiBenchmarkProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("BenchmarkProbe"));

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());

    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }

    MikaiClient client(QStringLiteral("fixture-not-an-api-key"));
    Runner runner;

    SettingsDialog dialog(client, runner);
    const QSize minSize = dialog.minimumSize();
    QSize useSize = requestedSize.isValid() ? requestedSize : minSize;
    useSize = mikai::probes::windowSize(useSize);
    dialog.resize(useSize);
    dialog.show();
    QApplication::processEvents();

    check(dialog.size() == useSize || !requestedSize.isValid(), "dialog sized as requested or minimum");
    check(mikai::probes::dialogActionsFit(dialog, useSize), "footer actions visible at size");

    auto* tabs = dialog.findChild<QTabWidget*>();
    check(tabs != nullptr, "settings tabs present");
    if (!tabs) return 1;

    auto* benchmarkScroll = dialog.findChild<QScrollArea*>(QStringLiteral("benchmarkScroll"));
    check(benchmarkScroll != nullptr, "benchmarkScroll found");
    if (!benchmarkScroll) return 1;

    int benchIdx = -1;
    for (int i = 0; i < tabs->count(); ++i) {
        QWidget* page = tabs->widget(i);
        if (page == benchmarkScroll || page->isAncestorOf(benchmarkScroll)) {
            benchIdx = i;
            break;
        }
    }
    check(benchIdx >= 0, "benchmark tab selected by ancestor of benchmarkScroll");
    if (benchIdx >= 0) {
        tabs->setCurrentIndex(benchIdx);
    }
    QApplication::processEvents();

    if (benchmarkScroll) {
        QScrollBar* hbar = benchmarkScroll->horizontalScrollBar();
        check(hbar->minimum() == hbar->maximum(), "no horizontal scrolling on benchmark page");
        benchmarkScroll->verticalScrollBar()->setValue(benchmarkScroll->verticalScrollBar()->minimum());
        QApplication::processEvents();
        QPushButton* runTop = nullptr;
        for (auto* b : dialog.findChildren<QPushButton*>()) {
            if (b->property("benchmarkRunAction").toBool()) { runTop = b; break; }
        }
        if (runTop) check(runTop->isVisibleTo(&dialog), "run action visible at scroll top");
        benchmarkScroll->verticalScrollBar()->setValue(benchmarkScroll->verticalScrollBar()->maximum());
        QApplication::processEvents();
        if (runTop) check(runTop->isVisibleTo(&dialog), "run action visible at scroll bottom");
    }

    auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("benchmarkModelTree"));
    check(tree != nullptr, "benchmarkModelTree present");

    if (tree != nullptr && parser.isSet(listOpt)) {
        for (int g = 0; g < tree->topLevelItemCount(); ++g) {
            auto* group = tree->topLevelItem(g);
            for (int c = 0; c < group->childCount(); ++c) {
                auto* leaf = group->child(c);
                std::printf("%s\t%s\t%s\n", qPrintable(group->text(0)),
                            qPrintable(leaf->data(0, Qt::UserRole + 1).toString()),
                            qPrintable(leaf->text(0)));
            }
        }
        std::fflush(stdout);
        return 0;
    }

    if (tree) {
        for (int g = 0; g < tree->topLevelItemCount(); ++g) {
            auto* group = tree->topLevelItem(g);
            for (int c = 0; c < group->childCount(); ++c) {
                group->child(c)->setCheckState(0, Qt::Unchecked);
            }
            group->setCheckState(0, Qt::Unchecked);
        }
    }

    QVector<ModelEntry> chosenModels;
    if (tree) {
        for (int g = 0; g < tree->topLevelItemCount(); ++g) {
            auto* group = tree->topLevelItem(g);
            for (int c = 0; c < group->childCount(); ++c) {
                auto* leaf = group->child(c);
                const QString id = leaf->data(0, Qt::UserRole + 1).toString();
                const bool wanted =
                    allModels || (requestedModels.isEmpty()
                                      ? id.startsWith(QLatin1String("anime4k:"))
                                      : requestedModels.contains(id));
                if (wanted) {
                    leaf->setCheckState(0, Qt::Checked);
                    chosenModels.append(ModelEntry{id, leaf->text(0), group->text(0)});
                }
            }
        }
    }
    check(!chosenModels.isEmpty(), "every requested model leaf is selected");
    bool hasCheckedLeaf = false;
    if (tree) {
        for (int g = 0; g < tree->topLevelItemCount() && !hasCheckedLeaf; ++g) {
            auto* group = tree->topLevelItem(g);
            for (int c = 0; c < group->childCount(); ++c) {
                if (group->child(c)->checkState(0) == Qt::Checked) {
                    hasCheckedLeaf = true;
                    break;
                }
            }
        }
    }
    check(hasCheckedLeaf, "model leaf checkState set as user would");
    for (const QString& requested : requestedModels) {
        if (std::none_of(chosenModels.cbegin(), chosenModels.cend(),
                         [&requested](const ModelEntry& entry) { return entry.id == requested; })) {
            std::fprintf(stderr, "FAIL --model %s not found\n", qPrintable(requested));
            return 2;
        }
    }
    // The result table has labels, not stable IDs.
    QHash<QString, ModelEntry> modelByLabel;
    for (const ModelEntry& entry : chosenModels) {
        if (modelByLabel.contains(entry.label)) {
            std::fprintf(stderr, "FAIL ambiguous model label \"%s\" for ids %s and %s\n",
                         qPrintable(entry.label), qPrintable(modelByLabel.value(entry.label).id),
                         qPrintable(entry.id));
            return 2;
        }
        modelByLabel.insert(entry.label, entry);
    }

    QVector<ClipEntry> chosenClips;
    for (auto* cb : dialog.findChildren<QCheckBox*>()) {
        if (cb->objectName() != QStringLiteral("benchmarkClipCheck")) continue;
        const QString name = cb->property("benchmarkClipName").toString();
        const bool wanted = allClips || name == targetClip;
        cb->setChecked(wanted);
        if (wanted) chosenClips.append(ClipEntry{name, cb->text()});
    }
    const bool clipFound = !chosenClips.isEmpty();
    check(clipFound, allClips ? "every bundled clip is selected" : "target clip checkbox present");
    if (!clipFound) {
        if (allClips) {
            std::fprintf(stderr, "FAIL no bundled clip checkbox found\n");
        } else {
            std::fprintf(stderr, "FAIL --clip checkbox absent\n");
        }
        return 2;
    }

    auto* rawBox = dialog.findChild<QCheckBox*>(QStringLiteral("benchmarkRawBox"));
    if (rawBox) rawBox->setChecked(true);

    QApplication::processEvents();

    QPushButton* runButton = nullptr;
    for (auto* b : dialog.findChildren<QPushButton*>()) {
        if (b->property("benchmarkRunAction").toBool()) {
            runButton = b;
            break;
        }
    }
    check(runButton != nullptr && runButton->isEnabled(), "run button present and enabled");

    auto* results = dialog.findChild<QTableWidget*>(QStringLiteral("benchmarkResults"));
    check(results != nullptr, "benchmarkResults table present");

    benchmarkScroll->verticalScrollBar()->setValue(benchmarkScroll->verticalScrollBar()->minimum());
    QApplication::processEvents();
    const QString beforeTop = QDir::current().absoluteFilePath("benchmark-before-top.png");
    check(dialog.grab().save(beforeTop), "before screenshot at scroll top");
    benchmarkScroll->verticalScrollBar()->setValue(benchmarkScroll->verticalScrollBar()->maximum());
    QApplication::processEvents();
    const QString beforeBottom = QDir::current().absoluteFilePath("benchmark-before-bottom.png");
    check(dialog.grab().save(beforeBottom), "before screenshot at scroll bottom");
    benchmarkScroll->verticalScrollBar()->setValue(benchmarkScroll->verticalScrollBar()->minimum());
    QApplication::processEvents();

    if (runButton) runButton->click();
    QApplication::processEvents();

    const qint64 expectedRows =
        (static_cast<qint64>(chosenModels.size()) + 1) * static_cast<qint64>(chosenClips.size());
    const qint64 deadlineMs = kPairBudgetMs * expectedRows;
    std::fprintf(stderr, "[benchmark_probe] expected rows=%lld deadline=%lld ms\n",
                 static_cast<long long>(expectedRows), static_cast<long long>(deadlineMs));
    bool completed = waitForUi([&] {
        return runButton && runButton->isEnabled() && results &&
               static_cast<qint64>(results->rowCount()) >= expectedRows;
    }, deadlineMs);
    check(completed, "run completed within the deadline, button reenabled, rows present");

    if (results) {
        mikai::probes::revealWidget(results);
        int selectRow = 0;
        QString failRole;
        if (expectFailure && results->rowCount() > 0) {
            for (int r = 0; r < results->rowCount(); ++r) {
                QTableWidgetItem* resItem = results->item(r, 2);
                QString role = resItem ? resItem->data(Qt::UserRole).toString() : QString();
                if (!role.isEmpty()) {
                    selectRow = r;
                    failRole = role;
                    break;
                }
            }
        }
        if (results->rowCount() > selectRow) {
            results->selectRow(selectRow);
            QApplication::processEvents();
        }
        if (expectFailure && !failRole.isEmpty()) {
            auto* detail = dialog.findChild<QLabel*>(QStringLiteral("benchmarkResultDetail"));
            const QRect visibleArea = benchmarkScroll->viewport()->rect();
            const QRect detailBounds(
                detail ? detail->mapTo(benchmarkScroll->viewport(), QPoint()) : QPoint(),
                detail ? detail->size() : QSize());
            check(detail != nullptr && detail->isVisibleTo(&dialog) &&
                      visibleArea.contains(detailBounds),
                  "failure explanation is inside the visible viewport after row selection");
            check(detail != nullptr && detail->text() == failRole, "detail label text equals UserRole reason");
        }
    }
    QApplication::processEvents();

    const QString afterShot = QDir::current().absoluteFilePath("benchmark-completed.png");
    check(dialog.grab().save(afterShot), "completed screenshot to cwd after reveal/select");
    if (expectFailure) {
        const QString detailShot = QDir::current().absoluteFilePath("benchmark-detail.png");
        check(dialog.grab().save(detailShot), "detail screenshot with selected fail row");
    }

    if (results) {
        int passCount = 0;
        int failCount = 0;
        for (int r = 0; r < results->rowCount(); ++r) {
            QString model = results->item(r, 0) ? results->item(r, 0)->text() : QString();
            QString clip = results->item(r, 1) ? results->item(r, 1)->text() : QString();
            QString thr = results->item(r, 2) ? results->item(r, 2)->text() : QString();
            double fps = parseFps(thr);
            QTableWidgetItem* resItem = results->item(r, 2);
            QString reason = resItem ? resItem->data(Qt::UserRole).toString() : QString();
            bool valid = std::isfinite(fps) && fps > 0.0;
            std::fprintf(stderr, "row %d: model=%s clip=%s throughput=%s numeric=%.2f valid=%d\n",
                         r, qPrintable(model), qPrintable(clip), qPrintable(thr), fps, int(valid));
            if (!reason.isEmpty()) {
                std::fprintf(stderr, "        reason: %s\n",
                             qPrintable(QString(reason).replace(QLatin1Char('\n'), QStringLiteral(" | "))));
            }
            if (expectFailure) {
                if (!reason.isEmpty()) {
                    ++failCount;
                } else {
                    if (valid) ++passCount;
                }
            } else {
                if (!valid) ++failures;
                else ++passCount;
            }
        }
        if (expectFailure) {
            check(passCount >= 1 && failCount >= 1, "with --expect-failure: baseline positive, model Failed with reason");
        }
    }

    if (benchmarkScroll) {
        QScrollBar* hbar = benchmarkScroll->horizontalScrollBar();
        check(hbar->minimum() == hbar->maximum(), "still no horizontal scrolling after run");
    }

    if (!outputPath.isEmpty() && !writeEvidence(outputPath, completed, chosenModels, chosenClips,
                                                modelByLabel, results)) {
        return 2;
    }

    std::fprintf(stderr, "[benchmark_probe] %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
