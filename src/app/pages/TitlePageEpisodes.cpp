#include "pages/TitlePageInternal.hpp"

namespace mikai::app {
using namespace title_page_detail;

void TitlePage::selectWatchEntry(EpisodeEntry* entry) {
    watchEntry_ = entry;
    // Only the pill grid on screen holds buttons that are still live: a page
    // that left the grid keeps pointers to buttons already queued for deletion,
    // so those are re-worded when their page is built again, never touched here.
    const int stateIndex = tabs_ != nullptr ? tabs_->currentIndex() : -1;
    if (stateIndex >= 0 && stateIndex < tabStates_.size() && tabStates_.at(stateIndex).rendered) {
        TabState& state = tabStates_[stateIndex];
        const int entryIndex = state.entries.indexOf(entry);
        if (entryIndex >= 0 && !state.displayed.contains(entry)) {
            state.page = entryIndex / state.pageSize;
            state.subpage = (entryIndex % state.pageSize) / 52;
            state.showFullRange = false;
            renderTabPage(stateIndex);
        }
        for (EpisodeEntry* candidate : state.displayed) {
            if (candidate != nullptr && candidate->button != nullptr) {
                const QSignalBlocker blocker(candidate->button);
                candidate->button->setChecked(candidate == entry);
            }
        }
    }
    updateWatchPanel();
}

TitlePage::EpisodeEntry* TitlePage::entryForNumber(int stateIndex, const QString& number) const {
    if (number.isEmpty() || stateIndex < 0 || stateIndex >= tabStates_.size()) {
        return nullptr;
    }
    for (EpisodeEntry* entry : tabStates_.at(stateIndex).entries) {
        if (entry != nullptr && entry->number == number) {
            return entry;
        }
    }
    return nullptr;
}

bool TitlePage::entryIsPlayable(const EpisodeEntry* entry) const {
    if (entry == nullptr || entry->unavailable || entry->embedUrl.isEmpty()) {
        return false;
    }
    return !qualityCache_.isNoSource(entry->embedUrl);
}

TitlePage::EpisodeEntry* TitlePage::firstUsableEntry(int stateIndex) const {
    if (stateIndex < 0 || stateIndex >= tabStates_.size()) {
        return nullptr;
    }
    const QVector<EpisodeEntry*>& entries = tabStates_.at(stateIndex).entries;
    EpisodeEntry* firstReal = nullptr;
    for (EpisodeEntry* entry : entries) {
        if (entry == nullptr) {
            continue;
        }
        if (firstReal == nullptr && !entry->unavailable) {
            firstReal = entry;
        }
        if (entryIsPlayable(entry)) {
            return entry;
        }
    }
    if (firstReal != nullptr) {
        return firstReal;
    }
    return entries.isEmpty() ? nullptr : entries.first();
}

TitlePage::EpisodeEntry* TitlePage::preferredEntry(int stateIndex) const {
    if (EpisodeEntry* entry = entryForNumber(
            stateIndex, watchEntry_ != nullptr ? watchEntry_->number : QString());
        entryIsPlayable(entry)) {
        return entry;
    }
    if (EpisodeEntry* entry = entryForNumber(stateIndex, watchHistory_.current(ref_));
        entryIsPlayable(entry)) {
        return entry;
    }
    return firstUsableEntry(stateIndex);
}

void TitlePage::applyEpisodeState(EpisodeEntry* entry) {
    if (entry == nullptr || entry->button == nullptr) {
        return;
    }
    QPushButton* button = entry->button;
    const bool playable = entryIsPlayable(entry);
    const bool current = playable && !watchHistory_.current(ref_).isEmpty() &&
                         entry->number == watchHistory_.current(ref_);
    const bool watched = playable && watchHistory_.isWatched(ref_, entry->number);
    const bool currentChanged = entry->paintedCurrent != current;
    const bool watchedChanged = entry->paintedWatched != watched;
    if (currentChanged || watchedChanged) {
        entry->paintedCurrent = current;
        entry->paintedWatched = watched;
        // Both marks are style properties of the pill (current ring, watched
        // tint); one repolish covers either, and only a pill whose state changed
        // pays for it - never a whole page of them.
        button->setProperty("current", current);
        button->setProperty("watched", watched);
        button->style()->unpolish(button);
        button->style()->polish(button);
        button->update();
    }
    if (watchedChanged && entry->watchedMark != nullptr) {
        entry->watchedMark->setVisible(watched);
    }

    const QString type = episodeType(entry);
    const QString classification = type.isEmpty() ? QString() : episodeTypeCaption(type);
    button->setAccessibleName(classification.isEmpty()
                                   ? entry->label
                                   : tr("%1, %2").arg(entry->label, classification));
    if (entry->unavailable) {
        const QString description = tr("%1 — unavailable").arg(entry->label) +
                                    (classification.isEmpty()
                                         ? QString()
                                         : QStringLiteral(" · ") + classification);
        button->setToolTip(description);
        button->setAccessibleDescription(description);
        return;
    }
    if (!playable) {
        const QString description = tr("%1 — no video for this player").arg(entry->label) +
                                    (classification.isEmpty()
                                         ? QString()
                                         : QStringLiteral(" · ") + classification);
        button->setToolTip(description);
        button->setAccessibleDescription(description);
        return;
    }
    QString description = tr("%1 — watch").arg(entry->label);
    if (current) {
        description += QStringLiteral(" · ") + tr("current episode");
    }
    if (watched) {
        description += QStringLiteral(" · ") + tr("watched episode");
    }
    if (!classification.isEmpty()) {
        description += QStringLiteral(" · ") + classification;
    }
    button->setToolTip(description);
    button->setAccessibleDescription(description);
}

void TitlePage::refreshEpisodeStates() {
    for (const TabState& state : tabStates_) {
        if (!state.rendered) {
            continue;
        }
        for (EpisodeEntry* entry : state.displayed) {
            applyEpisodeState(entry);
        }
    }
    updateWatchPanel();
}

void TitlePage::noteEpisodeStarted(const QString& number) {
    if (closed_ || number.isEmpty()) {
        return;
    }
    watchHistory_.setCurrent(ref_, number);
    const int stateIndex = tabs_ != nullptr ? tabs_->currentIndex() : -1;
    EpisodeEntry* entry = entryForNumber(stateIndex, number);
    if (entryIsPlayable(entry) && entry != watchEntry_) {
        selectWatchEntry(entry);
    }
    refreshEpisodeStates();
}

void TitlePage::setEpisodeWatched(const QString& number, bool watched) {
    if (closed_ || number.isEmpty()) {
        return;
    }
    watchHistory_.setWatched(ref_, number, watched);
    refreshEpisodeStates();
}

void TitlePage::noteEpisodeProgress(const QString& number, double position, double duration) {
    if (closed_ || number.isEmpty()) {
        return;
    }
    // setOffset stores the resume point, and marks the episode watched once the
    // position passes the end threshold - the case a stream that never reports a
    // clean end-of-file used to miss.
    const bool wasWatched = watchHistory_.isWatched(ref_, number);
    watchHistory_.setOffset(ref_, number, position, duration);
    if (!wasWatched && watchHistory_.isWatched(ref_, number)) {
        refreshEpisodeStates();
    }
}

void TitlePage::updateEpisodeLegend() {
    if (episodeLegend_ == nullptr) {
        return;
    }
    episodeLegend_->setVisible(tabs_ != nullptr && tabs_->count() > 0);
    if (legendCurrent_ != nullptr) {
        legendCurrent_->setText(tr("Current"));
    }
    if (legendWatched_ != nullptr) {
        legendWatched_->setText(tr("Watched"));
    }
    if (legendUnavailable_ != nullptr) {
        legendUnavailable_->setText(tr("Unavailable"));
    }
}

void TitlePage::updateWatchPanel() {
    if (watchPanel_ == nullptr) {
        return;
    }
    const bool hasEpisode = watchEntry_ != nullptr;
    watchPanel_->setVisible(hasEpisode);
    if (!hasEpisode) {
        watchPanel_->hide();
        watchButton_->setEnabled(false);
        if (heroActionContextHost_ != nullptr) {
            const QWidget* genres = heroActionContextHost_->findChild<QWidget*>(
                QStringLiteral("titleGenres"));
            heroActionContextHost_->setVisible(genres != nullptr && !genres->isHidden());
        }
        if (heroWatchButton_ != nullptr) {
            heroWatchButton_->setEnabled(false);
            heroWatchButton_->setText(tr("Watch"));
            heroWatchButton_->setToolTip(tr("Select an available episode to watch"));
        }
        if (watchWatchedButton_ != nullptr) {
            watchWatchedButton_->hide();
        }
        return;
    }
    watchEpisode_->setText(watchEntry_->label.isEmpty()
                               ? tr("Episode %1").arg(watchEntry_->number)
                               : watchEntry_->label);
    QStringList meta;
    if (tabs_ != nullptr) {
        const int index = tabs_->currentIndex();
        if (index >= 0 && index < tabStates_.size()) {
            meta << tabs_->tabText(index);
        }
    }
    // Only a quality a probe actually reported is written down.
    const int quality = releaseQuality_.value(activeRelease_, 0);
    if (quality > 0) {
        meta << QStringLiteral("%1p").arg(quality);
    }
    const bool playable = entryIsPlayable(watchEntry_);
    if (watchEntry_->unavailable) {
        meta << tr("Unavailable");
    } else if (!playable) {
        meta << tr("No video available");
    }
    const QString number = watchEntry_->number;
    const double offset = playable ? watchHistory_.offset(ref_, number) : 0.0;
    const double length = playable ? watchHistory_.duration(ref_, number) : 0.0;
    if (offset > 0.0) {
        meta << (length > 0.0 ? QStringLiteral("%1 / %2").arg(mplayer::shell::formatTime(offset),
                                                               mplayer::shell::formatTime(length))
                              : mplayer::shell::formatTime(offset));
    }
    watchMeta_->setText(meta.join(QStringLiteral(" · ")));
    if (heroActionContextHost_ != nullptr) {
        heroActionContextHost_->show();
    }
    if (watchProgress_ != nullptr) {
        watchProgress_->setVisible(offset > 0.0 && length > offset);
        if (length > 0.0) {
            static_cast<QProgressBar*>(watchProgress_)
                ->setValue(int(std::clamp(offset / length, 0.0, 1.0) * 1000.0));
        }
    }
    const QString actionText = offset > 0.0 ? tr("Continue") : tr("Watch");
    watchButton_->setText(actionText);
    if (heroWatchButton_ != nullptr) {
        heroWatchButton_->setText(actionText);
        heroWatchButton_->setEnabled(playable);
        heroWatchButton_->setToolTip(playable ? tr("%1 — watch").arg(watchEpisode_->text())
                                              : tr("This player has no video for this episode"));
    }
    watchButton_->setEnabled(playable);
    watchButton_->setToolTip(playable ? tr("%1 — watch").arg(watchEpisode_->text())
                                      : tr("This player has no video for this episode"));
    if (watchWatchedButton_ != nullptr) {
        const bool watched = playable && watchHistory_.isWatched(ref_, watchEntry_->number);
        watchWatchedButton_->show();
        watchWatchedButton_->setEnabled(playable);
        const QSignalBlocker blocker(watchWatchedButton_);
        watchWatchedButton_->setChecked(watched);
        watchWatchedButton_->setText(watched ? tr("Watched") : tr("Mark as watched"));
        watchWatchedButton_->setToolTip(watched ? tr("Mark this episode as unwatched")
                                                : tr("Mark this episode as watched"));
        watchWatchedButton_->setAccessibleName(watchWatchedButton_->text());
        watchWatchedButton_->setAccessibleDescription(
            watched ? tr("%1 — mark as unwatched").arg(watchEpisode_->text())
                    : tr("%1 — mark as watched").arg(watchEpisode_->text()));
    }
    // These captions change with the episode and the history, so the width is
    // measured here and not only at construction.
    refreshActionWidths();
}

QString TitlePage::episodeType(const EpisodeEntry* entry) const {
    if (entry == nullptr) {
        return {};
    }
    return entry->kind;
}

QString TitlePage::episodeTypeCaption(const QString& type) const {
    if (type == QLatin1String("mangaCanon")) return tr("Manga canon");
    if (type == QLatin1String("animeCanon")) return tr("Anime canon");
    if (type == QLatin1String("filler")) return tr("Filler");
    if (type == QLatin1String("mixed")) return tr("Mixed");
    return type;
}

void TitlePage::updateLicenseInfo(const backend::PlayerResult& player) {
    const bool playbackRestricted = detail_.licensed || player.licensed;
    if (!playbackRestricted && detail_.license.isEmpty() && player.license.isEmpty()) {
        licenseInfo_->clear();
        licenseInfo_->hide();
        return;
    }
    const QJsonObject license = !player.license.isEmpty() ? player.license : detail_.license;
    const QString rendered = licenseValueHtml(license);
    QString information = rendered.isEmpty()
        ? tr("This title is licensed, but license details were not provided by the API.")
        : tr("Licensed information: %1").arg(rendered);
    if (playbackRestricted) {
        information.prepend(tr("Playback unavailable due to licensing") + QStringLiteral("<br>"));
    }
    licenseInfo_->setText(information);
    licenseInfo_->show();
}

void TitlePage::updateEpisodeSummary() {
    if (episodeCount_ == nullptr) {
        return;
    }
    if (activeRelease_ < 0 || activeRelease_ >= releases_.size()) {
        episodeCount_->setText(QString());
        return;
    }
    const int count = backend::playableEpisodeCount(releases_.at(activeRelease_));
    episodeCount_->setText(count <= 0 ? QString()
                                      : (count == 1 ? tr("1 episode")
                                                    : tr("%1 episodes").arg(count)));
}

QString TitlePage::episodeCaption(const QString& number) {
    bool numeric = false;
    number.toInt(&numeric);
    return numeric ? tr("Episode %1").arg(number) : number;
}

QString TitlePage::scoresText(const backend::AnimeDetail& detail) {
    QStringList parts;
    if (detail.scores.mikai.has_value()) {
        QString text = QStringLiteral("Mikai ★ %1").arg(*detail.scores.mikai);
        if (detail.scores.mikai_count > 0) {
            text += QStringLiteral(" (%1)").arg(labels::compactNumber(detail.scores.mikai_count));
        }
        parts << text;
    }
    if (detail.scores.mal.has_value()) {
        QString text = QStringLiteral("MAL %1").arg(*detail.scores.mal);
        if (detail.scores.mal_count > 0) {
            text += QStringLiteral(" (%1)").arg(labels::compactNumber(detail.scores.mal_count));
        }
        parts << text;
    }
    return parts.join(QStringLiteral(" · "));
}

QString TitlePage::releaseChipLabel(const backend::Release& release) const {
    QStringList teams;
    for (const backend::Team& team : release.teams) {
        if (!team.name.isEmpty()) {
            teams << team.name;
        }
    }
    QString label = teams.isEmpty() ? labels::kind(release.kind)
                                    : teams.join(QLatin1String(", "));
    if (release.is_collab) {
        label += tr(" (collab)");
    }
    const int count = backend::playableEpisodeCount(release);
    if (count > 0) {
        label += QStringLiteral(" · %1").arg(count);
    }
    return label;
}

QString TitlePage::releaseChipToolTip(const backend::Release& release) const {
    QStringList teams;
    for (const backend::Team& team : release.teams) {
        if (!team.name.isEmpty()) {
            teams << team.name;
        }
    }
    QString label = labels::kind(release.kind);
    if (!teams.isEmpty()) {
        label += QStringLiteral(" — ") + teams.join(QLatin1String(", "));
    }
    if (release.is_collab) {
        label += tr(" (collab)");
    }
    const int count = backend::playableEpisodeCount(release);
    if (count > 0) {
        label += (count == 1 ? tr(" · 1 episode") : tr(" · %1 episodes").arg(count));
    }
    for (const backend::Team& team : release.teams) {
        if (teamReference(team).isEmpty()) {
            continue;
        }
        label += QLatin1Char('\n')
                 + tr("Right-click to browse releases from %1").arg(teamDisplayName(team));
    }
    return label;
}

QString TitlePage::releaseTrackLabel(const backend::Release& release) const {
    // Team only, no kind and no episode count: the kind heads the section in the
    // player's track menu, and a track menu is scoped to the episode on screen,
    // so "12 episodes" there would read as the length of what is about to play.
    QStringList teams;
    for (const backend::Team& team : release.teams) {
        if (!team.name.isEmpty()) {
            teams << team.name;
        }
    }
    if (!teams.isEmpty()) {
        return release.is_collab ? teams.join(QLatin1String(", ")) + tr(" (collab)")
                                 : teams.join(QLatin1String(", "));
    }
    return labels::kind(release.kind);
}

void TitlePage::setReleaseChipText(QPushButton* chip, const QString& text) {
    if (chip == nullptr) {
        return;
    }
    const int rowWidth = releaseChipsHost_ != nullptr ? releaseChipsHost_->width()
                                                       : theme::kTitlePageMaxWidth;
    int kindWidth = 0;
    for (const auto& kind : releaseKindLabels_) {
        if (!kind.second.isNull()) {
            kindWidth = qMax(kindWidth, kind.second->width());
        }
    }
    const int available = qMax(
        80, rowWidth - theme::kSpacingSmall * 4 - kindWidth - theme::kIconSize -
                theme::kTitleChipPaddingH * 2);
    chip->setMinimumWidth(0);
    chip->setMaximumWidth(available);
    chip->setAccessibleName(text);
    const QFontMetrics metrics(chip->font());
    const int textWidth = qMax(1, available - theme::kTitleChipPaddingH * 2);
    if (metrics.horizontalAdvance(text) <= textWidth) {
        chip->setText(text);
        return;
    }
    QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QString prefix;
    for (const QString& word : words) {
        const QString candidate = prefix.isEmpty() ? word : prefix + QLatin1Char(' ') + word;
        if (metrics.horizontalAdvance(candidate + QChar(0x2026)) > textWidth) {
            break;
        }
        prefix = candidate;
    }
    chip->setText(prefix.isEmpty()
                      ? metrics.elidedText(text, Qt::ElideRight, textWidth)
                      : prefix + QChar(0x2026));
}

void TitlePage::rebuildReleaseChips() {
    releaseKindLabels_.clear();
    if (providerBar_ != nullptr) providerBar_->setParent(releaseChipsHost_);
    if (providerSelectorHeading_ != nullptr) providerSelectorHeading_->setParent(releaseChipsHost_);
    for (QAbstractButton* button : releaseGroup_->buttons()) {
        releaseGroup_->removeButton(button);
    }
    const auto oldRows = releaseChipsHost_->findChildren<QWidget*>(
        QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* row : oldRows) {
        if (row == providerBar_ || row == providerSelectorHeading_) continue;
        releaseRowsLayout_->removeWidget(row);
        delete row;
    }
    releaseChips_.fill(nullptr, releases_.size());
    activeRelease_ = -1;
    releaseSelectorHeading_->setVisible(!releases_.isEmpty());
    playbackControls_->setVisible(!releases_.isEmpty());

    QVector<QString> kindOrder;
    QMap<QString, QVector<int>> releasesByKind;
    for (int index = 0; index < releases_.size(); ++index) {
        const QString& kind = releases_.at(index).kind;
        if (!releasesByKind.contains(kind)) {
            kindOrder.append(kind);
        }
        releasesByKind[kind].append(index);
    }

    QVector<QLabel*> kindLabels;
    bool providerPlaced = false;
    for (const QString& kind : kindOrder) {
        const bool divided = kind != kindOrder.last();
        auto* rowWidget = new QWidget(releaseChipsHost_);
        rowWidget->setObjectName(QStringLiteral("releaseLane"));
        rowWidget->setProperty("divided", divided);
        rowWidget->setAttribute(Qt::WA_StyledBackground);
        auto* row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, theme::kSpacingXs, 0,
                               divided ? theme::kSpacingMedium : theme::kSpacingXs);
        row->setSpacing(theme::kSpacingSmall);

        auto* kindIcon = new ReleaseKindIcon(kind == QLatin1String("sub"), rowWidget);
        row->addWidget(kindIcon, 0, Qt::AlignTop);

        auto* kindLabel = new QLabel(labels::kind(kind), rowWidget);
        kindLabel->setObjectName(QStringLiteral("releaseKind"));
        kindLabel->setFixedHeight(theme::kControlHeight - theme::kSpacingSmall);
        kindLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        // Keyed by the raw kind so a language switch re-words the row's caption
        // in place rather than rebuilding the chips and their selection.
        releaseKindLabels_.append({kind, kindLabel});
        kindLabels.append(kindLabel);
        row->addWidget(kindLabel, 0, Qt::AlignTop);
        auto* chips = new FlowLayout(theme::kSpacingSmall, theme::kSpacingXs);
        row->addLayout(chips, 1);

        for (const int index : releasesByKind.value(kind)) {
            auto* chip = new QPushButton(releaseChipLabel(releases_.at(index)), rowWidget);
            chip->setToolTip(releaseChipToolTip(releases_.at(index)));
            chip->setObjectName(QStringLiteral("releaseChip"));
            chip->setCheckable(true);
            chip->setCursor(Qt::PointingHandCursor);
            chip->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
            chip->setFixedHeight(theme::kControlHeight - theme::kSpacingSmall);
            setReleaseChipText(chip, releaseChipLabel(releases_.at(index)));
            releaseGroup_->addButton(chip, index);
            releaseChips_[index] = chip;
            chips->addWidget(chip);
            chip->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(chip, &QWidget::customContextMenuRequested, this,
                    [this, chip](const QPoint& position) {
                        releaseContextMenu(chip, chip->mapToGlobal(position));
                    });
        }
        if (kind == QLatin1String("sub") && providerBar_ != nullptr) {
            auto* divider = new QWidget(rowWidget);
            divider->setObjectName(QStringLiteral("releaseProviderDivider"));
            divider->setAttribute(Qt::WA_StyledBackground);
            divider->setFixedWidth(1);
            row->addSpacing(theme::kSpacingSmall);
            row->addWidget(divider);
            row->addSpacing(theme::kSpacingSmall);
            row->addWidget(providerSelectorHeading_, 0, Qt::AlignVCenter);
            row->addWidget(providerBar_, 0, Qt::AlignVCenter);
            providerPlaced = true;
        }
        releaseRowsLayout_->addWidget(rowWidget);
    }
    if (!providerPlaced && providerBar_ != nullptr) {
        auto* providerRow = new QWidget(releaseChipsHost_);
        providerRow->setObjectName(QStringLiteral("releaseProviderRow"));
        providerRow->setAttribute(Qt::WA_StyledBackground);
        auto* providerLayout = new QHBoxLayout(providerRow);
        providerLayout->setContentsMargins(0, theme::kSpacingSmall, 0, theme::kSpacingXs);
        providerLayout->setSpacing(theme::kSpacingSmall);
        providerLayout->addWidget(providerSelectorHeading_, 0, Qt::AlignVCenter);
        providerLayout->addWidget(providerBar_, 0, Qt::AlignVCenter);
        providerLayout->addStretch();
        releaseRowsLayout_->addWidget(providerRow);
    }

    // Add clickable team profile pills so users can jump directly from TitlePage
    // to any participating dubbing/subbing team's profile and anime catalogue.
    QSet<QString> seenTeamRefs;
    QList<QPair<QString, QString>> uniqueTeams;
    for (const backend::Release& rel : releases_) {
        for (const backend::Team& team : rel.teams) {
            const QString ref = teamReference(team);
            const QString name = teamDisplayName(team);
            if (!ref.isEmpty() && !name.isEmpty() && !seenTeamRefs.contains(ref)) {
                seenTeamRefs.insert(ref);
                uniqueTeams.append(qMakePair(ref, name));
            }
        }
    }
    if (!uniqueTeams.isEmpty()) {
        auto* teamRow = new QWidget(releaseChipsHost_);
        teamRow->setObjectName(QStringLiteral("titleTeamsRow"));
        auto* teamLayout = new QHBoxLayout(teamRow);
        teamLayout->setContentsMargins(0, theme::kSpacingXs, 0, 0);
        teamLayout->setSpacing(theme::kSpacingXs);
        auto* teamLbl = new QLabel(tr("Teams:"), teamRow);
        teamLbl->setObjectName(QStringLiteral("muted"));
        teamLayout->addWidget(teamLbl, 0, Qt::AlignVCenter);
        auto* chipsWrap = new QWidget(teamRow);
        {
            QSizePolicy cp(QSizePolicy::Expanding, QSizePolicy::Minimum);
            cp.setHeightForWidth(true);
            chipsWrap->setSizePolicy(cp);
        }
        auto* chipsFlow = new FlowLayout(theme::kSpacingXs, theme::kSpacingXs);
        chipsFlow->setContentsMargins(0, 0, 0, 0);
        chipsWrap->setLayout(chipsFlow);
        for (const auto& pair : uniqueTeams) {
            auto* btn = new QPushButton(pair.second, chipsWrap);
            btn->setObjectName(QStringLiteral("categoryChip"));
            btn->setCursor(Qt::PointingHandCursor);
            btn->setToolTip(tr("Open %1 team profile and releases").arg(pair.second));
            btn->setAccessibleName(tr("Browse team %1").arg(pair.second));
            const QString ref = pair.first;
            connect(btn, &QPushButton::clicked, this, [this, ref] { Q_EMIT teamRequested(ref); });
            chipsFlow->addWidget(btn);
        }
        teamLayout->addWidget(chipsWrap, 1);
        releaseRowsLayout_->addWidget(teamRow);
    }

    int kindLabelWidth = 0;
    for (QLabel* label : kindLabels) {
        kindLabelWidth = qMax(kindLabelWidth, label->sizeHint().width());
    }
    for (QLabel* label : kindLabels) {
        label->setFixedWidth(kindLabelWidth);
    }
    for (int index = 0; index < releaseChips_.size() && index < releases_.size(); ++index) {
        QPushButton* chip = releaseChips_.at(index);
        if (chip != nullptr) {
            QString label = releaseChipLabel(releases_.at(index));
            const int quality = releaseQuality_.value(index, 0);
            if (quality > 0) {
                label += QStringLiteral(" · %1p").arg(quality);
            }
            setReleaseChipText(chip, label);
        }
    }

    refreshReleaseChipSizing();
}

void TitlePage::releaseContextMenu(QPushButton* chip, const QPoint& globalPos) {
    const int index = chip == nullptr ? -1 : releaseGroup_->id(chip);
    if (index < 0 || index >= releases_.size()) {
        return;
    }
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("titleContextMenu"));
    for (const backend::Team& team : releases_.at(index).teams) {
        const QString teamRef = teamReference(team);
        if (teamRef.isEmpty()) {
            continue;
        }
        const QString teamName = teamDisplayName(team);
        QAction* action = menu.addAction(tr("Browse %1 releases").arg(teamName));
        action->setToolTip(tr("Browse titles from %1").arg(teamName));
        connect(action, &QAction::triggered, this, [this, teamRef] {
            Q_EMIT teamRequested(teamRef);
        });
    }
    if (menu.actions().isEmpty()) {
        return;
    }
    menu.exec(globalPos);
}

void TitlePage::fillEpisodes(int index) {
    if (index < 0 || index >= releases_.size() || index == activeRelease_) {
        return;
    }
    activeRelease_ = index;
    // Keep empty in-flight entries so new waiters join the existing request.
    for (const TabState& state : tabStates_) {
        for (EpisodeEntry* entry : state.entries) {
            if (entry == nullptr) {
                continue;
            }
            entryCallbacks_.remove(entry);
            auto waiters = fetchWaiters_.find(entry->embedUrl);
            if (waiters != fetchWaiters_.end()) {
                waiters.value().removeAll(entry);
            }
        }
    }
    // The tab handler renders a page and re-selects an episode, and both would
    // index the vector being replaced mid-teardown.
    const QSignalBlocker tabsBlocker(tabs_);
    const QSignalBlocker providerBlocker(providerBar_);
    while (providerBar_->count() > 0) providerBar_->removeTab(0);
    while (tabs_->count() > 0) {
        QWidget* page = tabs_->widget(0);
        tabs_->removeTab(0);
        page->deleteLater();
    }
    tabStates_.clear();
    watchEntry_ = nullptr;
    // The tabs are new widgets at the default height, so the last fit's answer
    // says nothing about this one.
    fittedHeight_ = 0;

    // Episodes are grouped by the site that serves them, so each tab is one
    // provider's catalogue. Tabs are strictly isolated: each holds only its own
    // episode buttons.
    QMap<QString, QVector<EpisodeEntry*>> grouped;
    for (const backend::Episode& episode : releases_.at(index).episodes) {
        const QString number = episodeNumber(episode);
        for (const backend::Source& source : episode.sources) {
            if (source.embed_url.isEmpty()) {
                continue;
            }
            auto* entry = new EpisodeEntry;
            entry->number = number;
            entry->label = episodeCaption(number);
            entry->kind = episode.kind;
            entry->embedUrl = source.embed_url;
            ownedEntries_.append(entry);
            grouped[labels::providerFor(source)].append(entry);
        }
    }

    // Special identifiers do not imply a continuous numbered catalogue.
    int knownTotal = detail_.episodes.total.value_or(0);
    if (knownTotal <= 0) {
        knownTotal = releases_.at(index).episodes_count.value_or(0);
    }
    int highestNumber = 0;
    bool numbered = knownTotal > 0 && !grouped.isEmpty();
    for (auto it = grouped.constBegin(); numbered && it != grouped.constEnd(); ++it) {
        for (const EpisodeEntry* entry : it.value()) {
            bool ok = false;
            const int value = entry->number.toInt(&ok);
            if (!ok || value <= 0) {
                numbered = false;
                break;
            }
            highestNumber = qMax(highestNumber, value);
        }
    }

    // Tabs follow the preferred source rather than the alphabet: a title that
    // ships Moon opens on it, and one without it keeps the old order.
    QStringList providerOrder;
    for (auto it = grouped.constBegin(); it != grouped.constEnd(); ++it) {
        providerOrder.append(it.key());
    }
    const QString preferredProvider = labels::preferredProvider();
    providerOrder = labels::orderedProviders(providerOrder, preferredProvider);

    for (const QString& tabProvider : providerOrder) {
        TabState state;
        state.provider = tabProvider;
        state.entries = grouped.value(tabProvider);
        state.pageSize = theme::kEpisodesPerPage;
        if (numbered) {
            QMap<int, EpisodeEntry*> byNumber;
            for (EpisodeEntry* entry : state.entries) {
                // A non-numeric number ("1x") used to collapse to 0 and
                // overwrite episode 0's slot; skip it with a warning instead.
                bool ok = false;
                const int n = entry->number.toInt(&ok);
                if (!ok) {
                    qWarning("TitlePage: skipping entry with non-numeric number '%s'",
                             qUtf8Printable(entry->number));
                    continue;
                }
                byNumber.insert(n, entry);
            }
            const int total = qMax(knownTotal, highestNumber);
            QVector<EpisodeEntry*> ordered;
            ordered.reserve(total);
            for (int number = 1; number <= total; ++number) {
                EpisodeEntry* existing = byNumber.value(number);
                if (existing != nullptr) {
                    ordered.append(existing);
                    continue;
                }
                auto* slot = new EpisodeEntry;
                slot->number = QString::number(number);
                slot->label = episodeCaption(slot->number);
                slot->unavailable = true;
                ownedEntries_.append(slot);
                ordered.append(slot);
            }
            state.entries = ordered;
        }

        auto* tabPage = new QWidget(tabs_);
        auto* tabLayout = new QVBoxLayout(tabPage);
        tabLayout->setContentsMargins(0, 0, 0, 0);
        tabLayout->setSpacing(theme::kSpacingSmall - theme::kSpacingXs);

        // The pager leads the section: page navigation sits at hand and the grid
        // cannot push it around as it grows.
        auto* pager = new RangePager(theme::kEpisodesPerPage, tabPage);
        pager->setEpisodeMode();
        state.pager = pager;
        pager->installEventFilter(this);
        const int stateIndex = tabStates_.size();
        connect(pager, &RangePager::pageRequested, this,
                [this, stateIndex](int page) { goToEpisodePage(stateIndex, page); });
        auto* navigation = new QHBoxLayout();
        navigation->setContentsMargins(0, 0, 0, 0);
        navigation->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        navigation->addWidget(pager, 1);
        auto* jump = new QLineEdit(tabPage);
        jump->setObjectName(QStringLiteral("episodeJump"));
        jump->setPlaceholderText(tr("Episode number"));
        jump->setToolTip(tr("Jump to episode"));
        jump->setFixedHeight(32);
        jump->setMaximumWidth(theme::kTitleDetailsWidth / 3);
        connect(jump, &QLineEdit::returnPressed, this, [this, stateIndex, jump] {
            if (stateIndex < 0 || stateIndex >= tabStates_.size()) return;
            TabState& current = tabStates_[stateIndex];
            const QString number = jump->text().trimmed();
            for (int i = 0; i < current.entries.size(); ++i) {
                if (current.entries.at(i)->number != number) continue;
                current.page = i / current.pageSize;
                current.subpage = (i % current.pageSize) / 52;
                current.showFullRange = false;
                current.typeFilter.clear();
                if (current.filterGroup != nullptr && !current.filterGroup->buttons().isEmpty())
                    current.filterGroup->buttons().first()->setChecked(true);
                renderTabPage(stateIndex);
                if (current.entries.at(i)->button != nullptr)
                    current.entries.at(i)->button->setFocus(Qt::OtherFocusReason);
                return;
            }
            flashStatus(tr("Episode not found"), true);
        });
        navigation->addWidget(jump);
        tabLayout->addLayout(navigation);

        state.subpageHost = new QWidget(tabPage);
        state.subpageLayout = new QHBoxLayout(state.subpageHost);
        state.subpageLayout->setContentsMargins(0, 0, 0, 0);
        state.subpageLayout->setSpacing(theme::kSpacingXs);
        tabLayout->addWidget(state.subpageHost);

        // The page scroll is the only scroller: a full page makes the title
        // taller instead of opening a nested viewport.
        auto* grid = new QWidget(tabPage);
        grid->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        state.grid = grid;
        state.gridLayout = new QGridLayout(grid);
        state.gridLayout->setContentsMargins(0, theme::kSpacingXs, 0, 0);
        state.gridLayout->setSpacing(theme::kSpacingSmall);
        grid->installEventFilter(this);
        tabLayout->addWidget(grid, 1);

        auto* filters = new QWidget(tabPage);
        auto* filterLayout = new FlowLayout(theme::kSpacingSmall, theme::kSpacingXs);
        filters->setLayout(filterLayout);
        QSizePolicy filterPolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        filterPolicy.setHeightForWidth(true);
        filters->setSizePolicy(filterPolicy);
        const QVector<QPair<QString, QString>> types{
            {QString(), tr("All")},
            {QStringLiteral("mangaCanon"), tr("Manga canon")},
            {QStringLiteral("mixed"), tr("Mixed")},
            {QStringLiteral("animeCanon"), tr("Anime canon")},
            {QStringLiteral("filler"), tr("Filler")}};
        QSet<QString> availableTypes;
        for (const EpisodeEntry* entry : state.entries) {
            if (entry == nullptr || entry->unavailable) continue;
            for (const auto& type : types) {
                if (!type.first.isEmpty() && entry->kind == type.first)
                    availableTypes.insert(type.first);
            }
        }
        filters->setVisible(!availableTypes.isEmpty());
        auto* filterGroup = new QButtonGroup(filters);
        state.filterGroup = filterGroup;
        filterGroup->setExclusive(true);
        for (const auto& type : types) {
            if (!type.first.isEmpty() && !availableTypes.contains(type.first)) continue;
            auto* filter = new QPushButton(type.second, filters);
            filter->setObjectName(QStringLiteral("episodeTypeFilter"));
            filter->setProperty("episodeType", type.first);
            if (!type.first.isEmpty()) {
                filter->setIcon(QIcon(episodeTypeDot(type.first, filter->devicePixelRatioF())));
                filter->setIconSize(QSize(10, 10));
            }
            filter->setCheckable(true);
            filter->setChecked(type.first.isEmpty());
            filterGroup->addButton(filter);
            filterLayout->addWidget(filter);
            connect(filter, &QPushButton::clicked, this, [this, stateIndex, kind = type.first] {
                tabStates_[stateIndex].typeFilter = kind;
                renderTabPage(stateIndex);
            });
        }
        tabLayout->addWidget(filters);

        const QString provider = state.provider;
        // Every entry has to know which tab owns it: the grid is built from
        // stateIndex, and the player's episode list is built from the same tab.
        // Left at -1, the pill grid read out of bounds and the playlist came
        // back empty, so the player offered no episode list at all.
        for (EpisodeEntry* entry : state.entries) {
            entry->stateIndex = stateIndex;
        }
        tabStates_.append(state);
        const QString known = labels::provider(provider);
        const QString providerName = known.isEmpty() ? capitalize(provider) : known;
        tabs_->addTab(tabPage, providerName);
        providerBar_->addTab(providerName);
    }
    providerSelectorHeading_->setVisible(tabs_->count() > 0);
    providerBar_->ensurePolished();
    providerBar_->setFixedHeight(
        qMax(theme::kControlHeight - theme::kSpacingSmall, providerBar_->sizeHint().height()));
    providerBar_->setVisible(tabs_->count() > 0);
    tabs_->setVisible(tabs_->count() > 0);

    if (tabs_->count() > 0) {
        // insertTab() already selected index 0; restore intent under the signal blocker.
        const QString wanted = labels::providerKey(
            chosenProvider_.isEmpty() ? preferredProvider : chosenProvider_);
        int index = 0;
        for (int i = 0; i < tabStates_.size(); ++i) {
            if (labels::providerKey(tabStates_.at(i).provider) == wanted) {
                index = i;
                break;
            }
        }
        tabs_->setCurrentIndex(index);
        providerBar_->setCurrentIndex(index);
        tabs_->tabBar()->hide();
    }
    // Only the visible tab gets its pills. A release can carry three providers
    // and each tab holds up to kEpisodesPerPage buttons, so building them all
    // up front cost three times the widget constructions, QSS polishes and
    // tooltips for the two thirds of them nobody ever saw. The rest are built
    // the first time they are selected.
    for (int stateIndex = 0; stateIndex < tabStates_.size(); ++stateIndex) {
        if (stateIndex == tabs_->currentIndex()) {
            renderTabPage(stateIndex);
        }
    }

    setStatusKind(tabs_->count() > 0 ? BaseStatus::Empty : BaseStatus::NoSources);
    updateEpisodeSummary();
    selectWatchEntry(preferredEntry(tabs_->currentIndex()));
    updateEpisodeLegend();
    fitEpisodeSection();
}

QPushButton* TitlePage::buildEpisodeButton(EpisodeEntry* entry, QWidget* grid) {
    // The grid comes from the tab being rendered, not from the entry: the entry
    // only ever needs its index for the player's playlist, and reading a vector
    // through an unvalidated index is how the pill grid ended up out of bounds.
    auto* button = new QPushButton(entry->number, grid);
    button->setObjectName(QStringLiteral("episodeButton"));
    button->setFixedSize(theme::kTitleEpisodeButtonWidth, theme::kTitleEpisodeButtonHeight);
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setCursor(Qt::PointingHandCursor);
    entry->label = episodeCaption(entry->number);
    entry->button = button;

    auto* mark = new QWidget(button);
    mark->setObjectName(QStringLiteral("episodeWatchedMark"));
    mark->setFixedSize(theme::kTitleWatchedMarkWidth, theme::kTitleWatchedMarkHeight);
    mark->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    mark->setAttribute(Qt::WA_StyledBackground, true);
    mark->move((theme::kTitleEpisodeButtonWidth - theme::kTitleWatchedMarkWidth) / 2,
               theme::kTitleEpisodeButtonHeight - theme::kTitleWatchedMarkHeight -
                   theme::kTitleWatchedMarkInset);
    mark->hide();
    entry->watchedMark = mark;
    entry->paintedWatched = false;
    entry->paintedCurrent = false;
    const QString type = episodeType(entry);
    if (episodeTypeColor(type).isValid()) {
        auto* badge = new QLabel(button);
        badge->setObjectName(QStringLiteral("episodeTypeBadge"));
        badge->setPixmap(episodeTypeDot(type, button->devicePixelRatioF()));
        badge->setToolTip(episodeTypeCaption(type));
        badge->setAccessibleName(badge->toolTip());
        badge->setAttribute(Qt::WA_TransparentForMouseEvents);
        badge->setFixedSize(10, 10);
        badge->move(button->width() - badge->width() - theme::kSpacingXs,
                    theme::kSpacingXs);
        entry->typeBadge = badge;
    }

    if (entry->unavailable) {
        button->setEnabled(false);
        button->setCursor(Qt::ArrowCursor);
        button->setAccessibleName(entry->label);
        applyEpisodeState(entry);
        return button;
    }

    button->setContextMenuPolicy(Qt::CustomContextMenu);
    if (qualityCache_.isNoSource(entry->embedUrl)) {
        markNoSource(entry);
    }
    connect(button, &QPushButton::clicked, this, [this, entry] {
        selectWatchEntry(entry);
        playEpisode(entry);
    });
    connect(button, &QWidget::customContextMenuRequested, this,
            [this, entry, button](const QPoint& position) {
                episodeContextMenu(entry, button->mapToGlobal(position));
            });
    applyEpisodeState(entry);
    return button;
}

void TitlePage::renderTabPage(int stateIndex) {
    if (stateIndex < 0 || stateIndex >= tabStates_.size()) {
        return;
    }
    TabState& state = tabStates_[stateIndex];
    state.rendered = true;
    for (EpisodeEntry* entry : state.displayed) {
        entry->button = nullptr;
        entry->watchedMark = nullptr;
        entry->typeBadge = nullptr;
    }
    state.displayed.clear();
    while (QLayoutItem* stale = state.gridLayout->takeAt(0)) {
        if (stale->widget() != nullptr) {
            stale->widget()->hide();
            stale->widget()->deleteLater();
        }
        delete stale;
    }

    while (QLayoutItem* item = state.subpageLayout->takeAt(0)) {
        if (item->widget() != nullptr) {
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }
    const int rangeStart = state.page * state.pageSize;
    const int rangeEnd = qMin(rangeStart + state.pageSize, state.entries.size());
    const int subpages = (rangeEnd - rangeStart + 51) / 52;
    if (subpages == 0) {
        state.subpageHost->hide();
        state.grid->updateGeometry();
        scheduleFit();
        return;
    }
    state.subpage = qBound(0, state.subpage, subpages - 1);
    for (int sub = 0; sub < subpages; ++sub) {
        const int first = rangeStart + sub * 52;
        const int last = qMin(first + 52, rangeEnd) - 1;
        auto* button = new QPushButton(
            QStringLiteral("%1 – %2").arg(state.entries.at(first)->number,
                                         state.entries.at(last)->number), state.subpageHost);
        button->setCheckable(true);
        button->setChecked(!state.showFullRange && sub == state.subpage);
        connect(button, &QPushButton::clicked, this, [this, stateIndex, sub] {
            TabState& current = tabStates_[stateIndex];
            current.subpage = sub;
            current.showFullRange = false;
            renderTabPage(stateIndex);
        });
        state.subpageLayout->addWidget(button);
    }
    auto* all = new QPushButton(tr("Show entire range"), state.subpageHost);
    all->setObjectName(QStringLiteral("episodeFullRange"));
    all->setCheckable(true);
    all->setChecked(state.showFullRange);
    connect(all, &QPushButton::clicked, this, [this, stateIndex] {
        tabStates_[stateIndex].showFullRange = true;
        renderTabPage(stateIndex);
    });
    state.subpageLayout->addWidget(all);
    state.subpageLayout->addStretch();
    state.subpageHost->setVisible(subpages > 1);
    const int start = state.showFullRange ? rangeStart : rangeStart + state.subpage * 52;
    const int end = state.showFullRange ? rangeEnd : qMin(start + 52, rangeEnd);
    for (int index = start; index < end; ++index) {
        EpisodeEntry* entry = state.entries.at(index);
        if (!state.typeFilter.isEmpty() && episodeType(entry) != state.typeFilter) continue;
        state.displayed.append(entry);
        buildEpisodeButton(entry, state.grid);
    }

    const int totalPages = qMax(1, (state.entries.size() + state.pageSize - 1) / state.pageSize);
    state.pager->setVisible(totalPages > 1);
    QStringList numbers;
    numbers.reserve(state.entries.size());
    for (const EpisodeEntry* entry : state.entries) {
        numbers << entry->number;
    }
    state.pager->setState(state.page + 1, totalPages, state.entries.size(), numbers);
    const int spacing = theme::kSpacingSmall;
    const int available =
        qMin(state.grid->width() > 0 ? state.grid->width() : tabs_->width(),
             availableEpisodeWidth());
    const int columns = qMax(
        1, (available + spacing) / (theme::kTitleEpisodeButtonWidth + spacing));
    QVector<QWidget*> buttons;
    buttons.reserve(state.displayed.size());
    for (EpisodeEntry* entry : state.displayed) {
        buttons.append(entry->button);
    }
    layoutEpisodeGrid(state.gridLayout, buttons, columns);
    state.columns = columns;
    for (EpisodeEntry* entry : state.displayed) {
        if (entry != nullptr && entry->button != nullptr) {
            const QSignalBlocker blocker(entry->button);
            entry->button->setChecked(entry == watchEntry_);
        }
    }
    state.grid->updateGeometry();
    scheduleFit();
}

void TitlePage::scheduleFit() {
    // One timer, restarted rather than a new singleShot per caller. Fitting
    // resizes the grids, which posts a Resize each, which reflows, which fits
    // again: a chain of one-shot posts kept a whole title load busy re-measuring
    // 140 pills several times over. Restarting collapses the chain to one pass.
    if (fitTimer_ != nullptr) {
        fitTimer_->start();
    }
}

void TitlePage::goToEpisodePage(int stateIndex, int page) {
    if (stateIndex < 0 || stateIndex >= tabStates_.size()) {
        return;
    }
    TabState& state = tabStates_[stateIndex];
    // Episodes are 0-based in the state and 1-based in the pager.
    const int totalPages = qMax(1, (state.entries.size() + state.pageSize - 1) / state.pageSize);
    const int index = page - 1;
    if (index >= 0 && index < totalPages && index != state.page) {
        state.page = index;
        state.subpage = 0;
        renderTabPage(stateIndex);
    }
}

int TitlePage::availableEpisodeWidth() const {
    // A grid's stale width must not prevent its parent shrinking to the viewport.
    const int viewport = overviewScroll_ != nullptr && overviewScroll_->viewport() != nullptr
                             ? overviewScroll_->viewport()->width()
                             : width();
    const bool compact = width() > 0 && width() < theme::kTitleCompactBreakpoint;
    const int inset = compact ? theme::kTitlePageCompactInset : theme::kTitlePageInset;
    const int tabsWidth = qMax(1, viewport - 2 * inset - 2 * theme::kSpacingMedium);
    if (tabs_ == nullptr) return tabsWidth;
    return static_cast<const TitlePageTabs*>(tabs_)->contentWidthForOuterWidth(tabsWidth);
}

void TitlePage::reflowGrid(int stateIndex) {
    if (stateIndex < 0 || stateIndex >= tabStates_.size()) {
        return;
    }
    TabState& state = tabStates_[stateIndex];
    // A compact pill per episode: 64px plus the gutter fits a row of 16.
    const int spacing = theme::kSpacingSmall;
    const int available =
        qMin(state.grid->width() > 0 ? state.grid->width() : tabs_->width(),
             availableEpisodeWidth());
    const int columns = qMax(
        1, (available + spacing) / (theme::kTitleEpisodeButtonWidth + spacing));
    if (columns == state.columns) {
        return;
    }
    QVector<QWidget*> buttons;
    buttons.reserve(state.displayed.size());
    for (EpisodeEntry* entry : state.displayed) {
        buttons.append(entry->button);
    }
    layoutEpisodeGrid(state.gridLayout, buttons, columns);
    state.columns = columns;
    state.grid->updateGeometry();
    scheduleFit();
}

bool TitlePage::eventFilter(QObject* watched, QEvent* event) {
    if (providerBar_ != nullptr && watched == providerBar_ && event->type() == QEvent::Wheel &&
        !static_cast<QWheelEvent*>(event)->modifiers().testFlag(Qt::AltModifier)) {
        if (overviewScroll_ != nullptr) {
            QCoreApplication::sendEvent(overviewScroll_->verticalScrollBar(), event);
        }
        return true;
    }
    if (event->type() == QEvent::Resize && tabs_ != nullptr &&
        tabs_->currentIndex() >= 0 && tabs_->currentIndex() < tabStates_.size() &&
        watched == tabStates_.at(tabs_->currentIndex()).pager) {
        scheduleFit();
    }
    if (event->type() == QEvent::MouseButtonRelease) {
        const QVariant genre = watched != nullptr ? watched->property("mikaiGenre") : QVariant();
        const QVariant tag = watched != nullptr ? watched->property("mikaiTag") : QVariant();
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (tag.isValid() && mouse->button() == Qt::LeftButton) {
            backend::AnimeQuery criteria;
            criteria.tags = {tag.toString()};
            Q_EMIT catalogueRequested(criteria);
            return true;
        }
        if (watched == detailsThumb_ && mouse->button() == Qt::LeftButton &&
            detailsThumb_->rect().contains(mouse->pos())) {
            openCoverViewer();
            return true;
        }
        if (genre.isValid() && mouse->button() == Qt::LeftButton) {
            Q_EMIT genreRequested(genre.toString());
            return true;
        }
    }
    if (overviewScroll_ != nullptr && watched == overviewScroll_->viewport() &&
        event->type() == QEvent::Resize) {
        reflowOverviewHeight();
    }
    if (watched == column_ && event->type() == QEvent::LayoutRequest) {
        scheduleOverviewReflow();
    }
    if ((watched == heading_ || watched == subtitle_) && event->type() == QEvent::Resize) {
        scheduleOverviewReflow();
    }
    if (watched == hero_ && event->type() == QEvent::Resize) {
        scheduleOverviewReflow();
    }
    if ((watched == heading_ || watched == subtitle_ || watched == detailsTitle_ ||
         watched == detailsOriginal_) && event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton &&
            static_cast<QLabel*>(watched)->rect().contains(mouse->pos())) {
            const QString displayed = static_cast<QLabel*>(watched)->text();
            if (!mikai::gui::copyText(displayed)) {
                flashStatus(tr("Failed to copy title"), true);
            } else {
                flashStatus(tr("Title copied"));
            }
            return true;
        }
    }
    if (watched == detailsThumb_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter ||
            key->key() == Qt::Key_Space) {
            openCoverViewer();
            return true;
        }
    }
    if ((watched == heading_ || watched == subtitle_ || watched == detailsTitle_ ||
         watched == detailsOriginal_) && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter ||
            key->key() == Qt::Key_Space) {
            const QString displayed = static_cast<QLabel*>(watched)->text();
            if (!mikai::gui::copyText(displayed)) {
                flashStatus(tr("Failed to copy title"), true);
            } else {
                flashStatus(tr("Title copied"));
            }
            return true;
        }
    }
    if (watched == description_ && event->type() == QEvent::Resize &&
        description_ != nullptr && description_->width() != descriptionPreviewWidth_) {
        refreshDescriptionPreview();
    }
    if (watched == hero_ && event->type() == QEvent::Resize) {
        updateBackdrop();
    }
    if (event->type() == QEvent::Resize) {
        for (int index = 0; index < tabStates_.size(); ++index) {
            if (tabStates_.at(index).grid == watched) {
                reflowGrid(index);
                break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TitlePage::escapePage() {
    if (detailsOpen_) {
        setDetailsOpen(false);
        return;
    }
    if (coverViewer_ != nullptr && coverViewer_->isVisible()) {
        coverViewer_->close();
        return;
    }
    Q_EMIT backRequested();
}

void TitlePage::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        escapePage();
        event->accept();
        return;
    }
    const bool typingInField = qobject_cast<QLineEdit*>(QApplication::focusWidget()) != nullptr;
    const Qt::KeyboardModifiers mods = event->modifiers();
    // Ctrl+G or '/' (when not typing in a text field) focuses the active episode jump box.
    if ((mods == Qt::ControlModifier && event->key() == Qt::Key_G) ||
        (!typingInField && mods == Qt::NoModifier && event->key() == Qt::Key_Slash)) {
        if (tabs_ != nullptr && tabs_->currentWidget() != nullptr) {
            if (auto* jump =
                    tabs_->currentWidget()->findChild<QLineEdit*>(QStringLiteral("episodeJump"));
                jump != nullptr && jump->isVisible()) {
                if (overviewScroll_ != nullptr) {
                    overviewScroll_->ensureWidgetVisible(jump);
                }
                jump->setFocus(Qt::ShortcutFocusReason);
                jump->selectAll();
                event->accept();
                return;
            }
        }
    }
    // Ctrl+Return / Ctrl+Enter triggers Watch / Continue when playable.
    if (mods == Qt::ControlModifier &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        if (watchButton_ != nullptr && watchButton_->isVisible() && watchButton_->isEnabled()) {
            watchButton_->click();
            event->accept();
            return;
        }
    }
    // Alt+Left / Alt+Right cycles between provider tabs when multiple providers exist.
    if (mods == Qt::AltModifier &&
        (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) &&
        providerBar_ != nullptr && providerBar_->count() > 1) {
        const int count = providerBar_->count();
        const int delta = event->key() == Qt::Key_Right ? 1 : -1;
        const int next = (providerBar_->currentIndex() + delta + count) % count;
        providerBar_->setCurrentIndex(next);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TitlePage::scheduleOverviewReflow() {
    // Coalesced: several of these fire from one layout pass, and each reflow
    // resizes the scroll widget, which is itself a content change.
    if (overviewReflowPending_) return;
    overviewReflowPending_ = true;
    QTimer::singleShot(0, this, [this] {
        overviewReflowPending_ = false;
        reflowOverviewHeight();
    });
}

void TitlePage::reflowOverviewHeight() {
    if (overviewScroll_ == nullptr || body_ == nullptr || column_ == nullptr ||
        columnLayout_ == nullptr) {
        return;
    }
    const QMargins margins = columnLayout_->contentsMargins();
    const int viewportWidth = qMax(1, overviewScroll_->viewport()->width());
    const int bodyWidth = qMax(1, qMin(theme::kTitlePageMaxWidth, viewportWidth));
    const int contentWidth = qMax(1, bodyWidth - margins.left() - margins.right());
    // Preferred heights at this width, not minimum hints: a child's minimum may
    // be larger than what it paints, and that gap was the scrollable emptiness.
    int contentHeight = columnLayout_->totalHeightForWidth(contentWidth);
    if (contentHeight < 0) contentHeight = columnLayout_->sizeHint().height();
    const int viewportHeight = qMax(1, overviewScroll_->viewport()->height());
    const int targetHeight = qMax(viewportHeight, contentHeight);
    if (body_->size() == QSize(bodyWidth, targetHeight)) {
        return;
    }
    // A short page still fills the viewport (its trailing stretch absorbs the
    // slack); a long one scrolls exactly to its content.
    body_->setFixedSize(bodyWidth, targetHeight);
}

void TitlePage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    applyResponsiveLayout();
    reflowOverviewHeight();
}

void TitlePage::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    } else if (event->type() == QEvent::FontChange ||
               event->type() == QEvent::ApplicationFontChange ||
               event->type() == QEvent::StyleChange) {
        // A new font changes every advance the groups were measured from.
        refreshActionWidths();
    }
}

bool TitlePage::event(QEvent* event) {
    // Qt consumes these two types in QWidget::event() and never forwards them
    // to changeEvent(), so the ratio refresh has to live here.
    const bool ratioChanged = event->type() == QEvent::DevicePixelRatioChange ||
                              event->type() == QEvent::ScreenChangeInternal;
    const bool result = QWidget::event(event);
    if (ratioChanged) {
        applyResponsiveLayout();
        scheduleOverviewReflow();
    }
    if (event->type() == QEvent::Show) {
        QTimer::singleShot(0, this, [this] {
            applyResponsiveLayout();
            reflowOverviewHeight();
        });
    }
    if (ratioChanged) {
        // The even-width rounding is a device-pixel decision, so a ratio change
        // measures every group again.
        refreshActionWidths();
        // Frames are painted at the ratio they will be shown at: a window that
        // moves to another screen repaints them from the pixels on hand and
        // asks for the pixels this screen can show, instead of stretching the
        // old ones onto it.
        refreshRelatedPosters();
        coverAppliedSize_ = {};
        applyCoverPixmap();
    }
    return result;
}

void TitlePage::fitEpisodeSection() {
    if (tabs_ == nullptr) {
        return;
    }
    for (int index = 0; index < tabStates_.size(); ++index) {
        if (tabStates_.at(index).grid != nullptr) {
            reflowGrid(index);
        }
    }
    const QWidget* currentPage = tabs_->currentWidget();
    if (currentPage == nullptr) {
        fittedHeight_ = 0;
        return;
    }
    // One column before the page has a width: the measured height is temporary,
    // and the layout's own Resize returns here once the width is known.
    if (currentPage->width() <= 0) {
        return;
    }
    const int frame = style()->pixelMetric(QStyle::PM_DefaultFrameWidth);
    const int tabBarHeight = tabs_->tabBar()->isHidden() ? 0 : tabs_->tabBar()->sizeHint().height();
    const int currentIndex = tabs_->currentIndex();
    if (currentPage->layout() != nullptr) {
        currentPage->layout()->activate();
    }
    // A pager whose chips wrapped needs the height its width implies; its
    // one-line size hint would clip the second row inside the fixed tab height.
    int pageHeight = qMax(currentPage->sizeHint().height(),
                          currentPage->heightForWidth(currentPage->width()));
    if (currentPage->layout() != nullptr)
        pageHeight = qMax(pageHeight, currentPage->layout()->minimumSize().height());
    if (currentIndex >= 0 && currentIndex < tabStates_.size()) {
        const TabState& state = tabStates_.at(currentIndex);
        if (state.grid != nullptr && currentPage->layout() != nullptr) {
            const QMargins margins = currentPage->layout()->contentsMargins();
            const int spacing = currentPage->layout()->spacing();
            int navHeight = 0;
            if (state.pager != nullptr && !state.pager->isHidden()) {
                navHeight = qMax(navHeight,
                                 qMax(state.pager->minimumHeight(), state.pager->sizeHint().height()));
            }
            if (auto* jump = currentPage->findChild<QLineEdit*>(QStringLiteral("episodeJump"));
                jump != nullptr && !jump->isHidden()) {
                navHeight = qMax(navHeight, qMax(jump->height(), jump->sizeHint().height()));
            }
            const int subpageHeight =
                (state.subpageHost != nullptr && !state.subpageHost->isHidden())
                    ? state.subpageHost->sizeHint().height()
                    : 0;
            const int gridHeight = state.grid->sizeHint().height();
            int filtersHeight = 0;
            if (state.filterGroup != nullptr) {
                if (auto* fw = qobject_cast<QWidget*>(state.filterGroup->parent());
                    fw != nullptr && !fw->isHidden()) {
                    filtersHeight =
                        qMax(fw->sizeHint().height(), fw->heightForWidth(currentPage->width()));
                }
            }
            int blocks = 0;
            int sum = 0;
            for (int h : {navHeight, subpageHeight, gridHeight, filtersHeight}) {
                if (h > 0) {
                    sum += h;
                    ++blocks;
                }
            }
            const int totalSpacing = blocks > 1 ? (blocks - 1) * spacing : 0;
            pageHeight = qMax(pageHeight, sum + totalSpacing + margins.top() + margins.bottom());
        }
    }
    // Size from the active page only: a taller grid from another release would
    // leave a blank panel. The page scroll owns the overflow.
    const int desired = tabBarHeight + pageHeight + 2 * frame;
    if (fittedHeight_ == desired && tabs_->height() == desired) {
        return;
    }
    fittedHeight_ = desired;
    tabs_->setFixedHeight(desired);
    reflowOverviewHeight();
    updateBackdrop();
}

bool TitlePage::confirmExplicitPlayback() {
    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, tr("Explicit content"), tr("This title is rated 18+. Start playback?"),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    return answer == QMessageBox::Yes;
}

void TitlePage::playEpisode(EpisodeEntry* entry) {
    // The warning is per title, not per episode: accepting it once is the point
    // of the preference, and the preference itself skips it entirely.
    if (!adultConfirmed_ && !currentAppearance().showAdult &&
        labels::ageRating(detail_.age_rating) == QLatin1String("18+")) {
        if (!confirmExplicitPlayback()) return;
        adultConfirmed_ = true;
    }
    // The quality fan-out is gone: one episode fetches when it is opened.
    ensureQualities(entry, [this](EpisodeEntry* loaded, const QMap<int, QString>& qualities) {
        openPlayer(loaded, qualities);
    });
}

void TitlePage::ensureQualities(
    EpisodeEntry* entry,
    const std::function<void(EpisodeEntry*, const QMap<int, QString>&)>& callback) {
    // Fetch qualities for one embed once; concurrent callers share the flight.
    if (closed_ || entry == nullptr) {
        return;
    }
    const QString embed = entry->embedUrl;
    if (embed.isEmpty() || qualityCache_.isNoSource(embed)) {
        flashStatus(tr("This player has no video for this episode"), true);
        return;
    }
    const QMap<int, QString> cached = qualityCache_.qualities(embed);
    if (!cached.isEmpty()) {
        callback(entry, cached);
        return;
    }
    const bool startFetch = !fetchWaiters_.contains(embed);
    QVector<EpisodeEntry*>& waiters = fetchWaiters_[embed];
    if (std::find(waiters.cbegin(), waiters.cend(), entry) == waiters.cend()) {
        waiters.append(entry);
    }
    entryCallbacks_[entry].append(callback);
    if (startFetch) {
        flashStatus(tr("Loading quality options…"));
        // The stream pool, not the bulk one: this probe's result is what the
        // reader is waiting on to start playback, so it should not queue behind
        // the discovery page's own fan-out.
        const QPointer<TitlePage> self(this);
        runner_.runStream(
            [embed] { return QVariant::fromValue(backend::qualityStreams(embed)); },
            [self, embed](const QVariant& result) {
                if (!self.isNull()) {
                    self->finishFetch(embed, result.value<QMap<int, QString>>(), QString());
                }
            },
            [self, embed](const backend::Failure& failure) {
                if (!self.isNull()) {
                    self->finishFetch(embed, QMap<int, QString>(), backend::failureLine(failure));
                }
            });
    }
}

void TitlePage::finishFetch(const QString& embed, const QMap<int, QString>& qualities,
                            const QString& message) {
    const QVector<EpisodeEntry*> waiters = fetchWaiters_.take(embed);
    if (closed_ || waiters.isEmpty()) {
        return;
    }
    if (!message.isEmpty()) {
        for (EpisodeEntry* entry : waiters) {
            entryCallbacks_.remove(entry);
        }
        if (message.contains(QStringLiteral("no_source"), Qt::CaseInsensitive)) {
            qualityCache_.markNoSource(embed);
            // Disable every pill for that embed, on whichever tab it lives: the
            // provider has no video, so the button is a lie wherever it is.
            for (TabState& state : tabStates_) {
                for (EpisodeEntry* entry : state.entries) {
                    if (entry->embedUrl == embed) {
                        markNoSource(entry);
                    }
                }
            }
            flashStatus(tr("This player has no video for this episode"), true);
        } else {
            flashStatus(tr("Failed to load episode qualities"), true);
        }
        return;
    }
    if (qualities.isEmpty()) {
        for (EpisodeEntry* entry : waiters) {
            entryCallbacks_.remove(entry);
        }
        flashStatus(tr("Could not find a stream for this episode"), true);
        return;
    }
    qualityCache_.remember(embed, qualities);
    for (EpisodeEntry* entry : waiters) {
        const auto callbacks = entryCallbacks_.take(entry);
        for (const auto& callback : callbacks) {
            if (callback) {
                callback(entry, qualities);
            }
        }
    }
}

void TitlePage::markNoSource(EpisodeEntry* entry) {
    if (entry == nullptr || entry->button == nullptr) {
        return;
    }
    entry->button->setEnabled(false);
    applyEpisodeState(entry);
    if (entry == watchEntry_) {
        updateWatchPanel();
    }
}

void TitlePage::episodeContextMenu(EpisodeEntry* entry, const QPoint& globalPos) {
    if (entry == nullptr) {
        return;
    }
    QMenu menu(this);
    QPointer<QMenu> copyMenu = menu.addMenu(tr("Copy to clipboard"));
    QAction* copyUrl = copyMenu->addAction(tr("Page URL"));
    copyUrl->setToolTip(tr("Copy the episode page URL"));
    connect(copyUrl, &QAction::triggered, this, [this, entry] { copyEpisode(entry); });
    const QString entryProvider =
        (entry->stateIndex >= 0 && entry->stateIndex < tabStates_.size())
            ? tabStates_.at(entry->stateIndex).provider
            : chosenProvider_;
    QAction* copyDeepLink = copyMenu->addAction(tr("Playback link"));
    copyDeepLink->setToolTip(tr("Copy a Mikai Ja Nai deep link to this episode"));
    connect(copyDeepLink, &QAction::triggered, this, [this, entry, entryProvider] {
        PlaybackLink link;
        link.ref = ref_;
        link.provider = entryProvider;
        link.episode = entry->number;
        if (activeRelease_ >= 0 && activeRelease_ < releases_.size() &&
            !releases_.at(activeRelease_).teams.isEmpty()) {
            link.teamSlug = releases_.at(activeRelease_).teams.first().slug;
        }
        const QString formatted = formatPlaybackLink(link);
        if (!formatted.isEmpty() && mikai::gui::copyText(formatted)) {
            flashStatus(tr("URL copied"));
        } else {
            flashStatus(tr("Failed to copy episode URL"), true);
        }
    });
    if (activeRelease_ >= 0 && activeRelease_ < releases_.size() &&
        !releases_.at(activeRelease_).teams.isEmpty()) {
        const QString teamSlug = releases_.at(activeRelease_).teams.first().slug;
        const QString releaseId = releases_.at(activeRelease_).id;
        const bool isFav = favouriteTeams_.isFavourite(ref_, entryProvider, teamSlug);
        QAction* favAction = menu.addAction(
            isFav ? tr("Remove favourite team") : tr("Set as favourite team"));
        connect(favAction, &QAction::triggered, this,
                [this, isFav, prov = entryProvider, teamSlug, releaseId] {
                    if (isFav) {
                        favouriteTeams_.clearFavourite(ref_);
                    } else {
                        favouriteTeams_.setFavourite(ref_, prov, teamSlug, releaseId);
                    }
                });
    }
    copyMenu->addSeparator();
    const bool noSource = qualityCache_.isNoSource(entry->embedUrl);
    QPointer<QAction> loading = copyMenu->addAction(noSource ? tr("No video available")
                                                             : tr("Loading quality options…"));
    loading->setEnabled(false);
    loading->setToolTip(noSource ? tr("This player has no video for this episode")
                                 : tr("Fetching direct video links"));

    const auto fillRows = [this, copyMenu, loading](const QMap<int, QString>& qualities) {
        if (copyMenu.isNull() || qualities.isEmpty()) {
            return;
        }
        QList<int> heights = qualities.keys();
        std::sort(heights.begin(), heights.end(), std::greater<int>());
        for (int height : heights) {
            const QString url = qualities.value(height);
            QAction* row = copyMenu->addAction(tr("%1p").arg(height));
            row->setToolTip(url);
            connect(row, &QAction::triggered, this,
                    [this, url, height] { copyStreamUrl(url, height); });
        }
        if (!loading.isNull()) {
            loading->setVisible(false);
        }
    };
    connect(copyMenu, &QMenu::aboutToShow, this,
            [this, entry, fillRows, loading, started = false]() mutable {
                if (started) {
                    return;
                }
                started = true;
                if (loading.isNull()) {
                    return;
                }
                ensureQualities(entry, [fillRows](EpisodeEntry*,
                                                  const QMap<int, QString>& qualities) {
                    fillRows(qualities);
                });
            });

    menu.addSeparator();
    const QString number = entry->number;
    const bool watched = watchHistory_.isWatched(ref_, number);
    QAction* mark = menu.addAction(tr("Mark as watched"));
    mark->setObjectName(QStringLiteral("episodeMarkWatchedAction"));
    mark->setToolTip(tr("Add this episode to watched"));
    mark->setEnabled(!watched);
    connect(mark, &QAction::triggered, this, [this, number] { setEpisodeWatched(number, true); });

    QAction* unwatch = menu.addAction(tr("Unwatch"));
    unwatch->setObjectName(QStringLiteral("episodeUnwatchAction"));
    unwatch->setToolTip(tr("Remove this episode from watched"));
    unwatch->setEnabled(watched);
    connect(unwatch, &QAction::triggered, this,
            [this, number] { setEpisodeWatched(number, false); });

    QStringList previousEpisodes;
    const int stateIndex = entry->stateIndex;
    if (stateIndex >= 0 && stateIndex < tabStates_.size()) {
        const QVector<EpisodeEntry*>& entries = tabStates_.at(stateIndex).entries;
        const auto clicked = std::find(entries.cbegin(), entries.cend(), entry);
        if (clicked != entries.cend()) {
            previousEpisodes.reserve(clicked - entries.cbegin());
            QSet<QString> seen;
            seen.reserve(clicked - entries.cbegin());
            for (auto it = entries.cbegin(); it != clicked; ++it) {
                const EpisodeEntry* candidate = *it;
                if (candidate == nullptr) {
                    continue;
                }
                if (candidate->number == number) {
                    break;
                }
                if (!candidate->number.isEmpty() && !seen.contains(candidate->number)) {
                    previousEpisodes.append(candidate->number);
                    seen.insert(candidate->number);
                }
            }
        }
    }

    QAction* watchPrevious = menu.addAction(tr("Watch all previous"));
    watchPrevious->setObjectName(QStringLiteral("episodeWatchPreviousAction"));
    watchPrevious->setEnabled(!previousEpisodes.isEmpty());
    connect(watchPrevious, &QAction::triggered, this,
            [this, previousEpisodes] {
                if (closed_ || previousEpisodes.isEmpty()) {
                    return;
                }
                watchHistory_.setEpisodesWatched(ref_, previousEpisodes, true);
                refreshEpisodeStates();
            });

    QAction* unwatchPrevious = menu.addAction(tr("Unwatch all previous"));
    unwatchPrevious->setObjectName(QStringLiteral("episodeUnwatchPreviousAction"));
    unwatchPrevious->setEnabled(!previousEpisodes.isEmpty());
    connect(unwatchPrevious, &QAction::triggered, this,
            [this, previousEpisodes] {
                if (closed_ || previousEpisodes.isEmpty()) {
                    return;
                }
                watchHistory_.setEpisodesWatched(ref_, previousEpisodes, false);
                refreshEpisodeStates();
            });

    menu.addSeparator();
    QAction* external = menu.addAction(tr("Open in external player"));
    external->setToolTip(tr("Open the best quality in mpv or VLC"));
    external->setEnabled(!qualityCache_.isNoSource(entry->embedUrl));
    connect(external, &QAction::triggered, this, [this, entry] { openExternalPlayer(entry); });
    QAction* browser = menu.addAction(tr("Open in browser"));
    browser->setToolTip(tr("Open the episode page"));
    connect(browser, &QAction::triggered, this, [this, entry] { openEmbedInBrowser(entry); });
    menu.setObjectName(QStringLiteral("episodeContextMenu"));
    menu.exec(globalPos);
}

void TitlePage::copyStreamUrl(const QString& url, int height) {
    if (!mikai::gui::copyText(url)) {
        flashStatus(tr("Failed to copy link"), true);
        return;
    }
    flashStatus(tr("Direct link copied (%1p)").arg(height));
}
void TitlePage::copyEpisode(EpisodeEntry* entry) {
    if (!mikai::gui::copyText(entry->embedUrl)) {
        flashStatus(tr("Failed to copy episode URL"), true);
        return;
    }
    flashStatus(qualityCache_.isNoSource(entry->embedUrl)
                    ? tr("URL copied (no video available for this player)")
                    : tr("URL copied"));
}

void TitlePage::openEmbedInBrowser(EpisodeEntry* entry) {
    // P0-02: embedUrl comes from the API, so it is gated on the shared
    // http/https allowlist before any OS dispatch — a file://, ms-msdt: or
    // other handler URL from a provider payload is never opened.
    if (!mikai::backend::isSafeStreamUrl(entry->embedUrl)) {
        qWarning("mikai: blocked non-http(s) embed link");
        flashStatus(tr("Link blocked"), true);
        return;
    }
    if (QDesktopServices::openUrl(QUrl(entry->embedUrl))) {
        flashStatus(tr("Opened in browser"));
    } else {
        flashStatus(tr("Failed to open in browser"), true);
    }
}

void TitlePage::openExternalPlayer(EpisodeEntry* entry) {
    ensureQualities(entry, [this](EpisodeEntry* loaded, const QMap<int, QString>& qualities) {
        openExternalStream(loaded, qualities);
    });
}

void TitlePage::openExternalStream(EpisodeEntry* entry, const QMap<int, QString>& qualities) {
    const int height = maxQuality(qualities);
    if (height <= 0) {
        flashStatus(tr("No available video quality found"), true);
        return;
    }
    QString program;
    QString name;
    for (const QString& candidate : {QStringLiteral("mpv"), QStringLiteral("vlc")}) {
        const QString found = QStandardPaths::findExecutable(candidate);
        if (!found.isEmpty()) {
            program = found;
            name = candidate;
            break;
        }
    }
    if (program.isEmpty()) {
        flashStatus(tr("External player not found — install mpv or VLC"), true);
        return;
    }
    const QString streamUrl = qualities.value(height).trimmed();
    if (!backend::isSafeStreamUrl(streamUrl)) {
        flashStatus(tr("No available video quality found"), true);
        return;
    }
    const QStringList arguments =
        name == QLatin1String("mpv")
            ? QStringList{QStringLiteral("--"), streamUrl}
            : QStringList{QStringLiteral("--play-and-exit"), QStringLiteral("--"), streamUrl};
    if (backend::startDetachedHostTool(program, arguments)) {
        flashStatus(tr("Opened in %1 · %2p").arg(name).arg(height));
    } else {
        flashStatus(tr("Failed to launch %1").arg(name), true);
    }
}

void TitlePage::probeReleaseQualities() {
    if (closed_) {
        return;
    }
    for (int index = 0; index < releases_.size(); ++index) {
        if (releaseProbed_.contains(index)) {
            continue;
        }
        const QString embed = firstEmbed(releases_.at(index));
        releaseProbed_.insert(index);
        if (embed.isEmpty()) {
            continue;
        }
        const int seq = releaseProbeSeq_;
        const QPointer<TitlePage> self(this);
        runner_.runStream(
            [embed] { return QVariant::fromValue(backend::qualityStreams(embed)); },
            [self, index, seq](const QVariant& result) {
                if (!self.isNull()) {
                    self->applyReleaseQuality(index, result.value<QMap<int, QString>>(), seq);
                }
            },
            [self, index, seq](const backend::Failure&) {
                if (!self.isNull()) {
                    self->applyReleaseQuality(index, QMap<int, QString>(), seq);
                }
            });
    }
}

int TitlePage::sessionProviderRank(const QString& provider) const {
    // A click is the viewer's answer for this title: after one, the other
    // sources only keep their payload order instead of Moon jumping back in
    // front of it. Without a click the configured preference (Moon) decides.
    if (!chosenProvider_.isEmpty()) {
        return labels::providerKey(provider) == labels::providerKey(chosenProvider_) ? 0 : 1;
    }
    return labels::preferredProviderRank(provider, labels::preferredProvider());
}

QString TitlePage::firstEmbed(const backend::Release& release) const {
    // The programmatic paths resolve a release through this one embed, so a
    // release that ships several sources has to answer with the preferred one
    // instead of whichever source the payload happened to list first.
    int bestRank = 4;
    QString best;
    for (const backend::Episode& episode : release.episodes) {
        for (const backend::Source& source : episode.sources) {
            if (source.embed_url.isEmpty()) {
                continue;
            }
            const int rank = sessionProviderRank(labels::providerFor(source));
            if (best.isEmpty() || rank < bestRank) {
                best = source.embed_url;
                bestRank = rank;
            }
        }
    }
    return best;
}

void TitlePage::refreshReleaseChipSizing() {
    const int chipHeight = qMax(theme::kControlHeight - theme::kSpacingSmall,
                                2 * theme::radiusSmall(currentAppearance()) + 2);
    for (QPushButton* chip : releaseChips_) {
        if (chip == nullptr) continue;
        chip->setFixedHeight(chipHeight);
        chip->setMinimumWidth(0);
        chip->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        chip->updateGeometry();
    }
}

void TitlePage::applyReleaseQuality(int index, const QMap<int, QString>& qualities, int seq) {
    if (closed_ || seq != releaseProbeSeq_ || index < 0 || index >= releaseChips_.size()) {
        return;
    }
    const int height = maxQuality(qualities);
    if (height <= 0) {
        return;
    }
    releaseQuality_.insert(index, height);
    QPushButton* chip = releaseChips_.at(index);
    if (chip != nullptr) {
        const QString suffix = QStringLiteral(" · %1p").arg(height);
        setReleaseChipText(chip, releaseChipLabel(releases_.at(index)) + suffix);
        chip->setToolTip(releaseChipToolTip(releases_.at(index)) + suffix);
        refreshReleaseChipSizing();
    }
    if (index == activeRelease_) {
        updateWatchPanel();
    }
}

QVector<QVariantMap> TitlePage::episodeVariants(const QString& number) const {
    // Every dub/sub track carrying this episode, across releases and providers.
    // The fields travel separately so the player can group by kind and badge the
    // quality, rather than re-parsing one joined string.
    QVector<QVariantMap> variants;
    QSet<QString> seen;
    for (int index = 0; index < releases_.size(); ++index) {
        const backend::Release& release = releases_.at(index);
        for (const backend::Episode& episode : release.episodes) {
            if (episodeNumber(episode) != number) {
                continue;
            }
            for (const backend::Source& source : episode.sources) {
                if (source.embed_url.isEmpty() || seen.contains(source.embed_url)) {
                    continue;
                }
                seen.insert(source.embed_url);
                const QString provider = labels::providerFor(source);
                const QString known = labels::provider(provider);
                const QString track = known.isEmpty() ? capitalize(provider) : known;
                const QString display = releaseTrackLabel(release);
                QVariantMap variant;
                variant.insert(QStringLiteral("kind"), release.kind);
                variant.insert(QStringLiteral("kind_label"), labels::kind(release.kind));
                variant.insert(QStringLiteral("display"), display);
                variant.insert(QStringLiteral("track"), track);
                variant.insert(QStringLiteral("label"),
                               QStringLiteral("%1 · %2").arg(display, track));
                variant.insert(QStringLiteral("embed"), source.embed_url);
                variant.insert(QStringLiteral("quality"), releaseQuality_.value(index, 0));
                // The raw key travels with the entry so the preferred source can
                // lead without re-reading the display label.
                variant.insert(QStringLiteral("provider"), labels::providerKey(provider));
                variants.append(variant);
            }
        }
    }
    // The player's source list leads with the source this page would play, so
    // picking one from it starts where the page would have; the rest keep their
    // release/provider order, and a viewer's tab choice leads over the default.
    std::stable_sort(variants.begin(), variants.end(),
                     [this](const QVariantMap& left, const QVariantMap& right) {
                         return sessionProviderRank(
                                    left.value(QStringLiteral("provider")).toString()) <
                                sessionProviderRank(
                                    right.value(QStringLiteral("provider")).toString());
                     });
    return variants;
}

QVector<QVariantMap> TitlePage::tabPlaylist(int stateIndex, const QString& number,
                                             int* currentIndex) {
    // The episodes of the release tab being watched, for the player sidebar.
    QVector<QVariantMap> playlist;
    int index = 0;
    if (stateIndex < 0 || stateIndex >= tabStates_.size()) {
        if (currentIndex != nullptr) {
            *currentIndex = 0;
        }
        return playlist;
    }
    const QVector<EpisodeEntry*>& entries = tabStates_.at(stateIndex).entries;
    for (EpisodeEntry* entry : entries) {
        if (entry == nullptr || entry->unavailable || entry->embedUrl.isEmpty()) {
            continue;
        }
        QVariantMap item;
        item.insert(QStringLiteral("label"), entry->label);
        item.insert(QStringLiteral("number"), entry->number);
        item.insert(QStringLiteral("embed"), entry->embedUrl);
        playlist.append(item);
        if (entry->number == number) {
            index = playlist.size() - 1;
        }
    }
    if (currentIndex != nullptr) {
        *currentIndex = index;
    }
    return playlist;
}



}  // namespace mikai::app
