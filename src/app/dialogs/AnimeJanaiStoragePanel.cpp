#include "AnimeJanaiStoragePanel.hpp"

#include "backend/Errors.hpp"
#include "theme/Metrics.hpp"
#include "theme/Theme.hpp"
#include "workers/Runner.hpp"
#include <native/components/AnimeJanaiRuntimeStore.hpp>

#include <QDebug>
#include <QEvent>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVariantMap>
#include <QVBoxLayout>

namespace mikai::app {
namespace native = mplayer::native;

AnimeJanaiStoragePanel::AnimeJanaiStoragePanel(QWidget* parent)
    : QFrame(parent), runner_(new Runner(this)) {
    setObjectName(QStringLiteral("settingsCard"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium,
                              theme::kSpacingMedium, theme::kSpacingMedium);
    layout->setSpacing(theme::kSpacingSmall);
    layout->setAlignment(Qt::AlignTop);
    heading_ = new QLabel(this);
    heading_->setObjectName(QStringLiteral("heading"));
    layout->addWidget(heading_);
    description_ = new QLabel(this);
    description_->setObjectName(QStringLiteral("muted"));
    description_->setWordWrap(true);
    layout->addWidget(description_);
    locationLabel_ = new QLabel(this);
    layout->addWidget(locationLabel_);
    location_ = new QLineEdit(this);
    location_->setObjectName(QStringLiteral("ajiRuntimeStoragePath"));
    location_->setReadOnly(true);
    layout->addWidget(location_);
    hint_ = new QLabel(this);
    hint_->setObjectName(QStringLiteral("muted"));
    hint_->setWordWrap(true);
    layout->addWidget(hint_);
    move_ = new QPushButton(this);
    move_->setObjectName(QStringLiteral("ajiRuntimeStorageMove"));
    restore_ = new QPushButton(this);
    restore_->setObjectName(QStringLiteral("ajiRuntimeStorageRestore"));
    for (auto* button : {move_, restore_}) {
        theme::applyActionRole(button, theme::ActionRole::Secondary);
        button->setMinimumHeight(theme::kControlHeight);
        layout->addWidget(button, 0, Qt::AlignLeft);
    }
    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("ajiRuntimeStorageProgress"));
    progress_->hide();
    layout->addWidget(progress_);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("ajiRuntimeStorageStatus"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    connect(move_, &QPushButton::clicked, this, [this] {
        if (isBusy() || externalBusy_) return;
        const QString destination = QFileDialog::getExistingDirectory(
            this, tr("Choose an empty local folder for shared TensorRT storage"));
        if (!destination.isEmpty()) moveStorage(destination, false);
    });
    connect(restore_, &QPushButton::clicked, this, [this] { moveStorage({}, true); });
    auto* timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, [this] {
        if (isBusy() != reportedBusy_) refresh();
    });
    timer->start();
    retranslateUi();
    refresh();
}

AnimeJanaiStoragePanel::~AnimeJanaiStoragePanel() {
    alive_->store(false);
}

bool AnimeJanaiStoragePanel::isBusy() const {
    return busy_ || native::animeJanaiRuntimeRelocationInProgress();
}

void AnimeJanaiStoragePanel::setExternalBusy(bool busy) {
    if (externalBusy_ == busy) return;
    externalBusy_ = busy;
    refresh();
}

void AnimeJanaiStoragePanel::retranslateUi() {
    heading_->setText(tr("Shared TensorRT storage"));
    description_->setText(tr("Shared by compatible AnimeJaNai applications. Moving the runtime does not move model files or compiled engines."));
    locationLabel_->setText(tr("Current location"));
    location_->setAccessibleName(tr("Shared TensorRT runtime location"));
    hint_->setText(tr("Close other AnimeJaNai applications before moving. On Windows, restart this app first if an AI backend has been used in this session. Neither drive may be disconnected during the move."));
    move_->setText(tr("Move to another drive…"));
    restore_->setText(tr("Use default location"));
    refresh();
}

void AnimeJanaiStoragePanel::refresh() {
    const auto storage = native::describeAnimeJanaiRuntimeStorage();
    if (location_->text() != storage.location) location_->setText(storage.location);
    const bool busy = isBusy();
    move_->setEnabled(storage.available && !busy && !externalBusy_);
    restore_->setEnabled(storage.available && storage.moved && !busy && !externalBusy_);
    if (!busy_) {
        status_->setText(busy ? tr("Another shared runtime storage operation is running.")
                             : !error_.isEmpty() ? error_ : completed_ ? tr("Runtime storage location updated.")
                                                                      : storage.reason);
    }
    status_->setVisible(!status_->text().isEmpty());
    if (reportedBusy_ != busy) {
        reportedBusy_ = busy;
        emit busyChanged();
    }
}

void AnimeJanaiStoragePanel::moveStorage(const QString& destination, bool restoreDefault) {
    if (isBusy() || externalBusy_) return;
    if (QMessageBox::warning(this, tr("Move shared TensorRT storage"),
            tr("This changes the shared location for all compatible AnimeJaNai applications. Close those applications first. Once copying starts, the move cannot be cancelled. Continue?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    busy_ = true;
    completed_ = false;
    error_.clear();
    status_->setText(tr("Preparing runtime manager…"));
    progress_->setRange(0, 0);
    progress_->show();
    refresh();
    const auto live = alive_;
    const Runner::GuiDispatch post = runner_->post();
    runner_->runTransfer(
        [destination, restoreDefault, post, live, ui = this] {
            const auto result = native::relocateAnimeJanaiSharedRuntime(
                destination, restoreDefault,
                [post, live, ui](const native::ComponentProvisionProgress& progress) {
                    post([live, ui, progress] {
                        if (live->load()) ui->applyProgress(progress);
                    });
                }, {});
            if (!result.ok) qWarning().noquote() << "Shared TensorRT storage:" << result.reason;
            return QVariant(QVariantMap{{QStringLiteral("ok"), result.ok},
                                        {QStringLiteral("reason"), result.reason}});
        },
        [this, live](const QVariant& value) {
            if (!live->load()) return;
            const auto result = value.toMap();
            finish(result.value(QStringLiteral("ok")).toBool(), result.value(QStringLiteral("reason")).toString());
        },
        [this, live](const backend::Failure& failure) {
            if (live->load()) finish(false, backend::failureLine(failure));
        });
}

void AnimeJanaiStoragePanel::applyProgress(const native::ComponentProvisionProgress& progress) {
    if (!busy_) return;
    status_->setText(progress.pack == QLatin1String("aji-runtime-storage")
                         ? tr("Moving shared runtime…") : tr("Preparing runtime manager…"));
    if (progress.total <= 0) {
        progress_->setRange(0, 0);
        return;
    }
    progress_->setRange(0, 1000);
    progress_->setValue(qBound(0, qRound(double(progress.received) / double(progress.total) * 1000), 1000));
    progress_->setFormat(tr("%1 / %2").arg(QLocale().formattedDataSize(progress.received),
                                         QLocale().formattedDataSize(progress.total)));
}

void AnimeJanaiStoragePanel::finish(bool ok, const QString& reason) {
    busy_ = false;
    completed_ = ok;
    error_ = reason;
    progress_->hide();
    refresh();
    emit locationChanged();
}

void AnimeJanaiStoragePanel::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) retranslateUi();
}
}
