// Proves the loader and the asynchronous transport against a real core module.
//
// The probe never searches for a runtime: the module path comes from argv or
// from MIKAI_MPV_MODULE, and that descriptor is what gets selected. A refusal
// prints the loader's own reason, because the reason is what a user is shown.
//
// The pinned module is also asked to swap to a different descriptor, which it
// must refuse by name while the first module stays selected.
//
// Usage: mpv_loader_probe [path/to/libmpv-2.dll] [path/to/another-core.dll]

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QThread>

#include <cstdio>
#include <utility>
#include <vector>

#include "MpvApi.hpp"
#include "MpvHandle.hpp"

using mplayer::AsyncReply;
using mplayer::MpvApi;
using mplayer::MpvBundle;
using mplayer::MpvHandle;
using mplayer::NodeValue;

namespace {

void say(const QString& line) {
    std::fprintf(stderr, "%s\n", line.toUtf8().constData());
    std::fflush(stderr);
}

MpvBundle descriptorFor(const QString& modulePath) {
    const QFileInfo info(modulePath);
    MpvBundle bundle;
    bundle.root = info.absolutePath();
    bundle.modulePath = info.absoluteFilePath();
    bundle.descriptorId = QStringLiteral("mpv-loader-probe");
    bundle.protocolVersion = 1;
    return bundle;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    const QStringList arguments = QCoreApplication::arguments();
    QString module = qEnvironmentVariable("MIKAI_MPV_MODULE");
    if (arguments.size() > 1) module = arguments.at(1);
    if (module.isEmpty()) {
        say(QStringLiteral("RESULT FAIL no module path: pass one as argv[1] or set MIKAI_MPV_MODULE"));
        return 2;
    }

    const MpvBundle bundle = descriptorFor(module);
    QString reason;
    if (!MpvApi::instance().load(bundle, &reason)) {
        say(QStringLiteral("RESULT FAIL ") + reason);
        return 1;
    }
    say(QStringLiteral("loaded %1 (client API %2, header API %3)")
            .arg(bundle.modulePath)
            .arg(MpvApi::instance().apiVersion())
            .arg(MpvApi::headerApiVersion()));
    // Same descriptor again: a no-op that succeeds, not a reload.
    if (!MpvApi::instance().load(bundle, &reason)) {
        say(QStringLiteral("RESULT FAIL re-selecting the same descriptor: ") + reason);
        return 1;
    }

    // A different module must be refused: swapping the code under a live handle
    // is the failure this pin exists to prevent. The second bundle may be named
    // as argv[2]; without one, the same module is re-presented under a different
    // descriptor, which is still a different selection.
    const QString otherModule =
        arguments.size() > 2
            ? arguments.at(2)
            : QDir(QCoreApplication::applicationDirPath())
                  .filePath(QStringLiteral("not-the-pinned-core.dll"));
    MpvBundle other = descriptorFor(otherModule);
    other.descriptorId = QStringLiteral("mpv-loader-probe-other");
    bool refusedByPolicy = false;
    if (!MpvApi::instance().load(other, &reason)) {
        refusedByPolicy = reason.contains(QStringLiteral("Refusing to replace"));
        if (!refusedByPolicy) say(QStringLiteral("swap refusal reason: ") + reason);
    } else {
        say(QStringLiteral("a swap of the live module was accepted, which must never happen"));
    }
    if (!MpvApi::instance().loaded() ||
        MpvApi::instance().bundle().modulePath != bundle.modulePath) {
        say(QStringLiteral("RESULT FAIL the refused swap changed the selected module"));
        return 1;
    }
    if (!refusedByPolicy) {
        say(QStringLiteral("RESULT FAIL a live module swap was not refused by name"));
        return 1;
    }
    say(QStringLiteral("a live module swap is refused by name and the pinned module survives"));

    try {
        MpvHandle handle({{"vo", "libmpv"}, {"idle", "yes"}, {"input-default-bindings", "no"}});

        int versionReplies = 0;
        int commandReplies = 0;
        int commandError = 0;
        int unknownReplies = 0;
        int nodeReplies = 0;
        int nodeError = 0;
        const auto versionId = handle.getPropertyAsync(
            "mpv-version", MPV_FORMAT_STRING,
            [&versionReplies](const AsyncReply& reply) {
                ++versionReplies;
                say(QStringLiteral("async property mpv-version error=%1 format=%2 value=%3")
                        .arg(reply.error)
                        .arg(static_cast<int>(reply.format))
                        .arg(QString::fromStdString(reply.text)));
            });
        const auto commandId = handle.commandAsync({"set", "pause", "yes"}, [&commandReplies, &commandError](
                                                                                 const AsyncReply& reply) {
            ++commandReplies;
            commandError = reply.error;
        });
        const auto unknownId = handle.getPropertyAsync(
            "no-such-property", MPV_FORMAT_DOUBLE,
            [&unknownReplies](const AsyncReply& reply) {
                ++unknownReplies;
                say(QStringLiteral("async unknown property error=%1").arg(reply.error));
            });
        // A command built from a node: the arena that builds it has to keep its
        // pointers valid for the duration of the call, and the reply arrives
        // correlated like any other.
        NodeValue nodeArgs;
        nodeArgs.format = MPV_FORMAT_NODE_ARRAY;
        for (const char* word : {"set", "pause", "yes"}) {
            NodeValue item;
            item.format = MPV_FORMAT_STRING;
            item.text = word;
            nodeArgs.list.push_back(std::move(item));
        }
        const auto nodeId = handle.commandNodeAsync(nodeArgs, [&nodeReplies, &nodeError](
                                                                   const AsyncReply& reply) {
            ++nodeReplies;
            nodeError = reply.error;
        });
        if (!versionId.valid() || !commandId.valid() || !unknownId.valid() || !nodeId.valid()) {
            say(QStringLiteral("RESULT FAIL an asynchronous request was refused at issue time"));
            return 1;
        }

        QElapsedTimer clock;
        clock.start();
        while ((versionReplies == 0 || commandReplies == 0 || unknownReplies == 0 || nodeReplies == 0) &&
               clock.elapsed() < 10000) {
            handle.drainEvents();
            QThread::msleep(5);
        }
        if (versionReplies == 0 || commandReplies == 0 || unknownReplies == 0 || nodeReplies == 0) {
            say(QStringLiteral("RESULT FAIL timed out waiting for replies (property=%1 command=%2 "
                               "unknown=%3 node=%4)")
                    .arg(versionReplies).arg(commandReplies).arg(unknownReplies).arg(nodeReplies));
            return 1;
        }
        // Each request is answered once, by its own reply.
        const bool correlated = versionReplies == 1 && commandReplies == 1 && unknownReplies == 1 &&
                                nodeReplies == 1 && commandError == 0 && nodeError == 0;
        say(QStringLiteral("pending after drain: %1").arg(handle.pendingAsyncCount()));
        // The event sink must observe the terminal end-file event: a failed
        // open surfaces only there, and the player turns exactly that event
        // into the retry chip. Loading a URL the fixture refuses with 404
        // proves the failure is delivered rather than swallowed.
        if (!correlated) {
            say(QStringLiteral("RESULT FAIL loader, transport, correlation, swap refusal"));
            return 1;
        }
        int endFileEvents = 0;
        int endFileError = 0;
        handle.setEventSink([&endFileEvents, &endFileError](mpv_event* event) {
            if (event != nullptr && event->event_id == MPV_EVENT_END_FILE) {
                ++endFileEvents;
                const auto* ended = static_cast<const mpv_event_end_file*>(event->data);
                if (ended != nullptr && ended->reason == MPV_END_FILE_REASON_ERROR) {
                    ++endFileError;
                }
            }
        });
        try {
            handle.loadFile("http://127.0.0.1:9/retry.mp4");
        } catch (const mplayer::MpvError& error) {
            say(QStringLiteral("RESULT FAIL issuing the refused load: %1")
                    .arg(QString::fromUtf8(error.what())));
            return 1;
        }
        QElapsedTimer endClock;
        endClock.start();
        while (endFileEvents == 0 && endClock.elapsed() < 15000) {
            handle.drainEvents();
            QThread::msleep(5);
        }
        say(QStringLiteral("end-file events=%1 error=%2").arg(endFileEvents).arg(endFileError));
        const bool endFileDelivered = endFileEvents >= 1 && endFileError >= 1;
        say(QStringLiteral("RESULT %1 loader, transport, correlation, swap refusal, end-file delivery")
                .arg(endFileDelivered ? QStringLiteral("PASS") : QStringLiteral("FAIL")));
        return endFileDelivered ? 0 : 1;
    } catch (const mplayer::MpvError& error) {
        say(QStringLiteral("RESULT FAIL ") + QString::fromStdString(error.what()));
        return 1;
    }
}