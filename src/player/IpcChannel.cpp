#include "IpcChannel.hpp"

#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

namespace mplayer {
namespace shell {

namespace {
std::mutex g_writeMutex;
}  // namespace

IpcReader::IpcReader(QObject* parent) : QThread(parent) {}
IpcReader::~IpcReader() { stop_ = true; }

void IpcReader::run() {
    QByteArray line;
    char buffer[4096];
    while (!stop_) {
        const size_t n = fread(buffer, 1, sizeof(buffer), stdin);
        if (n == 0) {
            // EOF: Python closed the pipe.
            emit hostClosed();
            return;
        }
        line.append(buffer, static_cast<int>(n));
        int newline;
        while ((newline = line.indexOf('\n')) >= 0) {
            const QByteArray raw = line.left(newline);
            line.remove(0, newline + 1);
            if (raw.trimmed().isEmpty()) continue;
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            if (doc.isObject()) {
                emit command(doc.object());
            }
        }
    }
}

void IpcWriter::send(const QJsonObject& event) {
    const QByteArray data =
        QJsonDocument(event).toJson(QJsonDocument::Compact);
    std::lock_guard<std::mutex> lock(g_writeMutex);
    fwrite(data.constData(), 1, static_cast<size_t>(data.size()), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

}  // namespace shell
}  // namespace mplayer
