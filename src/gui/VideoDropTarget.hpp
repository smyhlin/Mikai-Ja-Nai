#pragma once

#include <QColor>
#include <QFont>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QTimer>
#include <QVector>

class QEvent;
class QMimeData;
class QPainter;
class QWidget;

namespace mikai::gui {

class VideoDropTarget final : public QObject {
    Q_OBJECT

public:
    enum class OverlayMode { Widget, HostPainted };

    VideoDropTarget(QWidget* host, OverlayMode mode);
    ~VideoDropTarget() override;
    bool active() const { return !path_.isEmpty(); }
    bool acceptsTracks_ = false;
    void paintIndicator(QPainter& painter, const QRect& bounds) const;

    /// A player that can attach external tracks asks for them; a catalogue
    /// window keeps refusing a stray subtitle drop.
    void setAcceptsExternalTracks(bool accepts) { acceptsTracks_ = accepts; }

signals:
    void videoDropped(const QString& path);
    /// A dropped subtitle or audio file, only emitted when the host asked for
    /// external tracks.
    void subtitleDropped(const QString& path);
    void audioDropped(const QString& path);
    void indicatorChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class DropKind { None, Video, Subtitle, Audio };
    static QString classify(const QMimeData* mime, bool allowTracks, DropKind* kind);
    static QString videoPath(const QMimeData* mime);
    void beginIndicator(const QString& path);
    void refreshAppearance();
    void clearIndicator();

    /// One blittable piece of the indicator: its pixels at device resolution and
    /// the logical point it is drawn back at. The pixmap carries no device pixel
    /// ratio of its own - its size is the device rectangle it covers, which is
    /// what keeps two neighbouring layers meeting on a whole pixel.
    struct Layer {
        QPixmap pixmap;
        QPointF origin;
    };

    /// The indicator as a handful of layers rasterised at the physical pixel
    /// grid, because the paint sinks disagree about what they can antialias:
    /// the overlay widget is a raster device, the player's surface paints over
    /// a non-multisampled framebuffer, and Qt's OpenGL paint engine antialiases
    /// only through multisampling. Device-resolution alpha layers make both
    /// sinks draw the same smoothed coverage.
    ///
    /// The dashed perimeter is tiled rather than held as four window-long
    /// images: a layer wider or taller than GL_MAX_TEXTURE_SIZE cannot be
    /// uploaded by the OpenGL paint engine's texture cache, so every tile is
    /// kept inside a bound that any GPU this app runs on can hold.
    struct OverlayLayers {
        Layer card;
        QVector<Layer> ring;
        QRect available;
        QRect cardBox;
        qreal ratio = 0.0;
        bool built = false;
    };

    bool ensureLayers(const QRect& available, const QRect& card, int padding, qreal ratio) const;
    void dropLayers() const;
    void paintCard(QPainter& painter, const QRect& card, int padding) const;

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
    mutable OverlayLayers layers_;
    mutable int textWidth_ = -1;
    mutable QString displayedTitle_;
    mutable QString displayedFileName_;
    mutable QString displayedHint_;
};

} // namespace mikai::gui
