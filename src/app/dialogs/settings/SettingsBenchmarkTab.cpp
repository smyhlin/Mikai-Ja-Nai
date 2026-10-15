#include "dialogs/SettingsDialog.hpp"

#include "dialogs/settings/SettingsUi.hpp"
#include "theme/Metrics.hpp"

#include <native/components/AnimeJanaiRuntime.hpp>
#include <native/models/NativeRife.hpp>
#include <native/models/NativeSpatialAssets.hpp>
#include <player/Anime4K.hpp>
#include <player/MpvHandle.hpp>

#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QThread>
#include <QVBoxLayout>

#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mikai::app {
namespace {

/// The stable catalogue key a model row carries: "anime4k:<id>" for a shader
/// preset, or the AnimeJaNai preset id ("builtin1001", "spatial:<sha>",
/// "rife:<file>") for a filter model. A row's data is what the run resolves,
/// never its label.
constexpr auto kModelRole = Qt::UserRole + 1;
constexpr auto kClipRole = Qt::UserRole + 2;

}  // namespace

QWidget* SettingsDialog::buildBenchmarkTab() {
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("benchmarkScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* page = new QWidget(scroll);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(theme::kSpacingSmall, theme::kSpacingMedium, theme::kSpacingSmall,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    benchmarkHeading_ = new QLabel(tr("Playback Benchmark"), page);
    benchmarkHeading_->setObjectName(QStringLiteral("heading"));
    yieldCaptionWidth(benchmarkHeading_);
    root->addWidget(benchmarkHeading_);

    auto* desc = new QLabel(
        tr("Runs the real mpv pipeline offscreen (vo=null) over the bundled clips and reports "
           "decode + enhancement throughput in frames per second. Tick any number of clips and "
           "models; every selected clip is measured with every selected model. A model that "
           "cannot be resolved is reported by name rather than silently skipped."),
        page);
    desc->setObjectName(QStringLiteral("muted"));
    desc->setWordWrap(true);
    root->addWidget(desc);

    auto* lists = new QHBoxLayout();
    lists->setSpacing(theme::kSpacingMedium);

    auto* videoColumn = new QVBoxLayout();
    videoColumn->setSpacing(theme::kSpacingXs);
    benchmarkVideoTitle_ = new QLabel(tr("Clips"), page);
    benchmarkVideoTitle_->setObjectName(QStringLiteral("sectionHeading"));
    videoColumn->addWidget(benchmarkVideoTitle_);
    benchmarkVideoList_ = new QListWidget(page);
    benchmarkVideoList_->setObjectName(QStringLiteral("benchmarkVideoList"));
    benchmarkVideoList_->setMinimumHeight(150);
    for (const char* clip : {"480x360.mp4", "640x480.mp4", "768x576.mp4", "1280x720.mp4",
                             "1920x1080.mp4"}) {
        auto* item = new QListWidgetItem(QString::fromLatin1(clip), benchmarkVideoList_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setData(kClipRole, QString::fromLatin1(clip));
    }
    videoColumn->addWidget(benchmarkVideoList_, 1);
    lists->addLayout(videoColumn, 1);

    auto* modelColumn = new QVBoxLayout();
    modelColumn->setSpacing(theme::kSpacingXs);
    benchmarkModelTitle_ = new QLabel(tr("Models"), page);
    benchmarkModelTitle_->setObjectName(QStringLiteral("sectionHeading"));
    modelColumn->addWidget(benchmarkModelTitle_);
    benchmarkModelList_ = new QListWidget(page);
    benchmarkModelList_->setObjectName(QStringLiteral("benchmarkModelList"));
    benchmarkModelList_->setMinimumHeight(150);
    benchmarkModelList_->setSelectionMode(QAbstractItemView::NoSelection);
    modelColumn->addWidget(benchmarkModelList_, 1);
    benchmarkSelectAllModels_ = new QPushButton(tr("Select all / none"), page);
    benchmarkSelectAllModels_->setMinimumHeight(theme::kSettingsControlSize);
    connect(benchmarkSelectAllModels_, &QPushButton::clicked, this, [this] {
        if (benchmarkModelList_ == nullptr) return;
        bool anyUnchecked = false;
        for (int row = 0; row < benchmarkModelList_->count(); ++row) {
            if (benchmarkModelList_->item(row)->checkState() != Qt::Checked) anyUnchecked = true;
        }
        for (int row = 0; row < benchmarkModelList_->count(); ++row) {
            benchmarkModelList_->item(row)->setCheckState(anyUnchecked ? Qt::Checked : Qt::Unchecked);
        }
    });
    modelColumn->addWidget(benchmarkSelectAllModels_);
    lists->addLayout(modelColumn, 1);

    root->addLayout(lists);

    benchmarkRawBox_ = new QCheckBox(tr("Also measure the raw pipeline (no enhancement)"), page);
    benchmarkRawBox_->setChecked(true);
    root->addWidget(benchmarkRawBox_);

    benchmarkRunButton_ = new QPushButton(tr("Run Benchmark"), page);
    benchmarkRunButton_->setObjectName(QStringLiteral("primary"));
    benchmarkRunButton_->setMinimumHeight(theme::kSettingsControlSize);
    connect(benchmarkRunButton_, &QPushButton::clicked, this, &SettingsDialog::runBenchmark);
    root->addWidget(benchmarkRunButton_);

    benchmarkResults_ = new QPlainTextEdit(page);
    benchmarkResults_->setReadOnly(true);
    benchmarkResults_->setObjectName(QStringLiteral("benchmarkResults"));
    benchmarkResults_->setPlaceholderText(tr("Results appear here as model × clip fps lines."));
    benchmarkResults_->setMinimumHeight(220);
    root->addWidget(benchmarkResults_, 1);

    benchmarkNote_ = new QLabel(
        tr("Clips are the AnimeJaNai benchmark patterns, resolved beside the executable, in the "
           "source tree, or under the current directory. TensorRT builds one engine per model and "
           "resolution on first use; that build is excluded from the measured window."),
        page);
    benchmarkNote_->setObjectName(QStringLiteral("muted"));
    benchmarkNote_->setWordWrap(true);
    root->addWidget(benchmarkNote_);

    root->addStretch();
    scroll->setWidget(page);
    benchmarkScroll_ = scroll;
    refreshBenchmarkCatalogue();
    return scroll;
}

void SettingsDialog::refreshBenchmarkCatalogue() {
    if (benchmarkModelList_ == nullptr) return;
    benchmarkModelList_->clear();

    const auto addGroup = [this](const QString& title) {
        auto* heading = new QListWidgetItem(title, benchmarkModelList_);
        heading->setFlags(Qt::NoItemFlags);
    };
    const auto addModel = [this](const QString& key, const QString& label,
                                 const QString& tooltip, bool checked) {
        auto* item = new QListWidgetItem(label, benchmarkModelList_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        item->setData(kModelRole, key);
        item->setToolTip(tooltip);
    };

    addGroup(tr("Anime4K"));
    for (const mplayer::anime4k::ModeInfo& mode : mplayer::anime4k::availableModes()) {
        addModel(QStringLiteral("anime4k:") + mode.id, mplayer::anime4k::displayLabel(mode.mode),
                 mode.hint, false);
    }

    addGroup(tr("AnimeJaNai"));
    const QVector<mplayer::native::AnimeJanaiPreset> presets =
        mplayer::native::animeJanaiPresets();
    for (const mplayer::native::AnimeJanaiPreset& preset : presets) {
        const bool builtin = preset.id.startsWith(QLatin1String("builtin"));
        addModel(preset.id, preset.label,
                 preset.supported ? tr("slot %1 · %2").arg(preset.slot).arg(preset.modelFile.isEmpty()
                                                                               ? tr("built-in chain")
                                                                               : preset.modelFile)
                                  : preset.reason,
                 builtin);
    }
}

QString SettingsDialog::benchmarkModelId(const QListWidgetItem* item) const {
    return item != nullptr ? item->data(kModelRole).toString() : QString();
}

QString SettingsDialog::benchmarkClipName(const QListWidgetItem* item) const {
    return item != nullptr ? item->data(kClipRole).toString() : QString();
}

void SettingsDialog::retranslateBenchmarkTab() {
    if (benchmarkHeading_ == nullptr) return;
    benchmarkHeading_->setText(tr("Playback Benchmark"));
    benchmarkVideoTitle_->setText(tr("Clips"));
    benchmarkModelTitle_->setText(tr("Models"));
    benchmarkSelectAllModels_->setText(tr("Select all / none"));
    benchmarkRawBox_->setText(tr("Also measure the raw pipeline (no enhancement)"));
    benchmarkRunButton_->setText(tr("Run Benchmark"));
    benchmarkNote_->setText(
        tr("Clips are the AnimeJaNai benchmark patterns, resolved beside the executable, in the "
           "source tree, or under the current directory. TensorRT builds one engine per model and "
           "resolution on first use; that build is excluded from the measured window."));
    // The catalogue headings are the only translated rows the list itself owns;
    // the model labels are catalogue text and are restated by rebuilding it.
    refreshBenchmarkCatalogue();
}

QString SettingsDialog::locateBenchmarkClip(const QString& name) const {
    // Next to the exe (dist layout), or the source tree relative to the build
    // directory, or the current working directory.
    const QStringList candidates = {
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("benchmarks/" + name),
        QDir(QCoreApplication::applicationDirPath())
            .absoluteFilePath("../tests/fixtures/media/benchmarks/" + name),
        QDir(QCoreApplication::applicationDirPath())
            .absoluteFilePath("../../tests/fixtures/media/benchmarks/" + name),
        QDir::current().absoluteFilePath("tests/fixtures/media/benchmarks/" + name),
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) return candidate;
    }
    return {};
}

void SettingsDialog::runBenchmark() {
    if (benchmarkResults_ == nullptr || benchmarkVideoList_ == nullptr ||
        benchmarkModelList_ == nullptr) {
        return;
    }

    QStringList clips;
    for (int row = 0; row < benchmarkVideoList_->count(); ++row) {
        const QListWidgetItem* item = benchmarkVideoList_->item(row);
        if (item->checkState() == Qt::Checked) clips << benchmarkClipName(item);
    }
    struct ModelRow {
        QString key;
        QString label;
    };
    QVector<ModelRow> models;
    for (int row = 0; row < benchmarkModelList_->count(); ++row) {
        const QListWidgetItem* item = benchmarkModelList_->item(row);
        if (!(item->flags() & Qt::ItemIsUserCheckable)) continue;
        if (item->checkState() == Qt::Checked) models.append({benchmarkModelId(item), item->text()});
    }
    const bool measureRaw = benchmarkRawBox_->isChecked();
    if (clips.isEmpty() || (models.isEmpty() && !measureRaw)) {
        benchmarkResults_->clear();
        benchmarkResults_->appendPlainText(
            tr("Select at least one clip and either one model or the raw pipeline."));
        return;
    }

    benchmarkResults_->clear();
    benchmarkResults_->appendPlainText(
        tr("Running %1 clip(s) × %2 model(s)%3…")
            .arg(clips.size())
            .arg(models.size())
            .arg(measureRaw ? tr(" + raw") : QString()));
    benchmarkRunButton_->setEnabled(false);

    // Everything is resolved on the worker: the catalogues, the plan, the file
    // lookups and the measurement. The GUI thread only receives the report.
    QThread* worker = QThread::create([this, clips, models, measureRaw] {
        QStringList lines;
        const mplayer::native::Backend backend = mplayer::native::loadNativePreferences().backend;

        const auto measure = [this, &lines](const QString& label, const QString& clipName,
                                            const std::vector<std::pair<std::string, std::string>>&
                                                options) {
            const QString path = locateBenchmarkClip(clipName);
            if (path.isEmpty()) {
                lines << QStringLiteral("%1 @ %2 — %3").arg(label, clipName, tr("clip not found"));
                return;
            }
            try {
                std::vector<std::pair<std::string, std::string>> opts = options;
                opts.emplace_back("vo", "null");
                opts.emplace_back("audio", "no");
                opts.emplace_back("loop-file", "inf");
                opts.emplace_back("untimed", "yes");
                opts.emplace_back("cache", "no");
                auto handle = std::make_unique<mplayer::MpvHandle>(opts);
                handle->loadFile(path.toStdString());

                // The first advancing frame excludes process startup and any
                // one-time engine build from the measured window.
                double firstFrame = -1;
                QElapsedTimer elapsed;
                elapsed.start();
                while (elapsed.elapsed() < 20000) {
                    try {
                        const double frame = handle->getNumber("estimated-frame-number");
                        if (frame > 0) {
                            firstFrame = frame;
                            break;
                        }
                    } catch (...) {
                    }
                    QThread::msleep(40);
                }
                if (firstFrame < 0) {
                    lines << QStringLiteral("%1 @ %2 — %3").arg(label, clipName, tr("no frames"));
                    return;
                }

                const double startFrame = firstFrame;
                const double startSeconds = elapsed.elapsed() / 1000.0;
                QThread::msleep(3000);
                double endFrame = -1;
                try {
                    endFrame = handle->getNumber("estimated-frame-number");
                } catch (...) {
                }
                const double window = (elapsed.elapsed() / 1000.0) - startSeconds;
                const double fps =
                    (window > 0.2 && endFrame > startFrame) ? (endFrame - startFrame) / window : -1.0;
                lines << QStringLiteral("%1 @ %2 — %3")
                             .arg(label, clipName,
                                  fps > 0 ? tr("%1 fps").arg(fps, 0, 'f', 1)
                                          : tr("- (too slow or stalled)"));
            } catch (const mplayer::MpvError& error) {
                QString message = QString::fromStdString(error.what());
                if (!error.property().empty()) {
                    message += QStringLiteral(" (%1)").arg(QString::fromStdString(error.property()));
                }
                lines << QStringLiteral("%1 @ %2 — %3").arg(label, clipName, message);
            } catch (const std::exception& error) {
                lines << QStringLiteral("%1 @ %2 — %3")
                             .arg(label, clipName, QString::fromUtf8(error.what()));
            }
        };

        if (measureRaw) {
            for (const QString& clip : clips) {
                measure(tr("Raw"), clip, {});
            }
        }

        for (const ModelRow& model : models) {
            if (model.key.startsWith(QLatin1String("anime4k:"))) {
                const QString shader =
                    mplayer::anime4k::shaderStringForCandidate(model.key.mid(8));
                if (shader.isEmpty()) {
                    lines << QStringLiteral("%1 — %2").arg(model.label, tr("shader chain is not "
                                                                          "available"));
                    continue;
                }
                for (const QString& clip : clips) {
                    measure(model.label, clip, {{"glsl-shaders", shader.toStdString()}});
                }
                continue;
            }

            // An AnimeJaNai preset resolves to a complete filter spec, so the
            // measurement runs the real Aji chain rather than a stand-in.
            const mplayer::native::AnimeJanaiPlan plan =
                mplayer::native::prepareAnimeJanaiPlan(model.key, backend);
            if (!plan.available) {
                lines << QStringLiteral("%1 — %2")
                             .arg(model.label,
                                  plan.reason.isEmpty() ? tr("plan is not available") : plan.reason);
                continue;
            }
            std::vector<std::pair<std::string, std::string>> options{
                {"hwdec", "nvdec"},
                {"vf", plan.filterSpec.toStdString()},
            };
            for (const QString& clip : clips) {
                measure(model.label, clip, options);
            }
        }

        QMetaObject::invokeMethod(
            this,
            [this, lines] {
                if (benchmarkResults_ != nullptr) {
                    benchmarkResults_->appendPlainText(lines.join(QLatin1Char('\n')));
                }
                if (benchmarkRunButton_ != nullptr) benchmarkRunButton_->setEnabled(true);
            },
            Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

}  // namespace mikai::app
