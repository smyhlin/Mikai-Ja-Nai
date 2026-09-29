#pragma once

#include <QWidget>

class QLabel;
class QTimer;

namespace mikai::backend {
class MikaiClient;
}

namespace mikai::app {

/// Small server quota indicator shared by the catalog and title pages.
class RateLimitLabel : public QWidget {
    Q_OBJECT

public:
    explicit RateLimitLabel(backend::MikaiClient& client, QWidget* parent = nullptr);
    ~RateLimitLabel() override;

    /// Re-reads the client's quota snapshot. Cheap and non-blocking: the state
    /// lives behind the client's own lock and never issues a request.
    void refresh();

private:
    backend::MikaiClient& client_;
    QLabel* label_ = nullptr;
    QTimer* timer_ = nullptr;
};

}  // namespace mikai::app
