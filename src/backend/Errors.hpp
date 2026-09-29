#pragma once

#include <QString>

// MikaiApiError mirrors mikai_backend/errors.py. The API answers every failure
// with {ok: false, error: {code, message}}, and callers branch on `code`
// rather than on the status, so the three fields are the whole contract.
// It is thrown by every MikaiClient call and caught as
// `catch (const MikaiApiError&)`.
namespace mikai::backend {

struct MikaiApiError {
    QString code;
    QString message;
    int status = 0;
};

}  // namespace mikai::backend
