#include "TestQuickMenuModel.hpp"

#include "player/quick/PlayerMenuModel.hpp"

#include <QtCore/QPersistentModelIndex>
#include <QtCore/QStringList>
#include <QtCore/QVector>

using mplayer::quick::PlayerMenuModel;

namespace {

using Row = PlayerMenuModel::Row;

Row menuRow(const QString& id, const QString& label = QString()) {
    Row row;
    row.id = id;
    row.label = label.isEmpty() ? id : label;
    row.action = PlayerMenuModel::Action::SelectTrack;
    return row;
}

QStringList markedIds(const PlayerMenuModel& model) {
    QStringList ids;
    for (int i = 0; i < model.rowCount(); ++i) {
        const QModelIndex index = model.index(i, 0);
        if (model.data(index, PlayerMenuModel::SelectedRole).toBool()) {
            ids.append(model.data(index, PlayerMenuModel::IdRole).toString());
        }
    }
    return ids;
}

}  // namespace

// The old merge kept selectedId_ whenever that id survived, so a rebuild that
// stated a different row as selected left the old mark in place. The mark has
// to follow the incoming flags, because that is what a QML delegate draws.
void TestQuickMenuModel::incomingFlagsMoveTheMarkWhenTheSelectionChanges() {
    PlayerMenuModel model;
    model.setRows({menuRow(QStringLiteral("a")), menuRow(QStringLiteral("b")),
                   menuRow(QStringLiteral("c"))});
    model.setSelectedId(QStringLiteral("a"));
    QCOMPARE(markedIds(model), QStringList{QStringLiteral("a")});

    QVector<Row> next{menuRow(QStringLiteral("a")), menuRow(QStringLiteral("b")),
                      menuRow(QStringLiteral("c"))};
    next[1].selected = true;
    model.setRows(next);

    QCOMPARE(markedIds(model), QStringList{QStringLiteral("b")});
    QCOMPARE(model.selectedId(), QStringLiteral("b"));
    QVERIFY(markedIds(model).size() <= 1);
    QCOMPARE(model.data(model.index(0, 0), PlayerMenuModel::SelectedRole).toBool(), false);
    QCOMPARE(model.data(model.index(2, 0), PlayerMenuModel::SelectedRole).toBool(), false);
}

void TestQuickMenuModel::selectingAnotherExistingRowMovesTheMark() {
    PlayerMenuModel model;
    model.setRows({menuRow(QStringLiteral("a")), menuRow(QStringLiteral("b"))});

    model.setSelectedId(QStringLiteral("a"));
    QCOMPARE(markedIds(model), QStringList{QStringLiteral("a")});

    model.setSelectedId(QStringLiteral("b"));
    QCOMPARE(markedIds(model), QStringList{QStringLiteral("b")});
    QVERIFY(markedIds(model).size() <= 1);
    QCOMPARE(model.data(model.index(0, 0), PlayerMenuModel::SelectedRole).toBool(), false);
    QVERIFY(model.data(model.index(1, 0), PlayerMenuModel::SelectedRole).toBool());
}

void TestQuickMenuModel::aRebuildWithoutSelectionClearsTheMark() {
    PlayerMenuModel model;
    QVector<Row> first{menuRow(QStringLiteral("a")), menuRow(QStringLiteral("b"))};
    first[1].selected = true;
    model.setRows(first);
    QCOMPARE(model.selectedId(), QStringLiteral("b"));

    model.setRows({menuRow(QStringLiteral("a")), menuRow(QStringLiteral("b"))});

    QVERIFY(markedIds(model).isEmpty());
    QVERIFY(model.selectedId().isEmpty());
}

void TestQuickMenuModel::aRemovedSelectedRowLeavesNoStaleMark() {
    PlayerMenuModel model;
    QVector<Row> first{menuRow(QStringLiteral("a")), menuRow(QStringLiteral("b"))};
    first[1].selected = true;
    model.setRows(first);
    QCOMPARE(model.selectedId(), QStringLiteral("b"));

    model.setRows({menuRow(QStringLiteral("a"))});

    QCOMPARE(model.rowCount(), 1);
    QVERIFY(markedIds(model).isEmpty());
    QVERIFY(model.selectedId().isEmpty());
    QCOMPARE(model.indexOfId(QStringLiteral("b")), -1);
    QVERIFY(model.row(1).id.isEmpty());
}

void TestQuickMenuModel::disabledAndHeadingRowsNeverCarryTheMark() {
    PlayerMenuModel model;
    Row heading = menuRow(QStringLiteral("section"), QStringLiteral("Section"));
    heading.heading = true;
    heading.action = PlayerMenuModel::Action::None;
    heading.selected = true;
    Row disabled = menuRow(QStringLiteral("unavailable"));
    disabled.enabled = false;
    disabled.selected = true;

    model.setRows({heading, disabled, menuRow(QStringLiteral("playable"))});
    QVERIFY(markedIds(model).isEmpty());
    QVERIFY(model.selectedId().isEmpty());

    model.setSelectedId(QStringLiteral("unavailable"));
    QVERIFY(markedIds(model).isEmpty());
    QVERIFY(model.selectedId().isEmpty());
    model.setSelectedId(QStringLiteral("section"));
    QVERIFY(markedIds(model).isEmpty());

    model.setSelectedId(QStringLiteral("playable"));
    QCOMPARE(markedIds(model), QStringList{QStringLiteral("playable")});

    QCOMPARE(model.data(model.index(0, 0), PlayerMenuModel::HeadingRole).toBool(), true);
    QCOMPARE(model.data(model.index(1, 0), PlayerMenuModel::EnabledRole).toBool(), false);
}

void TestQuickMenuModel::reorderedRowsKeepTheirPersistentIdentity() {
    PlayerMenuModel model;
    model.setRows({menuRow(QStringLiteral("a")), menuRow(QStringLiteral("b")),
                   menuRow(QStringLiteral("c"))});
    const QPersistentModelIndex keep = model.index(1, 0);
    const QPersistentModelIndex removed = model.index(0, 0);
    QCOMPARE(keep.data(PlayerMenuModel::IdRole).toString(), QStringLiteral("b"));

    Row renamed = menuRow(QStringLiteral("b"), QStringLiteral("B renamed"));
    renamed.selected = true;
    model.setRows({menuRow(QStringLiteral("c")), renamed});

    QVERIFY(keep.isValid());
    QCOMPARE(keep.row(), 1);
    QCOMPARE(keep.data(PlayerMenuModel::IdRole).toString(), QStringLiteral("b"));
    QCOMPARE(keep.data(PlayerMenuModel::LabelRole).toString(), QStringLiteral("B renamed"));
    QCOMPARE(keep.data(PlayerMenuModel::SelectedRole).toBool(), true);
    QCOMPARE(model.selectedId(), QStringLiteral("b"));
    QVERIFY(!removed.isValid());
}

void TestQuickMenuModel::outOfRangeAndUnknownIdentitiesAreInert() {
    PlayerMenuModel model;
    model.setRows({menuRow(QStringLiteral("a"))});
    model.setSelectedId(QStringLiteral("a"));

    const QModelIndex invalid;
    QVERIFY(!model.data(invalid, PlayerMenuModel::IdRole).isValid());
    QVERIFY(!model.data(model.index(-1, 0), PlayerMenuModel::IdRole).isValid());
    QVERIFY(!model.data(model.index(5, 0), PlayerMenuModel::IdRole).isValid());
    QCOMPARE(model.row(-1).id, QString());
    QCOMPARE(model.row(model.rowCount()).id, QString());
    QCOMPARE(model.row(7).action, PlayerMenuModel::Action::None);
    QCOMPARE(model.indexOfId(QStringLiteral("missing")), -1);
    QCOMPARE(model.indexOfId(QString()), -1);

    model.setSelectedId(QStringLiteral("missing"));
    QVERIFY(markedIds(model).isEmpty());
    QVERIFY(model.selectedId().isEmpty());
    QCOMPARE(model.indexOfId(QStringLiteral("a")), 0);
}
