#pragma once

#include <QQuickView>

namespace mplayer::quick {
class ClipController;
class PlayerController;

class ClipWindow final : public QQuickView {
    Q_OBJECT
public:
    explicit ClipWindow(PlayerController* player, QWindow* transientParent = nullptr);
    ~ClipWindow() override;

    ClipController* controller() const { return controller_; }
    bool present();

signals:
    void dismissed();

protected:
    void closeEvent(QCloseEvent* event) override;
    bool event(QEvent* event) override;

private:
    void retranslate();
    ClipController* controller_ = nullptr;
    bool dismissed_ = false;
};

}
