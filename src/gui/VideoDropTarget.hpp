#pragma once

#include <QColor>
#include <QFont>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QEvent;
class QMimeData;
class QPainter;
class QRect;
class QWidget;

namespace mikai::gui {

class VideoDropTarget final : public QObject {
    Q_OBJECT

public:
    enum class OverlayMode { Widget, HostPainted };

    VideoDropTarget(QWidget* host, OverlayMode mode);
    ~VideoDropTarget() override;
    bool active() const { return !path_.isEmpty(); }
    void paintIndicator(QPainter& painter, const QRect& bounds) const;

signals:
    void videoDropped(const QString& path);
    void indicatorChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    static QString videoPath(const QMimeData* mime);
    void beginIndicator(const QString& path);
    void refreshAppearance();
    void clearIndicator();

    QPointer<QWidget> host_;
    QPointer<QWidget> overlay_;
    QString path_;
    QString fileName_;
    QString title_;
    QString hint_;
    QFont titleFont_;
    QFont bodyFont_;
    QColor background_;
    QColor foreground_;
    QColor border_;
    QColor muted_;
    QColor scrim_;
    QTimer leaveTimer_;
    quint64 dragGeneration_ = 0;
    quint64 leaveGeneration_ = 0;
    bool windowBlocked_ = false;
    int titleHeight_ = 0;
    int bodyHeight_ = 0;
    mutable int textWidth_ = -1;
    mutable QString displayedTitle_;
    mutable QString displayedFileName_;
    mutable QString displayedHint_;
};

} // namespace mikai::gui
