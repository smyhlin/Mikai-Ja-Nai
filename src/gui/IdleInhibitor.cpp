#include "IdleInhibitor.hpp"

#include <algorithm>
#include <iterator>

#include <QtGlobal>

#ifdef Q_OS_LINUX
#  include <QDBusConnection>
#  include <QDBusInterface>
#  include <QDBusMessage>
#  include <QDBusPendingCall>
#  include <QDBusPendingCallWatcher>
#  include <QDBusPendingReply>
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
    bool desiredActive = false;
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
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.login1"),
        QStringLiteral("/org/freedesktop/login1"),
        QStringLiteral("org.freedesktop.login1.Manager"),
        QStringLiteral("Inhibit"));
    msg << QStringLiteral("idle:sleep") << QStringLiteral("Mikai Ja Nai")
        << QString::fromLatin1(kInhibitReason) << QStringLiteral("block");
    auto* watcher = new QDBusPendingCallWatcher(
        QDBusConnection::systemBus().asyncCall(msg, 500));
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher,
                     [this, watcher]() {
                         QDBusPendingReply<QDBusUnixFileDescriptor> reply = *watcher;
                         if (reply.isValid() && platform_->desiredActive) {
                             const QDBusUnixFileDescriptor fd = reply.value();
                             if (fd.isValid()) platform_->logindFd = fd;
                         }
                         watcher->deleteLater();
                     });
    return true;
}

// The session screen saver, for sessions without logind (plain X11 setups).
bool IdleInhibitor::inhibitThroughScreenSaver() {
    for (const auto& [service, kde] : {std::pair{QStringLiteral("org.freedesktop.ScreenSaver"), false},
                                       std::pair{QStringLiteral("org.kde.screensaver"), true}}) {
        QDBusMessage msg = QDBusMessage::createMethodCall(
            service, QStringLiteral("/ScreenSaver"),
            QStringLiteral("org.freedesktop.ScreenSaver"),
            QStringLiteral("Inhibit"));
        msg << QStringLiteral("mikai") << QString::fromLatin1(kInhibitReason);
        auto* watcher = new QDBusPendingCallWatcher(
            QDBusConnection::sessionBus().asyncCall(msg, 500));
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher,
                         [this, service, kde, watcher]() {
                             QDBusPendingReply<quint32> reply = *watcher;
                             if (reply.isValid() && reply.value() != 0) {
                                 if (platform_->desiredActive && platform_->screenSaverCookie == 0) {
                                     platform_->screenSaverCookie = reply.value();
                                     platform_->kdeScreenSaver = kde;
                                 } else {
                                     QDBusMessage uninhibit = QDBusMessage::createMethodCall(
                                         service, QStringLiteral("/ScreenSaver"),
                                         QStringLiteral("org.freedesktop.ScreenSaver"),
                                         QStringLiteral("UnInhibit"));
                                     uninhibit << reply.value();
                                     QDBusConnection::sessionBus().asyncCall(uninhibit, 250);
                                 }
                             }
                             watcher->deleteLater();
                         });
    }
    return true;
}

void IdleInhibitor::releaseScreenSaver() {
    if (platform_->screenSaverCookie == 0) return;
    const QString service = platform_->kdeScreenSaver ? QStringLiteral("org.kde.screensaver")
                                                      : QStringLiteral("org.freedesktop.ScreenSaver");
    QDBusMessage msg = QDBusMessage::createMethodCall(
        service, QStringLiteral("/ScreenSaver"),
        QStringLiteral("org.freedesktop.ScreenSaver"),
        QStringLiteral("UnInhibit"));
    msg << platform_->screenSaverCookie;
    QDBusConnection::sessionBus().asyncCall(msg, 250);
    platform_->screenSaverCookie = 0;
}

bool IdleInhibitor::inhibitThroughPowerManagement() {
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
        QStringLiteral("/org/freedesktop/PowerManagement/Inhibit"),
        QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
        QStringLiteral("Inhibit"));
    msg << QStringLiteral("Mikai Ja Nai") << QString::fromLatin1(kInhibitReason);
    auto* watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(msg, 500));
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher,
                     [this, watcher]() {
                         QDBusPendingReply<quint32> reply = *watcher;
                         if (reply.isValid() && reply.value() != 0) {
                             if (platform_->desiredActive && platform_->powerManagementCookie == 0) {
                                 platform_->powerManagementCookie = reply.value();
                             } else {
                                 QDBusMessage uninhibit = QDBusMessage::createMethodCall(
                                     QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
                                     QStringLiteral("/org/freedesktop/PowerManagement/Inhibit"),
                                     QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
                                     QStringLiteral("UnInhibit"));
                                 uninhibit << reply.value();
                                 QDBusConnection::sessionBus().asyncCall(uninhibit, 250);
                             }
                         }
                         watcher->deleteLater();
                     });
    return true;
}

void IdleInhibitor::releasePowerManagement() {
    if (platform_->powerManagementCookie == 0) return;
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
        QStringLiteral("/org/freedesktop/PowerManagement/Inhibit"),
        QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
        QStringLiteral("UnInhibit"));
    msg << platform_->powerManagementCookie;
    QDBusConnection::sessionBus().asyncCall(msg, 250);
    platform_->powerManagementCookie = 0;
}

bool IdleInhibitor::inhibitThroughGnomeSession() {
    // Flags: 4 = suspend, 8 = idle (screen blanking / lock).
    constexpr quint32 kInhibitSuspendAndIdle = 4U | 8U;
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.gnome.SessionManager"),
        QStringLiteral("/org/gnome/SessionManager"),
        QStringLiteral("org.gnome.SessionManager"),
        QStringLiteral("Inhibit"));
    msg << QStringLiteral("Mikai Ja Nai") << quint32(0)
        << QString::fromLatin1(kInhibitReason) << kInhibitSuspendAndIdle;
    auto* watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(msg, 500));
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher,
                     [this, watcher]() {
                         QDBusPendingReply<quint32> reply = *watcher;
                         if (reply.isValid() && reply.value() != 0) {
                             if (platform_->desiredActive && platform_->gnomeSessionCookie == 0) {
                                 platform_->gnomeSessionCookie = reply.value();
                             } else {
                                 QDBusMessage uninhibit = QDBusMessage::createMethodCall(
                                     QStringLiteral("org.gnome.SessionManager"),
                                     QStringLiteral("/org/gnome/SessionManager"),
                                     QStringLiteral("org.gnome.SessionManager"),
                                     QStringLiteral("Uninhibit"));
                                 uninhibit << reply.value();
                                 QDBusConnection::sessionBus().asyncCall(uninhibit, 250);
                             }
                         }
                         watcher->deleteLater();
                     });
    return true;
}

void IdleInhibitor::releaseGnomeSession() {
    if (platform_->gnomeSessionCookie == 0) return;
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.gnome.SessionManager"),
        QStringLiteral("/org/gnome/SessionManager"),
        QStringLiteral("org.gnome.SessionManager"),
        QStringLiteral("Uninhibit"));
    msg << platform_->gnomeSessionCookie;
    QDBusConnection::sessionBus().asyncCall(msg, 250);
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
#ifdef Q_OS_LINUX
    platform_->desiredActive = active;
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
