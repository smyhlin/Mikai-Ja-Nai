#pragma once

#include "MediaInformationPresentation.hpp"

#include <QDialog>
#include <QPointer>

class QCloseEvent;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QStackedWidget;
class QTimer;

namespace mplayer {
class VideoSurface;
namespace shell {

class MediaInformationOverview;
class MediaInformationTree;

class MediaInformationDialog final : public QDialog {
    Q_OBJECT
public:
    explicit MediaInformationDialog(VideoSurface* surface, QWidget* parent = nullptr);
    ~MediaInformationDialog() override;

protected:
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    bool sourceIsCurrent() const;
    void requestInformation();
    void renderInformation();
    void updateActiveView();
    void updatePlainText();
    void cancelRequest();

    QPointer<VideoSurface> surface_;
    MediaInformation information_;
    MediaTrackSnapshot tracks_;
    QString sourceUrl_;
    QString trackError_;
    QString playbackError_;
    MediaInformationDocument document_;
    QLabel* modeLabel_ = nullptr;
    QComboBox* modeSelector_ = nullptr;
    QStackedWidget* views_ = nullptr;
    MediaInformationOverview* overview_ = nullptr;
    MediaInformationTree* tree_ = nullptr;
    QPlainTextEdit* plain_ = nullptr;
    QTimer* timeout_ = nullptr;
    bool overviewDirty_ = true;
    bool treeDirty_ = true;
    bool plainDirty_ = true;
    bool timedOut_ = false;
    bool requestActive_ = false;
};

} // namespace shell
} // namespace mplayer
