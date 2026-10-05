// The one place that decides whether a diagnostic string might carry a URL or a
// credential, and the one place that removes one.
//
// Status snapshots and artifact manifests are filled from sources this process
// does not own: the filter publishes the snapshot, and a manifest can be read
// back from a cache that another process, build or hand-edit wrote. None of
// those strings may reach a log, an exported document or a user-facing message
// verbatim, because any of them can carry a signed download URL, an API key, a
// bearer token or a query parameter that names one.
//
// `containsSensitiveNativeText` is the predicate the validators use to refuse
// such a document by name. `redactNativeDiagnosticText` is the boundary the
// writers use: it returns the text unchanged when nothing is sensitive, and
// otherwise replaces every sensitive run with a fixed marker. A URL is removed
// whole - never only its query token - so no partial query leaks.
//
// The behaviour is deliberately conservative and cheap. It is not a credential
// scanner: it recognises a short, fixed set of shapes (an absolute URI, an
// `mk_` API key, an `X-API-Key` header, an `Authorization: Bearer` value and a
// query parameter that names a token or secret). Ordinary text is never
// touched: Unicode, Windows drive paths such as `C:\models\rtx.onnx`, commas
// and percent signs pass through exactly as written.

#pragma once

#include <QtCore/QString>

namespace mplayer {
namespace native {

/// True when `text` carries a URL-shaped run or one of the credential
/// indicators named above. Such text MUST NOT cross a diagnostic boundary.
bool containsSensitiveNativeText(const QString& text);

/// `text` with every sensitive run replaced by a fixed `<redacted>` marker. A
/// URL is redacted whole, query string included, so a partial query can never
/// survive. Text with nothing sensitive is returned unchanged.
QString redactNativeDiagnosticText(const QString& text);

}  // namespace native
}  // namespace mplayer
