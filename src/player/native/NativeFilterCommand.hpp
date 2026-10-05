// The one producer of the named native filter's `vf` value:
//
//   @mikai_native:animejanai=<name>=%<utf8-byte-count>%<value>[:<name>=...]
//
// mpv parses a filter list with its own escaping (upstream `options/m_option.c`,
// `read_subparam`/`append_param`): after `=` a value is `%`, a decimal byte
// count, `%`, then exactly that many literal bytes. The count is over UTF-8
// bytes, so one multi-byte character is several; because the parser consumes the
// counted bytes without examining them, a drive colon, a comma, a percent, an
// `=`, whitespace and an empty value all survive unchanged. A value is never URI
// percent-encoded and never interpolated into a shell string.
//
// Two invariants are enforced here rather than left to the parser:
//   * every value is quoted, so an empty value stays an explicit assignment
//     instead of becoming a positional or flag form;
//   * an invalid option name or a value carrying U+0000 is refused by name,
//     because the core takes the argument as a C string and would otherwise
//     truncate it silently at that boundary.
//
// The count is decimal and never zero-padded: mpv reads it with `strtoll(..., 0)`,
// so a padded `%08%` would be read as octal and refuse the whole spec.
//
// QtCore only: this target is built without the mpv client header. The returned
// bytes are the spec exactly as handed to `vf add`, never decoded back.

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QVector>

namespace mplayer {
namespace native {

/// The filter's label and its registered name. Both are owned here because this
/// is the only QtCore-only place the producer and the `vf`-reading diagnostics
/// can share them from; `NativeStatus.hpp` takes the label from this header
/// rather than repeating the string beside the mpv handle it needs.
inline constexpr const char* kNativeStatusLabel = "mikai_native";
inline constexpr const char* kNativeFilterName = "animejanai";

/// One sub-option of the native filter as the caller writes it: the filter's own
/// option name (`model-dir`, `slot`, ...) and the raw value, unencoded. Order is
/// kept, because it is the order the core reads.
struct NativeFilterOption {
  QString name;
  QString value;
};

/// The complete `vf` value: `@mikai_native:animejanai` plus one quoted
/// assignment per option. Empty only on refusal, in which case `reasonKey` and
/// `reason` name the fact; both are cleared on success. A refusal never leaves a
/// partially built spec behind.
QByteArray nativeFilterSpec(const QVector<NativeFilterOption>& options,
                            QString* reasonKey = nullptr, QString* reason = nullptr);

}  // namespace native
}  // namespace mplayer
