#pragma once

#include "ShellTheme.hpp"
#include "ShellWidgets.hpp"

#include <QPushButton>

namespace mplayer {
namespace shell {

/// An episode stepper: the pale chevron that stands beside the picture, one per
/// side. Owned by the shell, not the dock, because it belongs to the episode
/// rather than to the transport: it appears only when there is somewhere to go
/// and it is never part of the dock's horizontal run.
///
/// The shape follows the application's corner radius rather than the 42 px disc
/// the shell used to paint under a plain IconButton, so at the square step it is
/// a rounded rectangle and at the largest step it is close to a circle.
class EpisodeNavButton : public QPushButton, public ChromePaint {
    Q_OBJECT
public:
    enum class Direction { Left, Right };

    explicit EpisodeNavButton(Direction direction, const QString& tooltip = QString(),
                              QWidget* parent = nullptr);

    void setDirection(Direction direction);
    Direction direction() const { return direction_; }

    void paintChrome(QPainter& painter) override;

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    Direction direction_;
    bool hover_ = false;
};

}  // namespace shell
}  // namespace mplayer
