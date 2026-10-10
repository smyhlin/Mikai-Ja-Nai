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

    bool capture(const QString& source, double timestamp, const QSize& outputSize,
                 const Cancelled& cancelled, const Progress& progress);
    QImage render(const QString& shaderId, const native::AnimeJanaiPlan& plan,
                  const Cancelled& cancelled, const Progress& progress);
    void stop();
    QString error() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}
