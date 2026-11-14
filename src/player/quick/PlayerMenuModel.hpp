// Typed rows for the Quick player's menus, track lists, episode lists and skip
// rows. C++ owns every field: a QML delegate reads roles and hands the row
// index back to the controller, never an engine property name or an engine
// object. Row::value is the QAction::data equivalent - an opaque controller
// token that the model neither parses nor writes.

#pragma once

#include <QtCore/QAbstractListModel>
#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QModelIndex>
#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtCore/QVector>
#include <QtQml/qqmlregistration.h>

namespace mplayer {
namespace quick {

class PlayerMenuModel : public QAbstractListModel {
    Q_OBJECT
    QML_NAMED_ELEMENT(PlayerMenuModel)
    QML_UNCREATABLE("PlayerMenuModel instances are owned by PlayerController")
public:
    /// What a row does when activated. The only parameter a command takes is
    /// Row::value, which the controller interprets; an unknown value is the
    /// controller's refusal to report, never a property write from QML.
    enum class Action : int {
        None = 0,           ///< Heading/informational row: activation is refused.
        OpenMenu,           ///< Push the level named by value.
        CloseMenus,         ///< Dismiss the whole cascading stack.
        TogglePlayback,
        SeekTo,             ///< value: seconds, decimal.
        SeekBy,             ///< value: signed seconds, decimal.
        SetVolume,          ///< value: 0..100.
        ToggleMute,
        SetSpeed,           ///< value: playback rate multiplier.
        SetVideoScaleMode,  ///< value: VideoScaleMode ordinal, decimal.
        SelectTrack,        ///< value: opaque track/embed token; subtitles use "off".
        LoadExternalTrack,  ///< value: "audio" or "sub".
        SetSubtitleDelay,   ///< value: absolute seconds.
        ResetSubtitleDelay,
        SelectQuality,      ///< value: opaque variant token.
        SelectEpisode,      ///< value: episode index, decimal.
        PreviousEpisode,
        NextEpisode,
        SkipSegment,        ///< value: segment index, decimal.
        CaptureFrame,
        OpenCompare,
        CopyMediaLink,
        OpenContainingFolder,
        OpenMediaInformation,
        OpenClipWindow,
        OpenSubtitleSearch,
        DownloadSubtitle,   ///< value: subtitle-result row index, decimal.
        OpenShortcuts,
        TogglePiP,
        ToggleFullscreen,
        MakeDefaultVideoPlayer,
        SetEnhancement,     ///< value: catalogue row id; "off" turns the chain off.
        OpenEnhancementSettings,
        PrepareProcessing,
        OpenProcessingProviders,
        TurnOffProcessing,
        ExitPlayer,
    };
    Q_ENUM(Action)

    /// One menu row. `id` is the stable identity: setRows() diffs by it, so a
    /// rebuilt list keeps the delegates of unchanged ids alive.
    struct Row {
        QString id;
        QString label;
        QString detail;
        QString tooltip;
        /// Name for the `player-icons` image provider; empty means no glyph.
        QString icon;
        bool enabled = true;
        bool selected = false;
        bool submenu = false;
        /// The row is a section caption: `label` carries its text and the row
        /// is never selectable or activatable.
        bool heading = false;
        Action action = Action::None;
        QString value;
    };

    enum Role : int {
        IdRole = Qt::UserRole + 1,
        LabelRole,
        DetailRole,
        TooltipRole,
        IconRole,
        EnabledRole,
        SelectedRole,
        SubmenuRole,
        HeadingRole,
        ActionRole,
        ValueRole,
    };
    Q_ENUM(Role)

    explicit PlayerMenuModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    const QVector<Row>& rows() const { return rows_; }

    /// Out of range yields an inert row (empty id, Action::None), so a stale
    /// index can never dispatch a command.
    Row row(int index) const;
    /// Whether the current row may receive keyboard selection/activation.
    /// Uses model state rather than delegate state, including for virtualized
    /// rows that have no live QML delegate.
    Q_INVOKABLE bool isSelectable(int index) const;

    int indexOfId(const QString& id) const;

    /// GUI thread only. Diffs by Row::id: a surviving id is moved, never
    /// re-inserted, so unchanged rows keep their delegate. The mark comes from
    /// the incoming `selected` flags alone - the first enabled, non-heading
    /// selected row wins and every other flag is dropped.
    void setRows(QVector<Row> rows);

    QString selectedId() const { return selectedId_; }

    /// Marks the enabled, non-heading row with this id; empty or unknown
    /// clears. Emits selectedIdChanged() and the changed `selected` roles.
    void setSelectedId(const QString& id);

signals:
    void selectedIdChanged();

private:
    QVector<Row> rows_;
    QString selectedId_;
};

}  // namespace quick
}  // namespace mplayer
