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

#include "FramePreviewService.hpp"
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
/// a superseded token, or the newest token for another instant, is not this
/// instant's picture.
bool previewReplyMatches(quint64 replyToken, quint64 currentToken, double replySeconds,
                         double currentSeconds);

/// Whether a pointer at `cursor` still belongs to the card that stands between
/// `bar` and `card`: inside either, or anywhere in the band the two of them
/// span. A pointer leaving the timeline for the card it just opened must not
/// kill the card halfway across the gap, which is why the band and not the two
/// rects alone is the answer. All three are in the same (global) coordinates.
bool hoverCardKeepsPointer(const QRect& bar, const QRect& card, const QPoint& cursor);

/// The hover card of one player window.
class HoverPreview : public QWidget, public ChromePaint {
    Q_OBJECT

public:
    /// `host` is the shell the card is positioned inside.
    explicit HoverPreview(QWidget* host);
    ~HoverPreview() override;

    /// The media the card previews, and the identity of the loaded source it
    /// belongs to. The epoch is what makes a same-URL reload a new source; an
    /// unchanged (url, epoch) pair costs nothing.
    void setSource(const QString& url, quint64 sourceEpoch);

    /// Shows the card for `seconds`, centred on `anchorGlobal` and above
    /// `barGlobal` unless that does not fit. `segmentLabel` arrives localised.
    /// `allowFrame` is false while the playhead is dragged, when a decode per
    /// pixel of travel has nothing to show for it.
    void present(double seconds, const QPoint& anchorGlobal, const QRect& barGlobal,
                 const QString& segmentLabel, bool allowFrame);

    /// Decodes `seconds` ahead of a hover and stores it. Low priority: the
    /// decoder serves a card request first, and a hover that arrives during a
    /// warm-up replaces it. Never published to the card as its own reply.
    void prewarm(double seconds);

    /// Hides the card and drops the outstanding decode.
    void dismiss();

    /// Stops the extraction thread without waiting for it. Called by the shell
    /// when its window closes; every later call is a no-op.
    void release();

    bool hasFrame() const { return !frame_.isNull(); }
    /// The instant the picture on the card was decoded for, or -1 when there is
    /// no picture. This is the decoder's own timestamp, which is what the card
    /// prints: it is never the pointer's instant relabelled onto an older frame.
    double frameSeconds() const { return frameSeconds_; }
    /// Whether `hasFrame()`'s picture is an explicitly stale one: the pointer has
    /// moved to another instant and the decode for it is still running.
    bool isFrameStale() const { return hasFrame() && !status_.isEmpty(); }
    /// What the card says while it has no picture for the hovered instant:
    /// "Decoding preview…" while a decode is in flight, a refusal's sentence
    /// once it is known, empty when the picture is the hovered instant's.
    QString statusText() const { return status_; }
    /// The decoder's own explanation behind a refusal, empty otherwise.
    QString statusDetail() const { return detail_; }
    /// What the preview decoder actually did: hits, worker requests and the last
    /// latencies, so a slow card can be told apart from a store that never hits.
    FramePreviewStats previewStats() const;

    void paintChrome(QPainter& painter) override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void place();
    void applyFrame(quint64 token, double requestedSeconds, double decodedSeconds,
                    const QImage& frame);
    void applyUnavailable(quint64 token, double seconds, const QString& reason);

    FramePreviewService* service_ = nullptr;
    /// Kept so a frame with a different aspect can re-place the card without
    /// the pointer having moved again.
    QPoint anchor_;
    QRect bar_;
    QString label_;
    QString status_;
    QString detail_;
    /// A refusal is terminal for this instant: no picture is coming. Tracked
    /// rather than inferred from the status text, which is display copy.
    bool refused_ = false;
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
