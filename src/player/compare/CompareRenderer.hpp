#pragma once

#include <QImage>
#include <QString>

#include <functional>
#include <memory>

#include "native/components/AnimeJanaiRuntime.hpp"

namespace mplayer::shell {

class CompareRenderer {
public:
    using Cancelled = std::function<bool()>;
    using Progress = std::function<void(const QString&)>;

    CompareRenderer();
    ~CompareRenderer();
    CompareRenderer(const CompareRenderer&) = delete;
    CompareRenderer& operator=(const CompareRenderer&) = delete;

    /// `fallbackSource` is used only when a fresh decode of `source` cannot be
    /// produced: its pixels are then the untouched source frame, captured from
    /// the surface with its filters removed, so a stream URL that refuses a
    /// second open does not leave the comparison blank.
    /// `fallbackProvider` is asked for a still only when a fresh decode of
    /// `source` cannot be produced. It is lazy on purpose: capturing from the
    /// playing surface removes that surface's video filter for a moment, which
    /// must not happen when the pane's own decode already works.
    bool capture(const QString& source, double timestamp, const QSize& outputSize,
                 const Cancelled& cancelled, const Progress& progress,
                 const std::function<QImage()>& fallbackProvider = {},
                 QImage* capturedSource = nullptr);
    QImage render(const QString& shaderId, const native::AnimeJanaiPlan& plan,
                  const Cancelled& cancelled, const Progress& progress);
    /// The unprocessed picture of the most recent native render: the same
    /// decoder, seek, step count, canvas and scaler with the stage removed.
    /// Empty when that render was a still, or its baseline could not be drawn.
    QImage baseline() const;
    /// One line of replay evidence for the last native render: the decoder that
    /// ran, the geometry that reached the output, the frames presented and the
    /// steps taken. Empty for a still render.
    QString replaySummary() const;
    /// The last frame handed to the video output, read through mpv's own
    /// `screenshot-raw` path (`bgr0`). It is taken from the frame mpv presented,
    /// not from the framebuffer, so it can prove that what was read back is what
    /// was delivered. Empty when the core cannot answer.
    QImage lastVideoFrame() const;
    void stop();
    QString error() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}
