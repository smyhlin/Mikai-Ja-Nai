#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <memory>

namespace mplayer {
namespace probes {

/// A loopback HTTP origin that answers slowly on purpose.
///
/// The only way to observe a real "opening" state is a source that takes a
/// while to produce its first byte, and the public internet is not a dependency
/// a probe may take. This serves one file from disk over 127.0.0.1 after a
/// fixed delay: bounded, local, and honest about what it is - network latency
/// on a real socket. Nothing here reaches into the player; the player sees an
/// ordinary http URL.
///
/// Extracted from InteractionProbe's hover-motion case so the load-transition
/// probe measures the same origin the opening state was measured against,
/// instead of a second fixture with different semantics.
class DelayedHttpSource : public QObject {
public:
    explicit DelayedHttpSource(QObject* parent = nullptr) : QObject(parent) {
        clock_.start();
        if (!server_.listen(QHostAddress::LocalHost, 0)) return;
        connect(&server_, &QTcpServer::newConnection, this, [this] { acceptPending(); });
    }

    bool serve(const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return false;
        payload_ = file.readAll();
        return !payload_.isEmpty();
    }

    /// How long a request is held before the first response byte is written.
    void setDelayMs(int delayMs) { delayMs_ = std::max(0, delayMs); }
    int delayMs() const { return delayMs_; }

    QString url(const QString& name = QStringLiteral("clip.mp4")) const {
        if (!server_.isListening()) return QString();
        return QStringLiteral("http://127.0.0.1:%1/%2").arg(server_.serverPort()).arg(name);
    }

    int connections() const { return connections_; }

    /// When the first response was actually written, in milliseconds from this
    /// origin's construction, or -1 while nothing has been answered yet. This is
    /// the boundary an "opening" claim has to be on the right side of.
    qint64 replyAtMs() const { return replyAtMs_; }

private:
    void acceptPending() {
        while (QTcpSocket* socket = server_.nextPendingConnection()) {
            ++connections_;
            const auto buffer = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                buffer->append(socket->readAll());
                if (buffer->indexOf("\r\n\r\n") < 0) return;
                const QByteArray head = buffer->left(buffer->indexOf("\r\n\r\n"));
                startResponse(socket, head);
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

    void startResponse(QTcpSocket* socket, const QByteArray& head) {
        // A demuxer probes with Range; answering 200 to one is a stream it
        // cannot seek, which is a different open path than the one measured.
        qint64 offset = 0;
        const int rangeAt = head.toLower().indexOf("range:");
        if (rangeAt >= 0) {
            const int equals = head.indexOf('=', rangeAt);
            const int dash = head.indexOf('-', equals + 1);
            if (equals > 0 && dash > equals) {
                offset = head.mid(equals + 1, dash - equals - 1).trimmed().toLongLong();
            }
        }
        offset = std::clamp<qint64>(offset, 0, payload_.size());
        const qint64 total = payload_.size();
        const QByteArray body = payload_.mid(static_cast<int>(offset));
        QTimer::singleShot(delayMs_, socket, [this, socket, offset, total, body] {
            if (socket->state() != QAbstractSocket::ConnectedState) return;
            // The first answer this origin ever gives: the instant a claim about
            // "before the source responded" is measured against.
            if (replyAtMs_ < 0) replyAtMs_ = clock_.elapsed();
            QByteArray response;
            response += offset > 0 ? "HTTP/1.1 206 Partial Content\r\n"
                                   : "HTTP/1.1 200 OK\r\n";
            response += "Content-Type: video/mp4\r\nAccept-Ranges: bytes\r\n";
            response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
            if (offset > 0) {
                response += "Content-Range: bytes " + QByteArray::number(offset) + "-" +
                            QByteArray::number(total - 1) + "/" + QByteArray::number(total) +
                            "\r\n";
            }
            response += "Connection: close\r\n\r\n";
            socket->write(response);
            socket->write(body);
            socket->disconnectFromHost();
        });
    }

    QTcpServer server_;
    QByteArray payload_;
    QElapsedTimer clock_;
    qint64 replyAtMs_ = -1;
    int delayMs_ = 2400;
    int connections_ = 0;
};

}  // namespace probes
}  // namespace mplayer
