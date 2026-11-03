#include "dialogs/SettingsDialog.hpp"

#include "dialogs/settings/SettingsUi.hpp"

#include <dialogs/ComponentStoreTab.hpp>

#include <QScrollArea>
#include <QVBoxLayout>

namespace mikai::app {

QWidget* SettingsDialog::buildComponentsTab() {
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("componentsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    auto* page = new QWidget(scroll);
    page->setObjectName(QStringLiteral("componentsPage"));
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    ComponentStoreTab* tab = nullptr;
    const bool adopted = adoptedComponents_ != nullptr;
    if (adopted) {
        adoptedComponentsTab_ = true;
        adoptedComponentsHome_ = adoptedComponents_->parentWidget();
        tab = adoptedComponents_;
        tab->setParent(page);
        tab->show();
    } else {
        tab = new ComponentStoreTab(page);
        tab->setClient(&client_);
    }
    componentStoreTab_ = tab;
    if (componentProvisioner_ != nullptr) tab->setComponentProvisioner(componentProvisioner_);
    // The runner is bound last: binding it is what starts the tab's first
    // inventory scan, and any later setter bumps the run generation, which
    // cancels that scan without a replacement.
    if (!adopted) tab->setRunner(&runner_);
    root->addWidget(tab);

    scroll->setWidget(page);
    componentsScroll_ = scroll;
    return scroll;
}

}  // namespace mikai::app
