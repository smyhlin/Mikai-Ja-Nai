#include "CompareExportComplete.hpp"

#include <QDesktopServices>
#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QPushButton>
#include <QStyle>
#include <QShowEvent>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace mplayer::shell {

CompareExportComplete::CompareExportComplete(const QString& path, QWidget* parent)
    : QDialog(parent), path_(path) {
    setWindowTitle(tr("Export complete"));
    setModal(false);

    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel(tr("Your comparison export is ready."), this);
    title->setWordWrap(true);
    layout->addWidget(title);

    auto* actions = new QHBoxLayout;
    openFileButton_ = new QPushButton(tr("Open file"), this);
    openFolderButton_ = new QPushButton(tr("Open folder"), this);
    copyFileButton_ = new QPushButton(tr("Copy file"), this);
    actions->addWidget(openFileButton_);
    actions->addWidget(openFolderButton_);
    actions->addWidget(copyFileButton_);
    layout->addLayout(actions);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status_);

    closeTimer_ = new QTimer(this);
    closeTimer_->setSingleShot(true);
    connect(closeTimer_, &QTimer::timeout, this, &QDialog::close);
    connect(openFileButton_, &QPushButton::clicked, this, &CompareExportComplete::openFile);
    connect(openFolderButton_, &QPushButton::clicked, this, &CompareExportComplete::openFolder);
    connect(copyFileButton_, &QPushButton::clicked, this, &CompareExportComplete::copyFile);

    setMinimumWidth(420);
}

void CompareExportComplete::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    closeTimer_->start(5000);
}

void CompareExportComplete::openFile() {
    const QFileInfo file(path_);
    if (!file.exists() || !file.isFile()) {
        setStatus(tr("The exported file is no longer available."), true);
        closeTimer_->stop();
        return;
    }

    const QUrl fileUrl = QUrl::fromLocalFile(file.absoluteFilePath());
    if (!QDesktopServices::openUrl(fileUrl)) {
        setStatus(tr("Could not open the exported file."), true);
        closeTimer_->stop();
        return;
    }
    setStatus(tr("Exported file opened."));
}

void CompareExportComplete::openFolder() {
    const QFileInfo file(path_);
    if (!file.exists() || !file.isFile()) {
        setStatus(tr("The exported file is no longer available."), true);
        closeTimer_->stop();
        return;
    }

    const QUrl folderUrl = QUrl::fromLocalFile(file.absolutePath());
    if (!QDesktopServices::openUrl(folderUrl)) {
        setStatus(tr("Could not open the output folder."), true);
        closeTimer_->stop();
        return;
    }
    setStatus(tr("Output folder opened."));
}

void CompareExportComplete::copyFile() {
    const QFileInfo file(path_);
    if (!file.exists() || !file.isFile()) {
        setStatus(tr("The exported file is no longer available."), true);
        closeTimer_->stop();
        return;
    }

    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        setStatus(tr("Could not access the clipboard."), true);
        closeTimer_->stop();
        return;
    }

    auto* mimeData = new QMimeData;
    mimeData->setUrls({QUrl::fromLocalFile(file.absoluteFilePath())});
    clipboard->setMimeData(mimeData);
    setStatus(tr("File copied to the clipboard."));
}

void CompareExportComplete::setStatus(const QString& text, bool isError) {
    status_->setText(text);
    status_->setProperty("error", isError);
    status_->style()->unpolish(status_);
    status_->style()->polish(status_);
}

}  // namespace mplayer::shell
