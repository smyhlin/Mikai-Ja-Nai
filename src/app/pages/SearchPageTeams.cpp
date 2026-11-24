#include "pages/SearchPageInternal.hpp"

namespace mikai::app {
using namespace search_page_detail;

void SearchPage::openTeamPicker() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Teams directory"));
    dialog.setObjectName(QStringLiteral("teamPickerDialog"));
    dialog.setMinimumSize(theme::kSettingsDialogMinWidth, theme::kTeamPickerMinHeight);
    dialog.resize(qMax(theme::kCatalogueFiltersWidth, 760),
                  qMax(theme::kTeamPickerDefaultHeight, 600));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(theme::kSpacingLarge, theme::kSpacingLarge,
                               theme::kSpacingLarge, theme::kSpacingLarge);
    layout->setSpacing(theme::kSpacingMedium);

    auto* dirHeader = new QFrame(&dialog);
    dirHeader->setObjectName(QStringLiteral("settingsCard"));
    auto* dirHeaderLayout = new QVBoxLayout(dirHeader);
    dirHeaderLayout->setContentsMargins(theme::kSpacingLarge, theme::kSpacingMedium,
                                        theme::kSpacingLarge, theme::kSpacingMedium);
    dirHeaderLayout->setSpacing(theme::kSpacingXs);
    auto* dirTitle = new QLabel(tr("Ukrainian Dubbing & Subtitles Teams"), dirHeader);
    dirTitle->setObjectName(QStringLiteral("sectionHeading"));
    auto* dirSub = new QLabel(
        tr("Double-click a team to browse its anime catalogue, or check multiple teams to filter."),
        dirHeader);
    dirSub->setObjectName(QStringLiteral("muted"));
    dirSub->setWordWrap(true);
    dirHeaderLayout->addWidget(dirTitle);
    dirHeaderLayout->addWidget(dirSub);
    layout->addWidget(dirHeader);
    auto* search = new QLineEdit(&dialog);
    search->setObjectName(QStringLiteral("teamSearchEdit"));
    search->setPlaceholderText(tr("Search teams (Ctrl+F or /)"));
    search->setAccessibleName(tr("Search Ukrainian dubbing and subtitles teams"));
    search->setClearButtonEnabled(true);
    auto* rows = new QListWidget(&dialog);
    rows->setObjectName(QStringLiteral("teamResultsList"));
    rows->setAccessibleName(tr("Teams directory list"));
    rows->setAccessibleDescription(
        tr("Press Enter to browse a team's anime, or Space to toggle catalogue filtering."));
    rows->setUniformItemSizes(true);
    auto* focusSearchShortcut1 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), &dialog);
    auto* focusSearchShortcut2 = new QShortcut(QKeySequence(Qt::Key_Slash), &dialog);
    const auto focusTeamSearch = [search] {
        search->setFocus(Qt::ShortcutFocusReason);
        search->selectAll();
    };
    connect(focusSearchShortcut1, &QShortcut::activated, &dialog, focusTeamSearch);
    connect(focusSearchShortcut2, &QShortcut::activated, &dialog, [search, focusTeamSearch] {
        if (!search->hasFocus()) focusTeamSearch();
    });
    auto* more = new QPushButton(tr("Load more"), &dialog);
    more->setObjectName(QStringLiteral("teamLoadMoreButton"));
    theme::applyActionRole(more, theme::ActionRole::Ghost);
    auto* browse = new QPushButton(tr("Browse anime"), &dialog);
    browse->setObjectName(QStringLiteral("teamBrowseButton"));
    theme::applyActionRole(browse, theme::ActionRole::Secondary);
    browse->setEnabled(false);
    auto* filterTeam = new QPushButton(tr("Filter catalogue"), &dialog);
    filterTeam->setObjectName(QStringLiteral("teamFilterButton"));
    theme::applyActionRole(filterTeam, theme::ActionRole::Primary);
    auto* cancelBtn = new QPushButton(tr("Cancel"), &dialog);
    cancelBtn->setObjectName(QStringLiteral("teamCancelButton"));
    theme::applyActionRole(cancelBtn, theme::ActionRole::Ghost);
    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    auto* teamControlsRow = new QHBoxLayout();
    teamControlsRow->setContentsMargins(0, 0, 0, 0);
    teamControlsRow->setSpacing(theme::kSpacingSmall);
    auto* voiceFilter = new QPushButton(tr("Voice"), &dialog);
    voiceFilter->setObjectName(QStringLiteral("teamVoiceFilterButton"));
    voiceFilter->setCheckable(true);
    voiceFilter->setToolTip(tr("Show only teams with Ukrainian voice dubbing"));
    theme::applyActionRole(voiceFilter, theme::ActionRole::Secondary);
    auto* subsFilter = new QPushButton(tr("Subtitles"), &dialog);
    subsFilter->setObjectName(QStringLiteral("teamSubsFilterButton"));
    subsFilter->setCheckable(true);
    subsFilter->setToolTip(tr("Show only teams with Ukrainian subtitles"));
    theme::applyActionRole(subsFilter, theme::ActionRole::Secondary);
    teamControlsRow->addWidget(voiceFilter);
    teamControlsRow->addWidget(subsFilter);
    teamControlsRow->addStretch(1);

    auto* teamSortBox = new QComboBox(&dialog);
    teamSortBox->setObjectName(QStringLiteral("teamSortCombo"));
    teamSortBox->setToolTip(tr("Sort teams"));
    teamSortBox->addItem(tr("Default order"), QString());
    teamSortBox->addItem(tr("Most anime"), QStringLiteral("count"));
    teamSortBox->addItem(tr("Recently updated"), QStringLiteral("updated"));
    teamSortBox->addItem(tr("Name"), QStringLiteral("name"));
    teamControlsRow->addWidget(teamSortBox);

    auto* teamOrderBtn = new QPushButton(tr("↓ Desc"), &dialog);
    teamOrderBtn->setObjectName(QStringLiteral("teamOrderButton"));
    teamOrderBtn->setCheckable(true);
    teamOrderBtn->setToolTip(tr("Toggle ascending / descending order"));
    theme::applyActionRole(teamOrderBtn, theme::ActionRole::Secondary);
    teamControlsRow->addWidget(teamOrderBtn);

    auto* selectedStrip = new FlowScrollHost(&dialog);
    selectedStrip->setObjectName(QStringLiteral("teamSelectedChipsStrip"));
    auto* selectedFlow = new FlowLayout(theme::kSpacingXs, theme::kSpacingXs);
    selectedFlow->setContentsMargins(0, 0, 0, 0);
    selectedStrip->setLayout(selectedFlow);
    selectedStrip->hide();

    auto* state = new QLabel(&dialog);
    state->setObjectName(QStringLiteral("muted"));
    layout->addWidget(search);
    layout->addLayout(teamControlsRow);
    layout->addWidget(selectedStrip);
    auto* emptyCard = new QFrame(&dialog);
    emptyCard->setObjectName(QStringLiteral("settingsCard"));
    auto* emptyLayout = new QVBoxLayout(emptyCard);
    emptyLayout->setContentsMargins(theme::kSpacingLarge, theme::kSpacingLarge,
                                    theme::kSpacingLarge, theme::kSpacingLarge);
    emptyLayout->setSpacing(theme::kSpacingSmall);
    auto* emptyTitle = new QLabel(tr("No teams found"), emptyCard);
    emptyTitle->setObjectName(QStringLiteral("sectionHeading"));
    auto* emptySub = new QLabel(tr("Try a different name or clear Voice / Subtitles filters."), emptyCard);
    emptySub->setObjectName(QStringLiteral("muted"));
    emptySub->setWordWrap(true);
    auto* emptyResetBtn = new QPushButton(tr("Reset search & filters"), emptyCard);
    theme::applyActionRole(emptyResetBtn, theme::ActionRole::Secondary);
    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(emptySub);
    emptyLayout->addWidget(emptyResetBtn, 0, Qt::AlignLeft);
    emptyCard->hide();

    layout->addWidget(state);
    layout->addWidget(emptyCard);
    layout->addWidget(rows, 1);
    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(theme::kSpacingSmall);
    buttons->addWidget(more);
    buttons->addStretch();
    buttons->addWidget(cancelBtn);
    buttons->addWidget(browse);
    buttons->addWidget(filterTeam);
    layout->addLayout(buttons);
    auto paging = std::make_shared<int>(1);
    auto totalPages = std::make_shared<int>(1);
    auto currentTerm = std::make_shared<QString>();
    auto request = std::make_shared<int>(0);
    auto inFlight = std::make_shared<bool>(false);
    auto selected = std::make_shared<backend::Team>();
    auto pickedTeamIds = std::make_shared<QSet<qint64>>();
    auto pickedTeamNames = std::make_shared<QHash<qint64, QString>>();
    for (qint64 id : catalogueQuery_.teams) pickedTeamIds->insert(id);
    const auto refreshSelectedChips = [selectedStrip, selectedFlow, pickedTeamIds, pickedTeamNames,
                                       rows, filterTeam]() {
        while (QLayoutItem* item = selectedFlow->takeAt(0)) {
            if (QWidget* w = item->widget()) w->deleteLater();
            delete item;
        }
        const int n = pickedTeamIds->size();
        filterTeam->setText(n > 0 ? tr("Filter catalogue (%1)").arg(n) : tr("Filter catalogue"));
        if (n == 0) {
            selectedStrip->hide();
            return;
        }
        for (qint64 id : *pickedTeamIds) {
            const QString lbl = pickedTeamNames->value(id, tr("Team #%1").arg(id));
            auto* chip = new QPushButton(lbl + QStringLiteral("  ×"), selectedStrip);
            chip->setObjectName(QStringLiteral("activeFilterChip"));
            chip->setCursor(Qt::PointingHandCursor);
            QObject::connect(chip, &QPushButton::clicked, selectedStrip, [id, pickedTeamIds, rows]() {
                pickedTeamIds->remove(id);
                for (int r = 0; r < rows->count(); ++r) {
                    QListWidgetItem* it = rows->item(r);
                    if (it->data(Qt::UserRole).toMap().value(QStringLiteral("id")).toLongLong() == id) {
                        it->setCheckState(Qt::Unchecked);
                        return;
                    }
                }
            });
            selectedFlow->addWidget(chip);
        }
        auto* clearBtn = new QPushButton(tr("Clear selected"), selectedStrip);
        clearBtn->setObjectName(QStringLiteral("clearFiltersChip"));
        clearBtn->setCursor(Qt::PointingHandCursor);
        QObject::connect(clearBtn, &QPushButton::clicked, selectedStrip,
                         [pickedTeamIds, rows, selectedStrip, filterTeam]() {
                             pickedTeamIds->clear();
                             for (int r = 0; r < rows->count(); ++r) {
                                 if (rows->item(r)->checkState() == Qt::Checked) {
                                     rows->item(r)->setCheckState(Qt::Unchecked);
                                 }
                             }
                             selectedStrip->hide();
                             filterTeam->setText(tr("Filter catalogue"));
                         });
        selectedFlow->addWidget(clearBtn);
        selectedStrip->show();
    };
    refreshSelectedChips();
    const QPointer<QListWidget> guardedRows(rows);
    const QPointer<QPushButton> guardedMore(more);
    const QPointer<QLabel> guardedState(state);
    const QPointer<QFrame> guardedEmpty(emptyCard);
    backend::MikaiClient* client = &client_;
    auto fetch = [this, client, guardedRows, guardedMore, guardedState, guardedEmpty, paging,
                  totalPages, currentTerm, request, inFlight, pickedTeamIds, voiceFilter,
                  subsFilter, teamSortBox, teamOrderBtn]
                 (const QString& term, int page) {
        *currentTerm = term;
        if (page == 1) {
            if (guardedRows) guardedRows->clear();
            *paging = 0;
            *totalPages = 1;
        }
        const int seq = ++*request;
        *inFlight = true;
        if (guardedState) guardedState->setText(tr("Loading…"));
        if (guardedMore) {
            guardedMore->setEnabled(false);
            guardedMore->setText(tr("Loading…"));
        }
        const std::optional<bool> haveVoice =
            voiceFilter->isChecked() ? std::optional<bool>(true) : std::nullopt;
        const std::optional<bool> haveSubs =
            subsFilter->isChecked() ? std::optional<bool>(true) : std::nullopt;
        const QString sortKey = teamSortBox->currentData().toString();
        const QString orderKey =
            sortKey.isEmpty() ? QString()
                              : (teamOrderBtn->isChecked() ? QStringLiteral("asc")
                                                           : QStringLiteral("desc"));
        runner_.run([client, term, page, haveVoice, haveSubs, sortKey, orderKey] {
            return QVariant::fromValue(
                client->getTeams(term, page, 50, haveVoice, haveSubs, sortKey, orderKey));
        }, [guardedRows, guardedMore, guardedState, guardedEmpty, paging, totalPages, request,
            inFlight, seq, page, pickedTeamIds]
             (const QVariant& v) {
            if (seq != *request || !guardedRows) return;
            *inFlight = false;
            if (guardedMore) guardedMore->setText(QObject::tr("Load more"));
            const backend::TeamsResult result = v.value<backend::TeamsResult>();
            const auto formatCompact = [](qint64 n) -> QString {
                if (n >= 1000000) return QStringLiteral("%1M").arg(n / 1000000.0, 0, 'f', 1);
                if (n >= 1000) return QStringLiteral("%1K").arg(n / 1000.0, 0, 'f', 1);
                return QLocale().toString(n);
            };
            for (const auto& profile : result.items) {
                // Populate off-list: itemChanged would erase seeded checks during initialization.
                QStringList meta;
                if (profile.has_voice) meta << QObject::tr("Voice");
                if (profile.has_subs) meta << QObject::tr("Subs");
                if (profile.anime_count > 0) {
                    meta << QObject::tr("%n anime", nullptr, profile.anime_count);
                }
                if (profile.views_total > 0) {
                    meta << QObject::tr("%1 views").arg(formatCompact(profile.views_total));
                }
                const QString rowText =
                    meta.isEmpty()
                        ? profile.team.name
                        : QStringLiteral("%1  ·  %2").arg(profile.team.name, meta.join(QStringLiteral(" · ")));
                auto* item = new QListWidgetItem(rowText);
                item->setSizeHint(QSize(0, 38));
                if (!profile.description.trimmed().isEmpty()) {
                    item->setToolTip(profile.team.name + QStringLiteral("\n") +
                                     profile.description.trimmed().left(240));
                }
                item->setData(Qt::UserRole, QVariantMap{
                    {QStringLiteral("id"), profile.team.id},
                    {QStringLiteral("slug"), profile.team.slug},
                    {QStringLiteral("name"), profile.team.name}});
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(pickedTeamIds->contains(profile.team.id)
                                        ? Qt::Checked : Qt::Unchecked);
                guardedRows->addItem(item);
            }
            *paging = page;
            const int total = result.total.value_or(guardedRows->count());
            *totalPages = result.pages.value_or(
                result.total.has_value() ? qMax(1, (total + 49) / 50)
                                         : page + (result.items.size() == 50 ? 1 : 0));
            if (guardedState) {
                guardedState->setText(QObject::tr("%1 teams · page %2 of %3")
                                          .arg(total).arg(page).arg(*totalPages));
                QAccessibleEvent ev(guardedState.data(), QAccessible::NameChanged);
                QAccessible::updateAccessibility(&ev);
            }
            const bool isEmpty = guardedRows->count() == 0;
            if (guardedEmpty) guardedEmpty->setVisible(isEmpty);
            guardedRows->setVisible(!isEmpty);
            if (guardedMore) guardedMore->setEnabled(page < *totalPages);
        }, [guardedState, guardedMore, guardedEmpty, request, inFlight, seq](const backend::Failure& failure) {
            if (guardedEmpty) guardedEmpty->hide();
            if (seq != *request) return;
            *inFlight = false;
            if (guardedState) guardedState->setText(failure.message);
            if (guardedMore) {
                guardedMore->setText(QObject::tr("Retry"));
                guardedMore->setEnabled(true);
            }
        });
    };
    auto* teamSearchTimer = new QTimer(&dialog);
    teamSearchTimer->setSingleShot(true);
    teamSearchTimer->setInterval(280);
    connect(teamSearchTimer, &QTimer::timeout, &dialog,
            [fetch, search] { fetch(search->text().trimmed(), 1); });
    connect(search, &QLineEdit::textChanged, &dialog,
            [teamSearchTimer](const QString&) { teamSearchTimer->start(); });
    connect(search, &QLineEdit::returnPressed, &dialog, [teamSearchTimer, fetch, search] {
        teamSearchTimer->stop();
        fetch(search->text().trimmed(), 1);
    });
    connect(voiceFilter, &QPushButton::toggled, &dialog,
            [fetch, search](bool) { fetch(search->text().trimmed(), 1); });
    connect(subsFilter, &QPushButton::toggled, &dialog,
            [fetch, search](bool) { fetch(search->text().trimmed(), 1); });
    connect(teamSortBox, &QComboBox::currentIndexChanged, &dialog,
            [fetch, search](int) { fetch(search->text().trimmed(), 1); });
    connect(teamOrderBtn, &QPushButton::toggled, &dialog,
            [fetch, search, teamOrderBtn](bool asc) {
                teamOrderBtn->setText(asc ? tr("↑ Asc") : tr("↓ Desc"));
                fetch(search->text().trimmed(), 1);
            });
    connect(emptyResetBtn, &QPushButton::clicked, &dialog,
            [search, voiceFilter, subsFilter, teamSortBox, fetch] {
                search->clear();
                voiceFilter->setChecked(false);
                subsFilter->setChecked(false);
                teamSortBox->setCurrentIndex(0);
                fetch(QString(), 1);
            });
    connect(more, &QPushButton::clicked, &dialog, [fetch, paging, totalPages, currentTerm] {
        if (*paging < *totalPages) fetch(*currentTerm, *paging + 1);
    });
    if (QScrollBar* vbar = rows->verticalScrollBar()) {
        connect(vbar, &QScrollBar::valueChanged, &dialog,
                [fetch, paging, totalPages, currentTerm, inFlight, vbar](int value) {
                    if (*inFlight || *paging >= *totalPages || vbar->maximum() <= 0) return;
                    if (value >= vbar->maximum() - 120) {
                        fetch(*currentTerm, *paging + 1);
                    }
                });
    }
    const auto syncSelectedFromItem = [selected, browse](QListWidgetItem* item) {
        *selected = backend::Team{};
        browse->setEnabled(item != nullptr);
        if (item == nullptr) return;
        const QVariantMap data = item->data(Qt::UserRole).toMap();
        selected->id = data.value(QStringLiteral("id")).toLongLong();
        selected->slug = data.value(QStringLiteral("slug")).toString();
        selected->name = data.value(QStringLiteral("name")).toString();
    };
    connect(rows, &QListWidget::itemChanged, &dialog,
            [pickedTeamIds, pickedTeamNames, selected, syncSelectedFromItem,
             refreshSelectedChips](QListWidgetItem* item) {
                const QVariantMap data = item->data(Qt::UserRole).toMap();
                const qint64 id = data.value(QStringLiteral("id")).toLongLong();
                const QString name = data.value(QStringLiteral("name")).toString();
                if (item->checkState() == Qt::Checked) {
                    pickedTeamIds->insert(id);
                    if (!name.isEmpty()) pickedTeamNames->insert(id, name);
                    if (selected->name.isEmpty()) syncSelectedFromItem(item);
                } else {
                    pickedTeamIds->remove(id);
                }
                refreshSelectedChips();
            });
    connect(rows, &QListWidget::currentItemChanged, &dialog, syncSelectedFromItem);
    connect(rows, &QListWidget::itemDoubleClicked, &dialog,
            [&dialog, syncSelectedFromItem](QListWidgetItem* item) {
                if (item == nullptr) return;
                syncSelectedFromItem(item);
                dialog.accept();
            });
    connect(rows, &QListWidget::itemActivated, &dialog,
            [&dialog, syncSelectedFromItem](QListWidgetItem* item) {
                if (item == nullptr) return;
                syncSelectedFromItem(item);
                dialog.accept();
            });
    bool filterSelected = false;
    connect(browse, &QPushButton::clicked, &dialog, [&dialog, selected] {
        if (!selected->name.isEmpty()) dialog.accept();
    });
    fetch(QString(), 1);
    connect(filterTeam, &QPushButton::clicked, &dialog, [&dialog, &filterSelected] {
        filterSelected = true;
        dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted && (filterSelected || !selected->name.isEmpty())) {
        if (filterSelected) {
            catalogueQuery_.teams.clear();
            for (qint64 id : *pickedTeamIds) catalogueQuery_.teams.append(id);
            applyCatalogueFilters();
        } else {
            browseSelectedTeam(*selected);
        }
    }
}

void SearchPage::browseTeam(const QString& ref) {
    if (ref.trimmed().isEmpty()) return;
    const int seq = ++teamRequestSeq_;
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run([client, ref] { return QVariant::fromValue(client->getTeam(ref)); },
        [self, seq](const QVariant& value) {
            if (self.isNull() || seq != self->teamRequestSeq_) return;
            const backend::TeamProfile profile = value.value<backend::TeamProfile>();
            self->activeTeamProfile_ = profile;
            self->browseSelectedTeam(profile.team);
        }, [self, seq](const backend::Failure& failure) {
            if (self.isNull() || seq != self->teamRequestSeq_) return;
            self->statusLine_ = Status::Failure;
            self->statusFailure_ = failure;
            self->renderStatus();
        });
}

void SearchPage::browseSelectedTeam(const backend::Team& team) {
    ++requestSeq_;
    ++catalogueRequestSeq_;
    const bool hasCachedProfile = activeTeamProfile_.team.id == team.id &&
                                  !activeTeamProfile_.team.name.isEmpty();
    if (!hasCachedProfile) activeTeamProfile_ = backend::TeamProfile{};
    activeTeam_ = team;
    teamBrowseMode_ = true;
    resultsHeading_->setText(tr("Anime from %1").arg(team.name));
    showSearchResults();
    if (teamBackButton_ == nullptr) {
        teamHeroCard_ = new QFrame(this);
        teamHeroCard_->setObjectName(QStringLiteral("settingsCard"));
        auto* heroV = new QVBoxLayout(teamHeroCard_);
        heroV->setContentsMargins(theme::kSpacingLarge, theme::kSpacingMedium,
                                  theme::kSpacingLarge, theme::kSpacingMedium);
        heroV->setSpacing(theme::kSpacingSmall);

        auto* topRow = new QHBoxLayout();
        topRow->setContentsMargins(0, 0, 0, 0);
        topRow->setSpacing(theme::kSpacingMedium);

        teamDirectoryButton_ = new QPushButton(tr("← Teams"), teamHeroCard_);
        teamDirectoryButton_->setObjectName(QStringLiteral("teamDirectoryButton"));
        theme::applyActionRole(teamDirectoryButton_, theme::ActionRole::Ghost);
        connect(teamDirectoryButton_, &QPushButton::clicked, this, &SearchPage::openTeamPicker);

        teamBreadcrumbLabel_ = new QLabel(teamHeroCard_);
        teamBreadcrumbLabel_->setObjectName(QStringLiteral("muted"));

        teamBackButton_ = new QPushButton(tr("All catalogue"), teamHeroCard_);
        teamBackButton_->setObjectName(QStringLiteral("teamBackButton"));
        theme::applyActionRole(teamBackButton_, theme::ActionRole::Ghost);

        topRow->addWidget(teamDirectoryButton_, 0, Qt::AlignVCenter);
        topRow->addWidget(teamBreadcrumbLabel_, 0, Qt::AlignVCenter);
        topRow->addStretch(1);
        topRow->addWidget(teamBackButton_, 0, Qt::AlignVCenter);
        heroV->addLayout(topRow);

        auto* bodyRow = new QHBoxLayout();
        bodyRow->setContentsMargins(0, 0, 0, 0);
        bodyRow->setSpacing(theme::kSpacingMedium);

        teamMonogram_ = new QLabel(teamHeroCard_);
        teamMonogram_->setObjectName(QStringLiteral("teamMonogram"));
        teamMonogram_->setFixedSize(48, 48);
        teamMonogram_->setAlignment(Qt::AlignCenter);
        QFont monoFont = teamMonogram_->font();
        monoFont.setPixelSize(18);
        monoFont.setWeight(QFont::Bold);
        teamMonogram_->setFont(monoFont);
        bodyRow->addWidget(teamMonogram_, 0, Qt::AlignTop);

        teamDetailLabel_ = new QLabel(teamHeroCard_);
        teamDetailLabel_->setObjectName(QStringLiteral("teamDetailLabel"));
        teamDetailLabel_->setWordWrap(true);
        teamDetailLabel_->setTextFormat(Qt::RichText);
        teamDetailLabel_->setOpenExternalLinks(false);
        teamDetailLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
        connect(teamDetailLabel_, &QLabel::linkActivated, this, [this](const QString& target) {
            if (target == QLatin1String("mikai-action://retry-team")) {
                browseSelectedTeam(activeTeam_);
                return;
            }
            const QUrl url(target);
            if (!url.isValid() || url.host().isEmpty() ||
                (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
                return;
            }
            if (!QDesktopServices::openUrl(url)) {
                reportFailure(Notice::Browser);
            }
        });
        bodyRow->addWidget(teamDetailLabel_, 1);
        heroV->addLayout(bodyRow);

        auto* actionsRow = new QHBoxLayout();
        actionsRow->setContentsMargins(0, 0, 0, 0);
        actionsRow->setSpacing(theme::kSpacingSmall);
        teamCombineFiltersButton_ = new QPushButton(tr("Combine with filters…"), teamHeroCard_);
        teamCombineFiltersButton_->setObjectName(QStringLiteral("teamCombineFiltersButton"));
        theme::applyActionRole(teamCombineFiltersButton_, theme::ActionRole::Secondary);
        connect(teamCombineFiltersButton_, &QPushButton::clicked, this, [this] {
            if (activeTeam_.id > 0 && !catalogueQuery_.teams.contains(activeTeam_.id)) {
                catalogueQuery_.teams.append(activeTeam_.id);
            }
            showCatalogueFilters();
        });
        teamCopyLinkButton_ = new QPushButton(tr("Copy link"), teamHeroCard_);
        teamCopyLinkButton_->setObjectName(QStringLiteral("teamCopyLinkButton"));
        theme::applyActionRole(teamCopyLinkButton_, theme::ActionRole::Ghost);
        connect(teamCopyLinkButton_, &QPushButton::clicked, this, [this] {
            const QString ref =
                activeTeam_.slug.isEmpty() ? QString::number(activeTeam_.id) : activeTeam_.slug;
            const QString url = QStringLiteral("https://mikai.me/teams/%1").arg(ref);
            if (!mikai::gui::copyText(url)) reportFailure(Notice::CopyLink);
        });
        teamOpenWebButton_ = new QPushButton(tr("Open on mikai.me"), teamHeroCard_);
        teamOpenWebButton_->setObjectName(QStringLiteral("teamOpenWebButton"));
        theme::applyActionRole(teamOpenWebButton_, theme::ActionRole::Ghost);
        connect(teamOpenWebButton_, &QPushButton::clicked, this, [this] {
            const QString ref =
                activeTeam_.slug.isEmpty() ? QString::number(activeTeam_.id) : activeTeam_.slug;
            const QUrl url(QStringLiteral("https://mikai.me/teams/%1").arg(ref));
            if (!QDesktopServices::openUrl(url)) reportFailure(Notice::Browser);
        });
        actionsRow->addWidget(teamCombineFiltersButton_);
        actionsRow->addWidget(teamCopyLinkButton_);
        actionsRow->addWidget(teamOpenWebButton_);
        teamPostersButton_ = new QPushButton(tr("UA Posters"), teamHeroCard_);
        teamPostersButton_->setObjectName(QStringLiteral("teamPostersButton"));
        teamPostersButton_->setToolTip(tr("Browse Ukrainian localized posters created by this team"));
        theme::applyActionRole(teamPostersButton_, theme::ActionRole::Ghost);
        connect(teamPostersButton_, &QPushButton::clicked, this, [this] {
            if (activeTeam_.id <= 0) return;
            QDialog dlg(this);
            dlg.setWindowTitle(tr("UA Posters — %1").arg(activeTeam_.name));
            dlg.resize(560, 440);
            auto* lay = new QVBoxLayout(&dlg);
            lay->setContentsMargins(theme::kSpacingLarge, theme::kSpacingMedium,
                                    theme::kSpacingLarge, theme::kSpacingMedium);
            lay->setSpacing(theme::kSpacingSmall);
            auto* statusLbl = new QLabel(tr("Loading localized Ukrainian posters…"), &dlg);
            statusLbl->setObjectName(QStringLiteral("muted"));
            auto* list = new QListWidget(&dlg);
            list->setObjectName(QStringLiteral("teamPostersList"));
            auto* closeBtn = new QPushButton(tr("Close"), &dlg);
            theme::applyActionRole(closeBtn, theme::ActionRole::Secondary);
            connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
            lay->addWidget(statusLbl);
            lay->addWidget(list, 1);
            lay->addWidget(closeBtn, 0, Qt::AlignRight);

            backend::MikaiClient* client = &client_;
            const qint64 teamId = activeTeam_.id;
            QPointer<QLabel> statusGuard(statusLbl);
            QPointer<QListWidget> listGuard(list);
            runner_.run(
                [client, teamId] {
                    return QVariant::fromValue(client->getUaPosters(1, 20, {}, teamId));
                },
                [statusGuard, listGuard](const QVariant& v) {
                    if (statusGuard.isNull() || listGuard.isNull()) return;
                    const QJsonObject payload = v.toJsonObject();
                    const QJsonArray items = payload.value(QStringLiteral("items")).toArray();
                    if (items.isEmpty()) {
                        statusGuard->setText(tr("This team has no localized Ukrainian posters yet."));
                        return;
                    }
                    statusGuard->setText(
                        items.size() == 1
                            ? tr("1 localized Ukrainian poster")
                            : tr("%1 localized Ukrainian posters").arg(items.size()));
                    for (const QJsonValue& val : items) {
                        const QJsonObject obj = val.toObject();
                        const QString author = obj.value(QStringLiteral("author")).toString();
                        const QString animeTitle =
                            obj.value(QStringLiteral("anime")).toObject().value(QStringLiteral("title")).toString();
                        const QString url =
                            obj.value(QStringLiteral("images")).toObject().value(QStringLiteral("big")).toObject().value(QStringLiteral("webp")).toString();
                        QString label = animeTitle.isEmpty() ? tr("Poster #%1").arg(obj.value(QStringLiteral("id")).toInteger())
                                                             : animeTitle;
                        if (!author.isEmpty()) label += QStringLiteral(" · %1").arg(author);
                        auto* row = new QListWidgetItem(label, listGuard.data());
                        row->setData(Qt::UserRole, url);
                        row->setToolTip(url);
                    }
                },
                [statusGuard](const backend::Failure& failure) {
                    if (!statusGuard.isNull()) {
                        statusGuard->setText(
                            failure.message.isEmpty()
                                ? tr("Localized posters are unavailable on this server.")
                                : failure.message);
                    }
                });
            connect(list, &QListWidget::itemActivated, &dlg, [](QListWidgetItem* item) {
                if (item == nullptr) return;
                const QUrl url(item->data(Qt::UserRole).toString());
                if (url.isValid() && (url.scheme() == QLatin1String("http") ||
                                      url.scheme() == QLatin1String("https"))) {
                    QDesktopServices::openUrl(url);
                }
            });
            dlg.exec();
        });
        actionsRow->addWidget(teamPostersButton_);
        actionsRow->addStretch(1);
        teamFilterEdit_ = new QLineEdit(teamHeroCard_);
        teamFilterEdit_->setObjectName(QStringLiteral("teamInlineSearchEdit"));
        teamFilterEdit_->setPlaceholderText(tr("Filter this team's anime…"));
        teamFilterEdit_->setClearButtonEnabled(true);
        teamFilterEdit_->setMaximumWidth(240);
        connect(teamFilterEdit_, &QLineEdit::returnPressed, this, [this] {
            if (teamBrowseMode_) loadTeamAnime(1);
        });
        connect(teamFilterEdit_, &QLineEdit::textChanged, this, [this](const QString& t) {
            if (teamBrowseMode_ && t.trimmed().isEmpty()) loadTeamAnime(1);
        });
        actionsRow->addWidget(teamFilterEdit_);
        heroV->addLayout(actionsRow);

        if (pageIntro_ != nullptr) {
            if (auto* intro = qobject_cast<QVBoxLayout*>(pageIntro_->layout())) {
                intro->insertWidget(1, teamHeroCard_);
            }
        }
        connect(teamBackButton_, &QPushButton::clicked, this, [this] {
            teamBrowseMode_ = false;
            ++requestSeq_;
            ++catalogueRequestSeq_;
            if (teamHeroCard_ != nullptr) teamHeroCard_->hide();
            teamBackButton_->hide();
            teamDetailLabel_->hide();
            applyCatalogueFilters();
        });
    }
    if (teamFilterEdit_ != nullptr) teamFilterEdit_->clear();
    if (teamMonogram_ != nullptr) {
        teamMonogram_->setText(team.name.trimmed().left(2).toUpper());
    }
    if (teamBreadcrumbLabel_ != nullptr) {
        teamBreadcrumbLabel_->setText(QStringLiteral("›  %1").arg(team.name));
    }
    if (teamHeroCard_ != nullptr) teamHeroCard_->show();
    teamBackButton_->setText(tr("All catalogue"));
    teamBackButton_->show();
    teamDetailLabel_->show();
    if (hasCachedProfile) {
        renderTeamProfile();
    } else {
        teamDetailLabel_->setText(tr("Loading team…").toHtmlEscaped());
        const int seq = ++teamRequestSeq_;
        const QString ref = team.slug.isEmpty() ? QString::number(team.id) : team.slug;
        // The client travels into the worker by value; the page is reached only
        // through a guard.
        backend::MikaiClient* client = &client_;
        const QPointer<SearchPage> self(this);
        runner_.run([client, ref] { return QVariant::fromValue(client->getTeam(ref)); },
            [self, seq](const QVariant& value) {
                if (self.isNull() || seq != self->teamRequestSeq_ || !self->teamBrowseMode_) return;
                self->activeTeamProfile_ = value.value<backend::TeamProfile>();
                self->renderTeamProfile();
            }, [self, seq](const backend::Failure& failure) {
                if (self.isNull() || seq != self->teamRequestSeq_) return;
                self->teamDetailLabel_->setText(
                    QStringLiteral("%1 — <a href=\"mikai-action://retry-team\">%2</a>")
                        .arg(failure.message.toHtmlEscaped(), tr("Retry")));
            });
    }
    loadTeamAnime(1);
}

void SearchPage::renderTeamProfile() {
    if (teamDetailLabel_ == nullptr || activeTeamProfile_.team.name.isEmpty()) return;
    const backend::TeamProfile& profile = activeTeamProfile_;
    QStringList details{profile.team.name.toHtmlEscaped()};
    if (!profile.description.isEmpty()) {
        details << profile.description.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    }
    if (!profile.birthday.isEmpty()) details << tr("Birthday: %1").arg(profile.birthday).toHtmlEscaped();
    const QLocale loc;
    details << tr("%1 anime · %2 total views · %3 views this month")
                   .arg(loc.toString(profile.anime_count),
                        loc.toString(profile.views_total),
                        loc.toString(profile.views_last_30_days))
                   .toHtmlEscaped();
    QStringList badges;
    if (profile.has_voice) badges << tr("Voice").toHtmlEscaped();
    if (profile.has_subs) badges << tr("Subtitles").toHtmlEscaped();
    if (!badges.isEmpty()) {
        details << badges.join(QStringLiteral(" · "));
    }
    const QStringList links = renderTeamLinks();
    if (!links.isEmpty()) {
        details << links.join(QStringLiteral("&nbsp;&nbsp;·&nbsp;&nbsp;"));
    }
    teamDetailLabel_->setText(details.join(QStringLiteral("<br>")));
    if (teamHeroCard_ != nullptr) {
        teamHeroCard_->setAccessibleName(tr("Team profile: %1").arg(profile.team.name));
    }
    QAccessibleEvent ev(teamDetailLabel_, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&ev);
}

QStringList SearchPage::renderTeamLinks() const {
    QStringList rows;
    for (auto it = activeTeamProfile_.links.cbegin(); it != activeTeamProfile_.links.cend(); ++it) {
        QString service;
        if (it.key() == QLatin1String("site") || it.key() == QLatin1String("website")) {
            service = tr("Website");
        } else if (it.key() == QLatin1String("donate")) {
            service = tr("Donate");
        } else {
            service = backend::teamLinkServiceName(it.key());
        }
        service = service.toHtmlEscaped();
        const QUrl url(it.value());
        const bool openable = url.isValid() && !url.host().isEmpty() &&
                              (url.scheme() == QLatin1String("http") ||
                               url.scheme() == QLatin1String("https"));
        if (!openable) {
            rows << QStringLiteral("%1: %2").arg(service, it.value().toHtmlEscaped());
            continue;
        }
        rows << QStringLiteral("<a style=\"color: %1; text-decoration: none;\" href=\"%2\">%3</a>")
                    .arg(theme::paletteFor(currentAppearance()).accent,
                         it.value().toHtmlEscaped(), service);
    }
    return rows;
}

void SearchPage::loadTeamAnime(int page) {
    const int seq = ++catalogueRequestSeq_;
    const backend::Team team = activeTeam_;
    const int limit = pageSize_;
    const QString inTeamSearch =
        teamFilterEdit_ != nullptr ? teamFilterEdit_->text().trimmed() : QString();
    const QString fmt = activeFormat_;
    const QString sortKey = catalogueQuery_.sort;
    const QString orderKey = catalogueQuery_.order;
    const bool useScopedSearch =
        team.id > 0 && (!inTeamSearch.isEmpty() || !fmt.isEmpty() ||
                        sortKey != QLatin1String("name") || !orderKey.isEmpty());
    statusLine_ = Status::LoadingSearch;
    renderStatus();
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run([client, team, page, limit, inTeamSearch, fmt, sortKey, orderKey, useScopedSearch] {
        if (useScopedSearch) {
            backend::AnimeQuery req;
            req.teams = {team.id};
            req.search = inTeamSearch;
            if (!fmt.isEmpty()) req.formats = {fmt};
            req.sort = sortKey;
            req.order = orderKey;
            req.page = page;
            req.limit = limit;
            return QVariant::fromValue(client->search(req));
        }
        return QVariant::fromValue(client->getTeamAnime(
            team.slug.isEmpty() ? QString::number(team.id) : team.slug, page, limit));
    }, [self, seq, page](const QVariant& value) {
        if (self.isNull() || seq != self->catalogueRequestSeq_ || !self->teamBrowseMode_) return;
        self->onResults(value.value<backend::SearchResult>(), page);
    }, [self, seq](const backend::Failure& failure) {
        if (self.isNull()) return;
        if (seq == self->catalogueRequestSeq_) self->onError(failure);
    });
}

}  // namespace mikai::app
