#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <atomic>
#include <memory>

namespace mplayer::shell {

bool saveComparisonGif(const QString& path, const QImage& a, const QImage& b,
                       int frameDelayMs, const std::atomic_bool& cancelled, QString* error);

class CompareGifExport final : public QObject {
    Q_OBJECT

public:
    explicit CompareGifExport(QObject* parent = nullptr);
    void start(const QString& path, const QImage& a, const QImage& b, int frameDelayMs);
    void cancel();

signals:
    void finished(bool succeeded, const QString& error);

private:
    std::shared_ptr<std::atomic_bool> cancelled_;
    bool started_ = false;
};

} // namespace mplayer::shell
