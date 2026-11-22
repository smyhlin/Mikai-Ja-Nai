#include "pages/SearchPageInternal.hpp"

namespace mikai::app {
using namespace search_page_detail;

void SearchPage::fetchPage(int page) {
    statusLine_ = browseMode_ ? Status::LoadingBrowse : Status::LoadingSearch;
    renderStatus();
    runResultsRequest(page);
}

void SearchPage::runResultsRequest(int page) {
    if (teamBrowseMode_) { loadTeamAnime(page); return; }
    requestSeq_ += 1;
    const int seq = requestSeq_;
    // The request is put together on the GUI thread, so the worker carries the
    // finished query instead of rebuilding it from the page.
    backend::AnimeQuery request = catalogueQuery_;
    request.search = currentQuery_;
    request.page = page;
    request.limit = pageSize_;
    // Selecting a format pill scopes the request to that single format instead
    // of appending onto multi-format catalogue criteria and duplicating entries.
    if (!activeFormat_.isEmpty()) {
        request.formats = {activeFormat_};
    }
    // Every catalogue request names its own criteria. The browse calls carry
    // the home carousels' fixed criteria - currently airing, newest episode
    // first - as values of their own, so a catalogue request routed through
    // one of them answered with the home ordering and the criterion the
    // viewer chose (a sort, an order, a filter) never applied. The home
    // carousel blocks keep their own browse calls; the catalogue does not use
    // them.
    setSearchInFlight(true);
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run(
        [client, request] { return QVariant::fromValue(client->search(request)); },
        [self, seq, page](const QVariant& result) {
            if (self.isNull() || seq != self->requestSeq_) {
                return;
            }
            self->setSearchInFlight(false);
            self->onResults(result.value<backend::SearchResult>(), page);
        },
        [self, seq](const backend::Failure& failure) {
            if (self.isNull() || seq != self->requestSeq_) {
                return;
            }
            self->setSearchInFlight(false);
            self->onError(failure);
        });
}

void SearchPage::showResultsStateCard(const QString& title, const QString& detail, bool isError) {
    if (resultsStateCard_ == nullptr || resultsViewStack_ == nullptr) return;
    resultsStateTitle_->setText(title);
    resultsStateDetail_->setText(detail);
    resultsStateRetry_->setVisible(isError);
    resultsStateClearFilters_->setVisible(hasExplicitCatalogueCriteria(catalogueQuery_));
    resultsStateResetSearch_->setVisible(!currentQuery_.isEmpty() || !homeCurrent_);
    resultsViewStack_->setCurrentWidget(resultsStateHost_);
}

void SearchPage::hideResultsStateCard() {
    if (resultsViewStack_ != nullptr &&
        resultsViewStack_->currentWidget() == resultsStateHost_) {
        resultsViewStack_->setCurrentWidget(gridViewMode_ ? static_cast<QWidget*>(resultsGridScroll_)
                                                          : static_cast<QWidget*>(results_));
    }
}

void SearchPage::setSearchInFlight(bool active) {
    if (searchInFlight_ == active) return;
    searchInFlight_ = active;
    if (searchButton_ != nullptr) {
        searchButton_->setText(active ? tr("Cancel") : tr("Search"));
        searchButton_->setToolTip(active ? tr("Cancel in-flight search") : tr("Search"));
        refreshHeaderActionWidths();
    }
    if (!homeCurrent_ && resultsViewStack_ != nullptr && resultsSkeleton_ != nullptr) {
        if (active) {
            resultsViewStack_->setCurrentWidget(resultsSkeleton_);
        }
    }
}

QStringList SearchPage::loadRecentSearches() const {
    QSettings s;
    return s.value(QStringLiteral("search/recentQueries")).toStringList();
}

void SearchPage::saveRecentSearch(const QString& term) {
    const QString clean = term.trimmed();
    if (clean.isEmpty()) return;
    QStringList recent = loadRecentSearches();
    recent.removeAll(clean);
    recent.prepend(clean);
    while (recent.size() > 8) recent.removeLast();
    QSettings s;
    s.setValue(QStringLiteral("search/recentQueries"), recent);
}

void SearchPage::hideSearchSuggestions() {
    if (suggestTimer_ != nullptr) suggestTimer_->stop();
    if (suggestPopup_ != nullptr) suggestPopup_->hide();
}

void SearchPage::showSearchSuggestions(const QString& term) {
    if (suggestPopup_ == nullptr || suggestList_ == nullptr || searchField_ == nullptr) return;
    auto positionAndShow = [this]() {
        const int rows = qMin(suggestList_->count(), 7);
        if (rows <= 0) {
            suggestPopup_->hide();
            return;
        }
        const int rowH = qMax(28, suggestList_->sizeHintForRow(0));
        suggestPopup_->setFixedWidth(searchField_->width());
        suggestPopup_->setFixedHeight(rows * rowH + theme::kSpacingSmall * 2 + 4);
        suggestPopup_->move(searchField_->mapToGlobal(QPoint(0, searchField_->height() + 4)));
        suggestPopup_->show();
    };

    if (term.isEmpty()) {
        const QStringList recent = loadRecentSearches();
        suggestList_->clear();
        if (recent.isEmpty()) {
            suggestPopup_->hide();
            return;
        }
        for (const QString& q : recent) {
            auto* item = new QListWidgetItem(tr("Recent: %1").arg(q), suggestList_);
            item->setData(Qt::UserRole, QStringLiteral("query"));
            item->setData(Qt::UserRole + 1, q);
        }
        auto* clearItem = new QListWidgetItem(tr("Clear recent searches"), suggestList_);
        clearItem->setData(Qt::UserRole, QStringLiteral("clear"));
        positionAndShow();
        return;
    }

    if (term.size() < 2) {
        suggestPopup_->hide();
        return;
    }
    const int seq = ++suggestSeq_;
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run(
        [client, term] {
            backend::AnimeQuery req;
            req.search = term;
            req.page = 1;
            req.limit = 5;
            return QVariant::fromValue(client->search(req));
        },
        [self, seq, term, positionAndShow](const QVariant& result) {
            if (self.isNull() || seq != self->suggestSeq_ || self->query_ == nullptr ||
                !self->query_->hasFocus() || self->query_->text().trimmed() != term) {
                return;
            }
            const auto res = result.value<backend::SearchResult>();
            self->suggestList_->clear();
            for (const backend::AnimeSummary& item : res.items) {
                QString label = labels::titleName(item.titles);
                if (item.year.has_value() && *item.year > 0) {
                    label += QStringLiteral(" (%1)").arg(*item.year);
                }
                if (!item.format.isEmpty()) {
                    label += QStringLiteral(" · ") + item.format.toUpper();
                }
                auto* row = new QListWidgetItem(label, self->suggestList_);
                row->setData(Qt::UserRole, QStringLiteral("anime"));
                row->setData(Qt::UserRole + 1, item.ref);
                row->setData(Qt::UserRole + 2, backend::posterUrl(item));
            }
            auto* allRow = new QListWidgetItem(tr("Search all results for \"%1\"").arg(term),
                                               self->suggestList_);
            allRow->setData(Qt::UserRole, QStringLiteral("query"));
            allRow->setData(Qt::UserRole + 1, term);
            positionAndShow();
        },
        [](const backend::Failure&) {});
}

void SearchPage::search() {
    hideSearchSuggestions();
    const QString query = query_->text().trimmed();
    if (!query.isEmpty()) {
        saveRecentSearch(query);
    }
    if (query.isEmpty()) {
        // An empty text with a criterion still chosen is a catalogue query, not
        // a return to the home collection: the sort or the order the filters
        // hold would be dropped otherwise.
        if (hasExplicitCatalogueCriteria(catalogueQuery_)) applyCatalogueFilters();
        else loadOngoing();
        return;
    }
    browseMode_ = false;
    ++teamRequestSeq_;
    teamBrowseMode_ = false;
    if (teamHeroCard_ != nullptr) teamHeroCard_->hide();
    if (teamBackButton_ != nullptr) teamBackButton_->hide();
    if (teamDetailLabel_ != nullptr) teamDetailLabel_->hide();
    currentQuery_ = query;
    activeFormat_.clear();
    page_ = 1;
    pages_ = 1;
    total_ = 0;
    savedResultsScroll_ = 0;
    savedSelectedRef_.clear();
    entries_.clear();
    results_->clear();
    pager_->hide();
    updateResultsHeading();
    showSearchResults();
    fetchPage(1);
}

void SearchPage::showDiscovery() {
    placeIntro(true);
    discoveryScroll_->show();
    if (resultsViewStack_ != nullptr) resultsViewStack_->hide();
    if (resultsToolbar_ != nullptr) resultsToolbar_->hide();
    if (activeFiltersBar_ != nullptr) activeFiltersBar_->hide();
    formatBar_->hide();
    resultsHeading_->show();
    homeCurrent_ = true;
    updateHomeIndicator();
    // Thumbs that landed while the feeds were hidden were left unpainted, so
    // the switch back is the moment to catch them up.
    applyFeedPosters();
}

void SearchPage::showSearchResults() {
    placeIntro(false);
    discoveryScroll_->hide();
    if (resultsToolbar_ != nullptr) resultsToolbar_->show();
    syncResultsSortToolbar();
    homeCurrent_ = false;
    rebuildActiveFilterChips();
    if (resultsViewStack_ != nullptr) {
        resultsViewStack_->show();
        if (!searchInFlight_) {
            resultsViewStack_->setCurrentWidget(
                gridViewMode_ ? static_cast<QWidget*>(resultsGridScroll_)
                              : static_cast<QWidget*>(results_));
        }
    }
    resultsHeading_->show();
    updateHomeIndicator();
    applyThumbs();
}

void SearchPage::onResults(const backend::SearchResult& result, int requestedPage) {
    rateLabel_->refresh();
    page_ = result.page.value_or(requestedPage);
    entries_ = result.items;
    total_ = result.total.value_or(entries_.size());
    // The server strides pages at the size it actually served, which is not
    // always the one asked for, so its own count is the authority. Falling back
    // to a local count is only safe when the same size was served back.
    pages_ = result.pages.value_or(qMax(1, (total_ + pageSize_ - 1) / pageSize_));
    if (pages_ < page_) {
        pages_ = page_;
    }
    // A different query, or the same query under a filter, is a different result
    // set, so its cached counts describe nothing the bar now shows.
    if (!activeFormat_.isEmpty() || formatTotalsQuery_ != currentQuery_) {
        formatTotals_.clear();
        formatTotalsSeq_ += 1;
        formatTotalsQuery_ = currentQuery_;
    }
    statusLine_ = Status::Results;
    updateResultsHeading();
    renderStatus();
    rebuildFormatBar();
    if (!teamBrowseMode_) {
        refreshFormatTotals();
    }
    populateRows();
    if (entries_.isEmpty()) {
        const QString heading =
            !currentQuery_.isEmpty()
                ? tr("No anime matched \"%1\"").arg(currentQuery_)
                : tr("No titles match these filters");
        const QString hint =
            hasExplicitCatalogueCriteria(catalogueQuery_)
                ? tr("Try broadening your active filters or clearing one or more criteria.")
                : tr("Check the spelling or try searching by English, Ukrainian, or romaji title.");
        showResultsStateCard(heading, hint, false);
    } else if (resultsViewStack_ != nullptr) {
        resultsViewStack_->setCurrentWidget(
            gridViewMode_ ? static_cast<QWidget*>(resultsGridScroll_)
                          : static_cast<QWidget*>(results_));
    }
    updatePager();
}

void SearchPage::rebuildFormatBar() {
    static const QStringList kOrder = {QStringLiteral("tv"),    QStringLiteral("movie"),
                                       QStringLiteral("special"), QStringLiteral("ova"),
                                       QStringLiteral("ona"),   QStringLiteral("music"),
                                       QStringLiteral("other"),  QStringLiteral("unknown")};
    QHash<QString, int> counts;
    for (const backend::AnimeSummary& entry : entries_) {
        // The API's own filter values are lower case, and the server only
        // accepts those, so a row's spelling is normalised before it is offered
        // as a tab or sent back as a filter.
        const QString format =
            entry.format.isEmpty() ? QStringLiteral("unknown") : entry.format.toLower();
        counts[format] += 1;
    }
    QStringList present;
    for (const QString& format : kOrder) {
        if (counts.value(format) > 0) {
            present << format;
        }
    }
    // A filtered answer carries the format that was asked for and nothing else.
    // Hiding the bar on that single-format response would strand the reader in
    // the filter: the control that asked for it, and the All escape that undoes
    // it, would both leave the screen. The active format therefore keeps its tab
    // (counting zero when the filtered page came back empty) and All sits beside
    // it, so the bar stays on screen exactly while there is a filter to escape.
    const bool filtering = !activeFormat_.isEmpty();
    if (filtering && !present.contains(activeFormat_)) {
        present << activeFormat_;
    }
    // Signals stay blocked for the whole rebuild: removing and re-adding tabs
    // moves the current index, and an unblocked move would look like the reader
    // picking a filter and start a new request over a language switch.
    formatBar_->blockSignals(true);
    while (formatBar_->count() > 0) {
        formatBar_->removeTab(0);
    }
    if (present.size() >= 2 || filtering) {
        formatBar_->addTab(tr("All (%1)").arg(total_));
        formatBar_->setTabData(0, QStringLiteral("all"));
        for (const QString& format : present) {
            // The count for a format is the result set's own once it has been
            // asked for; until then the page's figure is the honest answer.
            const int count = formatTotals_.value(format, counts.value(format));
            formatBar_->addTab(QStringLiteral("%1 (%2)").arg(format.toUpper()).arg(count));
            formatBar_->setTabData(formatBar_->count() - 1, format);
        }
        // The request that just landed may have carried the filter, so the tab
        // it belongs to has to be the one shown rather than "All".
        int restore = 0;
        if (filtering) {
            restore = formatBar_->count() - 1;
            while (restore > 0 && formatBar_->tabData(restore).toString() != activeFormat_) {
                --restore;
            }
            if (formatBar_->tabData(restore).toString() != activeFormat_) {
                // Unreachable while the active format is offered above, but the
                // state must never claim a filter the bar cannot show.
                restore = 0;
                activeFormat_.clear();
            }
        }
        formatBar_->setCurrentIndex(restore);
        formatBar_->show();
    } else {
        formatBar_->hide();
    }
    formatBar_->blockSignals(false);
}

void SearchPage::applyFormatFilter() {
    const int index = formatBar_->currentIndex();
    if (index < 0) {
        return;
    }
    // The filter is the server's, so picking a format is a new request over the
    // whole result set, not a re-read of the single page in hand.
    activeFormat_ = formatBar_->tabData(index).toString();
    if (activeFormat_ == QLatin1String("all")) {
        activeFormat_.clear();
    }
    fetchPage(1);
}

void SearchPage::refreshFormatTotals() {
    // Avoid N+1 per-format search() requests on every search query: each extra
    // call drains the Mikai API rate-limit quota. Format tabs use honest
    // page-local counts (and the exact total when a format tab is selected).
    ++formatTotalsSeq_;
    formatTotals_.clear();
}

void SearchPage::populateRows() {
    results_->clear();
    for (const backend::AnimeSummary& entry : entries_) {
        QString label = resultRowText(entry);
        if (!currentQuery_.isEmpty() &&
            label.indexOf(currentQuery_, 0, Qt::CaseInsensitive) < 0) {
            for (const QString& alt : {entry.titles.ua, entry.titles.english, entry.titles.original}) {
                if (!alt.trimmed().isEmpty() &&
                    alt.indexOf(currentQuery_, 0, Qt::CaseInsensitive) >= 0) {
                    label += QStringLiteral(" · ") + alt.trimmed();
                    break;
                }
            }
        }
        auto* item = new QListWidgetItem(label, results_);
        item->setToolTip(label);
        QVariantMap data;
        data.insert(QStringLiteral("ref"), entry.ref);
        data.insert(QStringLiteral("poster"), backend::posterUrl(entry));
        data.insert(QStringLiteral("thumb"), backend::thumbUrl(entry));
        item->setData(Qt::UserRole, data);
        const QString thumb = backend::thumbUrl(entry);
        const auto cached = icons_.constFind(thumb);
        if (!thumb.isEmpty() && cached != icons_.constEnd()) {
            item->setIcon(QIcon(cached.value()));
        }
        // Reading the cache is not enough: nothing ever put a search row's
        // thumb in it, because this is the only place that can ask for it.
        // requestThumb is a no-op once the url is cached or in flight, so one
        // call per row is safe and a repaint costs nothing.
        requestThumb(thumb);
    }
    populateResultCards();
}

void SearchPage::populateResultCards() {
    if (resultsGridLayout_ == nullptr || resultsGridHost_ == nullptr) return;
    for (TitleCard* card : resultCards_) {
        const QString key = card->imageKey();
        if (!key.isEmpty()) {
            cardsByImageKey_[key].removeAll(card);
        }
        cardsByRef_[card->ref()].removeAll(card);
        resultsGridLayout_->removeWidget(card);
        card->deleteLater();
    }
    resultCards_.clear();

    for (const backend::AnimeSummary& entry : entries_) {
        auto* card = new TitleCard(entry, resultsGridHost_);
        if (!currentQuery_.isEmpty()) {
            card->applySearchHighlight(currentQuery_, entry.titles);
        }
        connect(card, &TitleCard::openRequested, this,
                [this](const QString& ref, const QString& poster) {
                    if (resultsGridScroll_ != nullptr &&
                        resultsGridScroll_->verticalScrollBar() != nullptr) {
                        savedResultsScroll_ = resultsGridScroll_->verticalScrollBar()->value();
                    }
                    savedSelectedRef_ = ref;
                    Q_EMIT openRequested(ref, poster);
                });
        connect(card, &TitleCard::menuRequested, this, &SearchPage::animeCardMenu);
        resultsGridLayout_->addWidget(card);
        resultCards_.append(card);
        const QString key = card->imageKey();
        if (!key.isEmpty()) {
            cardsByImageKey_[key].append(card);
            const auto cached = rounded_.constFind(key);
            if (cached != rounded_.constEnd()) {
                card->applyPixmap(cached.value());
            } else {
                requestThumb(key);
            }
        }
        cardsByRef_[card->ref()].append(card);
        const auto detailIt = cardDetails_.constFind(card->ref());
        if (detailIt != cardDetails_.constEnd()) {
            card->applyDetail(detailIt.value());
        }
    }
    resultsGridHost_->updateGeometry();
}

void SearchPage::onError(const backend::Failure& failure) {
    rateLabel_->refresh();
    reportFailure(failure);
    if (!homeCurrent_) {
        showResultsStateCard(tr("Could not load results"),
                             failure.message.isEmpty() ? tr("The server did not respond. Please try again.")
                                                       : failure.message,
                             true);
    }
    updatePager();
}

void SearchPage::reportFailure(const backend::Failure& failure) {
    // The failure itself is kept, not just its line: a language switch restates
    // the app's own wording for the code and leaves the backend's line alone.
    statusLine_ = Status::Failure;
    statusFailure_ = failure;
    renderStatus();
    QAccessibleEvent event(status_, QAccessible::Alert);
    QAccessible::updateAccessibility(&event);
}

void SearchPage::reportFailure(Notice notice) {
    statusLine_ = Status::Notice;
    statusNotice_ = notice;
    renderStatus();
    QAccessibleEvent event(status_, QAccessible::Alert);
    QAccessible::updateAccessibility(&event);
}

void SearchPage::showBlockFailure(FailureView* view, QLabel* state, const backend::Failure& failure,
                                  const QString& context) {
    state->hide();
    view->showFailure(failure, context);
}

void SearchPage::clearBlockFailure(FailureView* view, QLabel* state) {
    view->clear();
    state->show();
}

void SearchPage::updatePager() {
    const bool showPager = !homeCurrent_ && !entries_.isEmpty() &&
                           (pages_ > 1 || pageSize_ != theme::kSearchResultsPerPage);
    pager_->setVisible(showPager);
    pager_->setState(page_, pages_, total_);
}

void SearchPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!homeCurrent_ && results_ != nullptr && results_->isVisible()) {
        if (!savedSelectedRef_.isEmpty()) {
            for (int i = 0; i < results_->count(); ++i) {
                QListWidgetItem* item = results_->item(i);
                if (item != nullptr &&
                    item->data(Qt::UserRole).toMap().value(QStringLiteral("ref")).toString() ==
                        savedSelectedRef_) {
                    results_->setCurrentItem(item);
                    break;
                }
            }
        }
        if (savedResultsScroll_ > 0 && results_->verticalScrollBar() != nullptr) {
            const int targetScroll = savedResultsScroll_;
            QTimer::singleShot(0, this, [this, targetScroll] {
                if (results_ != nullptr && results_->verticalScrollBar() != nullptr) {
                    results_->verticalScrollBar()->setValue(targetScroll);
                }
            });
        }
    }
}

void SearchPage::goToPage(int page) {
    if (page >= 1 && page <= pages_ && page != page_) {
        savedResultsScroll_ = 0;
        savedSelectedRef_.clear();
        fetchPage(page);
    }
}

void SearchPage::openItem(QListWidgetItem* item) {
    if (item == nullptr) {
        return;
    }
    const QVariantMap data = item->data(Qt::UserRole).toMap();
    const QString ref = data.value(QStringLiteral("ref")).toString();
    if (!ref.isEmpty()) {
        if (results_ != nullptr && results_->verticalScrollBar() != nullptr) {
            savedResultsScroll_ = results_->verticalScrollBar()->value();
        }
        savedSelectedRef_ = ref;
        Q_EMIT openRequested(ref, data.value(QStringLiteral("poster")).toString());
    }
}

void SearchPage::resultsContextMenu(const QPoint& position) {
    QListWidgetItem* item = results_->itemAt(position);
    if (item == nullptr) {
        return;
    }
    const QVariantMap data = item->data(Qt::UserRole).toMap();
    const QString ref = data.value(QStringLiteral("ref")).toString();
    if (ref.isEmpty()) {
        return;
    }
    animeCardMenu(ref, results_->mapToGlobal(position));
}

void SearchPage::animeCardMenu(const QString& ref, const QPoint& globalPos) {
    QString titleText;
    for (const backend::AnimeSummary& entry : entries_) {
        if (entry.ref == ref) {
            titleText = labels::titleName(entry.titles);
            break;
        }
    }
    if (titleText.isEmpty()) {
        for (const QString& key : {QStringLiteral("ongoing"), QStringLiteral("popular"),
                                   kRecentKey}) {
            for (const backend::AnimeSummary& entry : carouselEntries_.value(key)) {
                if (entry.ref == ref) {
                    titleText = labels::titleName(entry.titles);
                    break;
                }
            }
            if (!titleText.isEmpty()) break;
        }
    }

    QMenu menu(this);
    QAction* open = menu.addAction(tr("Open"));
    open->setToolTip(tr("Open the title page"));
    connect(open, &QAction::triggered, this, [this, ref] { openByRef(ref); });

    QAction* browser = menu.addAction(tr("Open in browser"));
    browser->setToolTip(tr("Open the page on mikai.me"));
    connect(browser, &QAction::triggered, this, [this, ref] {
        if (!QDesktopServices::openUrl(QUrl(pageUrlFor(ref)))) {
            reportFailure(Notice::Browser);
        }
    });

    menu.addSeparator();

    if (!titleText.isEmpty()) {
        QAction* copyTitle = menu.addAction(tr("Copy title"));
        copyTitle->setToolTip(tr("Copy the anime title"));
        connect(copyTitle, &QAction::triggered, this, [this, titleText] {
            if (!mikai::gui::copyText(titleText)) reportFailure(Notice::CopyId);
        });
    }
    QAction* copyLink = menu.addAction(tr("Copy link"));
    copyLink->setToolTip(tr("Copy the page address on mikai.me"));
    connect(copyLink, &QAction::triggered, this, [this, ref] {
        if (!mikai::gui::copyText(pageUrlFor(ref))) reportFailure(Notice::CopyLink);
    });
    QAction* copyRef = menu.addAction(tr("Copy ID"));
    copyRef->setToolTip(tr("Copy the title identifier"));
    connect(copyRef, &QAction::triggered, this, [this, ref] {
        if (!mikai::gui::copyText(ref)) reportFailure(Notice::CopyId);
    });
    menu.exec(globalPos);
}

QString SearchPage::pageUrlFor(const QString& ref) const {
    // The card menus only carry a ref, but the detail response that filled the
    // card also carries the slug the public page is served at. A link without
    // it is a 404, so the cached detail is what turns the id into a page.
    const auto it = cardDetails_.constFind(ref);
    if (it == cardDetails_.constEnd()) {
        return backend::animePageUrl(ref);
    }
    return backend::animePageUrl(ref, it.value().ids.value(QStringLiteral("slug")).toString());
}

void SearchPage::openByRef(const QString& ref) {
    // The card that raised the menu knows its own poster; look it up so the
    // title page can paint its cover before the detail call lands.
    for (const QString& key : {QStringLiteral("ongoing"), QStringLiteral("popular"),
                               kRecentKey}) {
        for (const backend::AnimeSummary& entry : carouselEntries_.value(key)) {
            if (entry.ref == ref) {
                Q_EMIT openRequested(ref, backend::posterUrl(entry));
                return;
            }
        }
    }
    for (const backend::AnimeSummary& entry : entries_) {
        if (entry.ref == ref) {
            Q_EMIT openRequested(ref, backend::posterUrl(entry));
            return;
        }
    }
    Q_EMIT openRequested(ref, QString());
}

void SearchPage::requestThumb(const QString& url) {
    // One task per image. A single task looping over the URLs made sixty
    // downloads wait on each other inside one worker, which is what the home
    // page felt like.
    if (url.isEmpty() || thumbsInFlight_.contains(url)) {
        return;
    }
    // A render rebuilds its widgets, so a cached image has to paint the ones
    // standing now. Skipping here left every rebuilt card on its placeholder
    // until the next full reload of that url.
    const auto painted = rounded_.constFind(url);
    if (painted != rounded_.constEnd()) {
        for (TitleCard* card : cardsByImageKey_.value(url)) {
            card->applyPixmap(painted.value());
        }
        applyThumbs();
        applyFeedPosters();
        return;
    }
    const auto icon = icons_.constFind(url);
    if (icon != icons_.constEnd()) {
        const QPixmap ready = roundedPoster(icon.value());
        rounded_.insert(url, ready);
        for (TitleCard* card : cardsByImageKey_.value(url)) {
            card->applyPixmap(ready);
        }
        applyThumbs();
        applyFeedPosters();
        return;
    }
    if (pendingThumbs_.contains(url)) return;
    thumbsInFlight_.insert(url);
    // The client travels into the worker by value; the page is reached only
    // through a guard, so a poster that lands after the page was closed is
    // dropped rather than painted into a page that is gone. The decode target is
    // the device size of the largest box the page draws a poster into, so a
    // HiDPI label gets its own pixels rather than an upscaled 1x raster.
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    const qreal requestedDpr = devicePixelRatioF();
    // Remember the ratio the first raster is decoded at, so a later palette or
    // screen event at an unchanged ratio leaves that artwork alone instead of
    // clearing caches that were never stale.
    artworkDpr_ = requestedDpr;
    runner_.runImage(
        [client, url, requestedDpr] {
            return QVariant::fromValue(gui::decodeImage(
                client->fetchBytes(url), gui::deviceSizeFor(kThumbDecodeSize, requestedDpr)));
        },
        [self, url, requestedDpr](const QVariant& result) {
            if (self.isNull()) {
                return;
            }
            self->thumbsInFlight_.remove(url);
            auto decoded = result.value<gui::DecodedImage>();
            if (decoded.image.isNull()) {
                qWarning().noquote() << "SearchPage: thumbnail decode failed:" << decoded.error;
                return;
            }
            // A decode that started on another screen is answered again instead
            // of being stretched onto this one.
            if (!qFuzzyCompare(requestedDpr, self->devicePixelRatioF())) {
                self->requestThumb(url);
                return;
            }
            self->applyThumb(url, std::move(decoded.image), requestedDpr);
        },
        [self, url](const backend::Failure&) {
            if (self.isNull()) {
                return;
            }
            self->thumbsInFlight_.remove(url);
        });
}

QPixmap SearchPage::roundedPoster(const QPixmap& pixmap) {
    // Every cache holds one frame per poster URL, so the frame is painted for
    // the ratio the page is on: a frame baked at another ratio is either soft
    // (upscaled) or wastefully large. The radius is the one the card poster's
    // own rule declares, so the artwork's corner and the surface behind it are
    // the same curve.
    return TitleCard::rounded(pixmap, QSize(theme::kHomePosterWidth, theme::kHomePosterHeight),
                              devicePixelRatioF(), theme::radiusSmall(currentAppearance()), true);
}

void SearchPage::applyThumb(const QString& url, QImage image, qreal dpr) {
    pendingThumbs_.insert(url, {std::move(image), dpr});
    thumbTimer_->start();
}

void SearchPage::flushThumbs() {
    if (pendingThumbs_.isEmpty()) {
        return;
    }
    // QPixmap creation stays on the GUI thread, with one image published per turn.
    auto it = pendingThumbs_.begin();
    const QString key = it.key();
    const PendingThumb pending = std::move(it.value());
    pendingThumbs_.erase(it);
    if (!qFuzzyCompare(pending.dpr, devicePixelRatioF())) {
        // The decode answered for the screen the page has left. Tagging it with
        // this ratio would dress a low-resolution raster up as one for the
        // label's own pixels, so it is asked for again instead.
        requestThumb(key);
    } else if (!icons_.contains(key)) {
        QPixmap pixmap = QPixmap::fromImage(pending.image);
        if (!pixmap.isNull()) {
            // The decode landed at the device size, so the raster is tagged with
            // the ratio it was asked for: an untagged pixmap would be drawn at
            // its device size in logical units and overflow the slot.
            pixmap.setDevicePixelRatio(pending.dpr);
            icons_.insert(key, pixmap);
            // A poster URL is shared by several cards; the index turns that
            // into a lookup instead of a scan of every card on the page. The
            // rounded variant is painted once per URL, not once per card.
            const QPixmap ready = roundedPoster(pixmap);
            rounded_.insert(key, ready);
            thumbCacheOrder_.removeAll(key);
            thumbCacheOrder_.append(key);
            constexpr int kMaxCachedThumbs = 320;
            while (icons_.size() > kMaxCachedThumbs && !thumbCacheOrder_.isEmpty()) {
                const QString evict = thumbCacheOrder_.takeFirst();
                if (evict == key) continue;
                icons_.remove(evict);
                rounded_.remove(evict);
            }
            const QList<TitleCard*> cards = cardsByImageKey_.value(key);
            for (TitleCard* card : cards) {
                card->applyPixmap(ready);
            }
        }
    }
    applyThumbs();
    applyFeedPosters();
    if (!pendingThumbs_.isEmpty()) {
        thumbTimer_->start();
    }
}

void SearchPage::refreshCardFrames() {
    // Rounded frames bake the corner radius and the cover shade, so a change to
    // either invalidates every cached one; the decoded icons stay reusable.
    rounded_.clear();
    for (auto it = cardsByImageKey_.cbegin(); it != cardsByImageKey_.cend(); ++it) {
        const auto icon = icons_.constFind(it.key());
        if (icon == icons_.constEnd()) continue;
        const QPixmap ready = roundedPoster(icon.value());
        rounded_.insert(it.key(), ready);
        for (TitleCard* card : it.value()) {
            card->applyPixmap(ready);
        }
    }
    applyThumbs();
    applyFeedPosters();
}

void SearchPage::refreshArtwork() {
    const qreal dpr = devicePixelRatioF();
    if (qFuzzyCompare(dpr, artworkDpr_)) {
        // A palette or screen event that did not change the ratio leaves the
        // decoded posters valid; only the decoded-at-one-ratio state is stale.
        return;
    }
    artworkDpr_ = dpr;
    // Poster rasters are decoded at the device size and the rounded frames bake
    // the ratio into their corners, so both are dropped and asked for again.
    // The rasters on hand are still the best pixels the page has: every slot is
    // repainted from them at the new ratio first, so the download window shows
    // a smoothly resampled frame instead of an old-ratio raster that QLabel
    // would blit unfiltered. Nothing of that bridging frame is cached, so the
    // decodes below still land at the resolution this screen can show.
    const QHash<QString, QPixmap> previous = icons_;
    icons_.clear();
    rounded_.clear();
    thumbCacheOrder_.clear();
    pendingThumbs_.clear();
    const int cardRadius = theme::radiusSmall(currentAppearance());
    for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
        const QPixmap frame = TitleCard::rounded(
            it.value(), QSize(theme::kHomePosterWidth, theme::kHomePosterHeight),
            devicePixelRatioF(), cardRadius, true);
        const QList<TitleCard*> cards = cardsByImageKey_.value(it.key());
        for (TitleCard* card : cards) {
            card->applyPixmap(frame);
        }
    }
    // A feed slot repaints its own box: the thumb there is cropped, unshaded
    // and matched to the small radius its rule declares.
    for (auto it = feedPosterLabels_.begin(); it != feedPosterLabels_.end(); ++it) {
        for (FeedPosterSlot& entry : it.value()) {
            entry.shown.clear();
            if (entry.label.isNull() || entry.url.isEmpty()) continue;
            const auto source = previous.constFind(entry.url);
            if (source == previous.constEnd()) continue;
            entry.label->setPixmap(TitleCard::rounded(source.value(), entry.label->size(),
                                                      entry.label->devicePixelRatioF(),
                                                      cardRadius, false));
        }
    }
    // Every URL that paints a raster has to be asked for again, not only the
    // carousel cards: feed posters and result-row thumbnails own slots of their
    // own, and a slot whose `shown` still names the URL would keep the old
    // raster. Collect first, request after, because a request re-enters
    // applyFeedPosters().
    QStringList urls;
    for (auto it = cardsByImageKey_.cbegin(); it != cardsByImageKey_.cend(); ++it) {
        urls.append(it.key());
    }
    for (auto it = feedPosterLabels_.cbegin(); it != feedPosterLabels_.cend(); ++it) {
        for (const FeedPosterSlot& entry : it.value()) {
            if (!entry.url.isEmpty()) urls.append(entry.url);
        }
    }
    if (results_ != nullptr) {
        for (int row = 0; row < results_->count(); ++row) {
            const QVariantMap data = results_->item(row)->data(Qt::UserRole).toMap();
            const QString thumb = data.value(QStringLiteral("thumb")).toString();
            if (!thumb.isEmpty()) urls.append(thumb);
        }
    }
    urls.removeDuplicates();
    for (const QString& url : urls) {
        requestThumb(url);
    }
    applyThumbs();
    applyFeedPosters();
}

void SearchPage::applyThumbs() {
    // In browse mode the result list is empty and hidden, and a per-row map
    // copy there is the whole cost of a thumb landing.
    if (!results_->isVisible()) {
        return;
    }
    for (int row = 0; row < results_->count(); ++row) {
        QListWidgetItem* item = results_->item(row);
        const QVariantMap data = item->data(Qt::UserRole).toMap();
        const QString thumb = data.value(QStringLiteral("thumb")).toString();
        const auto cached = icons_.constFind(thumb);
        if (!thumb.isEmpty() && cached != icons_.constEnd()) {
            item->setIcon(QIcon(cached.value()));
        }
    }
}

void SearchPage::applyFeedPosters() {
    if (!discoveryScroll_->isVisible()) {
        return;
    }
    for (auto labels = feedPosterLabels_.begin(); labels != feedPosterLabels_.end(); ++labels) {
        for (FeedPosterSlot& entry : labels.value()) {
            if (entry.label.isNull()) {
                continue;
            }
            // A label already showing its thumb has nothing to gain from another
            // pass over the feed's icon table.
            if (entry.shown == entry.url) {
                continue;
            }
            const auto cached = icons_.constFind(entry.url);
            if (cached == icons_.constEnd()) {
                continue;
            }
            // The slot is small but a 1x crop of it is still soft on a HiDPI
            // screen: crop and scale in device pixels, and round it to the
            // radius its own rule declares — the stylesheet rounds the label's
            // surface, never the pixmap drawn inside it, so an unrounded thumb
            // would square off the corners. No cover shade: at 34x48 the
            // gradient would swallow the artwork it exists to protect.
            const qreal ratio = entry.label->devicePixelRatioF();
            entry.label->setPixmap(TitleCard::rounded(
                cached.value(), entry.label->size(), ratio,
                theme::radiusSmall(currentAppearance()), false));
            entry.shown = entry.url;
        }
    }
}


}  // namespace mikai::app
