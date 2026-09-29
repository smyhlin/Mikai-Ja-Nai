#include "dialogs/SettingsDialog.hpp"

#include "state/Feeds.hpp"
#include "theme/Colors.hpp"
#include "theme/Icons.hpp"
#include "theme/Metrics.hpp"
#include "workers/Runner.hpp"

#include <backend/MikaiClient.hpp>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

namespace mikai::app {

SettingsDialog::SettingsDialog(backend::MikaiClient& client, Runner& runner, QWidget* parent)
    : QDialog(parent), client_(client), runner_(runner) {
    setWindowTitle(tr("Налаштування"));
    setMinimumWidth(480);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(theme::kSpacingLarge, theme::kSpacingLarge, theme::kSpacingLarge,
                             theme::kSpacingLarge);
    root->setSpacing(theme::kSpacingMedium);

    auto* headerRow = new QHBoxLayout();
    auto* iconLabel = new QLabel(this);
    iconLabel->setPixmap(theme::gearIcon(22, QLatin1String(theme::kAccent))
                             .pixmap(22, 22));
    auto* heading = new QLabel(tr("Токен доступу Mikai API"), this);
    heading->setObjectName(QStringLiteral("heading"));
    headerRow->addWidget(iconLabel);
    headerRow->addWidget(heading);
    headerRow->addStretch();
    root->addLayout(headerRow);

    auto* description = new QLabel(
        tr("Ключ доступу підіймає ліміт з 60 до 300 запитів на хвилину і зберігається "
           "локально в .env файлі."),
        this);
    description->setObjectName(QStringLiteral("muted"));
    description->setWordWrap(true);
    root->addWidget(description);

    auto* link = new QLabel(this);
    link->setTextFormat(Qt::RichText);
    link->setText(tr("<a style=\"color: %1; text-decoration: none;\" "
                     "href=\"https://mikai.me/user/developers\">"
                     "Отримати ключ у кабінеті розробника на mikai.me ↗</a>")
                       .arg(QLatin1String(theme::kAccent)));
    link->setOpenExternalLinks(true);
    link->setWordWrap(true);
    root->addWidget(link);

    token_ = new QLineEdit(this);
    token_->setPlaceholderText(tr("mk_... (залиште порожнім для анонімного доступу)"));
    token_->setClearButtonEnabled(true);
    const QString current = client_.apiKey();
    if (!current.isEmpty()) {
        token_->setText(current);
    }
    root->addWidget(token_);

    auto* quotaTitle = new QLabel(tr("Використання API"), this);
    quotaTitle->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(quotaTitle);
    quota_ = new QLabel(this);
    quota_->setObjectName(QStringLiteral("descriptionPanel"));
    quota_->setWordWrap(true);
    root->addWidget(quota_);

    auto* feedsTitle = new QLabel(tr("Блоки головної сторінки"), this);
    feedsTitle->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(feedsTitle);
    auto* feedsHint = new QLabel(
        tr("Вимкнений блок не завантажується і не витрачає запити API. Пошук і сторінка "
           "тайтлу працюють завжди."),
        this);
    feedsHint->setObjectName(QStringLiteral("muted"));
    feedsHint->setWordWrap(true);
    root->addWidget(feedsHint);

    QSettings settings;
    const QSet<QString> enabled = feeds::enabled(settings.value(feeds::settingsKey()).toString());
    for (const QString& key : feeds::all()) {
        auto* box = new QCheckBox(feeds::label(key), this);
        box->setChecked(enabled.contains(key));
        root->addWidget(box);
        feedBoxes_.insert(key, box);
    }

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("status"));
    status_->setWordWrap(true);
    root->addWidget(status_);

    testButton_ = new QPushButton(tr("Перевірити"), this);
    connect(testButton_, &QPushButton::clicked, this, &SettingsDialog::testToken);

    auto* buttons = new QDialogButtonBox(this);
    auto* cancel = buttons->addButton(tr("Скасувати"), QDialogButtonBox::RejectRole);
    auto* save = buttons->addButton(tr("Зберегти"), QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("primary"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, &SettingsDialog::save);
    buttons->addButton(testButton_, QDialogButtonBox::ActionRole);
    root->addWidget(buttons);

    quotaTimer_ = new QTimer(this);
    quotaTimer_->setInterval(1000);
    connect(quotaTimer_, &QTimer::timeout, this, &SettingsDialog::refreshQuota);
    quotaTimer_->start();
    refreshQuota();
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::testToken() {
    const QString candidate = token_->text().trimmed();
    status_->setText(tr("Перевірка токена…"));
    testButton_->setEnabled(false);
    runner_.run(
        [this, candidate] {
            const QMap<QString, QVariant> result = client_.validateApiKey(candidate);
            return QVariant::fromValue(result);
        },
        [this](const QVariant& raw) {
            testButton_->setEnabled(true);
            const QMap<QString, QVariant> result = raw.toMap();
            if (result.value(QStringLiteral("valid")).toBool()) {
                const bool authenticated =
                    result.value(QStringLiteral("authenticated")).toBool();
                const int limit = result.value(QStringLiteral("rate_limit"), 60).toInt();
                const QString name = result.value(QStringLiteral("name")).toString();
                if (authenticated && !name.isEmpty()) {
                    status_->setText(tr("✓ Ключ дійсний (користувач: %1, ліміт: %2/хв)")
                                         .arg(name)
                                         .arg(limit));
                } else if (authenticated) {
                    status_->setText(tr("✓ Ключ дійсний (ліміт: %1/хв)").arg(limit));
                } else {
                    status_->setText(tr("Анонімний доступ (ліміт: %1/хв)").arg(limit));
                }
            } else {
                const QString error = result.value(QStringLiteral("error")).toString();
                status_->setText(tr("❌ Помилка: %1")
                                     .arg(error.isEmpty() ? tr("Недійсний токен") : error));
            }
        },
        [this](const QString& message) {
            testButton_->setEnabled(true);
            status_->setText(tr("❌ Помилка з'єднання: %1").arg(message));
        });
}

void SettingsDialog::refreshQuota() {
    const backend::RateLimitState state = client_.rateLimitState();
    const QString limit = state.limit > 0 ? QString::number(state.limit) : QStringLiteral("—");
    QString text;
    if (!state.used.has_value() || !state.remaining.has_value()) {
        text = tr("Ліміт: %1 запитів/хв · лічильник очікує наступну відповідь API").arg(limit);
    } else {
        text = tr("За вікно: використано %1, доступно %2 із %3 запитів")
                   .arg(*state.used)
                   .arg(*state.remaining)
                   .arg(limit);
        if (state.resetIn > 0) {
            text += tr(" · оновлення через %1 с").arg(state.resetIn);
        }
    }
    text += tr("\nАктивні: %1 · у черзі: %2").arg(state.inFlight).arg(state.queued);
    if (state.queued > 0) {
        text += tr(" · запити продовжаться автоматично після оновлення ліміту");
    }
    quota_->setText(text);
}

void SettingsDialog::save() {
    const QString candidate = token_->text().trimmed();
    client_.setApiKey(candidate);
    QSet<QString> chosen;
    for (auto it = feedBoxes_.constBegin(); it != feedBoxes_.constEnd(); ++it) {
        if (it.value()->isChecked()) {
            chosen.insert(it.key());
        }
    }
    QSettings settings;
    settings.setValue(feeds::settingsKey(), feeds::serialize(chosen));
    accept();
}

}  // namespace mikai::app
