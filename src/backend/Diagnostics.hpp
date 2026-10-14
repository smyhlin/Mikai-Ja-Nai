// The one place that decides whether a diagnostic string might carry a remote
// credential, and the one place that removes one.
//
// Every string this process persists - the session log, a crash report, a
// forwarded media/backend log line - arrives from a source this process does
// not own: libmpv echoes the URLs it was handed, a transport reports the
// request line it failed on, and a filter quotes the command it was asked to
// run. Any of those can carry a signed download URL (its token lives in the
// path as well as the query), an API key or an `Authorization: Bearer` value.
// `AGENTS.md` records the boundary: those tokens never enter a log, a manifest or
// an exported diagnostic. This unit is what enforces it, so the enforcement is
// written once instead of once per caller.
//
// QtCore only, and deliberately so: the crash logger links this from an
// unhandled-exception filter's neighbourhood and the domain layer is scanned by
// the layering tests, so this may not grow a GUI or widget dependency.
//
// The behaviour is deliberate and cheap rather than a credential scanner:
//
//   * a remote URL is rewritten down to `scheme://authority`. Userinfo, path,
//     query and fragment are all dropped, which is the difference between
//     redacting a query token and redacting the signed path that carries it.
//     The host survives, so a report still names the endpoint that failed;
//   * a credential name/value pair keeps its name and loses its value, so
//     `X-API-Key: <redacted>` still says which credential was refused;
//   * `Bearer <token>` and the project's `mk_…` key shape lose the credential
//     and keep nothing but the marker;
//   * a scheme-less signed run (`/hls/7428/signed/ab12/index.m3u8?token=…`)
//     is redacted whole: a path-shaped run that carries a credential parameter
//     is removed, never just the parameter, so no partial run can leak.
//
// Ordinary text is never touched: Unicode, Windows drive paths such as
// `C:\models\rtx.onnx`, commas and percent signs pass through exactly as
// written, and a long hostile field cannot make the matchers backtrack because
// every pattern is a bounded prefix plus one negated character class.

#pragma once

#include <QtCore/QChar>
#include <QtCore/QRegularExpression>
#include <QtCore/QRegularExpressionMatch>
#include <QtCore/QRegularExpressionMatchIterator>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <algorithm>

namespace mikai::backend {

/// `text` with every sensitive run replaced by a fixed `<redacted>` marker.
/// Text carrying nothing sensitive is returned unchanged, byte for byte, so an
/// ordinary message is never damaged by the boundary that is supposed to
/// protect it.
inline QString redactDiagnostic(const QString& text);

namespace redaction_detail {

/// The one marker every redaction leaves behind. A fixed token rather than a
/// blank or a truncated value: it says something was removed without saying
/// anything about what.
inline const QString& redactionMarker() {
    static const QString marker = QStringLiteral("<redacted>");
    return marker;
}

/// A half-open range in the string being inspected. `qsizetype` matches
/// QString's own index type, so no narrowing happens on the way in.
struct Span {
    qsizetype start = 0;
    qsizetype length = 0;
};

/// Where a URL-ish run ends, for the purpose of expanding a credential
/// parameter to the whole run it sits in. Whitespace and the quotes, brackets
/// and separators that surround an embedded URL bound it, matching the shapes a
/// log line or a JSON document actually uses.
inline bool isRunBoundary(QChar character) {
    if (character.isSpace()) return true;
    static const QString boundaries = QStringLiteral("\"'<>()[]{},;|\\");
    return boundaries.contains(character);
}

/// A remote URL. The scheme list is an allow-list rather than any word: a
/// scheme this process never speaks to (a `file://` path, a Windows drive
/// letter) is not a remote credential, and matching it would only remove
/// information a reader needs. `://` carries the colon the cheap pre-check
/// below looks for, so this pattern is never reached without one.
inline const QRegularExpression& urlRunPattern() {
    static const QRegularExpression pattern(QStringLiteral(
        "(?i)\\b(?:https?|ftps?|sftp|rtmps?[ept]?|rtsps?u?|wss?|davs?|smb|ipfs|"
        "magnet)://[^\\s\"'<>]*"));
    return pattern;
}

/// A credential name, its separator and its value, as one match. The value is
/// its own capture group, so the name and separator in front of it survive the
/// redaction. `(?:bearer\s+)?` folds `Authorization: Bearer <token>` into a
/// single erased value, so the two words do not become two markers.
inline const QRegularExpression& credentialPairPattern() {
    static const QRegularExpression pattern(QStringLiteral(
        "(?i)\\b(?:x-api-key|x-api-token|api-key|apikey|api_key|secret-key|"
        "client-secret|client_secret|auth-token|session-token|access-token|"
        "access_key|refresh_token|id-token|id_token|proxy-authorization|"
        "authorization|accesskey|secret|token|credentials|credential|password|"
        "passwd|pass phrase|pwd|signature|sig|auth|jwt|key|session)"
        "\\b[\"']?(\\s*[:=]\\s*)(?:bearer\\s+)?(\"[^\"]*\"|[^\\s,;]+)"));
    return pattern;
}

/// `Bearer <token>` on its own, with no header name in front of it. The token
/// is at least eight URL-safe characters: a bare English "bearer" with nothing
/// after it is not a credential, and neither is a prose sentence that happens
/// to contain the word.
inline const QRegularExpression& bearerPattern() {
    static const QRegularExpression pattern(
        QStringLiteral("(?i)\\bbearer\\s+[A-Za-z0-9._~+/=\\-]{8,}"));
    return pattern;
}

/// The project's own API-key shape: `mk_` followed by a key-sized run. The same
/// value also reaches the credential-pair pattern when it is written as
/// `X-API-Key: mk_…`, and both erase the same span.
inline const QRegularExpression& apiKeyPattern() {
    static const QRegularExpression pattern(
        QStringLiteral("\\bmk_[A-Za-z0-9][A-Za-z0-9_.\\-]{6,}"));
    return pattern;
}

/// The cheap gate in front of the matchers. Every shape this unit recognises
/// needs either a separator (`scheme://`, `name: value`, `name=value`) or one
/// of the two spellings that can appear with neither, so a line with none of
/// them cannot match anything and is returned without a single regex call.
/// That is what keeps a routine mpv chatter line from paying for this at all.
inline bool looksSensitive(const QString& text) {
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        if (character == QLatin1Char(':') || character == QLatin1Char('=') ||
            character == QLatin1Char('/')) {
            return true;
        }
    }
    return text.contains(QStringLiteral("mk_"), Qt::CaseInsensitive) ||
           text.contains(QStringLiteral("earer"), Qt::CaseInsensitive);
}

/// `scheme://authority` plus the marker: everything after the authority - the
/// userinfo in front of the host and the path, query and fragment behind it -
/// is what a signed URL uses to prove a request, so all of it goes and the
/// host stays. A run that is nothing but a scheme and an authority is already
/// the whole answer and is returned untouched rather than marked.
inline QString urlAuthorityReplacement(const QString& run) {
    const qsizetype schemeEnd = run.indexOf(QLatin1String("://"));
    if (schemeEnd < 0) return run;
    const qsizetype authorityStart = schemeEnd + 3;

    qsizetype authorityEnd = run.size();
    for (qsizetype index = authorityStart; index < run.size(); ++index) {
        const QChar character = run.at(index);
        if (character == QLatin1Char('/') || character == QLatin1Char('?') ||
            character == QLatin1Char('#')) {
            authorityEnd = index;
            break;
        }
    }
    qsizetype userinfoEnd = -1;
    for (qsizetype index = authorityStart; index < authorityEnd; ++index) {
        if (run.at(index) == QLatin1Char('@')) userinfoEnd = index;
    }
    if (userinfoEnd < 0 && authorityEnd == run.size()) return run;

    const qsizetype hostStart = userinfoEnd < 0 ? authorityStart : userinfoEnd + 1;
    QString replacement = run.left(authorityStart);
    replacement += run.mid(hostStart, authorityEnd - hostStart);
    replacement += redactionMarker();
    return replacement;
}

/// Rewrites every remote URL run in `text` down to its authority. Text with no
/// remote URL is returned unchanged, and the writer behind it is not touched.
inline QString redactUrlRuns(const QString& text) {
    QRegularExpressionMatchIterator it = urlRunPattern().globalMatch(text);
    if (!it.hasNext()) return text;
    QString out;
    out.reserve(text.size());
    qsizetype cursor = 0;
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const qsizetype start = match.capturedStart();
        const qsizetype end = match.capturedEnd();
        if (end <= start || start < cursor) continue;
        out += text.mid(cursor, start - cursor);
        out += urlAuthorityReplacement(match.captured());
        cursor = end;
    }
    out += text.mid(cursor);
    return out;
}

/// Every occurrence of `pattern` in `text`, recorded as the span of capture
/// group `group` (0 for the whole match) and nothing else.
inline void collectSpans(const QRegularExpression& pattern, const QString& text, int group,
                         QVector<Span>* spans) {
    QRegularExpressionMatchIterator it = pattern.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const qsizetype length = match.capturedLength(group);
        if (length > 0) {
            spans->append(Span{match.capturedStart(group), length});
        }
    }
}

/// A credential parameter inside a path-shaped run is never removed on its
/// own: the run around it goes too, so a scheme-less signed path cannot survive
/// as the one part of a URL that was left alone. Runs that carry no `/` are
/// ordinary prose around a value and are left to the pair pattern, which keeps
/// the name and only erases the value.
inline void expandPathShapedRuns(const QString& text, QVector<Span>* spans) {
    for (Span& span : *spans) {
        qsizetype start = span.start;
        qsizetype end = span.start + span.length;
        while (start > 0 && !isRunBoundary(text.at(start - 1))) --start;
        while (end < text.size() && !isRunBoundary(text.at(end))) ++end;
        if (end <= start) continue;
        bool pathShaped = false;
        for (qsizetype index = start; index < end; ++index) {
            if (text.at(index) == QLatin1Char('/')) {
                pathShaped = true;
                break;
            }
        }
        if (pathShaped) {
            span.start = start;
            span.length = end - start;
        }
    }
}

/// Collects every credential run. Returns true when at least one was found, so
/// text that only needed its URLs rewritten is returned without a second pass
/// over its spans.
inline bool collectCredentialSpans(const QString& text, QVector<Span>* spans) {
    // Group 2 is the value; the name and the separator in front of it are what
    // makes the pair readable and are deliberately kept.
    collectSpans(credentialPairPattern(), text, 2, spans);
    collectSpans(bearerPattern(), text, 0, spans);
    collectSpans(apiKeyPattern(), text, 0, spans);
    expandPathShapedRuns(text, spans);
    return !spans->isEmpty();
}

/// Replaces the merged spans with the marker, leaving everything between them
/// byte for byte. Overlapping spans collapse into the first one to reach them,
/// so an `Authorization: Bearer` header and its token cannot leave a gap
/// between two redactions.
inline QString redactSpans(const QString& text, QVector<Span> spans) {
    std::sort(spans.begin(), spans.end(),
              [](const Span& a, const Span& b) { return a.start < b.start; });
    QString out;
    out.reserve(text.size());
    qsizetype cursor = 0;
    for (const Span& span : spans) {
        const qsizetype end = span.start + span.length;
        if (end <= cursor) continue;
        const qsizetype start = std::max(span.start, cursor);
        out += text.mid(cursor, start - cursor);
        out += redactionMarker();
        cursor = end;
    }
    out += text.mid(cursor);
    return out;
}

}  // namespace redaction_detail

inline QString redactDiagnostic(const QString& text) {
    if (text.isEmpty() || !redaction_detail::looksSensitive(text)) return text;
    const QString withoutUrls = redaction_detail::redactUrlRuns(text);
    QVector<redaction_detail::Span> spans;
    if (redaction_detail::collectCredentialSpans(withoutUrls, &spans)) {
        return redaction_detail::redactSpans(withoutUrls, spans);
    }
    return withoutUrls;
}

}  // namespace mikai::backend
