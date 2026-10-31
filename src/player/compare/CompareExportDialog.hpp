#pragma once

#include <QDialog>
#include <QString>

class QComboBox;
class QLineEdit;
class QWidget;

namespace mplayer::shell {

enum class CompareExportFormat { Gif, Mp4, AnimatedWebP };

class CompareExportDialog : public QDialog {
    Q_OBJECT

public:
    explicit CompareExportDialog(QWidget* parent = nullptr);

    QString outputPath() const { return outputPath_; }
    CompareExportFormat format() const;

private:
    void chooseDirectory();
    void updateExtension();
    void acceptExport();

    QLineEdit* directoryEdit_ = nullptr;
    QLineEdit* fileNameEdit_ = nullptr;
    QComboBox* formatCombo_ = nullptr;
    QString outputPath_;
};

}  // namespace mplayer::shell
