#pragma once

#include <QDialog>
#include <QMap>
#include <QSet>
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace mikai::backend {
class MikaiClient;
}

namespace mikai::app {

class Runner;

/// API key, live quota readout and the home-page feed switches.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    SettingsDialog(backend::MikaiClient& client, Runner& runner, QWidget* parent = nullptr);
    ~SettingsDialog() override;

private:
    void testToken();
    void refreshQuota();
    void save();

    backend::MikaiClient& client_;
    Runner& runner_;
    QLineEdit* token_ = nullptr;
    QLabel* quota_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* testButton_ = nullptr;
    QMap<QString, QCheckBox*> feedBoxes_;
    QTimer* quotaTimer_ = nullptr;
};

}  // namespace mikai::app
