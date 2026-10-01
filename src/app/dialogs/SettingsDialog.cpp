#include "dialogs/SettingsDialog.hpp"

#include "state/Feeds.hpp"
#include "theme/Colors.hpp"
#include "theme/Icons.hpp"
#include "theme/Metrics.hpp"
#include "workers/Runner.hpp"

#include <backend/MikaiClient.hpp>
#include <player/Anime4K.hpp>
#include <player/PresetShortcuts.hpp>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace mikai::app {
namespace {

/// Rows follow the preset menu categories; experiment labels retain their family.
QString shortcutGroupHeading(const QString& categoryId) {
    const QString label = mplayer::anime4k::categoryLabel(categoryId);
    return label.isEmpty() ? QString() : label;
}

/// The widest thing a one-key field can show: a full modifier stack in front of
/// a function key. Measuring it beats a written-down pixel width, which is
/// exactly the kind of dimension `theme/Metrics.hpp` keeps out of the widgets.
int shortcutFieldWidth(const QKeySequenceEdit* edit) {
    const QFontMetrics metrics(edit->font());
    return metrics.horizontalAdvance(QStringLiteral("Ctrl+Shift+Alt+F12")) +
           theme::kSpacingLarge;
}

}  // namespace

SettingsDialog::SettingsDialog(backend::MikaiClient& client, Runner& runner, QWidget* parent)
    : QDialog(parent), client_(client), runner_(runner) {
    setWindowTitle(tr("Налаштування"));
    setMinimumWidth(theme::kSettingsDialogMinWidth);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(theme::kSpacingLarge, theme::kSpacingLarge, theme::kSpacingLarge,
                             theme::kSpacingLarge);
    root->setSpacing(theme::kSpacingMedium);

    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    tabs_->addTab(buildGeneralTab(), theme::gearIcon(theme::kIconSize, QLatin1String(theme::kAccent)),
                  tr("Загальні"));
    tabs_->addTab(buildPresetsTab(),
                  theme::keyboardIcon(theme::kIconSize, QLatin1String(theme::kAccent)),
                  tr("Пресети"));
    if (!presetError_->isHidden()) {
        tabs_->setCurrentWidget(presetError_->parentWidget());
    }
    root->addWidget(tabs_, 1);

    auto* buttons = new QDialogButtonBox(this);
    auto* cancel = buttons->addButton(tr("Скасувати"), QDialogButtonBox::RejectRole);
    auto* save = buttons->addButton(tr("Зберегти"), QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("primary"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, &SettingsDialog::save);
    root->addWidget(buttons);

    quotaTimer_ = new QTimer(this);
    quotaTimer_->setInterval(1000);
    connect(quotaTimer_, &QTimer::timeout, this, &SettingsDialog::refreshQuota);
    quotaTimer_->start();
    refreshQuota();
}

SettingsDialog::~SettingsDialog() = default;

QWidget* SettingsDialog::buildGeneralTab() {
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium, theme::kSpacingMedium,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    auto* headerRow = new QHBoxLayout();
    auto* iconLabel = new QLabel(page);
    iconLabel->setPixmap(theme::gearIcon(22, QLatin1String(theme::kAccent)).pixmap(22, 22));
    auto* heading = new QLabel(tr("Токен доступу Mikai API"), page);
    heading->setObjectName(QStringLiteral("heading"));
    headerRow->addWidget(iconLabel);
    headerRow->addWidget(heading);
    headerRow->addStretch();
    root->addLayout(headerRow);

    auto* description = new QLabel(
        tr("Ключ доступу підіймає ліміт з 60 до 300 запитів на хвилину і зберігається "
           "локально в .env файлі."),
        page);
    description->setObjectName(QStringLiteral("muted"));
    description->setWordWrap(true);
    root->addWidget(description);

    auto* link = new QLabel(page);
    link->setTextFormat(Qt::RichText);
    link->setText(tr("<a style=\"color: %1; text-decoration: none;\" "
                     "href=\"https://mikai.me/user/developers\">"
                     "Отримати ключ у кабінеті розробника на mikai.me ↗</a>")
                       .arg(QLatin1String(theme::kAccent)));
    link->setOpenExternalLinks(true);
    link->setWordWrap(true);
    root->addWidget(link);

    token_ = new QLineEdit(page);
    token_->setPlaceholderText(tr("mk_... (залиште порожнім для анонімного доступу)"));
    token_->setClearButtonEnabled(true);
    const QString current = client_.apiKey();
    if (!current.isEmpty()) {
        token_->setText(current);
    }
    root->addWidget(token_);

    // The token check is an API concern, so it belongs with the token rather
    // than as a dialog-wide action sitting under the preset bindings.
    auto* tokenRow = new QHBoxLayout();
    tokenRow->setSpacing(theme::kSpacingSmall);
    tokenRow->addStretch();
    testButton_ = new QPushButton(tr("Перевірити"), page);
    connect(testButton_, &QPushButton::clicked, this, &SettingsDialog::testToken);
    tokenRow->addWidget(testButton_);
    root->addLayout(tokenRow);

    auto* quotaTitle = new QLabel(tr("Використання API"), page);
    quotaTitle->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(quotaTitle);
    quota_ = new QLabel(page);
    quota_->setObjectName(QStringLiteral("descriptionPanel"));
    quota_->setWordWrap(true);
    root->addWidget(quota_);

    auto* feedsTitle = new QLabel(tr("Блоки головної сторінки"), page);
    feedsTitle->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(feedsTitle);
    auto* feedsHint = new QLabel(
        tr("Вимкнений блок не завантажується і не витрачає запити API. Пошук і сторінка "
           "тайтлу працюють завжди."),
        page);
    feedsHint->setObjectName(QStringLiteral("muted"));
    feedsHint->setWordWrap(true);
    root->addWidget(feedsHint);

    QSettings settings;
    const QSet<QString> enabled = feeds::enabled(settings.value(feeds::settingsKey()).toString());
    for (const QString& key : feeds::all()) {
        auto* box = new QCheckBox(feeds::label(key), page);
        box->setChecked(enabled.contains(key));
        root->addWidget(box);
        feedBoxes_.insert(key, box);
    }

    status_ = new QLabel(page);
    status_->setObjectName(QStringLiteral("status"));
    status_->setWordWrap(true);
    root->addWidget(status_);

    root->addStretch();
    return page;
}

QWidget* SettingsDialog::buildPresetsTab() {
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium, theme::kSpacingMedium,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingMedium);

    auto* heading = new QLabel(tr("Гарячі клавіші пресетів Anime4K"), page);
    heading->setObjectName(QStringLiteral("heading"));
    root->addWidget(heading);

    auto* description = new QLabel(
        tr("Натисніть поле й задайте клавішу або комбінацію з Ctrl, Alt чи Shift. "
           "Кнопка «×» очищає призначення. Одна комбінація може належати лише одному "
           "пресету; вбудовані клавіші плеєра зарезервовані."),
        page);
    description->setObjectName(QStringLiteral("muted"));
    description->setWordWrap(true);
    root->addWidget(description);

    presetError_ = new QLabel(page);
    presetError_->setObjectName(QStringLiteral("status"));
    presetError_->setWordWrap(true);
    presetError_->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(theme::kDanger)));
    presetError_->hide();
    root->addWidget(presetError_);

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMaximumHeight(theme::kSettingsPresetListMaxHeight);
    auto* content = new QWidget(scroll);
    auto* list = new QVBoxLayout(content);
    list->setContentsMargins(0, 0, theme::kSpacingSmall, 0);
    list->setSpacing(theme::kSpacingXs);

    QSettings settings;
    settings.beginGroup(QLatin1String(mplayer::shell::kPresetShortcutSettingsGroup));
    QMap<QString, QString> loadedBindings;
    QString loadError;
    QString headingForCategory;
    for (const mplayer::anime4k::ModeInfo& mode : mplayer::anime4k::availableModes()) {
        const QString label = mplayer::anime4k::displayLabel(mode.mode);
        const QString headingText = shortcutGroupHeading(mode.category);
        if (!headingText.isEmpty() && headingText != headingForCategory) {
            headingForCategory = headingText;
            auto* groupHeading = new QLabel(headingText, content);
            groupHeading->setObjectName(QStringLiteral("sectionHeading"));
            list->addWidget(groupHeading);
        }

        auto* row = new QHBoxLayout();
        row->setSpacing(theme::kSpacingMedium);

        auto* name = new QLabel(label, content);
        name->setToolTip(mode.hint);
        row->addWidget(name, 1);

        auto* edit = new QKeySequenceEdit(content);
        edit->setMaximumSequenceLength(1);
        edit->setClearButtonEnabled(true);
        edit->setMinimumHeight(theme::kSettingsControlSize);
        edit->setFixedWidth(shortcutFieldWidth(edit));
        if (auto* input = edit->findChild<QLineEdit*>()) {
            input->setPlaceholderText(tr("Натисніть клавішу"));
        }
        const QString stored = settings.value(mode.id).toString();
        if (!stored.isEmpty()) {
            const QKeySequence sequence =
                QKeySequence::fromString(stored, QKeySequence::PortableText);
            if (!sequence.isEmpty()) {
                const QString portable = sequence.toString(QKeySequence::PortableText);
                const QString owner = loadedBindings.value(portable);
                if (mplayer::shell::isReservedPresetShortcut(sequence)) {
                    if (loadError.isEmpty()) {
                        loadError =
                            tr("Збережену клавішу %1 вже використовує плеєр; призначення для "
                               "«%2» очищено.")
                                .arg(portable, label);
                    }
                } else if (!owner.isEmpty()) {
                    if (loadError.isEmpty()) {
                        loadError =
                            tr("Збережена комбінація %1 дублювалася. Її залишено для «%2», "
                               "а призначення «%3» очищено.")
                                .arg(portable, owner, label);
                    }
                } else {
                    edit->setKeySequence(sequence);
                    loadedBindings.insert(portable, label);
                }
            }
        }
        edit->setAccessibleName(tr("Гаряча клавіша для %1").arg(label));
        edit->setToolTip(mode.hint);
        row->addWidget(edit, 0, Qt::AlignVCenter);

        list->addLayout(row);
        presetRows_.append({mode.id, label, edit});
        // A message left over from a rejected save names a key the user has
        // already changed, so it goes as soon as they touch the binding.
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this,
                [this, edit, label, previous = edit->keySequence()](const QKeySequence& sequence) mutable {
                    if (!sequence.isEmpty()) {
                        const QString portable = sequence.toString(QKeySequence::PortableText);
                        for (const PresetRow& other : presetRows_) {
                            if (other.edit == edit || other.edit->keySequence().isEmpty()) {
                                continue;
                            }
                            if (other.edit->keySequence().toString(QKeySequence::PortableText) ==
                                portable) {
                                QSignalBlocker blocker(edit);
                                edit->setKeySequence(previous);
                                showPresetError(
                                    tr("Комбінація %1 вже призначена пресету «%2». "
                                       "Для «%3» залишено попереднє значення.")
                                        .arg(portable, other.label, label));
                                return;
                            }
                        }
                        if (mplayer::shell::isReservedPresetShortcut(sequence)) {
                            QSignalBlocker blocker(edit);
                            edit->setKeySequence(previous);
                            showPresetError(
                                tr("Клавіша %1 вже використовується плеєром. "
                                   "Призначення для «%2» не змінено.")
                                    .arg(portable, label));
                            return;
                        }
                    }
                    previous = sequence;
                    presetError_->clear();
                    presetError_->hide();
                });
    }
    settings.endGroup();
    if (!loadError.isEmpty()) {
        presetError_->setText(loadError);
        presetError_->show();
    }
    list->addStretch();
    scroll->setWidget(content);
    root->addWidget(scroll, 1);
    return page;
}

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
                const bool authenticated = result.value(QStringLiteral("authenticated")).toBool();
                const int limit = result.value(QStringLiteral("rate_limit"), 60).toInt();
                const QString name = result.value(QStringLiteral("name")).toString();
                if (authenticated && !name.isEmpty()) {
                    status_->setText(
                        tr("✓ Ключ дійсний (користувач: %1, ліміт: %2/хв)").arg(name).arg(limit));
                } else if (authenticated) {
                    status_->setText(tr("✓ Ключ дійсний (ліміт: %1/хв)").arg(limit));
                } else {
                    status_->setText(tr("Анонімний доступ (ліміт: %1/хв)").arg(limit));
                }
            } else {
                const QString error = result.value(QStringLiteral("error")).toString();
                status_->setText(
                    tr("❌ Помилка: %1").arg(error.isEmpty() ? tr("Недійсний токен") : error));
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

QString SettingsDialog::collectPresetShortcuts(QMap<QString, QString>* presets) const {
    // PortableText is the comparison form: two fields showing the same combo
    // must collide however either was typed.
    QMap<QString, QString> ownerBySequence;
    for (const PresetRow& row : presetRows_) {
        const QKeySequence sequence = row.edit->keySequence();
        if (sequence.isEmpty()) {
            continue;
        }
        const QString portable = sequence.toString(QKeySequence::PortableText);
        if (mplayer::shell::isReservedPresetShortcut(sequence)) {
            return tr("Клавіша %1 вже зайнята плеєром і не може бути призначена пресету «%2».")
                .arg(portable, row.label);
        }
        const QString owner = ownerBySequence.value(portable);
        if (!owner.isEmpty()) {
            return tr("Комбінація %1 вже призначена пресету «%2».").arg(portable, owner);
        }
        ownerBySequence.insert(portable, row.label);
        presets->insert(row.id, portable);
    }
    return {};
}

void SettingsDialog::showPresetError(const QString& message) {
    presetError_->setText(message);
    presetError_->show();
    tabs_->setCurrentIndex(1);
}

void SettingsDialog::save() {
    // Every binding is checked before anything is written: a rejected shortcut
    // must not leave the token and feeds saved while the dialog stays open.
    QMap<QString, QString> presets;
    const QString error = collectPresetShortcuts(&presets);
    if (!error.isEmpty()) {
        showPresetError(error);
        return;
    }
    presetError_->clear();
    presetError_->hide();

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
    settings.beginGroup(QLatin1String(mplayer::shell::kPresetShortcutSettingsGroup));
    for (const PresetRow& row : presetRows_) {
        // A blank binding is a removal, not an empty value: the player reads an
        // unset preset as the key being absent.
        if (presets.contains(row.id)) {
            settings.setValue(row.id, presets.value(row.id));
        } else {
            settings.remove(row.id);
        }
    }
    settings.endGroup();
    accept();
}

}  // namespace mikai::app