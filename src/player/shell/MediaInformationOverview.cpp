#include "MediaInformationOverview.hpp"

#include "ShellTheme.hpp"

#include <QApplication>
#include <QFrame>
#include <QHash>
#include <QGridLayout>
#include <QLabel>
#include <QPalette>
#include <QPointer>
#include <QScrollBar>
#include <QStringList>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextOption>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace mplayer::shell {
namespace {

// Wrap unbroken filenames without inserting characters into copied text.
class FactValue final : public QTextEdit {
public:
    explicit FactValue(QWidget* parent) : QTextEdit(parent) {
        setReadOnly(true);
        setAcceptRichText(false);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        document()->setDocumentMargin(0);
        measurement_.setDocumentMargin(0);
        measurement_.setDefaultTextOption(document()->defaultTextOption());
        QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        setMinimumWidth(0);
    }

    QSize sizeHint() const override { return {24 * fontMetrics().averageCharWidth(), fontMetrics().lineSpacing()}; }
    QSize minimumSizeHint() const override { return {0, fontMetrics().lineSpacing()}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override {
        // A layout can query several candidate widths. Never resize the live
        // editor document here: doing so emits documentSizeChanged during the
        // parent's layout pass and disturbs its viewport/selection geometry.
        measurement_.setDefaultFont(font());
        measurement_.setTextWidth(std::max(1, width));
        return std::max(fontMetrics().lineSpacing(),
                        static_cast<int>(std::ceil(measurement_.size().height())));
    }

    void setValue(const QString& value) {
        const QString previous = toPlainText();
        if (value == previous) return;
        const QTextCursor selection = textCursor();
        qsizetype prefix = 0;
        const qsizetype commonSize = std::min(previous.size(), value.size());
        while (prefix < commonSize && previous[prefix] == value[prefix]) ++prefix;
        qsizetype suffix = 0;
        while (suffix < commonSize - prefix &&
               previous[previous.size() - suffix - 1] == value[value.size() - suffix - 1])
            ++suffix;
        const int oldEnd = static_cast<int>(previous.size() - suffix);
        const int newEnd = static_cast<int>(value.size() - suffix);
        const auto updatedPosition = [prefix, oldEnd, newEnd](int position) {
            if (position <= prefix) return position;
            if (position >= oldEnd) return position + newEnd - oldEnd;
            return std::min(position, newEnd);
        };
        QTextCursor edit(document());
        edit.setPosition(static_cast<int>(prefix));
        edit.setPosition(oldEnd, QTextCursor::KeepAnchor);
        edit.insertText(value.mid(prefix, value.size() - prefix - suffix));
        QTextCursor restored(document());
        restored.setPosition(updatedPosition(selection.anchor()));
        restored.setPosition(updatedPosition(selection.position()), QTextCursor::KeepAnchor);
        setTextCursor(restored);
        measurement_.setPlainText(value);
        QString escaped = value.toHtmlEscaped();
        escaped.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        setToolTip(QStringLiteral("<qt>%1</qt>").arg(escaped));
        updateGeometry();
    }

protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }

private:
    mutable QTextDocument measurement_;
};

enum class RowKind { Card, Heading, Summary, Fact };

struct OverviewRow {
    QString id;
    QString label;
    QString value;
    RowKind kind;
};

QVector<OverviewRow> overviewRows(const MediaInformationDocument& document) {
    QVector<OverviewRow> rows;
    for (const MediaInformationNode& section : document) {
        rows.push_back({section.id, section.label, section.value, RowKind::Card});
        const bool tracks = section.id == QStringLiteral("audio") || section.id == QStringLiteral("subtitles");
        for (const MediaInformationNode& node : section.children) {
            if (section.id == QStringLiteral("status")) {
                rows.push_back({node.id, node.label, node.value, RowKind::Summary});
                continue;
            }
            if (!tracks || node.children.isEmpty()) {
                rows.push_back({node.id, node.label, node.value, RowKind::Fact});
                continue;
            }
            rows.push_back({node.id, node.label, {}, RowKind::Heading});
            rows.push_back({node.id + QStringLiteral(".overview-summary"), node.label, node.value, RowKind::Summary});
            QStringList flags;
            for (const MediaInformationNode& fact : node.children) {
                if (fact.id.endsWith(QStringLiteral(".channels")) ||
                    fact.id.endsWith(QStringLiteral(".default")) ||
                    fact.id.endsWith(QStringLiteral(".forced")) ||
                    fact.id.endsWith(QStringLiteral(".external"))) {
                    flags.push_back(fact.label + QStringLiteral(": ") + fact.value);
                }
            }
            if (!flags.isEmpty()) {
                rows.push_back({node.id + QStringLiteral(".overview-flags"), node.label,
                                flags.join(QStringLiteral(" · ")), RowKind::Summary});
            }
            for (const MediaInformationNode& fact : node.children) {
                if (fact.id.endsWith(QStringLiteral(".external-file")))
                    rows.push_back({fact.id, fact.label, fact.value, RowKind::Fact});
            }
        }
    }
    return rows;
}

QLabel* headingLabel(QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setFont(uiFont(10, true));
    label->setMinimumWidth(0);
    return label;
}

}

struct MediaInformationOverview::Content {
    struct Widgets {
        QLabel* label = nullptr;
        FactValue* value = nullptr;
    };
    MediaInformationDocument document;
    QVector<OverviewRow> rows;
    QVector<Widgets> widgets;
};

MediaInformationOverview::MediaInformationOverview(QWidget* parent)
    : QScrollArea(parent), content_(std::make_unique<Content>()) {
    setObjectName(QStringLiteral("playerMediaInformationOverview"));
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}

MediaInformationOverview::~MediaInformationOverview() = default;

void MediaInformationOverview::setDocument(const MediaInformationDocument& document) {
    if (content_->document == document && widget() != nullptr) return;
    const QVector<OverviewRow> rows = overviewRows(document);
    bool sameStructure = rows.size() == content_->rows.size() && widget() != nullptr;
    if (sameStructure) {
        for (qsizetype i = 0; i < rows.size(); ++i) {
            if (rows[i].id != content_->rows[i].id || rows[i].kind != content_->rows[i].kind) {
                sameStructure = false;
                break;
            }
        }
    }
    const int scroll = verticalScrollBar()->value();
    QString anchorId;
    int anchorOffset = 0;
    const auto rowWidget = [](const Content::Widgets& widgets) -> QWidget* {
        return widgets.value != nullptr && !widgets.value->isHidden()
            ? static_cast<QWidget*>(widgets.value) : widgets.label;
    };
    for (qsizetype i = 0; i < content_->widgets.size(); ++i) {
        QWidget* candidate = rowWidget(content_->widgets[i]);
        if (candidate == nullptr) continue;
        const int top = candidate->mapTo(viewport(), QPoint()).y();
        if (top + candidate->height() > 0) {
            anchorId = content_->rows[i].id;
            anchorOffset = top;
            break;
        }
    }
    QPointer<QWidget> focus = QApplication::focusWidget();
    if (!sameStructure) {
        // Loading/error/track sections can come and go while the user is
        // copying a different field. Move surviving editors instead of
        // destroying their documents and selections with the old layout.
        QHash<QString, qsizetype> previousRows;
        for (qsizetype i = 0; i < content_->rows.size(); ++i)
            previousRows.insert(content_->rows[i].id, i);
        const QVector<Content::Widgets> previousWidgets = content_->widgets;
        auto* body = new QWidget;
        auto* layout = new QVBoxLayout(body);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(kFrameMargin);
        content_->widgets.clear();
        content_->widgets.reserve(rows.size());
        QGridLayout* grid = nullptr;
        QWidget* card = nullptr;
        int gridRow = 0;
        for (const OverviewRow& row : rows) {
            Content::Widgets widgets;
            const auto previous = previousRows.constFind(row.id);
            if (previous != previousRows.cend() &&
                content_->rows[*previous].kind == row.kind)
                widgets = previousWidgets[*previous];
            if (row.kind == RowKind::Card) {
                auto* frame = new QFrame(body);
                frame->setFrameShape(QFrame::StyledPanel);
                frame->setAutoFillBackground(true);
                QPalette colors = frame->palette();
                colors.setColor(QPalette::Window, bg0());
                colors.setColor(QPalette::WindowText, w95());
                colors.setColor(QPalette::Base, bg0());
                colors.setColor(QPalette::Text, w95());
                frame->setPalette(colors);
                card = frame;
                grid = new QGridLayout(frame);
                grid->setContentsMargins(kFrameMargin, kFrameMargin, kFrameMargin, kFrameMargin);
                grid->setHorizontalSpacing(2 * kFrameMargin);
                grid->setVerticalSpacing(kFrameMargin / 2);
                grid->setColumnStretch(1, 1);
                gridRow = 0;
                if (widgets.label == nullptr) widgets.label = headingLabel(card);
                else widgets.label->setParent(card);
                grid->addWidget(widgets.label, gridRow++, 0, 1, 2);
                if (widgets.value == nullptr) widgets.value = new FactValue(card);
                else widgets.value->setParent(card);
                grid->addWidget(widgets.value, gridRow++, 0, 1, 2);
                layout->addWidget(frame);
            } else if (row.kind == RowKind::Heading) {
                if (widgets.label == nullptr) widgets.label = headingLabel(card);
                else widgets.label->setParent(card);
                grid->addWidget(widgets.label, gridRow++, 0, 1, 2);
            } else {
                if (widgets.value == nullptr) widgets.value = new FactValue(card);
                else widgets.value->setParent(card);
                if (row.kind == RowKind::Fact) {
                    if (widgets.label == nullptr) {
                        widgets.label = new QLabel(card);
                        widgets.label->setTextFormat(Qt::PlainText);
                        widgets.label->setWordWrap(true);
                        widgets.label->setMinimumWidth(0);
                        widgets.label->setMaximumWidth(16 * fontMetrics().averageCharWidth());
                        QPalette colors = widgets.label->palette();
                        colors.setColor(QPalette::WindowText, w70());
                        widgets.label->setPalette(colors);
                    } else {
                        widgets.label->setParent(card);
                    }
                    grid->addWidget(widgets.label, gridRow, 0, Qt::AlignTop);
                    grid->addWidget(widgets.value, gridRow++, 1);
                } else {
                    grid->addWidget(widgets.value, gridRow++, 0, 1, 2);
                }
            }
            content_->widgets.push_back(widgets);
        }
        layout->addStretch();
        setWidget(body);
    }
    for (qsizetype i = 0; i < rows.size(); ++i) {
        const OverviewRow& row = rows[i];
        const Content::Widgets& widgets = content_->widgets[i];
        if (widgets.label != nullptr) {
            widgets.label->setText(row.label);
            widgets.label->show();
        }
        if (widgets.value != nullptr) {
            widgets.value->setVisible(!row.value.isEmpty());
            widgets.value->setValue(row.value);
            widgets.value->setAccessibleName(row.label);
        }
    }
    content_->document = document;
    content_->rows = rows;
    if (widget() != nullptr && widget()->layout() != nullptr) widget()->layout()->activate();
    if (focus != nullptr && isAncestorOf(focus)) focus->setFocus(Qt::OtherFocusReason);
    bool restoredAnchor = false;
    for (qsizetype i = 0; !anchorId.isEmpty() && i < rows.size(); ++i) {
        if (rows[i].id != anchorId) continue;
        if (QWidget* anchor = rowWidget(content_->widgets[i])) {
            const int top = anchor->mapTo(viewport(), QPoint()).y();
            verticalScrollBar()->setValue(verticalScrollBar()->value() + top - anchorOffset);
            restoredAnchor = true;
        }
        break;
    }
    if (!restoredAnchor) verticalScrollBar()->setValue(scroll);
}

}
