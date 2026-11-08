#pragma once

#include <QSize>
#include <QString>
#include <QObject>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mplayer::shell {

inline constexpr double kMaximumClipDuration = 60.0;

struct ClipExportRequest {
    QString source;
    QString outputPath;
    bool replaceExisting = false;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    QSize sourceSize;
    int maximumHeight = 1080;
    std::optional<qint64> audioTrackId;
    QString externalAudioPath;
    std::vector<std::pair<std::string, std::string>> sourceOptions;
};

class ClipExport final : public QObject {
    Q_OBJECT
public:
    explicit ClipExport(QObject* parent = nullptr);
    ~ClipExport() override;
    void start(const ClipExportRequest& request);
    void cancel();

signals:
    void encodingStarted(const QString& videoCodec);
    void progressChanged(double fraction);
    void finished(bool succeeded, const QString& outputPath, const QString& error);

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace mplayer::shell
