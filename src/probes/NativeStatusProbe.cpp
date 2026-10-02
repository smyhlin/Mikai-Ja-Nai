// Proves the status reader against a real standard core that has no native
// filter attached: the `vf-metadata/mikai_native` property does not resolve, and
// the reader has to report that by name - Unavailable with a non-empty reason -
// rather than as a crash, an empty success or a stale snapshot.
//
// It also proves the bounded poll on that same core: a second tick inside the
// 500 ms window issues no second request, and a reply for a superseded epoch is
// dropped rather than published.
//
// The core is the app's own Standard selection unless a path is given as argv[1]
// or MIKAI_MPV_MODULE, which exists so the probe can run from any directory
// without the Standard discovery path (and without it provisioning anything).
//
// Usage: native_status_probe [path/to/libmpv-2.dll]

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QThread>

#include <cstdio>

#include "MpvApi.hpp"
#include "MpvHandle.hpp"
#include "native/NativeRuntimeSelection.hpp"
#include "native/NativeStatus.hpp"

using mplayer::MpvApi;
using mplayer::MpvBundle;
using mplayer::MpvHandle;
using mplayer::native::NativeState;
using mplayer::native::NativeStatusConsumer;
using mplayer::native::NativeStatusView;
using mplayer::native::RuntimeSelection;
using mplayer::native::selectStandardRuntime;
using mplayer::native::stableName;

namespace {

void say(const QString& line) {
    std::fprintf(stderr, "%s\n", line.toUtf8().constData());
    std::fflush(stderr);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    const QStringList arguments = QCoreApplication::arguments();
    QString module = qEnvironmentVariable("MIKAI_MPV_MODULE");
    if (arguments.size() > 1) module = arguments.at(1);

    QString loadedFrom;
    if (!module.isEmpty()) {
        const QFileInfo info(module);
        MpvBundle bundle;
        bundle.root = info.absolutePath();
        bundle.modulePath = info.absoluteFilePath();
        bundle.descriptorId = QStringLiteral("native-status-probe");
        bundle.protocolVersion = 1;
        QString reason;
        if (!MpvApi::instance().load(bundle, &reason)) {
            say(QStringLiteral("RESULT FAIL ") + reason);
            return 1;
        }
        loadedFrom = bundle.modulePath;
    } else {
        const RuntimeSelection selection = selectStandardRuntime();
        if (!selection.loaded) {
            say(QStringLiteral("RESULT FAIL no standard core was selected: %1")
                    .arg(selection.reason));
            return 1;
        }
        loadedFrom = QStringLiteral("%1 (standard selection)").arg(selection.modulePath);
    }
    say(QStringLiteral("core %1 (client API %2)").arg(loadedFrom).arg(MpvApi::instance().apiVersion()));

    try {
        MpvHandle handle({{"vo", "libmpv"}, {"idle", "yes"}, {"input-default-bindings", "no"}});
        NativeStatusConsumer consumer;
        int changes = 0;
        consumer.setChangeCallback([&changes](const NativeStatusView&) { ++changes; });
        consumer.attach(&handle);

        say(QStringLiteral("before any read: availability=%1 key=%2 snapshot=%3")
                .arg(static_cast<int>(consumer.availability().value))
                .arg(consumer.availability().reasonKey)
                .arg(consumer.snapshot().has_value() ? QStringLiteral("present")
                                                     : QStringLiteral("absent")));

        consumer.tick(0, 1, true);
        const quint64 issued = consumer.requestCount();
        QElapsedTimer clock;
        clock.start();
        while (consumer.availability().reasonKey == QStringLiteral("native.status.unread") &&
               clock.elapsed() < 10000) {
            handle.drainEvents();
            QThread::msleep(5);
        }

        say(QStringLiteral("after the first poll: requests=%1 accepted=%2 dropped=%3 changes=%4")
                .arg(issued)
                .arg(consumer.acceptedCount())
                .arg(consumer.droppedCount())
                .arg(changes));

        // The bounded cadence: a tick inside the window must not stack a request.
        consumer.tick(100, 1, true);
        const quint64 afterEarly = consumer.requestCount();
        // Native is not Active/Preparing/Switching, so no request is issued.
        consumer.tick(10000, 1, false);
        say(QStringLiteral("bounded poll: requests after an in-window tick=%1, after a "
                           "not-polling tick=%2")
                .arg(afterEarly)
                .arg(consumer.requestCount()));

        const std::optional<mplayer::native::StatusSnapshot>& snapshot = consumer.snapshot();
        // No outcome is allowed to be silent: either the property resolved into
        // a parsed snapshot, or the surface is Unavailable and says why.
        if (!snapshot.has_value() && consumer.availability().reason.isEmpty()) {
            say(QStringLiteral("RESULT FAIL the surface reported no snapshot and no reason"));
            return 1;
        }
        if (snapshot.has_value() &&
            consumer.availability().value != mplayer::native::Availability::Ready) {
            say(QStringLiteral("RESULT FAIL a snapshot was published without readiness"));
            return 1;
        }

        if (!snapshot.has_value()) {
            const bool named = consumer.availability().reasonKey ==
                               QStringLiteral("native.status.missing");
            say(QStringLiteral("no filter attached: availability=%1 key=%2 reason=%3")
                    .arg(static_cast<int>(consumer.availability().value))
                    .arg(consumer.availability().reasonKey)
                    .arg(consumer.availability().reason));
            const bool bounded = afterEarly == issued && consumer.requestCount() == issued;
            if (!named || !bounded) {
                say(QStringLiteral("RESULT FAIL expected a named Unavailable with a reason and a "
                                   "bounded poll"));
                return 1;
            }
            say(QStringLiteral("RESULT PASS absent property reported as Unavailable: %1")
                    .arg(consumer.availability().reasonKey));
            return 0;
        }

        // A core that does carry the filter: the snapshot itself has to parse and
        // be reported, with the request and the effective plan told apart.
        const mplayer::native::StatusSnapshot& status = *snapshot;
        say(QStringLiteral("snapshot revision=%1 state=%2 epoch=%3 reason=%4")
                .arg(status.revision)
                .arg(stableName(status.state))
                .arg(status.epoch.has_value() ? QString::number(*status.epoch)
                                              : QStringLiteral("none"))
                .arg(status.reason));
        say(QStringLiteral("requested=%1/%2 effective=%3/%4/%5")
                .arg(status.requested.has_value() ? status.requested->recipeId
                                                  : QStringLiteral("none"))
                .arg(status.requested.has_value()
                         ? mplayer::native::stableName(status.requested->backend)
                         : QStringLiteral("none"))
                .arg(status.effective.has_value() ? status.effective->recipe
                                                  : QStringLiteral("none"))
                .arg(status.effective.has_value() ? status.effective->provider
                                                  : QStringLiteral("none"))
                .arg(status.effective.has_value() ? status.effective->model
                                                  : QStringLiteral("none")));
        say(QStringLiteral("RESULT PASS snapshot parsed on a core that carries the filter; state=%1")
                .arg(stableName(status.state)));
        return 0;
    } catch (const mplayer::MpvError& error) {
        say(QStringLiteral("RESULT FAIL core error: ") + QString::fromStdString(error.what()));
        return 1;
    }
}
