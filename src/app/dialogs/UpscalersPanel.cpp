#include "dialogs/UpscalersPanel.hpp"

#include "dialogs/settings/SettingsUi.hpp"
#include "theme/Metrics.hpp"
#include "theme/Theme.hpp"
#include "workers/Runner.hpp"

#include <backend/MikaiClient.hpp>

#include <QtCore/QFileInfo>

#include <stdexcept>
#include <QtCore/QLocale>
#include <QtCore/QPointer>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>

namespace mikai::app {

namespace {
/// Five minutes: these graphs are a few megabytes, and a stall that long is a
/// failure worth naming rather than an indefinite wait.
constexpr int kDownloadTimeoutMs = 5 * 60 * 1000;
}

UpscalersPanel::UpscalersPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("upscalersPanel"));
    buildUi();
    refresh();
}

UpscalersPanel::~UpscalersPanel() {
    alive_->store(false, std::memory_order_release);
}

void UpscalersPanel::setRunner(Runner* runner) {
    runner_ = runner;
}

void UpscalersPanel::setClient(backend::MikaiClient* client) {
    client_ = client;
}

void UpscalersPanel::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("upscalersScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("upscalersPage"));
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(theme::kSpacingSmall, theme::kSpacingMedium, theme::kSpacingSmall,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    auto* header = new QFrame(content);
    header->setObjectName(QStringLiteral("settingsCard"));
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium,
                                     theme::kSpacingMedium, theme::kSpacingMedium);
    headerLayout->setSpacing(theme::kSpacingXs);
    titleLabel_ = new QLabel(tr("Upscale models"), header);
    titleLabel_->setObjectName(QStringLiteral("heading"));
    headerLayout->addWidget(titleLabel_);
    descriptionLabel_ = new QLabel(
        tr("Optional ONNX upscalers applied as an AnimeJaNai chain step. Nothing here is "
           "bundled: a model is downloaded only when you ask for it, then stored under the "
           "app-owned model folder. Anime4K presets do not need any of them."),
        header);
    descriptionLabel_->setProperty("textRole", QStringLiteral("muted"));
    descriptionLabel_->setWordWrap(true);
    headerLayout->addWidget(descriptionLabel_);
    countLabel_ = new QLabel(header);
    countLabel_->setObjectName(QStringLiteral("upscalersCount"));
    countLabel_->setProperty("textRole", QStringLiteral("muted"));
    headerLayout->addWidget(countLabel_);
    root->addWidget(header);

    for (const mplayer::native::UpscalerModel& model : mplayer::native::upscalerCatalog()) {
        Row row;
        row.model = model;
        auto* card = new QFrame(content);
        card->setObjectName(QStringLiteral("upscalerRow_%1").arg(model.fileName));
        card->setProperty("settingsCard", true);
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium,
                                       theme::kSpacingMedium, theme::kSpacingMedium);
        cardLayout->setSpacing(theme::kSpacingXs);

        auto* line = new QHBoxLayout();
        line->setSpacing(theme::kSpacingSmall);
        auto* name = new QLabel(model.displayName, card);
        name->setObjectName(QStringLiteral("upscalerName_%1").arg(model.fileName));
        name->setWordWrap(true);
        QFont nameFont = name->font();
        nameFont.setBold(true);
        name->setFont(nameFont);
        line->addWidget(name, 1);

        row.state = new QLabel(card);
        row.state->setObjectName(QStringLiteral("upscalerState_%1").arg(model.fileName));
        row.state->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        line->addWidget(row.state, 0);

        auto* size = new QLabel(QLocale().formattedDataSize(model.bytes), card);
        size->setObjectName(QStringLiteral("upscalerSize_%1").arg(model.fileName));
        size->setProperty("textRole", QStringLiteral("muted"));
        line->addWidget(size, 0);

        row.action = new QPushButton(card);
        row.action->setObjectName(QStringLiteral("upscalerAction_%1").arg(model.fileName));
        row.action->setAccessibleName(model.displayName);
        theme::applyActionRole(row.action, theme::ActionRole::Secondary);
        line->addWidget(row.action, 0);
        cardLayout->addLayout(line);

        row.meta = new QLabel(
            tr("%1× upscale · %2 · publisher: %3")
                .arg(model.scale)
                .arg(model.precision, model.sourceCard.section(QLatin1Char('/'), -1)),
            card);
        row.meta->setObjectName(QStringLiteral("upscalerMeta_%1").arg(model.fileName));
        row.meta->setProperty("textRole", QStringLiteral("muted"));
        row.meta->setWordWrap(true);
        cardLayout->addWidget(row.meta);

        row.progress = new QProgressBar(card);
        row.progress->setObjectName(QStringLiteral("upscalerProgress_%1").arg(model.fileName));
        row.progress->setTextVisible(false);
        row.progress->setFixedHeight(6);
        row.progress->setRange(0, 100);
        row.progress->hide();
        cardLayout->addWidget(row.progress);

        row.status = new QLabel(card);
        row.status->setObjectName(QStringLiteral("upscalerStatus_%1").arg(model.fileName));
        row.status->setProperty("textRole", QStringLiteral("muted"));
        row.status->setWordWrap(true);
        row.status->hide();
        cardLayout->addWidget(row.status);

        const QString id = model.fileName;
        connect(row.action, &QPushButton::clicked, this, [this, id] {
            for (Row& candidate : rows_) {
                if (candidate.model.fileName != id) continue;
                if (candidate.running) return;
                const bool installed =
                    !mplayer::native::installedUpscalerPath(candidate.model).isEmpty();
                if (installed) startRemoval(candidate);
                else startDownload(candidate);
                return;
            }
        });
        rows_.append(row);
        root->addWidget(card);
    }

    noteLabel_ = new QLabel(
        tr("The publisher's model card is the licence record for these bytes; this project "
           "does not redistribute them. Downloaded graphs stay on this machine."),
        content);
    noteLabel_->setProperty("textRole", QStringLiteral("muted"));
    noteLabel_->setWordWrap(true);
    root->addWidget(noteLabel_);
    root->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll);
}

void UpscalersPanel::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event != nullptr && event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void UpscalersPanel::retranslateUi() {
    if (titleLabel_ != nullptr) titleLabel_->setText(tr("Upscale models"));
    if (descriptionLabel_ != nullptr) {
        descriptionLabel_->setText(
            tr("Optional ONNX upscalers applied as an AnimeJaNai chain step. Nothing here is "
               "bundled: a model is downloaded only when you ask for it, then stored under the "
               "app-owned model folder. Anime4K presets do not need any of them."));
    }
    if (noteLabel_ != nullptr) {
        noteLabel_->setText(
            tr("The publisher's model card is the licence record for these bytes; this project "
               "does not redistribute them. Downloaded graphs stay on this machine."));
    }
    for (Row& row : rows_) {
        if (row.meta != nullptr) {
            row.meta->setText(
                tr("%1× upscale · %2 · publisher: %3")
                    .arg(row.model.scale)
                    .arg(row.model.precision, row.model.sourceCard.section(QLatin1Char('/'), -1)));
        }
    }
    refresh();
}

void UpscalersPanel::refresh() {
    for (Row& row : rows_) renderRow(row);
    int installed = 0;
    for (const Row& row : rows_) {
        if (!mplayer::native::installedUpscalerPath(row.model).isEmpty()) ++installed;
    }
    if (countLabel_ != nullptr) {
        countLabel_->setText(tr("%1 of %2 models installed").arg(installed).arg(rows_.size()));
    }
}

bool UpscalersPanel::busy() const {
    for (const Row& row : rows_) {
        if (row.running) return true;
    }
    return false;
}

QStringList UpscalersPanel::rowIds() const {
    QStringList ids;
    for (const Row& row : rows_) ids.append(row.model.fileName);
    return ids;
}

void UpscalersPanel::renderRow(Row& row) {
    const bool installed = !mplayer::native::installedUpscalerPath(row.model).isEmpty();
    if (row.state != nullptr) {
        row.state->setText(installed ? tr("installed") : tr("not installed"));
        row.state->setProperty("textRole",
                               installed ? QString() : QStringLiteral("muted"));
        row.state->style()->unpolish(row.state);
        row.state->style()->polish(row.state);
    }
    if (row.action != nullptr && !row.running) {
        row.action->setText(installed ? tr("Remove") : tr("Download"));
        row.action->setEnabled(true);
        theme::applyActionRole(row.action, installed ? theme::ActionRole::Secondary
                                                     : theme::ActionRole::Primary);
    }
}

void UpscalersPanel::setRowStatus(Row& row, const QString& text, bool failure) {
    if (row.status == nullptr) return;
    row.status->setText(text);
    row.status->setProperty("textRole", failure ? QStringLiteral("danger") : QStringLiteral("muted"));
    row.status->style()->unpolish(row.status);
    row.status->style()->polish(row.status);
    row.status->setVisible(!text.isEmpty());
}

void UpscalersPanel::startDownload(Row& row) {
    if (runner_ == nullptr || client_ == nullptr) {
        setRowStatus(row, tr("A download needs the backend client and a worker pool."), true);
        return;
    }
    row.running = true;
    row.removing = false;
    row.action->setEnabled(false);
    row.action->setText(tr("Downloading…"));
    row.progress->setRange(0, 0);
    row.progress->show();
    setRowStatus(row, tr("Contacting the publisher…"), false);

    backend::MikaiClient* client = client_;
    const mplayer::native::UpscalerModel model = row.model;
    runner_->runTransfer(
        [client, model]() -> QVariant {
            // The bar is indeterminate for the transfer and turns into the
            // verified result: a few megabytes over a network that may not
            // report its total would otherwise sit at a stalled percentage.
            const QByteArray bytes =
                client->fetchBytes(model.url, kDownloadTimeoutMs, {},
                                   backend::MikaiClient::Redirects::FollowSafe);
            QString reason;
            const QString registered =
                mplayer::native::installUpscalerBytes(model, bytes, &reason);
            if (registered.isEmpty()) {
                throw std::runtime_error(reason.isEmpty()
                                             ? QStringLiteral("the graph was not installed")
                                                   .toStdString()
                                             : reason.toStdString());
            }
            return QVariant(registered);
        },
        [this, alive = alive_, fileName = row.model.fileName](const QVariant&) {
            if (!alive->load(std::memory_order_acquire)) return;
            if (Row* target = rowByFileName(fileName)) {
                target->running = false;
                target->progress->hide();
                setRowStatus(*target, tr("Installed."), false);
                renderRow(*target);
            }
            refresh();
            emit modelsChanged();
        },
        [this, alive = alive_, fileName = row.model.fileName](const backend::Failure& failure) {
            if (!alive->load(std::memory_order_acquire)) return;
            if (Row* target = rowByFileName(fileName)) {
                target->running = false;
                target->progress->hide();
                setRowStatus(*target, failure.message, true);
                renderRow(*target);
            }
        });
}

void UpscalersPanel::startRemoval(Row& row) {
    if (runner_ == nullptr) {
        setRowStatus(row, tr("Removing needs a worker pool."), true);
        return;
    }
    row.running = true;
    row.removing = true;
    row.action->setEnabled(false);
    row.action->setText(tr("Removing…"));
    setRowStatus(row, tr("Removing the stored graph…"), false);
    const mplayer::native::UpscalerModel model = row.model;
    runner_->runTransfer(
        [model]() -> QVariant {
            QString reason;
            if (!mplayer::native::removeInstalledUpscaler(model, &reason)) {
                throw std::runtime_error(reason.isEmpty() ? "not installed" : reason.toStdString());
            }
            return QVariant();
        },
        [this, alive = alive_, fileName = row.model.fileName](const QVariant&) {
            if (!alive->load(std::memory_order_acquire)) return;
            if (Row* target = rowByFileName(fileName)) {
                target->running = false;
                target->removing = false;
                setRowStatus(*target, QString(), false);
                renderRow(*target);
            }
            refresh();
            emit modelsChanged();
        },
        [this, alive = alive_, fileName = row.model.fileName](const backend::Failure& failure) {
            if (!alive->load(std::memory_order_acquire)) return;
            if (Row* target = rowByFileName(fileName)) {
                target->running = false;
                target->removing = false;
                setRowStatus(*target, failure.message, true);
                renderRow(*target);
            }
        });
}

UpscalersPanel::Row* UpscalersPanel::rowByFileName(const QString& fileName) {
    for (Row& row : rows_) {
        if (row.model.fileName == fileName) return &row;
    }
    return nullptr;
}

}  // namespace mikai::app
