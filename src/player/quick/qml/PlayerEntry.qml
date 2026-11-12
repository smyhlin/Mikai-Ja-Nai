import QtQuick
import Mikai.Player

// The executable module's real scene root (`Mikai.App/PlayerEntry`). It is the
// only root PlayerWindow loads; the window supplies this session's controller
// through setInitialProperties before loadFromModule, which satisfies the
// required property PlayerView inherits.
PlayerView {
    id: entry
}
