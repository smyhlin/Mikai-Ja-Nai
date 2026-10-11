// Proves the crash flow end to end: install the same logging the app installs,
// then fault on purpose and check that a report lands beside the exe.
//
// The unhandled-exception filter only does anything if it actually runs, and
// "it is wired up" is not the same as "it wrote a file". Run it with --crash.
//
// This probe is the injection point, so it faults THIS process - the probe
// binary - and never an installed app. Its log and its reports land beside the
// probe executable, which is a build-directory binary: `installCrashLog()`
// resolves the folder from the loader, so it names crash_probe.exe's own folder
// and cannot write next to an installed `mikai.exe`. The probe opens nothing
// else: it does not touch the native store, the component store, a settings
// file or any model, so a fault run cannot damage anything a real installation
// depends on.
//
// The redaction boundary is exercised by the two fixture lines below: a signed
// media URL and an Authorization header. Each is planted through the same
// writeLogLine append the Qt message handler uses, and the log is read straight
// back to prove both were redacted before they reached disk - which is also
// what makes them absent from the report written after the fault.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <cstdio>

#include "CrashLog.hpp"

namespace {

/// The folder the probe's own log and reports land in, and where a caller can
/// read them back. `installCrashLog()` resolves it from the loader, so this is
/// the crash_probe binary's own folder by construction - the same call the app
/// makes - and no relocation or override is applied. That is the isolation: the
/// production handler is driven exactly as production drives it.
QString probeOutputDir() {
    return QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("crash-probe-output"));
}

}  // namespace

int main(int argc, char** argv) {
    // applicationDirPath() is only meaningful once an application object
    // exists; without one it answers an empty string, which would make the
    // isolation check below refuse every run.
    QCoreApplication application(argc, argv);
    const QStringList args = argc > 1 ? QStringList{argv[1]} : QStringList();
    const bool crash = args.contains(QStringLiteral("--crash"));

    mikai::app::installCrashLog();

    const QString log = mikai::app::logPath();
    // Isolation is checked, not assumed. The session log must resolve beside
    // THIS probe's own executable; anything else means the handler picked up a
    // folder belonging to some other binary and the run is not safe to fault.
    const QString ownFolder = QCoreApplication::applicationDirPath();
    const bool isolated = QFileInfo(log).absolutePath().compare(ownFolder, Qt::CaseInsensitive) == 0;
    if (crash && !isolated) {
        std::fprintf(stderr,
                     "crash probe: refusing to fault: log %s is not beside %s\n",
                     qPrintable(log), qPrintable(ownFolder));
        return 2;
    }
    std::fprintf(stderr, "crash probe: log beside the probe exe=%d\n", isolated ? 1 : 0);

    mikai::app::writeLogLine(QStringLiteral("crash probe: log at %1").arg(log));

    // The two fixtures the KB-28 boundary has to survive. Both go through
    // writeLogLine, which is the same append the Qt message handler uses, so a
    // token planted here is redacted exactly as one from a worker qWarning.
    // Neither may appear in the session log or in the report this probe writes
    // after it faults.
    const QString signedLine =
        QStringLiteral("probe fixture: opening https://media.example.invalid/"
                       "hls/7428/signed/ab12/index.m3u8?token=SECRET123&sig=deadbeef");
    const QString bearerLine =
        QStringLiteral("probe fixture: Authorization: Bearer mk_live_9f3c2a7d4b6e");
    mikai::app::writeLogLine(signedLine);
    mikai::app::writeLogLine(bearerLine);

    // The log itself is read back with the same file API a report carries, so
    // the boundary is proved here rather than only in the fault path.
    {
        QFile file(log);
        if (file.open(QIODevice::ReadOnly)) {
            const QString stored = QString::fromUtf8(file.readAll());
            bool redacted = true;
            if (stored.contains(QStringLiteral("SECRET123")) ||
                stored.contains(QStringLiteral("deadbeef")) ||
                stored.contains(QStringLiteral("mk_live_9f3c2a7d4b6e")) ||
                stored.contains(QStringLiteral("signed/ab12/index.m3u8"))) {
                redacted = false;
            }
            std::fprintf(stderr, "crash probe: logged token redacted=%d\n", redacted ? 1 : 0);
            if (!redacted) {
                std::fprintf(stderr, "crash probe: a planted token reached the log\n");
                return 1;
            }
        }
    }

    std::fprintf(stderr, "crash probe: output folder=%s\n", qPrintable(probeOutputDir()));
    std::fprintf(stderr, "crash probe: log=%s\n", qPrintable(log));

    if (!crash) {
        std::fprintf(stderr, "crash probe: not crashing, pass --crash to fault\n");
        return 0;
    }

    mikai::app::writeLogLine(QStringLiteral("about to fault on purpose"));
    std::fflush(stderr);

    // An access violation, which is the shape of the crash this exists for.
    volatile int* nowhere = nullptr;
    *nowhere = 42;

    std::fprintf(stderr, "crash probe: SURVIVED the fault, the filter did not run\n");
    return 2;
}
