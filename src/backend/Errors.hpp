#pragma once

#include <QByteArray>
#include <QString>

namespace mikai::backend {

// MikaiApiError mirrors mikai_backend/errors.py. The API answers every failure
// with {ok: false, error: {code, message}}, and callers branch on `code`
// rather than on the status, so the three fields are the whole contract.
// It is thrown by every MikaiClient call and caught as
// `catch (const MikaiApiError&)`.
struct MikaiApiError {
    QString code;
    QString message;
    int status = 0;
};

// The failure envelope, decoded without throwing and without depending on the
// client that fetched the bytes. `MikaiClient` classifies every HTTP failure
// through this first, so a 403/404/422 server answer keeps its documented code
// and message instead of collapsing into a transport error. A transport failure
// has no envelope to decode, which is exactly what the bool reports.
struct ApiErrorEnvelope {
    QString code;
    QString message;
    int status = 0;
};

// Decodes `body` as a {ok: false, error: {code, message}} envelope. The
// `status` is recorded verbatim so the caller knows what the server answered.
// Returns false when the body is not that envelope at all - transport garbage,
// an HTML error page, a proxy response - in which case `out` is left alone and
// the caller stays on the transport-error path. A decoded envelope always
// fills the three fields: a missing code becomes "unknown" and a missing
// message becomes "request failed", so a consumer never branches on an empty
// string.
bool parseApiErrorEnvelope(const QByteArray& body, int status, ApiErrorEnvelope* out);

/// True for every status that is not a success: 4xx and 5xx. A Qt reply error
/// with no status at all is not an HTTP status error.
bool isHttpStatusError(int status);

}  // namespace mikai::backend
