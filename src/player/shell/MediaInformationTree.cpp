#include "MediaInformationTree.hpp"

#include "ShellTheme.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QHash>
#include <QHeaderView>
#include <QKeyEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTextOption>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <cmath>

namespace mplayer::shell {
namespace {

constexpr int nodeIdRole = Qt::UserRole;

// QTreeView's wordWrap alone does not grow rows to fit their contents. Measure
// and paint the same text layout, including paths with no word boundaries.
class WrappedInformationDelegate final : public QStyledItemDelegate {
public:
    explicit WrappedInformationDelegate(QTreeWidget* tree)
        : QStyledItemDelegate(tree), tree_(tree) {}

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem styled(option);
        initStyleOption(&styled, index);
        int width = tree_->columnWidth(index.column());
        if (index.column() == 0) {
            int depth = 1;
            for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) ++depth;
            width -= depth * tree_->indentation();
        }
        styled.rect = QRect(0, 0, std::max(1, width), option.fontMetrics.height());
        const QStyle* style = tree_->style();
        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &styled, tree_);
        QTextLayout layout(styled.text, styled.font);
        const qreal height = arrange(layout, std::max(1, textRect.width()), styled.direction);
        const int verticalMargin = style->pixelMetric(QStyle::PM_FocusFrameVMargin, &styled, tree_);
        return QSize(width, std::max(option.fontMetrics.height(), static_cast<int>(std::ceil(height)))
                                + 2 * verticalMargin);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem styled(option);
        initStyleOption(&styled, index);
        const QString text = styled.text;
        const QStyle* style = tree_->style();
        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &styled, tree_);
        styled.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &styled, painter, tree_);
        QTextLayout layout(text, styled.font);
        const qreal height = arrange(layout, std::max(1, textRect.width()), styled.direction);
        const QPalette::ColorGroup group = !(styled.state & QStyle::State_Enabled)
            ? QPalette::Disabled : (styled.state & QStyle::State_Active) ? QPalette::Active : QPalette::Inactive;
        const QPalette::ColorRole role = styled.state & QStyle::State_Selected
            ? QPalette::HighlightedText : QPalette::Text;
        painter->save();
        painter->setClipRect(option.rect);
        painter->setPen(styled.palette.color(group, role));
        layout.draw(painter, QPointF(textRect.left(), textRect.top() + std::max(0.0, (textRect.height() - height) / 2.0)));
        painter->restore();
    }

private:
    static qreal arrange(QTextLayout& layout, int width, Qt::LayoutDirection direction) {
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        option.setTextDirection(direction);
        layout.setTextOption(option);
        layout.beginLayout();
        qreal height = 0;
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(width);
            line.setPosition(QPointF(0, height));
            height += line.height();
        }
        layout.endLayout();
        return height;
    }

    QTreeWidget* tree_;
};

QTreeWidgetItem* appendNode(QTreeWidget* tree, QTreeWidgetItem* parent,
                            const MediaInformationNode& node,
                            const QHash<QString, bool>& expansion,
                            QHash<QString, QTreeWidgetItem*>& items) {
    auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
    item->setText(0, node.label);
    item->setText(1, node.value);
    item->setData(0, nodeIdRole, node.id);
    // Tooltips auto-detect rich text; escape metadata so tags remain literal.
    const auto tooltip = [](const QString& text) {
        QString escaped = text.toHtmlEscaped();
        escaped.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        return QStringLiteral("<qt>%1</qt>").arg(escaped);
    };
    item->setToolTip(0, tooltip(node.label));
    item->setToolTip(1, tooltip(node.value));
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    if (parent == nullptr) {
        QFont font = tree->font();
        font.setBold(true);
        item->setFont(0, font);
    }
    items.insert(node.id, item);
    for (const MediaInformationNode& child : node.children)
        appendNode(tree, item, child, expansion, items);
    if (!node.children.isEmpty()) item->setExpanded(expansion.value(node.id, parent == nullptr));
    return item;
}

}

MediaInformationTree::MediaInformationTree(QWidget* parent) : QTreeWidget(parent) {
    setObjectName(QStringLiteral("playerMediaInformationTree"));
    setFont(uiFont());
    setColumnCount(2);
    setHeaderLabels({
        QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Property"),
        QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Value")});
    setRootIsDecorated(true);
    setItemsExpandable(true);
    setUniformRowHeights(false);
    setWordWrap(true);
    setTextElideMode(Qt::ElideNone);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setIndentation(2 * kFrameMargin);
    setItemDelegate(new WrappedInformationDelegate(this));
    header()->setSectionsMovable(false);
    header()->setSectionResizeMode(0, QHeaderView::Interactive);
    header()->setSectionResizeMode(1, QHeaderView::Stretch);
    header()->setStretchLastSection(true);
    header()->setMinimumSectionSize(8 * kFrameMargin);
    setColumnWidth(0, 24 * kFrameMargin);
    connect(header(), &QHeaderView::sectionResized, this, [this] {
        // Interactive header dragging must obey the same bound as window
        // resizing; otherwise the value column disappears with no scrollbar.
        const int maximumPropertyWidth =
            std::max(header()->minimumSectionSize(), viewport()->width() / 2);
        if (columnWidth(0) > maximumPropertyWidth)
            setColumnWidth(0, maximumPropertyWidth);
        scheduleDelayedItemsLayout();
    });
}

void MediaInformationTree::setDocument(const MediaInformationDocument& document) {
    QHash<QString, bool> expansion;
    for (QTreeWidgetItemIterator iterator(this); *iterator; ++iterator) {
        QTreeWidgetItem* item = *iterator;
        if (item->childCount() > 0) expansion.insert(item->data(0, nodeIdRole).toString(), item->isExpanded());
    }
    const QString currentId = currentItem() ? currentItem()->data(0, nodeIdRole).toString() : QString();
    const bool currentSelected = currentItem() != nullptr && currentItem()->isSelected();
    const int column = currentColumn();
    const int scroll = verticalScrollBar()->value();
    QTreeWidgetItem* anchor = itemAt(QPoint(viewport()->width() / 2, 0));
    const QString anchorId = anchor ? anchor->data(0, nodeIdRole).toString() : QString();
    const int anchorOffset = anchor ? visualItemRect(anchor).top() : 0;
    const bool updatesWereEnabled = updatesEnabled();
    setUpdatesEnabled(false);
    const QSignalBlocker blocker(this);
    clear();
    setHeaderLabels({
        QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Property"),
        QCoreApplication::translate("mplayer::shell::MediaInformationDialog", "Value")});
    QHash<QString, QTreeWidgetItem*> items;
    for (const MediaInformationNode& node : document)
        appendNode(this, nullptr, node, expansion, items);
    if (QTreeWidgetItem* current = items.value(currentId)) {
        setCurrentItem(current, std::max(0, column), QItemSelectionModel::NoUpdate);
        current->setSelected(currentSelected);
    }
    // Resolve variable-height rows before restoring the visible stable-ID anchor.
    doItemsLayout();
    if (QTreeWidgetItem* restoredAnchor = items.value(anchorId)) {
        scrollToItem(restoredAnchor, QAbstractItemView::PositionAtTop);
        verticalScrollBar()->setValue(verticalScrollBar()->value() - anchorOffset);
    } else {
        verticalScrollBar()->setValue(scroll);
    }
    setUpdatesEnabled(updatesWereEnabled);
}

void MediaInformationTree::resizeEvent(QResizeEvent* event) {
    QTreeWidget::resizeEvent(event);
    // A property column must never consume the whole viewport after shrinking.
    const int available = viewport()->width();
    const int maximumPropertyWidth = std::max(header()->minimumSectionSize(), available / 2);
    if (columnWidth(0) > maximumPropertyWidth) setColumnWidth(0, maximumPropertyWidth);
    scheduleDelayedItemsLayout();
}

void MediaInformationTree::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Copy) && currentItem() != nullptr) {
        const QString property = currentItem()->text(0);
        const QString value = currentItem()->text(1);
        QApplication::clipboard()->setText(value.isEmpty() ? property : property + QStringLiteral(": ") + value);
        event->accept();
        return;
    }
    QTreeWidget::keyPressEvent(event);
}

}
