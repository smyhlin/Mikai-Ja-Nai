#include "NativeFilterCommand.hpp"

#include "NativeDiagnostics.hpp"

namespace mplayer {
namespace native {

namespace {

// mpv's own option-name alphabet (upstream `options/m_option.c`, NAMECH). A name
// outside it would itself have to be escaped, and this producer does not guess
// at an escaped name: it refuses one by name instead.
bool isOptionNameChar(QChar c) {
  return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
         (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
         (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
         c == QLatin1Char('_') || c == QLatin1Char('-');
}

}  // namespace

QByteArray nativeFilterSpec(const QVector<NativeFilterOption>& options, QString* reasonKey,
                            QString* reason) {
  const auto refuse = [reasonKey, reason](const QString& key, const QString& text) {
    if (reasonKey != nullptr) *reasonKey = key;
    // The same boundary the other native writers use: a hostile name that
    // happens to look like a URL or a credential is redacted, never echoed.
    if (reason != nullptr) *reason = redactNativeDiagnosticText(text);
    return QByteArray();
  };

  QByteArray spec;
  // One allocation for the whole spec. A UTF-16 unit is at most three UTF-8
  // bytes (a surrogate pair is four for two units), so this is an upper bound
  // rather than a second encoding pass.
  qsizetype bound = 32;
  for (const NativeFilterOption& option : options) {
    bound += 8 + option.name.size() + option.value.size() * 3;
  }
  spec.reserve(bound);

  spec.append('@');
  spec.append(kNativeStatusLabel);
  spec.append(':');
  spec.append(kNativeFilterName);

  for (qsizetype index = 0; index < options.size(); ++index) {
    const NativeFilterOption& option = options.at(index);
    if (option.name.isEmpty()) {
      return refuse(QStringLiteral("native.filter.optionName"),
                    QStringLiteral("a native filter option has no name"));
    }
    for (const QChar c : option.name) {
      if (!isOptionNameChar(c)) {
        return refuse(QStringLiteral("native.filter.optionName"),
                      QStringLiteral("native filter option '%1' is not a plain mpv option name")
                          .arg(option.name));
      }
    }

    // The value is quoted as its exact UTF-8 bytes; the count is what the core
    // consumes, so nothing inside the value is ever examined by the parser.
    const QByteArray value = option.value.toUtf8();
    if (value.contains('\0')) {
      // A U+0000 survives this length-delimited buffer but not the C string the
      // core takes the argument through, which would truncate the option there.
      // Refuse by name; never hand the core a silently shortened value.
      return refuse(QStringLiteral("native.filter.value.embeddedNul"),
                    QStringLiteral("native filter option '%1' carries an embedded NUL")
                        .arg(option.name));
    }

    spec.append(index == 0 ? '=' : ':');
    spec.append(option.name.toLatin1());
    spec.append('=');
    spec.append('%');
    spec.append(QByteArray::number(value.size()));
    spec.append('%');
    spec.append(value);
  }

  if (reasonKey != nullptr) reasonKey->clear();
  if (reason != nullptr) reason->clear();
  return spec;
}

}  // namespace native
}  // namespace mplayer
