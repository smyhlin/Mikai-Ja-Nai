#pragma once

#include "native/models/NativeUpscalers.hpp"

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>
#include <QtWidgets/QWidget>

namespace mikai::backend {
class MikaiClient;
}

class QLabel;
class QProgressBar;
class QPushButton;

namespace mikai::app {

class Runner;

/// The optional ONNX upscalers, one row each: what the graph is, what it costs
/// to download, whether it is in the store, and the one action that applies to
/// that state. Nothing downloads until a viewer asks for it, and the anime4k
/// presets stay independent of every row here.
class UpscalersPanel : public QWidget {
    Q_OBJECT

public:
    explicit UpscalersPanel(QWidget* parent = nullptr);

    void setRunner(Runner* runner);
    void setClient(backend::MikaiClient* client);

    /// Re-reads the store and re-renders every row. Never touches the network.
    void refresh();
    /// True while any row owns a download or removal.
    bool busy() const;

    /// The row id a viewer sees, for tests and probes.
    QStringList rowIds() const;

signals:
    void modelsChanged();

private:
    struct Row {
        mplayer::native::UpscalerModel model;
        QLabel* state = nullptr;
        QLabel* status = nullptr;
        QProgressBar* progress = nullptr;
        QPushButton* action = nullptr;
        bool running = false;
        bool removing = false;
    };

    void buildUi();
    void renderRow(Row& row);
    void startDownload(Row& row);
    void startRemoval(Row& row);
    void setRowStatus(Row& row, const QString& text, bool failure);
    Row* rowByFileName(const QString& fileName);

    Runner* runner_ = nullptr;
    backend::MikaiClient* client_ = nullptr;
    QVector<Row> rows_;
    QLabel* countLabel_ = nullptr;
};

}  // namespace mikai::app
