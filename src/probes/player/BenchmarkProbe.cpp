#include <QApplication>
#include <QCheckBox>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QOffscreenSurface>
#include <QUrl>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
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
#include "dialogs/settings/BenchmarkPlayback.hpp"
#include "dialogs/settings/BenchmarkReport.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "native/status/NativeStatus.hpp"
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

int verifyDiagnosticLogs(const QString& clip) {
    QTemporaryDir fixtures;
    check(fixtures.isValid(), "diagnostic fixtures have an isolated directory");
    if (!fixtures.isValid()) return 2;
    QOffscreenSurface surface;
    surface.create();
    check(surface.isValid(), "diagnostic sampler has an OpenGL surface");
    if (!surface.isValid()) return 2;

    for (const bool fatal : {false, true}) {
        const QString scriptPath = fixtures.filePath(
            fatal ? QStringLiteral("diagnostic_fatal.lua") : QStringLiteral("diagnostic_healthy.lua"));
        const QString missing = fixtures.filePath(QStringLiteral("missing-diagnostic-source.mp4"));
        const QByteArray action = fatal
            ? QByteArray("mp.commandv('loadfile', '") +
                  QUrl::fromLocalFile(missing).toEncoded() + "')"
            : QByteArray("mp.msg.info('Configured slot 1003: 640x360 -> 1280x720 (depth 3)')");
        // Script messages exercise the build-log policy without compiling an engine.
        const QByteArray script = R"lua(
local once = false
mp.register_event('file-loaded', function()
    if once then return end
    once = true
    mp.msg.info('building TensorRT engine for diagnostic-fixture')
    mp.add_timeout(0.2, function()
        mp.msg.error('diagnostic A')
        mp.msg.error('diagnostic B')
        mp.msg.error('diagnostic C')
        mp.msg.error('diagnostic D')
        mp.msg.error('diagnostic D')
    end)
    mp.add_timeout(0.4, function()
)lua" + action + R"lua(
    end)
end)
)lua";
        QSaveFile file(scriptPath);
        const bool saved = file.open(QIODevice::WriteOnly) &&
                           file.write(script) == script.size() && file.commit();
        check(saved, "diagnostic Lua fixture was written");
        if (!saved) return 2;

        bool building = false;
        bool failed = false;
        const auto result = mikai::app::runBenchmarkSample(
            clip, QSize(640, 360),
            {{"scripts", scriptPath.toStdString()}},
            mplayer::native::Backend::TensorRT, 1003, surface, {},
            [&](mikai::app::BenchmarkEngineBuildUpdate update, const QString&) {
                if (update == mikai::app::BenchmarkEngineBuildUpdate::Started) building = true;
                if (update == mikai::app::BenchmarkEngineBuildUpdate::Failed) failed = true;
            });
        check(building, "diagnostic fixture entered the build-wait state");
        if (!fatal) {
            check(!failed && result.error.isEmpty() && std::isfinite(result.fps) && result.fps > 0,
                  "error-level chatter during a build still yields measured video throughput");
            if (!result.error.isEmpty())
                std::fprintf(stderr, "diagnostic healthy reason: %s\n", qPrintable(result.error));
            continue;
        }
        std::fprintf(stderr, "diagnostic fatal reason: %s\n", qPrintable(result.error));
        const QStringList lines = result.error.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const QString buildFailure = QCoreApplication::translate(
            "mikai::app::BenchmarkPlayback", "TensorRT engine build failed.");
        check(failed && result.fps <= 0 &&
                  result.error.contains(QStringLiteral("missing-diagnostic-source.mp4")),
              "an actual source error aborts and retains its original cause");
        check(!lines.isEmpty() && lines.front() == buildFailure,
              "a fatal build-wait failure has one build-failure lead sentence");
        check(lines.size() <= 4 && lines.count(buildFailure) == 1,
              "the failure reason contains at most three diagnostic tail lines");
        QStringList unique = lines;
        unique.removeDuplicates();
        check(unique.size() == lines.size(), "the failure reason has no duplicated tail lines");
    }
    return failures == 0 ? 0 : 1;
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

int verifyBenchmarkSourceErrors(MikaiClient& client, Runner& runner) {
    const QDir clips(QCoreApplication::applicationDirPath() + QStringLiteral("/benchmarks"));
    QTemporaryDir fixture(clips.filePath(QStringLiteral("metadata-error-XXXXXX")));
    if (!fixture.isValid()) return 2;
    const QString corruptPath = QDir(fixture.path()).filePath(QStringLiteral("corrupt.mp4"));
    QSaveFile corrupt(corruptPath);
    if (!corrupt.open(QIODevice::WriteOnly) ||
        corrupt.write("not a media container") != 21 || !corrupt.commit()) return 2;
    QString sourceError;
    mikai::app::readBenchmarkSourceContext(corruptPath, &sourceError);
    check(!sourceError.isEmpty(), "metadata: libmpv refuses the real corrupt source");

    SettingsDialog dialog(client, runner);
    dialog.show();
    auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("benchmarkModelTree"));
    auto* run = dialog.findChild<QPushButton*>(QStringLiteral("benchmarkRunButton"));
    auto* results = dialog.findChild<QTableWidget*>(QStringLiteral("benchmarkResults"));
    auto* baseline = dialog.findChild<QCheckBox*>(QStringLiteral("benchmarkRawBox"));
    if (!tree || !run || !results || !baseline) return 2;
    bool shaderSelected = false;
    for (int groupIndex = 0; groupIndex < tree->topLevelItemCount(); ++groupIndex) {
        auto* group = tree->topLevelItem(groupIndex);
        group->setCheckState(0, Qt::Unchecked);
        for (int index = 0; index < group->childCount(); ++index) {
            auto* model = group->child(index);
            if (model->data(0, Qt::UserRole + 1).toString() == QLatin1String("anime4k:fast_a")) {
                model->setCheckState(0, Qt::Checked);
                shaderSelected = true;
            }
        }
    }
    check(shaderSelected, "metadata: baseline and a real Anime4K preset are selected");
    baseline->setChecked(true);
    bool firstClip = true;
    for (auto* clip : dialog.findChildren<QCheckBox*>(QStringLiteral("benchmarkClipCheck"))) {
        clip->setChecked(firstClip);
        if (firstClip) clip->setProperty("benchmarkClipName", clips.relativeFilePath(corruptPath));
        firstClip = false;
    }
    const QDir reports(QDir(mikai::app::benchmarkReportBaseDirectory()).filePath(QStringLiteral("bench")));
    const auto before = reports.entryList({QStringLiteral("benchmark-*.json")}, QDir::Files);
    run->click();
    check(waitForUi([&] { return run->isEnabled(); }, 10000),
          "metadata: a failed source does not leave the controls disabled");
    check(results->rowCount() == 2, "metadata: both selected rows report the failure");
    for (int row = 0; row < results->rowCount(); ++row) {
        const auto* item = results->item(row, 2);
        check(item && item->data(Qt::UserRole).toString() == sourceError,
              "metadata: the table preserves the libmpv source failure");
    }
    QStringList added = reports.entryList({QStringLiteral("benchmark-*.json")}, QDir::Files);
    for (const auto& name : before) added.removeAll(name);
    check(added.size() == 1, "metadata: exactly one failed-run report is saved");
    if (added.size() == 1) {
        QFile report(reports.filePath(added.first()));
        if (!report.open(QIODevice::ReadOnly)) return 2;
        const auto rows = QJsonDocument::fromJson(report.readAll()).object().value(
            QStringLiteral("results")).toArray();
        check(rows.size() == 2, "metadata: the report includes both failed rows");
        for (const auto& value : rows) {
            const auto row = value.toObject();
            check(row.value(QStringLiteral("fps")).isNull() &&
                      row.value(QStringLiteral("failureReason")).toString() == sourceError,
                  "metadata: the JSON retains the cause and contains no false FPS");
        }
    }
    return failures == 0 ? 0 : 1;
}

int verifyBenchmarkLifecycle(MikaiClient& client, Runner& runner) {
    // Probe executables do not embed the application's Ukrainian catalog.
    QSettings().setValue(QStringLiteral("ui/language"), QStringLiteral("en"));
    SettingsDialog dialog(client, runner);
    dialog.show();
    auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("benchmarkModelTree"));
    auto* run = dialog.findChild<QPushButton*>(QStringLiteral("benchmarkRunButton"));
    auto* activity = dialog.findChild<QLabel*>(QStringLiteral("benchmarkActivity"));
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("benchmarkRunStatus"));
    auto* results = dialog.findChild<QTableWidget*>(QStringLiteral("benchmarkResults"));
    auto* baseline = dialog.findChild<QCheckBox*>(QStringLiteral("benchmarkRawBox"));
    if (!tree || !run || !activity || !status || !results || !baseline) return 2;
    for (int group = 0; group < tree->topLevelItemCount(); ++group)
        tree->topLevelItem(group)->setCheckState(0, Qt::Unchecked);
    baseline->setChecked(true);
    int clipCount = 0;
    for (auto* clip : dialog.findChildren<QCheckBox*>(QStringLiteral("benchmarkClipCheck")))
        clip->setChecked(clipCount++ < 2);

    const QDir reports(QDir(mikai::app::benchmarkReportBaseDirectory()).filePath(QStringLiteral("bench")));
    const auto reportNames = [&] {
        return reports.entryList({QStringLiteral("benchmark-*.json")}, QDir::Files);
    };
    const auto before = reportNames();
    run->click();
    check(waitForUi([&] {
        return activity->text().contains(QStringLiteral("Measuring throughput"));
    }, 20000), "lifecycle: a real sample starts before closing Settings");

    QElapsedTimer cancellationTime;
    cancellationTime.start();
    dialog.reject();
    check(!dialog.isVisible(), "lifecycle: reject hides the still-live dialog");
    check(waitForUi([&] { return run->isEnabled(); }, 2000),
          "lifecycle: reject cancels sampling without waiting for dialog destruction");
    check(status->text().contains(QStringLiteral("cancel"), Qt::CaseInsensitive),
          "lifecycle: a canceled run is not reported as complete");
    check(results->rowCount() <= 1, "lifecycle: cancellation does not start the second clip");
    check(waitForUi([&] { return reportNames().size() == before.size() + 1; }, 5000),
          "lifecycle: closing writes one partial report");

    // A failed cancellation check must still leave no background work behind.
    waitForUi([&] { return run->isEnabled(); }, 20000);
    dialog.show();
    run->click();
    check(waitForUi([&] {
        return activity->text().contains(QStringLiteral("Measuring throughput")) &&
               !run->isEnabled();
    }, 20000), "lifecycle: the same dialog can start a new run after cancellation");
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    QPushButton* save = nullptr;
    if (buttons) {
        for (auto* button : buttons->findChildren<QPushButton*>()) {
            if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole) save = button;
        }
    }
    check(save != nullptr, "lifecycle: the real Settings Save action exists");
    if (save) save->click();
    check(!dialog.isVisible() && dialog.result() == QDialog::Accepted,
          "lifecycle: Save accepts the active dialog");
    check(waitForUi([&] { return run->isEnabled(); }, 2000) &&
              status->text().contains(QStringLiteral("cancel"), Qt::CaseInsensitive),
          "lifecycle: Save also cancels sampling without destroying Settings");
    check(waitForUi([&] { return reportNames().size() == before.size() + 2; }, 5000),
          "lifecycle: Save retains the canceled run's report");
    waitForUi([&] { return run->isEnabled(); }, 20000);
    dialog.show();
    run->click();
    check(waitForUi([&] {
        return activity->text().contains(QStringLiteral("Measuring throughput")) &&
               !run->isEnabled();
    }, 20000), "lifecycle: another real sample starts before application shutdown");

    int heartbeat = 0;
    QTimer timer;
    timer.setInterval(1);
    QObject::connect(&timer, &QTimer::timeout, [&] { ++heartbeat; });
    timer.start();
    bool drained = false;
    Runner::shutdownAll([&] { drained = true; });
    check(waitForUi([&] { return drained; }, 5000),
          "lifecycle: application shutdown drains active benchmark work");
    check(heartbeat > 0, "lifecycle: the GUI continues processing during shutdown");
    check(reportNames().size() == before.size() + 3,
          "lifecycle: shutdown finishes the benchmark report before reporting drained");
    timer.stop();
    waitForUi([&] { return reportNames().size() >= before.size() + 3; }, 20000);
    std::fprintf(stderr, "[benchmark lifecycle] %s (%d failures, cancel/drain %lld ms)\n",
                 failures == 0 ? "PASS" : "FAIL", failures,
                 static_cast<long long>(cancellationTime.elapsed()));
    return failures == 0 ? 0 : 1;
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
    QCommandLineOption diagnosticsOpt(
        QStringLiteral("diagnostic-log-smoke"),
        QStringLiteral("exercise benign build chatter and a real source error, without inference"),
        QStringLiteral("video"));
    parser.addOption(diagnosticsOpt);
    QCommandLineOption lifecycleOpt(
        QStringLiteral("lifecycle-smoke"),
        QStringLiteral("exercise close/reopen and application shutdown during real sampling"));
    parser.addOption(lifecycleOpt);
    QCommandLineOption sourceErrorOpt(
        QStringLiteral("source-error-smoke"),
        QStringLiteral("verify corrupt source causes in baseline and Anime4K rows and JSON"));
    parser.addOption(sourceErrorOpt);
    parser.process(app);
    if (parser.isSet(diagnosticsOpt)) {
        if (!mplayer::probes::selectRuntime()) return 2;
        return verifyDiagnosticLogs(parser.value(diagnosticsOpt));
    }

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
    if (parser.isSet(lifecycleOpt)) return verifyBenchmarkLifecycle(client, runner);
    if (parser.isSet(sourceErrorOpt)) return verifyBenchmarkSourceErrors(client, runner);

    SettingsDialog dialog(client, runner);
    const QSize minSize = dialog.minimumSize();
    QSize useSize = requestedSize.isValid() ? requestedSize : minSize;
    useSize = mikai::probes::windowSize(useSize);
    dialog.resize(useSize);
    dialog.show();
    QApplication::processEvents();

#ifdef Q_OS_LINUX
    auto* runtimeChoice = dialog.findChild<QComboBox*>(QStringLiteral("runtimeChoiceCombo"));
    check(runtimeChoice != nullptr &&
              runtimeChoice->findData(static_cast<int>(mplayer::native::RuntimeChoice::Native)) < 0 &&
              runtimeChoice->currentData().toInt() ==
                  static_cast<int>(mplayer::native::RuntimeChoice::Standard),
          "Linux settings offer only the Standard runtime");
    auto* backendChoice = dialog.findChild<QComboBox*>(QStringLiteral("animeJanaiRuntimeBackend"));
    check(backendChoice != nullptr &&
              backendChoice->findData(static_cast<int>(mplayer::native::Backend::DirectML)) < 0 &&
              backendChoice->currentData().toInt() ==
                  static_cast<int>(mplayer::native::Backend::TensorRT),
          "Linux component acquisition offers TensorRT rather than DirectML");
#endif

    check(dialog.size() == useSize || !requestedSize.isValid(), "dialog sized as requested or minimum");
    check(mikai::probes::dialogActionsFit(dialog, useSize), "footer actions visible at size");

    const bool benchmarkPageOpened = dialog.showPage(QStringLiteral("benchmark"));
    check(benchmarkPageOpened, "benchmark page selected");
    if (!benchmarkPageOpened) return 1;

    auto* benchmarkScroll = dialog.findChild<QScrollArea*>(QStringLiteral("benchmarkScroll"));
    check(benchmarkScroll != nullptr, "benchmarkScroll found");
    if (!benchmarkScroll) return 1;
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

    const QDir reportDirectory(
        QDir(mikai::app::benchmarkReportBaseDirectory()).filePath(QStringLiteral("bench")));
    const QStringList reportsBefore = reportDirectory.entryList(
        {QStringLiteral("benchmark-*.json")}, QDir::Files);
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
    if (completed) {
        QStringList newReports = reportDirectory.entryList(
            {QStringLiteral("benchmark-*.json")}, QDir::Files);
        for (const QString& oldReport : reportsBefore) newReports.removeAll(oldReport);
        check(newReports.size() == 1, "completion automatically writes exactly one benchmark JSON");
        if (newReports.size() == 1) {
            QFile report(reportDirectory.filePath(newReports.first()));
            check(report.open(QIODevice::ReadOnly), "the automatic report can be read");
            const QJsonDocument document = QJsonDocument::fromJson(report.readAll());
            const QJsonArray rows = document.object().value(QStringLiteral("results")).toArray();
            check(document.isObject() && rows.size() == expectedRows,
                  "the automatic report contains every measured or failed row");
            for (const QJsonValue& value : rows) {
                const QJsonObject row = value.toObject();
                check(!row.value(QStringLiteral("modelId")).toString().isEmpty(),
                      "automatic report rows retain stable model identifiers");
                check(row.value(QStringLiteral("fps")).isDouble() ||
                          (row.value(QStringLiteral("fps")).isNull() &&
                           !row.value(QStringLiteral("failureReason")).toString().isEmpty()),
                      "automatic report throughput is numeric or has an explicit failure cause");
            }
        }
    }

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
