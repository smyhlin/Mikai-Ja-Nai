#pragma once

#include <QDialog>
#include <QPointer>

QT_BEGIN_NAMESPACE
class QEvent;
class QCloseEvent;
QT_END_NAMESPACE

namespace mplayer {
class VideoSurface;
namespace shell {

class ClipDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ClipDialog(VideoSurface* source, const QString& caption, QWidget* parent = nullptr);
    ~ClipDialog() override;

protected:
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    class Private;
    Private* d_ = nullptr;
};

}  // namespace shell
}  // namespace mplayer
