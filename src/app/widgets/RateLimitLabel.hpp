#pragma once

#include <backend/MikaiClient.hpp>

#include <QWidget>

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QHideEvent;
class QHBoxLayout;
class QLabel;
class QShowEvent;
class QTimer;

namespace mikai::app {

/// Small server quota indicator shared by the catalog and title pages.
class RateLimitLabel : public QWidget {
    Q_OBJECT

public:
    /// How the same snapshot is presented. Inline is the catalogue/title page's
    /// bare line; Pill is the main page header's bordered chip with a status
    /// dot. The two share every fact and every request - only the surface and
    /// the dot differ, so the shared widget keeps one behaviour.
    enum class Presentation { Inline, Pill };

    explicit RateLimitLabel(backend::MikaiClient& client, QWidget* parent = nullptr);
    ~RateLimitLabel() override;

    /// Switches presentation. Inline is the default, so a surface that does not
    /// ask for the pill keeps exactly the line it always had.
    void setPresentation(Presentation presentation);
    Presentation presentation() const { return presentation_; }

signals:
    /// A click on the chip; both pages route it to the Access settings page, the
    /// one surface that can raise or replace the key the quota belongs to.
    void clicked();

public:
    /// Re-reads the client's quota snapshot. Cheap and non-blocking: the state
    /// lives behind the client's own lock and never issues a request.
    void refresh();

protected:
    /// The chip is a shortcut, not decoration: a left click anywhere on it opens
    /// the Access page. Keyboard activation matches so it is not mouse-only.
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    /// Restates the line in place after a LanguageChange from the snapshot the
    /// widget already rendered: the quota, the reset window and the queue depth
    /// stay exactly as they were shown, and nothing is asked of the server.
    void changeEvent(QEvent* event) override;
    /// T-412: the 1 Hz tick lives only while the widget is on screen; the
    /// timer is armed here and stopped by hideEvent.
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    /// The pill's tabular figures are a font feature, and a stylesheet re-polish
    /// replaces the label's font wholesale. Watching the label restores the
    /// feature instead of waiting for the next 1 Hz tick to do it.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// Paints one snapshot twice over - the compact quota line and the detail
    /// that both the tooltip and the accessible name carry. An identical
    /// line is a no-op.
    void renderState(const backend::RateLimitState& state);
    /// What the pill's dot may claim from one snapshot: a healthy window only
    /// when a response reported quota figures, an exhausted one when none is
    /// left, a low one near the end, and unknown otherwise.
    QString dotTone(const backend::RateLimitState& state) const;
    void applyDotTone(const QString& tone);
    /// Keeps the pill's digits on one advance so the line does not jitter as
    /// the counter moves. The styled font is re-read on every tick, so a sheet
    /// re-polish cannot quietly take the feature away.
    void applyTabularNumerals();
    void applyPresentationMargins();

    backend::MikaiClient& client_;
    backend::RateLimitState state_;
    QLabel* label_ = nullptr;
    /// The pill's status disc. Created after the text label so the shared
    /// surface's own `findChild<QLabel*>()` still finds the line it always did.
    QLabel* dot_ = nullptr;
    QHBoxLayout* layout_ = nullptr;
    QTimer* timer_ = nullptr;
    Presentation presentation_ = Presentation::Inline;
    /// T-412: the last rendered pair, so an unchanged snapshot costs nothing.
    QString renderedText_;
    QString renderedDetail_;
};

}  // namespace mikai::app
