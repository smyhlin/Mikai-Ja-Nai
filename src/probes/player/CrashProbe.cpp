// Proves the crash flow end to end: install the same logging the app installs,
// then fault on purpose and check that a report lands in the exe's folder.
//
// The unhandled-exception filter only does anything if it actually runs, and
// "it is wired up" is not the same as "it wrote a file". Run it with --crash.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>

#include <cstdio>

#include "CrashLog.hpp"

int main(int argc, char** argv) {
    const QStringList args = argc > 1 ? QStringList{argv[1]} : QStringList();
    const bool crash = args.contains(QStringLiteral("--crash"));

    mikai::app::installCrashLog();
    mikai::app::writeLogLine(QStringLiteral("crash probe: log at %1")
                                .arg(mikai::app::logPath()));
    std::fprintf(stderr, "crash probe: log=%s\n", qPrintable(mikai::app::logPath()));

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
