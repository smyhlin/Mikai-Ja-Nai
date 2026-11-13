#pragma once

#include <QFrame>
#include <atomic>
#include <memory>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

namespace mplayer::native { struct ComponentProvisionProgress; }

namespace mikai::app {
class Runner;

class AnimeJanaiStoragePanel final : public QFrame {
    Q_OBJECT
public:
    explicit AnimeJanaiStoragePanel(QWidget* parent = nullptr);
    ~AnimeJanaiStoragePanel() override;
    bool isBusy() const;
    void setExternalBusy(bool busy);
    void refresh();

signals:
    void busyChanged();
    void locationChanged();

protected:
    void changeEvent(QEvent* event) override;

private:
    void retranslateUi();
    void moveStorage(const QString& destination, bool restoreDefault);
    void applyProgress(const mplayer::native::ComponentProvisionProgress& progress);
    void finish(bool ok, const QString& reason);

    Runner* runner_;
    std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
    QLabel* heading_;
    QLabel* description_;
    QLabel* locationLabel_;
    QLineEdit* location_;
    QLabel* hint_;
    QLabel* status_;
    QProgressBar* progress_;
    QPushButton* move_;
    QPushButton* restore_;
    bool busy_ = false;
    bool externalBusy_ = false;
    bool reportedBusy_ = false;
    bool completed_ = false;
    QString error_;
};
}
