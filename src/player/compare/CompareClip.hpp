#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <atomic>
#include <memory>

namespace mplayer::shell {

enum class CompareClipFormat { Mp4, AnimatedWebP };

bool saveComparisonClip(const QString& path, const QImage& a, const QImage& b,
                        int frameDelayMs, CompareClipFormat format,
                        const std::atomic_bool& cancelled, QString* error);

class CompareClipExport final : public QObject {
    Q_OBJECT

public:
    explicit CompareClipExport(QObject* parent = nullptr);
    void start(const QString& path, const QImage& a, const QImage& b, int frameDelayMs,
               CompareClipFormat format);
    void cancel();

signals:
    void finished(bool succeeded, const QString& error);

private:
    std::shared_ptr<std::atomic_bool> cancelled_;
    bool started_ = false;
};

} // namespace mplayer::shell
