#pragma once

#include "backend/Errors.hpp"

#include <QWidget>

class QLabel;
class QPushButton;

namespace mikai::app {

/// One failed call, rendered the same way everywhere.
///
/// The app's failure surfaces used to say only *that* something failed: the
/// backend's stable code and the HTTP status were flattened into the message
/// string at the Runner boundary, so a Cloudflare 522, a rate limit and a
/// cancelled request could all read the same. This widget is where that stops.
/// It shows the line the backend produced, the machine code beside it
/// ("http_error · HTTP 522"), and the retry its owner wires up.
///
/// It hides itself until there is a failure to show, so a host places it where
/// the failure belongs and otherwise forgets about it: the host keeps owning
/// progress and empty states, this owns the failure state.
class FailureView : public QWidget {
    Q_OBJECT

public:
    explicit FailureView(QWidget* parent = nullptr);

    /// Show `failure`, with `context` as the leading clause. An empty context
    /// shows the backend's own line alone.
    void showFailure(const backend::Failure& failure, const QString& context = {});

    /// Hide again and keep nothing.
    void clear();

    /// Hide the built-in retry when the host already offers one: the updates and
    /// schedule rows carry their own always-visible Refresh action, so a second
    /// retry beside it would be noise.
    void setRetryVisible(bool visible);

    /// The code chip's text, empty when there is no code to name. Exposed so a
    /// test can assert that a 522 says 522.
    QString codeText() const;

    /// The line as rendered, for the same reason.
    QString messageText() const;

Q_SIGNALS:
    /// The viewer asked for the failed operation again. The owner re-runs it;
    /// this widget never knows what the operation was.
    void retryRequested();

private:
    QLabel* message_ = nullptr;
    QLabel* code_ = nullptr;
    QPushButton* retry_ = nullptr;
};

}  // namespace mikai::app
