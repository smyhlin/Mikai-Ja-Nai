#include "workers/Runner.hpp"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QRunnable>
#include <backend/Errors.hpp>
#include <backend/Streams.hpp>

#include <exception>
#include <memory>
#include <utility>

namespace mikai::app {
namespace {

// The pools cap the three kinds of work. Artwork gets a wide pool on purpose:
// poster downloads spend no API budget and only wait on the network, so sixty
// of them at four at a time is what made the home page look unfinished.
constexpr int kSharedThreads = 4;
constexpr int kBulkThreads = 3;
constexpr int kImageThreads = 12;
// Stream extraction is latency-critical and single-user: an episode click has
// to answer now, not after the feed fan-out sharing the bulk pool. Two threads
// is all one user clicking one episode at a time can spend.
constexpr int kStreamThreads = 2;

}  // namespace

/// Lives on the GUI thread. Every completion is posted here, so a callback
/// always runs where the widgets it touches live.
class Runner::Dispatcher : public QObject {
    Q_OBJECT

public:
    explicit Dispatcher(QObject* parent) : QObject(parent) {}
};

namespace {

/// The result of one task and the handlers waiting for it. Held by shared_ptr
/// so a completion that is still queued keeps the callbacks alive.
struct Delivery {
    Runner::OnOk onOk;
    Runner::OnErr onErr;
};

class Task : public QRunnable {
public:
    Task(std::shared_ptr<Runner::Dispatcher> dispatcher, Runner::Work work,
         std::shared_ptr<Delivery> delivery)
        : dispatcher_(std::move(dispatcher)), work_(std::move(work)),
          delivery_(std::move(delivery)) {
        setAutoDelete(true);
    }

    void run() override {
        QVariant result;
        bool ok = true;
        QString message;
        try {
            if (work_) {
                result = work_();
            }
        } catch (const backend::MikaiApiError& error) {
            // The stable error code is the branch point everywhere else, but a
            // task only reports the human message.
            ok = false;
            message = error.message.isEmpty() ? error.code : error.message;
        } catch (const backend::NoSourceError& error) {
            // Tagged so the title page can tell "this provider has no video"
            // apart from "the extraction failed".
            ok = false;
            message = QStringLiteral("no_source: %1").arg(QString::fromUtf8(error.what()));
        } catch (const std::exception& error) {
            ok = false;
            message = QString::fromUtf8(error.what());
        } catch (...) {
            ok = false;
            message = QStringLiteral("Невідома помилка");
        }
        // A queued call on the dispatcher: the pool thread hands the result
        // over and returns immediately, and the slot runs on the GUI thread.
        QMetaObject::invokeMethod(
            dispatcher_.get(), [dispatcher = dispatcher_, delivery = delivery_, result, ok,
                                message]() {
                if (ok) {
                    if (delivery->onOk) {
                        delivery->onOk(result);
                    }
                } else if (delivery->onErr) {
                    delivery->onErr(message);
                }
            },
            Qt::QueuedConnection);
    }

private:
    std::shared_ptr<Runner::Dispatcher> dispatcher_;
    Runner::Work work_;
    std::shared_ptr<Delivery> delivery_;
};

}  // namespace

struct Runner::Impl {
    explicit Impl(QObject* parent)
        : dispatcher(std::make_shared<Dispatcher>(parent)), shared(new QThreadPool(parent)),
          bulk(new QThreadPool(parent)), stream(new QThreadPool(parent)),
          image(new QThreadPool(parent)) {
        shared->setMaxThreadCount(kSharedThreads);
        bulk->setMaxThreadCount(kBulkThreads);
        stream->setMaxThreadCount(kStreamThreads);
        image->setMaxThreadCount(kImageThreads);
    }

    std::shared_ptr<Dispatcher> dispatcher;
    QThreadPool* shared;
    QThreadPool* bulk;
    QThreadPool* stream;
    QThreadPool* image;
};

Runner::Runner(QObject* parent) : impl_(std::make_unique<Impl>(parent)) {}

Runner::~Runner() {
    // A completion already queued still holds its own shared_ptr, so the
    // dispatcher's destruction cannot pull the callbacks out from under it.
    waitForDone(30000);
}

void Runner::start(QThreadPool* pool, Work work, OnOk onOk, OnErr onErr) {
    auto delivery = std::make_shared<Delivery>();
    delivery->onOk = std::move(onOk);
    delivery->onErr = std::move(onErr);
    pool->start(new Task(impl_->dispatcher, std::move(work), std::move(delivery)));
}

void Runner::run(Work work, OnOk onOk, OnErr onErr) {
    start(impl_->shared, std::move(work), std::move(onOk), std::move(onErr));
}

void Runner::runBulk(Work work, OnOk onOk, OnErr onErr) {
    start(impl_->bulk, std::move(work), std::move(onOk), std::move(onErr));
}

void Runner::runStream(Work work, OnOk onOk, OnErr onErr) {
    start(impl_->stream, std::move(work), std::move(onOk), std::move(onErr));
}

void Runner::runImage(Work work, OnOk onOk, OnErr onErr) {
    start(impl_->image, std::move(work), std::move(onOk), std::move(onErr));
}

bool Runner::waitForDone(int timeoutMs) {
    QElapsedTimer budget;
    budget.start();
    const auto left = [&budget, timeoutMs] {
        return qMax(0, timeoutMs - static_cast<int>(budget.elapsed()));
    };
    const bool bulk = impl_->bulk->waitForDone(left());
    const bool stream = impl_->stream->waitForDone(left());
    const bool image = impl_->image->waitForDone(left());
    return impl_->shared->waitForDone(left()) && bulk && stream && image;
}

}  // namespace mikai::app

#include "Runner.moc"
