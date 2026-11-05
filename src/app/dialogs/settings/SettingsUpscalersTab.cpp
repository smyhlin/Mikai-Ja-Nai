#include "dialogs/SettingsDialog.hpp"

#include "dialogs/UpscalersPanel.hpp"

namespace mikai::app {

QWidget* SettingsDialog::buildUpscalersTab() {
    auto* panel = new UpscalersPanel(this);
    panel->setRunner(&runner_);
    panel->setClient(&client_);
    return panel;
}

}  // namespace mikai::app
