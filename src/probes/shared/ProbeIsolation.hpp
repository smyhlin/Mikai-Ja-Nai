// Physical isolation for the probes whose scenarios mutate executable-local
// state.
//
// Production `nativeRoot()` is `<applicationDirPath>/native`, so an org name, an
// app name, a CWD and a redirected `QSettings` path buy exactly nothing: every
// store the native track owns - models, recipes, artifacts, components, runtime -
// still resolves beside the executable that is running. A probe that wipes that
// tree wipes the real install, which is what KB-02 was about.
//
// The one thing that does isolate it is the executable itself. So this header
// copies the running probe into an owned QTemporaryDir and relaunches that copy:
// real executable-local isolation, the same pattern the physical CTest launcher
// already uses for the suites. The Qt runtime and plugins are resolved from the
// modules this process already loaded and from the plugin tree it already found,
// `LOCALAPPDATA` is redirected into the same owned directory so app-local state
// is scratch too, the shader and translation directories beside the original
// executable are copied in read-only, and any explicitly named runtime is left
// where it is and only referenced. The parent then waits on a bound, removes
// exactly the directory it created - success, failure or crash alike - and
// returns the child's verdict. Nothing outside that directory is staged, written
// or moved.
//
// The marker is a directory plus a random token written into that directory. The
// child proceeds only when the marked directory is the one its own module was
// loaded from, the token file is a regular file whose content matches, and no
// component on the way is a reparse point. A marker dropped into an ordinary run
// - a forged root, somebody else's scratch, a junction - is refused before any
// probe code runs, so a wipe can never land on a tree this process does not own.
//
// Nothing here touches `nativeRoot()` itself: production storage stays
// executable-local with no override, exactly as it was.

#pragma once

#include <cstdio>

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUuid>

#include <vector>

#ifdef Q_OS_WIN
#  include <Windows.h>
#endif

#include "native/bundle/NativePaths.hpp"

namespace mikai {
namespace probes {
namespace isolation {

inline constexpr char kMarkerRootEnv[] = "MIKAI_PROBE_ISOLATED_ROOT";
inline constexpr char kMarkerTokenEnv[] = "MIKAI_PROBE_ISOLATED_TOKEN";
inline constexpr char kTokenFileName[] = "mikai-probe-isolation.token";

/// Environment variables that name an absolute runtime this run must keep
/// using. A relative value is resolved against the original executable's
/// directory, because the isolated copy lives somewhere else entirely.
inline constexpr const char* kRuntimePathVariables[] = {"MIKAI_MPV_MODULE", "MIKAI_PROBE_BUNDLE",
                                                        "MIKAI_ORT_RUNTIME_DIR"};

/// Bound on the child wait: the probes' own internal timers are far shorter, so
/// a child still running after this is a hung one and is killed rather than
/// waited on forever.
inline constexpr int kDefaultChildTimeoutMs = 900000;
inline constexpr int kStartTimeoutMs = 30000;
inline constexpr int kKillTimeoutMs = 10000;

/// What the entry point answered about this process.
enum class Gate {
    /// This process is the isolated copy (or the caller's explicit real-tree
    /// mode): run the probe body.
    RunHere,
    /// A fresh isolated child was launched, waited for and cleaned up. The
    /// caller must return `exitCode()`.
    Handed,
    /// Isolation could not be established. Nothing was mutated; return
    /// `exitCode()`.
    Refused,
};

struct Options {
    /// Name used in every diagnostic this header prints.
    const char* probe = "probe";
    /// True for a mode that must run against the caller's own real prepared
    /// tree - explicitly requested, publishing nothing and wiping nothing.
    /// Everything else is staged into owned scratch.
    bool runInPlace = false;
    /// Child wait bound in milliseconds; 0 keeps the built-in default.
    int childTimeoutMs = 0;
};

namespace detail {

inline int& exitCodeStorage() {
    static int code = 2;
    return code;
}

/// The running module's own path, resolved from the platform rather than from
/// argv: the isolated copy is a separate process started from its own file, and
/// the module record is the one fact neither an argument nor an environment
/// entry can forge.
inline QString ownModulePath() {
#ifdef Q_OS_WIN
    std::vector<wchar_t> buffer(MAX_PATH);
    for (int attempt = 0; attempt < 4; ++attempt) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                               static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < static_cast<DWORD>(buffer.size())) {
            return QString::fromWCharArray(buffer.data(), static_cast<int>(length));
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
    return {};
}

inline QString ownModuleDirectory() {
    const QString module = ownModulePath();
    return module.isEmpty() ? QString() : QFileInfo(module).absolutePath();
}

/// The volume root a path sits on, so "no component on the way" is answerable
/// without following anything.
inline QString volumeRootOf(const QString& path) {
    const QString cleaned = QDir::cleanPath(path);
    if (cleaned.size() >= 2 && cleaned.at(0).isLetter() && cleaned.at(1) == QLatin1Char(':')) {
        return cleaned.left(2) + QLatin1Char('/');
    }
    if (cleaned.startsWith(QLatin1String("//"))) {
        const QStringList parts = cleaned.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            return QStringLiteral("//") + parts.at(0) + QLatin1Char('/') + parts.at(1);
        }
    }
    return {};
}

/// True when no component from the volume root down to `path` is a reparse
/// point. A junction or symlink anywhere on the way makes the resolved path
/// somebody else's decision, which is why the scratch root is proven this way
/// before it is wiped.
inline bool noReparsePoints(const QString& path) {
    if (path.isEmpty()) return false;
    const QString volume = volumeRootOf(path);
    if (volume.isEmpty()) return false;
    return mplayer::native::pathHasNoReparsePoints(volume, path);
}

inline bool copyDirectoryTree(const QString& source, const QString& destination) {
    if (!QFileInfo(source).isDir()) return false;
    if (!QDir().mkpath(destination)) return false;
    QDirIterator entries(source, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        const QString absolute = entries.next();
        const QString relative = QDir(source).relativeFilePath(absolute);
        const QString target = QDir(destination).filePath(relative);
        if (QFileInfo(absolute).isDir()) {
            if (!QDir().mkpath(target)) return false;
            continue;
        }
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) return false;
        if (!QFile::copy(absolute, target)) return false;
    }
    return true;
}

/// The Qt binary directory this process is already resolving its own Qt from.
/// Resolving it from the loaded module rather than from a build-time path is
/// what makes the isolated copy run on exactly this run's Qt.
inline QString resolveQtBin() {
    const QString named = qEnvironmentVariable("MIKAI_PROBE_QT_BIN");
    if (!named.isEmpty() && QFile::exists(QDir(named).filePath(QStringLiteral("Qt6Core.dll")))) {
        return QDir::cleanPath(named);
    }
#ifdef Q_OS_WIN
    HMODULE core = nullptr;
    if (GetModuleHandleExW(0, L"Qt6Core.dll", &core) != 0 && core != nullptr) {
        std::vector<wchar_t> buffer(MAX_PATH);
        const DWORD length =
            GetModuleFileNameW(core, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length > 0 && length < static_cast<DWORD>(buffer.size())) {
            const QString directory =
                QFileInfo(QString::fromWCharArray(buffer.data(), static_cast<int>(length)))
                    .absolutePath();
            if (QFile::exists(QDir(directory).filePath(QStringLiteral("Qt6Core.dll")))) {
                return directory;
            }
        }
    }
#endif
    return {};
}

inline QString resolveQtPlugins(const QString& qtBin) {
    const QString named = qEnvironmentVariable("MIKAI_PROBE_QT_PLUGINS");
    if (!named.isEmpty() &&
        QFileInfo(QDir(named).filePath(QStringLiteral("platforms"))).isDir()) {
        return QDir::cleanPath(named);
    }
    const QStringList candidates = {
        qEnvironmentVariable("QT_PLUGIN_PATH"), QDir(qtBin).filePath(QStringLiteral("../plugins")),
        QLibraryInfo::path(QLibraryInfo::PluginsPath)};
    for (const QString& candidate : candidates) {
        if (candidate.isEmpty()) continue;
        if (QFileInfo(QDir(candidate).filePath(QStringLiteral("platforms"))).isDir()) {
            return QDir::cleanPath(candidate);
        }
    }
    return {};
}

/// The valid isolated root for *this* process: the guarded marker the parent
/// wrote, or empty. Empty also means a requested marker did not verify, and
/// `problem` names which, so a hostile marker is refused rather than trusted.
inline QString isolatedRoot(QString* problem = nullptr) {
    const auto reject = [problem](const QString& text) {
        if (problem != nullptr) *problem = text;
        return QString();
    };
    if (qEnvironmentVariableIsEmpty(kMarkerRootEnv)) return reject(QStringLiteral("none"));
    const QString marker = qEnvironmentVariable(kMarkerRootEnv);
    if (marker.trimmed().isEmpty()) return reject(QStringLiteral("the marker names no directory"));

    const QString moduleDirectory = ownModuleDirectory();
    if (moduleDirectory.isEmpty()) {
        return reject(QStringLiteral("the running module cannot be resolved"));
    }
    const QString cleaned = QDir::cleanPath(marker);
    const QString canonical = mplayer::native::canonicalPath(cleaned);
    const QString canonicalModule = mplayer::native::canonicalPath(moduleDirectory);
    const bool matches = cleaned.compare(moduleDirectory, mplayer::native::pathCaseSensitivity()) ==
                         0 ||
                         (!canonical.isEmpty() && !canonicalModule.isEmpty() &&
                          canonical.compare(canonicalModule,
                                            mplayer::native::pathCaseSensitivity()) == 0);
    if (!matches) {
        return reject(QStringLiteral("the marker names %1, which is not the directory this "
                                     "module was loaded from (%2)")
                          .arg(cleaned, moduleDirectory));
    }
    if (!QFileInfo(cleaned).isDir()) {
        return reject(QStringLiteral("the marked directory %1 does not exist").arg(cleaned));
    }
    const QString provenRoot = canonicalModule.isEmpty() ? cleaned : canonicalModule;
    if (!noReparsePoints(provenRoot)) {
        return reject(QStringLiteral("the marked directory %1 is reached through a reparse point")
                          .arg(cleaned));
    }

    const QString token = qEnvironmentVariable(kMarkerTokenEnv).trimmed();
    if (token.size() < 16) {
        return reject(QStringLiteral("the marker carries no token"));
    }
    const QString tokenPath = QDir(cleaned).filePath(QLatin1String(kTokenFileName));
    const QFileInfo tokenInfo(tokenPath);
    if (!tokenInfo.isFile() || tokenInfo.isSymLink()) {
        return reject(QStringLiteral("%1 is not a regular file").arg(tokenPath));
    }
    if (!mplayer::native::pathHasNoReparsePoints(provenRoot, tokenPath)) {
        return reject(QStringLiteral("%1 is reached through a reparse point").arg(tokenPath));
    }
    QFile tokenFile(tokenPath);
    if (!tokenFile.open(QIODevice::ReadOnly)) {
        return reject(QStringLiteral("%1 cannot be read").arg(tokenPath));
    }
    const QString stored = QString::fromUtf8(tokenFile.readAll()).trimmed();
    if (stored.compare(token) != 0) {
        return reject(QStringLiteral("the marker token does not match %1").arg(tokenPath));
    }
    return cleaned;
}

}  // namespace detail

/// The exit code to return when the entry point did not hand the run to this
/// process: the staged child's own verdict, or 2 for a refusal.
inline int exitCode() { return detail::exitCodeStorage(); }

/// The isolated root this process owns, or empty when it does not have one.
inline QString isolatedRoot() { return detail::isolatedRoot(); }

/// True when `path` is inside this process's isolated root and no component on
/// the way is a reparse point. A probe wipes only what this answers yes for.
inline bool isOwnedScratch(const QString& path) {
    const QString root = detail::isolatedRoot();
    if (root.isEmpty() || path.isEmpty()) return false;
    const QString cleaned = QDir::cleanPath(path);
    if (!mplayer::native::pathWithinRoot(root, cleaned)) return false;
    return mplayer::native::pathHasNoReparsePoints(root, cleaned);
}

/// The gate every mutating probe passes through before it constructs a
/// QApplication.
///
/// With no valid marker this process is the launcher: a copy of the running
/// executable goes into owned scratch, the copy is relaunched with a guarded
/// marker, waited for on a bound, and the scratch is removed afterwards. With a
/// valid marker this process *is* that copy and simply runs. A requested marker
/// that does not verify - forged root, foreign scratch, reparse path, wrong
/// token - is refused before anything else happens.
inline Gate enter(int argc, char** argv, const Options& options) {
    QStringList arguments;
    arguments.reserve(argc);
    for (int i = 0; i < argc; ++i) arguments << QString::fromLocal8Bit(argv[i]);

    const bool markerRequested = !qEnvironmentVariableIsEmpty(kMarkerRootEnv) ||
                                 !qEnvironmentVariableIsEmpty(kMarkerTokenEnv);
    QString problem;
    const QString root = detail::isolatedRoot(&problem);
    if (!root.isEmpty()) return Gate::RunHere;  // this process is the validated copy
    if (markerRequested) {
        std::fprintf(stderr, "[%s] refusing an unverified isolation marker: %s\n", options.probe,
                     qPrintable(problem));
        detail::exitCodeStorage() = 2;
        return Gate::Refused;
    }
    if (options.runInPlace) return Gate::RunHere;  // the caller's own real tree, read-only

    const QString module = detail::ownModulePath();
    if (module.isEmpty()) {
        std::fprintf(stderr, "[%s] cannot isolate: the running module cannot be resolved\n",
                     options.probe);
        detail::exitCodeStorage() = 2;
        return Gate::Refused;
    }
    const QString qtBin = detail::resolveQtBin();
    const QString qtPlugins = qtBin.isEmpty() ? QString() : detail::resolveQtPlugins(qtBin);
    if (qtBin.isEmpty()) {
        std::fprintf(stderr,
                     "[%s] cannot isolate: the Qt runtime this probe runs on cannot be located "
                     "(set MIKAI_PROBE_QT_BIN)\n",
                     options.probe);
        detail::exitCodeStorage() = 2;
        return Gate::Refused;
    }
    if (qtPlugins.isEmpty()) {
        std::fprintf(stderr,
                     "[%s] cannot isolate: the Qt plugin tree this probe runs on cannot be "
                     "located (set MIKAI_PROBE_QT_PLUGINS)\n",
                     options.probe);
        detail::exitCodeStorage() = 2;
        return Gate::Refused;
    }

    QTemporaryDir scratch(QDir(QDir::tempPath()).filePath(QStringLiteral("mikai-probe-XXXXXX")));
    if (!scratch.isValid()) {
        std::fprintf(stderr, "[%s] cannot isolate: no owned scratch directory: %s\n",
                     options.probe, qPrintable(scratch.errorString()));
        detail::exitCodeStorage() = 2;
        return Gate::Refused;
    }
    if (!detail::noReparsePoints(scratch.path())) {
        std::fprintf(stderr, "[%s] refusing scratch reached through a reparse point: %s\n",
                     options.probe, qPrintable(scratch.path()));
        return Gate::Refused;  // QTemporaryDir removes the directory it created
    }

    const QString staged = QDir(scratch.path()).filePath(QFileInfo(module).fileName());
    if (!QFile::copy(module, staged)) {
        std::fprintf(stderr, "[%s] cannot isolate: the executable cannot be copied to %s\n",
                     options.probe, qPrintable(staged));
        detail::exitCodeStorage() = 2;
        return Gate::Refused;  // QTemporaryDir removes what it created
    }

    // The token: random per run, only inside this owned directory, and the copy
    // refuses to proceed without matching it.
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces) +
                          QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString tokenPath = QDir(scratch.path()).filePath(QLatin1String(kTokenFileName));
    {
        QFile file(tokenPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(token.toUtf8()) != token.toUtf8().size()) {
            std::fprintf(stderr, "[%s] cannot isolate: the isolation marker cannot be written\n",
                         options.probe);
            detail::exitCodeStorage() = 2;
            return Gate::Refused;  // QTemporaryDir removes what it created
        }
    }

    // Read-only inputs the isolated copy needs beside itself: the shader and
    // translation directories, when the real executable has them. The original
    // is never moved or edited and its native tree is never copied.
    const QString moduleDirectory = QFileInfo(module).absolutePath();
    const QStringList copied = {QStringLiteral("shaders"), QStringLiteral("translations")};
    for (const QString& name : copied) {
        const QString source = QDir(moduleDirectory).filePath(name);
        const QString destination = QDir(scratch.path()).filePath(name);
        if (!QFileInfo(source).isDir()) continue;
        if (!detail::copyDirectoryTree(source, destination)) {
            std::fprintf(stderr, "[%s] cannot isolate: %s cannot be staged beside the copy\n",
                         options.probe, qPrintable(name));
            detail::exitCodeStorage() = 2;
            return Gate::Refused;  // QTemporaryDir removes what it created
        }
    }

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QLatin1String(kMarkerRootEnv), scratch.path());
    environment.insert(QLatin1String(kMarkerTokenEnv), token);
    // App-local state is scratch too: the cache the runtime writes and the
    // tree a probe clears both live under this owned directory now.
    environment.insert(QStringLiteral("LOCALAPPDATA"),
                       QDir(scratch.path()).filePath(QStringLiteral("appdata")));
    environment.insert(QStringLiteral("PATH"),
                       qtBin + QDir::listSeparator() + environment.value(QStringLiteral("PATH")));
    environment.insert(QStringLiteral("QT_PLUGIN_PATH"), qtPlugins);
    environment.insert(QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"),
                       QDir(qtPlugins).filePath(QStringLiteral("platforms")));
    if (environment.value(QStringLiteral("QT_QPA_PLATFORM")).isEmpty()) {
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    }
    // A relative runtime name is anchored to the original executable, never to
    // the isolated copy.
    for (const char* name : kRuntimePathVariables) {
        const QString value = environment.value(QLatin1String(name));
        if (value.isEmpty() || QFileInfo(value).isAbsolute()) continue;
        environment.insert(QLatin1String(name), QDir(moduleDirectory).absoluteFilePath(value));
    }

    QProcess child;
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    child.setWorkingDirectory(QDir::currentPath());
    child.start(staged, arguments.mid(1));
    if (!child.waitForStarted(kStartTimeoutMs)) {
        std::fprintf(stderr, "[%s] the isolated copy %s did not start: %s\n", options.probe,
                     qPrintable(staged), qPrintable(child.errorString()));
        detail::exitCodeStorage() = 2;
        return Gate::Handed;  // QTemporaryDir removes what it created on return
    }
    std::fprintf(stderr, "[%s] isolated executable directory: %s\n", options.probe,
                 qPrintable(scratch.path()));

    const int timeout = options.childTimeoutMs > 0 ? options.childTimeoutMs : kDefaultChildTimeoutMs;
    if (!child.waitForFinished(timeout) && child.state() != QProcess::NotRunning) {
        std::fprintf(stderr, "[%s] the isolated copy did not finish within %d ms; killing it\n",
                     options.probe, timeout);
        child.kill();
        child.waitForFinished(kKillTimeoutMs);
    }

    int code = 2;
    if (child.exitStatus() == QProcess::NormalExit) {
        code = child.exitCode();
    } else {
        std::fprintf(stderr, "[%s] the isolated copy exited abnormally: %s\n", options.probe,
                     qPrintable(child.errorString()));
        code = 2;
    }

    // Remove only the directory QTemporaryDir created, even on failure or a
    // crashed child: the public build output and the real native tree are never
    // staged and never removed.
    if (!scratch.remove()) {
        std::fprintf(stderr, "[%s] cannot remove owned scratch %s: %s\n", options.probe,
                     qPrintable(scratch.path()), qPrintable(scratch.errorString()));
        if (code == 0) code = 2;
    }
    detail::exitCodeStorage() = code;
    return Gate::Handed;
}

}  // namespace isolation
}  // namespace probes
}  // namespace mikai
