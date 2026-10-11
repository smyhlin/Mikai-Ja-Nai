// CTest owns this process; only its unique executable directory is disposable.
// Production nativeRoot() stays executable-local, with no test environment hook.
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTemporaryDir>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <tlhelp32.h>
#endif

#include <cstdio>
#include <cstdlib>

namespace {

// A hung suite must not outlive the CTest entry that owns it. The deadline is
// bounded by default and configurable per run: the previous waitForFinished(-1)
// relied on an external policy, which meant direct launcher use could park the
// caller forever with no scratch cleanup and no child.
constexpr qint64 kDefaultChildTimeoutMs = 10 * 60 * 1000;
constexpr qint64 kKillGraceMs = 10 * 1000;
// Short enough that a hung child is noticed without busy-waiting the CPU.
constexpr qint64 kPollSliceMs = 100;

constexpr int kExitTimeout = 3;

#  ifdef Q_OS_WIN
// A terminated job member reports this exit code. A child killed for overrunning
// the deadline did not exit with a verdict of its own.
constexpr unsigned long kTerminatedExitCode = 0xFFFFFFFFu;
#endif

// A non-positive or unparsable value is never silently clamped to the default:
// an explicit deadline nobody can read must not look honored.
bool parseTimeoutSpec(const QString& source, const QString& raw, qint64* out) {
    bool ok = false;
    const qint64 requested = raw.toLongLong(&ok);
    if (!ok || requested <= 0) {
        std::fprintf(stderr,
                     "[mikai_test_launcher] %s='%s' is not a positive number of milliseconds\n",
                     qPrintable(source), qPrintable(raw));
        return false;
    }
    *out = requested;
    return true;
}

// Precedence: --launcher-timeout-ms overrides MIKAI_TEST_TIMEOUT_MS, which
// overrides the built-in default. Returns -1 on an unreadable explicit value.
qint64 childTimeoutMs(const QString& explicitSpec) {
    qint64 timeout = kDefaultChildTimeoutMs;
    if (!explicitSpec.isEmpty()) {
        qint64 requested = 0;
        if (!parseTimeoutSpec(QStringLiteral("--launcher-timeout-ms"), explicitSpec, &requested)) {
            return -1;
        }
        return requested;
    }
    const QString raw = qEnvironmentVariable("MIKAI_TEST_TIMEOUT_MS");
    if (!raw.isEmpty()) {
        qint64 requested = 0;
        if (!parseTimeoutSpec(QStringLiteral("MIKAI_TEST_TIMEOUT_MS"), raw, &requested)) {
            return -1;
        }
        timeout = requested;
    }
    return timeout;
}

#ifdef Q_OS_WIN
// Windows has no POSIX fork hierarchy here: a grandchild of the test suite would
// survive TerminateProcess on the direct child and keep the scratch directory
// locked, which is exactly the invariant a deadline has to protect.
//
// Assigning a *running* process to a job is not sufficient. CreateProcess
// returns as soon as the new process's initial thread exists, and that thread is
// already executable: a child that spawns a descendant at the very top of
// main() can win the race and escape before AssignProcessToJobObject runs, so
// the job would contain the child but not its descendant.
//
// The modifier therefore holds the child's primary thread suspended while it is
// still the only thread in the process, assigns the (single-threaded) process to
// the job, and only then resumes it. Every thread the child ever creates is
// created after the assignment, so nothing the child runs can escape the job,
// and TerminateJobObject() bounds the whole run.

// Terminate-any-process-that-joins-the-job limit, set before the child does.
bool applyKillOnClose(HANDLE job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    ZeroMemory(&limits, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    return ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits)) != FALSE;
}

class OwnedJob {
public:
    OwnedJob() = default;
    ~OwnedJob() { close(); }

    // Creates the kill-on-close job the child is joined to. False when the OS
    // refuses, which the caller must treat as a refusal to run.
    bool create() {
        job_ = ::CreateJobObjectW(nullptr, nullptr);
        return job_ != nullptr;
    }

    OwnedJob(const OwnedJob&) = delete;
    OwnedJob& operator=(const OwnedJob&) = delete;

    bool valid() const { return job_ != nullptr; }

    /// Takes the suspended child by pid: opens it, joins it to the job and
    /// resumes its only thread. Until the thread is resumed the process has run
    /// no user code, so nothing it starts can escape the job. False with a named
    /// reason when any step fails.
    bool joinAndResume(qint64 pid, QString* error) {
        if (!adoptChild(pid, error)) return false;
        if (!::AssignProcessToJobObject(job_, childProcess_)) {
            *error = QStringLiteral("AssignProcessToJobObject failed (%1)")
                         .arg(static_cast<unsigned long>(::GetLastError()));
            return false;
        }
        memberJoined_ = true;
        return resumeOnlyThread(static_cast<unsigned long>(pid), error);
    }

    /// Opens the suspended child so it can be terminated even when the job
    /// assignment itself failed. The handle is ours and is closed by close().
    bool adoptChild(qint64 pid, QString* error) {
        if (childProcess_ != nullptr) return true;
        childProcess_ = ::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_SET_QUOTA |
                                          PROCESS_QUERY_LIMITED_INFORMATION,
                                      FALSE, static_cast<unsigned long>(pid));
        if (childProcess_ == nullptr) {
            *error = QStringLiteral("OpenProcess(%1) failed (%2)")
                         .arg(pid)
                         .arg(static_cast<unsigned long>(::GetLastError()));
            return false;
        }
        return true;
    }

    // Terminates every process in the job (the child and all of its
    // descendants) and waits for the job itself to drain, so the caller can
    // then remove the scratch directory with nothing holding it. Safe to call
    // when no process was ever joined.
    void killTree() {
        if (!job_) return;
        // A process that failed to join may still exist and may still be
        // suspended; TerminateJobObject cannot reach it. Kill it directly so no
        // live or wedged child is ever left behind.
        if (childProcess_ && !memberJoined_) {
            ::TerminateProcess(childProcess_, kTerminatedExitCode);
        }
        if (memberJoined_) {
            ::TerminateJobObject(job_, kTerminatedExitCode);
        }
        // The wait is what makes the confirmation meaningful: a job whose
        // member is wedged would still hold the scratch directory open.
        if (childProcess_ &&
            ::WaitForSingleObject(childProcess_, static_cast<DWORD>(kKillGraceMs)) ==
                WAIT_OBJECT_0) {
            killedAndDrained_ = true;
        }
        close();
    }

    // False when the timed kill could not confirm the tree is gone. The caller
    // must report it rather than claim the scratch is free.
    bool killConfirmed() const { return killedAndDrained_; }

private:
    /// Resumes the child's initial thread. A process created suspended has
    /// exactly one thread, and QProcess hands out neither thread handle, so the
    /// thread id is looked up in a snapshot and opened just to resume it.
    static bool resumeOnlyThread(unsigned long pid, QString* error) {
        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            *error = QStringLiteral("CreateToolhelp32Snapshot failed (%1)")
                         .arg(static_cast<unsigned long>(::GetLastError()));
            return false;
        }
        bool resumed = false;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(THREADENTRY32);
        if (::Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID != pid) continue;
                HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
                if (thread == nullptr) continue;
                const DWORD previous = ::ResumeThread(thread);
                ::CloseHandle(thread);
                if (previous == 0xFFFFFFFFu) {
                    *error = QStringLiteral("ResumeThread failed (%1)")
                                 .arg(static_cast<unsigned long>(::GetLastError()));
                    break;
                }
                resumed = true;
                break;
            } while (::Thread32Next(snapshot, &entry));
        }
        ::CloseHandle(snapshot);
        if (!resumed && error->isEmpty()) {
            *error = QStringLiteral("the suspended child has no resumable thread");
        }
        return resumed;
    }

    void close() {
        if (job_) {
            ::CloseHandle(job_);
            job_ = nullptr;
        }
        // This is the launcher's own handle from OpenProcess; QProcess keeps its
        // own, so closing this one never closes QProcess's.
        if (childProcess_) {
            ::CloseHandle(childProcess_);
            childProcess_ = nullptr;
        }
    }

    friend struct JobInstaller;

    HANDLE job_ = nullptr;
    HANDLE childProcess_ = nullptr;
    bool memberJoined_ = false;
    bool killedAndDrained_ = false;

public:
    bool installFailed = false;
};

// Installs the job into the child's CreateProcess arguments.
struct JobInstaller {
    OwnedJob* owner = nullptr;

    void operator()(QProcess::CreateProcessArguments* arguments) {
        if (!owner || !owner->job_) return;

        // The child is created suspended, so its only thread is the initial one
        // and it has not run any user code yet. The job joins it afterwards, by
        // pid: at this point the process does not exist, so processInformation
        // cannot be used for the assignment.
        arguments->flags |= CREATE_SUSPENDED;
        // The limit must be in place before the process joins the job.
        if (!applyKillOnClose(owner->job_)) {
            owner->installFailed = true;
        }
    }
};
#endif

bool copyTree(const QString& source, const QString& destination) {
    if (!QFileInfo(source).isDir() || !QDir().mkpath(destination)) return false;
    const QDir sourceDirectory(source);
    QDirIterator entries(source, QDir::AllEntries | QDir::Hidden | QDir::System |
                                    QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        entries.next();
        const QFileInfo entry = entries.fileInfo();
        const QString target = QDir(destination).filePath(
            sourceDirectory.relativeFilePath(entry.absoluteFilePath()));
        // Shader fixtures are ordinary files, not references outside the tree.
        if (entry.isSymLink()) return false;
        if (entry.isDir()) {
            if (!QDir().mkpath(target)) return false;
        } else if (!entry.isFile() || !QDir().mkpath(QFileInfo(target).absolutePath()) ||
                   !QFile::copy(entry.absoluteFilePath(), target)) {
            return false;
        }
    }
    return true;
}

int stageAndRun(const QStringList& arguments, const QString& directory,
                 const QString& explicitTimeoutSpec, const QStringList& forwarded) {
    const QString executable = QDir(directory).filePath(QFileInfo(arguments.at(1)).fileName());
    const QString mpvDirectory = QDir(directory).filePath(QStringLiteral("mpv"));
    const QString module = QDir(mpvDirectory).filePath(QStringLiteral("libmpv-2.dll"));
    if (!QFile::copy(arguments.at(1), executable) ||
        !copyTree(arguments.at(2), QDir(directory).filePath(QStringLiteral("shaders"))) ||
        !QDir().mkpath(mpvDirectory) || !QFile::copy(arguments.at(3), module)) {
        std::fprintf(stderr, "[mikai_test_launcher] cannot stage test executable/shaders/mpv in %s\n",
                     qPrintable(directory));
        return 2;
    }
    if (!QFileInfo(arguments.at(4)).isDir() ||
        !QFileInfo(QDir(arguments.at(5)).filePath(QStringLiteral("platforms"))).isDir()) {
        std::fprintf(stderr, "[mikai_test_launcher] configured Qt runtime/plugins are missing\n");
        return 2;
    }

    const qint64 timeoutMs = childTimeoutMs(explicitTimeoutSpec);
    if (timeoutMs < 0) return 2;

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PATH"), arguments.at(4) + QDir::listSeparator() +
                       environment.value(QStringLiteral("PATH")));
    environment.insert(QStringLiteral("QT_PLUGIN_PATH"), arguments.at(5));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"),
                       QDir(arguments.at(5)).filePath(QStringLiteral("platforms")));
    if (environment.value(QStringLiteral("QT_QPA_PLATFORM")).isEmpty()) {
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    }
    // The transport suite is given the same staged, pinned runtime as the
    // packaged-runtime suite, not an unrelated installed or environment DLL.
    environment.insert(QStringLiteral("MIKAI_MPV_MODULE"), module);

    QProcess child;
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    child.setInputChannelMode(QProcess::ForwardedInputChannel);
    child.setWorkingDirectory(directory);
    std::fprintf(stderr, "[mikai_test_launcher] isolated executable directory: %s\n",
                 qPrintable(directory));
    std::fprintf(stderr, "[mikai_test_launcher] child deadline: %lld ms\n",
                 static_cast<long long>(timeoutMs));
    std::fflush(stderr);

#ifdef Q_OS_WIN
    // The job exists so that the deadline can bound the whole run. Without it a
    // descendant would survive a kill, so a failure to create or assign one is
    // a refusal rather than a run with a weaker bound.
    OwnedJob job;
    if (!job.create()) {
        std::fprintf(stderr,
                     "[mikai_test_launcher] cannot create the child process job: %lu; a "
                     "descendant would survive the deadline\n",
                     static_cast<unsigned long>(::GetLastError()));
        return 2;
    }
    JobInstaller installer;
    installer.owner = &job;
    child.setCreateProcessArgumentsModifier(installer);

    // Abandons a child that exists but is still suspended: it is opened so it
    // can be terminated, killed and awaited, then reported as a refusal. The
    // suite never runs outside a job, because a descendant would escape it.
    const auto abandonSuspendedChild = [&](const QString& why) {
        std::fprintf(stderr,
                     "[mikai_test_launcher] %s: %lu; a descendant would survive the "
                     "deadline\n",
                     qPrintable(why), static_cast<unsigned long>(::GetLastError()));
        QString ignored;
        job.adoptChild(child.processId(), &ignored);
        job.killTree();
        if (!job.killConfirmed()) {
            std::fprintf(stderr,
                         "[mikai_test_launcher] the abandoned test process did not die within "
                         "%lld ms\n",
                         static_cast<long long>(kKillGraceMs));
        }
        child.kill();
        child.waitForFinished(kKillGraceMs);
        return 2;
    };

    // The child is created suspended, so it exists but has run nothing yet. It
    // is joined to the job by pid and only then resumed, which is what makes the
    // assignment race-free: nothing the child runs can precede the assignment.
    child.start(executable, forwarded);
    if (!child.waitForStarted()) {
        std::fprintf(stderr, "[mikai_test_launcher] cannot start tests: %s\n",
                     qPrintable(child.errorString()));
        return 2;
    }
    if (job.installFailed) {
        return abandonSuspendedChild(QStringLiteral("cannot bound the test process with a job"));
    }
    QString joinError;
    if (!job.joinAndResume(child.processId(), &joinError)) {
        std::fprintf(stderr, "[mikai_test_launcher] %s\n", qPrintable(joinError));
        return abandonSuspendedChild(QStringLiteral("cannot join the test process to the child job"));
    }
#else
    child.start(executable, forwarded);
    if (!child.waitForStarted()) {
        std::fprintf(stderr, "[mikai_test_launcher] cannot start tests: %s\n",
                     qPrintable(child.errorString()));
        return 2;
    }
#endif

    // Bounded wait: poll in slices so the process stays alive and responsive to
    // CTest while the deadline is honoured. waitForFinished(-1) never returned
    // for a hung child, so neither its nonzero exit nor its cleanup happened.
    QElapsedTimer elapsed;
    elapsed.start();
    while (child.state() != QProcess::NotRunning) {
        const qint64 remaining = timeoutMs - elapsed.elapsed();
        if (remaining <= 0) {
            std::fprintf(stderr,
                         "[mikai_test_launcher] tests exceeded the %lld ms deadline; killing "
                         "the process tree\n",
                         static_cast<long long>(timeoutMs));
            std::fflush(stderr);
#ifdef Q_OS_WIN
            // TerminateJobObject bounds the whole tree: the direct child and
            // every descendant, because the child was joined to the job before
            // it ran. It is awaited so the scratch directory is removable.
            job.killTree();
            if (!job.killConfirmed()) {
                std::fprintf(stderr,
                             "[mikai_test_launcher] the killed test tree did not exit "
                             "within %lld ms\n",
                             static_cast<long long>(kKillGraceMs));
            }
#else
            // No tree walk: the suites spawn no descendants, so killing the
            // direct child bounds the run.
            child.kill();
            if (!child.waitForFinished(kKillGraceMs)) {
                std::fprintf(stderr,
                             "[mikai_test_launcher] the killed test process did not exit "
                             "within %lld ms (state=%d)\n",
                             static_cast<long long>(kKillGraceMs),
                             static_cast<int>(child.state()));
            }
#endif
            std::fprintf(stderr, "[mikai_test_launcher] timed out (killed the test tree)\n");
            std::fflush(stderr);
            return kExitTimeout;
        }
        const qint64 slice = remaining < kPollSliceMs ? remaining : kPollSliceMs;
        if (!child.waitForFinished(slice)) {
            const QProcess::ProcessError error = child.error();
            if (error != QProcess::Timedout) {
                std::fprintf(stderr, "[mikai_test_launcher] cannot wait for tests: %s\n",
                             qPrintable(child.errorString()));
                return 2;
            }
        }
    }

    const int result = child.exitCode();
    if (child.exitStatus() == QProcess::CrashExit) {
        std::fprintf(stderr, "[mikai_test_launcher] test process crashed (rc=%d)\n", result);
        return result != 0 ? result : 1;
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() < 7) {
        std::fprintf(stderr, "usage: mikai_test_launcher <test-exe> <shaders> <mpv-dll> "
                     "<qt-runtime-bin> <qt-plugins> <scratch-parent> [options/args...]\n"
                     "options consumed by this launcher:\n"
                     "  --launcher-timeout-ms <ms>  bound the child run (default 600000)\n"
                     "  --launcher-deadline         print the effective deadline and exit\n");
        return 2;
    }
    // Options are consumed here, never forwarded: a child that received one
    // would treat it as an unknown QTest argument rather than a deadline.
    QString explicitTimeoutSpec;
    QStringList forwarded;
    for (int i = 7; i < arguments.size(); ++i) {
        const QString& argument = arguments.at(i);
        if (argument == QLatin1String("--launcher-timeout-ms")) {
            if (i + 1 >= arguments.size()) {
                std::fprintf(stderr, "[mikai_test_launcher] --launcher-timeout-ms needs a value\n");
                return 2;
            }
            explicitTimeoutSpec = arguments.at(++i);
            continue;
        }
        if (argument.startsWith(QLatin1String("--launcher-timeout-ms="))) {
            explicitTimeoutSpec = argument.mid(argument.indexOf(QLatin1Char('=')) + 1);
            continue;
        }
        if (argument == QLatin1String("--launcher-deadline")) {
            // Reports the effective deadline without running anything: a
            // verification script can assert the resolution without a child.
            const qint64 effective = childTimeoutMs(explicitTimeoutSpec);
            if (effective < 0) return 2;
            std::printf("%lld\n", static_cast<long long>(effective));
            std::fflush(stdout);
            return 0;
        }
        forwarded << argument;
    }
    if (!QDir().mkpath(arguments.at(6))) {
        std::fprintf(stderr, "[mikai_test_launcher] cannot create scratch parent\n");
        return 2;
    }
    QTemporaryDir scratch(QDir(arguments.at(6)).filePath(QStringLiteral("mikai-tests-XXXXXX")));
    if (!scratch.isValid()) {
        std::fprintf(stderr, "[mikai_test_launcher] cannot create owned executable directory: %s\n",
                     qPrintable(scratch.errorString()));
        return 2;
    }
    const int result = stageAndRun(arguments, scratch.path(), explicitTimeoutSpec, forwarded);
    // Remove only the directory QTemporaryDir created, even after fixture/test
    // failure, a child crash or a timeout kill. The public build output is never
    // moved/edited. This is the sole cleanup owner; the child is already gone by
    // the time any path above returns.
    if (!scratch.remove()) {
        std::fprintf(stderr, "[mikai_test_launcher] cannot remove owned scratch: %s\n",
                     qPrintable(scratch.path()));
        return result != 0 ? result : 2;
    }
    return result;
}