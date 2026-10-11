// The disposable child of the real test launcher (KB-26).
//
// Purpose-built for tests/cmake/LauncherContracts.cmake, which runs the
// production `mikai_test_launcher` binary against this one and asserts on the
// launcher's exit code, on the OS-visible process-tree state and on the
// launcher-owned scratch directory. This is a *fixture*, not a test: it asserts
// nothing itself and fakes no player, network or module behaviour. It only
// provides the process shapes the launcher has to survive.
//
//   --hang
//       Spawn an actual descendant process (not a thread) as the very first
//       statement of main(), before Qt's event loop ever runs, then park
//       forever. That ordering is the point: a job assignment performed while
//       the child is still suspended provably covers this process tree, while
//       one performed after start does not.
//   --exit-code <n>
//       Run the event loop and exit with exactly <n>, no descendant.
//   --print-id <file>
//       Write this process's identity (PID plus creation time) to <file>.
//   --descendant-id <file>
//       Where the spawned descendant writes its own identity, and the handshake
//       that orders the two files: the child records its identity only after
//       the descendant's is on disk, so a reader that sees the child's file is
//       guaranteed the descendant already started.
//   --descendant-ms <n>
//       How long the descendant parks before exiting by itself. Long: a
//       launcher that fails to reap the tree must leave a process that can
//       still be found, not one that quietly vanished.
//
// QtCore only: no GUI, no mpv, no app or backend library, no QTest, no AUTOMOC.
#include <QtCore/QCommandLineOption>
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QThread>
#include <QtCore/QTimer>

#include <cstdio>

namespace {

// A park long enough that only an outside actor can end it. The launcher owns
// the deadline; nothing in this fixture may stop the hang on its own.
constexpr int kForeverMs = 24 * 3600 * 1000;

// The descendant's self-exit horizon: far beyond the contract scenario, so an
// un-reaped descendant is still there to be found.
constexpr int kDefaultDescendantMs = 10 * 60 * 1000;

// How long the child waits for the descendant's identity file before giving up
// and recording its own. Bounded, so a broken handshake cannot outlive the
// launcher's deadline and turn a tree-kill failure into a hang.
constexpr int kHandshakeWaitMs = 5000;
constexpr int kHandshakePollMs = 10;

void writeIdentityFile(const QString& path) {
    // Both halves of a Win32 process's identity: the PID and the creation time.
    // The contract script reads the PID here, then asks the OS for the process
    // that holds it and compares the start time it reports. A recycled PID
    // cannot survive that comparison, which is why this is written out rather
    // than the process remembering its own number.
    QString text;
    text += QStringLiteral("pid=%1\n").arg(QCoreApplication::applicationPid());
    text += QStringLiteral("creation=%1\n").arg(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Unbuffered)) {
        file.write(text.toUtf8());
        file.flush();
        file.close();
    }
}

// Reads the value that follows `--name` in the raw argument list. Returns an
// empty string when the option is absent, so a caller can never mistake the
// first argument (the program path) for an option value.
QString optionValue(const QStringList& arguments, const QString& name) {
    const int index = arguments.indexOf(name);
    if (index < 0 || index + 1 >= arguments.size()) return QString();
    return arguments.at(index + 1);
}

// Detached on purpose: the descendant's fate belongs to the launcher's tree
// kill, not to this process's scope exit. A QProcess object destroyed while its
// child runs would terminate the descendant and hide a missing tree kill.
qint64 spawnDetachedDescendant(const QString& identityPath, int parkMs) {
    qint64 pid = 0;
    const bool started = QProcess::startDetached(
        QCoreApplication::applicationFilePath(),
        {QStringLiteral("--park-only"), QStringLiteral("--park-ms"), QString::number(parkMs),
         QStringLiteral("--print-id"), identityPath},
        QString(),
        &pid);
    if (!started || pid == 0) {
        std::fprintf(stderr, "[launcher_fixture] cannot start the detached descendant\n");
        std::fflush(stderr);
        return 0;
    }
    return pid;
}

// Waits (bounded) for the descendant's identity file: once it exists, the
// descendant is a real, running process with a real PID, so the child's own
// identity file is only written after the descendant is discoverable from the
// outside. Order here is the guarantee the contract script relies on.
void awaitDescendantIdentity(const QString& path) {
    int waited = 0;
    while (waited < kHandshakeWaitMs && !QFile::exists(path)) {
        QThread::msleep(kHandshakePollMs);
        waited += kHandshakePollMs;
    }
}

int hangWithDescendant(const QCoreApplication& application) {
    const QStringList arguments = application.arguments();
    QString descendantIdentity = optionValue(arguments, QStringLiteral("--descendant-id"));
    if (descendantIdentity.isEmpty()) {
        std::fprintf(stderr, "[launcher_fixture] --hang needs --descendant-id <file>\n");
        return 70;
    }
    QString identity = optionValue(arguments, QStringLiteral("--print-id"));
    if (identity.isEmpty()) {
        std::fprintf(stderr, "[launcher_fixture] --hang needs --print-id <file>\n");
        return 70;
    }
    const int descendantMs =
        !optionValue(arguments, QStringLiteral("--descendant-ms")).isEmpty()
            ? optionValue(arguments, QStringLiteral("--descendant-ms")).toInt()
            : kDefaultDescendantMs;

    const qint64 descendantPid = spawnDetachedDescendant(descendantIdentity, descendantMs);
    if (descendantPid == 0) return 71;
    // Order guarantees the contract script reads a descendant that really ran:
    // the descendant's identity file exists before the child's is written.
    awaitDescendantIdentity(descendantIdentity);
    writeIdentityFile(identity);

    // Park: the shape a hung test suite has. Only the outside stops this.
    QTimer::singleShot(kForeverMs, &application, &QCoreApplication::quit);
    return application.exec();
}

} // namespace

int main(int argc, char** argv) {
    // A plain QCoreApplication: this is not a Qt test and runs no QTest macros.
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mikai_launcher_child"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Test-launcher process-tree fixture"));
    const QCommandLineOption hangOption(QStringList() << QStringLiteral("hang") << QStringLiteral("park"),
                                         QStringLiteral("Spawn a real descendant and hang."));
    const QCommandLineOption exitCodeOption(QStringList() << QStringLiteral("exit-code"),
                                            QStringLiteral("Exit with this code, no descendant."),
                                            QStringLiteral("code"));
    const QCommandLineOption printIdOption(QStringList() << QStringLiteral("print-id"),
                                           QStringLiteral("Write this process's identity to this file."),
                                           QStringLiteral("file"));
    const QCommandLineOption descendantIdOption(QStringList() << QStringLiteral("descendant-id"),
                                                QStringLiteral("Where the descendant writes its identity."),
                                                QStringLiteral("file"));
    const QCommandLineOption descendantMsOption(
        QStringList() << QStringLiteral("descendant-ms"),
        QStringLiteral("How long the descendant parks before exiting by itself."),
        QStringLiteral("ms"));
    const QCommandLineOption parkOnlyOption(QStringList() << QStringLiteral("park-only"),
                                            QStringLiteral("Internal: park only, spawn nothing."));
    const QCommandLineOption parkMsOption(QStringList() << QStringLiteral("park-ms"),
                                          QStringLiteral("Internal: park duration in ms."),
                                          QStringLiteral("ms"));
    parser.addOptions({hangOption, exitCodeOption, printIdOption, descendantIdOption,
                        descendantMsOption, parkOnlyOption, parkMsOption});
    parser.process(application);

    // Internal descendant mode. This is the process a correct launcher reaps
    // along with its parent, and the one a direct-child-only kill leaves behind.
    if (parser.isSet(parkOnlyOption)) {
        bool ok = false;
        const int parkMs = parser.value(parkMsOption).toInt(&ok);
        if (!ok || parkMs <= 0) return 71;
        writeIdentityFile(parser.value(printIdOption));
        QTimer::singleShot(parkMs, &application, &QCoreApplication::quit);
        return application.exec();
    }

    if (parser.isSet(hangOption)) {
        return hangWithDescendant(application);
    }

    if (parser.isSet(exitCodeOption)) {
        bool ok = false;
        const int code = parser.value(exitCodeOption).toInt(&ok);
        if (!ok) {
            std::fprintf(stderr, "[launcher_fixture] --exit-code needs an integer\n");
            return 73;
        }
        // A healthy child still has a full application life before it leaves.
        QTimer::singleShot(0, &application, [&application, code]() {
            application.exit(code);
        });
        return application.exec();
    }

    std::fprintf(stderr, "[launcher_fixture] nothing to do: pass --hang or --exit-code\n");
    return 74;
}
