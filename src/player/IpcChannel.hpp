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
#include <QObject>
#include <QTimer>

#include <mutex>

namespace mplayer {
namespace shell {

/// Polls stdin for complete JSON lines and re-emits each one.
///
/// Deliberately not a reader thread. A thread parked in fgets cannot be stopped
/// without either TerminateThread (which leaves the CRT's stdin lock held and
/// the process faults on the way out) or CancelSynchronousIo on a handle Qt
/// stops handing out once QThread::handle() leaves the public API. PeekNamedPipe
/// answers "is there data" without blocking, so this needs no thread at all, and
/// shutdown becomes "stop the timer".
class IpcReader : public QObject {
    Q_OBJECT

public:
    explicit IpcReader(QObject* parent = nullptr);
    ~IpcReader() override;

    /// Begins polling. Cheap: it only calls PeekNamedPipe.
    void start();
    void requestStop();

Q_SIGNALS:
    void command(const QJsonObject& command);
    /// stdin reached EOF or broke: the host is gone, so the player should exit.
    void hostClosed();

private:
    void poll();

    QTimer timer_;
    QByteArray buffer_;
    bool stopped_ = false;
};

class IpcWriter {
public:
    /// Writes one JSON object as a line and flushes it, so Python sees it
    /// immediately rather than when a pipe buffer happens to fill.
    static void send(const QJsonObject& event);
};

}  // namespace shell
}  // namespace mplayer
