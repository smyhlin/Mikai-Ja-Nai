#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QPushButton;
class QShowEvent;
class QTimer;

namespace mplayer::shell {

class CompareExportComplete final : public QDialog {
    Q_OBJECT

public:
    explicit CompareExportComplete(const QString& path, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void openFile();
    void openFolder();
    void copyFile();
    void setStatus(const QString& text, bool isError = false);

    QString path_;
    QLabel* status_ = nullptr;
    QPushButton* openFileButton_ = nullptr;
    QPushButton* openFolderButton_ = nullptr;
    QPushButton* copyFileButton_ = nullptr;
    QTimer* closeTimer_ = nullptr;
};

}  // namespace mplayer::shell
