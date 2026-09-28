// Line-delimited JSON over the player process's stdin/stdout.
//
// The player is a separate executable because PySide6 and the C++ Qt are
// separate Qt builds whose Qt6Core.dll/Qt6Gui.dll/Qt6Widgets.dll collide by
// name: loading both into one process fails with WinError 127. A process
// boundary is not a workaround for that, it is the only shape that works, and
// the player was already its own top-level window before the split.
//
// Protocol: one JSON object per line in each direction. Python writes commands
// to the child's stdin; the child writes events to its stdout. Nothing else is
// shared, so a malformed line can only drop one message.

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <mutex>

namespace mplayer {
namespace shell {

/// Reads newline-delimited JSON from stdin on its own thread and re-emits each
/// object. The read is blocking, which is why this is a QThread rather than a
/// QSocketNotifier: a notifier on a Windows pipe is unreliable, and the thread
/// has a life that ends with the process anyway.
class IpcReader : public QThread {
    Q_OBJECT

public:
    explicit IpcReader(QObject* parent = nullptr);
    ~IpcReader() override;

Q_SIGNALS:
    /// Emitted for each parsed command, on an arbitrary thread.
    void command(const QJsonObject& command);
    /// stdin reached EOF: the host is gone, so the player should exit.
    void hostClosed();

protected:
    void run() override;

private:
    std::atomic<bool> stop_{false};
};

class IpcWriter {
public:
    /// Writes one JSON object as a line and flushes it, so Python sees it
    /// immediately rather than when a pipe buffer happens to fill.
    static void send(const QJsonObject& event);
};

}  // namespace shell
}  // namespace mplayer
