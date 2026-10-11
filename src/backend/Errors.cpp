#include "Errors.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace mikai::backend {

bool isHttpStatusError(int status) { return status >= 400; }

bool parseApiErrorEnvelope(const QByteArray& body, int status, ApiErrorEnvelope* out) {
    if (out == nullptr) {
        return false;
    }
    if (body.isEmpty()) {
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    const QJsonObject envelope = document.object();
    if (envelope.value(QStringLiteral("ok")).toBool()) {
        // A success envelope is not a failure, however the caller asked.
        return false;
    }
    // An envelope states its own shape: either the API said `ok: false` or it
    // attached an error object. Anything else is a JSON body that happens to be
    // valid - a proxy's status page, a truncated object - and calling it a named
    // API failure would invent a code the server never sent.
    const bool statedFailure = envelope.contains(QStringLiteral("ok")) ||
                               envelope.contains(QStringLiteral("error"));
    if (!statedFailure) {
        return false;
    }
    const QJsonObject error = envelope.value(QStringLiteral("error")).toObject();
    const QJsonValue code = error.value(QStringLiteral("code"));
    const QJsonValue message = error.value(QStringLiteral("message"));
    out->status = status;
    out->code = code.isString() && !code.toString().isEmpty() ? code.toString()
                                                              : QStringLiteral("unknown");
    out->message = message.isString() && !message.toString().isEmpty()
                       ? message.toString()
                       : QStringLiteral("request failed");
    return true;
}

}  // namespace mikai::backend
