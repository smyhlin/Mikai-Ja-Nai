// CTest owns this process; only its unique executable directory is disposable.
// Production nativeRoot() stays executable-local, with no test environment hook.
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTemporaryDir>
#ifdef Q_OS_LINUX
#include <QtCore/QThread>
#endif

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <tlhelp32.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#ifdef Q_OS_LINUX
#include <cerrno>
#include <csignal>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

#ifdef Q_OS_LINUX
// QProcess owns the direct test child. Once that child exits, Linux reparents
// its orphaned descendants to this subreaper. Every child visible here belongs
// to the single test process tree launched below; use its kernel parent PID and
// start time rather than a process name or a broad system-wide match.
struct AdoptedChildIdentity {
    pid_t pid = -1;
    unsigned long long startTime = 0;
};

bool readLinuxProcessIdentity(pid_t pid, pid_t* parentPid,
                              unsigned long long* startTime) {
    QFile statFile(QStringLiteral("/proc/%1/stat").arg(static_cast<qlonglong>(pid)));
    if (!statFile.open(QIODevice::ReadOnly)) return false;
    const QByteArray stat = statFile.readAll();
    const qsizetype commandEnd = stat.lastIndexOf(')');
    if (commandEnd < 0) return false;

    // comm is parenthesized but may itself contain spaces or ')'. The final ')'
    // is the delimiter before field 3 (state).
    const QList<QByteArray> fields = stat.mid(commandEnd + 1).simplified().split(' ');
    if (fields.size() <= 19) return false;
    bool parentOk = false;
    bool startOk = false;
    const qlonglong parsedParent = fields.at(1).toLongLong(&parentOk);
    const qulonglong parsedStart = fields.at(19).toULongLong(&startOk);
    if (!parentOk || !startOk) return false;
    *parentPid = static_cast<pid_t>(parsedParent);
    *startTime = static_cast<unsigned long long>(parsedStart);
    return true;
}

QList<AdoptedChildIdentity> directAdoptedChildren(bool* scanSucceeded) {
    QList<AdoptedChildIdentity> children;
    QDir proc(QStringLiteral("/proc"));
    const QStringList entries =
        proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    const pid_t launcherPid = ::getpid();
    bool launcherEntryFound = false;
    for (const QString& entry : entries) {
        bool pidOk = false;
        const qlonglong parsedPid = entry.toLongLong(&pidOk);
        if (!pidOk || parsedPid <= 0) continue;
        if (parsedPid == launcherPid) {
            launcherEntryFound = true;
            continue;
        }

        const pid_t pid = static_cast<pid_t>(parsedPid);
        pid_t parentPid = -1;
        unsigned long long startTime = 0;
        if (readLinuxProcessIdentity(pid, &parentPid, &startTime) &&
            parentPid == launcherPid) {
            children.append({pid, startTime});
        }
    }
    *scanSucceeded = launcherEntryFound;
    return children;
}

bool stillOwnedAdoptedChild(const AdoptedChildIdentity& child) {
    pid_t parentPid = -1;
    unsigned long long startTime = 0;
    return readLinuxProcessIdentity(child.pid, &parentPid, &startTime) &&
           parentPid == ::getpid() && startTime == child.startTime;
}

// Kills and reaps only direct children adopted by this launcher. Killing a
// child can orphan another generation, so rescan until the owned tree is empty.
bool killAndReapAdoptedChildren(bool* foundChildren) {
    QElapsedTimer timer;
    timer.start();
    bool found = false;
    while (timer.elapsed() < kKillGraceMs) {
        bool scanSucceeded = false;
        const QList<AdoptedChildIdentity> children =
            directAdoptedChildren(&scanSucceeded);
        if (!scanSucceeded) {
            std::fprintf(stderr,
                         "[mikai_test_launcher] cannot enumerate direct children in /proc\n");
            *foundChildren = found;
            return false;
        }
        if (children.isEmpty()) {
            *foundChildren = found;
            return true;
        }
        found = true;

        for (const AdoptedChildIdentity& child : children) {
            if (!stillOwnedAdoptedChild(child)) continue;
            if (::kill(child.pid, SIGKILL) != 0 && errno != ESRCH) {
                std::fprintf(stderr,
                             "[mikai_test_launcher] cannot kill adopted test descendant %ld: %d\n",
                             static_cast<long>(child.pid), errno);
            }
        }

        bool madeProgress = false;
        for (const AdoptedChildIdentity& child : children) {
            int status = 0;
            const pid_t waited = ::waitpid(child.pid, &status, WNOHANG);
            if (waited == child.pid || (waited < 0 && errno == ECHILD)) {
                madeProgress = true;
            } else if (waited < 0 && errno != EINTR) {
                std::fprintf(stderr,
                             "[mikai_test_launcher] cannot reap adopted test descendant %ld: %d\n",
                             static_cast<long>(child.pid), errno);
            }
        }
        if (!madeProgress) QThread::msleep(10);
    }

    *foundChildren = found;
    bool scanSucceeded = false;
    const QList<AdoptedChildIdentity> remaining =
        directAdoptedChildren(&scanSucceeded);
    return scanSucceeded && remaining.isEmpty();
}
#endif

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
        childPid_ = static_cast<DWORD>(pid);
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

    // QProcess has already observed the direct child exit and captured its
    // status. Release this second process handle so job accounting can distinguish
    // completed-root bookkeeping from any live descendants.
    bool releaseExitedChildHandle() {
        if (!childProcess_) return true;
        if (::WaitForSingleObject(childProcess_, 0) != WAIT_OBJECT_0) return false;
        ::CloseHandle(childProcess_);
        childProcess_ = nullptr;
        return true;
    }

    // Stops every process still in the job before scratch removal. Job signaling
    // is not an end-of-job predicate for ordinary process exits, so inspect the
    // member snapshot and verify each non-root PID against this job.
    void killTree(bool forceTermination = false) {
        if (!job_) return;
        if (childProcess_ && !memberJoined_ &&
            !::TerminateProcess(childProcess_, kTerminatedExitCode)) {
            activeMemberCountKnown_ = false;
            activeMemberCountError_ = ::GetLastError();
        }

        if (memberJoined_) {
            hadActiveMembers_ = false;
            if (!scanLiveDescendants(&hadActiveMembers_)) {
                activeMemberCountKnown_ = false;
            }
            if (forceTermination || !activeMemberCountKnown_ || hadActiveMembers_) {
                if (!::TerminateJobObject(job_, kTerminatedExitCode)) {
                    activeMemberCountKnown_ = false;
                    activeMemberCountError_ = ::GetLastError();
                }
            }

            QElapsedTimer elapsed;
            elapsed.start();
            while (activeMemberCountKnown_) {
                bool liveDescendant = false;
                if (!scanLiveDescendants(&liveDescendant)) {
                    activeMemberCountKnown_ = false;
                    break;
                }
                bool directChildAlive = false;
                if (childProcess_) {
                    const DWORD result = ::WaitForSingleObject(childProcess_, 0);
                    if (result == WAIT_TIMEOUT) {
                        directChildAlive = true;
                    } else if (result != WAIT_OBJECT_0) {
                        activeMemberCountKnown_ = false;
                        activeMemberCountError_ = ::GetLastError();
                        break;
                    }
                }
                if (!liveDescendant && !directChildAlive) {
                    killedAndDrained_ = true;
                    break;
                }
                if (elapsed.elapsed() >= kKillGraceMs) break;
                ::Sleep(10);
            }
        } else if (childProcess_) {
            killedAndDrained_ =
                ::WaitForSingleObject(childProcess_, static_cast<DWORD>(kKillGraceMs)) ==
                WAIT_OBJECT_0;
        }
        close();
    }

    // False when the timed kill could not confirm the tree is gone. The caller
    // must report it rather than claim the scratch is free.
    bool killConfirmed() const { return killedAndDrained_; }
    bool activeMemberCountKnown() const { return activeMemberCountKnown_; }
    unsigned long activeMemberCountError() const { return activeMemberCountError_; }
    bool hadActiveMembers() const { return hadActiveMembers_; }

private:
    bool scanLiveDescendants(bool* found) {
        *found = false;
        size_t capacity = 1;
        std::vector<unsigned char> storage;
        JOBOBJECT_BASIC_PROCESS_ID_LIST* list = nullptr;
        bool listed = false;
        for (int attempt = 0; attempt < 8; ++attempt) {
            const size_t bytes =
                FIELD_OFFSET(JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList) +
                capacity * sizeof(ULONG_PTR);
            if (bytes > std::numeric_limits<DWORD>::max()) {
                activeMemberCountError_ = ERROR_NOT_ENOUGH_MEMORY;
                return false;
            }
            storage.resize(std::max(bytes, sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST)));
            list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(storage.data());
            if (::QueryInformationJobObject(job_, JobObjectBasicProcessIdList, list,
                                            static_cast<DWORD>(storage.size()), nullptr)) {
                listed = true;
                break;
            }
            const DWORD error = ::GetLastError();
            if (error != ERROR_MORE_DATA) {
                activeMemberCountError_ = error;
                return false;
            }
            const size_t assigned = list->NumberOfAssignedProcesses;
            capacity = assigned > capacity ? assigned : capacity * 2;
        }
        if (!listed) {
            activeMemberCountError_ = ERROR_MORE_DATA;
            return false;
        }

        for (ULONG i = 0; i < list->NumberOfProcessIdsInList; ++i) {
            const DWORD pid = static_cast<DWORD>(list->ProcessIdList[i]);
            if (pid == childPid_) continue;
            HANDLE process =
                ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (process == nullptr) {
                const DWORD error = ::GetLastError();
                if (error == ERROR_INVALID_PARAMETER) continue;
                activeMemberCountError_ = error;
                return false;
            }
            const DWORD firstWait = ::WaitForSingleObject(process, 0);
            if (firstWait == WAIT_OBJECT_0) {
                ::CloseHandle(process);
                continue;
            }
            if (firstWait != WAIT_TIMEOUT) {
                activeMemberCountError_ = ::GetLastError();
                ::CloseHandle(process);
                return false;
            }
            BOOL stillInJob = FALSE;
            if (!::IsProcessInJob(process, job_, &stillInJob)) {
                activeMemberCountError_ = ::GetLastError();
                ::CloseHandle(process);
                return false;
            }
            const DWORD finalWait = ::WaitForSingleObject(process, 0);
            ::CloseHandle(process);
            if (finalWait == WAIT_OBJECT_0 || !stillInJob) continue;
            if (finalWait != WAIT_TIMEOUT) {
                activeMemberCountError_ = ::GetLastError();
                return false;
            }
            *found = true;
        }
        return true;
    }

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
    DWORD childPid_ = 0;
    bool memberJoined_ = false;
    bool killedAndDrained_ = false;
    bool activeMemberCountKnown_ = true;
    unsigned long activeMemberCountError_ = 0;
    bool hadActiveMembers_ = false;

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
        // cannot be used for the assignment. The fixture's forwarded standard
        // handles work without attaching a console; detaching avoids a conhost
        // helper being mistaken for a fixture descendant in the job.
        arguments->flags |= CREATE_SUSPENDED | DETACHED_PROCESS;
        // The limit must be in place before the process joins the job.
        if (!applyKillOnClose(owner->job_)) {
            owner->installFailed = true;
        }
    }
};
#endif

bool stageMpvRuntime(const QString& moduleSource, const QString& runtimeRoot,
                     const QString& mpvDirectory, QString* stagedModule) {
    const QFileInfo moduleInfo(moduleSource);
    if (!moduleInfo.isFile() || moduleInfo.isSymLink() || !moduleInfo.isReadable()) return false;
    if (runtimeRoot.isEmpty()) {
        *stagedModule = QDir(mpvDirectory).filePath(moduleInfo.fileName());
        return QFile::copy(moduleSource, *stagedModule);
    }
#ifdef Q_OS_LINUX
    const QFileInfo rootInfo(runtimeRoot);
    if (!rootInfo.isDir() || rootInfo.isSymLink()) return false;
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    const QString canonicalModule = moduleInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty() || canonicalModule.isEmpty() ||
        QFileInfo(canonicalModule).absolutePath() != canonicalRoot) return false;

    QDir root(canonicalRoot);
    const QStringList entries = root.entryList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    bool moduleCopied = false;
    for (const QString& name : entries) {
        const QFileInfo source(root.filePath(name));
        if (source.isSymLink()) return false;
        if (!name.contains(QLatin1String(".so"))) continue;
        if (!source.isFile() || !source.isReadable()) return false;
        const QString destination = QDir(mpvDirectory).filePath(name);
        if (!QFile::copy(source.absoluteFilePath(), destination)) return false;
        if (source.canonicalFilePath() == canonicalModule) {
            *stagedModule = destination;
            moduleCopied = true;
        }
    }
    return moduleCopied;
#else
    Q_UNUSED(moduleSource);
    Q_UNUSED(runtimeRoot);
    Q_UNUSED(mpvDirectory);
    Q_UNUSED(stagedModule);
    return false;
#endif
}

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
                 const QString& explicitTimeoutSpec, const QString& runtimeRoot,
                 const QStringList& forwarded) {
    const QString executable = QDir(directory).filePath(QFileInfo(arguments.at(1)).fileName());
    const QString mpvDirectory = QDir(directory).filePath(QStringLiteral("mpv"));
    QString module;
    if (!QFile::copy(arguments.at(1), executable) ||
        !copyTree(arguments.at(2), QDir(directory).filePath(QStringLiteral("shaders"))) ||
        !QDir().mkpath(mpvDirectory) ||
        !stageMpvRuntime(arguments.at(3), runtimeRoot, mpvDirectory, &module)) {
        std::fprintf(stderr, "[mikai_test_launcher] cannot stage test executable/shaders/mpv module in %s\n",
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
#ifdef Q_OS_WIN
    environment.insert(QStringLiteral("PATH"), arguments.at(4) + QDir::listSeparator() +
                       environment.value(QStringLiteral("PATH")));
#else
    environment.insert(QStringLiteral("LD_LIBRARY_PATH"), arguments.at(4));
#endif
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
        job.killTree(true);
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
#ifdef Q_OS_LINUX
    pid_t processGroup = -1;
    child.setChildProcessModifier([] {
        if (::setpgid(0, 0) != 0) std::_Exit(127);
    });
#endif
    child.start(executable, forwarded);
    if (!child.waitForStarted()) {
        std::fprintf(stderr, "[mikai_test_launcher] cannot start tests: %s\n",
                     qPrintable(child.errorString()));
        return 2;
    }
#ifdef Q_OS_LINUX
    processGroup = static_cast<pid_t>(child.processId());
#endif
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
            job.killTree(true);
            if (!job.killConfirmed()) {
                std::fprintf(stderr,
                             "[mikai_test_launcher] the killed test tree did not exit "
                             "within %lld ms\n",
                             static_cast<long long>(kKillGraceMs));
            }
#else
            // The process group contains ordinary descendants. A truly detached
            // child has left it, but after the fixture dies the subreaper adopts
            // that process directly and can identify it safely by PID, parent
            // PID and /proc start time.
            bool cleanupConfirmed = true;
            if (processGroup > 0 && ::kill(-processGroup, SIGKILL) != 0 && errno != ESRCH) {
                std::fprintf(stderr, "[mikai_test_launcher] cannot kill test process group: %d\n",
                             errno);
                cleanupConfirmed = false;
            }
            if (!child.waitForFinished(kKillGraceMs) &&
                child.state() != QProcess::NotRunning) {
                std::fprintf(stderr,
                             "[mikai_test_launcher] test process did not exit after the tree kill; "
                             "trying direct-child termination\n");
                child.kill();
                if (!child.waitForFinished(kKillGraceMs) &&
                    child.state() != QProcess::NotRunning) {
                    std::fprintf(stderr,
                                 "[mikai_test_launcher] direct test process did not exit within "
                                 "%lld ms\n",
                                 static_cast<long long>(kKillGraceMs));
                    cleanupConfirmed = false;
                }
            }
            bool adoptedChildrenFound = false;
            const bool adoptedChildrenDrained =
                killAndReapAdoptedChildren(&adoptedChildrenFound);
            if (!adoptedChildrenDrained) {
                std::fprintf(stderr,
                             "[mikai_test_launcher] adopted test descendants did not drain within "
                             "%lld ms; scratch cleanup may be incomplete\n",
                             static_cast<long long>(kKillGraceMs));
                cleanupConfirmed = false;
            }
            if (!cleanupConfirmed) {
                std::fprintf(stderr,
                             "[mikai_test_launcher] timed-out process tree cleanup was not confirmed\n");
                return 2;
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
    const QProcess::ExitStatus exitStatus = child.exitStatus();

#ifdef Q_OS_WIN
    // QProcess retains its native process handle until it is closed. Close it
    // before querying the job so a completed root process is not counted as a
    // remaining job member while its handle is still owned by QProcess.
    child.close();
    const bool directChildHandleReleased = job.releaseExitedChildHandle();
    // A normal or abnormal direct-child exit still has to drain any detached
    // processes retained by its job before the owned scratch directory is
    // removed. An empty job preserves the child's exit status.
    job.killTree(!directChildHandleReleased);
    if (!directChildHandleReleased) {
        std::fprintf(stderr,
                     "[mikai_test_launcher] cannot confirm the direct test process has exited\n");
        return 2;
    }
    if (!job.killConfirmed()) {
        std::fprintf(stderr,
                     "[mikai_test_launcher] remaining test-job processes did not drain within "
                     "%lld ms; scratch cleanup may be incomplete\n",
                     static_cast<long long>(kKillGraceMs));
        return 2;
    }
    if (!job.activeMemberCountKnown()) {
        std::fprintf(stderr,
                     "[mikai_test_launcher] cannot inspect the test job's remaining processes: "
                     "%lu\n",
                     job.activeMemberCountError());
        return 2;
    }
    if (job.hadActiveMembers()) {
        std::fprintf(stderr,
                     "[mikai_test_launcher] tests exited with live descendants; cleaned the "
                     "owned job processes\n");
        return 2;
    }
#endif

#ifdef Q_OS_LINUX
    // The direct child can exit normally or crash while a detached descendant
    // remains outside its process group. It is now an adopted direct child of
    // this subreaper; kill and reap it before returning or removing scratch.
    bool adoptedChildrenFound = false;
    const bool adoptedChildrenDrained =
        killAndReapAdoptedChildren(&adoptedChildrenFound);
    if (!adoptedChildrenDrained) {
        std::fprintf(stderr,
                     "[mikai_test_launcher] unexpected adopted test descendants did not drain "
                     "within %lld ms; scratch cleanup may be incomplete\n",
                     static_cast<long long>(kKillGraceMs));
        return 2;
    }
    if (adoptedChildrenFound) {
        std::fprintf(stderr,
                     "[mikai_test_launcher] tests exited with live descendants; cleaned the "
                     "owned descendant processes\n");
        return 2;
    }
#endif
    if (exitStatus == QProcess::CrashExit) {
        std::fprintf(stderr, "[mikai_test_launcher] test process crashed (rc=%d)\n", result);
        return result != 0 ? result : 1;
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
#ifdef Q_OS_LINUX
    // Make orphaned descendants children of this launcher so they can be
    // reaped after their dedicated process group is terminated.
    if (::prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) {
        std::fprintf(stderr, "[mikai_test_launcher] cannot become a child subreaper\n");
        return 2;
    }
    bool procScanSucceeded = false;
    directAdoptedChildren(&procScanSucceeded);
    if (!procScanSucceeded) {
        std::fprintf(stderr, "[mikai_test_launcher] cannot enumerate direct children in /proc\n");
        return 2;
    }
#endif
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() < 7) {
        std::fprintf(stderr, "usage: mikai_test_launcher <test-exe> <shaders> <mpv-module> "
                     "<qt-runtime-bin> <qt-plugins> <scratch-parent> [options/args...]\n"
                     "options consumed by this launcher:\n"
                     "  --launcher-timeout-ms <ms>  bound the child run (default 600000)\n"
                     "  --launcher-deadline         print the effective deadline and exit\n"
                     "  --mpv-runtime-dir <path>    stage a private Linux mpv dependency closure\n");
        return 2;
    }
    // Options are consumed here, never forwarded: a child that received one
    // would treat it as an unknown QTest argument rather than a deadline.
    QString explicitTimeoutSpec;
    QString runtimeRoot;
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
        if (argument == QLatin1String("--mpv-runtime-dir")) {
            if (i + 1 >= arguments.size() || !runtimeRoot.isEmpty()) {
                std::fprintf(stderr, "[mikai_test_launcher] --mpv-runtime-dir needs one value\n");
                return 2;
            }
            runtimeRoot = arguments.at(++i);
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
    const int result = stageAndRun(arguments, scratch.path(), explicitTimeoutSpec,
                                   runtimeRoot, forwarded);
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