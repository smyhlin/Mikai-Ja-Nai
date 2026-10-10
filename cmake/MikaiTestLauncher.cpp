// CTest owns this process; only its unique executable directory is disposable.
// Production nativeRoot() stays executable-local, with no test environment hook.
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTemporaryDir>

#include <cstdio>

namespace {

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

int stageAndRun(const QStringList& arguments, const QString& directory) {
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
    std::fflush(stderr);
    child.start(executable, arguments.mid(7));
    if (!child.waitForStarted()) {
        std::fprintf(stderr, "[mikai_test_launcher] cannot start tests: %s\n",
                     qPrintable(child.errorString()));
        return 2;
    }
    if (!child.waitForFinished(-1) && child.state() != QProcess::NotRunning) {
        child.kill();
        child.waitForFinished(-1);
        std::fprintf(stderr, "[mikai_test_launcher] cannot wait for tests: %s\n",
                     qPrintable(child.errorString()));
        return 2;
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
                     "<qt-runtime-bin> <qt-plugins> <scratch-parent> [suite/QTest args...]\n");
        return 2;
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
    const int result = stageAndRun(arguments, scratch.path());
    // Remove only the directory QTemporaryDir created, even after fixture/test
    // failure or a child crash. The public build output is never moved/edited.
    if (!scratch.remove()) {
        std::fprintf(stderr, "[mikai_test_launcher] cannot remove owned scratch: %s\n",
                     qPrintable(scratch.path()));
        return result != 0 ? result : 2;
    }
    return result;
}
