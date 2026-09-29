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
void installCrashLog();

/// Writes one line to the session log. Safe to call from anywhere on the GUI
/// thread; it is the same path the Qt message handler takes.
void writeLogLine(const QString& line);

}  // namespace mikai::app
