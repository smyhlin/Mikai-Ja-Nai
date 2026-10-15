#pragma once

#include "theme/Metrics.hpp"

#include <QFrame>
#include <QLabel>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace mikai::app {

/// A caption that must never set its page's width floor. The styled heading and
/// section-heading fonts are wide and a translated caption is wider still: left
/// to its own size hint a caption votes its whole advance into the page, and a
/// page wider than its viewport can only be read by panning it sideways. Under
/// an Ignored horizontal policy the caption yields and wraps instead.
inline void yieldCaptionWidth(QLabel* label) {
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Ignored, label->sizePolicy().verticalPolicy());
}

/// Creates a styled surface card with standard internal margins and spacing.
inline QFrame* makeSettingsCard(QWidget* parent, QVBoxLayout** layoutOut) {
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("settingsCard"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium,
                               theme::kSpacingMedium, theme::kSpacingMedium);
    layout->setSpacing(theme::kSpacingSmall);
    if (layoutOut != nullptr) *layoutOut = layout;
    return card;
}

}  // namespace mikai::app
