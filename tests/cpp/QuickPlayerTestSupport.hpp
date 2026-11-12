// Minimal shared setup for Quick player tests that need a real libmpv core.
// The runtime is selected only by the isolated launcher's explicit module path;
// no system search or fake core can make a transport assertion pass.
#pragma once

#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QString>

#include "player/core/MpvApi.hpp"
#include "player/core/VideoSession.hpp"

namespace mikai_test::quick {

inline bool loadExplicitMpvRuntime(QString* error = nullptr) {
    auto& api = mplayer::MpvApi::instance();
    if (api.loaded()) return true;

    const QString modulePath = qEnvironmentVariable("MIKAI_MPV_MODULE");
    if (modulePath.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("MIKAI_MPV_MODULE must name the real libmpv module");
        }
        return false;
    }

    const QFileInfo moduleInfo(modulePath);
    mplayer::MpvBundle bundle;
    bundle.root = moduleInfo.absolutePath();
    bundle.modulePath = moduleInfo.absoluteFilePath();
    bundle.descriptorId = QStringLiteral("quick-player-test-runtime");
    bundle.protocolVersion = 1;
    return api.load(bundle, error);
}

inline QString benchmarkVideoPath() {
    return QDir::cleanPath(QStringLiteral(MIKAI_SOURCE_DIR "/resources/benchmarks/1920x1080.mp4"));
}

inline bool openBenchmarkVideo(mplayer::VideoSession& session, QString* error = nullptr) {
    const QString path = benchmarkVideoPath();
    if (!QFileInfo::exists(path)) {
        if (error != nullptr) {
            *error = QStringLiteral("Real benchmark video is missing: %1").arg(path);
        }
        return false;
    }
    if (!loadExplicitMpvRuntime(error)) return false;
    if (session.open(path)) return true;
    if (error != nullptr && error->isEmpty()) {
        *error = QStringLiteral("VideoSession could not create a real mpv core");
    }
    return false;
}

}  // namespace mikai_test::quick
