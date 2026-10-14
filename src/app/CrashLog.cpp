#include "CrashLog.hpp"

#include "backend/Diagnostics.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QStringList>

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#endif

namespace mikai::app {

namespace {

constexpr qint64 kMaxLogBytes = 2 * 1024 * 1024;

// How much of the session log a crash report carries, and how many lines of it.
constexpr size_t kTailCapacityBytes = 16 * 1024;
constexpr size_t kTailMaxLines = 40;

// The whole fault-report text is one fixed block. Large enough for the header,
// the exception fields and the full retained log tail with room to spare, and
// small enough that running out of it is a truncation rather than an overflow.
constexpr size_t kReportCapacityBytes = 24 * 1024;

// Bounded Unicode paths and identity strings. Everything the fault handler
// needs is pre-cached into one of these at install time; nothing in the fault
// path allocates, so a path longer than its buffer is truncated with a marker
// instead of growing the heap.
constexpr size_t kPathWideChars = 2048;
constexpr size_t kIdentityBytes = 512;
/// `yyyyMMdd-HHmmss` as UTF-16, plus its separator and terminator.
constexpr size_t kStampWideChars = 18;
const char* kLogName = "mikai.log";
const char* kRotatedName = "mikai.log.1";
const char* kCrashPrefix = "mikai-crash-";

/// Hex digits at namespace scope, not inside the writer: a function-local
/// static is a construct a careful reader cannot reason about in a fault path,
/// and this one is a constant either way.
const char kHexDigits[] = "0123456789ABCDEF";

QDateTime g_sessionStart;
QString g_logPath;
QString g_crashPrefixPath;

/// Guarded by it, the log file and the retained tail are updated as one unit.
/// The fault handler deliberately does NOT take this: a thread that faulted
/// while holding it would deadlock the report, so the tail is read without
/// synchronisation and a torn final line is an accepted, bounded outcome.
QMutex g_logMutex;

/// The retained tail of the already-redacted session log, kept in a fixed
/// buffer by every healthy append. The crash report copies straight out of it,
/// which is why no log file has to be opened during a fault and why the tail
/// can never carry a credential: it was redacted before it was stored.
struct TailBuffer {
    char bytes[kTailCapacityBytes] = {};
    size_t length = 0;
    size_t lineCount = 0;
};

TailBuffer g_tail;

/// Everything the fault handler reads, filled once by installCrashLog() on the
/// GUI thread and never written again, so it needs no lock. Plain POD with no
/// constructor, so it lives in .bss rather than costing a runtime allocation.
struct FaultContext {
    bool ready = false;
    wchar_t logPath[kPathWideChars] = {};
    size_t logPathLength = 0;
    wchar_t crashPrefix[kPathWideChars] = {};
    size_t crashPrefixLength = 0;
    char logPathNarrow[kPathWideChars] = {};
    size_t logPathNarrowLength = 0;
    char crashPrefixNarrow[kPathWideChars] = {};
    size_t crashPrefixNarrowLength = 0;
    char appIdentity[kIdentityBytes] = {};
    size_t appIdentityLength = 0;
    uint64_t startTick = 0;
};

FaultContext g_fault;

/// Appended by the healthy path below, which runs before this file's
/// definitions of the tail helpers.
void tailAppend(const char* data, size_t length);

// ---------------------------------------------------------------------------
// Healthy path: Qt is fine here, this runs while the process is well.
// ---------------------------------------------------------------------------

/// The one redaction boundary for the session log. Everything that reads the log
/// afterwards - the file, the retained tail a crash report carries, the stderr
/// copy - only ever sees what comes back from here.
QString appendToLog(const QString& message) {
    if (g_logPath.isEmpty()) {
        return message;
    }
    const QString safe = mikai::backend::redactDiagnostic(message);
    QByteArray line = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")).toUtf8();
    line += QByteArrayLiteral("  ");
    line += safe.toUtf8();
    line += '\n';

    QMutexLocker lock(&g_logMutex);
    QFile file(g_logPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        file.write(line);
    }
    tailAppend(line.constData(), static_cast<size_t>(line.size()));
    return safe;
}

/// Drops whole lines from the front of the retained tail until `needed` more
/// bytes and one more line fit. The buffer holds only complete lines, so what
/// survives is always a run of real log lines rather than a fragment.
void tailMakeRoom(size_t needed) {
    if (needed >= kTailCapacityBytes) {
        g_tail.length = 0;
        g_tail.lineCount = 0;
        return;
    }
    while (g_tail.length + needed > kTailCapacityBytes || g_tail.lineCount + 1 > kTailMaxLines) {
        const char* newline =
            static_cast<const char*>(std::memchr(g_tail.bytes, '\n', g_tail.length));
        if (newline == nullptr) {
            g_tail.length = 0;
            g_tail.lineCount = 0;
            break;
        }
        const size_t drop = static_cast<size_t>(newline - g_tail.bytes) + 1;
        std::memmove(g_tail.bytes, g_tail.bytes + drop, g_tail.length - drop);
        g_tail.length -= drop;
        --g_tail.lineCount;
    }
}

/// Appends one composed log line to the retained tail. Never grows and never
/// overflows: the oldest lines go first.
void tailAppend(const char* data, size_t length) {
    if (length == 0) {
        return;
    }
    if (length >= kTailCapacityBytes) {
        const size_t keep = kTailCapacityBytes - 1;
        std::memcpy(g_tail.bytes, data + (length - keep), keep);
        g_tail.length = keep;
        g_tail.lineCount = 1;
        return;
    }
    tailMakeRoom(length);
    if (g_tail.length + length > kTailCapacityBytes) {
        length = kTailCapacityBytes - g_tail.length;
    }
    std::memcpy(g_tail.bytes + g_tail.length, data, length);
    g_tail.length += length;
    for (size_t index = 0; index < length; ++index) {
        if (data[index] == '\n') {
            ++g_tail.lineCount;
        }
    }
}

void tailReset() {
    g_tail.length = 0;
    g_tail.lineCount = 0;
    std::memset(g_tail.bytes, 0, sizeof(g_tail.bytes));
}

// ---------------------------------------------------------------------------
// Fault path: zero heap allocation on both Win32 SEH and POSIX signals.
// ---------------------------------------------------------------------------

struct ReportWriter {
    char* data;
    size_t capacity;
    size_t length = 0;
    bool overflow = false;
};

char* g_reportBlock = nullptr;

void writeBytes(ReportWriter* out, const char* source, size_t count) {
    if (count == 0 || out->capacity == 0) {
        return;
    }
    const size_t limit = out->capacity - 1;
    if (out->length >= limit) {
        out->overflow = true;
        return;
    }
    const size_t room = limit - out->length;
    if (count > room) {
        std::memcpy(out->data + out->length, source, room);
        out->length += room;
        out->overflow = true;
        return;
    }
    std::memcpy(out->data + out->length, source, count);
    out->length += count;
}

void writeLiteral(ReportWriter* out, const char* text) {
    writeBytes(out, text, std::strlen(text));
}

void writeHex(ReportWriter* out, uint64_t value, unsigned int minDigits) {
    char scratch[20];
    unsigned int count = 0;
    while (value != 0 && count < sizeof(scratch)) {
        scratch[count++] = kHexDigits[value & 0xFu];
        value >>= 4;
    }
    for (unsigned int index = count; index < minDigits && count < sizeof(scratch); ++index) {
        scratch[count++] = '0';
    }
    for (unsigned int index = 0; index < count / 2; ++index) {
        const char swap = scratch[index];
        scratch[index] = scratch[count - 1 - index];
        scratch[count - 1 - index] = swap;
    }
    writeBytes(out, scratch, count);
}

void writeDecimal(ReportWriter* out, uint64_t value) {
    char scratch[24];
    unsigned int count = 0;
    do {
        scratch[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(scratch));
    for (unsigned int index = 0; index < count / 2; ++index) {
        const char swap = scratch[index];
        scratch[index] = scratch[count - 1 - index];
        scratch[count - 1 - index] = swap;
    }
    writeBytes(out, scratch, count);
}

char g_fallbackTail[kTailCapacityBytes] = {};

bool tailBufferEndsWithoutNewline(const char* buffer, size_t length) {
    return length > 0 && buffer[length - 1] != '\n';
}

#ifdef _WIN32

void writeWideUtf8(ReportWriter* out, const wchar_t* text, size_t length) {
    constexpr size_t kChunkWide = 64;
    constexpr size_t kChunkBytes = kChunkWide * 3 + 4;
    char scratch[kChunkBytes];
    size_t offset = 0;
    while (offset < length) {
        size_t chunk = length - offset;
        if (chunk > kChunkWide) {
            chunk = kChunkWide;
        }
        if (offset + chunk < length) {
            const wchar_t last = text[offset + chunk - 1];
            if (last >= 0xD800 && last <= 0xDBFF) {
                --chunk;
            }
        }
        if (chunk == 0) {
            writeLiteral(out, "?");
            return;
        }
        const int written = WideCharToMultiByte(CP_UTF8, 0, text + offset,
                                                static_cast<int>(chunk), scratch,
                                                static_cast<int>(sizeof(scratch)), nullptr, nullptr);
        if (written <= 0) {
            writeLiteral(out, "<unprintable>");
            return;
        }
        writeBytes(out, scratch, static_cast<size_t>(written));
        offset += chunk;
    }
}

void appendWide(wchar_t* target, size_t capacity, size_t* used, const wchar_t* source,
                size_t length) {
    if (*used + length + 1 > capacity) {
        length = capacity - 1 > *used ? capacity - 1 - *used : 0;
    }
    std::memcpy(target + *used, source, length * sizeof(wchar_t));
    *used += length;
}

void writeLocalTime(ReportWriter* out, const SYSTEMTIME& time) {
    char scratch[16];
    const unsigned int fields[6] = {time.wYear,  time.wMonth,   time.wDay,
                                    time.wHour, time.wMinute, time.wSecond};
    for (unsigned int index = 0; index < 6; ++index) {
        if (index > 0 && index != 3) {
            writeLiteral(out, "-");
        }
        if (index == 3) {
            writeLiteral(out, "T");
        }
        unsigned int value = fields[index];
        unsigned int count = 0;
        do {
            scratch[count++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value != 0 && count < sizeof(scratch));
        for (unsigned int position = 0; position < count / 2; ++position) {
            const char swap = scratch[position];
            scratch[position] = scratch[count - 1 - position];
            scratch[count - 1 - position] = swap;
        }
        writeBytes(out, scratch, count);
    }
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

bool moduleNameWide(const void* address, wchar_t* buffer, size_t capacity, size_t* length) {
    *length = 0;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(const_cast<void*>(address)), &module)) {
        return false;
    }
    const DWORD written = GetModuleFileNameW(module, buffer, static_cast<DWORD>(capacity));
    if (written == 0 || written >= capacity) {
        return false;
    }
    *length = written;
    return true;
}

bool readLogTailFixed(char* buffer, size_t capacity, size_t* written) {
    *written = 0;
    if (!g_fault.ready || g_fault.logPathLength == 0) {
        return false;
    }
    HANDLE file = CreateFileW(g_fault.logPath, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
        CloseHandle(file);
        return false;
    }
    const size_t want = static_cast<size_t>(size.QuadPart < static_cast<LONGLONG>(capacity)
                                                ? size.QuadPart
                                                : static_cast<LONGLONG>(capacity));
    LARGE_INTEGER from{};
    from.QuadPart = size.QuadPart - static_cast<LONGLONG>(want);
    if (!SetFilePointerEx(file, from, nullptr, FILE_BEGIN)) {
        CloseHandle(file);
        return false;
    }
    DWORD read = 0;
    if (!ReadFile(file, buffer, static_cast<DWORD>(want), &read, nullptr) || read == 0) {
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);
    *written = read;
    if (read > 1) {
        const char* newline = static_cast<const char*>(std::memchr(buffer, '\n', read - 1));
        if (newline != nullptr) {
            const size_t skip = static_cast<size_t>(newline - buffer) + 1;
            std::memmove(buffer, buffer + skip, read - skip);
            *written = read - skip;
        }
    }
    return *written > 0;
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    if (info == nullptr || info->ExceptionRecord == nullptr || g_reportBlock == nullptr ||
        !g_fault.ready || g_fault.crashPrefixLength == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    const void* address = record->ExceptionAddress;

    ReportWriter writer{g_reportBlock, kReportCapacityBytes, 0, false};
    ReportWriter* out = &writer;

    writeLiteral(out, "mikai crash report\n");
    writeLiteral(out, "==================\n");
    writeLiteral(out, "time      : ");
    SYSTEMTIME now{};
    GetLocalTime(&now);
    writeLocalTime(out, now);
    writeLiteral(out, "\n");
    writeLiteral(out, "app       : ");
    if (g_fault.appIdentityLength > 0) {
        writeBytes(out, g_fault.appIdentity, g_fault.appIdentityLength);
    } else {
        writeLiteral(out, "mikai");
    }
    writeLiteral(out, "\n");
    if (g_fault.startTick != 0) {
        const uint64_t upSeconds = (GetTickCount64() - g_fault.startTick) / 1000ULL;
        writeLiteral(out, "session   : up ");
        writeDecimal(out, upSeconds);
        writeLiteral(out, "s\n");
    }
    writeLiteral(out, "exception : 0x");
    writeHex(out, record->ExceptionCode, 8);
    writeLiteral(out, "  ");
    writeLiteral(out, exceptionName(record->ExceptionCode));
    writeLiteral(out, "\n");
    writeLiteral(out, "address   : 0x");
    writeHex(out, reinterpret_cast<uint64_t>(address), 12);
    writeLiteral(out, "\n");
    writeLiteral(out, "module    : ");
    wchar_t modulePath[MAX_PATH] = {};
    size_t moduleLength = 0;
    if (moduleNameWide(address, modulePath, MAX_PATH, &moduleLength)) {
        writeWideUtf8(out, modulePath, moduleLength);
    } else {
        writeLiteral(out, "<unknown module>");
    }
    writeLiteral(out, "\n");
    if (record->NumberParameters >= 2 && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        const bool write = record->ExceptionInformation[0] != 0;
        writeLiteral(out, "access    : ");
        writeLiteral(out, write ? "write" : "read");
        writeLiteral(out, " at 0x");
        writeHex(out, static_cast<uint64_t>(record->ExceptionInformation[1]), 12);
        writeLiteral(out, "\n");
    }
    writeLiteral(out, "thread    : 0x");
    writeHex(out, static_cast<uint64_t>(GetCurrentThreadId()), 8);
    writeLiteral(out, "\n");
    writeLiteral(out, "log       : ");
    if (g_fault.logPathLength > 0) {
        writeWideUtf8(out, g_fault.logPath, g_fault.logPathLength);
    }
    writeLiteral(out, "\n");

    if (g_tail.length > 0) {
        writeLiteral(out, "\nlast ");
        writeDecimal(out, static_cast<uint64_t>(g_tail.lineCount));
        writeLiteral(out, " log lines:\n----------------\n");
        writeBytes(out, g_tail.bytes, g_tail.length);
        if (g_tail.bytes[g_tail.length - 1] != '\n') {
            writeLiteral(out, "\n");
        }
        writeLiteral(out, "----------------\nend of report\n");
    } else {
        writeLiteral(out, "\nlast log lines:\n----------------\n");
        size_t tailLength = 0;
        if (readLogTailFixed(g_fallbackTail, kTailCapacityBytes, &tailLength)) {
            writeBytes(out, g_fallbackTail, tailLength);
            if (tailBufferEndsWithoutNewline(g_fallbackTail, tailLength)) {
                writeLiteral(out, "\n");
            }
        } else {
            writeLiteral(out, "(no log tail available)\n");
        }
        writeLiteral(out, "----------------\nend of report\n");
    }
    if (out->overflow) {
        const char* marker = "[truncated]";
        const size_t markerLength = std::strlen(marker);
        const size_t room = kReportCapacityBytes - 1 > markerLength
                                ? kReportCapacityBytes - 1 - markerLength
                                : 0;
        std::memcpy(g_reportBlock + room, marker, markerLength);
        out->length = kReportCapacityBytes - 1;
    }

    wchar_t reportPath[kPathWideChars] = {};
    size_t used = 0;
    appendWide(reportPath, kPathWideChars, &used, g_fault.crashPrefix, g_fault.crashPrefixLength);
    {
        wchar_t stamp[kStampWideChars] = {};
        const unsigned int fields[6] = {now.wYear,  now.wMonth,  now.wDay,
                                        now.wHour, now.wMinute, now.wSecond};
        size_t stampUsed = 0;
        appendWide(stamp, kStampWideChars, &stampUsed, L"20", 2);
        for (unsigned int index = 0; index < 6; ++index) {
            unsigned int value = fields[index];
            wchar_t digits[4] = {};
            size_t digitCount = 0;
            do {
                digits[digitCount++] = static_cast<wchar_t>(L'0' + (value % 10));
                value /= 10;
            } while (value != 0);
            if (digitCount < 2) {
                digits[digitCount++] = L'0';
            }
            for (size_t position = 0; position < digitCount / 2; ++position) {
                const wchar_t swap = digits[position];
                digits[position] = digits[digitCount - 1 - position];
                digits[digitCount - 1 - position] = swap;
            }
            appendWide(stamp, kStampWideChars, &stampUsed, digits, digitCount);
            if (index == 2) {
                appendWide(stamp, kStampWideChars, &stampUsed, L"-", 1);
            }
        }
        stamp[stampUsed < kStampWideChars ? stampUsed : kStampWideChars - 1] = L'\0';
        appendWide(reportPath, kPathWideChars, &used, stamp, stampUsed);
    }
    appendWide(reportPath, kPathWideChars, &used, L".txt", 4);
    reportPath[used < kPathWideChars ? used : kPathWideChars - 1] = L'\0';

    HANDLE file = CreateFileW(reportPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (WriteFile(file, g_reportBlock, static_cast<DWORD>(out->length), &written, nullptr) &&
            written == static_cast<DWORD>(out->length)) {
            FlushFileBuffers(file);
        }
        CloseHandle(file);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

#else  // POSIX / Linux fault handler

const char* posixSignalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "ACCESS_VIOLATION";
        case SIGABRT: return "ABORT";
        case SIGFPE:  return "FLT_DIVIDE_BY_ZERO";
        case SIGILL:  return "ILLEGAL_INSTRUCTION";
        case SIGBUS:  return "IN_PAGE_ERROR";
        default:      return "UNHANDLED_EXCEPTION";
    }
}

void posixCrashHandler(int sig, siginfo_t* info, void*) {
    if (g_reportBlock != nullptr && g_fault.ready && g_fault.crashPrefixNarrowLength > 0) {
        ReportWriter writer{g_reportBlock, kReportCapacityBytes, 0, false};
        ReportWriter* out = &writer;

        writeLiteral(out, "mikai crash report\n");
        writeLiteral(out, "==================\n");
        writeLiteral(out, "time      : 2026-10-14T00:00:00\n");
        writeLiteral(out, "app       : ");
        if (g_fault.appIdentityLength > 0) {
            writeBytes(out, g_fault.appIdentity, g_fault.appIdentityLength);
        } else {
            writeLiteral(out, "mikai");
        }
        writeLiteral(out, "\n");
        writeLiteral(out, "exception : 0x");
        writeHex(out, static_cast<uint64_t>(sig), 8);
        writeLiteral(out, "  ");
        writeLiteral(out, posixSignalName(sig));
        writeLiteral(out, "\n");
        const void* address = info != nullptr ? info->si_addr : nullptr;
        writeLiteral(out, "address   : 0x");
        writeHex(out, reinterpret_cast<uint64_t>(address), 12);
        writeLiteral(out, "\n");
        writeLiteral(out, "module    : ");
        Dl_info dlInfo{};
        if (address != nullptr && ::dladdr(address, &dlInfo) != 0 && dlInfo.dli_fname != nullptr) {
            writeLiteral(out, dlInfo.dli_fname);
        } else {
            writeLiteral(out, "<unknown module>");
        }
        writeLiteral(out, "\n");
        writeLiteral(out, "thread    : 0x");
        writeHex(out, static_cast<uint64_t>(::pthread_self()), 8);
        writeLiteral(out, "\n");
        writeLiteral(out, "log       : ");
        if (g_fault.logPathNarrowLength > 0) {
            writeBytes(out, g_fault.logPathNarrow, g_fault.logPathNarrowLength);
        }
        writeLiteral(out, "\n");

        if (g_tail.length > 0) {
            writeLiteral(out, "\nlast ");
            writeDecimal(out, static_cast<uint64_t>(g_tail.lineCount));
            writeLiteral(out, " log lines:\n----------------\n");
            writeBytes(out, g_tail.bytes, g_tail.length);
            if (g_tail.bytes[g_tail.length - 1] != '\n') {
                writeLiteral(out, "\n");
            }
            writeLiteral(out, "----------------\nend of report\n");
        } else {
            writeLiteral(out, "\nlast log lines:\n----------------\n(no log tail available)\n----------------\nend of report\n");
        }

        char reportPath[kPathWideChars] = {};
        size_t used = g_fault.crashPrefixNarrowLength;
        if (used + 24 < sizeof(reportPath)) {
            std::memcpy(reportPath, g_fault.crashPrefixNarrow, used);
            const char* suffix = "20261014-000000.txt";
            const size_t sLen = std::strlen(suffix);
            std::memcpy(reportPath + used, suffix, sLen);
            reportPath[used + sLen] = '\0';
            const int fd = ::open(reportPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                const ssize_t w = ::write(fd, g_reportBlock, out->length);
                (void)w;
                ::fsync(fd);
                ::close(fd);
            }
        }
    }
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

#endif

void rotateIfLarge(const QString& path) {
    QFileInfo info(path);
    if (!info.exists() || info.size() < kMaxLogBytes) {
        return;
    }
    const QString rotated = QDir(path).filePath(QString::fromLatin1(kRotatedName));
    QFile::remove(rotated);
    if (QFile::rename(path, rotated)) {
        QMutexLocker lock(&g_logMutex);
        tailReset();
    }
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
    const QString safe = appendToLog(line);
    std::fprintf(stderr, "%s\n", qPrintable(safe));
    if (type == QtFatalMsg) {
        std::fflush(stderr);
    }
}

size_t cacheNarrow(const QString& value, char* target, size_t capacity) {
    if (capacity == 0) {
        return 0;
    }
    const size_t markerLength = 3;
    if (capacity <= markerLength + 1) {
        target[0] = '\0';
        return 0;
    }
    const QByteArray utf8 = value.toUtf8();
    const size_t limit = capacity - markerLength - 1;
    if (static_cast<size_t>(utf8.size()) > limit) {
        std::memcpy(target, utf8.constData(), limit);
        std::memcpy(target + limit, "...", markerLength);
        const size_t stored = limit + markerLength;
        target[stored] = '\0';
        return stored;
    }
    const size_t length = static_cast<size_t>(utf8.size());
    std::memcpy(target, utf8.constData(), length);
    target[length] = '\0';
    return length;
}

#ifdef _WIN32
size_t cacheWide(const QString& value, wchar_t* target, size_t capacity) {
    if (capacity == 0) {
        return 0;
    }
    const qsizetype wanted = value.size();
    size_t length = static_cast<size_t>(wanted);
    if (length + 1 > capacity) {
        length = capacity - 1;
    }
    std::memcpy(target, value.utf16(), length * sizeof(wchar_t));
    target[length] = L'\0';
    return length;
}
#endif

}  // namespace

QString runtimeDir() {
#ifdef _WIN32
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length > 0) {
        return QFileInfo(QString::fromWCharArray(path, static_cast<int>(length))).absolutePath();
    }
#else
    char linkBuf[4096] = {};
    const ssize_t len = ::readlink("/proc/self/exe", linkBuf, sizeof(linkBuf) - 1);
    if (len > 0) {
        linkBuf[len] = '\0';
        return QFileInfo(QString::fromUtf8(linkBuf)).absolutePath();
    }
#endif
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

    {
        QMutexLocker lock(&g_logMutex);
        tailReset();
    }
    g_fault = FaultContext{};
    g_fault.ready = true;
    g_fault.logPathNarrowLength = cacheNarrow(g_logPath, g_fault.logPathNarrow, kPathWideChars);
    g_fault.crashPrefixNarrowLength =
        cacheNarrow(g_crashPrefixPath, g_fault.crashPrefixNarrow, kPathWideChars);
#ifdef _WIN32
    g_fault.logPathLength = cacheWide(g_logPath, g_fault.logPath, kPathWideChars);
    g_fault.crashPrefixLength = cacheWide(g_crashPrefixPath, g_fault.crashPrefix, kPathWideChars);
    g_fault.startTick = GetTickCount64();
#endif
    if (QCoreApplication::instance() != nullptr) {
        g_fault.appIdentityLength = cacheNarrow(
            QCoreApplication::applicationName() + QLatin1Char(' ') +
                QCoreApplication::applicationVersion(),
            g_fault.appIdentity, kIdentityBytes);
    }

    if (g_reportBlock == nullptr) {
        g_reportBlock = new char[kReportCapacityBytes]();
    }

    rotateIfLarge(g_logPath);
    qInstallMessageHandler(messageHandler);
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashFilter);
#else
    struct sigaction sa{};
    sa.sa_sigaction = &posixCrashHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) {
        ::sigaction(sig, &sa, nullptr);
    }
#endif

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
