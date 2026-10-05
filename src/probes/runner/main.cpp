// The manifest-driven probe runner.
//
// One console executable for the whole declarative suite: it loads every
// *.json manifest in a probe directory (or one manifest file), runs each
// through mikai::probes::ProbeRunner, and prints a per-manifest verdict plus
// a final summary. A manifest that cannot even be parsed is reported by file
// and counted as a failure, never skipped silently.
//
// Usage: probe_runner [probe-dir | manifest.json] [--filter <substring>] [--list]
//        [--isolate [--isolate-timeout-ms <ms>]]
//
// These are the only options: --filter selects the manifests whose manifest
// `name` contains the substring, --list prints name/category/description
// without running anything, --isolate runs each selected manifest in a fresh
// child process (the runner re-invoked on itself), --isolate-timeout-ms caps
// one child in milliseconds (default 120000).

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
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
    std::fprintf(stderr, " [--filter <substring>] [--list] [--isolate [--isolate-timeout-ms <ms>]]\n");
    std::fprintf(stderr,
                 "language: MIKAI_PROBE_LANGUAGE is the runner default; a manifest's own "
                 "\"language\" overrides it for that manifest only\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    QString positional;
    QString filter;
    bool listOnly = false;
    bool isolate = false;
    int isolateTimeoutMs = 120000;
    // Flags the parent was given that an isolated child must repeat. --isolate
    // and --isolate-timeout-ms are never recorded here: a child always runs
    // its single manifest in-process and must not recurse.
    QStringList forwardArgs;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--filter")) {
            ++i;
            if (i >= argc) {
                return usage(QStringLiteral("--filter needs a substring"));
            }
            filter = QString::fromLocal8Bit(argv[i]);
            forwardArgs << QStringLiteral("--filter") << filter;
        } else if (arg == QStringLiteral("--list")) {
            listOnly = true;
            forwardArgs << QStringLiteral("--list");
        } else if (arg == QStringLiteral("--isolate")) {
            isolate = true;
        } else if (arg == QStringLiteral("--isolate-timeout-ms")) {
            ++i;
            if (i >= argc) {
                return usage(QStringLiteral("--isolate-timeout-ms needs milliseconds"));
            }
            bool ok = false;
            const int ms = QString::fromLocal8Bit(argv[i]).toInt(&ok);
            if (!ok || ms <= 0) {
                return usage(QStringLiteral("--isolate-timeout-ms needs a positive integer"));
            }
            isolateTimeoutMs = ms;
        } else if (arg.startsWith(QStringLiteral("--isolate-timeout-ms="))) {
            bool ok = false;
            const int ms = arg.mid(arg.indexOf(QLatin1Char('=')) + 1).toInt(&ok);
            if (!ok || ms <= 0) {
                return usage(QStringLiteral("--isolate-timeout-ms needs a positive integer"));
            }
            isolateTimeoutMs = ms;
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

    // --isolate fans the selected manifests out to fresh child processes: the
    // runner re-invoked on itself as `<self> <manifest> [--filter ..] [--list]`
    // with the parent's environment inherited (QProcess passes it through
    // unless setEnvironment is called, which this never does). The child's
    // output is replayed verbatim under a `=== <name> ===` header, so
    // --isolate output is a superset of the default output, never a different
    // format; the parent's own summary aggregates the children's
    // `probe_runner: …` lines. A child that crashes (or never starts) reports
    // `[FAIL] <name>: child died (exit <code>)`, one that exceeds
    // --isolate-timeout-ms reports `[FAIL] <name>: child timed out
    // (exit <code>)`, and the run continues with the next manifest.
    if (isolate) {
        const QString self = QCoreApplication::applicationFilePath();
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
                std::printf("=== %s ===\n", qPrintable(manifest->name));
                std::fflush(stdout);
                QStringList childArgs;
                childArgs << file << forwardArgs;
                QProcess child;
                child.setProcessChannelMode(QProcess::MergedChannels);
                child.start(self, childArgs);
                if (!child.waitForStarted(isolateTimeoutMs)) {
                    std::printf("[FAIL] %s: child died (exit -1)\n",
                                qPrintable(manifest->name));
                    ++refused;
                    continue;
                }
                const bool finished = child.waitForFinished(isolateTimeoutMs);
                const QByteArray out = child.readAllStandardOutput();
                if (!out.isEmpty()) {
                    std::fwrite(out.constData(), 1, static_cast<size_t>(out.size()), stdout);
                }
                if (!finished) {
                    child.kill();
                    child.waitForFinished(5000);
                    std::printf("[FAIL] %s: child timed out (exit %d)\n",
                                qPrintable(manifest->name), child.exitCode());
                    ++refused;
                    continue;
                }
                if (child.exitStatus() != QProcess::NormalExit) {
                    std::printf("[FAIL] %s: child died (exit %d)\n",
                                qPrintable(manifest->name), child.exitCode());
                    ++refused;
                    continue;
                }
                if (child.exitCode() != 0) {
                    ++refused;
                }
            }
            return refused == 0 ? 0 : 1;
        }
        int manifests = 0;
        int assertions = 0;
        int failed = 0;
        for (const QString& file : manifestFiles) {
            QString reason;
            const std::optional<mikai::probes::ProbeManifest> manifest =
                mikai::probes::loadProbeManifest(file, &reason);
            if (!manifest.has_value()) {
                std::printf("[FAIL] %s: %s\n", qPrintable(file), qPrintable(reason));
                ++manifests;
                ++failed;
                continue;
            }
            if (!filter.isEmpty() && !manifest->name.contains(filter)) {
                continue;
            }
            ++manifests;
            std::printf("=== %s ===\n", qPrintable(manifest->name));
            std::fflush(stdout);
            QStringList childArgs;
            childArgs << file << forwardArgs;
            QProcess child;
            child.setProcessChannelMode(QProcess::MergedChannels);
            child.start(self, childArgs);
            if (!child.waitForStarted(isolateTimeoutMs)) {
                std::printf("[FAIL] %s: child died (exit -1)\n",
                            qPrintable(manifest->name));
                ++failed;
                continue;
            }
            const bool finished = child.waitForFinished(isolateTimeoutMs);
            const QByteArray out = child.readAllStandardOutput();
            if (!out.isEmpty()) {
                std::fwrite(out.constData(), 1, static_cast<size_t>(out.size()), stdout);
                std::fflush(stdout);
            }
            if (!finished) {
                child.kill();
                child.waitForFinished(5000);
                std::printf("[FAIL] %s: child timed out (exit %d)\n",
                            qPrintable(manifest->name), child.exitCode());
                ++failed;
                continue;
            }
            if (child.exitStatus() != QProcess::NormalExit) {
                std::printf("[FAIL] %s: child died (exit %d)\n",
                            qPrintable(manifest->name), child.exitCode());
                ++failed;
                continue;
            }
            int childAssertions = 0;
            int childFailed = 0;
            bool haveSummary = false;
            for (const QByteArray& line : out.split('\n')) {
                int dummy = 0;
                int a = 0;
                int f = 0;
                if (std::sscanf(line.constData(),
                                "probe_runner: %d manifests, %d assertions, %d failed", &dummy, &a,
                                &f) == 3) {
                    childAssertions = a;
                    childFailed = f;
                    haveSummary = true;
                }
            }
            assertions += childAssertions;
            if (!haveSummary) {
                if (child.exitCode() != 0) {
                    std::printf("[FAIL] %s: child died (exit %d)\n",
                                qPrintable(manifest->name), child.exitCode());
                    ++failed;
                }
                continue;
            }
            if (childFailed > 0 || child.exitCode() != 0) {
                ++failed;
            }
        }
        std::printf("probe_runner: %d manifests, %d assertions, %d failed\n", manifests, assertions,
                    failed);
        return failed == 0 ? 0 : 1;
    }

    if (listOnly) {
        // MIKAI_PROBE_LANGUAGE is the runner default, installed once at startup
        // by initializeUi. A manifest's own "language" overrides it for that
        // manifest only, and the runner restores the default after that
        // manifest tears down, so each line below says which of the two applies.
        const QString runnerLanguage =
            qEnvironmentVariable("MIKAI_PROBE_LANGUAGE", QStringLiteral("en"));
        std::printf("language: MIKAI_PROBE_LANGUAGE=%s is the runner default; a manifest's "
                    "own \"language\" overrides it for that manifest only\n",
                    qPrintable(runnerLanguage));
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
            const bool ownLanguage = !manifest->language.isEmpty();
            const QString shown = ownLanguage ? manifest->language : runnerLanguage;
            const QString source = ownLanguage ? QStringLiteral("manifest language")
                                                : QStringLiteral("runner default");
            std::printf("%s [%s] (language %s, %s) - %s\n", qPrintable(manifest->name),
                        qPrintable(manifest->category), qPrintable(shown), qPrintable(source),
                        qPrintable(manifest->description));
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
