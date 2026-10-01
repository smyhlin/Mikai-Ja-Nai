#pragma once

#include <QDialog>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>

class QCheckBox;
class QComboBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTimer;

namespace mikai::backend {
class MikaiClient;
}

namespace mikai::app {

class Runner;

/// API key, live quota readout, the home-page feed switches, and the Anime4K
/// preset hotkeys.
///
/// The dialog is tabbed because the two halves have nothing in common: the
/// General tab is about the Mikai account, the Presets tab is about the player.
/// A binding is only written once every binding validates, so a reserved or
/// duplicated key cannot half-apply.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    SettingsDialog(backend::MikaiClient& client, Runner& runner, QWidget* parent = nullptr);
    ~SettingsDialog() override;

private:
    /// One editable preset binding: the stable catalogue id the shortcut is
    /// stored under, the label shown to the user, and the editor holding it.
    struct PresetRow {
        QString id;
        QString label;
        QKeySequenceEdit* edit = nullptr;
    };

    QWidget* buildGeneralTab();
    QWidget* buildPresetsTab();
    void testToken();
    void refreshQuota();
    /// Collects the bindings and reports the first one that must not be saved:
    /// a reserved player key or a sequence another row already takes. Empty on
    /// success, in which case `presets` holds every non-empty binding keyed by
    /// the preset id.
    QString collectPresetShortcuts(QMap<QString, QString>* presets) const;
    void showPresetError(const QString& message);
    void save();

    backend::MikaiClient& client_;
    Runner& runner_;
    QLineEdit* token_ = nullptr;
    QComboBox* language_ = nullptr;
    QLabel* quota_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* testButton_ = nullptr;
    QMap<QString, QCheckBox*> feedBoxes_;
    QTimer* quotaTimer_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QLabel* presetError_ = nullptr;
    QVector<PresetRow> presetRows_;
};

}  // namespace mikai::app