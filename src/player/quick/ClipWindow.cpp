#include "ClipWindow.hpp"
#include "ClipController.hpp"
#include "PlayerController.hpp"
#include "PlayerIconProvider.hpp"

#include <QCloseEvent>
#include <QDebug>
#include <QEvent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickImageProvider>
#include <QScreen>
#include <QVariant>

namespace mplayer::quick {

ClipWindow::ClipWindow(PlayerController* player, QWindow* transientParent)
    : controller_(new ClipController(player->session(), player->caption(), this)) {
    setObjectName(QStringLiteral("clipWindow"));
    setFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
             Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
    if (transientParent != nullptr) setTransientParent(transientParent);
    setColor(Qt::black);
    setResizeMode(QQuickView::SizeRootObjectToView);
    setMinimumSize(QSize(760, 680));
    resize(1060, 820);
    installPlayerIcons(engine());
    engine()->addImageProvider(QStringLiteral("clip-frames"), controller_->createImageProvider());
    setInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(player)},
        {QStringLiteral("editor"), QVariant::fromValue(controller_)},
    });
    loadFromModule(QStringLiteral("Mikai.Player"), QStringLiteral("ClipView"));
    connect(controller_, &ClipController::closeRequested, this, [this] { close(); }, Qt::QueuedConnection);
    retranslate();
}

ClipWindow::~ClipWindow() { controller_->shutdown(); }

void ClipWindow::retranslate() {
    setTitle(controller_->caption().trimmed().isEmpty()
                 ? tr("Cut moment") : tr("Cut moment — %1").arg(controller_->caption()));
    controller_->retranslate();
    engine()->retranslate();
}

bool ClipWindow::present() {
    if (rootObject() == nullptr) {
        for (const auto& error : errors()) qWarning().noquote() << error.toString();
        return false;
    }
    if (!isVisible() && transientParent() != nullptr) {
        QScreen* targetScreen = transientParent()->screen();
        if (targetScreen != nullptr) {
            setScreen(targetScreen);
            const QRect available = targetScreen->availableGeometry();
            const QSize fitted(qMin(width(), available.width()), qMin(height(), available.height()));
            resize(fitted.expandedTo(minimumSize()));
            setPosition(available.center() - QPoint(width() / 2, height() / 2));
        }
    }
    if (visibility() == QWindow::Minimized) showNormal();
    else show();
    raise();
    requestActivate();
    return true;
}

void ClipWindow::closeEvent(QCloseEvent* event) {
    controller_->shutdown();
    QQuickView::closeEvent(event);
    if (event->isAccepted() && !dismissed_) {
        dismissed_ = true;
        emit dismissed();
    }
}

bool ClipWindow::event(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && controller_ != nullptr) retranslate();
    return QQuickView::event(event);
}

}
