#include "dialogs/SettingsDialog.hpp"

#include "dialogs/settings/BenchmarkPlayback.hpp"
#include "dialogs/settings/BenchmarkReport.hpp"
#include "dialogs/settings/SettingsUi.hpp"
#include "workers/Runner.hpp"
#include <backend/Cancellation.hpp>
#include "theme/Colors.hpp"
#include "state/AppearanceStore.hpp"
#include "theme/Palette.hpp"
#include "theme/Metrics.hpp"
#include "theme/Theme.hpp"
#include "widgets/FlowLayout.hpp"

#include <native/components/AnimeJanaiCatalogWatcher.hpp>
#include <native/components/AnimeJanaiRuntime.hpp>
#include <native/components/NativeGpuProbe.hpp>
#include <native/models/NativeSpatialAssets.hpp>
#include <native/models/NativeUpscalers.hpp>
#include <player/shaders/Anime4K.hpp>

#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QOffscreenSurface>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSet>
#include <QShortcut>
#include <QStackedWidget>
#include <QStyle>
#include <QSysInfo>
#include <QSurfaceFormat>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
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
/// A previously checked model that the live catalogue no longer offers. The
/// row stays visible so the selection is not silently dropped, but it never
/// becomes runnable and it is not user-checkable.
constexpr auto kUnavailableRole = Qt::UserRole + 2;
/// The label captured with a checked id, so a missing model can still be named.
constexpr auto kModelLabelRole = Qt::UserRole + 3;
/// Expansion captured before a filter forces groups open, so clearing the
/// filter (or a catalogue refresh mid-filter) restores what the user had open.
constexpr auto kFilterExpansionRole = Qt::UserRole + 4;

/// An explicit deselection releases a disappeared model instead of leaving an
/// unavailable row checked forever. Without this, Clear selection could not
/// clear what the catalogue no longer backs, and the next refresh would
/// resurrect the selection from checked ids.
void releaseUnavailableBenchmarkRows(QTreeWidgetItem* group) {
    std::vector<QTreeWidgetItem*> unavailable;
    for (int index = 0; index < group->childCount(); ++index) {
        QTreeWidgetItem* child = group->child(index);
        if (child->data(0, kUnavailableRole).toBool()) unavailable.push_back(child);
    }
    for (QTreeWidgetItem* child : unavailable) delete child;
}

struct BenchmarkResultRow {
    QString model;
    QString clip;
    QString result;
    bool successful = false;
};

QString benchmarkBackendId(mplayer::native::Backend backend) {
    return mplayer::native::stableName(backend);
}

/// The engine's name as a person reads it. The benchmark has no picker of its
/// own: the engine comes from the Components selection, so this only reports.
QString benchmarkEngineName(mplayer::native::Backend backend) {
    switch (backend) {
        case mplayer::native::Backend::TensorRT: return SettingsDialog::tr("TensorRT");
        case mplayer::native::Backend::DirectML: return SettingsDialog::tr("DirectML");
        case mplayer::native::Backend::TensorRTRTX:
            return SettingsDialog::tr("TensorRT for RTX");
    }
    return QString();
}

// Read the player's Standard-AJI choice, not the private Native preference.
std::optional<mplayer::native::Backend> storedStandardAnimeJanaiBackendChoice() {
    const QString stored =
        QSettings().value(QStringLiteral("player/animejanai/backend")).toString();
    return mplayer::native::backendFromName(stored);
}

#ifdef Q_OS_WIN
// Preserve the GPU-detected default when no Standard choice exists.
mplayer::native::Backend storedStandardAnimeJanaiBackend() {
    if (const auto backend = storedStandardAnimeJanaiBackendChoice()) {
        return *backend;
    }
    const auto& gpu = mplayer::native::observeComputeCapability();
    return mplayer::native::defaultAnimeJanaiBackend(gpu);
}
#endif

QString benchmarkElapsedText(qint64 milliseconds) {
    const qint64 seconds = milliseconds / 1000;
    return QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

// Throughput is the numeric column; keep its header aligned with the figures.
void alignBenchmarkThroughputColumn(QTableWidget* table) {
    if (table == nullptr) return;
    if (QTableWidgetItem* header = table->horizontalHeaderItem(2)) {
        header->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
}

// Secondary lines sit one step in from the heading they explain. Headings stay
// flush so larger text is always further left than the caption under it.
void indentUnderHeading(QLabel* label) {
    if (label == nullptr) return;
    label->setContentsMargins(theme::kSpacingSmall, 0, 0, 0);
}

QString benchmarkResultsTsv(const QTableWidget* table, bool selectedOnly) {
    if (table == nullptr || table->rowCount() == 0) return {};
    if (selectedOnly && table->currentRow() < 0) return {};
    QStringList lines;
    lines.append(QStringList{SettingsDialog::tr("Model"), SettingsDialog::tr("Clip"),
                             SettingsDialog::tr("Throughput")}
                     .join(QLatin1Char('\t')));
    for (int row = 0; row < table->rowCount(); ++row) {
        if (selectedOnly && table->currentRow() != row) continue;
        const auto cell = [table, row](int column) {
            const QTableWidgetItem* item = table->item(row, column);
            return item != nullptr ? item->text() : QString();
        };
        QString line = QStringList{cell(0), cell(1), cell(2)}.join(QLatin1Char('\t'));
        const QTableWidgetItem* result = table->item(row, 2);
        const QString detail = result != nullptr ? result->data(Qt::UserRole).toString() : QString();
        if (!detail.isEmpty()) line += QLatin1Char('\t') + detail;
        lines.append(line);
    }
    return lines.join(QLatin1Char('\n'));
}

}  // namespace

QWidget* SettingsDialog::buildBenchmarkTab() {
    auto* page = new QWidget(this);
    page->setObjectName(QStringLiteral("benchmarkPage"));

    auto* rootLayout = new QVBoxLayout(page);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* scroll = new QScrollArea(page);
    scroll->setObjectName(QStringLiteral("benchmarkScroll"));
    scroll->setWidgetResizable(true);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

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
    benchmarkHeading_->setContentsMargins(0, 0, 0, 0);
    benchmarkHeading_->setWordWrap(true);
    benchmarkHeading_->setMinimumWidth(0);
    headerLayout->addWidget(benchmarkHeading_);

    benchmarkDescription_ =
        new QLabel(tr("Compare rendering speed at 2× input resolution."), headerWidget);
    styleMutedCaption(benchmarkDescription_);
    benchmarkDescription_->setWordWrap(true);
    indentUnderHeading(benchmarkDescription_);
    yieldCaptionWidth(benchmarkDescription_);
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
    benchmarkVideoTitle_->setContentsMargins(0, 0, 0, 0);
    benchmarkVideoTitle_->setWordWrap(true);
    benchmarkVideoTitle_->setMinimumWidth(0);

    benchmarkClipCount_ = new QLabel(clipsCard);
    benchmarkClipCount_->setObjectName(QStringLiteral("benchmarkClipCount"));
    stylePillBadge(benchmarkClipCount_);
    applyTabularNumerals(benchmarkClipCount_);

    benchmarkSelectAllClips_ = new QPushButton(tr("Select all"), clipsCard);
    benchmarkSelectAllClips_->setObjectName(QStringLiteral("benchmarkSelectAllClips"));
    benchmarkSelectAllClips_->setProperty("benchmarkSelectionControl", QStringLiteral("clips"));
    theme::applyActionRole(benchmarkSelectAllClips_, theme::ActionRole::Ghost);
    benchmarkSelectAllClips_->setAutoDefault(false);
    benchmarkSelectAllClips_->setDefault(false);
    benchmarkSelectAllClips_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    polishInteractiveControl(benchmarkSelectAllClips_, theme::kSettingsControlSize);

    clipsHeader->addWidget(benchmarkVideoTitle_, 1, Qt::AlignVCenter);
    clipsHeader->addWidget(benchmarkClipCount_, 0, Qt::AlignVCenter);
    clipsHeader->addWidget(benchmarkSelectAllClips_, 0, Qt::AlignVCenter);
    clipsCardLayout->addLayout(clipsHeader);

    // The tiles wrap onto as many lines as the card has room for. A fixed
    // column count reserves each column's widest tile on every line, which is
    // wider than the page at the dialog's minimum and hides the row behind a
    // sideways pan.
    auto* clipGrid = new FlowLayout(theme::kSpacingSmall, theme::kSpacingSmall);
    clipGrid->setContentsMargins(0, theme::kSpacingXs, 0, theme::kSpacingXs);
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
        clipGrid->addWidget(check);
        benchmarkClipChecks_.append(check);
        connect(check, &QCheckBox::toggled, this, [this] { updateBenchmarkSelectionSummary(); });
    }
    clipsCardLayout->addLayout(clipGrid);

    clipsCardLayout->addWidget(makeSettingsDivider(clipsCard));

    benchmarkRawBox_ = new WrappingCheckBox(clipsCard);
    benchmarkRawBox_->setText(tr("Include the unfiltered baseline"));
    benchmarkRawBox_->setObjectName(QStringLiteral("benchmarkRawBox"));
    polishInteractiveControl(benchmarkRawBox_, theme::kSettingsControlSize);
    benchmarkRawBox_->setChecked(true);
    benchmarkRawBox_->setToolTip(
        tr("Measures playback with no shader or model, so filtered runs have a reference."));
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
    benchmarkModelTitle_->setContentsMargins(0, 0, 0, 0);
    benchmarkModelTitle_->setWordWrap(true);
    benchmarkModelTitle_->setMinimumWidth(0);

    benchmarkModelCount_ = new QLabel(modelsCard);
    benchmarkModelCount_->setObjectName(QStringLiteral("benchmarkModelCount"));
    stylePillBadge(benchmarkModelCount_);
    applyTabularNumerals(benchmarkModelCount_);

    benchmarkSelectAllModels_ = new QPushButton(tr("Select all"), modelsCard);
    benchmarkSelectAllModels_->setObjectName(QStringLiteral("benchmarkSelectAllModels"));
    benchmarkSelectAllModels_->setProperty("benchmarkSelectionControl", QStringLiteral("models"));
    theme::applyActionRole(benchmarkSelectAllModels_, theme::ActionRole::Ghost);
    benchmarkSelectAllModels_->setAutoDefault(false);
    benchmarkSelectAllModels_->setDefault(false);
    benchmarkSelectAllModels_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    polishInteractiveControl(benchmarkSelectAllModels_, theme::kSettingsControlSize);

    modelsHeader->addWidget(benchmarkModelTitle_, 1, Qt::AlignVCenter);
    modelsHeader->addWidget(benchmarkModelCount_, 0, Qt::AlignVCenter);
    modelsHeader->addWidget(benchmarkSelectAllModels_, 0, Qt::AlignVCenter);
    modelsCardLayout->addLayout(modelsHeader);

    benchmarkModelSearch_ = new QLineEdit(modelsCard);
    benchmarkModelSearch_->setObjectName(QStringLiteral("benchmarkModelSearch"));
    benchmarkModelSearch_->setPlaceholderText(tr("Filter by model, group, or file"));
    benchmarkModelSearch_->setClearButtonEnabled(true);
    benchmarkModelSearch_->setMinimumHeight(theme::kSettingsControlSize);
    benchmarkModelSearch_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    benchmarkModelSearch_->setAccessibleName(tr("Filter benchmark models"));
    benchmarkModelSearch_->setToolTip(
        tr("Show models whose name, group, file, or id matches. Clearing restores open groups."));
    modelsCardLayout->addWidget(benchmarkModelSearch_);

    benchmarkModelTree_ = new QTreeWidget(modelsCard);
    benchmarkModelTree_->setObjectName(QStringLiteral("benchmarkModelTree"));
    benchmarkModelTree_->setHeaderHidden(true);
    benchmarkModelTree_->setColumnCount(1);
    // Disclosure arrows need more than the 8px text step or they collide with
    // the checkbox hit target.
    benchmarkModelTree_->setIndentation(theme::kSpacingMedium + theme::kSpacingXs);
    benchmarkModelTree_->setFixedHeight(theme::kBenchmarkSelectorHeight);
    benchmarkModelTree_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    benchmarkModelTree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    benchmarkModelTree_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    benchmarkModelTree_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    benchmarkModelTree_->setTextElideMode(Qt::ElideRight);
    benchmarkModelTree_->setUniformRowHeights(true);
    benchmarkModelTree_->setSelectionMode(QAbstractItemView::NoSelection);
    benchmarkModelTree_->setTabKeyNavigation(false);
    benchmarkModelTree_->setFocusPolicy(Qt::StrongFocus);
    benchmarkModelTree_->setAccessibleName(tr("Available benchmark models"));
    benchmarkModelTree_->setAccessibleDescription(
        tr("Check the models to measure. A group checkbox selects every model in that group. Space toggles the focused row."));
    benchmarkModelTree_->setToolTip(
        tr("Space toggles the focused model. A group checkbox selects the whole group."));
    modelsCardLayout->addWidget(benchmarkModelTree_);

    bodyLayout->addWidget(modelsCard);

    connect(benchmarkModelSearch_, &QLineEdit::textChanged, this,
            [this](const QString& query) { filterBenchmarkModels(query); });
    connect(benchmarkModelTree_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem* item, int) {
                if (benchmarkSelectionUpdating_ || item == nullptr) return;
                if (item->data(0, kUnavailableRole).toBool()) return;
                benchmarkSelectionUpdating_ = true;
                if (item->parent() == nullptr) {
                    const Qt::CheckState state =
                        item->checkState(0) == Qt::Checked ? Qt::Checked : Qt::Unchecked;
                    for (int index = 0; index < item->childCount(); ++index) {
                        QTreeWidgetItem* child = item->child(index);
                        if (child->data(0, kUnavailableRole).toBool()) continue;
                        child->setCheckState(0, state);
                    }
                    if (state == Qt::Unchecked) releaseUnavailableBenchmarkRows(item);
                    item->setCheckState(0, state);
                } else {
                    QTreeWidgetItem* group = item->parent();
                    int checked = 0;
                    int available = 0;
                    for (int index = 0; index < group->childCount(); ++index) {
                        const QTreeWidgetItem* child = group->child(index);
                        if (child->data(0, kUnavailableRole).toBool()) continue;
                        ++available;
                        checked += child->checkState(0) == Qt::Checked ? 1 : 0;
                    }
                    group->setCheckState(0, available == 0 || checked == 0
                                                ? Qt::Unchecked
                                                : checked == available ? Qt::Checked
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
                const QTreeWidgetItem* child = group->child(index);
                if (child->data(0, kUnavailableRole).toBool()) continue;
                anyUnchecked |= child->checkState(0) != Qt::Checked;
            }
        }
        benchmarkSelectionUpdating_ = true;
        const Qt::CheckState state = anyUnchecked ? Qt::Checked : Qt::Unchecked;
        for (int groupIndex = 0; groupIndex < benchmarkModelTree_->topLevelItemCount();
             ++groupIndex) {
            QTreeWidgetItem* group = benchmarkModelTree_->topLevelItem(groupIndex);
            int available = 0;
            int checked = 0;
            for (int index = 0; index < group->childCount(); ++index) {
                QTreeWidgetItem* child = group->child(index);
                if (child->data(0, kUnavailableRole).toBool()) continue;
                ++available;
                child->setCheckState(0, state);
                checked += state == Qt::Checked ? 1 : 0;
            }
            if (state == Qt::Unchecked) releaseUnavailableBenchmarkRows(group);
            group->setCheckState(0, available == 0 || checked == 0
                                        ? Qt::Unchecked
                                        : checked == available ? Qt::Checked
                                                               : Qt::PartiallyChecked);
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
    benchmarkResultsTitle_->setContentsMargins(0, 0, 0, 0);
    benchmarkResultsTitle_->setWordWrap(true);
    benchmarkResultsTitle_->setMinimumWidth(0);

    benchmarkRunStatus_ = new QLabel(tr("Ready"), resultsCard);
    benchmarkRunStatus_->setObjectName(QStringLiteral("benchmarkRunStatus"));
    stylePillBadge(benchmarkRunStatus_);
    styleStatusBanner(benchmarkRunStatus_, QStringLiteral("neutral"));
    applyTabularNumerals(benchmarkRunStatus_);

    resultsHeader->addWidget(benchmarkResultsTitle_, 1, Qt::AlignVCenter);
    resultsHeader->addWidget(benchmarkRunStatus_, 0, Qt::AlignVCenter);
    resultsCardLayout->addLayout(resultsHeader);

    benchmarkNote_ = new QLabel(tr("Preparation excluded from FPS."), resultsCard);
    styleMutedCaption(benchmarkNote_);
    benchmarkNote_->setWordWrap(true);
    indentUnderHeading(benchmarkNote_);
    yieldCaptionWidth(benchmarkNote_);
    resultsCardLayout->addWidget(benchmarkNote_);

    benchmarkProgress_ = new QProgressBar(resultsCard);
    benchmarkProgress_->setObjectName(QStringLiteral("benchmarkProgress"));
    benchmarkProgress_->setTextVisible(false);
    benchmarkProgress_->setAccessibleName(tr("Benchmark run progress"));
    benchmarkProgress_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    benchmarkProgress_->setProperty("runState", QStringLiteral("idle"));
    benchmarkProgress_->hide();
    resultsCardLayout->addWidget(benchmarkProgress_);

    benchmarkActivity_ = new QLabel(resultsCard);
    benchmarkActivity_->setObjectName(QStringLiteral("benchmarkActivity"));
    benchmarkActivity_->setProperty("textRole", QStringLiteral("muted"));
    benchmarkActivity_->setWordWrap(true);
    indentUnderHeading(benchmarkActivity_);
    yieldCaptionWidth(benchmarkActivity_);
    applyTabularNumerals(benchmarkActivity_);
    benchmarkActivity_->hide();
    resultsCardLayout->addWidget(benchmarkActivity_);

    benchmarkEngineProgress_ = new QProgressBar(resultsCard);
    benchmarkEngineProgress_->setObjectName(QStringLiteral("benchmarkEngineProgress"));
    benchmarkEngineProgress_->setTextVisible(false);
    benchmarkEngineProgress_->setRange(0, 0);
    benchmarkEngineProgress_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    benchmarkEngineProgress_->setAccessibleName(tr("Engine build progress; indeterminate"));
    benchmarkEngineProgress_->hide();
    resultsCardLayout->addWidget(benchmarkEngineProgress_);

    benchmarkEngineElapsedLabel_ = new QLabel(resultsCard);
    benchmarkEngineElapsedLabel_->setObjectName(QStringLiteral("benchmarkEngineElapsed"));
    benchmarkEngineElapsedLabel_->setProperty("textRole", QStringLiteral("muted"));
    indentUnderHeading(benchmarkEngineElapsedLabel_);
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
    indentUnderHeading(benchmarkResultDetail_);
    yieldCaptionWidth(benchmarkResultDetail_);
    benchmarkResultDetail_->hide();

    benchmarkResultStack_ = new QStackedWidget(resultsCard);
    benchmarkEmptyState_ = new QLabel(
        tr("Run a benchmark to see throughput for each selected clip and model."),
        benchmarkResultStack_);
    benchmarkEmptyState_->setObjectName(QStringLiteral("benchmarkEmptyState"));
    benchmarkEmptyState_->setWordWrap(true);
    benchmarkEmptyState_->setAlignment(Qt::AlignCenter);
    yieldCaptionWidth(benchmarkEmptyState_);
    benchmarkResultStack_->addWidget(benchmarkEmptyState_);

    benchmarkResults_ = new QTableWidget(benchmarkResultStack_);
    benchmarkResults_->setObjectName(QStringLiteral("benchmarkResults"));
    benchmarkResults_->setColumnCount(3);
    benchmarkResults_->setHorizontalHeaderLabels({tr("Model"), tr("Clip"), tr("Throughput")});
    alignBenchmarkThroughputColumn(benchmarkResults_);
    benchmarkResults_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    benchmarkResults_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    benchmarkResults_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    benchmarkResults_->horizontalHeader()->setHighlightSections(false);
    benchmarkResults_->horizontalHeader()->setStretchLastSection(false);
    benchmarkResults_->verticalHeader()->hide();
    benchmarkResults_->verticalHeader()->setDefaultSectionSize(theme::kSettingsControlSize);
    benchmarkResults_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    benchmarkResults_->setSelectionBehavior(QAbstractItemView::SelectRows);
    benchmarkResults_->setSelectionMode(QAbstractItemView::SingleSelection);
    benchmarkResults_->setAlternatingRowColors(true);
    benchmarkResults_->setShowGrid(false);
    benchmarkResults_->setWordWrap(false);
    benchmarkResults_->setTextElideMode(Qt::ElideRight);
    benchmarkResults_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    benchmarkResults_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    benchmarkResults_->setTabKeyNavigation(false);
    benchmarkResults_->setCornerButtonEnabled(false);
    benchmarkResults_->setSortingEnabled(false);
    benchmarkResults_->setFocusPolicy(Qt::StrongFocus);
    benchmarkResults_->setAccessibleName(tr("Benchmark throughput results"));
    benchmarkResults_->setAccessibleDescription(
        tr("Each row is one clip measured with one model. Select a failed row for the reason. Ctrl+C copies the row."));
    applyTabularNumerals(benchmarkResults_);
    benchmarkResultStack_->addWidget(benchmarkResults_);
    benchmarkResultStack_->setFixedHeight(theme::kBenchmarkEmptyHeight);
    benchmarkResultStack_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    resultsCardLayout->addWidget(benchmarkResultStack_);
    // Failure text sits under the row that produced it, not above the table,
    // so reading a result does not scroll the row out of view.
    resultsCardLayout->addWidget(benchmarkResultDetail_);

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

    const auto copyResults = [this](bool selectedOnly) {
        const QString text = benchmarkResultsTsv(benchmarkResults_, selectedOnly);
        if (text.isEmpty()) return;
        if (QClipboard* clipboard = QGuiApplication::clipboard()) clipboard->setText(text);
    };
    auto* copyShortcut = new QShortcut(QKeySequence::Copy, benchmarkResults_);
    copyShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copyShortcut, &QShortcut::activated, this, [copyResults] { copyResults(true); });
    benchmarkResults_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(benchmarkResults_, &QTableWidget::customContextMenuRequested, this,
            [this, copyResults](const QPoint& pos) {
                if (benchmarkResults_->rowCount() == 0) return;
                QMenu menu(benchmarkResults_);
                QAction* copyRow = menu.addAction(tr("Copy row"));
                copyRow->setEnabled(benchmarkResults_->currentRow() >= 0);
                QAction* copyAll = menu.addAction(tr("Copy all results"));
                const QAction* chosen = menu.exec(benchmarkResults_->viewport()->mapToGlobal(pos));
                if (chosen == copyRow) copyResults(true);
                else if (chosen == copyAll) copyResults(false);
            });

    auto* findModels = new QShortcut(QKeySequence::Find, page);
    findModels->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findModels, &QShortcut::activated, this, [this] {
        if (benchmarkModelSearch_ == nullptr || !benchmarkModelSearch_->isEnabled()) return;
        benchmarkModelSearch_->setFocus(Qt::ShortcutFocusReason);
        benchmarkModelSearch_->selectAll();
    });

    scroll->setWidget(body);
    benchmarkScroll_ = scroll;
    rootLayout->addWidget(scroll, 1);

    // ── Sticky Action Footer Bar ─────────────────────────────────────────────
    // Clamp the bar. A wrapping summary beside stretch 0 content otherwise
    // grows into a tall empty band under the scroll area.
    auto* bottomBar = new QWidget(page);
    bottomBar->setObjectName(QStringLiteral("benchmarkBottomBar"));
    bottomBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    const int benchmarkFooterHeight = theme::kControlHeight + theme::kSpacingSmall * 2;
    bottomBar->setMinimumHeight(benchmarkFooterHeight);
    // Two lines of a translated summary, never a stretched empty band.
    bottomBar->setMaximumHeight(benchmarkFooterHeight * 2);
    auto* barLayout = new QHBoxLayout(bottomBar);
    barLayout->setContentsMargins(theme::kSpacingMedium, theme::kSpacingSmall,
                                  theme::kSpacingMedium, theme::kSpacingSmall);
    barLayout->setSpacing(theme::kSpacingMedium);

    benchmarkRunSummary_ = new QLabel(bottomBar);
    benchmarkRunSummary_->setObjectName(QStringLiteral("benchmarkRunSummary"));
    benchmarkRunSummary_->setProperty("textRole", QStringLiteral("muted"));
    benchmarkRunSummary_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    yieldCaptionWidth(benchmarkRunSummary_);
    benchmarkRunSummary_->setMinimumWidth(0);
    applyTabularNumerals(benchmarkRunSummary_);
    barLayout->addWidget(benchmarkRunSummary_, 1, Qt::AlignVCenter);

    benchmarkEngineChoice_ = new QLabel(bottomBar);
    benchmarkEngineChoice_->setObjectName(QStringLiteral("benchmarkEngineChoice"));
    benchmarkEngineChoice_->setProperty("textRole", QStringLiteral("muted"));
    benchmarkEngineChoice_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    barLayout->addWidget(benchmarkEngineChoice_, 0, Qt::AlignVCenter);

    benchmarkRunButton_ = new QPushButton(tr("Run benchmark"), bottomBar);
    benchmarkRunButton_->setObjectName(QStringLiteral("benchmarkRunButton"));
    benchmarkRunButton_->setProperty("benchmarkRunAction", true);
    benchmarkRunButton_->setAutoDefault(false);
    benchmarkRunButton_->setDefault(false);
    benchmarkRunButton_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    benchmarkRunButton_->setFocusPolicy(Qt::StrongFocus);
    theme::applyActionRole(benchmarkRunButton_, theme::ActionRole::Primary);
    polishInteractiveControl(benchmarkRunButton_, theme::kControlHeight);
    connect(benchmarkRunButton_, &QPushButton::clicked, this, [this] {
        if (benchmarkRunning_) {
            if (benchmarkCancellation_) {
                benchmarkCancellation_->store(true);
                benchmarkRunButton_->setEnabled(false);
                benchmarkRunStatus_->setText(tr("Cancelling…"));
                styleStatusBanner(benchmarkRunStatus_, QStringLiteral("neutral"));
            }
            return;
        }
        runBenchmark();
    });
    barLayout->addWidget(benchmarkRunButton_, 0, Qt::AlignVCenter);

    rootLayout->addWidget(bottomBar);

    connect(benchmarkRawBox_, &QCheckBox::toggled, this,
            [this] { updateBenchmarkSelectionSummary(); });

    // The catalogue is a filesystem-backed inventory: a graph pasted, renamed,
    // replaced or removed while this tab is open must appear without a tab
    // switch or a dialog restart. The watcher only announces metadata changes;
    // the rebuild below preserves what the user was doing.
    auto* catalogueWatcher = new mplayer::native::AnimeJanaiCatalogWatcher(page);
    connect(catalogueWatcher, &mplayer::native::AnimeJanaiCatalogWatcher::catalogChanged, this,
            [this] { refreshBenchmarkCatalogue(); });
    catalogueWatcher->refresh();

    refreshBenchmarkCatalogue();
    updateBenchmarkSelectionSummary();
    return page;
}

void SettingsDialog::refreshBenchmarkCatalogue() {
    if (benchmarkModelTree_ == nullptr) return;

    // A filesystem refresh is not a fresh page: keep what the user checked,
    // where they were looking, how the groups were expanded, and the filter.
    QHash<QString, QString> checkedIds;
    QHash<QString, bool> groupExpansion;
    QString currentId;
    const bool hasPreviousCatalogue = benchmarkModelTree_->topLevelItemCount() > 0;
    const int previousScrollY = benchmarkModelTree_->verticalScrollBar()->value();
    const int previousScrollX = benchmarkModelTree_->horizontalScrollBar()->value();
    if (hasPreviousCatalogue) {
        for (int groupIndex = 0; groupIndex < benchmarkModelTree_->topLevelItemCount();
             ++groupIndex) {
            const QTreeWidgetItem* group = benchmarkModelTree_->topLevelItem(groupIndex);
            const QString groupKey = group->data(0, Qt::UserRole).toString();
            const QVariant heldOpen = group->data(0, kFilterExpansionRole);
            groupExpansion.insert(groupKey.isEmpty() ? group->text(0) : groupKey,
                                  heldOpen.isValid() ? heldOpen.toBool() : group->isExpanded());
            for (int index = 0; index < group->childCount(); ++index) {
                const QTreeWidgetItem* item = group->child(index);
                if (item->checkState(0) == Qt::Checked) {
                    const QString storedLabel = item->data(0, kModelLabelRole).toString();
                    checkedIds.insert(benchmarkModelId(item),
                                      storedLabel.isEmpty() ? item->text(0) : storedLabel);
                }
            }
        }
        if (const QTreeWidgetItem* current = benchmarkModelTree_->currentItem();
            current != nullptr && current->parent() != nullptr) {
            currentId = benchmarkModelId(current);
        }
    }

    benchmarkSelectionUpdating_ = true;
    struct BenchmarkTreeUpdates {
        QTreeWidget* tree;
        explicit BenchmarkTreeUpdates(QTreeWidget* value) : tree(value) {
            tree->setUpdatesEnabled(false);
        }
        ~BenchmarkTreeUpdates() { tree->setUpdatesEnabled(true); }
    } holdTreeUpdates(benchmarkModelTree_);
    benchmarkModelTree_->clear();

    const auto addGroup = [this, &groupExpansion](const QString& key, const QString& title) {
        auto* group = new QTreeWidgetItem(benchmarkModelTree_);
        group->setText(0, title);
        group->setData(0, Qt::UserRole, key);
        QFont heading = benchmarkModelTree_->font();
        heading.setWeight(QFont::DemiBold);
        group->setFont(0, heading);
        group->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        group->setToolTip(0, tr("Check to select every available model in this group."));
        group->setExpanded(
            groupExpansion.value(key, key == QLatin1String("processing:upscale")));
        return group;
    };
    QSet<QString> presentIds;
    const auto addModel = [this, &checkedIds, &presentIds, hasPreviousCatalogue](
                              QTreeWidgetItem* group, const QString& key, const QString& label,
                              const QString& tooltip, bool defaultChecked, bool supported) {
        if (key.isEmpty() || presentIds.contains(key)) return;
        auto* item = new QTreeWidgetItem(group);
        item->setText(0, label);
        item->setFlags(supported ? Qt::ItemIsEnabled | Qt::ItemIsUserCheckable
                                 : Qt::ItemIsEnabled);
        item->setData(0, kUnavailableRole, !supported);
        const bool checked =
            hasPreviousCatalogue ? checkedIds.contains(key) : (defaultChecked && supported);
        item->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
        item->setData(0, kModelRole, key);
        item->setData(0, kModelLabelRole, label);
        item->setToolTip(0, tooltip);
        presentIds.insert(key);
    };

    QTreeWidgetItem* restoreGroup =
        addGroup(QStringLiteral("processing:restore"),
                 QCoreApplication::translate("QObject", "Restore (Anime4K)"));
    for (const mplayer::anime4k::ModeInfo& mode : mplayer::anime4k::availableModes()) {
        if (mode.mode == mplayer::anime4k::Mode::Off) continue;
        addModel(restoreGroup, QStringLiteral("anime4k:") + mode.id,
                 mplayer::anime4k::displayLabel(mode.mode), mode.hint, false, true);
    }

    const QVector<mplayer::native::AnimeJanaiPreset> presets =
        mplayer::native::animeJanaiPresets();
    const mplayer::native::AnimeJanaiConfig managerConfig =
        mplayer::native::AnimeJanaiConfigStore::load();
    const QVector<mplayer::native::AnimeJanaiSlot> defaultProfiles =
        mplayer::native::AnimeJanaiConfigStore::defaultProfiles(managerConfig);
    const QVector<mplayer::native::StoredSpatialAsset> storedSpatial =
        mplayer::native::storedSpatialAssets();
    QTreeWidgetItem* upscaleGroup =
        addGroup(QStringLiteral("processing:upscale"),
                 QCoreApplication::translate("QObject", "Upscale"));
    QTreeWidgetItem* interpolationGroup =
        addGroup(QStringLiteral("processing:interpolation"),
                 QCoreApplication::translate("QObject", "Interpolation (RIFE)"));

    const auto profileUsesRife = [](const mplayer::native::AnimeJanaiPreset& preset,
                                    const QVector<mplayer::native::AnimeJanaiSlot>& profiles) {
        if (preset.rife || preset.id.startsWith(QLatin1String("rife:"))) return true;
        for (const mplayer::native::AnimeJanaiSlot& profile : profiles) {
            if (profile.number != preset.slot) continue;
            return std::any_of(profile.chains.cbegin(), profile.chains.cend(),
                               [](const mplayer::native::AnimeJanaiChain& chain) {
                                   return chain.rifeEnabled;
                               });
        }
        return false;
    };

    QSet<QString> upscalerStems;
    for (const mplayer::native::UpscalerModel& u : mplayer::native::upscalerCatalog()) {
        upscalerStems.insert(u.fileName.toLower());
        upscalerStems.insert(QFileInfo(u.fileName).completeBaseName().toLower());
    }

    for (const mplayer::native::AnimeJanaiPreset& preset : presets) {
        if (preset.id.startsWith(QLatin1String("spatial:custom:"))) {
            const QString stem = preset.id.mid(QStringLiteral("spatial:custom:").size()).toLower();
            if (upscalerStems.contains(stem)) continue;
        }

        const auto& profiles = preset.id.startsWith(QLatin1String("builtin_slot:"))
                                   ? managerConfig.customSlots
                                   : defaultProfiles;
        const bool interpolation = profileUsesRife(preset, profiles);
        QString tooltip = preset.supported
                              ? tr("slot %1 · %2")
                                    .arg(preset.slot)
                                    .arg(preset.modelFile.isEmpty() ? tr("built-in chain")
                                                                   : preset.modelFile)
                              : preset.reason;
        bool supported = preset.supported;
        if (preset.id.startsWith(QLatin1String("spatial:")) &&
            !preset.id.startsWith(QLatin1String("spatial:custom:"))) {
            const QString assetId = preset.id.mid(QStringLiteral("spatial:").size());
            const auto stored = std::find_if(
                storedSpatial.cbegin(), storedSpatial.cend(),
                [&assetId](const mplayer::native::StoredSpatialAsset& row) {
                    return row.asset.id == assetId;
                });
            if (stored == storedSpatial.cend()) {
                supported = false;
                tooltip = tr("Not downloaded. Install this graph from Components.");
            } else if (!stored->verified) {
                supported = false;
                tooltip = stored->reason.isEmpty()
                              ? tr("The installed graph could not be verified.")
                              : stored->reason;
            }
        }
        const bool defaultChecked = preset.id.startsWith(QLatin1String("builtin")) &&
                                    !interpolation;
        addModel(interpolation ? interpolationGroup : upscaleGroup, preset.id,
                 preset.label, tooltip, defaultChecked, supported);
    }

    for (const mplayer::native::UpscalerModel& model : mplayer::native::upscalerCatalog()) {
        const QString id = QStringLiteral("upscaler:") + model.fileName;
        const bool installed = !mplayer::native::installedUpscalerPath(model).isEmpty();
        addModel(upscaleGroup, id, model.displayName,
                 installed ? tr("%1× · %2").arg(model.scale).arg(model.precision)
                           : tr("Not downloaded. Install this graph from Settings › Upscalers."),
                 false, installed);
    }

    // A checked model that the live catalogue no longer offers stays visible
    // and named, but cannot be run; it is never replaced by a similar model.
    const auto groupForId = [restoreGroup, upscaleGroup, interpolationGroup](const QString& key) {
        if (key.startsWith(QLatin1String("anime4k:"))) return restoreGroup;
        if (key.startsWith(QLatin1String("rife:"))) return interpolationGroup;
        return upscaleGroup;
    };
    for (auto it = checkedIds.cbegin(); it != checkedIds.cend(); ++it) {
        if (presentIds.contains(it.key())) continue;
        auto* item = new QTreeWidgetItem(groupForId(it.key()));
        item->setText(0, tr("%1 (unavailable)").arg(it.value()));
        item->setFlags(Qt::ItemIsEnabled);
        item->setCheckState(0, Qt::Checked);
        item->setData(0, kModelRole, it.key());
        item->setData(0, kModelLabelRole, it.value());
        item->setData(0, kUnavailableRole, true);
        item->setToolTip(0, tr("This model is no longer in the catalogue; it will not run."));
    }

    const auto updateGroupState = [](QTreeWidgetItem* group) {
        int checked = 0;
        int available = 0;
        for (int index = 0; index < group->childCount(); ++index) {
            const QTreeWidgetItem* item = group->child(index);
            if (item->data(0, kUnavailableRole).toBool()) continue;
            ++available;
            checked += item->checkState(0) == Qt::Checked ? 1 : 0;
        }
        group->setCheckState(0, available == 0 || checked == 0
                                    ? Qt::Unchecked
                                    : checked == available ? Qt::Checked
                                                           : Qt::PartiallyChecked);
    };
    for (int groupIndex = 0; groupIndex < benchmarkModelTree_->topLevelItemCount(); ++groupIndex) {
        updateGroupState(benchmarkModelTree_->topLevelItem(groupIndex));
    }
    benchmarkSelectionUpdating_ = false;
    for (int groupIndex = 0; groupIndex < benchmarkModelTree_->topLevelItemCount(); ++groupIndex) {
        QTreeWidgetItem* group = benchmarkModelTree_->topLevelItem(groupIndex);
        const QString groupKey = group->data(0, Qt::UserRole).toString();
        const auto expanded = groupExpansion.constFind(
            groupKey.isEmpty() ? group->text(0) : groupKey);
        if (expanded != groupExpansion.cend()) group->setExpanded(*expanded);
    }
    // Apply the filter after expansion is restored so a live query can reopen
    // matching groups without forgetting the user's collapsed state.
    filterBenchmarkModels(benchmarkModelSearch_->text());
    if (!currentId.isEmpty()) {
        for (int groupIndex = 0; groupIndex < benchmarkModelTree_->topLevelItemCount();
             ++groupIndex) {
            QTreeWidgetItem* group = benchmarkModelTree_->topLevelItem(groupIndex);
            for (int index = 0; index < group->childCount(); ++index) {
                QTreeWidgetItem* item = group->child(index);
                if (benchmarkModelId(item) == currentId) {
                    benchmarkModelTree_->setCurrentItem(item);
                    break;
                }
            }
        }
    }
    benchmarkModelTree_->verticalScrollBar()->setValue(previousScrollY);
    benchmarkModelTree_->horizontalScrollBar()->setValue(previousScrollX);
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
        const bool snapshot = group->data(0, kFilterExpansionRole).isValid();
        if (!filter.isEmpty() && !snapshot) {
            group->setData(0, kFilterExpansionRole, group->isExpanded());
        }
        int visibleChildren = 0;
        for (int index = 0; index < group->childCount(); ++index) {
            QTreeWidgetItem* model = group->child(index);
            const QString haystack = model->text(0) + QLatin1Char('\n') + model->toolTip(0) +
                                     QLatin1Char('\n') + model->data(0, kModelRole).toString();
            const bool visible =
                groupMatches || haystack.contains(filter, Qt::CaseInsensitive);
            model->setHidden(!visible);
            visibleChildren += visible ? 1 : 0;
        }
        if (filter.isEmpty()) {
            if (snapshot) {
                group->setExpanded(group->data(0, kFilterExpansionRole).toBool());
                group->setData(0, kFilterExpansionRole, {});
            }
            group->setHidden(group->childCount() == 0);
        } else {
            group->setHidden(visibleChildren == 0);
            if (visibleChildren > 0) group->setExpanded(true);
        }
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
    int selectedAvailable = 0;
    int totalAvailable = 0;
    int selectedUnavailable = 0;
    if (benchmarkModelTree_ != nullptr) {
        for (int groupIndex = 0; groupIndex < benchmarkModelTree_->topLevelItemCount();
             ++groupIndex) {
            const QTreeWidgetItem* group = benchmarkModelTree_->topLevelItem(groupIndex);
            totalModels += group->childCount();
            for (int index = 0; index < group->childCount(); ++index) {
                const QTreeWidgetItem* item = group->child(index);
                const bool checked = item->checkState(0) == Qt::Checked;
                selectedModels += checked ? 1 : 0;
                if (item->data(0, kUnavailableRole).toBool()) {
                    selectedUnavailable += checked ? 1 : 0;
                } else {
                    ++totalAvailable;
                    selectedAvailable += checked ? 1 : 0;
                }
            }
        }
    }

    benchmarkClipCount_->setText(
        tr("%1 / %2 selected").arg(selectedClips).arg(totalClips));
    benchmarkSelectAllClips_->setText(selectedClips == totalClips ? tr("Clear selection")
                                                                  : tr("Select all"));
    benchmarkSelectAllClips_->setToolTip(selectedClips == totalClips ? tr("Clear every clip")
                                                                     : tr("Select every clip"));
    benchmarkModelCount_->setText(
        tr("%1 / %2 selected").arg(selectedModels).arg(totalModels));
    // A checked unavailable row is still a selection: Clear must stay reachable
    // even when the catalogue currently offers nothing to select.
    const bool everyAvailableChecked =
        selectedModels > 0 && selectedAvailable == totalAvailable;
    benchmarkSelectAllModels_->setText(everyAvailableChecked ? tr("Clear selection")
                                                             : tr("Select all"));
    benchmarkSelectAllModels_->setToolTip(
        everyAvailableChecked
            ? tr("Clear every model, including ones the catalogue no longer offers")
            : tr("Select every available model"));
    // Unavailable rows keep their selection visible but are not run inputs;
    // nothing is substituted for a model that left the catalogue.
    const bool includeBaseline = benchmarkRawBox_->isChecked();
    const int runs = selectedClips * (selectedAvailable + (includeBaseline ? 1 : 0));
    const bool canRun = selectedClips > 0 && (selectedAvailable > 0 || includeBaseline);
    if (benchmarkRunning_) {
        benchmarkRunButton_->setText(tr("Cancel"));
        benchmarkRunButton_->setToolTip(tr("Stop after the current sample. Finished rows are kept."));
        benchmarkRunButton_->setAccessibleDescription(
            tr("Stop the benchmark that is currently running."));
        benchmarkRunButton_->setEnabled(benchmarkCancellation_ != nullptr &&
                                        !benchmarkCancellation_->load());
        if (benchmarkProgress_ != nullptr && benchmarkProgress_->maximum() > 0) {
            benchmarkRunSummary_->setText(tr("Running · %1 / %2")
                                              .arg(benchmarkProgress_->value())
                                              .arg(benchmarkProgress_->maximum()));
        }
        return;
    }
    QStringList summary{tr("Clips: %1").arg(selectedClips),
                        tr("Models: %1").arg(selectedAvailable),
                        tr("Runs: %1").arg(runs)};
    if (selectedUnavailable > 0) {
        summary.append(tr("%1 unavailable").arg(selectedUnavailable));
    }
    benchmarkRunSummary_->setText(summary.join(QStringLiteral(" · ")));
    benchmarkRunButton_->setText(tr("Run benchmark"));
    benchmarkRunButton_->setEnabled(canRun);
    benchmarkRunButton_->setToolTip(
        canRun ? tr("Measure throughput for each selected clip and model. Preparation is excluded.")
               : tr("Select at least one clip and one available model, or include the baseline."));
    benchmarkRunButton_->setAccessibleDescription(
        tr("Starts throughput measurement for the selected clips and models."));
}

void SettingsDialog::retranslateBenchmarkTab() {
    if (benchmarkHeading_ == nullptr) return;
    benchmarkHeading_->setText(tr("Benchmark"));
    benchmarkDescription_->setText(tr("Compare rendering speed at 2× input resolution."));
    benchmarkVideoTitle_->setText(tr("Video clips"));
    benchmarkModelTitle_->setText(tr("Enhancement models"));
    benchmarkModelSearch_->setPlaceholderText(tr("Filter by model, group, or file"));
    benchmarkModelSearch_->setAccessibleName(tr("Filter benchmark models"));
    benchmarkModelSearch_->setToolTip(
        tr("Show models whose name, group, file, or id matches. Clearing restores open groups."));
    benchmarkRawBox_->setText(tr("Include the unfiltered baseline"));
    benchmarkRawBox_->setToolTip(
        tr("Measures playback with no shader or model, so filtered runs have a reference."));
    benchmarkResultsTitle_->setText(tr("Results"));
    benchmarkNote_->setText(tr("Preparation excluded from FPS."));
    benchmarkEmptyState_->setText(
        tr("Run a benchmark to see throughput for each selected clip and model."));
    benchmarkResults_->setHorizontalHeaderLabels({tr("Model"), tr("Clip"), tr("Throughput")});
    alignBenchmarkThroughputColumn(benchmarkResults_);
    benchmarkResults_->setAccessibleName(tr("Benchmark throughput results"));
    benchmarkResults_->setAccessibleDescription(
        tr("Each row is one clip measured with one model. Select a failed row for the reason. Ctrl+C copies the row."));
    benchmarkProgress_->setAccessibleName(tr("Benchmark run progress"));
    benchmarkEngineProgress_->setAccessibleName(tr("Engine build progress; indeterminate"));
    benchmarkModelTree_->setAccessibleName(tr("Available benchmark models"));
    benchmarkModelTree_->setAccessibleDescription(
        tr("Check the models to measure. A group checkbox selects every model in that group. Space toggles the focused row."));
    benchmarkModelTree_->setToolTip(
        tr("Space toggles the focused model. A group checkbox selects the whole group."));
    if (!benchmarkRunning_ && benchmarkResults_->rowCount() == 0) {
        benchmarkRunStatus_->setText(tr("Ready"));
        styleStatusBanner(benchmarkRunStatus_, QStringLiteral("neutral"));
    }
    refreshBenchmarkCatalogue();
    updateBenchmarkSelectionSummary();
}

QString SettingsDialog::locateBenchmarkClip(const QString& name) {
    // Beside the executable for installed and portable Windows payloads, the
    // installation prefix's share directory for Linux packages, and the source
    // tree's own resources for a development build.
    return mikai::app::locateBenchmarkClip(name, QCoreApplication::applicationDirPath());
}

void SettingsDialog::refreshBenchmarkEngineChoice() {
    if (benchmarkEngineChoice_ == nullptr) return;
    // Re-read on every show and every run: the Components tab writes this key
    // while the dialog is open, and a benchmark that named a stale engine would
    // be reporting a measurement of something else.
#ifdef Q_OS_LINUX
    const std::optional<mplayer::native::Backend> stored =
        storedStandardAnimeJanaiBackendChoice();
    const mplayer::native::Backend engine =
        stored == mplayer::native::Backend::TensorRTRTX ? *stored
                                                        : mplayer::native::Backend::TensorRT;
#else
    const mplayer::native::Backend engine = storedStandardAnimeJanaiBackend();
#endif
    benchmarkEngineChoice_->setText(
        tr("Engine: %1").arg(benchmarkEngineName(engine)));
    benchmarkEngineChoice_->setToolTip(
        tr("The benchmark runs the engine selected in Settings › Components."));
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
            if (item->checkState(0) == Qt::Checked &&
                !item->data(0, kUnavailableRole).toBool()) {
                const QString storedLabel = item->data(0, kModelLabelRole).toString();
                models.append({benchmarkModelId(item),
                               storedLabel.isEmpty() ? item->text(0) : storedLabel});
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
        benchmarkRunStatus_->setText(tr("Failed"));
        styleStatusBanner(benchmarkRunStatus_, QStringLiteral("warning"));
        return;
    }

    const auto cancellation = std::make_shared<std::atomic<bool>>(false);
    benchmarkCancellation_ = cancellation;
    benchmarkRunning_ = true;
    benchmarkResults_->setRowCount(0);
    benchmarkResultDetail_->clear();
    benchmarkResultDetail_->hide();
    benchmarkProgress_->setProperty("runState", QStringLiteral("running"));
    theme::repolish(benchmarkProgress_);
    benchmarkResultStack_->setCurrentWidget(benchmarkEmptyState_);
    benchmarkResultStack_->setFixedHeight(theme::kBenchmarkEmptyHeight);
    styleStatusBanner(benchmarkRunStatus_, QStringLiteral("running"));
    benchmarkRunStatus_->setText(tr("Benchmark running · %1 / %2").arg(0).arg(totalRuns));
    benchmarkProgress_->setRange(0, totalRuns);
    benchmarkProgress_->setValue(0);
    benchmarkProgress_->show();
    benchmarkActivity_->setText(tr("Starting benchmark…"));
    refreshBenchmarkEngineChoice();
    benchmarkActivity_->show();
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
    const Runner::GuiDispatch post = runner_.post();
    const QString reportDirectory = mikai::app::benchmarkReportBaseDirectory();
    const QString applicationVersion = QCoreApplication::applicationVersion();
#ifdef Q_OS_LINUX
    // There is no published Linux RTX runtime, but a stored/imported RTX name
    // must not be silently measured as classic TensorRT: keep it so every plan
    // returns its own platform refusal with the requested backend named. DirectML
    // keeps the historical Linux clamp, and with no stored Standard choice the
    // benchmark stays on classic TensorRT exactly as before.
    const std::optional<mplayer::native::Backend> storedStandard =
        storedStandardAnimeJanaiBackendChoice();
    const mplayer::native::Backend backend =
        storedStandard == mplayer::native::Backend::TensorRTRTX
            ? *storedStandard
            : mplayer::native::Backend::TensorRT;
#else
    const mplayer::native::Backend backend = storedStandardAnimeJanaiBackend();
#endif
    QHash<QString, QString> clipPaths;
    for (const QString& clip : clips) clipPaths.insert(clip, locateBenchmarkClip(clip));
    // X-M15: hour-scale work (engine builds alone can run 10 min each)
    // belongs on the transfer pool, not the shared API pool.
    runner_.runTransfer(
        [dialog, cancellation, post, reportDirectory, applicationVersion, backend, clipPaths,
         clips, models, measureRaw, totalRuns, surface]() -> QVariant {
            const backend::WorkCancellationScope cancellationScope(cancellation.get());
            int completedRuns = 0;
            int failedRuns = 0;

            // Settings can close before this worker finishes; report ownership cannot be a widget.
            mikai::app::BenchmarkReport report;
            report.startedAt = QDateTime::currentDateTimeUtc();
            const mplayer::native::GpuComputeCapability& gpu =
                mplayer::native::observeComputeCapability();
            report.runtimeMetadata = {
                {QStringLiteral("applicationVersion"), applicationVersion},
                {QStringLiteral("qt"), QString::fromLatin1(qVersion())},
                {QStringLiteral("os"), QSysInfo::prettyProductName()},
                {QStringLiteral("gpu"), gpu.name},
                {QStringLiteral("gpuDriver"), gpu.driverVersion},
                {QStringLiteral("gpuSmMajor"), gpu.smMajor},
                {QStringLiteral("gpuSmMinor"), gpu.smMinor},
                {QStringLiteral("backend"), benchmarkBackendId(backend)}};

            const auto recordResult = [&report](const QString& modelId, const QString& modelLabel,
                                                const QString& clipFile, const QString& sourcePath,
                                                const QSize& sourceSize, const QSize& targetSize,
                                                double fps, const QString& failureReason,
                                                const QString& backendId, qint64 enginePrepMs,
                                                const QDateTime& startedAt,
                                                const QDateTime& finishedAt) {
                mikai::app::BenchmarkReportResult entry;
                entry.modelId = modelId;
                entry.modelLabel = modelLabel;
                entry.clip = clipFile;
                entry.sourcePath = sourcePath;
                entry.sourceSize = sourceSize;
                entry.targetSize = targetSize;
                entry.fps = fps;
                entry.failureReason = failureReason;
                entry.backend = backendId;
                entry.enginePrepMs = enginePrepMs;
                entry.startedAt = startedAt;
                entry.finishedAt = finishedAt;
                report.results.append(entry);
            };

            const auto publishActivity = [dialog, post, cancellation](const QString& text) {
                post([dialog, cancellation, text] {
                        if (cancellation->load() || dialog->isNull()) return;
                        SettingsDialog* self = dialog->data();
                        if (self->benchmarkActivity_ != nullptr) {
                            self->benchmarkActivity_->setText(text);
                        }
                    });
            };

            const auto publishEngineBuild = [dialog, post, cancellation, backend](
                                                BenchmarkEngineBuildUpdate update,
                                                const QString& detail) {
                post([dialog, cancellation, update, detail, backend] {
                        if (cancellation->load() || dialog->isNull()) return;
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
                                SettingsDialog::tr("%1 engine build elapsed · %2")
                                    .arg(benchmarkEngineName(backend),
                                         benchmarkElapsedText(0)));
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
                    });
            };

            const auto append = [dialog, post, cancellation, totalRuns, &completedRuns, &failedRuns](
                                    const QString& model, const QString& clip,
                                    const QString& result, bool successful) {
                const BenchmarkResultRow resultRow{model, clip, result, successful};
                if (!successful) ++failedRuns;
                const int completed = ++completedRuns;
                post([dialog, cancellation, resultRow, completed, totalRuns] {
                        if (dialog->isNull()) return;
                        SettingsDialog* self = dialog->data();
                        // A sample that already finished still belongs in the table
                        // after Cancel. Only a newer run invalidates this token.
                        if (self->benchmarkCancellation_ != cancellation) return;
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
                        clipItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                        QFont clipFont = self->benchmarkResults_->font();
                        clipFont.setFeature(QFont::Tag("tnum"), 1);
                        clipItem->setFont(clipFont);
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
                        const theme::Palette resultPalette =
                            theme::paletteFor(currentAppearance());
                        resultItem->setForeground(QColor(resultRow.successful
                                                             ? resultPalette.textPrimary
                                                             : resultPalette.dangerText));
                        self->benchmarkResults_->setItem(row, 2, resultItem);
                        const int resultHeight =
                            self->benchmarkResults_->horizontalHeader()->height() +
                            std::max(2, row + 1) *
                                self->benchmarkResults_->verticalHeader()->defaultSectionSize() +
                            self->benchmarkResults_->frameWidth() * 2;
                        self->benchmarkResultStack_->setFixedHeight(
                            std::min(theme::kBenchmarkResultsMaxHeight, resultHeight));
                        self->benchmarkResultStack_->setCurrentWidget(self->benchmarkResults_);
                        self->benchmarkProgress_->setValue(completed);
                        self->benchmarkRunStatus_->setText(
                            SettingsDialog::tr("Benchmark running · %1 / %2")
                                .arg(completed)
                                .arg(totalRuns));
                        self->benchmarkRunSummary_->setText(
                            SettingsDialog::tr("Running · %1 / %2").arg(completed).arg(totalRuns));
                    });
            };

            QHash<QString, mplayer::native::AnimeJanaiSourceContext> sourceContexts;
            QHash<QString, QString> sourceErrors;
            for (const QString& clip : clips) {
                if (backend::WorkCancellationScope::cancellationRequested()) break;
                // A clip the package did not ship has no media to inspect; the
                // per-run measure step reports it as "clip not found" instead of
                // handing an empty path to the decoder.
                const QString path = clipPaths.value(clip);
                if (path.isEmpty()) continue;
                QString error;
                sourceContexts.insert(clip, readBenchmarkSourceContext(path, &error));
                if (!error.isEmpty()) sourceErrors.insert(clip, error);
            }
            const auto measure = [&append, &publishActivity, &publishEngineBuild, &recordResult,
                                  &sourceContexts, &sourceErrors, &clipPaths, &surface, dialog,
                                  cancellation, post](
                                     const QString& modelId, const QString& label,
                                     const QString& clipName,
                                     const std::vector<std::pair<std::string, std::string>>&
                                         options,
                                     int slot = -1,
                                     mplayer::native::Backend engineBackend =
                                         mplayer::native::Backend::DirectML,
                                     const QString& backendId = QString(),
                                     int ajiStageCount = 1) {
                if (backend::WorkCancellationScope::cancellationRequested()) return;
                const auto source = sourceContexts.value(clipName);
                const QSize sourceSize(source.width, source.height);
                const QSize targetSize = sourceSize * 2;
                const QString path = clipPaths.value(clipName);
                if (path.isEmpty()) {
                    const QString reason = SettingsDialog::tr("clip not found");
                    append(label, clipName, reason, false);
                    recordResult(modelId, label, clipName, {}, sourceSize, targetSize, -1.0, reason,
                                 backendId, -1, QDateTime::currentDateTimeUtc(),
                                 QDateTime::currentDateTimeUtc());
                    return;
                }
                // Without source dimensions every target is 0x0 and the sample
                // would report a generic size error instead of the real cause.
                const QString sourceError = sourceErrors.value(clipName);
                if (!sourceError.isEmpty() || sourceSize.isEmpty()) {
                    const QString reason = sourceError.isEmpty()
                        ? SettingsDialog::tr("could not read the clip's dimensions")
                        : sourceError;
                    append(label, clipName, reason, false);
                    recordResult(modelId, label, clipName, path, sourceSize, targetSize, -1.0,
                                 reason, backendId, -1, QDateTime::currentDateTimeUtc(),
                                 QDateTime::currentDateTimeUtc());
                    return;
                }

                const QString clipLabel = QFileInfo(clipName).completeBaseName().replace(
                    QLatin1Char('x'), QStringLiteral(" × "));
                const QString runLabel = QStringLiteral("%1 · %2").arg(label, clipLabel);
                publishActivity(SettingsDialog::tr("Loading clip · %1").arg(runLabel));
                const QDateTime runStartedAt = QDateTime::currentDateTimeUtc();
                post([dialog, cancellation] {
                        if (cancellation->load() || dialog->isNull()) return;
                        dialog->data()->benchmarkEngineElapsedLabel_->hide();
                    });
                const BenchmarkSampleResult result = runBenchmarkSample(
                    path, sourceSize * 2, options, engineBackend, slot, *surface,
                    [&publishActivity, &runLabel](const QString& phase) {
                        publishActivity(QStringLiteral("%1 · %2").arg(phase, runLabel));
                    },
                    [&publishEngineBuild, &runLabel](BenchmarkEngineBuildUpdate update,
                                                     const QString&) {
                        publishEngineBuild(update, runLabel);
                    }, ajiStageCount, cancellation.get());
                const bool successful = result.error.isEmpty() && result.fps > 0;
                append(label, clipName,
                       successful ? SettingsDialog::tr("%1 fps").arg(result.fps, 0, 'f', 1)
                                  : result.error,
                       successful);
                recordResult(modelId, label, clipName, path, sourceSize, targetSize,
                             successful ? result.fps : -1.0,
                             successful ? QString() : result.error, backendId,
                             result.enginePrepMs, runStartedAt, QDateTime::currentDateTimeUtc());
            };

            if (measureRaw) {
                for (const QString& clip : clips) {
                    if (backend::WorkCancellationScope::cancellationRequested()) break;
                    measure(QStringLiteral("baseline"), SettingsDialog::tr("Baseline"), clip, {},
                            -1, mplayer::native::Backend::DirectML, QStringLiteral("none"));
                }
            }

            for (const ModelRow& model : models) {
                if (backend::WorkCancellationScope::cancellationRequested()) break;
                if (model.key.startsWith(QLatin1String("anime4k:"))) {
                    const QString shader =
                        mplayer::anime4k::shaderStringForCandidate(model.key.mid(8));
                    if (shader.isEmpty()) {
                        const QString reason = SettingsDialog::tr("shader chain is not available");
                        for (const QString& clip : clips) {
                            append(model.label, clip, reason, false);
                            const QDateTime now = QDateTime::currentDateTimeUtc();
                            recordResult(model.key, model.label, clip, {}, {}, {}, -1.0, reason,
                                         QStringLiteral("anime4k"), -1, now, now);
                        }
                        continue;
                    }
                    for (const QString& clip : clips) {
                        if (backend::WorkCancellationScope::cancellationRequested()) break;
                        measure(model.key, model.label, clip,
                                {{"glsl-shaders", shader.toStdString()}}, -1,
                                mplayer::native::Backend::DirectML, QStringLiteral("anime4k"));
                    }
                    continue;
                }

                for (const QString& clip : clips) {
                    if (backend::WorkCancellationScope::cancellationRequested()) break;
                    const auto source = sourceContexts.value(clip);
                    const auto plan = mplayer::native::prepareAnimeJanaiPlan(
                        model.key, backend,
                        [] { return backend::WorkCancellationScope::cancellationRequested(); }, source,
                        [](const QString& preset, QString* reason) {
                            return mplayer::anime4k::filterShaderProgramForCandidate(
                                preset, QString(), reason);
                        });
                    if (!plan.available) {
                        const QString reason = !sourceErrors.value(clip).isEmpty()
                            ? sourceErrors.value(clip)
                            : (plan.reason.isEmpty()
                                ? SettingsDialog::tr("plan is not available") : plan.reason);
                        append(model.label, clip, reason, false);
                        const QDateTime now = QDateTime::currentDateTimeUtc();
                        recordResult(model.key, model.label, clip,
                                     clipPaths.value(clip),
                                     QSize(source.width, source.height),
                                     QSize(source.width, source.height) * 2,
                                     -1.0, reason, benchmarkBackendId(backend), -1, now, now);
                        continue;
                    }
                    // X-H5: the benchmark builds its own core, so the
                    // backend closure must be preloaded here too — before
                    // the BenchmarkPlayback request is built — or a Linux
                    // AnimeJaNai benchmark run before opening any video
                    // fails with "Failed to create inference context".
                    if (plan.requiresAji) {
                        mplayer::native::preloadAnimeJanaiBackendLibs(plan.backendLibDirs);
                    }
                    std::vector<std::pair<std::string, std::string>> options{
                        {"vf", plan.filterSpec.toStdString()}};
                    if (plan.requiresAji) {
                        // RTX builds and decodes through the CUDA family, so it
                        // needs NVDEC like classic TensorRT; only DirectML is a
                        // D3D11VA backend.
                        options.emplace_back(
                            "hwdec", mplayer::native::isTensorRtFamily(plan.backend) ? "nvdec"
                                                                                     : "d3d11va");
                    }
                    const int ajiStages = static_cast<int>(std::count_if(
                        plan.stages.cbegin(), plan.stages.cend(),
                        [](const mplayer::native::AnimeJanaiPlanStage& stage) {
                            return stage.kind ==
                                mplayer::native::AnimeJanaiPlanStage::Kind::Aji;
                        }));
                    measure(model.key, model.label, clip, options,
                            plan.requiresAji ? plan.slot : -1, plan.backend,
                            plan.requiresAji ? benchmarkBackendId(plan.backend)
                                             : QStringLiteral("anime4k"), ajiStages);
                    if (plan.cacheLease) plan.cacheLease->release();
                }
            }

            report.finishedAt = QDateTime::currentDateTimeUtc();
            const mikai::app::BenchmarkReportWriteResult written =
                mikai::app::writeBenchmarkReport(reportDirectory, report);
            if (!written.success) {
                std::fprintf(stderr, "[benchmark] report was not written: %s\n",
                             qPrintable(written.error));
            }

            const int resultCount = completedRuns;
            const int failedCount = failedRuns;
            const QString reportPath = written.path;
            const QString reportError = written.error;
            const bool stopped = backend::WorkCancellationScope::cancellationRequested();
            post([dialog, cancellation, stopped, resultCount, failedCount, reportPath, reportError] {
                    if (dialog->isNull()) return;
                    SettingsDialog* self = dialog->data();
                    if (self->benchmarkCancellation_ != cancellation) return;
                    self->benchmarkResultStack_->setCurrentWidget(self->benchmarkResults_);
                    self->benchmarkProgress_->setValue(resultCount);
                    self->benchmarkProgress_->hide();
                    if (reportError.isEmpty()) {
                        self->benchmarkActivity_->setText(
                            SettingsDialog::tr("Report saved · %1")
                                .arg(QFileInfo(reportPath).fileName()));
                        self->benchmarkActivity_->setToolTip(reportPath);
                        self->benchmarkActivity_->show();
                    } else {
                        self->benchmarkActivity_->setText(
                            SettingsDialog::tr("Report not saved · %1").arg(reportError));
                        self->benchmarkActivity_->setToolTip(reportError);
                        self->benchmarkActivity_->show();
                    }
                    self->benchmarkProgress_->setProperty(
                        "runState", stopped ? QStringLiteral("idle") :
                                    failedCount > 0 ? QStringLiteral("failure")
                                                   : QStringLiteral("success"));
                    theme::repolish(self->benchmarkProgress_);
                    self->resetBenchmarkControls();
                    styleStatusBanner(self->benchmarkRunStatus_,
                                      stopped ? QStringLiteral("neutral") :
                                      failedCount == 0 ? QStringLiteral("success")
                                                       : QStringLiteral("warning"));
                    self->benchmarkRunStatus_->setText(
                        stopped ? SettingsDialog::tr("Benchmark cancelled.") : failedCount == 0
                            ? SettingsDialog::tr("Complete · %1 results").arg(resultCount)
                            : SettingsDialog::tr("Complete · %1 results · %2 failed")
                                  .arg(resultCount)
                                  .arg(failedCount));
                });
            return {};
        },
        {},
        [dialog, cancellation](const backend::Failure& failure) {
            if (dialog->isNull()) return;
            SettingsDialog* self = dialog->data();
            if (self->benchmarkCancellation_ != cancellation) return;
            self->resetBenchmarkControls();
            self->benchmarkActivity_->setText(failure.message);
            self->benchmarkActivity_->show();
            self->benchmarkRunStatus_->setText(SettingsDialog::tr("Failed"));
            styleStatusBanner(self->benchmarkRunStatus_, QStringLiteral("warning"));
        });
}

void SettingsDialog::resetBenchmarkControls() {
    benchmarkRunning_ = false;
    benchmarkRunButton_->setText(tr("Run benchmark"));
    benchmarkEngineElapsedLabel_->hide();
    benchmarkEngineTimer_->stop();
    benchmarkEngineProgress_->hide();
    benchmarkProgress_->hide();
    for (QCheckBox* check : benchmarkClipChecks_) check->setEnabled(true);
    benchmarkModelSearch_->setEnabled(true);
    benchmarkModelTree_->setEnabled(true);
    benchmarkRawBox_->setEnabled(true);
    benchmarkSelectAllClips_->setEnabled(true);
    benchmarkSelectAllModels_->setEnabled(true);
    updateBenchmarkSelectionSummary();
}

}  // namespace mikai::app
