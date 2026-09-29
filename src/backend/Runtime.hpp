#pragma once

#include <QDir>
#include <QString>
#include <QStringList>

#include <optional>

// Runtime discovery mirrors mikai_backend/mpv_runtime.py: find a folder that
// holds a usable libmpv, either the one the app provisioned earlier or one
// shipped next to the executable. On Windows x64 it can also download the
// latest mpv development archive.
namespace mikai::backend {

QDir cacheDir();
QStringList libraryNames();

// First directory under `folder` (recursively) that contains a supported
// libmpv library, or std::nullopt.
std::optional<QDir> findRuntime(const QString& hint = {});

// Same search, but throws std::runtime_error when nothing is found and
// autoDownload is false. With autoDownload it fetches the runtime first.
QDir ensureRuntime(const QString& hint = {}, bool autoDownload = true);

}  // namespace mikai::backend
