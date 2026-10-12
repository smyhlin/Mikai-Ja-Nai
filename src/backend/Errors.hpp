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

// One failed call, as a surface that has to explain it needs the failure: the
// stable code the backend branched on, the HTTP status when the failure reached
// one, and the line to show. The code and the status are what a report quotes,
// so they travel with the message instead of being flattened into it.
struct Failure {
    QString code;
    QString message;
    int status = 0;
};

// The machine token shown beside the message: the API's own code, plus the HTTP
// status when there was one - "http_error · HTTP 522", "rate_limited · HTTP 429",
// "network_error". Deliberately not translated: it is the part of the line that
// has to survive a screenshot. Empty when there is nothing to name, in which
// case the surface shows the message alone.
QString failureCodeLabel(const Failure& failure);

// The message with its code appended when there is one, which is what a status
// line shows: "Операція скасована · network_error", "Сервер не відповів · HTTP 522".
// The message alone when there is no code to name.
QString failureLine(const Failure& failure);

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
