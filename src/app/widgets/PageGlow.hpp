#pragma once

#include <QWidget>

namespace mikai::app {

/// The accent glow behind the top of a scrolling page. It tracks its parent's
/// width and the chosen height, paints nothing when the preference is off, and
/// always sits under its siblings so content cards stay readable.
class PageGlow : public QWidget {
    Q_OBJECT

public:
    explicit PageGlow(QWidget* parent);

    /// Re-reads the appearance and resizes to the chosen share of the host.
    /// The glow has no layout entry, so this is the only thing that moves it.
    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
};

}  // namespace mikai::app
