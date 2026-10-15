#include "dialogs/SettingsDialog.hpp"

#include "I18n.hpp"
#include "dialogs/settings/SettingsHelpers.hpp"
#include "dialogs/settings/SettingsUi.hpp"
#include "state/Feeds.hpp"
#include "theme/Colors.hpp"
#include "theme/Icons.hpp"
#include "theme/Metrics.hpp"
#include "workers/Runner.hpp"

#include <backend/MikaiClient.hpp>
#include <dialogs/ComponentStoreTab.hpp>
#include <player/Anime4K.hpp>

#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

namespace mikai::app {

QWidget* SettingsDialog::buildGeneralTab() {
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("nativeGeneralScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* page = new QWidget(scroll);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(theme::kSpacingSmall, theme::kSpacingMedium, theme::kSpacingSmall,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    // ── Card 1: Mikai API Access Token & Live Rate-Limit Readout ──────────────
    QVBoxLayout* apiLayout = nullptr;
    QFrame* apiCard = makeSettingsCard(page, &apiLayout);

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(theme::kSpacingSmall);
    auto* iconLabel = new QLabel(apiCard);
    iconLabel->setObjectName(QStringLiteral("muted"));
    iconLabel->setPixmap(theme::gearIcon(20, QLatin1String(theme::kAccent)).pixmap(20, 20));
    tokenHeading_ = new QLabel(tr("Mikai API access token"), apiCard);
    tokenHeading_->setObjectName(QStringLiteral("heading"));
    yieldCaptionWidth(tokenHeading_);
    headerRow->addWidget(iconLabel, 0, Qt::AlignVCenter);
    headerRow->addWidget(tokenHeading_, 1, Qt::AlignVCenter);
    apiLayout->addLayout(headerRow);

    tokenDescription_ = new QLabel(
        tr("An API key raises the limit from 60 to 300 requests per minute and is stored "
           "locally in the .env file."),
        apiCard);
    tokenDescription_->setObjectName(QStringLiteral("muted"));
    tokenDescription_->setWordWrap(true);
    apiLayout->addWidget(tokenDescription_);

    tokenLink_ = new QLabel(apiCard);
    tokenLink_->setObjectName(QStringLiteral("muted"));
    tokenLink_->setTextFormat(Qt::RichText);
    tokenLink_->setText(tr("<a style=\"color: %1; text-decoration: none;\" "
                           "href=\"https://mikai.me/user/developers\">"
                           "Get a key from the developer dashboard on mikai.me ↗</a>")
                             .arg(QLatin1String(theme::kAccent)));
    tokenLink_->setOpenExternalLinks(true);
    tokenLink_->setWordWrap(true);
    apiLayout->addWidget(tokenLink_);

    auto* tokenInputRow = new QHBoxLayout();
    tokenInputRow->setSpacing(theme::kSpacingSmall);
    token_ = new QLineEdit(apiCard);
    token_->setPlaceholderText(tr("mk_... (leave blank for anonymous access)"));
    token_->setAccessibleName(tr("Mikai API access token"));
    token_->setClearButtonEnabled(true);
    token_->setMinimumHeight(theme::kSettingsControlSize);
    const QString current = client_.apiKey();
    if (!current.isEmpty()) {
        token_->setText(current);
    }
    tokenInputRow->addWidget(token_, 1);

    testButton_ = new QPushButton(tr("Test"), apiCard);
    testButton_->setAccessibleName(tr("Test API token"));
    testButton_->setMinimumHeight(theme::kSettingsControlSize);
    connect(testButton_, &QPushButton::clicked, this, &SettingsDialog::testToken);
    tokenInputRow->addWidget(testButton_, 0);
    apiLayout->addLayout(tokenInputRow);

    status_ = new QLabel(apiCard);
    status_->setObjectName(QStringLiteral("status"));
    status_->setWordWrap(true);
    apiLayout->addWidget(status_);

    quotaTitle_ = new QLabel(tr("API usage"), apiCard);
    quotaTitle_->setObjectName(QStringLiteral("sectionHeading"));
    yieldCaptionWidth(quotaTitle_);
    apiLayout->addWidget(quotaTitle_);
    quota_ = new QLabel(apiCard);
    quota_->setObjectName(QStringLiteral("descriptionPanel"));
    quota_->setWordWrap(true);
    apiLayout->addWidget(quota_);

    root->addWidget(apiCard);

    // ── Card 2: Home Page Feed Sections (2-Column Grid) ───────────────────────
    QVBoxLayout* feedsLayout = nullptr;
    QFrame* feedsCard = makeSettingsCard(page, &feedsLayout);

    feedsTitle_ = new QLabel(tr("Home page sections"), feedsCard);
    feedsTitle_->setObjectName(QStringLiteral("sectionHeading"));
    yieldCaptionWidth(feedsTitle_);
    feedsLayout->addWidget(feedsTitle_);
    feedsHint_ = new QLabel(
        tr("Disabled sections are not loaded and do not use API requests. Search and title "
           "pages are always available."),
        feedsCard);
    feedsHint_->setObjectName(QStringLiteral("muted"));
    feedsHint_->setWordWrap(true);
    feedsLayout->addWidget(feedsHint_);

    QSettings settings;
    const QSet<QString> enabled = feeds::enabled(settings.value(feeds::settingsKey()).toString());
    initialFeeds_ = enabled;
    auto* feedsGrid = new QGridLayout();
    feedsGrid->setHorizontalSpacing(theme::kSpacingMedium);
    feedsGrid->setVerticalSpacing(theme::kSpacingXs);
    const QStringList allFeeds = feeds::all();
    for (int i = 0; i < allFeeds.size(); ++i) {
        const QString& key = allFeeds.at(i);
        auto* box = new QCheckBox(feeds::label(key), feedsCard);
        box->setAccessibleName(feeds::label(key));
        box->setMinimumHeight(theme::kSettingsControlSize);
        box->setChecked(enabled.contains(key));
        feedsGrid->addWidget(box, i / 2, i % 2);
        feedBoxes_.insert(key, box);
    }
    feedsGrid->setColumnStretch(0, 1);
    feedsGrid->setColumnStretch(1, 1);
    feedsLayout->addLayout(feedsGrid);

    root->addWidget(feedsCard);

    // ── Card 3: Interface Language ────────────────────────────────────────────
    QVBoxLayout* languageLayout = nullptr;
    QFrame* languageCard = makeSettingsCard(page, &languageLayout);

    languageTitle_ = new QLabel(tr("Interface language"), languageCard);
    languageTitle_->setObjectName(QStringLiteral("sectionHeading"));
    yieldCaptionWidth(languageTitle_);
    languageLayout->addWidget(languageTitle_);

    language_ = new QComboBox(languageCard);
    language_->setObjectName(QStringLiteral("uiLanguageCombo"));
    language_->setMinimumHeight(theme::kSettingsControlSize);
    language_->addItem(tr("Use system language"), QStringLiteral("system"));
    language_->addItem(tr("English"), QStringLiteral("en"));
    language_->addItem(tr("Ukrainian"), QStringLiteral("uk"));
    language_->setAccessibleName(tr("Interface language"));
    const int languageIndex = language_->findData(languagePreference());
    if (languageIndex >= 0) {
        language_->setCurrentIndex(languageIndex);
    }
    languageLayout->addWidget(language_);

    languageHint_ = new QLabel(
        tr("Applied immediately to the running windows, including the open player."),
        languageCard);
    languageHint_->setObjectName(QStringLiteral("muted"));
    languageHint_->setWordWrap(true);
    languageLayout->addWidget(languageHint_);

    root->addWidget(languageCard);

    root->addStretch();
    scroll->setWidget(page);
    generalScroll_ = scroll;
    return scroll;
}

bool SettingsDialog::feedsChanged() const {
    QSet<QString> chosen;
    for (auto it = feedBoxes_.constBegin(); it != feedBoxes_.constEnd(); ++it) {
        if (it.value()->isChecked()) {
            chosen.insert(it.key());
        }
    }
    return chosen != initialFeeds_;
}

void SettingsDialog::testToken() {
    const QString candidate = token_->text().trimmed();
    status_->setText(tr("Checking token…"));
    testButton_->setEnabled(false);
    QPointer<SettingsDialog> self(this);
    backend::MikaiClient* client = &client_;
    runner_.run(
        [client, candidate] {
            const QMap<QString, QVariant> result = client->validateApiKey(candidate);
            return QVariant::fromValue(result);
        },
        [self](const QVariant& raw) {
            if (!self) return;
            self->testButton_->setEnabled(true);
            const QMap<QString, QVariant> result = raw.toMap();
            if (result.value(QStringLiteral("valid")).toBool()) {
                const bool authenticated = result.value(QStringLiteral("authenticated")).toBool();
                const int limit = result.value(QStringLiteral("rate_limit"), 60).toInt();
                const QString name = result.value(QStringLiteral("name")).toString();
                if (authenticated && !name.isEmpty()) {
                    self->status_->setText(
                        tr("Key is valid (user: %1, limit: %2/min)").arg(name).arg(limit));
                } else if (authenticated) {
                    self->status_->setText(tr("Key is valid (limit: %1/min)").arg(limit));
                } else {
                    self->status_->setText(
                        tr("Anonymous access (limit: %1/min)").arg(limit));
                }
            } else {
                const QString code = result.value(QStringLiteral("error_code")).toString();
                const QString error = result.value(QStringLiteral("error")).toString();
                const QString translatedError = localizedApiError(code);
                const QString displayError =
                    translatedError.isEmpty()
                        ? (error.isEmpty() ? tr("Invalid token") : error)
                        : translatedError;
                self->status_->setText(tr("Error: %1").arg(displayError));
            }
        },
        [self](const backend::Failure& failure) {
            if (!self) return;
            self->testButton_->setEnabled(true);
            self->status_->setText(tr("Connection error: %1").arg(backend::failureLine(failure)));
        });
}

void SettingsDialog::refreshQuota() {
    const backend::RateLimitState state = client_.rateLimitState();
    const QString limit = state.limit > 0 ? QString::number(state.limit) : QStringLiteral("—");
    QString text;
    if (!state.used.has_value() || !state.remaining.has_value()) {
        text = tr("Limit: %1 requests/min · waiting for the next API response to update the counter")
                   .arg(limit);
    } else {
        text = tr("Window: %1 used, %2 of %3 requests available")
                   .arg(*state.used)
                   .arg(*state.remaining)
                   .arg(limit);
        if (state.resetIn > 0) {
            text += tr(" · resets in %1 s").arg(state.resetIn);
        }
    }
    text += tr("\nActive: %1 · queued: %2").arg(state.inFlight).arg(state.queued);
    if (state.queued > 0) {
        text += tr(" · requests will continue automatically when the limit resets");
    }
    quota_->setText(text);
}

void SettingsDialog::retranslateGeneralTab() {
    tokenHeading_->setText(tr("Mikai API access token"));
    tokenDescription_->setText(
        tr("An API key raises the limit from 60 to 300 requests per minute and is stored "
           "locally in the .env file."));
    tokenLink_->setText(tr("<a style=\"color: %1; text-decoration: none;\" "
                           "href=\"https://mikai.me/user/developers\">"
                           "Get a key from the developer dashboard on mikai.me ↗</a>")
                            .arg(QLatin1String(theme::kAccent)));
    token_->setPlaceholderText(tr("mk_... (leave blank for anonymous access)"));
    token_->setAccessibleName(tr("Mikai API access token"));
    testButton_->setText(tr("Test"));
    testButton_->setAccessibleName(tr("Test API token"));

    quotaTitle_->setText(tr("API usage"));
    feedsTitle_->setText(tr("Home page sections"));
    feedsHint_->setText(
        tr("Disabled sections are not loaded and do not use API requests. Search and title "
           "pages are always available."));
    for (auto it = feedBoxes_.constBegin(); it != feedBoxes_.constEnd(); ++it) {
        it.value()->setText(feeds::label(it.key()));
        it.value()->setAccessibleName(feeds::label(it.key()));
    }

    languageTitle_->setText(tr("Interface language"));
    languageHint_->setText(tr("Applied immediately to the running windows, including the open player."));
    const auto restateLanguage = [this](const QString& data, const QString& caption) {
        const int index = language_->findData(data);
        if (index >= 0) language_->setItemText(index, caption);
    };
    restateLanguage(QStringLiteral("system"), tr("Use system language"));
    restateLanguage(QStringLiteral("en"), tr("English"));
    restateLanguage(QStringLiteral("uk"), tr("Ukrainian"));
    language_->setAccessibleName(tr("Interface language"));

    refreshQuota();
}


}  // namespace mikai::app
