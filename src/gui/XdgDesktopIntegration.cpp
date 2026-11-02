#include "XdgDesktopIntegration.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QProcess>
#include <QStandardPaths>
#ifdef Q_OS_LINUX
#include <QTemporaryFile>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mikai::gui::xdg {
namespace {
QString root(const char* variable, const QString& fallback) {
    const QString value = qEnvironmentVariable(variable);
    return QDir::isAbsolutePath(value) ? value : QDir::home().filePath(fallback);
}

QString execArgument(const QString& path) {
    if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null) ||
        path.contains(QLatin1Char('=')) || path.contains(QLatin1Char('\t')) ||
        path.contains(QLatin1Char('\n')) || path.contains(QLatin1Char('\r'))) return {};
    QString result = QStringLiteral("\"");
    for (const QChar c : path) {
        // Desktop Entry string escaping is applied before Exec argument parsing.
        if (c == QLatin1Char('\\')) result += QStringLiteral("\\\\\\\\");
        else if (c == QLatin1Char('"') || c == QLatin1Char('$') || c == QLatin1Char('`')) {
            result += QStringLiteral("\\\\");
            result += c;
        } else if (c == QLatin1Char('%')) result += QStringLiteral("%%");
        else result += c;
    }
    return result + QLatin1Char('"');
}

void refreshMimeCache(Entry entry, const QString& path) {
#ifdef Q_OS_LINUX
    if (entry != Entry::VideoPlayer) return;
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("update-desktop-database"));
    if (!tool.isEmpty()) QProcess::startDetached(tool, {QFileInfo(path).absolutePath()});
#else
    Q_UNUSED(entry);
    Q_UNUSED(path);
#endif
}

bool fail(QString* error, const QString& message, const QString& path) {
    if (error) *error = QCoreApplication::translate("XdgDesktopIntegration", "%1: %2").arg(message, path);
    return false;
}
}

QString entryPath(Entry entry) {
    return entry == Entry::Startup
        ? QDir(root("XDG_CONFIG_HOME", QStringLiteral(".config"))).filePath(QStringLiteral("autostart/mikai.desktop"))
        : QDir(root("XDG_DATA_HOME", QStringLiteral(".local/share"))).filePath(QStringLiteral("applications/mikai.desktop"));
}

QString desktopContents(Entry entry, const QString& executable) {
    const QString exec = execArgument(executable);
    if (exec.isEmpty()) return {};
    QString text = QStringLiteral("[Desktop Entry]\nType=Application\nVersion=1.0\nName=Mikai Ja Nai\nX-Mikai-Managed=true\nX-Mikai-Executable=")
        + QString::fromLatin1(executable.toUtf8().toBase64()) + QStringLiteral("\nExec=") + exec;
    if (entry == Entry::Startup) return text + QStringLiteral("\nTerminal=false\n");
    return text + QStringLiteral(" --open-video %f\nIcon=mikai\nTerminal=false\nCategories=AudioVideo;Video;Player;\nMimeType=video/mp4;video/x-matroska;video/webm;video/quicktime;video/x-msvideo;video/mpeg;video/ogg;\nStartupNotify=true\nStartupWMClass=mikai\n");
}

namespace {
QString retainedContents(const QString& base, bool advertised) {
    QString result = base;
    if (!advertised) {
        result.remove(QStringLiteral("MimeType=video/mp4;video/x-matroska;video/webm;video/quicktime;video/x-msvideo;video/mpeg;video/ogg;\n"));
        result += QStringLiteral("NoDisplay=true\n");
    }
    return result + QStringLiteral("X-Mikai-DefaultRegistration=true\n");
}

// Early retained entries omitted MimeType and NoDisplay but still carried the
// marker. Only this exact legacy-named serialization is owned; the current
// generation always writes NoDisplay for hidden candidates.
QString legacyHiddenContents(const QString& legacyBase) {
    QString result = legacyBase;
    result.remove(QStringLiteral("MimeType=video/mp4;video/x-matroska;video/webm;video/quicktime;video/x-msvideo;video/mpeg;video/ogg;\n"));
    return result + QStringLiteral("X-Mikai-DefaultRegistration=true\n");
}

enum class ManagedEntry { Foreign, Ordinary, Retained, Hidden };

// Current and legacy generated entries are owned only when their complete
// contents are canonical and their executable is current or no longer exists.
ManagedEntry managedEntry(Entry entry, const QByteArray& bytes) {
    const QByteArray key("\nX-Mikai-Executable=");
    const qsizetype start = bytes.indexOf(key);
    if (start < 0) return ManagedEntry::Foreign;
    const qsizetype valueStart = start + key.size();
    const qsizetype end = bytes.indexOf('\n', valueStart);
    if (end < 0) return ManagedEntry::Foreign;
    const QByteArray encoded = bytes.mid(valueStart, end - valueStart);
    const QByteArray decoded = QByteArray::fromBase64(encoded);
    if (decoded.isEmpty() || decoded.toBase64() != encoded) return ManagedEntry::Foreign;
    const QString executable = QString::fromUtf8(decoded);
    if (executable.toUtf8() != decoded) return ManagedEntry::Foreign;
    const QString currentBase = desktopContents(entry, executable);
    if (currentBase.isEmpty()) return ManagedEntry::Foreign;
    QString legacyBase = currentBase;
    legacyBase.replace(QStringLiteral("Name=Mikai Ja Nai\n"),
                       QStringLiteral("Name=Mikai\n"));
#ifdef Q_OS_LINUX
    struct stat status{};
    const QByteArray oldPath = QFile::encodeName(executable);
    if (::lstat(oldPath.constData(), &status) != 0) {
        if (errno != ENOENT) return ManagedEntry::Foreign;
    } else if (QFileInfo(executable).canonicalFilePath() !=
               QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath()) {
        return ManagedEntry::Foreign;
    }
#else
    const QFileInfo oldExecutable(executable);
    if (oldExecutable.exists() || oldExecutable.isSymLink()) {
        if (oldExecutable.canonicalFilePath() !=
            QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath())
            return ManagedEntry::Foreign;
    }
#endif
    for (const QString& base : {currentBase, legacyBase}) {
        if (bytes == base.toUtf8()) return ManagedEntry::Ordinary;
        if (entry != Entry::VideoPlayer) continue;
        if (bytes == retainedContents(base, true).toUtf8()) return ManagedEntry::Retained;
        if (bytes == retainedContents(base, false).toUtf8()) return ManagedEntry::Hidden;
    }
    if (entry == Entry::VideoPlayer &&
        bytes == legacyHiddenContents(legacyBase).toUtf8()) return ManagedEntry::Hidden;
    return ManagedEntry::Foreign;
}

bool packagedEntry(const QByteArray& bytes) {
    // Only recognize the exact desktop entry shipped by this project, and
    // only when its PATH-resolved executable is this running installation.
    const QByteArray packaged =
        "[Desktop Entry]\nType=Application\nVersion=1.0\nName=Mikai Ja Nai\n"
        "GenericName=Video Player\nComment=Watch video with optional Anime4K processing\n"
        "Exec=mikai --open-video %f\nTryExec=mikai\nIcon=mikai\nTerminal=false\n"
        "Categories=AudioVideo;Video;Player;\n"
        "MimeType=video/mp4;video/x-matroska;video/webm;video/quicktime;video/x-msvideo;video/mpeg;video/ogg;\n"
        "StartupNotify=true\nStartupWMClass=mikai\nKeywords=video;player;anime;Anime4K;\n";
    QByteArray legacyPackaged = packaged;
    legacyPackaged.replace("Name=Mikai Ja Nai\n", "Name=Mikai\n");
    if (bytes != packaged && bytes != legacyPackaged) return false;
    const QString executable = QStandardPaths::findExecutable(QStringLiteral("mikai"));
    const QString actual = QFileInfo(executable).canonicalFilePath();
    return !actual.isEmpty() && actual == QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
}

enum class Action { Enable, Disable, RegisterDefault };
bool change(Entry entry, Action action, QString* error) {
    if (error) error->clear();
    const QString path = entryPath(entry);
    const QString expected = desktopContents(entry, QCoreApplication::applicationFilePath());
    if (expected.isEmpty()) return fail(error, QCoreApplication::translate("XdgDesktopIntegration", "The application executable path is unavailable or invalid"), path);
    const QByteArray ordinary = expected.toUtf8();
    const QByteArray retained = retainedContents(expected, true).toUtf8();
    const QByteArray hidden = retainedContents(expected, false).toUtf8();
    QFileInfo info(path);
    if (info.isSymLink()) return fail(error, QCoreApplication::translate("XdgDesktopIntegration", "A symbolic link was left unchanged"), path);
    QByteArray bytes = action == Action::RegisterDefault ? retained : ordinary;
    if (info.exists()) {
        QFile file(path);
        if (!info.isFile() || !file.open(QIODevice::ReadOnly))
            return fail(error, QCoreApplication::translate("XdgDesktopIntegration", "An entry not managed by this installation was left unchanged"), path);
        const QByteArray existing = file.readAll();
        file.close();
        if (entry == Entry::VideoPlayer && packagedEntry(existing)) {
            if (action != Action::Disable) return true;
            return fail(error, QCoreApplication::translate("XdgDesktopIntegration", "This entry belongs to the installed package and cannot be disabled here"), path);
        }
        const ManagedEntry state = existing == ordinary
            ? ManagedEntry::Ordinary
            : managedEntry(entry, existing);
        if (existing != ordinary && state == ManagedEntry::Foreign)
            return fail(error, QCoreApplication::translate("XdgDesktopIntegration", "An entry not managed by this installation was left unchanged"), path);
        const bool hasRetention = state == ManagedEntry::Retained || state == ManagedEntry::Hidden;
        if (action == Action::Disable && !hasRetention) {
            if (!QFile::remove(path)) return fail(error, QCoreApplication::translate("XdgDesktopIntegration", "Could not remove the managed entry"), path);
            refreshMimeCache(entry, path);
            return true;
        }
        if (hasRetention) bytes = action == Action::Disable ? hidden : retained;
        if (existing == bytes) return true;
        QSaveFile replacement(path);
        replacement.setDirectWriteFallback(false);
        if (!replacement.open(QIODevice::WriteOnly) || replacement.write(bytes) != bytes.size() || !replacement.commit())
            return fail(error, QCoreApplication::translate("XdgDesktopIntegration", "Could not update the managed entry"), path);
        refreshMimeCache(entry, path);
        return true;
    }
    if (action == Action::Disable) return true;
    if (!QDir().mkpath(info.absolutePath())) return fail(error, QCoreApplication::translate("XdgDesktopIntegration", "Could not create the integration directory"), info.absolutePath());
#ifdef Q_OS_LINUX
    // Publish a fully written temporary file without ever replacing a file
    // created by another process after the ownership check.
    QTemporaryFile file(info.absolutePath() + QStringLiteral("/.mikai-XXXXXX"));
    if (!file.open() || file.write(bytes) != bytes.size() || !file.flush())
        return fail(error, QCoreApplication::translate("XdgDesktopIntegration", "Could not write the managed entry"), path);
    const QByteArray temporaryPath = QFile::encodeName(file.fileName());
    file.close();
    const QByteArray destinationPath = QFile::encodeName(path);
    if (::link(temporaryPath.constData(), destinationPath.constData()) != 0)
        return fail(error, QCoreApplication::translate("XdgDesktopIntegration", "Could not publish the managed entry; an existing entry was left unchanged"), path);
#else
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(error, QCoreApplication::translate("XdgDesktopIntegration", "Could not write the managed entry"), path);
#endif
    refreshMimeCache(entry, path);
    return true;
}
}  // namespace

bool enabled(Entry entry) {
    const QFileInfo info(entryPath(entry));
    if (info.isSymLink() || !info.isFile()) return false;
    QFile file(info.filePath());
    const QString expected = desktopContents(entry, QCoreApplication::applicationFilePath());
    if (expected.isEmpty() || !file.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = file.readAll();
    if (entry == Entry::VideoPlayer && packagedEntry(bytes)) return true;
    const ManagedEntry state = managedEntry(entry, bytes);
    return state == ManagedEntry::Ordinary || state == ManagedEntry::Retained;
}

bool setEnabled(Entry entry, bool enable, QString* error) {
    return change(entry, enable ? Action::Enable : Action::Disable, error);
}

bool registerDefaultCandidate(QString* error) {
    return change(Entry::VideoPlayer, Action::RegisterDefault, error);
}
}  // namespace mikai::gui::xdg
