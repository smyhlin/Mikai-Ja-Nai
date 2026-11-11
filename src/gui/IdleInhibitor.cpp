#include "IdleInhibitor.hpp"

#include <algorithm>
#include <iterator>

#include <QtGlobal>

#ifdef Q_OS_LINUX
#  include <QDBusConnection>
#  include <QDBusInterface>
#  include <QDBusMessage>
#  include <QDBusUnixFileDescriptor>

#  include <utility>
#endif

#ifdef Q_OS_WIN
#  include <QtCore/qt_windows.h>
#endif

namespace mikai::gui {

struct IdleInhibitor::Platform {
#ifdef Q_OS_LINUX
    // logind hands back a file descriptor, and closing it releases the lock.
    // The screen-saver interface hands back a cookie instead; KDE and most
    // X11/Wayland desktops implement one of the two.
    QDBusUnixFileDescriptor logindFd;
    quint32 screenSaverCookie = 0;
    bool kdeScreenSaver = false;
#endif
};

namespace {
constexpr auto kInhibitReason = "Video is playing";
}  // namespace

IdleInhibitor::IdleInhibitor() : platform_(std::make_unique<Platform>()) {}

IdleInhibitor::~IdleInhibitor() {
#ifdef Q_OS_LINUX
    // Closing the descriptor is what releases a logind lock.
    platform_->logindFd = QDBusUnixFileDescriptor();
#endif
    releaseScreenSaver();
}

bool IdleInhibitor::active() const { return !owners_.empty(); }

#ifdef Q_OS_LINUX

// logind blocks idle (screen blanking, automatic suspend) for as long as the
// returned descriptor stays open.
bool IdleInhibitor::inhibitThroughLogind() {
    QDBusInterface manager(QStringLiteral("org.freedesktop.login1"),
                           QStringLiteral("/org/freedesktop/login1"),
                           QStringLiteral("org.freedesktop.login1.Manager"),
                           QDBusConnection::systemBus());
    if (!manager.isValid()) return false;
    const QDBusMessage reply =
        manager.call(QStringLiteral("Inhibit"), QStringLiteral("idle"), QStringLiteral("Mikai Ja Nai"),
                     QString::fromLatin1(kInhibitReason), QStringLiteral("block"));
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return false;
    const QDBusUnixFileDescriptor fd = reply.arguments().first().value<QDBusUnixFileDescriptor>();
    if (!fd.isValid()) return false;
    platform_->logindFd = fd;
    return true;
}

// The session screen saver, for sessions without logind (plain X11 setups).
bool IdleInhibitor::inhibitThroughScreenSaver() {
    for (const auto& [service, kde] : {std::pair{QStringLiteral("org.freedesktop.ScreenSaver"), false},
                                       std::pair{QStringLiteral("org.kde.screensaver"), true}}) {
        QDBusInterface screensaver(service, QStringLiteral("/ScreenSaver"),
                                   QStringLiteral("org.freedesktop.ScreenSaver"),
                                   QDBusConnection::sessionBus());
        if (!screensaver.isValid()) continue;
        const QDBusMessage reply = screensaver.call(QStringLiteral("Inhibit"),
                                                    QStringLiteral("mikai"),
                                                    QString::fromLatin1(kInhibitReason));
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) continue;
        platform_->screenSaverCookie = reply.arguments().first().toUInt();
        platform_->kdeScreenSaver = kde;
        return platform_->screenSaverCookie != 0;
    }
    return false;
}

void IdleInhibitor::releaseScreenSaver() {
    if (platform_->screenSaverCookie == 0) return;
    const QString service = platform_->kdeScreenSaver ? QStringLiteral("org.kde.screensaver")
                                                      : QStringLiteral("org.freedesktop.ScreenSaver");
    QDBusInterface screensaver(service, QStringLiteral("/ScreenSaver"),
                               QStringLiteral("org.freedesktop.ScreenSaver"),
                               QDBusConnection::sessionBus());
    if (screensaver.isValid()) {
        screensaver.call(QStringLiteral("UnInhibit"), platform_->screenSaverCookie);
    }
    platform_->screenSaverCookie = 0;
}

#else

bool IdleInhibitor::inhibitThroughLogind() { return false; }
bool IdleInhibitor::inhibitThroughScreenSaver() { return false; }
void IdleInhibitor::releaseScreenSaver() {}

#endif

void IdleInhibitor::setActiveFor(const void* owner, bool active) {
    if (owner == nullptr) return;
    const bool wasActive = !owners_.empty();
    const auto found = std::find(owners_.begin(), owners_.end(), owner);
    if (active) {
        if (found == owners_.end()) owners_.push_back(owner);
    } else if (found != owners_.end()) {
        // Unordered removal: the last owner takes the freed slot.
        *found = owners_.back();
        owners_.pop_back();
    }
    const bool isActive = !owners_.empty();
    if (isActive != wasActive) apply(isActive);
}

void IdleInhibitor::release(const void* owner) { setActiveFor(owner, false); }

void IdleInhibitor::apply(bool active) {
#ifdef Q_OS_WIN
    // One platform call, from the same setActiveFor() path the Linux
    // logind/D-Bus path uses. The execution state is per-thread and the
    // inhibitor is driven from the GUI thread, so the request lasts until
    // this same thread clears it; the system clears it anyway when the
    // thread ends. SetThreadExecutionState has existed since Windows 2000,
    // so there is no Windows version to state a limitation for.
    if (active) {
        SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    } else {
        SetThreadExecutionState(ES_CONTINUOUS);
    }
    return;
#endif
    if (active) {
        if (inhibitThroughLogind()) return;
        inhibitThroughScreenSaver();
        return;
    }
#ifdef Q_OS_LINUX
    // Closing the descriptor is what releases a logind lock.
    platform_->logindFd = QDBusUnixFileDescriptor();
#endif
    releaseScreenSaver();
}

IdleInhibitor& idleInhibitor() {
    static IdleInhibitor inhibitor;
    return inhibitor;
}

}  // namespace mikai::gui
