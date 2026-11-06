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
                 const std::function<QImage()>& fallbackProvider = {});
    QImage render(const QString& shaderId, const native::AnimeJanaiPlan& plan,
                  const Cancelled& cancelled, const Progress& progress);
    void stop();
    QString error() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}
