#include "Labels.hpp"

#include "state/AppearanceStore.hpp"

#include <QCoreApplication>
#include <QStringList>
#include <QTranslator>
#include <QVariant>

// These are free functions rather than a QObject subclass, so every string is
// extracted under the explicit "Labels" context instead of a class name, and
// each one passes its English source as a literal at the translate() call site.
// lupdate reads literals out of the call, so a table that only handed a lookup
// helper a key/value pair would ship an empty catalog. The comparisons stay
// case-sensitive on the raw token, which is how the API spells them.
namespace mikai::app::labels {
namespace {

/// The embedded Ukrainian catalogue, used only to *look up* a content name in
/// a language other than the interface's. It is never installed, so switching
/// the names language cannot change any interface text.
QTranslator* ukCatalog() {
    static QTranslator* translator = []() -> QTranslator* {
        auto* candidate = new QTranslator();
        if (candidate->load(QStringLiteral(":/i18n/mikai_uk.qm"))) return candidate;
        delete candidate;
        return nullptr;
    }();
    return translator;
}

/// The language a content name should be shown in. "Interface" keeps the
/// historical behaviour: the same translator the rest of the window uses.
QString content(const char* source, const QString& interfaceText) {
    switch (currentAppearance().nameLanguage) {
        case theme::NameLanguage::English:
        case theme::NameLanguage::Original:
            return QString::fromUtf8(source);
        case theme::NameLanguage::Ukrainian:
            if (QTranslator* catalog = ukCatalog()) {
                const QString text = catalog->translate("Labels", source);
                if (!text.isEmpty()) return text;
            }
            break;
        case theme::NameLanguage::Interface:
            break;
    }
    return interfaceText;
}

}  // namespace

QString titleName(const backend::Titles& titles) {
    const QString ua = titles.ua.trimmed();
    const QString english = titles.english.trimmed();
    const QString original = titles.original.trimmed();
    const auto firstNonEmpty = [](std::initializer_list<QString> values) {
        for (const QString& value : values) {
            if (!value.isEmpty()) return value;
        }
        return QStringLiteral("?");
    };
    switch (currentAppearance().titleLanguage) {
        case theme::TitleLanguage::English:
            return firstNonEmpty({english, ua, original});
        case theme::TitleLanguage::Original:
            return firstNonEmpty({original, english, ua});
        case theme::TitleLanguage::Ukrainian:
            break;
    }
    return firstNonEmpty({ua, english, original});
}

QString status(const QString& raw) {
    if (raw == QLatin1String("finished")) {
        return content("Finished", QCoreApplication::translate("Labels", "Finished"));
    }
    if (raw == QLatin1String("ongoing")) {
        return content("Airing", QCoreApplication::translate("Labels", "Airing"));
    }
    if (raw == QLatin1String("announce")) {
        return content("Announced", QCoreApplication::translate("Labels", "Announced"));
    }
    if (raw == QLatin1String("cancelled")) {
        return content("Cancelled", QCoreApplication::translate("Labels", "Cancelled"));
    }
    if (raw == QLatin1String("break")) {
        return content("On hiatus", QCoreApplication::translate("Labels", "On hiatus"));
    }
    if (raw == QLatin1String("unknown")) {
        return content("Unknown", QCoreApplication::translate("Labels", "Unknown"));
    }
    return raw;
}

QString kind(const QString& raw) {
    if (raw == QLatin1String("voice")) {
        return content("Dubbed", QCoreApplication::translate("Labels", "Dubbed"));
    }
    if (raw == QLatin1String("sub")) {
        return content("Subtitled", QCoreApplication::translate("Labels", "Subtitled"));
    }
    return raw;
}

QString provider(const QString& raw) {
    // Brand names, not prose: they belong to the provider and to the API
    // responses that name them, so a catalog entry could only ever copy them.
    if (raw == QLatin1String("ashdi")) {
        return QStringLiteral("Ashdi");
    }
    if (raw == QLatin1String("moon")) {
        return QStringLiteral("MoonAnime");
    }
    if (raw == QLatin1String("tortuga")) {
        return QStringLiteral("Tortuga");
    }
    return raw;
}

QString format(const QString& raw) {
    if (raw == QLatin1String("tv")) {
        return content("TV series", QCoreApplication::translate("Labels", "TV series"));
    }
    if (raw == QLatin1String("movie")) {
        return content("Movie", QCoreApplication::translate("Labels", "Movie"));
    }
    if (raw == QLatin1String("ova")) {
        return QStringLiteral("OVA");
    }
    if (raw == QLatin1String("ona")) {
        return content("Web series", QCoreApplication::translate("Labels", "Web series"));
    }
    if (raw == QLatin1String("special")) {
        return content("Special", QCoreApplication::translate("Labels", "Special"));
    }
    if (raw == QLatin1String("music")) {
        return content("Music video", QCoreApplication::translate("Labels", "Music video"));
    }
    if (raw == QLatin1String("other")) {
        return content("Other", QCoreApplication::translate("Labels", "Other"));
    }
    return raw;
}

QString relation(const QString& raw) {
    if (raw == QLatin1String("prequel")) {
        return content("Prequel", QCoreApplication::translate("Labels", "Prequel"));
    }
    if (raw == QLatin1String("sequel")) {
        return content("Sequel", QCoreApplication::translate("Labels", "Sequel"));
    }
    if (raw == QLatin1String("sideStory")) {
        return content("Side story", QCoreApplication::translate("Labels", "Side story"));
    }
    if (raw == QLatin1String("parentStory")) {
        return content("Main story", QCoreApplication::translate("Labels", "Main story"));
    }
    if (raw == QLatin1String("spinOff")) {
        return content("Spin-off", QCoreApplication::translate("Labels", "Spin-off"));
    }
    if (raw == QLatin1String("alternativeVersion")) {
        return content("Alternative version", QCoreApplication::translate("Labels", "Alternative version"));
    }
    if (raw == QLatin1String("alternativeSetting")) {
        return content("Alternative setting", QCoreApplication::translate("Labels", "Alternative setting"));
    }
    if (raw == QLatin1String("summary")) {
        return content("Summary", QCoreApplication::translate("Labels", "Summary"));
    }
    if (raw == QLatin1String("fullStory")) {
        return content("Full story", QCoreApplication::translate("Labels", "Full story"));
    }
    if (raw == QLatin1String("adaptation")) {
        return content("Adaptation", QCoreApplication::translate("Labels", "Adaptation"));
    }
    if (raw == QLatin1String("other")) {
        return content("Other", QCoreApplication::translate("Labels", "Other"));
    }
    if (raw == QLatin1String("similar")) {
        return content("Similar", QCoreApplication::translate("Labels", "Similar"));
    }
    // The API mixes camelCase and snake_case for the same concept, so retry the
    // keys against a case-folded token before giving up on the raw spelling.
    // Only the all-lowercase keys can match here, exactly as before: the
    // camelCase keys keep their own exact branch above.
    const QString folded = raw.toLower();
    if (folded == QLatin1String("prequel")) {
        return content("Prequel", QCoreApplication::translate("Labels", "Prequel"));
    }
    if (folded == QLatin1String("sequel")) {
        return content("Sequel", QCoreApplication::translate("Labels", "Sequel"));
    }
    if (folded == QLatin1String("summary")) {
        return content("Summary", QCoreApplication::translate("Labels", "Summary"));
    }
    if (folded == QLatin1String("adaptation")) {
        return content("Adaptation", QCoreApplication::translate("Labels", "Adaptation"));
    }
    if (folded == QLatin1String("other")) {
        return content("Other", QCoreApplication::translate("Labels", "Other"));
    }
    if (folded == QLatin1String("similar")) {
        return content("Similar", QCoreApplication::translate("Labels", "Similar"));
    }
    // Unknown token: prettify it the way the raw API spelling reads.
    QString spaced = raw;
    spaced.replace(QLatin1Char('_'), QLatin1Char(' '));
    if (spaced.isEmpty()) {
        return spaced;
    }
    spaced[0] = spaced.at(0).toUpper();
    return spaced;
}

QString source(const QString& raw) {
    if (raw == QLatin1String("manga")) {
        return content("Manga", QCoreApplication::translate("Labels", "Manga"));
    }
    if (raw == QLatin1String("novel")) {
        return content("Novel", QCoreApplication::translate("Labels", "Novel"));
    }
    if (raw == QLatin1String("light_novel")) {
        return content("Light novel", QCoreApplication::translate("Labels", "Light novel"));
    }
    if (raw == QLatin1String("web_comic")) {
        return content("Web comic", QCoreApplication::translate("Labels", "Web comic"));
    }
    if (raw == QLatin1String("visual_novel")) {
        return content("Visual novel", QCoreApplication::translate("Labels", "Visual novel"));
    }
    if (raw == QLatin1String("other")) {
        return content("Other", QCoreApplication::translate("Labels", "Other"));
    }
    return raw;
}

QString country(const QString& raw) {
    if (raw == QLatin1String("JP")) {
        return content("Japan", QCoreApplication::translate("Labels", "Japan"));
    }
    if (raw == QLatin1String("KR")) {
        return content("South Korea", QCoreApplication::translate("Labels", "South Korea"));
    }
    if (raw == QLatin1String("CN")) {
        return content("China", QCoreApplication::translate("Labels", "China"));
    }
    return raw;
}

QString ageRating(const QString& raw) {
    // Four spellings of the same rating share one translated phrase.
    if (raw == QLatin1String("all") || raw == QLatin1String("any") ||
        raw == QLatin1String("g") || raw == QLatin1String("pg")) {
        return content("All ages", QCoreApplication::translate("Labels", "All ages"));
    }
    // The remaining ratings are numbers: only the two-letter rating prefix is
    // dropped, and the digits read the same in every language.
    if (raw == QLatin1String("pg12")) {
        return QStringLiteral("12+");
    }
    if (raw == QLatin1String("pg13")) {
        return QStringLiteral("13+");
    }
    if (raw == QLatin1String("pg16")) {
        return QStringLiteral("16+");
    }
    if (raw == QLatin1String("r17")) {
        return QStringLiteral("17+");
    }
    if (raw == QLatin1String("r18")) {
        return QStringLiteral("18+");
    }
    return raw;
}

QString season(const QString& startDate) {
    const QStringList parts = startDate.split(QLatin1Char('-'));
    if (parts.size() < 2) {
        return {};
    }
    bool yearOk = false;
    bool monthOk = false;
    const int year = parts.at(0).toInt(&yearOk);
    const int month = parts.at(1).toInt(&monthOk);
    if (!yearOk || !monthOk) {
        return {};
    }
    // January names no season, so those dates render as the bare year alone.
    if (month < 2 || month > 12) {
        return QString::number(year);
    }
    QString name;
    if (month == 2) {
        name = content("Winter", QCoreApplication::translate("Labels", "Winter"));
    } else if (month <= 5) {
        name = content("Spring", QCoreApplication::translate("Labels", "Spring"));
    } else if (month <= 9) {
        name = content("Summer", QCoreApplication::translate("Labels", "Summer"));
    } else {
        name = content("Autumn", QCoreApplication::translate("Labels", "Autumn"));
    }
    return QStringLiteral("%1 %2").arg(name).arg(year);
}

QString compactNumber(qint64 value) {
    if (value >= 1000000) {
        QString text = QString::number(static_cast<double>(value) / 1000000.0, 'f', 1);
        if (text.endsWith(QLatin1String(".0"))) {
            text.chop(2);
        }
        return text + QStringLiteral("M");
    }
    if (value >= 1000) {
        QString text = QString::number(static_cast<double>(value) / 1000.0, 'f', 1);
        if (text.endsWith(QLatin1String(".0"))) {
            text.chop(2);
        }
        return text + QStringLiteral("K");
    }
    return QString::number(value);
}

QString variant(const QVariant& value) {
    if (!value.isValid() || value.isNull()) {
        return {};
    }
    if (value.typeId() == QMetaType::QString) {
        return value.toString();
    }
    if (value.typeId() == QMetaType::Int || value.typeId() == QMetaType::LongLong) {
        return QString::number(value.toLongLong());
    }
    const double number = value.toDouble();
    if (qFuzzyCompare(number, qRound(number))) {
        return QString::number(qRound(number));
    }
    return QString::number(number);
}

}  // namespace mikai::app::labels
