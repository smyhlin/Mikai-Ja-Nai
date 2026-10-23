#include "MediaInformationOverview.hpp"

#include "ShellTheme.hpp"

#include <QAbstractTextDocumentLayout>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPalette>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStringList>
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
        QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        setMinimumWidth(0);
        connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
                this, [this] { updateGeometry(); });
    }

    QSize sizeHint() const override { return {24 * fontMetrics().averageCharWidth(), fontMetrics().lineSpacing()}; }
    QSize minimumSizeHint() const override { return {0, fontMetrics().lineSpacing()}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override {
        document()->setTextWidth(std::max(1, width));
        return std::max(fontMetrics().lineSpacing(), static_cast<int>(std::ceil(document()->size().height())));
    }

    void setValue(const QString& value) {
        if (value == toPlainText()) return;
        setPlainText(value);
        QString escaped = value.toHtmlEscaped();
        escaped.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        setToolTip(QStringLiteral("<qt>%1</qt>").arg(escaped));
        updateGeometry();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QTextEdit::resizeEvent(event);
        updateGeometry();
    }
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
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
    if (!sameStructure) {
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
                widgets.label = headingLabel(card);
                grid->addWidget(widgets.label, gridRow++, 0, 1, 2);
                widgets.value = new FactValue(card);
                grid->addWidget(widgets.value, gridRow++, 0, 1, 2);
                layout->addWidget(frame);
            } else if (row.kind == RowKind::Heading) {
                widgets.label = headingLabel(card);
                grid->addWidget(widgets.label, gridRow++, 0, 1, 2);
            } else {
                widgets.value = new FactValue(card);
                if (row.kind == RowKind::Fact) {
                    widgets.label = new QLabel(card);
                    widgets.label->setTextFormat(Qt::PlainText);
                    widgets.label->setWordWrap(true);
                    widgets.label->setMinimumWidth(0);
                    widgets.label->setMaximumWidth(16 * fontMetrics().averageCharWidth());
                    QPalette colors = widgets.label->palette();
                    colors.setColor(QPalette::WindowText, w70());
                    widgets.label->setPalette(colors);
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
        if (widgets.label != nullptr) widgets.label->setText(row.label);
        if (widgets.value != nullptr) {
            widgets.value->setVisible(!row.value.isEmpty());
            widgets.value->setValue(row.value);
            widgets.value->setAccessibleName(row.label);
        }
    }
    content_->document = document;
    content_->rows = rows;
    if (widget() != nullptr && widget()->layout() != nullptr) widget()->layout()->activate();
    verticalScrollBar()->setValue(scroll);
}

}
