// M02 hermetic HTTP/HLS fixture server.
//
// One process, loopback only, no outbound network and no API key. It serves the
// real generated media under tests/fixtures/media and tests/fixtures/hls so
// every later playback, HLS and expiry-recovery proof has a decodable source
// that never touches a CDN.
//
// The endpoint list, the recorded header facts and the signed-URL failure and
// recovery contract are documented in tests/fixtures/README.md.

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>

#include <cstdio>

namespace {

constexpr int kMaxHeaderBytes = 64 * 1024;

// A fixed fixture constant, not a secret: it exists so a wrong or stale
// signature fails validation for a real reason while staying reproducible.
constexpr char kSigSalt[] = "mikai-fixture-hls-v1";

struct RequestFact {
    int seq = 0;
    QString method;
    QString path;
    QStringList queryKeys;
    int status = 0;
    bool acceptLanguagePresent = false;
    QString acceptLanguage;
    bool acceptLanguageCommaFree = false;
    bool userAgentPresent = false;
    bool rangePresent = false;
    QString rangeSpec;
    qint64 bytesServed = 0;

    QJsonObject toJson() const {
        QJsonObject acceptLanguageObject;
        acceptLanguageObject.insert(QStringLiteral("present"), acceptLanguagePresent);
        acceptLanguageObject.insert(QStringLiteral("value"), acceptLanguage);
        acceptLanguageObject.insert(QStringLiteral("commaFree"), acceptLanguageCommaFree);

        QJsonArray keys;
        for (const QString& key : queryKeys) {
            keys.append(key);
        }

        QJsonObject object;
        object.insert(QStringLiteral("seq"), seq);
        object.insert(QStringLiteral("method"), method);
        object.insert(QStringLiteral("path"), path);
        object.insert(QStringLiteral("queryKeys"), keys);
        object.insert(QStringLiteral("status"), status);
        object.insert(QStringLiteral("acceptLanguage"), acceptLanguageObject);
        object.insert(QStringLiteral("userAgentPresent"), userAgentPresent);
        object.insert(QStringLiteral("rangePresent"), rangePresent);
        object.insert(QStringLiteral("range"), rangeSpec);
        object.insert(QStringLiteral("bytesServed"), static_cast<double>(bytesServed));
        return object;
    }
};

struct Rung {
    QString name;   // v180 / v360 / v720, the on-disk directory
    int width = 0;
    int height = 0; // the RESOLUTION the app's quality ladder keys on
    QString codec;
};

const QList<Rung> &rungs() {
    static const QList<Rung> list = {
        {QStringLiteral("v180"), 320, 180, QStringLiteral("avc1.42C01E")},
        {QStringLiteral("v360"), 640, 360, QStringLiteral("avc1.4D401E")},
        {QStringLiteral("v720"), 1280, 720, QStringLiteral("hvc1.2.4.L120.B0")},
    };
    return list;
}

QString signatureFor(const QString& rung, qint64 expiresAt) {
    const QString payload = QString::fromLatin1(kSigSalt) + QLatin1Char('|') + rung +
                            QLatin1Char('|') + QString::number(expiresAt);
    const QByteArray digest =
        QCryptographicHash::hash(payload.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    return QString::fromLatin1(digest);
}

QString httpDate(qint64 msecsSinceEpoch) {
    return QDateTime::fromMSecsSinceEpoch(msecsSinceEpoch, QTimeZone::UTC)
        .toString(QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
}

QString mimeFor(const QString& path) {
    if (path.endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive)) {
        return QStringLiteral("application/vnd.apple.mpegurl");
    }
    if (path.endsWith(QStringLiteral(".ts"), Qt::CaseInsensitive)) {
        return QStringLiteral("video/mp2t");
    }
    if (path.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive)) {
        return QStringLiteral("video/mp4");
    }
    if (path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
        return QStringLiteral("application/json");
    }
    return QStringLiteral("application/octet-stream");
}

QString reasonFor(int status) {
    switch (status) {
        case 200: return QStringLiteral("OK");
        case 206: return QStringLiteral("Partial Content");
        case 400: return QStringLiteral("Bad Request");
        case 403: return QStringLiteral("Forbidden");
        case 404: return QStringLiteral("Not Found");
        case 405: return QStringLiteral("Method Not Allowed");
        case 416: return QStringLiteral("Range Not Satisfiable");
        case 431: return QStringLiteral("Request Header Fields Too Large");
        default: return QStringLiteral("Unknown");
    }
}

struct ByteRange {
    qint64 first = 0;
    qint64 last = 0;
};

// Handles the three single-range forms a media client actually sends:
// "bytes=a-b", open-ended "bytes=a-" and suffix "bytes=-n". A multi-range spec
// is deliberately not honoured, so the caller answers the whole entity.
bool parseByteRange(const QString& header, qint64 size, ByteRange* out) {
    const QString spec = header.trimmed();
    if (!spec.startsWith(QStringLiteral("bytes="), Qt::CaseInsensitive)) {
        return false;
    }
    const QStringList parts = spec.mid(6).split(QLatin1Char(','));
    if (parts.size() != 1) {
        return false;
    }
    const QStringList bounds = parts.first().split(QLatin1Char('-'));
    if (bounds.size() != 2) {
        return false;
    }
    const QString firstText = bounds.at(0).trimmed();
    const QString lastText = bounds.at(1).trimmed();

    if (firstText.isEmpty()) {
        bool ok = false;
        const qint64 requested = lastText.toLongLong(&ok);
        if (!ok || requested <= 0) {
            return false;
        }
        const qint64 clamped = qMin(requested, size);
        if (clamped <= 0) {
            return false;
        }
        out->first = size - clamped;
        out->last = size - 1;
        return true;
    }

    bool okFirst = false;
    const qint64 first = firstText.toLongLong(&okFirst);
    if (!okFirst || first < 0 || first >= size) {
        return false;
    }
    // An open-ended "bytes=100-" runs to the end of the entity.
    if (lastText.isEmpty()) {
        out->first = first;
        out->last = size - 1;
        return true;
    }
    bool okLast = false;
    const qint64 last = lastText.toLongLong(&okLast);
    if (!okLast || last < first) {
        return false;
    }
    out->first = first;
    out->last = qMin(last, size - 1);
    return true;
}

class FixtureServer : public QObject {
public:
    FixtureServer(QString mediaDir, QString hlsDir, QString factsPath, QString portFile,
                  bool requireAcceptLanguage, QObject* parent = nullptr)
        : QObject(parent),
          mediaDir_(std::move(mediaDir)),
          hlsDir_(std::move(hlsDir)),
          factsPath_(std::move(factsPath)),
          portFile_(std::move(portFile)),
          requireAcceptLanguage_(requireAcceptLanguage) {}

    bool start(quint16 port, QString* error) {
        if (!QFileInfo(mediaDir_).isDir()) {
            *error = QStringLiteral("media directory not found: %1").arg(mediaDir_);
            return false;
        }
        if (!QFileInfo(hlsDir_).isDir()) {
            *error = QStringLiteral("hls directory not found: %1").arg(hlsDir_);
            return false;
        }
        for (const Rung& rung : rungs()) {
            if (!QFileInfo(QDir(hlsDir_).filePath(rung.name)).isDir()) {
                *error = QStringLiteral("hls rung directory not found: %1").arg(rung.name);
                return false;
            }
        }
        primaryClip_ = QDir(mediaDir_).filePath(primaryClipName());
        if (!QFileInfo(primaryClip_).isFile()) {
            *error = QStringLiteral("primary clip not found: %1").arg(primaryClip_);
            return false;
        }

        server_ = new QTcpServer(this);
        connect(server_, &QTcpServer::newConnection, this, &FixtureServer::onNewConnection);
        if (!server_->listen(QHostAddress::LocalHost, port)) {
            *error = QStringLiteral("listen failed: %1").arg(server_->errorString());
            return false;
        }
        return true;
    }

    quint16 port() const { return server_->serverPort(); }

    void writePortFile() const {
        if (portFile_.isEmpty()) {
            return;
        }
        QFile file(portFile_);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QByteArray::number(server_->serverPort()));
        }
    }

private:
    static QString primaryClipName() {
        return QStringLiteral("motion_320x180_24fps_8bit_hevc.mp4");
    }

    void onNewConnection() {
        while (QTcpSocket* socket = server_->nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            socket->setProperty("pending", QByteArray());
        }
    }

    // The header may arrive across several reads, so it is buffered until the
    // blank line. One request per connection: the response always closes the
    // socket, so no request body is ever drained and no blocking wait is needed.
    void onReadyRead(QTcpSocket* socket) {
        QByteArray buffer = socket->property("pending").toByteArray() + socket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            if (buffer.size() > kMaxHeaderBytes) {
                sendUnrouted(socket, 431, QStringLiteral("Request Header Fields Too Large"),
                             QByteArray(), QStringLiteral("text/plain"));
                return;
            }
            socket->setProperty("pending", buffer);
            return;
        }
        socket->setProperty("pending", QByteArray());
        handleRequest(socket, buffer.left(headerEnd));
    }

    void handleRequest(QTcpSocket* socket, const QByteArray& head) {
        const QList<QByteArray> lines = head.split('\n');
        const QList<QByteArray> requestLine =
            lines.isEmpty() ? QList<QByteArray>() : lines.first().trimmed().split(' ');
        if (requestLine.size() < 3) {
            sendUnrouted(socket, 400, QStringLiteral("Bad Request"), QByteArray(),
                         QStringLiteral("text/plain"));
            return;
        }

        const QString method = QString::fromLatin1(requestLine.at(0));
        const QUrl target = QUrl::fromEncoded(requestLine.at(1));
        const bool isHead = method == QStringLiteral("HEAD");
        const bool isGet = method == QStringLiteral("GET");

        // A non GET/HEAD request is refused without reading its body; closing
        // the connection discards it.
        if (!isGet && !isHead) {
            respond(socket, 405, QByteArray(), QStringLiteral("text/plain"), QByteArray(),
                    RequestFact(), isHead);
            return;
        }

        RequestFact fact;
        ++seq_;
        fact.seq = seq_;
        fact.method = method;
        fact.path = target.path();
        const QUrlQuery query(target);
        for (const auto& item : query.queryItems(QUrl::FullyDecoded)) {
            fact.queryKeys.append(item.first);
        }

        QMap<QString, QString> headers;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray rawLine = lines.at(i).trimmed();
            const int colon = rawLine.indexOf(':');
            if (rawLine.isEmpty() || colon <= 0) {
                continue;
            }
            headers.insert(QString::fromLatin1(rawLine.left(colon)).trimmed().toLower(),
                           QString::fromLatin1(rawLine.mid(colon + 1)).trimmed());
        }
        const QString acceptLanguage = headers.value(QStringLiteral("accept-language"));
        fact.acceptLanguagePresent = !acceptLanguage.isEmpty();
        fact.acceptLanguage = acceptLanguage;
        fact.acceptLanguageCommaFree =
            fact.acceptLanguagePresent && !acceptLanguage.contains(QLatin1Char(','));
        fact.userAgentPresent = headers.contains(QStringLiteral("user-agent"));
        fact.rangePresent = headers.contains(QStringLiteral("range"));
        fact.rangeSpec = headers.value(QStringLiteral("range"));

        // A CDN rejects the header's presence, so that is the only thing
        // optionally enforced. Comma-freeness is recorded rather than enforced:
        // the Qt extraction path legitimately sends a multi-entry value while
        // mpv's http-header-fields needs the single comma-free form, so
        // enforcing both would lock out one of the two real clients.
        if (requireAcceptLanguage_ && !fact.acceptLanguagePresent) {
            respondText(socket, 400, QStringLiteral("missing Accept-Language"),
                        QStringLiteral("application/json"), isHead, fact);
            return;
        }

        const QString path = target.path();
        if (path == QStringLiteral("/media.mp4")) {
            // Fetching the primary clip starts a new client session, so the
            // retry clip's 404-once contract is re-armed here. Without this the
            // latch would live for the whole fixture process: on the second and
            // later sessions of a long-lived fixture /retry.mp4 would answer
            // with the bytes where the documented contract promises a 404,
            // which is per-process state leaking across tests.
            retryServed_ = false;
            serveFile(socket, primaryClip_, isHead, fact);
            return;
        }
        if (path == QStringLiteral("/retry.mp4")) {
            if (!retryServed_) {
                retryServed_ = true;
                respondText(socket, 404, QStringLiteral("first request fails by contract"),
                            QStringLiteral("application/json"), isHead, fact);
                return;
            }
            // The same file object as /media.mp4, so the bytes cannot differ.
            serveFile(socket, primaryClip_, isHead, fact);
            return;
        }
        if (path.startsWith(QStringLiteral("/media/"))) {
            const QString name = path.mid(7);
            if (name.isEmpty() || name.contains(QLatin1Char('/')) ||
                name.contains(QLatin1String("..")) || name.contains(QLatin1Char('\\'))) {
                sendUnrouted(socket, 404, QStringLiteral("Not Found"), QByteArray(),
                             QStringLiteral("text/plain"));
                return;
            }
            serveFile(socket, QDir(mediaDir_).filePath(name), isHead, fact);
            return;
        }
        if (path == QStringLiteral("/hls/master.m3u8")) {
            const bool fresh =
                query.queryItemValue(QStringLiteral("fresh")) == QStringLiteral("1");
            respondText(socket, 200, masterPlaylist(fresh), mimeFor(path), isHead, fact);
            return;
        }
        if (path.startsWith(QStringLiteral("/hls/"))) {
            serveHls(socket, target, isHead, fact);
            return;
        }
        if (path == QStringLiteral("/manifest.json")) {
            respondText(socket, 200, manifest(), mimeFor(path), isHead, fact);
            return;
        }
        if (path == QStringLiteral("/_facts")) {
            respondText(socket, 200, jsonText(factsJson()), mimeFor(path), isHead, fact);
            return;
        }
        if (path == QStringLiteral("/_facts/summary")) {
            respondText(socket, 200, jsonText(factsSummaryJson()), mimeFor(path), isHead, fact);
            return;
        }
        sendUnrouted(socket, 404, QStringLiteral("Not Found"), QByteArray(),
                     QStringLiteral("text/plain"));
    }

    void serveHls(QTcpSocket* socket, const QUrl& target, bool isHead, RequestFact fact) {
        const QString relative = target.path().mid(5); // strip "/hls/"
        if (relative.contains(QStringLiteral("..")) || relative.contains(QLatin1Char('\\'))) {
            sendUnrouted(socket, 404, QStringLiteral("Not Found"), QByteArray(),
                         QStringLiteral("text/plain"));
            return;
        }
        const Rung* rung = nullptr;
        for (const Rung& candidate : rungs()) {
            if (relative.startsWith(candidate.name + QLatin1Char('/'))) {
                rung = &candidate;
                break;
            }
        }
        if (!rung) {
            sendUnrouted(socket, 404, QStringLiteral("Not Found"), QByteArray(),
                         QStringLiteral("text/plain"));
            return;
        }

        // Only the playlist is signed; segments are public inside the fixture.
        if (relative.endsWith(QStringLiteral("/playlist.m3u8")) &&
            !validateSignature(*rung, QUrlQuery(target))) {
            QJsonObject error;
            error.insert(QStringLiteral("error"), QStringLiteral("expired_signature"));
            error.insert(QStringLiteral("rung"), rung->name);
            error.insert(QStringLiteral("expired"), true);
            error.insert(QStringLiteral("remedy"),
                         QStringLiteral("re-extract the master: /hls/master.m3u8?fresh=1"));
            respondText(socket, 403,
                        QString::fromUtf8(QJsonDocument(error).toJson(QJsonDocument::Compact)),
                        QStringLiteral("application/json"), isHead, fact);
            return;
        }

        const QFileInfo info(QDir(hlsDir_).filePath(relative));
        if (!info.isFile()) {
            sendUnrouted(socket, 404, QStringLiteral("Not Found"), QByteArray(),
                         QStringLiteral("text/plain"));
            return;
        }
        serveFile(socket, info.absoluteFilePath(), isHead, fact);
    }

    // A missing, malformed or stale signature fails; a failure is never a
    // silent success.
    bool validateSignature(const Rung& rung, const QUrlQuery& query) const {
        const QString expiresText =
            query.queryItemValue(QStringLiteral("exp"), QUrl::FullyDecoded);
        const QString signature =
            query.queryItemValue(QStringLiteral("sig"), QUrl::FullyDecoded);
        if (expiresText.isEmpty() || signature.isEmpty()) {
            return false;
        }
        bool ok = false;
        const qint64 expiresAt = expiresText.toLongLong(&ok);
        if (!ok || expiresAt <= QDateTime::currentSecsSinceEpoch()) {
            return false;
        }
        return signature == signatureFor(rung.name, expiresAt);
    }

    // Variant URIs are relative so parseVariants() must resolve them against
    // the master URL. Bandwidth is measured from the real segments on disk.
    QString masterPlaylist(bool fresh) const {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const qint64 expiredAt = now - 3600; // the stale URL a client still holds
        const qint64 liveAt = now + 3600;

        QString text = QStringLiteral("#EXTM3U\n");
        text += QStringLiteral("#EXT-X-VERSION:3\n");
        text += QStringLiteral("#EXT-X-INDEPENDENT-SEGMENTS\n");
        for (const Rung& rung : rungs()) {
            const bool stale = !fresh && rung.name == QStringLiteral("v360");
            const qint64 expiresAt = stale ? expiredAt : liveAt;
            text += QStringLiteral("#EXT-X-STREAM-INF:BANDWIDTH=%1,CODECS=\"%2\",RESOLUTION=%3x%4\n")
                        .arg(rungBandwidth(rung))
                        .arg(rung.codec)
                        .arg(rung.width)
                        .arg(rung.height);
            text += QStringLiteral("%1/playlist.m3u8?exp=%2&sig=%3\n")
                        .arg(rung.name)
                        .arg(expiresAt)
                        .arg(signatureFor(rung.name, expiresAt));
        }
        return text;
    }

    qint64 rungBandwidth(const Rung& rung) const {
        qint64 total = 0;
        const QDir dir(QDir(hlsDir_).filePath(rung.name));
        const QStringList segments = dir.entryList({QStringLiteral("seg*.ts")}, QDir::Files);
        for (const QString& segment : segments) {
            total += QFileInfo(dir.filePath(segment)).size();
        }
        // Each rung is six seconds of media.
        return qMax<qint64>(1, (total * 8) / 6);
    }

    QString manifest() const {
        QJsonArray media;
        const QDir dir(mediaDir_);
        const QStringList clips = dir.entryList({QStringLiteral("*.mp4")}, QDir::Files, QDir::Name);
        for (const QString& clip : clips) {
            const QFileInfo info(dir.filePath(clip));
            QJsonObject object;
            object.insert(QStringLiteral("name"), clip);
            object.insert(QStringLiteral("bytes"), static_cast<double>(info.size()));
            object.insert(QStringLiteral("url"), QStringLiteral("/media/%1").arg(clip));
            media.append(object);
        }
        QJsonArray variants;
        for (const Rung& rung : rungs()) {
            QJsonObject object;
            object.insert(QStringLiteral("rung"), rung.name);
            object.insert(QStringLiteral("width"), rung.width);
            object.insert(QStringLiteral("height"), rung.height);
            object.insert(QStringLiteral("codec"), rung.codec);
            object.insert(QStringLiteral("url"),
                          QStringLiteral("/hls/%1/playlist.m3u8").arg(rung.name));
            variants.append(object);
        }
        QJsonObject root;
        root.insert(QStringLiteral("media"), media);
        root.insert(QStringLiteral("hlsVariants"), variants);
        root.insert(QStringLiteral("primaryClip"), primaryClipName());
        root.insert(QStringLiteral("primaryClipUrl"), QStringLiteral("/media.mp4"));
        root.insert(QStringLiteral("retryClipUrl"), QStringLiteral("/retry.mp4"));
        root.insert(QStringLiteral("masterUrl"), QStringLiteral("/hls/master.m3u8"));
        root.insert(QStringLiteral("freshMasterUrl"), QStringLiteral("/hls/master.m3u8?fresh=1"));
        return jsonText(root);
    }

    void serveFile(QTcpSocket* socket, const QString& absolutePath, bool isHead,
                   RequestFact fact) {
        QFile file(absolutePath);
        if (!file.open(QIODevice::ReadOnly)) {
            sendUnrouted(socket, 404, QStringLiteral("Not Found"), QByteArray(),
                         QStringLiteral("text/plain"));
            return;
        }
        const QFileInfo info(absolutePath);
        const QByteArray content = file.readAll();
        const qint64 size = content.size();
        const QByteArray etag = QStringLiteral("\"%1-%2\"")
                                    .arg(QString::number(info.lastModified().toMSecsSinceEpoch()))
                                    .arg(QString::number(size))
                                    .toLatin1();
        const qint64 lastModifiedMsecs = info.lastModified().toMSecsSinceEpoch();
        const QString mime = mimeFor(absolutePath);

        if (!fact.rangeSpec.isEmpty()) {
            ByteRange range;
            if (parseByteRange(fact.rangeSpec, size, &range)) {
                const qint64 length = range.last - range.first + 1;
                respond(socket, 206, content.mid(range.first, length), mime,
                        "Content-Range: bytes " +
                            QStringLiteral("%1-%2/%3")
                                .arg(range.first)
                                .arg(range.last)
                                .arg(size)
                                .toLatin1(),
                        fact, isHead, etag, lastModifiedMsecs);
                return;
            }
            if (fact.rangeSpec.trimmed().startsWith(QStringLiteral("bytes="))) {
                respond(socket, 416, QByteArray(), QStringLiteral("application/octet-stream"),
                        "Content-Range: bytes */" + QByteArray::number(size), fact, isHead, etag,
                        lastModifiedMsecs);
                return;
            }
        }

        respond(socket, 200, content, mime, QByteArray(), fact, isHead, etag, lastModifiedMsecs);
    }

    void respondText(QTcpSocket* socket, int status, const QString& text, const QString& mime,
                     bool isHead, RequestFact fact) {
        respond(socket, status, text.toUtf8(), mime, QByteArray(), fact, isHead);
    }

    // Recorded too, so a rejected or malformed request is visible in the facts
    // rather than silently absent from them.
    void sendUnrouted(QTcpSocket* socket, int status, const QString& reason,
                      const QByteArray& body, const QString& mime) {
        RequestFact fact;
        ++seq_;
        fact.seq = seq_;
        fact.status = status;
        facts_.append(fact);
        writeFacts();
        respondRaw(socket, status, reason, body, mime, QByteArray(), false, QByteArray(), 0);
    }

    void respond(QTcpSocket* socket, int status, const QByteArray& body, const QString& mime,
                 const QByteArray& extraHeaders, RequestFact fact, bool isHead,
                 const QByteArray& etag = QByteArray(), qint64 lastModifiedMsecs = 0) {
        fact.status = status;
        fact.bytesServed = body.size();
        facts_.append(fact);
        writeFacts();
        respondRaw(socket, status, reasonFor(status), body, mime, extraHeaders, isHead, etag,
                   lastModifiedMsecs);
    }

    // HEAD reports the Content-Length a GET would return but sends no body.
    void respondRaw(QTcpSocket* socket, int status, const QString& reason, const QByteArray& body,
                    const QString& mime, const QByteArray& extraHeaders, bool isHead,
                    const QByteArray& etag, qint64 lastModifiedMsecs) {
        QByteArray head;
        head += "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason.toLatin1() + "\r\n";
        head += "Content-Type: " + mime.toLatin1() + "\r\n";
        head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        head += "Accept-Ranges: bytes\r\n";
        if (!etag.isEmpty()) {
            head += "ETag: " + etag + "\r\n";
        }
        if (lastModifiedMsecs > 0) {
            head += "Last-Modified: " + httpDate(lastModifiedMsecs).toLatin1() + "\r\n";
        }
        head += "Cache-Control: no-store\r\n";
        head += "Connection: close\r\n";
        if (!extraHeaders.isEmpty()) {
            head += extraHeaders + "\r\n";
        }
        head += "\r\n";

        const QByteArray response = isHead ? head : head + body;
        socket->write(response);
        // The connection is closed only once every byte of the response has
        // left the write buffer. A client can be slower to read than this
        // server is to write (a cold ffmpeg connection reading a fresh clip),
        // and a FIN that overtakes a half-delivered body is exactly the
        // failure this fixture must never hand a media client.
        if (socket->bytesToWrite() == 0) {
            socket->disconnectFromHost();
            return;
        }
        connect(socket, &QTcpSocket::bytesWritten, socket, [socket] {
            if (socket->bytesToWrite() == 0) {
                socket->disconnectFromHost();
            }
        });
    }

    QJsonObject factsJson() const {
        QJsonArray array;
        for (const RequestFact& fact : facts_) {
            array.append(fact.toJson());
        }
        QJsonObject root;
        root.insert(QStringLiteral("total"), facts_.size());
        root.insert(QStringLiteral("requests"), array);
        return root;
    }

    QJsonObject factsSummaryJson() const {
        int acceptLanguagePresent = 0;
        int acceptLanguageCommaFree = 0;
        int userAgentPresent = 0;
        int rangeRequests = 0;
        for (const RequestFact& fact : facts_) {
            acceptLanguagePresent += fact.acceptLanguagePresent ? 1 : 0;
            acceptLanguageCommaFree += fact.acceptLanguageCommaFree ? 1 : 0;
            userAgentPresent += fact.userAgentPresent ? 1 : 0;
            rangeRequests += fact.rangePresent ? 1 : 0;
        }
        QJsonObject root;
        root.insert(QStringLiteral("total"), facts_.size());
        root.insert(QStringLiteral("acceptLanguagePresent"), acceptLanguagePresent);
        root.insert(QStringLiteral("acceptLanguageCommaFree"), acceptLanguageCommaFree);
        root.insert(QStringLiteral("userAgentPresent"), userAgentPresent);
        root.insert(QStringLiteral("rangeRequests"), rangeRequests);
        return root;
    }

    void writeFacts() const {
        if (factsPath_.isEmpty()) {
            return;
        }
        QFile file(factsPath_);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(factsJson()).toJson(QJsonDocument::Indented));
        }
    }

    static QString jsonText(const QJsonObject& object) {
        return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
    }

    QTcpServer* server_ = nullptr;
    QString mediaDir_;
    QString hlsDir_;
    QString primaryClip_;
    QString factsPath_;
    QString portFile_;
    bool requireAcceptLanguage_ = false;
    bool retryServed_ = false;
    int seq_ = 0;
    QList<RequestFact> facts_;
};

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("mikai_http_fixture"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Hermetic loopback HTTP/HLS fixture server for the mikai test matrix."));
    parser.addHelpOption();
    const QCommandLineOption mediaOption(
        QStringLiteral("media-dir"), QStringLiteral("Directory holding the generated clips."),
        QStringLiteral("dir"));
    const QCommandLineOption hlsOption(
        QStringLiteral("hls-dir"),
        QStringLiteral("Directory holding the generated HLS renditions."), QStringLiteral("dir"));
    const QCommandLineOption portOption(
        QStringLiteral("port"), QStringLiteral("TCP port; 0 picks a free one."),
        QStringLiteral("port"), QStringLiteral("0"));
    const QCommandLineOption factsOption(
        QStringLiteral("facts"),
        QStringLiteral("Write received-header facts to this JSON file as requests arrive."),
        QStringLiteral("file"));
    const QCommandLineOption portFileOption(
        QStringLiteral("port-file"),
        QStringLiteral("Write the bound port to this file once listening."),
        QStringLiteral("file"));
    const QCommandLineOption requireLanguageOption(
        QStringLiteral("require-accept-language"),
        QStringLiteral("Answer 400 when a request carries no Accept-Language header."));
    parser.addOptions({mediaOption, hlsOption, portOption, factsOption, portFileOption,
                       requireLanguageOption});
    parser.process(app);

    const QString mediaDir =
        parser.isSet(mediaOption)
            ? parser.value(mediaOption)
            : QDir(QStringLiteral(MIKAI_FIXTURE_MEDIA_DIR)).absolutePath();
    const QString hlsDir = parser.isSet(hlsOption)
                               ? parser.value(hlsOption)
                               : QDir(QStringLiteral(MIKAI_FIXTURE_HLS_DIR)).absolutePath();

    bool portOk = false;
    const int port = parser.value(portOption).toInt(&portOk);
    if (!portOk || port < 0 || port > 65535) {
        std::fprintf(stderr, "invalid --port\n");
        return 2;
    }

    FixtureServer server(mediaDir, hlsDir, parser.value(factsOption),
                         parser.value(portFileOption), parser.isSet(requireLanguageOption));
    QString error;
    if (!server.start(static_cast<quint16>(port), &error)) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 1;
    }
    server.writePortFile();

    const quint16 bound = server.port();
    std::printf("mikai_http_fixture listening on http://127.0.0.1:%u\n", bound);
    std::printf("  media clip  : http://127.0.0.1:%u/media.mp4\n", bound);
    std::printf("  retry once  : http://127.0.0.1:%u/retry.mp4\n", bound);
    std::printf("  hls master  : http://127.0.0.1:%u/hls/master.m3u8\n", bound);
    std::printf("  fresh master: http://127.0.0.1:%u/hls/master.m3u8?fresh=1\n", bound);
    std::printf("  header facts: http://127.0.0.1:%u/_facts\n", bound);
    std::fflush(stdout);

    return app.exec();
}
