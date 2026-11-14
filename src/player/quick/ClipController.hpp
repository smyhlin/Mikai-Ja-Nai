#pragma once

#include "ClipSelection.hpp"
#include "../core/VideoSession.hpp"
#include "../shell/ClipExport.hpp"
#include "../shell/FramePreviewService.hpp"

#include <QAbstractListModel>
#include <QColor>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>

class QQuickImageProvider;

namespace mplayer::quick {

struct ClipFrameStore;

class ClipThumbnailModel : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
public:
    enum Role { ImageSourceRole = Qt::UserRole + 1, TimeRole };
    explicit ClipThumbnailModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void reset(int count, double start, double end);
    void setImage(int row, const QUrl& source);
    double timeAt(int row) const;

private:
    struct Thumbnail { double time; QUrl source; };
    QVector<Thumbnail> rows_;
};

class ClipController : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(ClipController)
    QML_UNCREATABLE("ClipController is owned by the clip window")
    Q_PROPERTY(mplayer::VideoSession* session READ session CONSTANT)
    Q_PROPERTY(mplayer::quick::ClipThumbnailModel* thumbnails READ thumbnails CONSTANT)
    Q_PROPERTY(QString caption READ caption CONSTANT)
    Q_PROPERTY(double windowStart READ windowStart NOTIFY selectionChanged)
    Q_PROPERTY(double windowEnd READ windowEnd NOTIFY selectionChanged)
    Q_PROPERTY(double selectionStart READ selectionStart NOTIFY selectionChanged)
    Q_PROPERTY(double selectionEnd READ selectionEnd NOTIFY selectionChanged)
    Q_PROPERTY(double selectionDuration READ selectionDuration NOTIFY selectionChanged)
    Q_PROPERTY(QString startText READ startText NOTIFY selectionChanged)
    Q_PROPERTY(QString endText READ endText NOTIFY selectionChanged)
    Q_PROPERTY(QString windowStartText READ windowStartText NOTIFY selectionChanged)
    Q_PROPERTY(QString windowEndText READ windowEndText NOTIFY selectionChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY positionChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY previewChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY previewChanged)
    Q_PROPERTY(bool previewLoading READ previewLoading NOTIFY previewChanged)
    Q_PROPERTY(bool videoVisible READ videoVisible NOTIFY previewChanged)
    Q_PROPERTY(QUrl previewSource READ previewSource NOTIFY previewChanged)
    Q_PROPERTY(bool editingEnabled READ editingEnabled NOTIFY stateChanged)
    Q_PROPERTY(bool canExport READ canExport NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool choosingOutput READ choosingOutput NOTIFY stateChanged)
    Q_PROPERTY(bool overwritePending READ overwritePending NOTIFY stateChanged)
    Q_PROPERTY(bool failed READ failed NOTIFY stateChanged)
    Q_PROPERTY(bool saved READ saved NOTIFY stateChanged)
    Q_PROPERTY(bool cancelPending READ cancelPending NOTIFY stateChanged)
    Q_PROPERTY(int qualityHeight READ qualityHeight WRITE setQualityHeight NOTIFY selectionChanged)
    Q_PROPERTY(QString estimatedSize READ estimatedSize NOTIFY selectionChanged)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString overwriteFileName READ overwriteFileName NOTIFY stateChanged)
    Q_PROPERTY(QUrl completedPath READ completedPath NOTIFY stateChanged)
    Q_PROPERTY(QColor accentInk READ accentInk NOTIFY themeChanged)

public:
    explicit ClipController(VideoSession* source, QString caption, QObject* parent = nullptr);
    ~ClipController() override;

    VideoSession* session() const { return preview_; }
    ClipThumbnailModel* thumbnails() { return &thumbnails_; }
    QString caption() const { return caption_; }
    double windowStart() const { return selection_.windowStart; }
    double windowEnd() const { return selection_.windowEnd; }
    double selectionStart() const { return selection_.start; }
    double selectionEnd() const { return selection_.end; }
    double selectionDuration() const { return selection_.length(); }
    QString startText() const;
    QString endText() const;
    QString windowStartText() const;
    QString windowEndText() const;
    double position() const { return position_; }
    QString positionText() const;
    bool playing() const { return playing_; }
    bool muted() const { return muted_; }
    bool previewLoading() const { return previewLoading_; }
    bool videoVisible() const { return videoVisible_; }
    QUrl previewSource() const { return previewSource_; }
    bool editingEnabled() const;
    bool canExport() const;
    bool busy() const;
    bool choosingOutput() const;
    bool overwritePending() const;
    bool failed() const;
    bool saved() const;
    bool cancelPending() const;
    int qualityHeight() const { return qualityHeight_; }
    QString estimatedSize() const;
    double progress() const { return progress_; }
    QString statusText() const;
    QString overwriteFileName() const;
    QUrl completedPath() const { return QUrl::fromLocalFile(completedPath_); }
    QColor accentInk() const;
    QQuickImageProvider* createImageProvider() const;

    Q_INVOKABLE void setSelection(double start, double end, int editedBoundary = 0);
    Q_INVOKABLE void moveSelection(double originalStart, double delta);
    Q_INVOKABLE void setStartToCurrent();
    Q_INVOKABLE void setEndToCurrent();
    Q_INVOKABLE void applyDurationPreset(double seconds);
    Q_INVOKABLE void setFilmstripWidth(double width);
    Q_INVOKABLE void beginTimelineGesture();
    Q_INVOKABLE void seekTo(double seconds);
    Q_INVOKABLE void stepSeconds(double delta);
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void notePresentedFrame();
    Q_INVOKABLE void requestExport();
    Q_INVOKABLE void acceptOutput(const QUrl& path);
    Q_INVOKABLE void cancelOutput();
    Q_INVOKABLE void confirmOverwrite(bool replace);
    Q_INVOKABLE void cancelExport();
    Q_INVOKABLE void revealOutput();
    Q_INVOKABLE void copyOutputPath();
    Q_INVOKABLE void requestClose();
    void setQualityHeight(int height);
    void retranslate();
    void shutdown();

signals:
    void selectionChanged();
    void positionChanged();
    void previewChanged();
    void stateChanged();
    void themeChanged();
    void outputRequested(const QUrl& suggestedFile);
    void closeRequested();

private:
    enum class ExportState { Editing, Choosing, Confirming, Encoding, Cancelling, Saved, Failed };
    struct TileRequest { int row; double seconds; quint64 generation; };
    struct ActiveTile { TileRequest request; quint64 token; };

    void initializeSelection(double duration);
    void applySelection(const ClipSelection& selection);
    void pausePreview();
    void captureTracks();
    void syncPreviewAudio();
    void invalidate(const char* message);
    void rebuildThumbnails();
    void queueStill();
    void pumpThumbnails();
    void finishThumbnail(quint64 token, double decodedSeconds, const QImage& frame,
                         const QString& reason = {});
    bool pendingExportIsCurrent() const;
    void startExport();
    void setStatus(const char* message, QStringList arguments = {});
    void setExternalError(const QString& message);

    QPointer<VideoSession> source_;
    VideoSession* preview_ = nullptr;
    QString caption_;
    QString sourceUrl_;
    quint64 sourceEpoch_ = 0;
    double anchor_ = 0.0;
    ClipSelection selection_;
    QSize sourceSize_;
    double streamBytesPerSecond_ = 0.0;
    double position_ = 0.0;
    double pendingSeek_ = 0.0;
    bool seeking_ = false;
    bool playing_ = false;
    bool muted_ = false;
    bool previewOpened_ = false;
    bool previewLoaded_ = false;
    bool previewLoading_ = true;
    bool videoVisible_ = false;
    bool invalidated_ = false;
    bool stopped_ = false;
    int qualityHeight_ = 1080;
    std::optional<qint64> audioTrack_;
    QString externalAudio_;
    QString previewExternalAudio_;

    ClipThumbnailModel thumbnails_;
    shell::FramePreviewService frames_;
    std::shared_ptr<ClipFrameStore> frameStore_;
    QTimer thumbnailTimer_;
    QTimer stillTimer_;
    QVector<TileRequest> tileQueue_;
    std::optional<ActiveTile> activeTile_;
    quint64 tileToken_ = 0;
    quint64 tileGeneration_ = 0;
    quint64 imageRevision_ = 0;
    int tileCount_ = 12;
    double stillRequestedTime_ = 0.0;
    QUrl previewSource_;

    shell::ClipExport exporter_;
    ExportState exportState_ = ExportState::Editing;
    std::optional<shell::ClipExportRequest> pendingExport_;
    QString outputPath_;
    QString completedPath_;
    QString codec_;
    double progress_ = 0.0;
    QByteArray statusSource_;
    QStringList statusArguments_;
    QString externalError_;
};

}
