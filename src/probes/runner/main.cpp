// The manifest-driven probe runner.
//
// One console executable for the whole declarative suite: it loads every
// *.json manifest in a probe directory (or one manifest file), runs each
// through mikai::probes::ProbeRunner, and prints a per-manifest verdict plus
// a final summary. A manifest that cannot even be parsed is reported by file
// and counted as a failure, never skipped silently.
//
// Usage: probe_runner [probe-dir | manifest.json] [--filter <substring>] [--list]
//
// These are the only options: --filter selects the manifests whose manifest
// `name` contains the substring, --list prints name/category/description
// without running anything.

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>
#include <optional>

#include "ProbeManifest.hpp"
#include "ProbeRunner.hpp"
#include "ProbeRuntime.hpp"
#include "ProbeUi.hpp"

namespace {

int usage(const QString& problem) {
    if (!problem.isEmpty()) {
        std::fprintf(stderr, "probe_runner: %s\n", qPrintable(problem));
    }
    std::fprintf(stderr, "usage: probe_runner [probe-dir | manifest.json]");
    std::fprintf(stderr, " [--filter <substring>] [--list]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    QString positional;
    QString filter;
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--filter")) {
            ++i;
            if (i >= argc) {
                return usage(QStringLiteral("--filter needs a substring"));
            }
            filter = QString::fromLocal8Bit(argv[i]);
        } else if (arg == QStringLiteral("--list")) {
            listOnly = true;
        } else if (arg.startsWith(QStringLiteral("--"))) {
            return usage(QStringLiteral("unknown option: ") + arg);
        } else if (!positional.isNull()) {
            return usage(QStringLiteral("at most one probe directory or manifest path"));
        } else {
            positional = arg;
        }
    }

    // A single manifest file is run (or listed) on its own; anything else
    // names the directory to enumerate (default `probes/`). A dangling `.json`
    // path still counts as a manifest so its absence is reported as a failure,
    // not silently ignored.
    bool singleFile = false;
    QString directory = QStringLiteral("probes/");
    if (!positional.isNull()) {
        const QFileInfo info(positional);
        if (info.isFile() || positional.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
            singleFile = true;
        } else {
            directory = positional;
        }
    }

    QStringList manifestFiles;
    if (singleFile) {
        manifestFiles << positional;
    } else {
        const QDir root(directory);
        if (!root.exists()) {
            std::fprintf(stderr, "probe_runner: probe directory not found: %s\n",
                         qPrintable(directory));
            return 2;
        }
        // Manifests are grouped by category, so discovery is recursive; a
        // probe is found the same way whether it sits at the root or one
        // category deep.
        for (const QString& name :
             root.entryList(QStringList() << QStringLiteral("*.json"), QDir::Files, QDir::Name)) {
            manifestFiles << root.filePath(name);
        }
        for (const QString& category :
             root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QDir sub(root.filePath(category));
            for (const QString& name : sub.entryList(QStringList() << QStringLiteral("*.json"),
                                                    QDir::Files, QDir::Name)) {
                manifestFiles << sub.filePath(name);
            }
        }
        if (manifestFiles.isEmpty()) {
            std::fprintf(stderr, "probe_runner: warning: no manifests in %s\n",
                         qPrintable(directory));
        }
    }

    QApplication app(argc, argv);
    if (!mikai::probes::initializeUi(app)) {
        return 2;
    }
    app.setQuitOnLastWindowClosed(false);

    // The user's own player settings stay theirs: everything the runner (and
    // every surface it builds) reads or writes lands in a temporary directory
    // that dies with the process.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "probe_runner: FAIL cannot create an isolated settings directory\n");
        return 2;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiProbeRunner"));
    QCoreApplication::setApplicationName(QStringLiteral("ProbeRunner"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());

    // The run proves something about the runtime it actually loaded; without
    // one there is nothing to run on. selectRuntime already prints which
    // module, descriptor and kind were selected (or the refusal and why).
    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }

    if (listOnly) {
        int refused = 0;
        for (const QString& file : manifestFiles) {
            QString reason;
            const std::optional<mikai::probes::ProbeManifest> manifest =
                mikai::probes::loadProbeManifest(file, &reason);
            if (!manifest.has_value()) {
                std::printf("[FAIL] %s: %s\n", qPrintable(file), qPrintable(reason));
                ++refused;
                continue;
            }
            if (!filter.isEmpty() && !manifest->name.contains(filter)) {
                continue;
            }
            std::printf("%s [%s] - %s\n", qPrintable(manifest->name),
                        qPrintable(manifest->category), qPrintable(manifest->description));
        }
        return refused == 0 ? 0 : 1;
    }

    mikai::probes::ProbeRunner runner;
    int manifests = 0;
    int assertions = 0;
    int failed = 0;
    for (const QString& file : manifestFiles) {
        QString reason;
        const std::optional<mikai::probes::ProbeManifest> manifest =
            mikai::probes::loadProbeManifest(file, &reason);
        if (!manifest.has_value()) {
            // No manifest name is known, so the file path is the name: this is
            // still a verdict line and still counts as a failure.
            std::printf("[FAIL] %s: %s\n", qPrintable(file), qPrintable(reason));
            ++manifests;
            ++failed;
            continue;
        }
        if (!filter.isEmpty() && !manifest->name.contains(filter)) {
            continue;
        }
        ++manifests;
        const mikai::probes::ProbeRunResult result = runner.run(*manifest);
        assertions += result.assertions;
        if (result.ok) {
            std::printf("[PASS] %s\n", qPrintable(manifest->name));
        } else {
            ++failed;
            if (result.detail.isEmpty()) {
                std::printf("[FAIL] %s (%d of %d assertions failed)\n",
                            qPrintable(manifest->name), result.failed, result.assertions);
            } else {
                std::printf("[FAIL] %s: %s\n", qPrintable(manifest->name),
                            qPrintable(result.detail));
            }
        }
    }
    std::printf("probe_runner: %d manifests, %d assertions, %d failed\n", manifests, assertions,
                failed);
    return failed == 0 ? 0 : 1;
}
