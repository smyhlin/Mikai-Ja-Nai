#include "MediaInformationDialog.hpp"

#include "MediaInformationOverview.hpp"
#include "MediaInformationTree.hpp"
#include "ShellTheme.hpp"
#include "VideoSurface.hpp"

#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPlainTextEdit>
#include <QScreen>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextOption>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace mplayer::shell {

MediaInformationDialog::MediaInformationDialog(VideoSurface* surface, QWidget* parent)
    : QDialog(parent), surface_(surface), tracks_(surface->mediaTracks()),
      sourceUrl_(surface->sourceUrl()) {
    setObjectName(QStringLiteral("playerMediaInformation"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::NonModal);
    setFont(uiFont());
    QSize available(680, 560);
    if (const QScreen* targetScreen = screen()) {
        available = (targetScreen->availableGeometry().size() -
                     QSize(4 * kFrameMargin, 8 * kFrameMargin)).expandedTo(QSize(1, 1));
    }
    setMinimumSize(QSize(420, 320).boundedTo(available));
    resize(QSize(680, 560).boundedTo(available));
    QPalette colors = palette();
    colors.setColor(QPalette::Window, flyout());
    colors.setColor(QPalette::WindowText, w95());
    colors.setColor(QPalette::Base, bg0());
    colors.setColor(QPalette::Text, w95());
    colors.setColor(QPalette::Button, pill());
    colors.setColor(QPalette::ButtonText, w95());
    colors.setColor(QPalette::Highlight, accent());
    colors.setColor(QPalette::HighlightedText, w95());
    setPalette(colors);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2 * kFrameMargin, 2 * kFrameMargin,
                               2 * kFrameMargin, 2 * kFrameMargin);
    layout->setSpacing(kFrameMargin);
    auto* modeLayout = new QHBoxLayout;
    modeLayout->setSpacing(kFrameMargin);
    modeLabel_ = new QLabel(this);
    modeSelector_ = new QComboBox(this);
    modeSelector_->setObjectName(QStringLiteral("playerMediaInformationMode"));
    modeSelector_->addItems({tr("Overview"), tr("Tree"), tr("Plain text")});
    modeLabel_->setBuddy(modeSelector_);
    modeLayout->addWidget(modeLabel_);
    modeLayout->addWidget(modeSelector_);
    modeLayout->addStretch();
    layout->addLayout(modeLayout);
    views_ = new QStackedWidget(this);
    layout->addWidget(views_, 1);
    connect(modeSelector_, &QComboBox::currentIndexChanged, this,
            [this] { updateActiveView(); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
    information_.coreIdentity = tracks_.coreIdentity;
    information_.sourceEpoch = surface->sourceEpoch();
    timeout_ = new QTimer(this);
    timeout_->setSingleShot(true);
    timeout_->setInterval(8000);
    connect(this, &QDialog::finished, this, [this] {
        cancelRequest();
        timeout_->stop();
        if (surface_ != nullptr) disconnect(surface_, nullptr, this, nullptr);
    });
    connect(timeout_, &QTimer::timeout, this, [this] {
        cancelRequest();
        timedOut_ = true;
        information_.loading = false;
        renderInformation();
    });
    connect(surface, &VideoSurface::sourceEpochChanged, this, [this] {
        if (!sourceIsCurrent()) close();
    });
    connect(surface, &QObject::destroyed, this, &QDialog::close);
    connect(surface, &VideoSurface::mediaInformationInvalidated, this, &QDialog::close);
    connect(surface, &VideoSurface::mediaTracksChanged, this, [this] {
        if (!sourceIsCurrent()) {
            close();
            return;
        }
        tracks_ = surface_->mediaTracks();
        if (tracks_.ready) trackError_.clear();
        if (tracks_.ready && !requestActive_ && information_.loading && !timedOut_ &&
            playbackError_.isEmpty()) {
            requestInformation();
            return;
        }
        renderInformation();
    });
    connect(surface, &VideoSurface::mediaTracksError, this, [this](const QString& error) {
        if (!sourceIsCurrent()) return;
        trackError_ = error;
        renderInformation();
    });
    connect(surface, &VideoSurface::mediaLoaded, this, [this] {
        if (sourceIsCurrent() && !requestActive_) requestInformation();
    });
    connect(surface, &VideoSurface::errorOccurred, this, [this](const QString& error) {
        if (!sourceIsCurrent()) return;
        cancelRequest();
        timeout_->stop();
        information_.loading = false;
        playbackError_ = error;
        renderInformation();
    });
    renderInformation();
    QTimer::singleShot(0, this, [this] { requestInformation(); });
}

MediaInformationDialog::~MediaInformationDialog() {
    cancelRequest();
}

bool MediaInformationDialog::sourceIsCurrent() const {
    return surface_ != nullptr && sourceUrl_ == surface_->sourceUrl() &&
        information_.sourceEpoch == surface_->sourceEpoch() &&
        information_.coreIdentity == surface_->mediaTracks().coreIdentity;
}

void MediaInformationDialog::cancelRequest() {
    if (requestActive_ && surface_ != nullptr) surface_->cancelMediaInformation();
    requestActive_ = false;
}

void MediaInformationDialog::requestInformation() {
    if (!isVisible()) return;
    if (!sourceIsCurrent() || !playbackError_.isEmpty()) {
        close();
        return;
    }
    cancelRequest();
    timedOut_ = false;
    information_.loading = true;
    timeout_->start();
    const QPointer<MediaInformationDialog> guard(this);
    requestActive_ = surface_->requestMediaInformation([guard](const MediaInformation& information) {
        if (guard == nullptr || !guard->sourceIsCurrent() || !guard->isVisible()) return;
        if (information.coreIdentity != guard->information_.coreIdentity ||
            information.sourceEpoch != guard->information_.sourceEpoch) return;
        guard->information_ = information;
        if (!information.loading) {
            guard->timeout_->stop();
            guard->renderInformation();
        }
    });
    // A not-yet-loaded source is retried by mediaLoaded, without probing or
    // blocking the playback core. The same deadline covers that wait.
    renderInformation();
}

void MediaInformationDialog::renderInformation() {
    setWindowTitle(tr("Media information"));
    modeLabel_->setText(tr("&View:"));
    modeSelector_->setAccessibleName(tr("Information view"));
    modeSelector_->setItemText(0, tr("Overview"));
    modeSelector_->setItemText(1, tr("Tree"));
    modeSelector_->setItemText(2, tr("Plain text"));
    const MediaInformationDocument document = buildMediaInformationDocument(
        information_, tracks_, timedOut_, trackError_, playbackError_);
    if (document_ != document) {
        document_ = document;
        overviewDirty_ = true;
        treeDirty_ = true;
        plainDirty_ = true;
    }
    updateActiveView();
}

void MediaInformationDialog::updateActiveView() {
    switch (modeSelector_->currentIndex()) {
    case 1:
        if (tree_ == nullptr) {
            tree_ = new MediaInformationTree(views_);
            views_->addWidget(tree_);
        }
        if (treeDirty_) {
            tree_->setDocument(document_);
            treeDirty_ = false;
        }
        tree_->setAccessibleName(tr("Media information tree"));
        views_->setCurrentWidget(tree_);
        break;
    case 2:
        if (plain_ == nullptr) {
            plain_ = new QPlainTextEdit(views_);
            plain_->setObjectName(QStringLiteral("playerMediaInformationPlainText"));
            plain_->setReadOnly(true);
            plain_->setUndoRedoEnabled(false);
            plain_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            plain_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
            plain_->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            plain_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            views_->addWidget(plain_);
        }
        if (plainDirty_) {
            updatePlainText();
            plainDirty_ = false;
        }
        plain_->setAccessibleName(tr("Media information plain text"));
        views_->setCurrentWidget(plain_);
        break;
    default:
        if (overview_ == nullptr) {
            overview_ = new MediaInformationOverview(views_);
            views_->addWidget(overview_);
        }
        if (overviewDirty_) {
            overview_->setDocument(document_);
            overviewDirty_ = false;
        }
        overview_->setAccessibleName(tr("Media information overview"));
        views_->setCurrentWidget(overview_);
        break;
    }
}

void MediaInformationDialog::updatePlainText() {
    const QString text = mediaInformationPlainText(document_);
    const QString previous = plain_->toPlainText();
    if (previous == text) return;
    const QTextCursor selection = plain_->textCursor();
    const int anchor = selection.anchor();
    const int position = selection.position();
    const int scroll = plain_->verticalScrollBar()->value();

    // Change only the differing span: positions in unchanged text before and
    // after it stay attached to their content when track selection refreshes.
    qsizetype prefix = 0;
    const qsizetype commonSize = std::min(previous.size(), text.size());
    while (prefix < commonSize && previous[prefix] == text[prefix]) ++prefix;
    qsizetype suffix = 0;
    while (suffix < commonSize - prefix &&
           previous[previous.size() - suffix - 1] == text[text.size() - suffix - 1]) ++suffix;
    const int oldEnd = static_cast<int>(previous.size() - suffix);
    const int newEnd = static_cast<int>(text.size() - suffix);
    const auto updatedPosition = [prefix, oldEnd, newEnd](int position) {
        if (position <= prefix) return position;
        if (position >= oldEnd) return position + newEnd - oldEnd;
        return std::min(position, newEnd);
    };
    QTextCursor edit(plain_->document());
    edit.setPosition(static_cast<int>(prefix));
    edit.setPosition(oldEnd, QTextCursor::KeepAnchor);
    edit.insertText(text.mid(prefix, text.size() - prefix - suffix));

    QTextCursor restored(plain_->document());
    restored.setPosition(updatedPosition(anchor));
    restored.setPosition(updatedPosition(position), QTextCursor::KeepAnchor);
    plain_->setTextCursor(restored);
    plain_->verticalScrollBar()->setValue(scroll);
}

void MediaInformationDialog::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) renderInformation();
}

void MediaInformationDialog::closeEvent(QCloseEvent* event) {
    cancelRequest();
    timeout_->stop();
    QDialog::closeEvent(event);
}

} // namespace mplayer::shell
