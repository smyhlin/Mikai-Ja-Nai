#pragma once

#include <backend/MikaiClient.hpp>

#include <QWidget>

class QEvent;
class QHideEvent;
class QLabel;
class QShowEvent;
class QTimer;

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

protected:
    /// Restates the line in place after a LanguageChange from the snapshot the
    /// widget already rendered: the quota, the reset window and the queue depth
    /// stay exactly as they were shown, and nothing is asked of the server.
    void changeEvent(QEvent* event) override;
    /// T-412: the 1 Hz tick lives only while the widget is on screen; the
    /// timer is armed here and stopped by hideEvent.
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    /// Paints one snapshot twice over - the compact quota line and the detail
    /// that both the tooltip and the accessible name carry. An identical
    /// line is a no-op.
    void renderState(const backend::RateLimitState& state);

    backend::MikaiClient& client_;
    backend::RateLimitState state_;
    QLabel* label_ = nullptr;
    QTimer* timer_ = nullptr;
    /// T-412: the last rendered pair, so an unchanged snapshot costs nothing.
    QString renderedText_;
    QString renderedDetail_;
};

}  // namespace mikai::app
