#include "CrashLog.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTextStream>

#include <cstdio>

#include <windows.h>

namespace mikai::app {

namespace {

constexpr qint64 kMaxLogBytes = 2 * 1024 * 1024;
constexpr int kTailBytes = 16 * 1024;
constexpr int kTailLines = 40;
const char* kLogName = "mikai.log";
const char* kRotatedName = "mikai.log.1";
const char* kCrashPrefix = "mikai-crash-";

QDateTime g_sessionStart;
QString g_logPath;
QString g_crashPrefixPath;

QString crashStamp() {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
}

/// Appends a line to the session log. Stays on plain QFile: this is called from
/// the Qt message handler, where throwing is not an option.
void appendToLog(const QString& line) {
    if (g_logPath.isEmpty()) {
        return;
    }
    QFile file(g_logPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream out(&file);
    out << QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")) << "  " << line
        << "\n";
}

/// The last few dozen lines of the session log, so a crash report carries the
/// run that produced it. Read with Win32 rather than Qt: this is called from an
/// unhandled-exception filter, where the C++ heap may already be corrupt and
/// allocating again is how a crash becomes a hang.
QString readLogTail() {
    if (g_logPath.isEmpty()) {
        return {};
    }
    HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(g_logPath.utf16()), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart == 0) {
        CloseHandle(file);
        return {};
    }
    const LONG longSize = static_cast<LONG>(size.QuadPart);
    const DWORD want = static_cast<DWORD>(
        size.QuadPart < kTailBytes ? size.QuadPart : static_cast<LONGLONG>(kTailBytes));
    SetFilePointer(file, longSize - static_cast<LONG>(want), nullptr, FILE_BEGIN);
    QByteArray buffer;
    buffer.resize(static_cast<int>(want));
    DWORD read = 0;
    ReadFile(file, buffer.data(), want, &read, nullptr);
    CloseHandle(file);
    buffer.resize(static_cast<int>(read));

    QString text = QString::fromUtf8(buffer);
    const QStringList lines = text.split(QLatin1Char('\n'));
    const int from = qMax(0, lines.size() - kTailLines);
    return lines.mid(from).join(QLatin1Char('\n'));
}

QString moduleNameForAddress(const void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(const_cast<void*>(address)), &module)) {
        return QStringLiteral("<unknown module>");
    }
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    return length > 0 ? QString::fromWCharArray(path, static_cast<int>(length))
                      : QStringLiteral("<unnamed module>");
}

const char* exceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
            return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:
            return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_IN_PAGE_ERROR:
            return "IN_PAGE_ERROR";
        case EXCEPTION_PRIV_INSTRUCTION:
            return "PRIV_INSTRUCTION";
        default:
            return "UNHANDLED_EXCEPTION";
    }
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    if (info != nullptr && info->ExceptionRecord != nullptr) {
        const EXCEPTION_RECORD* record = info->ExceptionRecord;
        const void* address = record->ExceptionAddress;
        const QString reportPath =
            g_crashPrefixPath + crashStamp() + QStringLiteral(".txt");

        QString text;
        text += QStringLiteral("mikai crash report\n");
        text += QStringLiteral("==================\n");
        text += QStringLiteral("time      : ") +
                QDateTime::currentDateTime().toString(Qt::ISODate) + QLatin1Char('\n');
        if (QCoreApplication::instance() != nullptr) {
            text += QStringLiteral("app       : ") + QCoreApplication::applicationName() +
                    QLatin1Char(' ') + QCoreApplication::applicationVersion() + QLatin1Char('\n');
        } else {
            text += QStringLiteral("app       : mikai\n");
        }
        if (g_sessionStart.isValid()) {
            text += QStringLiteral("session   : started ") +
                    g_sessionStart.toString(QStringLiteral("HH:mm:ss")) + QStringLiteral(", up ") +
                    QString::number(g_sessionStart.secsTo(QDateTime::currentDateTime())) +
                    QStringLiteral("s\n");
        }
        text += QStringLiteral("exception : 0x") +
                QString::number(static_cast<quint32>(record->ExceptionCode), 16).toUpper() +
                QStringLiteral("  ") + QString::fromLatin1(exceptionName(record->ExceptionCode)) +
                QLatin1Char('\n');
        text += QStringLiteral("address   : ") + QStringLiteral("0x") +
                QString::number(reinterpret_cast<quintptr>(address), 16).toUpper() + QLatin1Char('\n');
        text += QStringLiteral("module    : ") + moduleNameForAddress(address) + QLatin1Char('\n');
        if (record->NumberParameters >= 2 &&
            record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            const bool write = record->ExceptionInformation[0] != 0;
            text += QStringLiteral("access    : ") + (write ? QStringLiteral("write")
                                                           : QStringLiteral("read")) +
                    QStringLiteral(" at 0x") +
                    QString::number(reinterpret_cast<quintptr>(record->ExceptionInformation[1]), 16)
                        .toUpper() +
                    QLatin1Char('\n');
        }
        text += QStringLiteral("thread    : 0x") +
                QString::number(static_cast<quintptr>(GetCurrentThreadId()), 16).toUpper() +
                QLatin1Char('\n');
        text += QStringLiteral("log       : ") + g_logPath + QLatin1Char('\n');
        text += QStringLiteral("\nlast ") + QString::number(kTailLines) +
                QStringLiteral(" log lines:\n----------------\n");
        text += readLogTail();
        text += QStringLiteral("\n----------------\nend of report\n");

        HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(reportPath.utf16()), GENERIC_WRITE, 0,
                                  nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            const QByteArray utf8 = text.toUtf8();
            DWORD written = 0;
            WriteFile(file, utf8.constData(), static_cast<DWORD>(utf8.size()), &written, nullptr);
            CloseHandle(file);
        }
    }
    // Let the OS have it: swallowing a crash would leave a dead process with no
    // dialog and no report path anyone could find.
    return EXCEPTION_CONTINUE_SEARCH;
}

void rotateIfLarge(const QString& path) {
    QFileInfo info(path);
    if (!info.exists() || info.size() < kMaxLogBytes) {
        return;
    }
    const QString rotated = QDir(path).filePath(QString::fromLatin1(kRotatedName));
    QFile::remove(rotated);
    QFile::rename(path, rotated);
}

void reportPreviousCrashes(const QString& dir) {
    const QStringList crashes =
        QDir(dir).entryList({QString::fromLatin1(kCrashPrefix) + QStringLiteral("*.txt")},
                            QDir::Files, QDir::Time);
    if (crashes.isEmpty()) {
        return;
    }
    for (const QString& name : crashes) {
        appendToLog(QStringLiteral("WARNING: a previous session left a crash report: %1").arg(name));
    }
}

void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const char* level = "INFO";
    switch (type) {
        case QtDebugMsg:
            level = "DEBUG";
            break;
        case QtInfoMsg:
            level = "INFO";
            break;
        case QtWarningMsg:
            level = "WARN";
            break;
        case QtCriticalMsg:
            level = "CRIT";
            break;
        case QtFatalMsg:
            level = "FATAL";
            break;
    }
    QString line = QStringLiteral("[%1] %2").arg(QLatin1String(level), message);
    if (context.file != nullptr) {
        line += QStringLiteral("  (%1:%2)").arg(QLatin1String(context.file)).arg(context.line);
    }
    appendToLog(line);
    std::fprintf(stderr, "%s\n", qPrintable(line));
    if (type == QtFatalMsg) {
        std::fflush(stderr);
    }
}

}  // namespace

QString runtimeDir() {
    // The exe's own folder, read from the loader. Not QDir::currentPath() -
    // launching from a shortcut or a file manager leaves the cwd somewhere else
    // entirely, and a crash report in the wrong directory is a report nobody
    // finds. Not QCoreApplication::applicationDirPath() either, because this has
    // to work before the application object exists.
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length > 0) {
        return QFileInfo(QString::fromWCharArray(path, static_cast<int>(length))).absolutePath();
    }
    if (QCoreApplication::instance() != nullptr) {
        return QCoreApplication::applicationDirPath();
    }
    return QDir::currentPath();
}

QString logPath() { return g_logPath; }

void writeLogLine(const QString& line) { appendToLog(line); }

void installCrashLog() {
    const QString dir = runtimeDir();
    g_logPath = QDir(dir).filePath(QString::fromLatin1(kLogName));
    g_crashPrefixPath = QDir(dir).filePath(QString::fromLatin1(kCrashPrefix));
    g_sessionStart = QDateTime::currentDateTime();

    rotateIfLarge(g_logPath);
    qInstallMessageHandler(messageHandler);
    SetUnhandledExceptionFilter(crashFilter);

    appendToLog(QStringLiteral("---- mikai started ----"));
    if (QCoreApplication::instance() != nullptr) {
        appendToLog(QStringLiteral("version  : %1 %2")
                        .arg(QCoreApplication::applicationName(),
                             QCoreApplication::applicationVersion()));
    }
    appendToLog(QStringLiteral("folder   : %1").arg(dir));
    reportPreviousCrashes(dir);
}

}  // namespace mikai::app
