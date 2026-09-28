#include "IpcChannel.hpp"

#include <QJsonDocument>

#include <windows.h>

#include <cstdio>
#include <io.h>

namespace mplayer {
namespace shell {

namespace {
std::mutex g_writeMutex;
}  // namespace

IpcReader::IpcReader(QObject* parent) : QObject(parent) {
    timer_.setInterval(20);
    connect(&timer_, &QTimer::timeout, this, &IpcReader::poll);
}

IpcReader::~IpcReader() = default;

void IpcReader::start() { timer_.start(); }

void IpcReader::requestStop() {
    stopped_ = true;
    timer_.stop();
}

void IpcReader::poll() {
    if (stopped_) return;

    const HANDLE stdinHandle = reinterpret_cast<HANDLE>(_get_osfhandle(0));
    if (stdinHandle == INVALID_HANDLE_VALUE || stdinHandle == nullptr) {
        emit hostClosed();
        requestStop();
        return;
    }

    // Ask how much is actually there rather than blocking for a fixed amount:
    // the protocol is line-delimited and commands are tens of bytes, so any
    // read that waits for a full buffer is a read that never returns.
    DWORD available = 0;
    if (!PeekNamedPipe(stdinHandle, nullptr, 0, nullptr, &available, nullptr)) {
        // A broken pipe is the host going away, not an error to report.
        emit hostClosed();
        requestStop();
        return;
    }
    if (available == 0) return;

    QByteArray chunk;
    chunk.resize(static_cast<int>(available));
    DWORD read = 0;
    if (!ReadFile(stdinHandle, chunk.data(), available, &read, nullptr) || read == 0) {
        emit hostClosed();
        requestStop();
        return;
    }
    chunk.truncate(static_cast<int>(read));
    buffer_.append(chunk);

    int newline = 0;
    while ((newline = buffer_.indexOf('\n')) >= 0) {
        const QByteArray raw = buffer_.left(newline).trimmed();
        buffer_.remove(0, newline + 1);
        if (raw.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (doc.isObject()) {
            emit command(doc.object());
        }
    }
}

void IpcWriter::send(const QJsonObject& event) {
    const QByteArray data = QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
    std::lock_guard<std::mutex> lock(g_writeMutex);
    // Flush immediately: the host is waiting on this line, and a buffered write
    // would sit here until the pipe happened to fill.
    fwrite(data.constData(), 1, static_cast<size_t>(data.size()), stdout);
    fflush(stdout);
}

}  // namespace shell
}  // namespace mplayer
