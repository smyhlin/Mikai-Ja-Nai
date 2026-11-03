// What the player window says about itself before it has a picture.
//
// The dialog is shown the moment an episode is picked, and the runtime
// resolution plus the first decode leave it empty for as long as they take. A
// bare dark window is indistinguishable from a failed one, so this overlay sits
// above the video surface - never inside its GL pass - and states the wait: a
// moving arc, the sentence for what is happening, and the episode it is
// happening to.
//
// It never takes the pointer: the picture below still receives clicks, and the
// overlay is not a modal block on the player.

#pragma once

#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace mplayer {
namespace shell {

class OpeningOverlay : public QWidget {
    Q_OBJECT

public:
    explicit OpeningOverlay(QWidget* parent = nullptr);

    /// Shows the wait with `headline` and `detail`. Both arrive localised; this
    /// widget holds no copy of the words and translates none of them.
    void showOpening(const QString& headline, const QString& detail);
    /// Restates only the detail line - the runtime layer's own sentence arriving
    /// later, for instance - without restarting the wait.
    void setDetail(const QString& detail);
    /// The wait is over: a frame is on screen, or there will never be one.
    void hideOpening();

    /// Whether the overlay is currently claiming the player is opening.
    bool isOpening() const { return shown_; }

    /// Exposed for diagnostics: the headline currently on screen.
    QString headline() const { return headline_; }
    QString detail() const { return detail_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QTimer* timer_ = nullptr;
    QString headline_;
    QString detail_;
    int phase_ = 0;
    bool shown_ = false;
};

}  // namespace shell
}  // namespace mplayer
