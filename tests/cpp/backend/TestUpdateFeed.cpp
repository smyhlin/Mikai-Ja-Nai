#include "TestUpdateFeed.hpp"

#include <backend/Errors.hpp>
#include <backend/MikaiClient.hpp>
#include <backend/UpdateFeed.hpp>

#include <QtCore/QDir>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QSet>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

using mikai::backend::AnimeUpdate;
using mikai::backend::MikaiApiError;
using mikai::backend::MikaiClient;
using mikai::backend::UpdateFeedState;
using mikai::backend::refreshUpdateFeed;
using mikai::backend::setEnvFilePathOverride;

namespace {

class FeedServer final : public QObject {
public:
    explicit FeedServer(QObject* parent = nullptr) : QObject(parent) {
        QObject::connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* socket = server_.nextPendingConnection()) {
                buffers_.insert(socket, {});
                QObject::connect(socket, &QTcpSocket::disconnected, socket, [this, socket] {
                    buffers_.remove(socket);
                    served_.remove(socket);
                    socket->deleteLater();
                });
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    if (served_.contains(socket)) return;
                    buffers_[socket] += socket->readAll();
                    const int end = buffers_[socket].indexOf("\r\n\r\n");
                    if (end < 0) return;
                    served_.insert(socket);
                    QByteArray body = replies_.isEmpty()
                        ? QByteArrayLiteral(R"({"ok":false,"error":{"code":"unexpected","message":"unexpected request"}})")
                        : replies_.takeFirst();
                    const int status = statuses_.isEmpty() ? 200 : statuses_.takeFirst();
                    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) +
                        (status == 200 ? QByteArrayLiteral(" OK\r\n") : QByteArrayLiteral(" Internal Server Error\r\n"));
                    response += QByteArrayLiteral("Content-Type: application/json\r\nConnection: close\r\nContent-Length: ");
                    response += QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body;
                    socket->write(response);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    bool start() { return server_.listen(QHostAddress::LocalHost, 0); }
    QString baseUrl() const {
        return QStringLiteral("http://127.0.0.1:%1/public/v1").arg(server_.serverPort());
    }
    void enqueue(const QByteArray& body, int status = 200) {
        replies_.append(body);
        statuses_.append(status);
    }

private:
    QTcpServer server_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    QList<QByteArray> replies_;
    QList<int> statuses_;
    QSet<QTcpSocket*> served_;
};

QJsonObject update(const QString& anime, const QString& time, const QString& kind,
                   const QString& provider, const QJsonValue& episode, qint64 teamId,
                   const QString& teamName) {
    QJsonObject animeObject;
    animeObject.insert(QStringLiteral("ids"), QJsonObject{{QStringLiteral("mikai"), anime}});
    animeObject.insert(QStringLiteral("titles"), QJsonObject{{QStringLiteral("english"), QStringLiteral("Original title")}});
    QJsonObject team;
    team.insert(QStringLiteral("id"), static_cast<double>(teamId));
    team.insert(QStringLiteral("slug"), QStringLiteral("team-%1").arg(teamId));
    team.insert(QStringLiteral("name"), teamName);
    QJsonObject item;
    item.insert(QStringLiteral("anime"), animeObject);
    item.insert(QStringLiteral("teams"), QJsonArray{team});
    item.insert(QStringLiteral("teamName"), teamName);
    item.insert(QStringLiteral("kind"), kind);
    item.insert(QStringLiteral("provider"), provider);
    item.insert(QStringLiteral("episode"), episode);
    item.insert(QStringLiteral("publishedAt"), time);
    item.insert(QStringLiteral("embedUrl"), QStringLiteral("https://player.example/%1/%2").arg(provider, episode.toVariant().toString()));
    return item;
}

QJsonObject changedMetadataUpdate(const QString& timestamp) {
    QJsonObject item = update(QStringLiteral("anime-a"), timestamp, QStringLiteral("voice"),
                              QStringLiteral("p1"), 12, 31, QStringLiteral("Renamed Alpha"));
    QJsonObject anime = item.value(QStringLiteral("anime")).toObject();
    anime.insert(QStringLiteral("titles"),
                 QJsonObject{{QStringLiteral("english"), QStringLiteral("Updated title")}});
    anime.insert(QStringLiteral("images"),
                  QJsonObject{{QStringLiteral("poster"),
                               QJsonObject{{QStringLiteral("big"),
                                            QJsonObject{{QStringLiteral("webp"),
                                                         QStringLiteral("https://img.example/updated.webp")}}}}}});
    item.insert(QStringLiteral("anime"), anime);
    QJsonArray teams = item.value(QStringLiteral("teams")).toArray();
    QJsonObject team = teams.first().toObject();
    team.insert(QStringLiteral("name"), QStringLiteral("Renamed Alpha"));
    teams[0] = team;
    item.insert(QStringLiteral("teams"), teams);
    item.insert(QStringLiteral("embedUrl"), QStringLiteral("https://player.example/updated"));
    return item;
}

QJsonObject twoTeamUpdate(const QString& timestamp, bool reverse, bool renamed) {
    QJsonObject item = update(QStringLiteral("anime-a"), timestamp, QStringLiteral("voice"),
                              QStringLiteral("p5"), 9, 41, QStringLiteral("Alpha"));
    QJsonObject alpha{{QStringLiteral("id"), 41}, {QStringLiteral("slug"), QStringLiteral("alpha")},
                      {QStringLiteral("name"), renamed ? QStringLiteral("Renamed Alpha") : QStringLiteral("Alpha")}};
    QJsonObject beta{{QStringLiteral("id"), 42}, {QStringLiteral("slug"), QStringLiteral("beta")},
                     {QStringLiteral("name"), renamed ? QStringLiteral("Renamed Beta") : QStringLiteral("Beta")}};
    item.insert(QStringLiteral("teams"), reverse ? QJsonArray{beta, alpha} : QJsonArray{alpha, beta});
    item.insert(QStringLiteral("teamName"), renamed ? QStringLiteral("Renamed teams")
                                                    : QStringLiteral("Alpha + Beta"));
    return item;
}

QByteArray response(int page, int pages, const QJsonArray& items) {
    const QJsonObject root{{QStringLiteral("ok"), true}, {QStringLiteral("page"), page},
                          {QStringLiteral("pages"), pages}, {QStringLiteral("total"), items.size()},
                          {QStringLiteral("result"), items}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

}  // namespace

void TestUpdateFeed::initTestCase() {
    QVERIFY2(envDirectory_.isValid(), "cannot create an isolated .env directory");
    setEnvFilePathOverride(QDir(envDirectory_.path()).filePath(QStringLiteral(".env")));
}

void TestUpdateFeed::cleanupTestCase() {
    setEnvFilePathOverride(QString());
}

void TestUpdateFeed::refreshPaginatesAndDeduplicatesExactBoundaryEvents() {
    FeedServer server;
    QVERIFY(server.start());
    const QString stamp = QStringLiteral("2026-10-20T14:00:00+02:00");
    const QString sameInstant = QStringLiteral("2026-10-20T12:00:00Z");
    server.enqueue(response(1, 3, QJsonArray{
        update(QStringLiteral("anime-a"), stamp, QStringLiteral("voice"), QStringLiteral("p1"), 12, 31, QStringLiteral("Alpha")),
        update(QStringLiteral("anime-a"), stamp, QStringLiteral("voice"), QStringLiteral("p2"), 12, 31, QStringLiteral("Alpha")),
        update(QStringLiteral("anime-a"), stamp, QStringLiteral("voice"), QStringLiteral("p1"), 12.5, 31, QStringLiteral("Alpha")),
        update(QStringLiteral("anime-a"), stamp, QStringLiteral("voice"), QStringLiteral("p1"), 12, 32, QStringLiteral("Beta")),
        update(QStringLiteral("anime-a"), stamp, QStringLiteral("sub"), QStringLiteral("p1"), 12, 31, QStringLiteral("Alpha")),
        twoTeamUpdate(stamp, false, false)}));
    MikaiClient client(QStringLiteral("mk_update_feed_synthetic"), server.baseUrl());
    UpdateFeedState state = refreshUpdateFeed(client, {}, 6);
    QCOMPARE(state.items.size(), 6);
    QCOMPARE(state.watermark, QStringLiteral("2026-10-20T12:00:00.000Z"));

    const QString next = QStringLiteral("2026-10-20T12:00:01Z");
    server.enqueue(response(1, 2, QJsonArray{
        changedMetadataUpdate(sameInstant),
        update(QStringLiteral("anime-a"), sameInstant, QStringLiteral("voice"), QStringLiteral("p1"), 12.5, 31, QStringLiteral("Alpha")),
        twoTeamUpdate(sameInstant, true, true),
        update(QStringLiteral("anime-b"), next, QStringLiteral("sub"), QStringLiteral("p3"), 3, 33, QStringLiteral("Gamma"))}));
    server.enqueue(response(2, 3, QJsonArray{
        update(QStringLiteral("anime-b"), next, QStringLiteral("sub"), QStringLiteral("p3"), 3, 33, QStringLiteral("Gamma")),
        update(QStringLiteral("anime-c"), QStringLiteral("2026-10-20T12:00:02Z"), QStringLiteral("sub"), QStringLiteral("p4"), 4.5, 34, QStringLiteral("Delta"))}));
    server.enqueue(response(3, 3, QJsonArray{
        update(QStringLiteral("anime-d"), QStringLiteral("2026-10-20T12:00:03Z"), QStringLiteral("voice"), QStringLiteral("p6"), 5, 35, QStringLiteral("Epsilon"))}));
    state = refreshUpdateFeed(client, state, 6);
    QCOMPARE(state.items.size(), 6);
    QCOMPARE(state.items.at(0).anime.ref, QStringLiteral("anime-d"));
    QCOMPARE(state.items.at(1).anime.ref, QStringLiteral("anime-c"));
    QCOMPARE(state.items.at(2).anime.ref, QStringLiteral("anime-b"));
    QCOMPARE(state.items.at(3).team_name, QStringLiteral("Renamed Alpha"));
    QCOMPARE(state.items.at(3).teams.first().name, QStringLiteral("Renamed Alpha"));
    QCOMPARE(state.items.at(3).anime.titles.english, QStringLiteral("Updated title"));
    QCOMPARE(state.items.at(3).anime.poster.big.webp, QStringLiteral("https://img.example/updated.webp"));
    QCOMPARE(state.items.at(3).embed_url, QStringLiteral("https://player.example/updated"));
    QCOMPARE(state.watermark, QStringLiteral("2026-10-20T12:00:01.000Z"));

    server.enqueue(response(1, 1, QJsonArray{twoTeamUpdate(sameInstant, true, true)}));
    state = refreshUpdateFeed(client, state, 2);
    QCOMPARE(state.items.size(), 2);
    QCOMPARE(state.items.at(0).anime.ref, QStringLiteral("anime-d"));
    QCOMPARE(state.items.at(1).anime.ref, QStringLiteral("anime-c"));
    QCOMPARE(state.watermark, QStringLiteral("2026-10-20T12:00:01.000Z"));
}

void TestUpdateFeed::emptyRefreshRetainsThePublishedBuffer() {
    FeedServer server;
    QVERIFY(server.start());
    const QString stamp = QStringLiteral("2026-10-20T12:00:00Z");
    server.enqueue(response(1, 1, QJsonArray{update(QStringLiteral("anime-a"), stamp, QStringLiteral("voice"), QStringLiteral("p1"), 1, 31, QStringLiteral("Alpha"))}));
    MikaiClient client(QStringLiteral("mk_update_feed_synthetic"), server.baseUrl());
    const UpdateFeedState before = refreshUpdateFeed(client, {}, 12);
    server.enqueue(response(1, 1, QJsonArray{}));
    const UpdateFeedState after = refreshUpdateFeed(client, before, 12);
    QCOMPARE(after.watermark, before.watermark);
    QCOMPARE(after.items.size(), before.items.size());
    QCOMPARE(after.items.first().anime.ref, before.items.first().anime.ref);
}

void TestUpdateFeed::failedRefreshDoesNotMutateThePreviousState() {
    FeedServer server;
    QVERIFY(server.start());
    const QString stamp = QStringLiteral("2026-10-20T12:00:00Z");
    server.enqueue(response(1, 1, QJsonArray{update(QStringLiteral("anime-a"), stamp, QStringLiteral("voice"), QStringLiteral("p1"), 1, 31, QStringLiteral("Alpha"))}));
    MikaiClient client(QStringLiteral("mk_update_feed_synthetic"), server.baseUrl());
    const UpdateFeedState previous = refreshUpdateFeed(client, {}, 12);
    server.enqueue(QByteArrayLiteral(R"({"ok":false,"error":{"code":"unavailable","message":"offline"}})"), 500);
    QVERIFY_THROWS_EXCEPTION(MikaiApiError, refreshUpdateFeed(client, previous, 12));
    QCOMPARE(previous.watermark, QStringLiteral("2026-10-20T12:00:00.000Z"));
    QCOMPARE(previous.items.size(), 1);
}

void TestUpdateFeed::malformedTimestampFailsWithoutPublishingPartialState() {
    FeedServer server;
    QVERIFY(server.start());
    const QString stamp = QStringLiteral("2026-10-20T12:00:00Z");
    server.enqueue(response(1, 1, QJsonArray{
        update(QStringLiteral("anime-a"), stamp, QStringLiteral("voice"), QStringLiteral("p1"), 1, 31, QStringLiteral("Alpha"))}));
    MikaiClient client(QStringLiteral("mk_update_feed_synthetic"), server.baseUrl());
    const UpdateFeedState previous = refreshUpdateFeed(client, {}, 12);

    server.enqueue(response(1, 2, QJsonArray{
        update(QStringLiteral("anime-b"), QStringLiteral("2026-10-20T12:00:01Z"), QStringLiteral("sub"),
               QStringLiteral("p2"), 2, 32, QStringLiteral("Beta"))}));
    QJsonObject invalid = update(QStringLiteral("anime-c"), QString(), QStringLiteral("voice"),
                                 QStringLiteral("p3"), 3, 33, QStringLiteral("Gamma"));
    server.enqueue(response(2, 2, QJsonArray{invalid}));

    QVERIFY_THROWS_EXCEPTION(MikaiApiError, refreshUpdateFeed(client, previous, 12));
    QCOMPARE(previous.watermark, QStringLiteral("2026-10-20T12:00:00.000Z"));
    QCOMPARE(previous.items.size(), 1);
    QCOMPARE(previous.items.first().anime.ref, QStringLiteral("anime-a"));
}

int runTestUpdateFeed(int argc, char** argv) {
    TestUpdateFeed test;
    return QTest::qExec(&test, argc, argv);
}
