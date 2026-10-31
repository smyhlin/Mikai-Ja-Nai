#include "CompareExportDialog.hpp"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QDialogButtonBox>
#include <QVBoxLayout>

namespace mplayer::shell {
namespace {

QString extensionFor(CompareExportFormat format) {
    switch (format) {
    case CompareExportFormat::Gif: return QStringLiteral("gif");
    case CompareExportFormat::Mp4: return QStringLiteral("mp4");
    case CompareExportFormat::AnimatedWebP: return QStringLiteral("webp");
    }
    return QStringLiteral("gif");
}

QString defaultDirectory() {
    const QStringList candidates{
        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation),
        QDir::homePath()
    };
    for (const QString& candidate : candidates) {
        if (!candidate.isEmpty() && QFileInfo(candidate).isDir() &&
            QFileInfo(candidate).isWritable()) {
            return QDir::cleanPath(candidate);
        }
    }
    return QDir::currentPath();
}

}  // namespace

CompareExportDialog::CompareExportDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Export comparison"));
    setModal(true);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;

    formatCombo_ = new QComboBox(this);
    formatCombo_->setObjectName(QStringLiteral("compareExportFormat"));
    formatCombo_->addItem(tr("GIF animation"), static_cast<int>(CompareExportFormat::Gif));
    formatCombo_->addItem(tr("MP4 video (silent)"), static_cast<int>(CompareExportFormat::Mp4));
    formatCombo_->addItem(tr("Animated WebP"), static_cast<int>(CompareExportFormat::AnimatedWebP));
    formatCombo_->setAccessibleName(tr("Export format"));

    auto* formatLabel = new QLabel(tr("Format:"), this);
    formatLabel->setBuddy(formatCombo_);
    form->addRow(formatLabel, formatCombo_);

    auto* directoryRow = new QHBoxLayout;
    directoryEdit_ = new QLineEdit(defaultDirectory(), this);
    directoryEdit_->setObjectName(QStringLiteral("compareExportDirectory"));
    directoryEdit_->setReadOnly(true);
    directoryEdit_->setAccessibleName(tr("Output folder"));
    auto* browseButton = new QPushButton(tr("Browse…"), this);
    browseButton->setObjectName(QStringLiteral("compareExportBrowse"));
    browseButton->setAccessibleName(tr("Choose output folder"));
    directoryRow->addWidget(directoryEdit_, 1);
    directoryRow->addWidget(browseButton);
    auto* directoryLabel = new QLabel(tr("Folder:"), this);
    directoryLabel->setBuddy(browseButton);
    form->addRow(directoryLabel, directoryRow);

    fileNameEdit_ = new QLineEdit(this);
    fileNameEdit_->setObjectName(QStringLiteral("compareExportFileName"));
    fileNameEdit_->setAccessibleName(tr("File name"));
    auto* fileNameLabel = new QLabel(tr("File name:"), this);
    fileNameLabel->setBuddy(fileNameEdit_);
    form->addRow(fileNameLabel, fileNameEdit_);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Save)->setDefault(true);
    layout->addWidget(buttons);

    connect(browseButton, &QPushButton::clicked, this, &CompareExportDialog::chooseDirectory);
    connect(formatCombo_, &QComboBox::currentIndexChanged, this,
            &CompareExportDialog::updateExtension);
    connect(buttons, &QDialogButtonBox::accepted, this, &CompareExportDialog::acceptExport);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updateExtension();
}

CompareExportFormat CompareExportDialog::format() const {
    return static_cast<CompareExportFormat>(formatCombo_->currentData().toInt());
}

void CompareExportDialog::chooseDirectory() {
    const QString selected = QFileDialog::getExistingDirectory(
        this, tr("Choose output folder"), directoryEdit_->text(), QFileDialog::ShowDirsOnly);
    if (!selected.isEmpty()) directoryEdit_->setText(QDir::cleanPath(selected));
}

void CompareExportDialog::updateExtension() {
    const QString suffix = extensionFor(format());
    QString name = fileNameEdit_->text();
    if (name.isEmpty()) {
        fileNameEdit_->setText(tr("comparison.%1").arg(suffix));
        return;
    }
    const QFileInfo current(name);
    const QString base = current.completeBaseName();
    const QString updated = (base.isEmpty() ? name : base) + QLatin1Char('.') + suffix;
    if (updated != name) {
        const int cursor = fileNameEdit_->cursorPosition();
        fileNameEdit_->setText(updated);
        fileNameEdit_->setCursorPosition(qMin(cursor, updated.size()));
    }
}

void CompareExportDialog::acceptExport() {
    const QString directory = QDir::cleanPath(directoryEdit_->text());
    const QString name = fileNameEdit_->text().trimmed();
    if (directory.isEmpty() || !QFileInfo(directory).isDir() || !QFileInfo(directory).isWritable()) {
        QMessageBox::warning(this, tr("Export unavailable"),
                             tr("The selected output folder is unavailable or not writable."));
        return;
    }
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..") ||
        name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) ||
        QFileInfo(name).isAbsolute() || QFileInfo(name).fileName() != name ||
        name.contains(QChar::Null) || name.contains(QLatin1Char('\n')) ||
        name.contains(QLatin1Char('\r'))) {
        QMessageBox::warning(this, tr("Invalid file name"),
                             tr("Enter a file name without a folder path."));
        fileNameEdit_->setFocus();
        fileNameEdit_->selectAll();
        return;
    }

    const QString expectedSuffix = extensionFor(format());
    const QString finalName = QFileInfo(name).completeBaseName() + QLatin1Char('.') + expectedSuffix;
    const QString target = QDir(directory).filePath(finalName);
    const QFileInfo targetInfo(target);
    if (targetInfo.exists()) {
        if (!targetInfo.isFile()) {
            QMessageBox::warning(this, tr("Export unavailable"),
                                 tr("A folder already exists with that name."));
            return;
        }
        const auto answer = QMessageBox::warning(
            this, tr("Replace existing file?"),
            tr("%1 already exists. Do you want to replace it?").arg(finalName),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
    }

    outputPath_ = target;
    accept();
}

}  // namespace mplayer::shell
