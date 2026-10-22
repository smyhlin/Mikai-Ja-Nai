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
    if (adoptedComponents_ != nullptr) {
        adoptedComponentsTab_ = true;
        adoptedComponentsHome_ = adoptedComponents_->parentWidget();
        tab = adoptedComponents_;
        tab->setParent(page);
        tab->show();
    } else {
        tab = new ComponentStoreTab(page);
        tab->setRunner(&runner_);
        tab->setClient(&client_);
    }
    componentStoreTab_ = tab;
    if (componentProvisioner_ != nullptr) tab->setComponentProvisioner(componentProvisioner_);
    root->addWidget(tab);

    scroll->setWidget(page);
    componentsScroll_ = scroll;
    return scroll;
}

}  // namespace mikai::app
