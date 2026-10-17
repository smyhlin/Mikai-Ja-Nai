#include "dialogs/SettingsDialog.hpp"

#include "dialogs/settings/BenchmarkPlayback.hpp"
#include "dialogs/settings/SettingsUi.hpp"
#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"

#include <native/components/AnimeJanaiRuntime.hpp>
#include <native/models/NativeRife.hpp>
#include <native/models/NativeSpatialAssets.hpp>
#include <player/shaders/Anime4K.hpp>

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
#include <QOffscreenSurface>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QSurfaceFormat>
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

QString benchmarkElapsedText(qint64 milliseconds) {
    const qint64 seconds = milliseconds / 1000;
    return QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

QWidget* SettingsDialog::buildBenchmarkTab() {
    auto* page = new QWidget(this);
    page->setObjectName(QStringLiteral("benchmarkPage"));
    applySettingsTheme(page);

    auto* rootLayout = new QVBoxLayout(page);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* scroll = new QScrollArea(page);
    scroll->setObjectName(QStringLiteral("benchmarkScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* body = new QWidget(scroll);
    body->setObjectName(QStringLiteral("benchmarkPage"));
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(theme::kSpacingSmall, theme::kSpacingMedium,
                                   theme::kSpacingSmall, theme::kSpacingMedium);
    bodyLayout->setSpacing(theme::kSpacingMedium);

    // ── Header Card: Benchmark Overview ──────────────────────────────────────
    auto* headerWidget = new QFrame(body);
    headerWidget->setObjectName(QStringLiteral("benchmarkHeader"));
    auto* headerLayout = new QVBoxLayout(headerWidget);
    headerLayout->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium,
                                     theme::kSpacingMedium, theme::kSpacingMedium);
    headerLayout->setSpacing(theme::kSpacingXs);

    benchmarkHeading_ = new QLabel(tr("Benchmark"), headerWidget);
    stylePageHeading(benchmarkHeading_);
    headerLayout->addWidget(benchmarkHeading_);

    benchmarkDescription_ =
        new QLabel(tr("Compare rendering speed at 2× input resolution."), headerWidget);
    styleMutedCaption(benchmarkDescription_);
    headerLayout->addWidget(benchmarkDescription_);
    bodyLayout->addWidget(headerWidget);

    // ── Card 1: Video Clips & Baseline Selection ─────────────────────────────
    QVBoxLayout* clipsCardLayout = nullptr;
    QFrame* clipsCard = makeSettingsCard(body, &clipsCardLayout);
    clipsCardLayout->setSpacing(theme::kSpacingSmall);

    auto* clipsHeader = new QHBoxLayout();
    clipsHeader->setContentsMargins(0, 0, 0, 0);
    clipsHeader->setSpacing(theme::kSpacingSmall);

    benchmarkVideoTitle_ = new QLabel(tr("Video clips"), clipsCard);
    styleSectionHeading(benchmarkVideoTitle_);

    benchmarkClipCount_ = new QLabel(clipsCard);
    benchmarkClipCount_->setObjectName(QStringLiteral("benchmarkClipCount"));
    stylePillBadge(benchmarkClipCount_);

    benchmarkSelectAllClips_ = new QPushButton(tr("Select all"), clipsCard);
    benchmarkSelectAllClips_->setObjectName(QStringLiteral("textAction"));
    benchmarkSelectAllClips_->setProperty("benchmarkSelectionControl", QStringLiteral("clips"));
    polishInteractiveControl(benchmarkSelectAllClips_, theme::kSettingsControlSize);

    clipsHeader->addWidget(benchmarkVideoTitle_, 1, Qt::AlignVCenter);
    clipsHeader->addWidget(benchmarkClipCount_, 0, Qt::AlignVCenter);
    clipsHeader->addWidget(benchmarkSelectAllClips_, 0, Qt::AlignVCenter);
    clipsCardLayout->addLayout(clipsHeader);

    auto* clipGrid = new QGridLayout();
    clipGrid->setContentsMargins(0, theme::kSpacingXs, 0, theme::kSpacingXs);
    clipGrid->setHorizontalSpacing(theme::kSpacingSmall);
    clipGrid->setVerticalSpacing(theme::kSpacingSmall);
    struct ClipInfo {
        const char* file;
        const char* dimensions;
    };
    constexpr ClipInfo clips[] = {
        {"480x360.mp4", "480 × 360"},
        {"640x480.mp4", "640 × 480"},
        {"768x576.mp4", "768 × 576"},
        {"1280x720.mp4", "1280 × 720"},
        {"1920x1080.mp4", "1920 × 1080"},
    };
    benchmarkClipChecks_.clear();
    for (int index = 0; index < static_cast<int>(std::size(clips)); ++index) {
        const ClipInfo& clip = clips[index];
        auto* check = new QCheckBox(QString::fromUtf8(clip.dimensions), clipsCard);
        check->setObjectName(QStringLiteral("benchmarkClipCheck"));
        check->setProperty("benchmarkClipName", QString::fromLatin1(clip.file));
        check->setToolTip(QString::fromLatin1(clip.file));
        check->setChecked(true);
        polishInteractiveControl(check, theme::kSettingsControlSize);
        applyTabularNumerals(check);
        clipGrid->addWidget(check, index / 3, index % 3);
        benchmarkClipChecks_.append(check);
        connect(check, &QCheckBox::toggled, this, [this] { updateBenchmarkSelectionSummary(); });
    }
    clipGrid->setColumnStretch(0, 1);
    clipGrid->setColumnStretch(1, 1);
    clipGrid->setColumnStretch(2, 1);
    clipsCardLayout->addLayout(clipGrid);

    clipsCardLayout->addWidget(makeSettingsDivider(clipsCard));

    benchmarkRawBox_ = new QCheckBox(tr("Include the unfiltered baseline"), clipsCard);
    benchmarkRawBox_->setObjectName(QStringLiteral("benchmarkRawBox"));
    polishInteractiveControl(benchmarkRawBox_, theme::kSettingsControlSize);
    benchmarkRawBox_->setChecked(true);
    clipsCardLayout->addWidget(benchmarkRawBox_);

    bodyLayout->addWidget(clipsCard);

    // ── Card 2: Enhancement Models Filter & Tree ─────────────────────────────
    QVBoxLayout* modelsCardLayout = nullptr;
    QFrame* modelsCard = makeSettingsCard(body, &modelsCardLayout);
    modelsCardLayout->setSpacing(theme::kSpacingSmall);

    auto* modelsHeader = new QHBoxLayout();
    modelsHeader->setContentsMargins(0, 0, 0, 0);
    modelsHeader->setSpacing(theme::kSpacingSmall);

    benchmarkModelTitle_ = new QLabel(tr("Enhancement models"), modelsCard);
    styleSectionHeading(benchmarkModelTitle_);

    benchmarkModelCount_ = new QLabel(modelsCard);
    benchmarkModelCount_->setObjectName(QStringLiteral("benchmarkModelCount"));
    stylePillBadge(benchmarkModelCount_);

    benchmarkSelectAllModels_ = new QPushButton(tr("Select all"), modelsCard);
    benchmarkSelectAllModels_->setObjectName(QStringLiteral("textAction"));
    benchmarkSelectAllModels_->setProperty("benchmarkSelectionControl", QStringLiteral("models"));
    polishInteractiveControl(benchmarkSelectAllModels_, theme::kSettingsControlSize);

    modelsHeader->addWidget(benchmarkModelTitle_, 1, Qt::AlignVCenter);
    modelsHeader->addWidget(benchmarkModelCount_, 0, Qt::AlignVCenter);
    modelsHeader->addWidget(benchmarkSelectAllModels_, 0, Qt::AlignVCenter);
    modelsCardLayout->addLayout(modelsHeader);

    benchmarkModelSearch_ = new QLineEdit(modelsCard);
    benchmarkModelSearch_->setObjectName(QStringLiteral("benchmarkModelSearch"));
    benchmarkModelSearch_->setPlaceholderText(tr("Filter by model or group"));
    benchmarkModelSearch_->setClearButtonEnabled(true);
    benchmarkModelSearch_->setMinimumHeight(theme::kSettingsControlSize);
    benchmarkModelSearch_->setAccessibleName(tr("Filter benchmark models"));
    modelsCardLayout->addWidget(benchmarkModelSearch_);

    benchmarkModelTree_ = new QTreeWidget(modelsCard);
    benchmarkModelTree_->setObjectName(QStringLiteral("benchmarkModelTree"));
    benchmarkModelTree_->setHeaderHidden(true);
    benchmarkModelTree_->setColumnCount(1);
    benchmarkModelTree_->setIndentation(theme::kSpacingSmall);
    benchmarkModelTree_->setFixedHeight(theme::kBenchmarkSelectorHeight);
    benchmarkModelTree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    benchmarkModelTree_->setTextElideMode(Qt::ElideMiddle);
    benchmarkModelTree_->setUniformRowHeights(true);
    benchmarkModelTree_->setSelectionMode(QAbstractItemView::NoSelection);
    benchmarkModelTree_->setAccessibleName(tr("Available benchmark models"));
    modelsCardLayout->addWidget(benchmarkModelTree_);

    bodyLayout->addWidget(modelsCard);

    connect(benchmarkModelSearch_, &QLineEdit::textChanged, this,
            [this](const QString& query) { filterBenchmarkModels(query); });
    connect(benchmarkModelTree_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem* item, int) {
                if (benchmarkSelectionUpdating_ || item == nullptr) return;
                benchmarkSelectionUpdating_ = true;
                if (item->parent() == nullptr) {
                    const Qt::CheckState state =
                        item->checkState(0) == Qt::Checked ? Qt::Checked : Qt::Unchecked;
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
                    group->setCheckState(0, checked == 0
                                                ? Qt::Unchecked
                                                : checked == group->childCount()
                                                      ? Qt::Checked
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
        bool anyUnchecked = false;
        for (int groupIndex = 0; groupIndex < benchmarkModelTree_->topLevelItemCount();
             ++groupIndex) {
            const QTreeWidgetItem* group = benchmarkModelTree_->topLevelItem(groupIndex);
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

    // ── Card 3: Live Telemetry & Throughput Results Table ────────────────────
    QVBoxLayout* resultsCardLayout = nullptr;
    QFrame* resultsCard = makeSettingsCard(body, &resultsCardLayout);
    resultsCardLayout->setSpacing(theme::kSpacingSmall);

    auto* resultsHeader = new QHBoxLayout();
    resultsHeader->setContentsMargins(0, 0, 0, 0);
    resultsHeader->setSpacing(theme::kSpacingSmall);

    benchmarkResultsTitle_ = new QLabel(tr("Results"), resultsCard);
    styleSectionHeading(benchmarkResultsTitle_);

    benchmarkRunStatus_ = new QLabel(tr("Ready"), resultsCard);
    benchmarkRunStatus_->setObjectName(QStringLiteral("benchmarkRunStatus"));
    stylePillBadge(benchmarkRunStatus_);
    styleStatusBanner(benchmarkRunStatus_, QStringLiteral("neutral"));

    resultsHeader->addWidget(benchmarkResultsTitle_, 1, Qt::AlignVCenter);
    resultsHeader->addWidget(benchmarkRunStatus_, 0, Qt::AlignVCenter);
    resultsCardLayout->addLayout(resultsHeader);

    benchmarkNote_ = new QLabel(tr("Preparation excluded from FPS."), resultsCard);
    styleMutedCaption(benchmarkNote_);
    resultsCardLayout->addWidget(benchmarkNote_);

    benchmarkProgress_ = new QProgressBar(resultsCard);
    benchmarkProgress_->setObjectName(QStringLiteral("benchmarkProgress"));
    benchmarkProgress_->setTextVisible(false);
    benchmarkProgress_->setAccessibleName(tr("Benchmark run progress"));
    benchmarkProgress_->setProperty("runState", QStringLiteral("idle"));
    benchmarkProgress_->hide();
    resultsCardLayout->addWidget(benchmarkProgress_);

    benchmarkActivity_ = new QLabel(resultsCard);
    benchmarkActivity_->setObjectName(QStringLiteral("benchmarkActivity"));
    benchmarkActivity_->setProperty("textRole", QStringLiteral("muted"));
    benchmarkActivity_->setWordWrap(true);
    yieldCaptionWidth(benchmarkActivity_);
    applyTabularNumerals(benchmarkActivity_);
    benchmarkActivity_->hide();
    resultsCardLayout->addWidget(benchmarkActivity_);

    benchmarkEngineProgress_ = new QProgressBar(resultsCard);
    benchmarkEngineProgress_->setObjectName(QStringLiteral("benchmarkEngineProgress"));
    benchmarkEngineProgress_->setTextVisible(false);
    benchmarkEngineProgress_->setRange(0, 0);
    benchmarkEngineProgress_->setAccessibleName(tr("Engine build progress; indeterminate"));
    benchmarkEngineProgress_->hide();
    resultsCardLayout->addWidget(benchmarkEngineProgress_);

    benchmarkEngineElapsedLabel_ = new QLabel(resultsCard);
    benchmarkEngineElapsedLabel_->setObjectName(QStringLiteral("benchmarkEngineElapsed"));
    benchmarkEngineElapsedLabel_->setProperty("textRole", QStringLiteral("muted"));
    applyTabularNumerals(benchmarkEngineElapsedLabel_);
    benchmarkEngineElapsedLabel_->hide();
    resultsCardLayout->addWidget(benchmarkEngineElapsedLabel_);

    benchmarkEngineTimer_ = new QTimer(this);
    benchmarkEngineTimer_->setInterval(1000);
    connect(benchmarkEngineTimer_, &QTimer::timeout, this, [this] {
        if (!benchmarkEngineElapsedLabel_->isVisible()) return;
        benchmarkEngineElapsedLabel_->setText(
            tr("Engine build elapsed · %1")
                .arg(benchmarkElapsedText(benchmarkEngineBuildElapsed_.elapsed())));
    });

    benchmarkResultDetail_ = new QLabel(resultsCard);
    benchmarkResultDetail_->setObjectName(QStringLiteral("benchmarkResultDetail"));
    benchmarkResultDetail_->setWordWrap(true);
    benchmarkResultDetail_->setTextFormat(Qt::PlainText);
    benchmarkResultDetail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    yieldCaptionWidth(benchmarkResultDetail_);
    benchmarkResultDetail_->hide();
    resultsCardLayout->addWidget(benchmarkResultDetail_);

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
    benchmarkResults_->verticalHeader()->setDefaultSectionSize(theme::kSettingsControlSize);
    benchmarkResults_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    benchmarkResults_->setSelectionBehavior(QAbstractItemView::SelectRows);
    benchmarkResults_->setSelectionMode(QAbstractItemView::SingleSelection);
    benchmarkResults_->setAlternatingRowColors(true);
    benchmarkResults_->setShowGrid(false);
    benchmarkResults_->setWordWrap(false);
    applyTabularNumerals(benchmarkResults_);
    benchmarkResultStack_->addWidget(benchmarkResults_);
    benchmarkResultStack_->setFixedHeight(theme::kBenchmarkEmptyHeight);
    resultsCardLayout->addWidget(benchmarkResultStack_);

    bodyLayout->addWidget(resultsCard);
    bodyLayout->addStretch();

    connect(benchmarkResults_, &QTableWidget::itemSelectionChanged, this, [this] {
        if (benchmarkResultDetail_ == nullptr) return;
        const int row = benchmarkResults_->currentRow();
        if (row < 0) {
            benchmarkResultDetail_->clear();
            benchmarkResultDetail_->hide();
            return;
        }
        QTableWidgetItem* resItem = benchmarkResults_->item(row, 2);
        QString detail = resItem ? resItem->data(Qt::UserRole).toString() : QString();
        if (!detail.isEmpty()) {
            benchmarkResultDetail_->setText(detail);
            benchmarkResultDetail_->show();
            QTimer::singleShot(0, this, [this] {
                benchmarkScroll_->ensureWidgetVisible(benchmarkResultDetail_, 0, 0);
            });
        } else {
            benchmarkResultDetail_->hide();
        }
    });

    scroll->setWidget(body);
    benchmarkScroll_ = scroll;
    rootLayout->addWidget(scroll, 1);

    // ── Sticky Action Footer Bar ─────────────────────────────────────────────
    auto* bottomBar = new QWidget(page);
    bottomBar->setObjectName(QStringLiteral("benchmarkBottomBar"));
    auto* barLayout = new QHBoxLayout(bottomBar);
    barLayout->setContentsMargins(theme::kSpacingMedium, theme::kSpacingSmall,
                                  theme::kSpacingMedium, theme::kSpacingSmall);
    barLayout->setSpacing(theme::kSpacingMedium);

    benchmarkRunSummary_ = new QLabel(bottomBar);
    benchmarkRunSummary_->setObjectName(QStringLiteral("benchmarkRunSummary"));
    benchmarkRunSummary_->setProperty("textRole", QStringLiteral("muted"));
    yieldCaptionWidth(benchmarkRunSummary_);
    applyTabularNumerals(benchmarkRunSummary_);
    barLayout->addWidget(benchmarkRunSummary_, 1, Qt::AlignVCenter);

    benchmarkRunButton_ = new QPushButton(tr("Run benchmark"), bottomBar);
    benchmarkRunButton_->setObjectName(QStringLiteral("benchmarkRunButton"));
    benchmarkRunButton_->setProperty("benchmarkRunAction", true);
    benchmarkRunButton_->setProperty("primaryAction", true);
    polishInteractiveControl(benchmarkRunButton_, theme::kPrimaryControlSize);
    connect(benchmarkRunButton_, &QPushButton::clicked, this, &SettingsDialog::runBenchmark);
    barLayout->addWidget(benchmarkRunButton_, 0, Qt::AlignVCenter);

    rootLayout->addWidget(bottomBar);

    connect(benchmarkRawBox_, &QCheckBox::toggled, this,
            [this] { updateBenchmarkSelectionSummary(); });
    refreshBenchmarkCatalogue();
    updateBenchmarkSelectionSummary();
    return page;
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
        if (mode.mode == mplayer::anime4k::Mode::Off) continue;
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
        QStringList{tr("Clips: %1").arg(selectedClips),
                    tr("Models: %1").arg(selectedModels),
                    tr("Runs: %1").arg(runs)}
            .join(QStringLiteral(" · ")));
    benchmarkRunButton_->setEnabled(!benchmarkRunning_ && selectedClips > 0 &&
                                    (selectedModels > 0 || benchmarkRawBox_->isChecked()));
}

void SettingsDialog::retranslateBenchmarkTab() {
    if (benchmarkHeading_ == nullptr) return;
    benchmarkHeading_->setText(tr("Benchmark"));
    benchmarkDescription_->setText(tr("Compare rendering speed at 2× input resolution."));
    benchmarkVideoTitle_->setText(tr("Video clips"));
    benchmarkModelTitle_->setText(tr("Enhancement models"));
    benchmarkModelSearch_->setPlaceholderText(tr("Filter by model or group"));
    benchmarkModelSearch_->setAccessibleName(tr("Filter benchmark models"));
    benchmarkRawBox_->setText(tr("Include the unfiltered baseline"));
    benchmarkRunButton_->setText(tr("Run benchmark"));
    benchmarkResultsTitle_->setText(tr("Results"));
    benchmarkNote_->setText(tr("Preparation excluded from FPS."));
    benchmarkEmptyState_->setText(
        tr("Run a benchmark to see throughput for each selected clip and model."));
    benchmarkResults_->setHorizontalHeaderLabels({tr("Model"), tr("Clip"), tr("Throughput")});
    benchmarkProgress_->setAccessibleName(tr("Benchmark run progress"));
    benchmarkEngineProgress_->setAccessibleName(tr("Engine build progress; indeterminate"));
    benchmarkModelTree_->setAccessibleName(tr("Available benchmark models"));
    if (!benchmarkRunning_ && benchmarkResults_->rowCount() == 0) {
        benchmarkRunStatus_->setText(tr("Ready"));
        styleStatusBanner(benchmarkRunStatus_, QStringLiteral("neutral"));
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

    auto surface = std::shared_ptr<QOffscreenSurface>(
        new QOffscreenSurface(), [](QOffscreenSurface* value) { value->deleteLater(); });
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    surface->setFormat(format);
    surface->create();
    if (!surface->isValid()) {
        benchmarkActivity_->setText(tr("Could not create the offscreen OpenGL surface."));
        benchmarkActivity_->show();
        return;
    }

    benchmarkRunning_ = true;
    benchmarkResults_->setRowCount(0);
    benchmarkResultDetail_->clear();
    benchmarkResultDetail_->hide();
    benchmarkProgress_->setProperty("runState", QStringLiteral("running"));
    repolishWidget(benchmarkProgress_);
    benchmarkResultStack_->setCurrentWidget(benchmarkEmptyState_);
    benchmarkResultStack_->setFixedHeight(theme::kBenchmarkEmptyHeight);
    styleStatusBanner(benchmarkRunStatus_, QStringLiteral("running"));
    benchmarkRunStatus_->setText(tr("Benchmark running · %1 / %2").arg(0).arg(totalRuns));
    benchmarkProgress_->setRange(0, totalRuns);
    benchmarkProgress_->setValue(0);
    benchmarkProgress_->show();
    benchmarkActivity_->setText(tr("Starting benchmark…"));
    benchmarkActivity_->show();
    benchmarkRunButton_->setText(tr("Running…"));
    QTimer::singleShot(0, this, [this] {
        benchmarkScroll_->ensureWidgetVisible(benchmarkRunStatus_, 0, 0);
    });
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
        QThread::create([dialog, application, clips, models, measureRaw, totalRuns, surface] {
            int completedRuns = 0;
            int failedRuns = 0;
            const mplayer::native::Backend backend =
                mplayer::native::loadNativePreferences().backend;

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

                        auto* resultItem = new QTableWidgetItem(
                            resultRow.successful ? resultRow.result
                                                 : SettingsDialog::tr("Failed"));
                        resultItem->setToolTip(resultRow.result);
                        if (!resultRow.successful) {
                            resultItem->setData(Qt::UserRole, resultRow.result);
                        }
                        resultItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                        QFont numFont = self->benchmarkResults_->font();
                        numFont.setFeature(QFont::Tag("tnum"), 1);
                        numFont.setWeight(QFont::DemiBold);
                        resultItem->setFont(numFont);
                        resultItem->setForeground(QColor(QLatin1String(
                            resultRow.successful ? theme::kText : theme::kDanger)));
                        self->benchmarkResults_->setItem(row, 2, resultItem);
                        const int resultHeight =
                            self->benchmarkResults_->horizontalHeader()->height() +
                            std::max(2, row + 1) *
                                self->benchmarkResults_->verticalHeader()->defaultSectionSize() +
                            self->benchmarkResults_->frameWidth() * 2;
                        self->benchmarkResultStack_->setFixedHeight(
                            std::min(theme::kBenchmarkResultsMaxHeight, resultHeight));
                        self->benchmarkResultStack_->setCurrentWidget(self->benchmarkResults_);
                        if (!resultRow.successful) self->benchmarkResults_->selectRow(row);
                        self->benchmarkProgress_->setValue(completed);
                        self->benchmarkRunStatus_->setText(
                            SettingsDialog::tr("Benchmark running · %1 / %2")
                                .arg(completed)
                                .arg(totalRuns));
                    },
                    Qt::QueuedConnection);
            };

            const auto measure = [&append, &publishActivity, &publishEngineBuild, &surface, dialog](
                                     const QString& label, const QString& clipName,
                                     const std::vector<std::pair<std::string, std::string>>&
                                         options,
                                     int slot = -1,
                                     mplayer::native::Backend engineBackend =
                                         mplayer::native::Backend::DirectML) {
                const QString path = SettingsDialog::locateBenchmarkClip(clipName);
                if (path.isEmpty()) {
                    append(label, clipName, SettingsDialog::tr("clip not found"), false);
                    return;
                }

                const QStringList dimensions =
                    QFileInfo(clipName).completeBaseName().split(QLatin1Char('x'));
                const QSize sourceSize(dimensions.value(0).toInt(), dimensions.value(1).toInt());
                const QString clipLabel = QFileInfo(clipName).completeBaseName().replace(
                    QLatin1Char('x'), QStringLiteral(" × "));
                const QString runLabel = QStringLiteral("%1 · %2").arg(label, clipLabel);
                publishActivity(SettingsDialog::tr("Loading clip · %1").arg(runLabel));
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [dialog] {
                        if (dialog->isNull()) return;
                        dialog->data()->benchmarkEngineElapsedLabel_->hide();
                    },
                    Qt::QueuedConnection);
                const BenchmarkSampleResult result = runBenchmarkSample(
                    path, sourceSize * 2, options, engineBackend, slot, *surface,
                    [&publishActivity, &runLabel](const QString& phase) {
                        publishActivity(QStringLiteral("%1 · %2").arg(phase, runLabel));
                    },
                    [&publishEngineBuild, &runLabel](BenchmarkEngineBuildUpdate update,
                                                     const QString&) {
                        publishEngineBuild(update, runLabel);
                    });
                const bool successful = result.error.isEmpty() && result.fps > 0;
                append(label, clipName,
                       successful ? SettingsDialog::tr("%1 fps").arg(result.fps, 0, 'f', 1)
                                  : result.error,
                       successful);
            };

            if (measureRaw) {
                for (const QString& clip : clips) {
                    measure(SettingsDialog::tr("Baseline"), clip, {});
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
                    self->benchmarkProgress_->hide();
                    self->benchmarkActivity_->hide();
                    self->benchmarkProgress_->setProperty(
                        "runState", failedCount > 0 ? QStringLiteral("failure")
                                                    : QStringLiteral("success"));
                    repolishWidget(self->benchmarkProgress_);
                    self->benchmarkEngineElapsedLabel_->hide();
                    self->benchmarkEngineTimer_->stop();
                    self->benchmarkEngineProgress_->hide();
                    self->benchmarkRunning_ = false;
                    self->benchmarkRunButton_->setText(SettingsDialog::tr("Run benchmark"));
                    styleStatusBanner(self->benchmarkRunStatus_,
                                      failedCount == 0 ? QStringLiteral("success")
                                                       : QStringLiteral("warning"));
                    self->benchmarkRunStatus_->setText(
                        failedCount == 0
                            ? SettingsDialog::tr("Complete · %1 results").arg(resultCount)
                            : SettingsDialog::tr("Complete · %1 results · %2 failed")
                                  .arg(resultCount)
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
