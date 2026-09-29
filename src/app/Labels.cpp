#include "Labels.hpp"

#include <QHash>
#include <QStringList>
#include <QVariant>

namespace mikai::app::labels {
namespace {

QString pick(const QString& raw, std::initializer_list<QPair<const char*, const char*>> table) {
    for (const auto& entry : table) {
        if (raw == QLatin1String(entry.first)) {
            return QString::fromUtf8(entry.second);
        }
    }
    return raw;
}

}  // namespace

QString status(const QString& raw) {
    return pick(raw, {{"finished", "Завершено"},
                      {"ongoing", "Онгоінґ"},
                      {"announce", "Анонсовано"},
                      {"cancelled", "Скасовано"},
                      {"break", "Перерва"},
                      {"unknown", "Невідомо"}});
}

QString kind(const QString& raw) {
    return pick(raw, {{"voice", "Озвучення"}, {"sub", "Субтитри"}});
}

QString provider(const QString& raw) {
    return pick(raw, {{"ashdi", "Ashdi"}, {"moon", "MoonAnime"}, {"tortuga", "Tortuga"}});
}

QString format(const QString& raw) {
    return pick(raw, {{"tv", "ТБ серіал"},
                      {"movie", "Фільм"},
                      {"ova", "OVA"},
                      {"ona", "Web-серіал"},
                      {"special", "Спецвипуск"},
                      {"music", "Музичне відео"},
                      {"other", "Інше"}});
}

QString relation(const QString& raw) {
    static const QHash<QString, QString> kLabels = {
        {QStringLiteral("prequel"), QStringLiteral("Передісторія")},
        {QStringLiteral("sequel"), QStringLiteral("Продовження")},
        {QStringLiteral("sideStory"), QStringLiteral("Побічна історія")},
        {QStringLiteral("parentStory"), QStringLiteral("Основна історія")},
        {QStringLiteral("spinOff"), QStringLiteral("Спіноф")},
        {QStringLiteral("alternativeVersion"), QStringLiteral("Інша версія")},
        {QStringLiteral("alternativeSetting"), QStringLiteral("Інший сетинг")},
        {QStringLiteral("summary"), QStringLiteral("Переказ")},
        {QStringLiteral("fullStory"), QStringLiteral("Повна історія")},
        {QStringLiteral("adaptation"), QStringLiteral("Адаптація")},
        {QStringLiteral("other"), QStringLiteral("Інше")},
        {QStringLiteral("similar"), QStringLiteral("Схоже")},
    };
    const auto it = kLabels.constFind(raw);
    if (it != kLabels.constEnd()) {
        return it.value();
    }
    // The API mixes camelCase and snake_case for the same concept; fall back to
    // a case-folded match before giving up and prettifying the raw token.
    const auto folded = kLabels.constFind(raw.toLower());
    if (folded != kLabels.constEnd()) {
        return folded.value();
    }
    QString spaced = raw;
    spaced.replace(QLatin1Char('_'), QLatin1Char(' '));
    if (spaced.isEmpty()) {
        return spaced;
    }
    spaced[0] = spaced.at(0).toUpper();
    return spaced;
}

QString source(const QString& raw) {
    return pick(raw, {{"manga", "Манґа"},
                      {"novel", "Роман"},
                      {"light_novel", "Легка новела"},
                      {"web_comic", "Вебкомікс"},
                      {"visual_novel", "Візуальна новела"},
                      {"other", "Інше"}});
}

QString country(const QString& raw) {
    return pick(raw, {{"JP", "Японія"},
                      {"KR", "Південна Корея"},
                      {"CN", "Китай"}});
}

QString ageRating(const QString& raw) {
    return pick(raw, {{"all", "Для всіх"},
                      {"any", "Для всіх"},
                      {"g", "Для всіх"},
                      {"pg", "Для всіх"},
                      {"pg12", "12+"},
                      {"pg13", "13+"},
                      {"pg16", "16+"},
                      {"r17", "17+"},
                      {"r18", "18+"}});
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
    static const char* kSeasons[] = {nullptr, nullptr, "Зима", "Весна", "Весна", "Весна",
                                     "Літо", "Літо",  "Літо", "Літо", "Осінь", "Осінь", "Осінь"};
    if (month < 1 || month > 12 || kSeasons[month] == nullptr) {
        return QString::number(year);
    }
    return QStringLiteral("%1 %2").arg(QString::fromUtf8(kSeasons[month])).arg(year);
}

QString compactNumber(qint64 value) {
    if (value >= 1000000) {
        QString text = QString::number(static_cast<double>(value) / 1000000.0, 'f', 1);
        if (text.endsWith(QLatin1String(".0M"))) {
            text.chop(2);
        }
        return text + QStringLiteral("M");
    }
    if (value >= 1000) {
        QString text = QString::number(static_cast<double>(value) / 1000.0, 'f', 1);
        if (text.endsWith(QLatin1String(".0K"))) {
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
