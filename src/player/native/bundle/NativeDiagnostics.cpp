#include "NativeDiagnostics.hpp"

#include <algorithm>

#include <QtCore/QRegularExpression>
#include <QtCore/QRegularExpressionMatch>
#include <QtCore/QRegularExpressionMatchIterator>
#include <QtCore/QVector>

namespace mplayer {
namespace native {

namespace {

/// The one marker every redaction leaves behind. A fixed token rather than a
/// blank or a truncated value: it says something was removed without saying
/// anything about what.
const QString& redactionMarker() {
  static const QString marker = QStringLiteral("<redacted>");
  return marker;
}

/// A half-open range in the string being inspected. `qsizetype` matches
/// QString's own index type, so no narrowing happens on the way in.
struct Span {
  qsizetype start = 0;
  qsizetype length = 0;
};

/// An absolute URI: a short scheme, `://`, then everything up to the next
/// whitespace or quote. Bounding the scheme and using one negated class keeps
/// the matcher linear, so a long hostile field cannot make it backtrack.
const QRegularExpression& urlRunPattern() {
  static const QRegularExpression pattern(
      QStringLiteral("[A-Za-z][A-Za-z0-9+.\\-]{1,31}://[^\\s\"'<>]*"));
  return pattern;
}

/// An API-key header name, with its value when one is attached. `[:=]` is
/// required before a value is consumed, so the bare words in a sentence are not
/// swallowed along with the following prose.
const QRegularExpression& headerPattern() {
  static const QRegularExpression pattern(
      QStringLiteral("(?i)\\b(?:x-api-key|api-key|apikey)\\b(?:\\s*[:=]\\s*[^\\s,;]+)?"));
  return pattern;
}

/// `Authorization: Bearer <token>`. The token is at least eight URL-safe
/// characters; a bare English "bearer" with no credential after it is not a
/// secret.
const QRegularExpression& bearerPattern() {
  static const QRegularExpression pattern(
      QStringLiteral("(?i)\\bbearer\\s+[A-Za-z0-9._~+/=\\-]{8,}"));
  return pattern;
}

/// The project's own API-key shape: `mk_` followed by a key-sized run.
const QRegularExpression& apiKeyPattern() {
  static const QRegularExpression pattern(QStringLiteral("\\bmk_[A-Za-z0-9][A-Za-z0-9_\\-]{6,}"));
  return pattern;
}

/// A query parameter that names a credential. The key needs word boundaries so
/// "tokenizer=..." is not read as a token, and `\s*=\s*` tolerates the spacing
/// a hand-written URL might carry.
const QRegularExpression& queryParameterPattern() {
  static const QRegularExpression pattern(QStringLiteral(
      "(?i)\\b(?:access_token|refresh_token|id_token|token|api_?key|apikey|secret|"
      "client_secret|password|passwd|pwd|signature|sig|auth|session)\\s*=\\s*[^\\s]+"));
  return pattern;
}

/// Where a URL-ish run ends for the purpose of expanding a query parameter to
/// the whole run. Quotes, brackets and separators bound it alongside whitespace.
bool isRunBoundary(QChar character) {
  if (character.isSpace()) return true;
  static const QString boundaries = QStringLiteral("\"'<>()[]{},;");
  return boundaries.contains(character);
}

void collectMatches(const QRegularExpression& pattern, const QString& text, QVector<Span>* spans) {
  QRegularExpressionMatchIterator it = pattern.globalMatch(text);
  while (it.hasNext()) {
    const QRegularExpressionMatch match = it.next();
    if (match.capturedLength() > 0) {
      spans->append(Span{match.capturedStart(), match.capturedLength()});
    }
  }
}

/// A credential query parameter is never removed on its own: the whole
/// whitespace-delimited run around it (its URL, if it is one) is removed, so no
/// partial query can leak past the redaction.
void collectQuerySpans(const QString& text, QVector<Span>* spans) {
  QRegularExpressionMatchIterator it = queryParameterPattern().globalMatch(text);
  while (it.hasNext()) {
    const QRegularExpressionMatch match = it.next();
    qsizetype start = match.capturedStart();
    qsizetype end = match.capturedEnd();
    while (start > 0 && !isRunBoundary(text.at(start - 1))) --start;
    while (end < text.size() && !isRunBoundary(text.at(end))) ++end;
    if (end > start) spans->append(Span{start, end - start});
  }
}

/// Collects every sensitive run. Returns true when at least one was found.
bool collectSensitiveSpans(const QString& text, QVector<Span>* spans) {
  collectMatches(urlRunPattern(), text, spans);
  collectMatches(headerPattern(), text, spans);
  collectMatches(bearerPattern(), text, spans);
  collectMatches(apiKeyPattern(), text, spans);
  collectQuerySpans(text, spans);
  return !spans->isEmpty();
}

/// Replaces the merged spans with the marker, leaving everything between them
/// byte-for-byte.
QString redactSpans(const QString& text, QVector<Span> spans) {
  std::sort(spans.begin(), spans.end(),
            [](const Span& a, const Span& b) { return a.start < b.start; });
  QString out;
  out.reserve(text.size());
  qsizetype cursor = 0;
  for (const Span& span : spans) {
    const qsizetype end = span.start + span.length;
    if (end <= cursor) continue;  // Already covered by an earlier span.
    const qsizetype start = std::max(span.start, cursor);
    out += text.mid(cursor, start - cursor);
    out += redactionMarker();
    cursor = end;
  }
  out += text.mid(cursor);
  return out;
}

}  // namespace

bool containsSensitiveNativeText(const QString& text) {
  QVector<Span> spans;
  return collectSensitiveSpans(text, &spans);
}

QString redactNativeDiagnosticText(const QString& text) {
  QVector<Span> spans;
  if (!collectSensitiveSpans(text, &spans)) return text;
  return redactSpans(text, spans);
}

}  // namespace native
}  // namespace mplayer
