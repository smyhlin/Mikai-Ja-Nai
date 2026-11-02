#include "ForegroundPeer.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QScreen>
#include <QWidget>

#include <cstdio>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

namespace mikai::probes {

ForegroundPeer::~ForegroundPeer() {
    stop();
}

bool ForegroundPeer::start(const QRect& globalGeometry, QString* error) {
    stop();
    const QRect rect = globalGeometry.isEmpty()
                           ? QRect(QPoint(240, 160), QSize(480, 360))
                           : globalGeometry;
    process_.setProgram(QCoreApplication::applicationFilePath());
    process_.setArguments({QStringLiteral("--foreground-helper"),
                           QString::number(rect.x()), QString::number(rect.y()),
                           QString::number(rect.width()), QString::number(rect.height())});
    process_.start();
    if (!process_.waitForStarted(4000)) {
        if (error != nullptr) *error = QStringLiteral("foreground helper did not start");
        return false;
    }
    QString output;
    while (!output.contains(QStringLiteral("winid=")) && process_.waitForReadyRead(4000)) {
        output += QString::fromLocal8Bit(process_.readAllStandardOutput());
    }
    output += QString::fromLocal8Bit(process_.readAllStandardOutput());
    const qsizetype pidAt = output.indexOf(QStringLiteral("pid="));
    const qsizetype winidAt = output.indexOf(QStringLiteral("winid="));
    if (pidAt < 0 || winidAt < 0) {
        if (error != nullptr) *error = QStringLiteral("foreground helper produced no pid/winid");
        stop();
        return false;
    }
    bool pidOk = false;
    bool winidOk = false;
    processId_ = output.mid(pidAt + 4).section(QLatin1Char(' '), 0, 0).toLongLong(&pidOk);
    QString winidText = output.mid(winidAt + 6).section(QLatin1Char('\n'), 0, 0).trimmed();
    if (winidText.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        winidText = winidText.mid(2);
    windowId_ = winidText.toULongLong(&winidOk, 16);
#ifdef Q_OS_WIN
    supported_ = pidOk && winidOk && windowId_ != 0;
#else
    supported_ = false;
#endif
    if (!supported_) {
        if (error != nullptr) {
            *error = QStringLiteral("native activation of another process's window is not "
                                    "available on this platform");
        }
        stop();
        return false;
    }
    return true;
}

bool ForegroundPeer::activate() const {
#ifdef Q_OS_WIN
    const HWND target = reinterpret_cast<HWND>(windowId_);
    if (target == nullptr || !IsWindow(target)) return false;
    if (SetForegroundWindow(target)) return true;
    const HWND current = GetForegroundWindow();
    const DWORD currentThread =
        current != nullptr ? GetWindowThreadProcessId(current, nullptr) : 0;
    const DWORD targetThread = GetWindowThreadProcessId(target, nullptr);
    const DWORD thisThread = GetCurrentThreadId();
    if (currentThread == 0 || targetThread == 0) return false;
    const bool attachedCurrent =
        currentThread != thisThread &&
        AttachThreadInput(currentThread, thisThread, TRUE) != FALSE;
    const bool attachedTarget =
        targetThread != thisThread &&
        AttachThreadInput(currentThread, targetThread, TRUE) != FALSE;
    SetForegroundWindow(target);
    SetActiveWindow(target);
    if (attachedTarget) AttachThreadInput(currentThread, targetThread, FALSE);
    if (attachedCurrent) AttachThreadInput(currentThread, thisThread, FALSE);
    return GetForegroundWindow() == target;
#else
    return false;
#endif
}

void ForegroundPeer::sendToBack() const {
#ifdef Q_OS_WIN
    const HWND target = reinterpret_cast<HWND>(windowId_);
    if (target == nullptr || !IsWindow(target)) return;
    SetWindowPos(target, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
}

void ForegroundPeer::raiseToFront() const {
#ifdef Q_OS_WIN
    const HWND target = reinterpret_cast<HWND>(windowId_);
    if (target == nullptr || !IsWindow(target)) return;
    SetWindowPos(target, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
}

bool ForegroundPeer::ownsForeground() const {
#ifdef Q_OS_WIN
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr || reinterpret_cast<quintptr>(foreground) != windowId_)
        return false;
    DWORD owner = 0;
    GetWindowThreadProcessId(foreground, &owner);
    return owner != 0 && owner != GetCurrentProcessId() &&
           owner == static_cast<DWORD>(processId_);
#else
    return false;
#endif
}

void ForegroundPeer::stop() {
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(2000);
    }
    windowId_ = 0;
    processId_ = 0;
    supported_ = false;
}

int runForegroundHelperMode(QApplication& app, const QStringList& arguments) {
    QRect geometry(QPoint(240, 160), QSize(480, 360));
    const qsizetype flag = arguments.indexOf(QStringLiteral("--foreground-helper"));
    if (flag >= 0 && flag + 4 < arguments.size()) {
        geometry = QRect(arguments.at(flag + 1).toInt(), arguments.at(flag + 2).toInt(),
                         arguments.at(flag + 3).toInt(), arguments.at(flag + 4).toInt());
    } else if (QScreen* screen = app.primaryScreen()) {
        geometry.moveCenter(screen->geometry().center());
    }
    QWidget window;
    window.setStyleSheet(QStringLiteral("background:#000000;"));
    window.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    window.setGeometry(geometry);
    window.show();
    window.raise();
    window.activateWindow();
    const WId nativeId = window.winId();
    std::printf("MIKAI_FOREGROUND_HELPER pid=%lld winid=0x%llx\n",
                static_cast<long long>(QCoreApplication::applicationPid()),
                static_cast<unsigned long long>(nativeId));
    std::fflush(stdout);
    return app.exec();
}

} // namespace mikai::probes
