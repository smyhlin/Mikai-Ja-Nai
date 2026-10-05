#pragma once

#include <cstdio>

#include <QAbstractButton>
#include <QApplication>
#include <QDialogButtonBox>
#include <QLocale>
#include <QScrollArea>
#include <QSize>

#include "I18n.hpp"
#include "theme/Theme.hpp"

namespace mikai::probes {

inline bool initializeUi(QApplication& app) {
    const QString language = qEnvironmentVariable("MIKAI_PROBE_LANGUAGE", "en");
    if (language != QLatin1String("en") && language != QLatin1String("uk")) {
        std::fprintf(stderr, "FAIL unsupported probe language\n");
        return false;
    }

    // A probe links mikai_app, which does not embed the application catalog, so
    // a Ukrainian run passes the real built file in. An unset variable then
    // leaves the embedded default, which the probe cannot load and must fail on.
    const QString catalog = language == QLatin1String("uk")
                                ? qEnvironmentVariable("MIKAI_PROBE_TRANSLATION")
                                : QString();
    if (!mikai::app::applyInterfaceLanguage(language, catalog)) {
        std::fprintf(stderr,
                     "FAIL interface language %s unavailable (application catalog %s)\n",
                     qPrintable(language), catalog.isEmpty() ? "<unset>" : qPrintable(catalog));
        return false;
    }

    // Print the language and catalog that are actually installed, so an English
    // screenshot cannot be passed off as Ukrainian geometry evidence.
    std::fprintf(stderr, "mikai probe ui: requested=%s installed=%s locale=%s catalog=%s\n",
                 qPrintable(language), qPrintable(mikai::app::activeInterfaceLanguage()),
                 qPrintable(QLocale().name()),
                 catalog.isEmpty() ? "<none>" : qPrintable(catalog));

    app.setStyle(QStringLiteral("Fusion"));
    app.setStyleSheet(mikai::app::theme::buildStylesheet());
    return true;
}

inline bool dialogActionsFit(QWidget& dialog, QSize requestedSize) {
    std::fprintf(stderr, "Dialog geometry: %dx%d requested %dx%d DPR %.2f\n",
                 dialog.width(), dialog.height(), requestedSize.width(),
                 requestedSize.height(), dialog.devicePixelRatioF());
    if (dialog.size() != requestedSize) return false;
    auto* box = dialog.findChild<QDialogButtonBox*>();
    if (!box) return false;
    bool saveVisible = false;
    bool cancelVisible = false;
    for (QAbstractButton* button : box->buttons()) {
        const auto role = box->buttonRole(button);
        if (role != QDialogButtonBox::AcceptRole && role != QDialogButtonBox::RejectRole) {
            continue;
        }
        const QRect geometry(button->mapTo(&dialog, QPoint()), button->size());
        if (!button->isVisibleTo(&dialog) || !dialog.rect().contains(geometry)) return false;
        saveVisible = saveVisible || role == QDialogButtonBox::AcceptRole;
        cancelVisible = cancelVisible || role == QDialogButtonBox::RejectRole;
    }
    return saveVisible && cancelVisible;
}

inline void revealWidget(QWidget* widget) {
    if (!widget) return;
    for (QWidget* parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {
        if (auto* scroll = qobject_cast<QScrollArea*>(parent)) {
            scroll->ensureWidgetVisible(widget);
            QApplication::processEvents();
            return;
        }
    }
}

inline QSize windowSize(QSize fallback) {
    bool widthValid = false;
    bool heightValid = false;
    const int width = qEnvironmentVariableIntValue("MIKAI_PROBE_WIDTH", &widthValid);
    const int height = qEnvironmentVariableIntValue("MIKAI_PROBE_HEIGHT", &heightValid);
    return QSize(widthValid && width > 0 ? width : fallback.width(),
                 heightValid && height > 0 ? height : fallback.height());
}

}  // namespace mikai::probes
