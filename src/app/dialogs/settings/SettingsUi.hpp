#pragma once

#include "theme/Appearance.hpp"
#include "theme/Metrics.hpp"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace mikai::app {

/// Enables OpenType tabular figures (`tnum`) on a widget's font so live
/// counters, rate-limit timers, selection counts, and FPS tables never jitter
/// horizontally when digits change.
inline void applyTabularNumerals(QWidget* widget) {
    if (widget == nullptr) return;
    QFont font = widget->font();
    font.setFeature(QFont::Tag("tnum"), 1);
    font.setStyleStrategy(QFont::PreferAntialias);
    widget->setFont(font);
}

/// Applies a pointing-hand cursor and minimum interactive height to actionable
/// desktop controls while keeping focus policies keyboard-accessible.
inline void polishInteractiveControl(QWidget* control, int minHeight = theme::kSettingsControlSize) {
    if (control == nullptr) return;
    if (minHeight > 0) {
        control->setMinimumHeight(minHeight);
    }
    control->setCursor(Qt::PointingHandCursor);
}

/// A caption that must never set its page's width floor. The styled heading and
/// section-heading fonts are wide and a translated caption is wider still: left
/// to its own size hint a caption votes its whole advance into the page, and a
/// page wider than its viewport can only be read by panning it sideways. Under
/// an Ignored horizontal policy the caption yields and wraps instead.
inline void yieldCaptionWidth(QLabel* label) {
    if (label == nullptr) return;
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Ignored, label->sizePolicy().verticalPolicy());
}

/// A check box that wraps its own caption. QCheckBox paints its caption on one
/// line and never elides it, so a translated caption votes its whole advance
/// into the page and the page either pans sideways or clips the words. Here the
/// caption is measured and drawn with word wrapping beside the indicator, and
/// the wrapped height is what the layout receives; text() still carries the
/// whole caption, so the accessible name, the mnemonic and every translation
/// keep reading it.
class WrappingCheckBox final : public QCheckBox {
 public:
    explicit WrappingCheckBox(QWidget* parent = nullptr) : QCheckBox(parent) {
        // A layout asks qSmartMinSize first, and a policy without the shrink
        // flag is answered with max(sizeHint, minimumSizeHint) - the whole
        // one-line caption - before minimumSizeHint is ever consulted. Only a
        // shrinkable policy lets the wrapped minimum below stand.
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    }

    QSize minimumSizeHint() const override {
        const QSize base = QCheckBox::sizeHint();
        // The width below which no word fits on a line; the height is left to
        // heightForWidth at the width the layout actually hands out, or a row
        // that wraps twice would still reserve the height of its narrowest
        // possible wrap.
        return QSize(horizontalChrome(base) + longestWordAdvance(), base.height());
    }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override {
        const QSize base = QCheckBox::sizeHint();
        const int textWidth = qMax(0, width - horizontalChrome(base));
        const int lines = wrappedLines(textWidth).size();
        return qMax(base.height(),
                    base.height() - fontMetrics().height() + lines * fontMetrics().lineSpacing());
    }

 protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QStyleOptionButton option;
        initStyleOption(&option);
        // The style draws the indicator, the surface and the label in the
        // sheet's own ink and mnemonic handling; the caption it receives is the
        // same words on as many lines as the box has room for.
        option.text =
            wrappedLines(qMax(0, horizontalWidth(option.rect.width()))).join(QLatin1Char('\n'));
        QPainter painter(this);
        style()->drawControl(QStyle::CE_CheckBox, &option, &painter, this);
    }

 private:
    /// Everything a single-line caption adds beside its own advance: the
    /// indicator, the label spacing and the sheet's box model, as the style
    /// computes them for this control. The style measures the caption with its
    /// mnemonics shown, so the advance it counted has no `&` in it either.
    int horizontalChrome(const QSize& singleLine) const {
        QString spoken = text();
        spoken.remove(QLatin1Char('&'));
        return qMax(0, singleLine.width() - fontMetrics().horizontalAdvance(spoken));
    }

    int horizontalWidth(int widgetWidth) const {
        return widgetWidth - horizontalChrome(QCheckBox::sizeHint());
    }

    /// The caption's words greedily packed into the width the box has, measured
    /// without the mnemonic markers the style turns into underlines.
    QStringList wrappedLines(int textWidth) const {
        const QString caption = text();
        if (caption.isEmpty()) return {};
        if (textWidth <= 0) return {caption};
        const QStringList words = caption.split(QLatin1Char(' '));
        const auto advance = [this](const QString& line) {
            QString spoken = line;
            spoken.remove(QLatin1Char('&'));
            return fontMetrics().horizontalAdvance(spoken);
        };
        QStringList lines;
        QString line;
        for (const QString& word : words) {
            const QString candidate = line.isEmpty() ? word : line + QLatin1Char(' ') + word;
            if (!line.isEmpty() && advance(candidate) > textWidth) {
                lines.append(line);
                line = word;
            } else {
                line = candidate;
            }
        }
        if (!line.isEmpty()) lines.append(line);
        return lines;
    }

    /// The width below which no word can stand on its own line: a caption wraps
    /// at any width down to this one and never below it.
    int longestWordAdvance() const {
        int widest = 0;
        for (const QStringView part : QStringView(text()).split(QLatin1Char(' '))) {
            QString word = part.toString();
            word.remove(QLatin1Char('&'));
            widest = qMax(widest, fontMetrics().horizontalAdvance(word));
        }
        return widest;
    }
};

/// Styles a top-level page heading with balanced weight and tight tracking.
inline void stylePageHeading(QLabel* label) {
    if (label == nullptr) return;
    label->setObjectName(QStringLiteral("heading"));
    yieldCaptionWidth(label);
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, -0.3);
    font.setStyleStrategy(QFont::PreferAntialias);
    label->setFont(font);
}

/// Styles a card or section heading with medium-bold weight and snug tracking.
inline void styleSectionHeading(QLabel* label) {
    if (label == nullptr) return;
    label->setObjectName(QStringLiteral("sectionHeading"));
    yieldCaptionWidth(label);
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, -0.15);
    font.setStyleStrategy(QFont::PreferAntialias);
    label->setFont(font);
}

/// Styles a muted description or helper caption for comfortable reading.
inline void styleMutedCaption(QLabel* label) {
    if (label == nullptr) return;
    label->setObjectName(QStringLiteral("muted"));
    yieldCaptionWidth(label);
}

/// Styles a compact pill counter or status badge (e.g., "3 / 5 selected", "Ready").
inline void stylePillBadge(QLabel* label, const QString& tone = QStringLiteral("purple")) {
    if (label == nullptr) return;
    label->setProperty("badgeTone", tone);
    label->setAlignment(Qt::AlignCenter);
    applyTabularNumerals(label);
    theme::repolish(label);
}

/// Styles a semantic status/error/callout banner (`"neutral"`, `"info"`,
/// `"success"`, `"warning"`, `"danger"`).
inline void styleStatusBanner(QLabel* label, const QString& state) {
    if (label == nullptr) return;
    label->setProperty("statusTone", state);
    applyTabularNumerals(label);
    theme::repolish(label);
}

/// Creates a 1px hairline horizontal divider within a card or between sections.
inline QFrame* makeSettingsDivider(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setObjectName(QStringLiteral("settingsDivider"));
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setFixedHeight(1);
    return line;
}

/// Creates a styled surface card with concentric radius and standard internal
/// margins (`kSpacingMedium` = 16px) and vertical rhythm.
inline QFrame* makeSettingsCard(QWidget* parent, QVBoxLayout** layoutOut) {
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("settingsCard"));
    card->setFrameShape(QFrame::StyledPanel);
    auto* layout = new QVBoxLayout(card);
    const int pad = theme::kSpacingMedium;
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(theme::kSpacingSmall);
    if (layoutOut != nullptr) *layoutOut = layout;
    return card;
}

/// Creates an inset sub-panel inside a card (concentric inner radius `kRadiusMd`)
/// for live telemetry, quota readouts, or grouped controls.
inline QFrame* makeSettingsSubCard(QWidget* parent, QVBoxLayout** layoutOut) {
    auto* subCard = new QFrame(parent);
    subCard->setObjectName(QStringLiteral("settingsSubCard"));
    subCard->setFrameShape(QFrame::StyledPanel);
    auto* layout = new QVBoxLayout(subCard);
    const int padH = theme::kSpacingMedium;
    const int padV = theme::kSpacingSmall + theme::kSpacingXs;
    layout->setContentsMargins(padH, padV, padH, padV);
    layout->setSpacing(theme::kSpacingXs);
    if (layoutOut != nullptr) *layoutOut = layout;
    return subCard;
}

/// Creates a unified page header block (`heading` + `muted` subtitle) so every
/// settings tab shares a calm, editorial top hierarchy.
inline QFrame* makeSettingsPageHeader(QWidget* parent, QLabel** headingOut,
                                      QLabel** descriptionOut, const QString& titleText,
                                      const QString& descriptionText,
                                      QWidget* leadingBadge = nullptr) {
    auto* headerFrame = new QFrame(parent);
    headerFrame->setObjectName(QStringLiteral("settingsPageHeader"));
    auto* headerLayout = new QVBoxLayout(headerFrame);
    headerLayout->setContentsMargins(theme::kSpacingSmall, theme::kSpacingSmall,
                                     theme::kSpacingSmall, theme::kSpacingXs);
    headerLayout->setSpacing(theme::kSpacingXs);

    auto* titleRow = new QHBoxLayout();
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(theme::kSpacingSmall);
    if (leadingBadge != nullptr) {
        leadingBadge->setParent(headerFrame);
        titleRow->addWidget(leadingBadge, 0, Qt::AlignVCenter);
    }
    auto* heading = new QLabel(titleText, headerFrame);
    stylePageHeading(heading);
    titleRow->addWidget(heading, 1, Qt::AlignVCenter);
    headerLayout->addLayout(titleRow);

    auto* description = new QLabel(descriptionText, headerFrame);
    styleMutedCaption(description);
    headerLayout->addWidget(description);

    if (headingOut != nullptr) *headingOut = heading;
    if (descriptionOut != nullptr) *descriptionOut = description;
    return headerFrame;
}

/// Builds the Slight Purple QSS stylesheet for a settings page or scroll area.
/// Designed so nested surfaces follow concentric radii (`16px` card -> `12px`
/// sub-card -> `8px` control -> `6px` badge) and slight-purple accents highlight
/// interactive focus, checked states, and active telemetry without visual noise.
QString settingsStylesheet(const theme::Appearance& appearance);

}  // namespace mikai::app
