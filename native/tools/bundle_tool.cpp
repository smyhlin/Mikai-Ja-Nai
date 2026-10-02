// mikai_bundle_tool - validate, describe, list and install runtime bundles.
//
// Every subcommand is a file-level instrument: none loads a module or starts a
// core, so a passing `verify` says only that the manifest matches the bytes and
// the closure stays inside the bundle apart from the Windows system exceptions.
// Loadability and filter registration stay the loader's business.

#include "native/NativeBundle.hpp"

#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTextStream>

namespace {

using namespace mplayer;

void describeBundle(QTextStream& out, const native::Bundle& bundle) {
    out << "  kind             " << native::bundleKindName(bundle.kind) << '\n'
        << "  version          " << bundle.version << '\n'
        << "  descriptorId     " << bundle.descriptorId << '\n'
        << "  root             " << bundle.root << '\n'
        << "  module           " << bundle.modulePath << '\n'
        << "  files            " << bundle.files.size() << '\n'
        << "  mpv client API   "
        << (bundle.mpvApiVersion == 0 ? QStringLiteral("learned at load")
                                      : QString::number(bundle.mpvApiVersion))
        << '\n'
        << "  protocolVersion  " << bundle.protocolVersion << '\n'
        << "  engineSchema     " << bundle.engineSchemaVersion << '\n';
    for (const QString& dep : bundle.deps) out << "  dep              " << dep << '\n';
}

int verify(const QString& path, QTextStream& out, QTextStream& err) {
    QString reason;
    const std::optional<native::Bundle> bundle = native::readBundle(path, &reason);
    if (!bundle.has_value()) {
        err << "rejected: " << path << ": " << reason << '\n';
        return 2;
    }
    if (!native::validateBundle(*bundle, &reason)) {
        err << "rejected: " << path << ": " << reason << '\n';
        return 2;
    }
    out << "accepted: " << path << '\n';
    describeBundle(out, *bundle);
    return 0;
}

int describe(const QString& modulePath, QTextStream& out, QTextStream& err) {
    QString reason;
    const std::optional<native::Bundle> bundle = native::describeStandardRuntime(modulePath, &reason);
    if (!bundle.has_value()) {
        err << "rejected: " << modulePath << ": " << reason << '\n';
        return 2;
    }
    out << "described: " << modulePath << '\n';
    describeBundle(out, *bundle);
    return 0;
}

int list(const QStringList& roots, QTextStream& out, QTextStream& err) {
    QStringList rejections;
    const QList<native::Bundle> bundles = native::discoverBundles(roots, &rejections);
    for (const native::Bundle& bundle : bundles) {
        out << "found: " << bundle.root << '\n';
        describeBundle(out, bundle);
    }
    for (const QString& rejection : rejections) err << "rejected: " << rejection << '\n';
    if (bundles.isEmpty() && rejections.isEmpty()) out << "no bundle found under the given roots\n";
    return 0;
}

int install(const QString& source, const QString& runtimeRoot, QTextStream& out, QTextStream& err) {
    QString reason;
    const std::optional<native::Bundle> bundle = native::readBundle(source, &reason);
    if (!bundle.has_value()) {
        err << "rejected: " << source << ": " << reason << '\n';
        return 2;
    }
    if (!native::installAtomic(*bundle, runtimeRoot, &reason)) {
        err << "install failed: " << reason << '\n';
        return 3;
    }
    out << "installed: " << bundle->root << " into " << runtimeRoot << '\n';
    out << "  published as " << native::bundleDirectoryName(bundle->kind, bundle->version) << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mikai_bundle_tool"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Validate, describe, list and install mikai runtime bundles. No subcommand "
                       "loads a module."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("command"),
                                 QStringLiteral("verify | describe | list | install"));
    parser.addPositionalArgument(QStringLiteral("path"),
                                 QStringLiteral("bundle root, or module path for 'describe'"));
    parser.addOption({QStringLiteral("root"), QStringLiteral("Runtime root for 'install', or a "
                                                            "search root for 'list' (repeatable)"),
                       QStringLiteral("directory")});
    parser.process(application);

    QTextStream out(stdout);
    QTextStream err(stderr);
    const QStringList arguments = parser.positionalArguments();
    if (arguments.isEmpty()) {
        err << "a command is required\n";
        return 2;
    }
    const QString command = arguments.first();
    const QString path = arguments.size() > 1 ? arguments.at(1) : QString{};
    const QStringList roots = parser.values(QStringLiteral("root"));
    const QString runtimeRoot =
        roots.isEmpty() ? native::runtimeStorageRoot() : roots.first();

    int rc = 2;
    if (command == QLatin1String("verify")) {
        if (path.isEmpty()) {
            err << "verify needs a bundle root\n";
        } else {
            rc = verify(QDir(path).absolutePath(), out, err);
        }
    } else if (command == QLatin1String("describe")) {
        if (path.isEmpty()) {
            err << "describe needs a module path\n";
        } else {
            rc = describe(QFileInfo(path).absoluteFilePath(), out, err);
        }
    } else if (command == QLatin1String("list")) {
        rc = list(roots, out, err);
    } else if (command == QLatin1String("install")) {
        if (path.isEmpty()) {
            err << "install needs a bundle root\n";
        } else {
            rc = install(QDir(path).absolutePath(), runtimeRoot, out, err);
        }
    } else {
        err << "unknown command '" << command << "'\n";
    }

    out.flush();
    err.flush();
    if (rc == 0)
        out << "This is file-level validation only: not module loadability, not native filter "
               "registration.\n";
    return rc;
}