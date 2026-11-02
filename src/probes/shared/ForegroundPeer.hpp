// A separate probe process that shows one black window, so foreground switching
// is exercised against another process rather than a window of our own.
#pragma once

#include <QProcess>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QtGlobal>

class QApplication;

namespace mikai::probes {

class ForegroundPeer {
public:
    ForegroundPeer() = default;
    ForegroundPeer(const ForegroundPeer&) = delete;
    ForegroundPeer& operator=(const ForegroundPeer&) = delete;
    ~ForegroundPeer();

    /// Spawns this executable in --foreground-helper mode and waits for its
    /// pid/window id line. `globalGeometry` is the black window's screen rect.
    bool start(const QRect& globalGeometry, QString* error);
    void stop();

    /// Native activation of another process's window is implemented on Windows
    /// only; elsewhere the measurement is reported unavailable.
    bool supported() const { return supported_; }
    bool activate() const;
    /// Places the peer behind the player after activation; cross-process
    /// SetWindowPos, so the occlusion removal is deterministic.
    void sendToBack() const;
    /// Raises the peer's window without changing the foreground; used by the
    /// readiness barrier so the first away interval shows the black window.
    void raiseToFront() const;
    /// True only when the native foreground is this peer's window and its pid is
    /// not ours.
    bool ownsForeground() const;

    quintptr windowId() const { return windowId_; }
    qint64 processId() const { return processId_; }

private:
    QProcess process_;
    quintptr windowId_ = 0;
    qint64 processId_ = 0;
    bool supported_ = false;
};

/// Early mode: shows the black window, prints `pid=... winid=...`, runs the loop.
int runForegroundHelperMode(QApplication& app, const QStringList& arguments);

} // namespace mikai::probes
