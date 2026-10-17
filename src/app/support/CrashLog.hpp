#pragma once

#include <QString>

namespace mikai::app {

/// The folder the running executable lives in. Everything the crash flow writes
/// goes here, so a report is found next to the app rather than somewhere the
/// user has to be told to look.
QString runtimeDir();

/// Absolute path of the rolling session log.
QString logPath();

/// Installs the message handler and the unhandled-exception filter. Call this
/// first thing in main(), before anything that could fail gets a chance to.
///
/// Three things follow from one call:
///   * every qDebug/qInfo/qWarning/qCritical is appended to `mikai.log`, which
///     rotates once it passes 2 MB so it cannot grow forever;
///   * an unhandled exception (an access violation, a stack overflow) writes
///     `mikai-crash-<timestamp>.txt` beside the exe, carrying the exception
///     code, the faulting address, the module it landed in, and the tail of the
///     session log so the crash has context rather than just a code;
///   * a session that starts while a previous crash report is still on disk
///     says so in the log, so a silent death is never invisible.
///
/// The fault handler itself is Win32-only from the moment the exception arrives:
/// it reads paths and identity that were pre-cached here and writes a fixed
/// block with CreateFileW/WriteFile, so it touches no Qt object, no
/// QString/QByteArray, no heap allocation, no logging mutex, no dynamic UTF-8
/// buffer and no CRT formatting. The report is written without depending on a
/// working allocator, which is what a corrupt heap cannot survive. It is still
/// only what an unhandled-exception filter can be: it does not claim to catch
/// OS fast-fail, RaiseFailFastException or a crash raised while a debugger is
/// attached, and a heap already too damaged to reach the filter produces no
/// report at all.
void installCrashLog();

/// Writes one line to the session log. Safe to call from any thread; it is the
/// same path the Qt message handler takes.
///
/// The one boundary where a received line is redacted: a remote URL loses its
/// userinfo, path, query and fragment, and a credential loses its value, before
/// anything is written to disk. The log tail a crash report carries is therefore
/// already redacted and needs no second pass.
void writeLogLine(const QString& line);

}  // namespace mikai::app
