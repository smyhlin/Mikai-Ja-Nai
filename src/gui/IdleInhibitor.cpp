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
    // Desktop environments do not all honor the same inhibition bus:
    // - systemd-logind blocks logind idle/sleep actions (fd lock),
    // - org.freedesktop.ScreenSaver / org.kde.screensaver blocks DPMS & blanking,
    // - org.freedesktop.PowerManagement.Inhibit blocks KDE/XFCE auto-suspend,
    // - org.gnome.SessionManager blocks GNOME idle + suspend.
    // Acquiring all available ones together ensures the screen stays awake on
    // KDE Plasma, GNOME, XFCE, and Wayland/X11 sessions alike.
    QDBusUnixFileDescriptor logindFd;
    quint32 screenSaverCookie = 0;
    bool kdeScreenSaver = false;
    quint32 powerManagementCookie = 0;
    quint32 gnomeSessionCookie = 0;
#endif
};

namespace {
constexpr auto kInhibitReason = "Player window is open";
}  // namespace

IdleInhibitor::IdleInhibitor() : platform_(std::make_unique<Platform>()) {}

IdleInhibitor::~IdleInhibitor() {
#ifdef Q_OS_LINUX
    // Closing the descriptor is what releases a logind lock.
    platform_->logindFd = QDBusUnixFileDescriptor();
#endif
    releaseScreenSaver();
    releasePowerManagement();
    releaseGnomeSession();
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
    // X-M7: the default DBus timeout is 25 s; a wedged logind must not
    // freeze the GUI thread on every playback start/stop.
    manager.setTimeout(250);
    const QDBusMessage reply =
        manager.call(QStringLiteral("Inhibit"), QStringLiteral("idle:sleep"),
                     QStringLiteral("Mikai Ja Nai"), QString::fromLatin1(kInhibitReason),
                     QStringLiteral("block"));
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
        screensaver.setTimeout(250);
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
        screensaver.setTimeout(250);
        screensaver.call(QStringLiteral("UnInhibit"), platform_->screenSaverCookie);
    }
    platform_->screenSaverCookie = 0;
}

bool IdleInhibitor::inhibitThroughPowerManagement() {
    QDBusInterface power(QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
                         QStringLiteral("/org/freedesktop/PowerManagement/Inhibit"),
                         QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
                         QDBusConnection::sessionBus());
    if (!power.isValid()) return false;
    power.setTimeout(250);
    const QDBusMessage reply = power.call(QStringLiteral("Inhibit"),
                                          QStringLiteral("Mikai Ja Nai"),
                                          QString::fromLatin1(kInhibitReason));
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return false;
    platform_->powerManagementCookie = reply.arguments().first().toUInt();
    return platform_->powerManagementCookie != 0;
}

void IdleInhibitor::releasePowerManagement() {
    if (platform_->powerManagementCookie == 0) return;
    QDBusInterface power(QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
                         QStringLiteral("/org/freedesktop/PowerManagement/Inhibit"),
                         QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
                         QDBusConnection::sessionBus());
    if (power.isValid()) {
        power.setTimeout(250);
        power.call(QStringLiteral("UnInhibit"), platform_->powerManagementCookie);
    }
    platform_->powerManagementCookie = 0;
}

bool IdleInhibitor::inhibitThroughGnomeSession() {
    QDBusInterface session(QStringLiteral("org.gnome.SessionManager"),
                           QStringLiteral("/org/gnome/SessionManager"),
                           QStringLiteral("org.gnome.SessionManager"),
                           QDBusConnection::sessionBus());
    if (!session.isValid()) return false;
    session.setTimeout(250);
    // Flags: 4 = suspend, 8 = idle (screen blanking / lock).
    constexpr quint32 kInhibitSuspendAndIdle = 4U | 8U;
    const QDBusMessage reply = session.call(QStringLiteral("Inhibit"),
                                            QStringLiteral("Mikai Ja Nai"),
                                            quint32(0),
                                            QString::fromLatin1(kInhibitReason),
                                            kInhibitSuspendAndIdle);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return false;
    platform_->gnomeSessionCookie = reply.arguments().first().toUInt();
    return platform_->gnomeSessionCookie != 0;
}

void IdleInhibitor::releaseGnomeSession() {
    if (platform_->gnomeSessionCookie == 0) return;
    QDBusInterface session(QStringLiteral("org.gnome.SessionManager"),
                           QStringLiteral("/org/gnome/SessionManager"),
                           QStringLiteral("org.gnome.SessionManager"),
                           QDBusConnection::sessionBus());
    if (session.isValid()) {
        session.setTimeout(250);
        session.call(QStringLiteral("Uninhibit"), platform_->gnomeSessionCookie);
    }
    platform_->gnomeSessionCookie = 0;
}

#else

bool IdleInhibitor::inhibitThroughLogind() { return false; }
bool IdleInhibitor::inhibitThroughScreenSaver() { return false; }
void IdleInhibitor::releaseScreenSaver() {}
bool IdleInhibitor::inhibitThroughPowerManagement() { return false; }
void IdleInhibitor::releasePowerManagement() {}
bool IdleInhibitor::inhibitThroughGnomeSession() { return false; }
void IdleInhibitor::releaseGnomeSession() {}

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
        // Acquire all available session & system inhibitors together: logind
        // alone does not stop KDE PowerDevil, GNOME, or X11/Wayland DPMS from
        // blanking the display.
        inhibitThroughLogind();
        inhibitThroughScreenSaver();
        inhibitThroughPowerManagement();
        inhibitThroughGnomeSession();
        return;
    }
#ifdef Q_OS_LINUX
    // Closing the descriptor is what releases a logind lock.
    platform_->logindFd = QDBusUnixFileDescriptor();
#endif
    releaseScreenSaver();
    releasePowerManagement();
    releaseGnomeSession();
}

IdleInhibitor& idleInhibitor() {
    static IdleInhibitor inhibitor;
    return inhibitor;
}

}  // namespace mikai::gui
