// The player process entry point.
//
// The player cannot be a DLL inside mikai: PySide6 and this Qt are separate
// builds whose Qt6Core.dll / Qt6Gui.dll / Qt6Widgets.dll collide by DLL name,
// and the loader binds the second copy to the first, so the import table fails
// to resolve (WinError 127). The player is therefore its own process, talking
// newline-delimited JSON over stdin/stdout. That is the whole reason this file
// exists.

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantMap>

#include <cstdio>

#include <windows.h>

#include "IpcChannel.hpp"
#include "PlayerDialog.hpp"

using namespace mplayer::shell;

namespace {

QList<QualityEntry> qualitiesFromJson(const QJsonObject& object) {
    QList<QualityEntry> out;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        bool ok = false;
        const int height = it.key().toInt(&ok);
        if (ok) out.append({height, it.value().toString()});
    }
    return out;
}

QVector<QVariantMap> mapsFromJson(const QJsonValue& value) {
    QVector<QVariantMap> out;
    for (const QJsonValue& item : value.toArray()) {
        if (item.isObject()) out.append(item.toObject().toVariantMap());
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // The whole configuration arrives as one JSON object on argv[1]: a playlist,
    // a variant list and a quality map are far too structured for a command
    // line, and quoting them through one is a bug farm.
    if (argc < 2) {
        std::fprintf(stderr, "usage: mikai-player <config.json>\n");
        return 2;
    }
    QFile configFile(QString::fromLocal8Bit(argv[1]));
    if (!configFile.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot read config: %s\n", qPrintable(configFile.errorString()));
        return 2;
    }
    const QJsonDocument document = QJsonDocument::fromJson(configFile.readAll());
    if (!document.isObject()) {
        std::fprintf(stderr, "config is not a JSON object\n");
        return 2;
    }
    const QJsonObject config = document.object();

    // The dialog's host is whoever has to answer for the things this process
    // cannot do alone: stream extraction, episode lookup and the browser
    // handoff all live in mikai.
    class Bridge : public PlayerHost {
    public:
        void requestVariant(const QString& embed, const QString& label) override {
            IpcWriter::send(QJsonObject{{"ev", "variant"}, {"embed", embed}, {"label", label}});
        }
        void requestEpisode(int index) override {
            IpcWriter::send(QJsonObject{{"ev", "episode"}, {"index", index}});
        }
        void requestTrackProbe(const QString& embed) override {
            IpcWriter::send(QJsonObject{{"ev", "trackProbe"}, {"embed", embed}});
        }
        void requestOpenOriginal() override {
            IpcWriter::send(QJsonObject{{"ev", "openOriginal"}});
        }
        void requestCopyStreamUrl() override { IpcWriter::send(QJsonObject{{"ev", "copyUrl"}}); }
        void playerClosed() override { IpcWriter::send(QJsonObject{{"ev", "closed"}}); }
        void playbackFailed() override {
            IpcWriter::send(QJsonObject{{"ev", "playbackFailed"}});
        }
    };
    static Bridge bridge;

    const QVector<QVariantMap> variants = mapsFromJson(config.value("variants"));
    const QVector<QVariantMap> playlist = mapsFromJson(config.value("playlist"));

    auto* dialog = new PlayerDialog(config.value("caption").toString(),
                                    config.value("url").toString(),
                                    qualitiesFromJson(config.value("qualities").toObject()),
                                    config.value("height").toInt(), &bridge);
    // The track picker starts empty and the chip stays hidden until there is
    // something to pick, so the launch-time variant list has to land before the
    // first paint rather than as a later command.
    dialog->setVariants(variants, config.value("currentEmbed").toString());
    dialog->setPlaylist(playlist, config.value("episodeIndex").toInt());
    dialog->startPlayer();
    dialog->show();

    IpcReader reader;
    QObject::connect(&reader, &IpcReader::command, dialog, [dialog](const QJsonObject& command) {
        const QString name = command.value("cmd").toString();
        if (name == "applyEpisode") {
            dialog->applyEpisode(command.value("label").toString(),
                                 command.value("embed").toString(),
                                 command.value("index").toInt(),
                                 qualitiesFromJson(command.value("qualities").toObject()),
                                 mapsFromJson(command.value("variants")));
        } else if (name == "applyVariant") {
            dialog->applyVariant(qualitiesFromJson(command.value("qualities").toObject()),
                                 command.value("label").toString(),
                                 command.value("embed").toString());
        } else if (name == "applyRefreshedStreams") {
            dialog->applyRefreshedStreams(qualitiesFromJson(command.value("qualities").toObject()),
                                          command.value("height").toInt());
        } else if (name == "setTrackQuality") {
            dialog->setTrackQuality(command.value("embed").toString(),
                                    command.value("quality").toInt());
        } else if (name == "setPlaylist") {
            dialog->setPlaylist(mapsFromJson(command.value("playlist")),
                                command.value("index").toInt());
        } else if (name == "setVariants") {
            dialog->setVariants(mapsFromJson(command.value("variants")),
                                command.value("currentEmbed").toString());
        } else if (name == "queryQuality") {
            // A real report, not an echo: the host needs the height the child
            // actually settled on after a track swap, which it cannot know.
            IpcWriter::send(QJsonObject{{"ev", "quality"},
                                        {"height", dialog->currentHeight()}});
        } else if (name == "showError") {
            dialog->showError(command.value("message").toString());
        } else if (name == "close") {
            dialog->close();
        }
    });

    // The host disappearing means mikai is gone; there is nothing left to save.
    QObject::connect(&reader, &IpcReader::hostClosed, &app, [dialog] {
        dialog->close();
        dialog->deleteLater();
        QCoreApplication::quit();
    });

    reader.start();
    const int result = app.exec();
    // The reader is parked in fgets with the host still attached. TerminateThread
    // leaves the CRT's stdin lock held and the process then faults on the way out,
    // so cancel the blocking read first and only escalate if it survives.
    reader.requestStop();
    return result;
}
