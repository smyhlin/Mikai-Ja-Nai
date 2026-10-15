#include "dialogs/SettingsDialog.hpp"

#include "dialogs/settings/SettingsUi.hpp"
#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"

#include <native/components/AnimeJanaiRuntime.hpp>
#include <native/models/NativeRife.hpp>
#include <native/models/NativeSpatialAssets.hpp>
#include <native/components/AnimeJanaiBuildLog.hpp>
#include <player/Anime4K.hpp>
#include <player/MpvHandle.hpp>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFont>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <iterator>
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

struct BenchmarkResultRow {
    QString model;
    QString clip;
    QString result;
    bool successful = false;
};
enum class BenchmarkEngineBuildUpdate { Started, Ready, Failed };
constexpr int kBenchmarkStartupTimeoutMs = 20000;
constexpr int kBenchmarkEngineBuildTimeoutMs = 10 * 60 * 1000;

QString benchmarkElapsedText(qint64 milliseconds) {
    const qint64 seconds = milliseconds / 1000;
    return QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

QWidget* SettingsDialog::buildBenchmarkTab() {
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("benchmarkScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* page = new QWidget(scroll);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(theme::kSpacingSmall, theme::kSpacingMedium,
                             theme::kSpacingSmall, theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    QVBoxLayout* heroLayout = nullptr;
    auto* hero = makeSettingsCard(page, &heroLayout);
    auto* heroTop = new QHBoxLayout();
    benchmarkHeading_ = new QLabel(tr("Playback Benchmark"), hero);
    benchmarkHeading_->setObjectName(QStringLiteral("heading"));
    yieldCaptionWidth(benchmarkHeading_);
    benchmarkMode_ = new QLabel(tr("Off-screen · real pipeline"), hero);
    benchmarkMode_->setObjectName(QStringLiteral("benchmarkMode"));
    heroTop->addWidget(benchmarkHeading_, 1);
    heroTop->addWidget(benchmarkMode_, 0, Qt::AlignTop);
    heroLayout->addLayout(heroTop);

    benchmarkDescription_ = new QLabel(
        tr("Choose clips and models to compare. Each selected clip/model pair is one run."),
        hero);
    benchmarkDescription_->setObjectName(QStringLiteral("muted"));
    benchmarkDescription_->setWordWrap(true);
    heroLayout->addWidget(benchmarkDescription_);
    root->addWidget(hero);

    QVBoxLayout* matrixLayout = nullptr;
    auto* matrixCard = makeSettingsCard(page, &matrixLayout);
    benchmarkMatrixTitle_ = new QLabel(tr("Test matrix"), matrixCard);
    benchmarkMatrixTitle_->setObjectName(QStringLiteral("sectionHeading"));
    matrixLayout->addWidget(benchmarkMatrixTitle_);

    auto* clipsHeader = new QHBoxLayout();
    benchmarkVideoTitle_ = new QLabel(tr("Video clips"), matrixCard);
    benchmarkVideoTitle_->setObjectName(QStringLiteral("sectionHeading"));
    yieldCaptionWidth(benchmarkVideoTitle_);
    benchmarkClipCount_ = new QLabel(matrixCard);
    benchmarkClipCount_->setObjectName(QStringLiteral("benchmarkClipCount"));
    benchmarkSelectAllClips_ = new QPushButton(tr("Clear selection"), matrixCard);
    benchmarkSelectAllClips_->setObjectName(QStringLiteral("textAction"));
    benchmarkSelectAllClips_->setProperty("benchmarkSelectionControl", QStringLiteral("clips"));
    benchmarkSelectAllClips_->setMinimumHeight(theme::kSettingsControlSize);
    clipsHeader->addWidget(benchmarkVideoTitle_, 1);
    clipsHeader->addWidget(benchmarkClipCount_);
    clipsHeader->addWidget(benchmarkSelectAllClips_);
    matrixLayout->addLayout(clipsHeader);

    auto* clipGrid = new QGridLayout();
    clipGrid->setContentsMargins(0, 0, 0, 0);
    clipGrid->setHorizontalSpacing(theme::kSpacingSmall);
    clipGrid->setVerticalSpacing(theme::kSpacingSmall);
    struct ClipInfo {
        const char* file;
        const char* dimensions;
        const char* aspect;
    };
    constexpr ClipInfo clips[] = {
        {"480x360.mp4", "480 × 360", "4:3"},
        {"640x480.mp4", "640 × 480", "4:3"},
        {"768x576.mp4", "768 × 576", "4:3"},
        {"1280x720.mp4", "1280 × 720", "16:9"},
        {"1920x1080.mp4", "1920 × 1080", "16:9"},
    };
    benchmarkClipChecks_.clear();
    for (int index = 0; index < static_cast<int>(std::size(clips)); ++index) {
        const ClipInfo& clip = clips[index];
        auto* row = new QFrame(matrixCard);
        row->setObjectName(QStringLiteral("benchmarkClipRow"));
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(theme::kSpacingSmall, theme::kSpacingXs,
                                      theme::kSpacingSmall, theme::kSpacingXs);
        auto* check = new QCheckBox(QString::fromUtf8(clip.dimensions), row);
        check->setObjectName(QStringLiteral("benchmarkClipCheck"));
        check->setProperty("benchmarkClipName", QString::fromLatin1(clip.file));
        check->setToolTip(QString::fromLatin1(clip.file));
        check->setChecked(true);
        check->setMinimumHeight(theme::kBenchmarkClipRowHeight);
        rowLayout->addWidget(check, 1);

        auto* aspect = new QLabel(QString::fromLatin1(clip.aspect), row);
        aspect->setObjectName(QStringLiteral("muted"));
        rowLayout->addWidget(aspect, 0, Qt::AlignRight | Qt::AlignVCenter);
        clipGrid->addWidget(row, index / 3, index % 3);
        benchmarkClipChecks_.append(check);
        connect(check, &QCheckBox::toggled, this,
                [this] { updateBenchmarkSelectionSummary(); });
    }
    matrixLayout->addLayout(clipGrid);

    auto* modelsHeader = new QHBoxLayout();
    benchmarkModelTitle_ = new QLabel(tr("Enhancement models"), matrixCard);
    benchmarkModelTitle_->setObjectName(QStringLiteral("sectionHeading"));
    yieldCaptionWidth(benchmarkModelTitle_);
    benchmarkModelCount_ = new QLabel(matrixCard);
    benchmarkModelCount_->setObjectName(QStringLiteral("benchmarkModelCount"));
    benchmarkSelectAllModels_ = new QPushButton(tr("Select all"), matrixCard);
    benchmarkSelectAllModels_->setObjectName(QStringLiteral("textAction"));
    benchmarkSelectAllModels_->setProperty("benchmarkSelectionControl", QStringLiteral("models"));
    benchmarkSelectAllModels_->setMinimumHeight(theme::kSettingsControlSize);
    modelsHeader->addWidget(benchmarkModelTitle_, 1);
    modelsHeader->addWidget(benchmarkModelCount_);
    modelsHeader->addWidget(benchmarkSelectAllModels_);
    matrixLayout->addLayout(modelsHeader);

    benchmarkModelSearch_ = new QLineEdit(matrixCard);
    benchmarkModelSearch_->setObjectName(QStringLiteral("benchmarkModelSearch"));
    benchmarkModelSearch_->setPlaceholderText(tr("Filter by model or group"));
    benchmarkModelSearch_->setClearButtonEnabled(true);
    benchmarkModelSearch_->setAccessibleName(tr("Filter benchmark models"));
    matrixLayout->addWidget(benchmarkModelSearch_);

    benchmarkModelTree_ = new QTreeWidget(matrixCard);
    benchmarkModelTree_->setObjectName(QStringLiteral("benchmarkModelTree"));
    benchmarkModelTree_->setHeaderHidden(true);
    benchmarkModelTree_->setColumnCount(1);
    benchmarkModelTree_->setIndentation(theme::kBenchmarkTreeIndentation);
    benchmarkModelTree_->setMinimumHeight(theme::kSettingsControlSize * 4);
    benchmarkModelTree_->setMaximumHeight(theme::kBenchmarkSelectorHeight);
    benchmarkModelTree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    benchmarkModelTree_->setTextElideMode(Qt::ElideMiddle);
    benchmarkModelTree_->setUniformRowHeights(true);
    benchmarkModelTree_->setSelectionMode(QAbstractItemView::NoSelection);
    benchmarkModelTree_->setAccessibleName(tr("Available benchmark models"));
    matrixLayout->addWidget(benchmarkModelTree_);

    connect(benchmarkModelSearch_, &QLineEdit::textChanged, this,
            [this](const QString& query) { filterBenchmarkModels(query); });
    connect(benchmarkModelTree_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem* item, int) {
                if (benchmarkSelectionUpdating_ || item == nullptr) return;
                benchmarkSelectionUpdating_ = true;
                if (item->parent() == nullptr) {
                    const Qt::CheckState state = item->checkState(0) == Qt::Checked
                                                     ? Qt::Checked
                                                     : Qt::Unchecked;
                    for (int index = 0; index < item->childCount(); ++index) {
                        item->child(index)->setCheckState(0, state);
                    }
                    item->setCheckState(0, state);
                } else {
                    QTreeWidgetItem* group = item->parent();
                    int checked = 0;
                    for (int index = 0; index < group->childCount(); ++index) {
                        checked += group->child(index)->checkState(0) == Qt::Checked ? 1 : 0;
                    }
                    group->setCheckState(
                        0, checked == 0 ? Qt::Unchecked
                                        : checked == group->childCount() ? Qt::Checked
                                                                         : Qt::PartiallyChecked);
                }
                benchmarkSelectionUpdating_ = false;
                updateBenchmarkSelectionSummary();
            });

    connect(benchmarkSelectAllClips_, &QPushButton::clicked, this, [this] {
        const bool anyUnchecked =
            std::any_of(benchmarkClipChecks_.cbegin(), benchmarkClipChecks_.cend(),
                        [](const QCheckBox* check) { return !check->isChecked(); });
        for (QCheckBox* check : benchmarkClipChecks_) check->setChecked(anyUnchecked);
    });

    connect(benchmarkSelectAllModels_, &QPushButton::clicked, this, [this] {
        if (benchmarkModelTree_ == nullptr) return;
        bool anyUnchecked = false;
        for (int groupIndex = 0; groupIndex < benchmarkModelTree_->topLevelItemCount();
             ++groupIndex) {
            QTreeWidgetItem* group = benchmarkModelTree_->topLevelItem(groupIndex);
            for (int index = 0; index < group->childCount(); ++index) {
                anyUnchecked |= group->child(index)->checkState(0) != Qt::Checked;
            }
        }
        benchmarkSelectionUpdating_ = true;
        const Qt::CheckState state = anyUnchecked ? Qt::Checked : Qt::Unchecked;
        for (int groupIndex = 0; groupIndex < benchmarkModelTree_->topLevelItemCount();
             ++groupIndex) {
            QTreeWidgetItem* group = benchmarkModelTree_->topLevelItem(groupIndex);
            for (int index = 0; index < group->childCount(); ++index) {
                group->child(index)->setCheckState(0, state);
            }
            group->setCheckState(0, state);
        }
        benchmarkSelectionUpdating_ = false;
        updateBenchmarkSelectionSummary();
    });

    benchmarkRawBox_ = new QCheckBox(tr("Include the unfiltered baseline"), matrixCard);
    benchmarkRawBox_->setObjectName(QStringLiteral("benchmarkRawBox"));
    benchmarkRawBox_->setMinimumHeight(theme::kSettingsControlSize);
    benchmarkRawBox_->setChecked(true);
    matrixLayout->addWidget(benchmarkRawBox_);

    benchmarkRunSummary_ = new QLabel(matrixCard);
    benchmarkRunSummary_->setObjectName(QStringLiteral("benchmarkRunSummary"));
    benchmarkRunSummary_->setProperty("textRole", QStringLiteral("muted"));
    matrixLayout->addWidget(benchmarkRunSummary_);

    auto* runRow = new QHBoxLayout();
    runRow->addStretch();
    benchmarkRunButton_ = new QPushButton(tr("Run benchmark"), matrixCard);
    benchmarkRunButton_->setObjectName(QStringLiteral("primary"));
    benchmarkRunButton_->setProperty("benchmarkRunAction", true);
    benchmarkRunButton_->setMinimumHeight(theme::kPrimaryControlSize);
    connect(benchmarkRunButton_, &QPushButton::clicked, this, &SettingsDialog::runBenchmark);
    runRow->addWidget(benchmarkRunButton_);
    matrixLayout->addLayout(runRow);
    root->addWidget(matrixCard);

    QVBoxLayout* resultsCardLayout = nullptr;
    auto* resultsCard = makeSettingsCard(page, &resultsCardLayout);
    auto* resultsHeader = new QHBoxLayout();
    benchmarkResultsTitle_ = new QLabel(tr("Results"), resultsCard);
    benchmarkResultsTitle_->setObjectName(QStringLiteral("sectionHeading"));
    benchmarkRunStatus_ = new QLabel(tr("Ready"), resultsCard);
    benchmarkRunStatus_->setObjectName(QStringLiteral("benchmarkRunStatus"));
    resultsHeader->addWidget(benchmarkResultsTitle_, 1);
    resultsHeader->addWidget(benchmarkRunStatus_);
    resultsCardLayout->addLayout(resultsHeader);

    benchmarkProgress_ = new QProgressBar(resultsCard);
    benchmarkProgress_->setObjectName(QStringLiteral("benchmarkProgress"));
    benchmarkProgress_->setTextVisible(false);
    benchmarkProgress_->setAccessibleName(tr("Benchmark run progress"));
    benchmarkProgress_->hide();
    resultsCardLayout->addWidget(benchmarkProgress_);

    benchmarkActivity_ = new QLabel(tr("Ready when you are."), resultsCard);
    benchmarkActivity_->setObjectName(QStringLiteral("benchmarkActivity"));
    benchmarkActivity_->setWordWrap(true);
    resultsCardLayout->addWidget(benchmarkActivity_);

    benchmarkEngineProgress_ = new QProgressBar(resultsCard);
    benchmarkEngineProgress_->setObjectName(QStringLiteral("benchmarkEngineProgress"));
    benchmarkEngineProgress_->setTextVisible(false);
    benchmarkEngineProgress_->setRange(0, 0);
    benchmarkEngineProgress_->setAccessibleName(tr("TensorRT engine build progress; indeterminate"));
    benchmarkEngineProgress_->hide();
    resultsCardLayout->addWidget(benchmarkEngineProgress_);

    benchmarkEngineElapsedLabel_ = new QLabel(resultsCard);
    benchmarkEngineElapsedLabel_->setObjectName(QStringLiteral("benchmarkEngineElapsed"));
    benchmarkEngineElapsedLabel_->setProperty("textRole", QStringLiteral("muted"));
    benchmarkEngineElapsedLabel_->hide();
    resultsCardLayout->addWidget(benchmarkEngineElapsedLabel_);

    benchmarkEngineTimer_ = new QTimer(this);
    benchmarkEngineTimer_->setInterval(1000);
    connect(benchmarkEngineTimer_, &QTimer::timeout, this, [this] {
        if (!benchmarkEngineElapsedLabel_->isVisible()) return;
        const qint64 seconds = benchmarkEngineBuildElapsed_.elapsed() / 1000;
        benchmarkEngineElapsedLabel_->setText(
            tr("TensorRT engine build elapsed · %1")
                .arg(QStringLiteral("%1:%2:%3")
                         .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
                         .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
                         .arg(seconds % 60, 2, 10, QLatin1Char('0'))));
    });

    benchmarkNote_ = new QLabel(
        tr("For TensorRT, FPS measurement waits for the backend to report the selected slot "
           "configured. Engine-build time is shown separately and excluded."),
        resultsCard);
    benchmarkNote_->setObjectName(QStringLiteral("muted"));
    benchmarkNote_->setWordWrap(true);
    resultsCardLayout->addWidget(benchmarkNote_);

    benchmarkResultStack_ = new QStackedWidget(resultsCard);
    benchmarkEmptyState_ = new QLabel(
        tr("Run a benchmark to see throughput for each selected clip and model."),
        benchmarkResultStack_);
    benchmarkEmptyState_->setObjectName(QStringLiteral("benchmarkEmptyState"));
    benchmarkEmptyState_->setWordWrap(true);
    benchmarkEmptyState_->setAlignment(Qt::AlignCenter);
    benchmarkResultStack_->addWidget(benchmarkEmptyState_);

    benchmarkResults_ = new QTableWidget(benchmarkResultStack_);
    benchmarkResults_->setObjectName(QStringLiteral("benchmarkResults"));
    benchmarkResults_->setColumnCount(3);
    benchmarkResults_->setHorizontalHeaderLabels({tr("Model"), tr("Clip"), tr("Throughput")});
    benchmarkResults_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    benchmarkResults_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    benchmarkResults_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    benchmarkResults_->verticalHeader()->hide();
    benchmarkResults_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    benchmarkResults_->setSelectionBehavior(QAbstractItemView::SelectRows);
    benchmarkResults_->setSelectionMode(QAbstractItemView::SingleSelection);
    benchmarkResults_->setAlternatingRowColors(true);
    benchmarkResults_->setShowGrid(false);
    benchmarkResults_->setWordWrap(false);
    benchmarkResults_->setMinimumHeight(theme::kBenchmarkResultsHeight);
    benchmarkResultStack_->addWidget(benchmarkResults_);
    benchmarkResultStack_->setMinimumHeight(theme::kBenchmarkResultsHeight);
    resultsCardLayout->addWidget(benchmarkResultStack_);
    root->addWidget(resultsCard);

    root->addStretch();
    scroll->setWidget(page);
    benchmarkScroll_ = scroll;

    connect(benchmarkRawBox_, &QCheckBox::toggled, this,
            [this] { updateBenchmarkSelectionSummary(); });
    refreshBenchmarkCatalogue();
    return scroll;
}

void SettingsDialog::refreshBenchmarkCatalogue() {
    if (benchmarkModelTree_ == nullptr) return;

    QSet<QString> checkedIds;
    const bool hasPreviousCatalogue = benchmarkModelTree_->topLevelItemCount() > 0;
    if (hasPreviousCatalogue) {
        for (int groupIndex = 0; groupIndex < benchmarkModelTree_->topLevelItemCount();
             ++groupIndex) {
            const QTreeWidgetItem* group = benchmarkModelTree_->topLevelItem(groupIndex);
            for (int index = 0; index < group->childCount(); ++index) {
                const QTreeWidgetItem* item = group->child(index);
                if (item->checkState(0) == Qt::Checked) checkedIds.insert(benchmarkModelId(item));
            }
        }
    }

    benchmarkSelectionUpdating_ = true;
    benchmarkModelTree_->clear();

    const auto addGroup = [this](const QString& title) {
        auto* group = new QTreeWidgetItem(benchmarkModelTree_);
        group->setText(0, title);
        QFont heading = benchmarkModelTree_->font();
        heading.setWeight(QFont::DemiBold);
        group->setFont(0, heading);
        group->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        group->setExpanded(title == tr("AnimeJaNai"));
        return group;
    };
    const auto addModel = [this, &checkedIds, hasPreviousCatalogue](
                              QTreeWidgetItem* group, const QString& key, const QString& label,
                              const QString& tooltip, bool defaultChecked) {
        auto* item = new QTreeWidgetItem(group);
        item->setText(0, label);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        const bool checked =
            hasPreviousCatalogue ? checkedIds.contains(key) : defaultChecked;
        item->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
        item->setData(0, kModelRole, key);
        item->setToolTip(0, tooltip);
    };

    QTreeWidgetItem* anime4kGroup = addGroup(tr("Anime4K"));
    for (const mplayer::anime4k::ModeInfo& mode : mplayer::anime4k::availableModes()) {
        addModel(anime4kGroup, QStringLiteral("anime4k:") + mode.id,
                 mplayer::anime4k::displayLabel(mode.mode), mode.hint, false);
    }

    const QVector<mplayer::native::AnimeJanaiPreset> presets =
        mplayer::native::animeJanaiPresets();
    QTreeWidgetItem* animeJanaiGroup = addGroup(tr("AnimeJaNai"));
    for (const mplayer::native::AnimeJanaiPreset& preset : presets) {
        if (!preset.id.startsWith(QLatin1String("builtin"))) continue;
        const QString tooltip =
            preset.supported
                ? tr("slot %1 · %2")
                      .arg(preset.slot)
                      .arg(preset.modelFile.isEmpty() ? tr("built-in chain") : preset.modelFile)
                : preset.reason;
        addModel(animeJanaiGroup, preset.id, preset.label, tooltip, true);
    }

    QTreeWidgetItem* spatialGroup = addGroup(tr("Spatial"));
    for (const mplayer::native::AnimeJanaiPreset& preset : presets) {
        if (!preset.id.startsWith(QLatin1String("spatial:"))) continue;
        const QString tooltip =
            preset.supported ? tr("slot %1 · %2").arg(preset.slot).arg(preset.modelFile)
                             : preset.reason;
        addModel(spatialGroup, preset.id, preset.label, tooltip, false);
    }

    QTreeWidgetItem* rifeGroup = addGroup(tr("RIFE"));
    for (const mplayer::native::AnimeJanaiPreset& preset : presets) {
        if (!preset.id.startsWith(QLatin1String("rife:"))) continue;
        const QString tooltip =
            preset.supported ? tr("slot %1 · %2").arg(preset.slot).arg(preset.modelFile)
                             : preset.reason;
        addModel(rifeGroup, preset.id, preset.label, tooltip, false);
    }

    const auto updateGroupState = [](QTreeWidgetItem* group) {
        int checked = 0;
        for (int index = 0; index < group->childCount(); ++index) {
            checked += group->child(index)->checkState(0) == Qt::Checked ? 1 : 0;
        }
        group->setCheckState(0, checked == 0 ? Qt::Unchecked
                                             : checked == group->childCount() ? Qt::Checked
                                                                              : Qt::PartiallyChecked);
    };
    for (int groupIndex = 0; groupIndex < benchmarkModelTree_->topLevelItemCount(); ++groupIndex) {
        updateGroupState(benchmarkModelTree_->topLevelItem(groupIndex));
    }
    benchmarkSelectionUpdating_ = false;
    filterBenchmarkModels(benchmarkModelSearch_->text());
    updateBenchmarkSelectionSummary();
}

QString SettingsDialog::benchmarkModelId(const QTreeWidgetItem* item) const {
    return item != nullptr ? item->data(0, kModelRole).toString() : QString();
}

void SettingsDialog::filterBenchmarkModels(const QString& query) {
    if (benchmarkModelTree_ == nullptr) return;
    const QString filter = query.trimmed();
    for (int groupIndex = 0; groupIndex < benchmarkModelTree_->topLevelItemCount(); ++groupIndex) {
        QTreeWidgetItem* group = benchmarkModelTree_->topLevelItem(groupIndex);
        const bool groupMatches =
            filter.isEmpty() || group->text(0).contains(filter, Qt::CaseInsensitive);
        int visibleChildren = 0;
        for (int index = 0; index < group->childCount(); ++index) {
            QTreeWidgetItem* model = group->child(index);
            const bool visible =
                groupMatches || model->text(0).contains(filter, Qt::CaseInsensitive);
            model->setHidden(!visible);
            visibleChildren += visible ? 1 : 0;
        }
        group->setHidden(!filter.isEmpty() && visibleChildren == 0);
        if (!filter.isEmpty() && visibleChildren > 0) group->setExpanded(true);
    }
}

QString SettingsDialog::benchmarkClipName(const QCheckBox* checkbox) const {
    return checkbox != nullptr ? checkbox->property("benchmarkClipName").toString() : QString();
}

void SettingsDialog::updateBenchmarkSelectionSummary() {
    if (benchmarkClipCount_ == nullptr || benchmarkModelCount_ == nullptr ||
        benchmarkRunSummary_ == nullptr || benchmarkRunButton_ == nullptr) {
        return;
    }

    const int selectedClips = static_cast<int>(std::count_if(
        benchmarkClipChecks_.cbegin(), benchmarkClipChecks_.cend(),
        [](const QCheckBox* check) { return check->isChecked(); }));
    const int totalClips = benchmarkClipChecks_.size();

    int selectedModels = 0;
    int totalModels = 0;
    if (benchmarkModelTree_ != nullptr) {
        for (int groupIndex = 0; groupIndex < benchmarkModelTree_->topLevelItemCount();
             ++groupIndex) {
            const QTreeWidgetItem* group = benchmarkModelTree_->topLevelItem(groupIndex);
            totalModels += group->childCount();
            for (int index = 0; index < group->childCount(); ++index) {
                selectedModels += group->child(index)->checkState(0) == Qt::Checked ? 1 : 0;
            }
        }
    }

    benchmarkClipCount_->setText(
        tr("%1 / %2 selected").arg(selectedClips).arg(totalClips));
    benchmarkSelectAllClips_->setText(selectedClips == totalClips ? tr("Clear selection")
                                                                  : tr("Select all"));
    benchmarkModelCount_->setText(
        tr("%1 / %2 selected").arg(selectedModels).arg(totalModels));
    benchmarkSelectAllModels_->setText(
        selectedModels == totalModels && totalModels > 0 ? tr("Clear selection")
                                                          : tr("Select all"));
    const int runs = selectedClips * (selectedModels + (benchmarkRawBox_->isChecked() ? 1 : 0));
    benchmarkRunSummary_->setText(
        tr("%1 clips · %2 models · %3 runs").arg(selectedClips).arg(selectedModels).arg(runs));
    benchmarkRunButton_->setEnabled(!benchmarkRunning_ && selectedClips > 0 &&
                                    (selectedModels > 0 || benchmarkRawBox_->isChecked()));
}

void SettingsDialog::retranslateBenchmarkTab() {
    if (benchmarkHeading_ == nullptr) return;
    benchmarkHeading_->setText(tr("Playback Benchmark"));
    benchmarkMode_->setText(tr("Off-screen · real pipeline"));
    benchmarkDescription_->setText(
        tr("Choose clips and models to compare. Each selected clip/model pair is one run."));
    benchmarkMatrixTitle_->setText(tr("Test matrix"));
    benchmarkVideoTitle_->setText(tr("Video clips"));
    benchmarkModelTitle_->setText(tr("Enhancement models"));
    benchmarkModelSearch_->setPlaceholderText(tr("Filter by model or group"));
    benchmarkModelSearch_->setAccessibleName(tr("Filter benchmark models"));
    benchmarkRawBox_->setText(tr("Include the unfiltered baseline"));
    benchmarkRunButton_->setText(tr("Run benchmark"));
    benchmarkResultsTitle_->setText(tr("Results"));
    benchmarkNote_->setText(
        tr("For TensorRT, FPS measurement waits for the backend to report the selected slot "
           "configured. Engine-build time is shown separately and excluded."));
    benchmarkEmptyState_->setText(
        tr("Run a benchmark to see throughput for each selected clip and model."));
    benchmarkResults_->setHorizontalHeaderLabels({tr("Model"), tr("Clip"), tr("Throughput")});
    benchmarkProgress_->setAccessibleName(tr("Benchmark run progress"));
    benchmarkEngineProgress_->setAccessibleName(
        tr("TensorRT engine build progress; indeterminate"));
    benchmarkModelTree_->setAccessibleName(tr("Available benchmark models"));
    if (!benchmarkRunning_ && benchmarkResults_->rowCount() == 0) {
        benchmarkRunStatus_->setText(tr("Ready"));
        benchmarkActivity_->setText(tr("Ready when you are."));
    }
    refreshBenchmarkCatalogue();
    updateBenchmarkSelectionSummary();
}

QString SettingsDialog::locateBenchmarkClip(const QString& name) {
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
    if (benchmarkRunning_ || benchmarkResults_ == nullptr || benchmarkModelTree_ == nullptr) return;

    QStringList clips;
    for (const QCheckBox* check : benchmarkClipChecks_) {
        if (check->isChecked()) clips.append(benchmarkClipName(check));
    }
    struct ModelRow {
        QString key;
        QString label;
    };
    QVector<ModelRow> models;
    for (int groupIndex = 0; groupIndex < benchmarkModelTree_->topLevelItemCount();
         ++groupIndex) {
        const QTreeWidgetItem* group = benchmarkModelTree_->topLevelItem(groupIndex);
        for (int index = 0; index < group->childCount(); ++index) {
            const QTreeWidgetItem* item = group->child(index);
            if (item->checkState(0) == Qt::Checked) {
                models.append({benchmarkModelId(item), item->text(0)});
            }
        }
    }
    const bool measureRaw = benchmarkRawBox_->isChecked();
    if (clips.isEmpty() || (models.isEmpty() && !measureRaw)) return;
    const int totalRuns =
        static_cast<int>(clips.size()) *
        (static_cast<int>(models.size()) + (measureRaw ? 1 : 0));

    benchmarkRunning_ = true;
    benchmarkResults_->setRowCount(0);
    benchmarkResultStack_->setCurrentWidget(benchmarkEmptyState_);
    benchmarkRunStatus_->setText(tr("Benchmark running · %1 / %2").arg(0).arg(totalRuns));
    benchmarkProgress_->setRange(0, totalRuns);
    benchmarkProgress_->setValue(0);
    benchmarkProgress_->show();
    benchmarkActivity_->setText(tr("Starting benchmark…"));
    benchmarkEngineTimer_->stop();
    benchmarkEngineProgress_->hide();
    benchmarkEngineElapsedLabel_->hide();
    for (QCheckBox* check : benchmarkClipChecks_) check->setEnabled(false);
    benchmarkModelSearch_->setEnabled(false);
    benchmarkModelTree_->setEnabled(false);
    benchmarkRawBox_->setEnabled(false);
    benchmarkSelectAllClips_->setEnabled(false);
    benchmarkSelectAllModels_->setEnabled(false);
    updateBenchmarkSelectionSummary();

    const auto dialog = std::make_shared<QPointer<SettingsDialog>>(this);
    QCoreApplication* const application = QCoreApplication::instance();
    QThread* worker =
        QThread::create([dialog, application, clips, models, measureRaw, totalRuns] {
        int completedRuns = 0;
        int failedRuns = 0;
        const mplayer::native::Backend backend = mplayer::native::loadNativePreferences().backend;

        const auto publishActivity = [dialog, application](const QString& text) {
            QMetaObject::invokeMethod(
                application,
                [dialog, text] {
                    if (dialog->isNull()) return;
                    SettingsDialog* self = dialog->data();
                    if (self->benchmarkActivity_ != nullptr) {
                        self->benchmarkActivity_->setText(text);
                    }
                },
                Qt::QueuedConnection);
        };

        const auto publishEngineBuild = [dialog, application](
                                            BenchmarkEngineBuildUpdate update,
                                            const QString& detail) {
            QMetaObject::invokeMethod(
                application,
                [dialog, update, detail] {
                    if (dialog->isNull()) return;
                    SettingsDialog* self = dialog->data();
                    if (self->benchmarkActivity_ == nullptr ||
                        self->benchmarkEngineProgress_ == nullptr ||
                        self->benchmarkEngineElapsedLabel_ == nullptr ||
                        self->benchmarkEngineTimer_ == nullptr) {
                        return;
                    }

                    if (update == BenchmarkEngineBuildUpdate::Started) {
                        self->benchmarkEngineBuildElapsed_.start();
                        self->benchmarkEngineProgress_->setRange(0, 0);
                        self->benchmarkEngineProgress_->show();
                        self->benchmarkEngineElapsedLabel_->setText(
                            SettingsDialog::tr("TensorRT engine build elapsed · %1")
                                .arg(benchmarkElapsedText(0)));
                        self->benchmarkEngineElapsedLabel_->show();
                        self->benchmarkEngineTimer_->start();
                        self->benchmarkActivity_->setText(
                            SettingsDialog::tr("Building TensorRT engine · %1").arg(detail));
                        return;
                    }

                    const qint64 elapsed = self->benchmarkEngineBuildElapsed_.isValid()
                                               ? self->benchmarkEngineBuildElapsed_.elapsed()
                                               : 0;
                    self->benchmarkEngineTimer_->stop();
                    self->benchmarkEngineProgress_->hide();
                    self->benchmarkEngineElapsedLabel_->setText(
                        update == BenchmarkEngineBuildUpdate::Ready
                            ? SettingsDialog::tr("TensorRT engine ready after %1")
                                  .arg(benchmarkElapsedText(elapsed))
                            : SettingsDialog::tr("TensorRT engine build failed after %1")
                                  .arg(benchmarkElapsedText(elapsed)));
                    self->benchmarkEngineElapsedLabel_->show();
                    self->benchmarkActivity_->setText(
                        update == BenchmarkEngineBuildUpdate::Ready
                            ? SettingsDialog::tr("Engine ready; measuring throughput · %1")
                                  .arg(detail)
                            : SettingsDialog::tr("TensorRT engine build failed · %1")
                                  .arg(detail));
                },
                Qt::QueuedConnection);
        };

        const auto append = [dialog, application, totalRuns, &completedRuns, &failedRuns](
                                const QString& model, const QString& clip,
                                const QString& result, bool successful) {
            const BenchmarkResultRow resultRow{model, clip, result, successful};
            if (!successful) ++failedRuns;
            const int completed = ++completedRuns;
            QMetaObject::invokeMethod(
                application,
                [dialog, resultRow, completed, totalRuns] {
                    if (dialog->isNull()) return;
                    SettingsDialog* self = dialog->data();
                    const int row = self->benchmarkResults_->rowCount();
                    self->benchmarkResults_->insertRow(row);

                    auto* modelItem = new QTableWidgetItem(resultRow.model);
                    modelItem->setToolTip(resultRow.model);
                    self->benchmarkResults_->setItem(row, 0, modelItem);

                    const QString clipName =
                        resultRow.clip.isEmpty()
                            ? QStringLiteral("—")
                            : QFileInfo(resultRow.clip).completeBaseName().replace(
                                  QLatin1Char('x'), QStringLiteral(" × "));
                    auto* clipItem = new QTableWidgetItem(clipName);
                    clipItem->setToolTip(resultRow.clip);
                    self->benchmarkResults_->setItem(row, 1, clipItem);

                    auto* resultItem = new QTableWidgetItem(resultRow.result);
                    resultItem->setToolTip(resultRow.result);
                    resultItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                    resultItem->setForeground(QColor(QLatin1String(
                        resultRow.successful ? theme::kAccent : theme::kDanger)));
                    self->benchmarkResults_->setItem(row, 2, resultItem);
                    self->benchmarkResultStack_->setCurrentWidget(self->benchmarkResults_);
                    self->benchmarkProgress_->setValue(completed);
                    self->benchmarkRunStatus_->setText(
                        SettingsDialog::tr("Benchmark running · %1 / %2")
                            .arg(completed)
                            .arg(totalRuns));
                },
                Qt::QueuedConnection);
        };

        const auto measure = [&append, &publishActivity, &publishEngineBuild](
                                 const QString& label, const QString& clipName,
                                 const std::vector<std::pair<std::string, std::string>>& options,
                                 int slot = -1,
                                 mplayer::native::Backend engineBackend =
                                     mplayer::native::Backend::DirectML) {
            const QString path = SettingsDialog::locateBenchmarkClip(clipName);
            if (path.isEmpty()) {
                append(label, clipName, SettingsDialog::tr("clip not found"), false);
                return;
            }

            const QString clipLabel = QFileInfo(clipName).completeBaseName().replace(
                QLatin1Char('x'), QStringLiteral(" × "));
            const QString runLabel = QStringLiteral("%1 · %2").arg(label, clipLabel);
            publishActivity(SettingsDialog::tr("Loading clip · %1").arg(runLabel));
            try {
                std::vector<std::pair<std::string, std::string>> opts = options;
                opts.emplace_back("vo", "null");
                opts.emplace_back("audio", "no");
                opts.emplace_back("loop-file", "inf");
                opts.emplace_back("untimed", "yes");
                opts.emplace_back("cache", "no");
                const bool watchesEngineBuild =
                    slot >= 0 && engineBackend == mplayer::native::Backend::TensorRT;
                mplayer::native::AnimeJanaiBuildGate buildGate;
                QElapsedTimer engineBuildElapsed;
                double observedFrame = -1;
                auto handle = std::make_unique<mplayer::MpvHandle>(opts);
                if (watchesEngineBuild) {
                    handle->setBackendLogObservationEnabled(true);
                    handle->setEventSink([&](mpv_event* event) {
                        if (event == nullptr || event->event_id != MPV_EVENT_LOG_MESSAGE ||
                            event->data == nullptr) {
                            return;
                        }
                        const auto* message =
                            static_cast<const mpv_event_log_message*>(event->data);
                        const QString line =
                            QString::fromUtf8(message->text == nullptr ? "" : message->text).trimmed();
                        const QString prefix =
                            QString::fromUtf8(message->prefix == nullptr ? "" : message->prefix);
                        const bool wasBuilding = buildGate.buildActive();
                        const mplayer::native::AnimeJanaiBuildLogEvent buildEvent =
                            buildGate.observeLog(prefix, line, engineBackend, slot);

                        if (buildEvent ==
                            mplayer::native::AnimeJanaiBuildLogEvent::TensorRtBuildStarted) {
                            if (!wasBuilding) {
                                engineBuildElapsed.start();
                                publishEngineBuild(BenchmarkEngineBuildUpdate::Started, runLabel);
                            }
                            return;
                        }
                        if (buildEvent ==
                            mplayer::native::AnimeJanaiBuildLogEvent::SlotConfigured) {
                            if (wasBuilding) {
                                publishEngineBuild(BenchmarkEngineBuildUpdate::Ready, runLabel);
                            }
                            return;
                        }
                        if (!buildGate.buildActive()) return;

                        const QString lower = (prefix + QLatin1Char(' ') + line).toLower();
                        const bool backendMessage =
                            lower.contains(QStringLiteral("aji")) ||
                            lower.contains(QStringLiteral("animejanai")) ||
                            lower.contains(QStringLiteral("inference shim")) ||
                            lower.contains(QStringLiteral("inference")) ||
                            lower.contains(QStringLiteral("model")) ||
                            lower.contains(QStringLiteral("backend")) ||
                            lower.contains(QStringLiteral("engine"));
                        const bool failed =
                            lower.contains(QStringLiteral("error")) ||
                            lower.contains(QStringLiteral("failed")) ||
                            lower.contains(QStringLiteral("failure"));
                        if (backendMessage && failed) {
                            buildGate.fail();
                            publishEngineBuild(BenchmarkEngineBuildUpdate::Failed, runLabel);
                        }
                    });
                }

                handle->observe("estimated-frame-number", MPV_FORMAT_DOUBLE,
                                [&observedFrame](const mplayer::Property& property) {
                                    if (property.isNumber()) observedFrame = property.number;
                                });
                handle->loadFile(path.toStdString());

                double firstFrame = -1;
                QElapsedTimer startup;
                startup.start();
                while (true) {
                    handle->drainEvents();
                    if (buildGate.failed()) break;
                    if (observedFrame > 0) firstFrame = observedFrame;
                    if (firstFrame > 0 &&
                        (!watchesEngineBuild || buildGate.readyToMeasure())) {
                        break;
                    }
                    const bool engineBuildActive =
                        watchesEngineBuild && buildGate.buildActive();
                    const qint64 elapsed =
                        engineBuildActive ? engineBuildElapsed.elapsed() : startup.elapsed();
                    const int timeout = engineBuildActive ? kBenchmarkEngineBuildTimeoutMs
                                                          : kBenchmarkStartupTimeoutMs;
                    if (elapsed >= timeout) break;
                    QThread::msleep(40);
                }
                handle->drainEvents();

                if (buildGate.failed()) {
                    append(label, clipName,
                           SettingsDialog::tr("TensorRT engine build failed"), false);
                    return;
                }
                if (watchesEngineBuild && buildGate.buildActive()) {
                    buildGate.fail();
                    publishEngineBuild(BenchmarkEngineBuildUpdate::Failed, runLabel);
                    append(label, clipName,
                           SettingsDialog::tr("TensorRT engine build timed out"), false);
                    return;
                }
                if (watchesEngineBuild && !buildGate.readyToMeasure()) {
                    buildGate.fail();
                    append(label, clipName,
                           SettingsDialog::tr("TensorRT configuration was not reported"), false);
                    return;
                }
                if (firstFrame < 0) {
                    append(label, clipName, SettingsDialog::tr("no frames"), false);
                    return;
                }

                double startFrame = firstFrame;
                QElapsedTimer measured;
                measured.start();
                publishActivity(SettingsDialog::tr("Measuring throughput · %1").arg(runLabel));
                while (measured.elapsed() < 3000) {
                    handle->drainEvents();
                    if (buildGate.failed()) {
                        append(label, clipName,
                               SettingsDialog::tr("TensorRT engine build failed"), false);
                        return;
                    }
                    if (buildGate.buildActive()) {
                        while (engineBuildElapsed.elapsed() < kBenchmarkEngineBuildTimeoutMs &&
                               buildGate.buildActive()) {
                            handle->drainEvents();
                            QThread::msleep(40);
                        }
                        handle->drainEvents();
                        if (buildGate.failed()) {
                            append(label, clipName,
                                   SettingsDialog::tr("TensorRT engine build failed"), false);
                            return;
                        }
                        if (!buildGate.readyToMeasure()) {
                            buildGate.fail();
                            publishEngineBuild(BenchmarkEngineBuildUpdate::Failed, runLabel);
                            append(label, clipName,
                                   SettingsDialog::tr("TensorRT engine build timed out"), false);
                            return;
                        }
                        startFrame = observedFrame;
                        measured.restart();
                        publishActivity(
                            SettingsDialog::tr("Measuring throughput · %1").arg(runLabel));
                        continue;
                    }
                    QThread::msleep(40);
                }
                handle->drainEvents();
                if (buildGate.failed()) {
                    append(label, clipName,
                           SettingsDialog::tr("TensorRT engine build failed"), false);
                    return;
                }

                const double endFrame = observedFrame;
                const double window = measured.elapsed() / 1000.0;
                const double fps =
                    (window > 0.2 && endFrame > startFrame)
                        ? (endFrame - startFrame) / window
                        : -1.0;
                append(label, clipName,
                       fps > 0 ? SettingsDialog::tr("%1 fps").arg(fps, 0, 'f', 1)
                               : SettingsDialog::tr("- (too slow or stalled)"),
                       fps > 0);
            } catch (const mplayer::MpvError& error) {
                QString message = QString::fromStdString(error.what());
                if (!error.property().empty()) {
                    message += QStringLiteral(" (%1)").arg(QString::fromStdString(error.property()));
                }
                append(label, clipName, message, false);
            } catch (const std::exception& error) {
                append(label, clipName, QString::fromUtf8(error.what()), false);
            }
        };

        if (measureRaw) {
            for (const QString& clip : clips) {
                measure(SettingsDialog::tr("Raw"), clip, {});
            }
        }

        for (const ModelRow& model : models) {
            if (model.key.startsWith(QLatin1String("anime4k:"))) {
                const QString shader =
                    mplayer::anime4k::shaderStringForCandidate(model.key.mid(8));
                if (shader.isEmpty()) {
                    for (const QString& clip : clips) {
                        append(model.label, clip,
                               SettingsDialog::tr("shader chain is not available"), false);
                    }
                    continue;
                }
                for (const QString& clip : clips) {
                    measure(model.label, clip, {{"glsl-shaders", shader.toStdString()}});
                }
                continue;
            }

            const mplayer::native::AnimeJanaiPlan plan =
                mplayer::native::prepareAnimeJanaiPlan(model.key, backend);
            if (!plan.available) {
                const QString reason =
                    plan.reason.isEmpty() ? SettingsDialog::tr("plan is not available")
                                          : plan.reason;
                for (const QString& clip : clips) append(model.label, clip, reason, false);
                continue;
            }
            const std::vector<std::pair<std::string, std::string>> options{
                {"hwdec", "nvdec"},
                {"vf", plan.filterSpec.toStdString()},
            };
            for (const QString& clip : clips) {
                measure(model.label, clip, options, plan.slot, plan.backend);
            }
        }

        const int resultCount = completedRuns;
        const int failedCount = failedRuns;
        QMetaObject::invokeMethod(
            application,
            [dialog, resultCount, failedCount] {
                if (dialog->isNull()) return;
                SettingsDialog* self = dialog->data();
                self->benchmarkResultStack_->setCurrentWidget(self->benchmarkResults_);
                self->benchmarkProgress_->setValue(resultCount);
                self->benchmarkEngineTimer_->stop();
                self->benchmarkEngineProgress_->hide();
                self->benchmarkRunning_ = false;
                self->benchmarkRunStatus_->setText(
                    failedCount == 0
                        ? SettingsDialog::tr("Complete · %1 results").arg(resultCount)
                        : SettingsDialog::tr("Complete · %1 results · %2 failed")
                              .arg(resultCount)
                              .arg(failedCount));
                self->benchmarkActivity_->setText(
                    failedCount == 0
                        ? SettingsDialog::tr("Benchmark finished.")
                        : SettingsDialog::tr("Benchmark finished with %1 failed runs.")
                              .arg(failedCount));
                for (QCheckBox* check : self->benchmarkClipChecks_) check->setEnabled(true);
                self->benchmarkModelSearch_->setEnabled(true);
                self->benchmarkModelTree_->setEnabled(true);
                self->benchmarkRawBox_->setEnabled(true);
                self->benchmarkSelectAllClips_->setEnabled(true);
                self->benchmarkSelectAllModels_->setEnabled(true);
                self->updateBenchmarkSelectionSummary();
            },
            Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

}  // namespace mikai::app
