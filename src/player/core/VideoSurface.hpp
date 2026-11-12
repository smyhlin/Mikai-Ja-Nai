// Widgets presentation adapter.
//
// This class is no longer a player: it is a QOpenGLWidget whose backing
// framebuffer receives the shared VideoRenderBridge's stable output, with the
// widget-only chrome (overlay, opening placeholder, rounded corners) painted on
// top. Everything else - the mpv core, source identity, transport intent,
// tracks, enhancement chains, policies - lives in one non-visual VideoSession,
// and every GL object lives in one VideoRenderBridge. This class is the
// presentation host between the two: it implements VideoPresenter over that
// bridge.
//
// Rendering stays on the GUI thread with Qt's GL context current; a
// QOpenGLWidget drives Qt's basic render loop here, so the GUI thread IS the
// rendering thread. Core requests are asynchronous.

#pragma once

#include <QImage>
#include <QMetaObject>
#include <QOpenGLWidget>
#include <QPointF>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <memory>

#include "VideoPresenter.hpp"
#include "VideoRenderBridge.hpp"
#include "VideoSession.hpp"

// Declared where the view is complete: paintOverlay() hands the overlay code a
// painter, never an owned object.
class QPainter;
class QOpenGLContext;
class QShowEvent;
class QHideEvent;

namespace mplayer {

/// The Widgets adapter. Owns one VideoSession (control and observed state), one
/// VideoRenderBridge (the only GL renderer) and this widget's chrome.
class VideoSurface : public QOpenGLWidget, public VideoPresenter {
    Q_OBJECT

public:
    explicit VideoSurface(QWidget* parent = nullptr);
    ~VideoSurface() override;

    /// The canonical engine object. Widget rendering and lifecycle stay on this adapter.
    VideoSession* session() const { return session_; }

    // ── widget chrome ─────────────────────────────────────────────────────
    /// Rounds the displayed picture's corners with antialiased alpha coverage,
    /// in logical pixels; 0 keeps them square. Chrome belongs to this widget,
    /// never to the session or the renderer.
    void setDisplayCornerRadius(int radius);
    int displayCornerRadius() const { return displayCornerRadius_; }

    /// Returns the composed widget framebuffer for renderer probes and captures.
    QImage grabVideoFramebuffer() { return session_->grabVideoFramebuffer(); }

    // ── widget render facts ───────────────────────────────────────────────
    using RenderTargetColorBits = mplayer::RenderTargetColorBits;
    using UiProfileCounters = mplayer::UiProfileCounters;
    using PaintScheduleDecision = mplayer::PaintScheduleDecision;

    // VideoSession queries these through VideoPresenter; read the bridge directly
    // here to avoid re-entering this widget.
    int renderTargetInternalFormat() const {
        return bridge_ != nullptr ? bridge_->targetInternalFormat() : 0;
    }
    RenderTargetColorBits renderTargetColorBits() const {
        return bridge_ != nullptr ? bridge_->targetColorBits() : RenderTargetColorBits{};
    }
    int renderTargetDepthBitsPerChannel() const {
        return bridge_ != nullptr ? bridge_->targetDepthBitsPerChannel() : 0;
    }
    bool renderTargetFormatProbed() const {
        return bridge_ != nullptr && bridge_->targetFormatProbed();
    }
    bool isContextReady() const { return bridge_ != nullptr && bridge_->contextReady(); }
    bool waitForContext(int timeoutMs) override;
    quint64 renderContextGeneration() const override {
        return bridge_ != nullptr ? bridge_->contextGeneration() : 0;
    }
    bool hasVisibleOutputFrame() const override {
        return bridge_ != nullptr && bridge_->hasVisibleOutputFrame();
    }
    bool hasRetainedPicture() const override {
        return bridge_ != nullptr && bridge_->hasRetainedPicture();
    }
    qint64 retainedPictureBytes() const override {
        return bridge_ != nullptr ? bridge_->retainedPictureBytes() : 0;
    }
    UiProfileCounters uiProfileCounters() const override {
        return bridge_ != nullptr ? bridge_->uiProfileCounters() : UiProfileCounters{};
    }

    /// A paint that presents no new frame must neither consume the frame a
    /// pacing slot is holding nor stop that slot.
    static PaintScheduleDecision decidePaintSchedule(bool pacedRelease, bool forced,
                                                     bool armedSlot, bool frameOwed,
                                                     bool holdOwesOutput) {
        return VideoRenderBridge::decidePaintSchedule(pacedRelease, forced, armedSlot, frameOwed,
                                                      holdOwesOutput);
    }

signals:
    /// Widget-only chrome: emitted once per paintGL, after the picture and Qt's
    /// GL state are restored and before the painter is ended.
    void paintOverlay(QPainter* painter);

    // ── VideoPresenter overrides ──────────────────────────────────────────
public:
    RenderTargetFacts targetFacts() const override;
    bool canPresentNow() const override;
    void requestRepaint() override;

    void acquireRetainedPicture() override;
    void releaseRetainedPicture() override;
    void retainAcrossLoad() override;
    void armLoadLanding(uint64_t sourceEpoch) override;
    void noteLoadLanded(uint64_t sourceEpoch) override;
    void noteLoadRefused(uint64_t sourceEpoch) override;
    void invalidateRetainedPictures() override;
    void refreshRenderState() override;

    QImage readVideoFrame(QString* reason) override;
    void readVideoFrameAsync(std::function<void(QImage, QString)> completion) override;

    bool ensureRenderContext(std::shared_ptr<MpvHandle> core, QString* reason = nullptr) override;
    void releaseRenderContext() override;

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    /// The GUI-thread end of mpv's update callback (the bridge coalesces it).
    /// Advances render state, arms or drops the presentation slot and asks for
    /// the paint that presents what mpv handed out.
    void onRenderUpdate();
    /// The armed pacing slot arrived: this paint is the one that presents the
    /// frame, and it measures the lead the next slot subtracts.
    void onPacingTick();
    /// The frame painted last actually reached the compositor.
    void onFrameSwapped();

    bool ensureContextCurrent(QOpenGLContext** previous);
    void releaseContextCurrent(QOpenGLContext* previous);
    void handleContextAboutToBeDestroyed();
    void notifyTargetState();
    void publishPresentation(const RenderResult& result);
    void reportRenderFailure(int renderError);

    void paintWithoutRenderer();
    void updatePlaceholderAnimation();
    void paintOpeningPlaceholder(QPainter& painter);
    void paintOpeningRing(QPainter& painter, const QPointF& centre, double radius,
                          double penWidth);
    void paintDisplayCornerCoverage(QPainter& painter, int targetWidth, int targetHeight);

    static bool clientFramePacingEnabled();

    VideoSession* session_ = nullptr;
    /// The only renderer for this view: one stable overlay-free output, the
    /// render context and every GL object.
    std::unique_ptr<VideoRenderBridge> bridge_;
    /// Guards the bridge's `onUpdate` after this widget is gone.
    std::shared_ptr<const void> presenterToken_;
    QOpenGLContext* context_ = nullptr;
    QMetaObject::Connection contextDestroyedConnection_;

    /// State of the pass about to run, captured on this thread. Immutable for
    /// the duration of the pass.
    RenderSnapshot snapshot_;
    RenderTargetFacts facts_;
    /// A repaint was asked for because the chain changed under a possibly
    /// paused core; the next pass forces one render-state advance.
    bool refreshRequested_ = false;
    /// True once a pass really ran with the render context: before that an
    /// unexposed window must still be allowed to paint, because the context
    /// exists only because Qt already painted this widget.
    bool paintedOnce_ = false;
    /// One render failure report per streak, never one per paint.
    bool renderFailureStreak_ = false;
    bool destroying_ = false;

    /// Defers a pending frame to the compositor slot mpv named for it.
    QTimer pacingTimer_;
    /// Animates the no-picture placeholder.
    QTimer placeholderTimer_;
    int placeholderPhase_ = 0;

    /// Antialiased alpha coverage for the displayed picture's four corners.
    QImage displayCornerTopLeft_;
    QImage displayCornerTopRight_;
    QImage displayCornerBottomLeft_;
    QImage displayCornerBottomRight_;
    int displayCornerRadius_ = 0;
    qreal displayCornerRatio_ = 0.0;

    /// The opening ring, rasterised once per phase into one reused buffer.
    QImage openingRing_;
    qreal openingRingRatio_ = 0.0;
    double openingRingRadius_ = 0.0;
    int openingRingPhase_ = -1;
};

}  // namespace mplayer
