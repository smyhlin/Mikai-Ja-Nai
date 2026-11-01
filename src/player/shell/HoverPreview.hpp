// The card above the timeline: the frame at the hovered instant, its timestamp
// and the skip segment that instant falls in.
//
// A child of the shell rather than a Qt tooltip, so it stays inside the window
// and follows the shell's own placement; opaque, so painting it does not need a
// second pass over the picture. It never takes mouse events or focus, so a
// viewer can still scrub through it.

#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QWidget>

#include "ShellWidgets.hpp"

namespace mplayer {
namespace shell {

class FramePreviewService;

/// The rect the card takes inside `bounds`, anchored on the timeline and above
/// the bar, or below it when there is no room above. Clamped to `bounds` on
/// both axes. `aspect` is the frame's width/height; anything implausible is
/// read as 16:9.
QRect previewCardRect(const QRect& bounds, const QPoint& anchor, const QRect& bar,
                      double aspect);

/// Whether a decoder reply describes what the card shows: the newest token the
/// card issued and the same instant to within one frame time. A reply carrying
/// a superseded token, or the newest token for another instant, is dropped
/// rather than painted.
bool previewReplyMatches(quint64 replyToken, quint64 currentToken, double replySeconds,
                         double currentSeconds);

/// The hover card of one player window.
class HoverPreview : public QWidget, public ChromePaint {
    Q_OBJECT

public:
    /// `host` is the shell the card is positioned inside.
    explicit HoverPreview(QWidget* host);
    ~HoverPreview() override;

    /// The media the card previews; an unchanged URL costs nothing.
    void setSource(const QString& url);

    /// Shows the card for `seconds`, centred on `anchorGlobal` and above
    /// `barGlobal` unless that does not fit. `segmentLabel` arrives localised.
    /// `allowFrame` is false while the playhead is dragged, when a decode per
    /// pixel of travel has nothing to show for it.
    void present(double seconds, const QPoint& anchorGlobal, const QRect& barGlobal,
                 const QString& segmentLabel, bool allowFrame);

    /// Hides the card and drops the outstanding decode.
    void dismiss();

    /// Stops the extraction thread without waiting for it. Called by the shell
    /// when its window closes; every later call is a no-op.
    void release();

    bool hasFrame() const { return !frame_.isNull(); }
    /// Why the card carries no frame, when the decoder said so; empty while a
    /// frame is still being decoded.
    QString statusText() const { return status_; }

    void paintChrome(QPainter& painter) override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void place();
    void applyFrame(quint64 token, double seconds, const QImage& frame);
    void applyUnavailable(quint64 token, double seconds, const QString& reason);

    FramePreviewService* service_ = nullptr;
    /// Kept so a frame with a different aspect can re-place the card without
    /// the pointer having moved again.
    QPoint anchor_;
    QRect bar_;
    QString label_;
    QString status_;
    QImage frame_;
    /// The instant `frame_` was decoded for, or -1 when there is no frame.
    double frameSeconds_ = -1.0;
    double seconds_ = 0.0;
    double aspect_ = 16.0 / 9.0;
    /// The newest request issued; a reply carrying another token belongs to a
    /// pointer position that is no longer on screen.
    quint64 token_ = 0;
    bool released_ = false;
};

}  // namespace shell
}  // namespace mplayer
