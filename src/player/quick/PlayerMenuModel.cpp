#include "PlayerMenuModel.hpp"

#include <QtCore/QDebug>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QThread>

#include <utility>

namespace mplayer {
namespace quick {

namespace {

void collectChangedRoles(const PlayerMenuModel::Row& before, const PlayerMenuModel::Row& after,
                         QList<int>& roles) {
    if (before.label != after.label) roles.append(PlayerMenuModel::LabelRole);
    if (before.detail != after.detail) roles.append(PlayerMenuModel::DetailRole);
    if (before.tooltip != after.tooltip) roles.append(PlayerMenuModel::TooltipRole);
    if (before.icon != after.icon) roles.append(PlayerMenuModel::IconRole);
    if (before.enabled != after.enabled) roles.append(PlayerMenuModel::EnabledRole);
    if (before.selected != after.selected) roles.append(PlayerMenuModel::SelectedRole);
    if (before.submenu != after.submenu) roles.append(PlayerMenuModel::SubmenuRole);
    if (before.heading != after.heading) roles.append(PlayerMenuModel::HeadingRole);
    if (before.action != after.action) roles.append(PlayerMenuModel::ActionRole);
    if (before.value != after.value) roles.append(PlayerMenuModel::ValueRole);
}

/// A section caption and a disabled row are shown but cannot carry the mark.
bool selectable(const PlayerMenuModel::Row& row) { return row.enabled && !row.heading; }

}  // namespace

PlayerMenuModel::PlayerMenuModel(QObject* parent) : QAbstractListModel(parent) {}

int PlayerMenuModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(rows_.size());
}

QVariant PlayerMenuModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.column() != 0 || index.row() < 0 ||
        index.row() >= rows_.size()) {
        return QVariant();
    }
    const Row& row = rows_.at(index.row());
    switch (role) {
        case Qt::DisplayRole:
        case LabelRole: return row.label;
        case Qt::ToolTipRole:
        case TooltipRole: return row.tooltip;
        case IdRole: return row.id;
        case DetailRole: return row.detail;
        case IconRole: return row.icon;
        case EnabledRole: return row.enabled;
        case SelectedRole: return row.selected;
        case SubmenuRole: return row.submenu;
        case HeadingRole: return row.heading;
        case ActionRole: return int(row.action);
        case ValueRole: return row.value;
        default: return QVariant();
    }
}

QHash<int, QByteArray> PlayerMenuModel::roleNames() const {
    return {
        {IdRole, "id"},
        {LabelRole, "label"},
        {DetailRole, "detail"},
        {TooltipRole, "tooltip"},
        {IconRole, "icon"},
        {EnabledRole, "enabled"},
        {SelectedRole, "selected"},
        {SubmenuRole, "submenu"},
        {HeadingRole, "heading"},
        {ActionRole, "action"},
        {ValueRole, "value"},
    };
}

PlayerMenuModel::Row PlayerMenuModel::row(int index) const {
    if (index < 0 || index >= rows_.size()) return Row();
    return rows_.at(index);
}

bool PlayerMenuModel::isSelectable(int index) const {
    return index >= 0 && index < rows_.size() && selectable(rows_.at(index));
}

int PlayerMenuModel::indexOfId(const QString& id) const {
    if (id.isEmpty()) return -1;
    for (qsizetype i = 0; i < rows_.size(); ++i) {
        if (rows_.at(i).id == id) return int(i);
    }
    return -1;
}

void PlayerMenuModel::setRows(QVector<Row> rows) {
    Q_ASSERT(QThread::currentThread() == thread());

    // The id is the diff key, so a malformed row cannot be stored.
    QVector<Row> accepted;
    accepted.reserve(rows.size());
    QSet<QString> seen;
    for (Row& entry : rows) {
        if (entry.id.isEmpty()) {
            qWarning("PlayerMenuModel: dropping a row without an id (label \"%s\")",
                     qUtf8Printable(entry.label));
            continue;
        }
        if (seen.contains(entry.id)) {
            qWarning("PlayerMenuModel: dropping duplicate row id \"%s\"",
                     qUtf8Printable(entry.id));
            continue;
        }
        seen.insert(entry.id);
        accepted.append(std::move(entry));
    }
    rows = std::move(accepted);

    // The incoming flags are authoritative: the first eligible selected row
    // carries the mark, and a list with none clears it, so a rebuild can never
    // inherit a selection the caller no longer states.
    QString nextSelected;
    for (const Row& entry : rows) {
        if (entry.selected && selectable(entry)) {
            nextSelected = entry.id;
            break;
        }
    }
    for (Row& entry : rows) {
        entry.selected = !nextSelected.isEmpty() && entry.id == nextSelected;
    }

    for (qsizetype i = rows_.size() - 1; i >= 0; --i) {
        if (seen.contains(rows_.at(i).id)) continue;
        beginRemoveRows(QModelIndex(), int(i), int(i));
        rows_.removeAt(i);
        endRemoveRows();
    }

    for (qsizetype i = 0; i < rows.size(); ++i) {
        if (!(i < rows_.size() && rows_.at(i).id == rows.at(i).id)) {
            const int from = indexOfId(rows.at(i).id);
            if (from >= 0) {
                // Qt's destinationChild is measured before removing the source
                // row. Moving down therefore targets one past the final index.
                const int destination = from < i ? int(i) + 1 : int(i);
                beginMoveRows(QModelIndex(), from, from, QModelIndex(), destination);
                rows_.move(from, i);
                endMoveRows();
            } else {
                beginInsertRows(QModelIndex(), int(i), int(i));
                rows_.insert(i, rows.at(i));
                endInsertRows();
                continue;
            }
        }
        QList<int> changedRoles;
        collectChangedRoles(rows_.at(i), rows.at(i), changedRoles);
        if (changedRoles.isEmpty()) continue;
        rows_[i] = rows.at(i);
        emit dataChanged(index(int(i), 0), index(int(i), 0), changedRoles);
    }

    const bool selectionChanged = selectedId_ != nextSelected;
    selectedId_ = nextSelected;
    if (selectionChanged) emit selectedIdChanged();
}

void PlayerMenuModel::setSelectedId(const QString& id) {
    Q_ASSERT(QThread::currentThread() == thread());

    int target = -1;
    if (!id.isEmpty()) {
        const int found = indexOfId(id);
        if (found >= 0 && selectable(rows_.at(found))) target = found;
    }

    if (rows_.isEmpty() && selectedId_.isEmpty()) return;

    QList<int> touched;
    for (qsizetype i = 0; i < rows_.size(); ++i) {
        const bool selected = int(i) == target;
        if (rows_.at(i).selected == selected) continue;
        rows_[i].selected = selected;
        touched.append(int(i));
    }
    for (int i : touched) {
        emit dataChanged(index(i, 0), index(i, 0), {SelectedRole});
    }
    const QString next = target >= 0 ? rows_.at(target).id : QString();
    if (selectedId_ == next) return;
    selectedId_ = next;
    emit selectedIdChanged();
}

}  // namespace quick
}  // namespace mplayer
