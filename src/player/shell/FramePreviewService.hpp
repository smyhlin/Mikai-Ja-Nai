// The frame behind the timeline's hover card: a second, headless libmpv core
// (vo=null, screenshot-sw=yes) that decodes one frame per hovered instant on
// its own thread, so a hover never moves the player's playhead, never paints
// through its render context and never blocks the GUI thread.

#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>
#include <QtGlobal>

QT_BEGIN_NAMESPACE
class QThread;
QT_END_NAMESPACE

namespace mplayer {
namespace shell {

class FramePreviewWorker;

/// Copies one `screenshot-raw video rgba` capture into an owned frame.
///
/// Every declared size is validated before it indexes anything: the channel
/// order must be RGBA, the pitch must cover the width, and a payload shorter
/// than the declared rows is refused rather than read past. Padding beyond
/// `width * 4` per row is dropped. A refused capture is a null image and, when
/// `reason` is given, the sentence that says why.
QImage previewFrameFromRaw(const QByteArray& format, qint64 width, qint64 height, qint64 stride,
                           const char* payload, qsizetype size, QString* reason = nullptr);

/// Decodes single frames of the current source on its own thread.
///
/// The service lives on the GUI thread and owns the thread; the mpv core is
/// created, used and destroyed on the worker, and only plain values cross.
class FramePreviewService : public QObject {
    Q_OBJECT

public:
    explicit FramePreviewService(QObject* parent = nullptr);
    ~FramePreviewService() override;

    /// The media the next request decodes from. The core is not created here -
    /// a window nobody hovers costs no decoder - and an unchanged URL is a
    /// no-op, because the shell republishes it for reasons that do not move it.
    void setSource(const QString& url);
    QString source() const { return source_; }

    /// Asks for the frame at `seconds`; the answer carries `token` back, so a
    /// caller that hovers faster than the decoder shows only its last request.
    /// `token` 0 is not a request.
    void requestFrame(quint64 token, double seconds);

    /// Drops the outstanding request. A capture in flight is cancelled; a seek
    /// or a load that cannot be recalled settles without answering.
    void cancel();

    /// Stops the worker without waiting for it: the core is destroyed on the
    /// worker thread and the thread quits behind the GUI. Idempotent, and a
    /// shut service starts nothing afterwards.
    void shutdown();

signals:
    void frameReady(quint64 token, double seconds, const QImage& frame);
    /// No frame exists for that instant - an unseekable source, a refused
    /// capture - and `reason` is the sentence that says so.
    void frameUnavailable(quint64 token, double seconds, const QString& reason);

    // ── posted to the worker; not part of the consumer's contract ──────────
    void sourceChanged(const QString& url, quint64 generation);
    void frameRequested(quint64 token, double seconds);
    void cancelRequested();

private:
    void ensureWorker();

    QThread* thread_ = nullptr;
    FramePreviewWorker* worker_ = nullptr;
    QString source_;
    /// Bumped per source change, so the worker can tell a request issued
    /// against the source it holds from one issued against a replaced URL.
    quint64 generation_ = 0;
    quint64 pendingToken_ = 0;
    bool stopped_ = false;
};

}  // namespace shell
}  // namespace mplayer
