#include "pages/SearchPageInternal.hpp"

namespace mikai::app {
using namespace search_page_detail;

SearchPage::SearchPage(Runner& runner, backend::MikaiClient& client, QWidget* parent)
    : QWidget(parent), runner_(runner), client_(client) {
    pageSize_ = theme::kSearchResultsPerPage;
    for (const QString& key : feeds::all()) {
        feedSeq_.insert(key, 0);
    }
    QSettings settings;
    enabledFeeds_ = feeds::enabled(settings.value(feeds::settingsKey()).toString());
    recentItems_ = readRecentTitles();

    setObjectName(QStringLiteral("searchPage"));

    auto* pageLayout = new QHBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);

    sidebar_ = new QWidget(this);
    sidebar_->setObjectName(QStringLiteral("searchSidebar"));
    sidebar_->setAttribute(Qt::WA_StyledBackground, true);
    auto* sidebarLayout = new QVBoxLayout(sidebar_);
    sidebarLayout->setContentsMargins(theme::kSearchSidebarInset, theme::kSearchSidebarInset,
                                      theme::kSearchSidebarInset, theme::kSearchSidebarInset);
    sidebarLayout->setSpacing(theme::kSpacingSmall);

    sidebarBrandLayout_ = new QHBoxLayout();
    sidebarBrandLayout_->setContentsMargins(0, 0, 0, 0);
    sidebarBrandLayout_->setSpacing(theme::kIconTextGap);

    headerLogo_ = new QLabel(sidebar_);
    headerLogo_->setObjectName(QStringLiteral("headerLogo"));
    headerLogo_->setAccessibleName(tr("Mikai Ja Nai on GitHub"));
    headerLogo_->setAccessibleDescription(tr("Open the project page on GitHub"));
    headerLogo_->setFocusPolicy(Qt::NoFocus);
    headerLogo_->setCursor(Qt::PointingHandCursor);
    headerLogo_->setToolTip(tr("Open project on GitHub"));
    headerLogo_->installEventFilter(this);
    headerLogo_->setFixedSize(theme::kSearchSidebarBrandIconSize,
                              theme::kSearchSidebarBrandIconSize);
    headerLogoSource_.load(QStringLiteral(":/icons/app/mikai-mono.png"));
    sidebarBrandLayout_->addWidget(headerLogo_, 0, Qt::AlignVCenter);
    sidebarBrandName_ = new QLabel(tr("Mikai Ja Nai"), sidebar_);
    sidebarBrandName_->setObjectName(QStringLiteral("sidebarBrandName"));
    sidebarBrandName_->setAccessibleName(tr("Mikai Ja Nai on GitHub"));
    sidebarBrandName_->setAccessibleDescription(tr("Open the project page on GitHub"));
    sidebarBrandName_->setFocusPolicy(Qt::NoFocus);
    sidebarBrandName_->setCursor(Qt::PointingHandCursor);
    sidebarBrandName_->setToolTip(tr("Open project on GitHub"));
    sidebarBrandName_->installEventFilter(this);
    {
        QFont brandFont = sidebarBrandName_->font();
        brandFont.setPixelSize(theme::kSearchSidebarBrandFontSize);
        brandFont.setWeight(QFont::DemiBold);
        sidebarBrandName_->setFont(brandFont);
    }
    sidebarBrandLayout_->addWidget(sidebarBrandName_, 1, Qt::AlignVCenter);
    sidebarLayout->addLayout(sidebarBrandLayout_);
    refreshHeaderLogo();

    const QSize sidebarGlyphBox(theme::kSearchSidebarActionIconSize + theme::kIconGlyphPad,
                                theme::kSearchSidebarActionIconSize);
    auto* navLayout = new QVBoxLayout();
    navLayout->setContentsMargins(0, 0, 0, 0);
    navLayout->setSpacing(theme::kSpacingXs);

    homeButton_ = new QToolButton(sidebar_);
    homeButton_->setObjectName(QStringLiteral("homeButton"));
    homeButton_->setText(tr("Home"));
    homeButton_->setToolTip(tr("Home"));
    homeButton_->setAccessibleName(tr("Home"));
    homeButton_->setCheckable(true);
    homeButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    homeButton_->setIconSize(sidebarGlyphBox);
    homeButton_->setFixedHeight(theme::kControlHeight);
    homeButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    theme::applyActionRole(homeButton_, theme::ActionRole::Ghost);
    connect(homeButton_, &QToolButton::clicked, this, [this] {
        // Clicking the current Home entry must not clear its selected state.
        returnHome();
        updateHomeIndicator();
    });
    navLayout->addWidget(homeButton_);

    filtersButton_ = new QToolButton(sidebar_);
    filtersButton_->setObjectName(QStringLiteral("catalogueFiltersButton"));
    filtersButton_->setText(tr("Filters"));
    filtersButton_->setToolTip(tr("Filters"));
    filtersButton_->setAccessibleName(tr("Filters"));
    filtersButton_->setCheckable(true);
    filtersButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    filtersButton_->setIconSize(sidebarGlyphBox);
    filtersButton_->setFixedHeight(theme::kControlHeight);
    filtersButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    theme::applyActionRole(filtersButton_, theme::ActionRole::Ghost);
    navLayout->addWidget(filtersButton_);

    teamsButton_ = new QToolButton(sidebar_);
    teamsButton_->setObjectName(QStringLiteral("catalogueTeamsButton"));
    teamsButton_->setText(tr("Teams…"));
    teamsButton_->setToolTip(tr("Teams…"));
    teamsButton_->setAccessibleName(tr("Teams…"));
    teamsButton_->setCheckable(true);
    teamsButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    teamsButton_->setIconSize(sidebarGlyphBox);
    teamsButton_->setFixedHeight(theme::kControlHeight);
    teamsButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    theme::applyActionRole(teamsButton_, theme::ActionRole::Ghost);
    navLayout->addWidget(teamsButton_);
    sidebarLayout->addLayout(navLayout);
    sidebarLayout->addStretch(1);

    searchContent_ = new QWidget(this);
    searchContent_->setObjectName(QStringLiteral("searchContent"));
    rootLayout_ = new QVBoxLayout(searchContent_);
    rootLayout_->setContentsMargins(theme::kSpacingLarge, theme::kSpacingLarge,
                                    theme::kSpacingLarge, theme::kSpacingLarge);
    rootLayout_->setSpacing(theme::kSpacingMedium);

    pageLayout->addWidget(sidebar_, 0);
    pageLayout->addWidget(searchContent_, 1);

    // The header is one row: the title search takes the free width, its action
    // and the passive quota pill sit beside it.
    auto* header = new QWidget(searchContent_);
    header->setObjectName(QStringLiteral("searchHeader"));
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    header->setFixedHeight(theme::kControlHeight);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(theme::kSearchHeaderSpacing);

    rateLabel_ = new RateLimitLabel(client_, header);
    rateLabel_->setObjectName(QStringLiteral("ratePill"));
    // The main page presents the shared quota snapshot as the header pill; the
    // catalogue and title page keep its inline line.
    rateLabel_->setPresentation(RateLimitLabel::Presentation::Pill);
    rateLabel_->setFixedHeight(theme::kControlHeight);
    connect(rateLabel_, &RateLimitLabel::clicked, this,
            &SearchPage::accessSettingsRequested);

    mediaButton_ = new QToolButton(sidebar_);
    mediaButton_->setObjectName(QStringLiteral("openMediaButton"));
    mediaButton_->setText(tr("Open video"));
    mediaButton_->setToolTip(tr("Open video"));
    mediaButton_->setAccessibleName(tr("Open video"));
    mediaButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mediaButton_->setIconSize(sidebarGlyphBox);
    mediaButton_->setFixedHeight(theme::kControlHeight);
    mediaButton_->setPopupMode(QToolButton::InstantPopup);
    mediaButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    theme::applyActionRole(mediaButton_, theme::ActionRole::Secondary);
    // The menu and its actions are owned by the button; the actions are kept
    // because their captions are this page's to restate.
    auto* mediaMenu = new QMenu(mediaButton_);
    mediaMenu->setObjectName(QStringLiteral("openMediaMenu"));
    openFileAction_ = mediaMenu->addAction(tr("Video file…"));
    openFileAction_->setObjectName(QStringLiteral("openVideoFile"));
    connect(openFileAction_, &QAction::triggered, this, &SearchPage::mediaFileRequested);
    openUrlAction_ = mediaMenu->addAction(tr("Video URL…"));
    openUrlAction_->setObjectName(QStringLiteral("openVideoUrl"));
    connect(openUrlAction_, &QAction::triggered, this, &SearchPage::mediaUrlRequested);
    mediaButton_->setMenu(mediaMenu);
    sidebarLayout->addWidget(mediaButton_);

    keysButton_ = new QToolButton(sidebar_);
    keysButton_->setObjectName(QStringLiteral("settingsChip"));
    keysButton_->setText(tr("Shortcuts"));
    keysButton_->setToolTip(tr("Keyboard shortcuts (?)"));
    keysButton_->setAccessibleName(tr("Keyboard shortcuts"));
    keysButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    keysButton_->setIcon(theme::keyboardIcon());
    keysButton_->setIconSize(sidebarGlyphBox);
    keysButton_->setFixedHeight(theme::kControlHeight);
    keysButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    theme::applyActionRole(keysButton_, theme::ActionRole::Secondary);
    connect(keysButton_, &QToolButton::clicked, this, [this] {
        if (keyHints_ == nullptr) {
            keyHints_ = new mplayer::shell::KeyHints(this);
        }
        keyHints_->popupNear(keysButton_->mapToGlobal(
            QPoint(keysButton_->width() / 2, keysButton_->height())));
    });
    sidebarLayout->addWidget(keysButton_);

    settingsButton_ = new QToolButton(sidebar_);
    settingsButton_->setObjectName(QStringLiteral("settingsChip"));
    settingsButton_->setText(tr("Settings"));
    settingsButton_->setToolTip(tr("Settings"));
    settingsButton_->setAccessibleName(tr("Settings"));
    settingsButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    settingsButton_->setIcon(theme::outlineGearIcon());
    settingsButton_->setIconSize(sidebarGlyphBox);
    settingsButton_->setFixedHeight(theme::kControlHeight);
    settingsButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    theme::applyActionRole(settingsButton_, theme::ActionRole::Secondary);
    connect(settingsButton_, &QToolButton::clicked, this, &SearchPage::settingsRequested);
    sidebarLayout->addWidget(settingsButton_);
    updatesButton_ = new QToolButton(sidebar_);
    updatesButton_->setObjectName(QStringLiteral("checkForUpdatesButton"));
    updatesButton_->setText(tr("Check for updates"));
    updatesButton_->setToolTip(tr("Check for updates"));
    updatesButton_->setAccessibleName(tr("Check for updates"));
    updatesButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    updatesButton_->setFixedHeight(theme::kControlHeight);
    updatesButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    theme::applyActionRole(updatesButton_, theme::ActionRole::Secondary);
    connect(updatesButton_, &QToolButton::clicked, this, &SearchPage::checkForUpdatesRequested);
    sidebarLayout->addWidget(updatesButton_);

    // The field is one framed control: a leading glyph, the line edit, and a
    // non-interactive keycap in the trailing inset. The keycap is a sibling of
    // the edit rather than an overlay, so neither it nor the clear button can
    // ever cover the other.
    searchField_ = new QWidget(header);
    searchField_->setObjectName(QStringLiteral("searchField"));
    // The frame's bed, hairline and focus ring are the sheet's: a plain widget
    // has to ask the style to paint them.
    searchField_->setAttribute(Qt::WA_StyledBackground, true);
    searchField_->installEventFilter(this);
    auto* fieldLayout = new QHBoxLayout(searchField_);
    fieldLayout->setContentsMargins(theme::kSearchFieldInset, 0,
                                    theme::kSearchFieldTrailingInset, 0);
    fieldLayout->setSpacing(theme::kIconTextGap);

    searchGlyph_ = new QLabel(searchField_);
    searchGlyph_->setObjectName(QStringLiteral("searchFieldGlyph"));
    searchGlyph_->setFixedSize(theme::kIconSize, theme::kIconSize);
    searchGlyph_->setFocusPolicy(Qt::NoFocus);
    searchGlyph_->setAttribute(Qt::WA_TransparentForMouseEvents);
    fieldLayout->addWidget(searchGlyph_, 0, Qt::AlignVCenter);

    query_ = new QLineEdit(searchField_);
    query_->setObjectName(QStringLiteral("searchQuery"));
    query_->setPlaceholderText(tr("Search by title…"));
    query_->setClearButtonEnabled(true);
    query_->installEventFilter(this);
    fieldLayout->addWidget(query_, 1);

    searchKeycap_ = new SearchKeycap(searchField_);
    searchKeycap_->setText(QStringLiteral("Ctrl+K"));
    searchKeycap_->setToolTip(tr("Press Ctrl+K or / to focus search"));
    searchKeycap_->setAccessibleName(tr("Press Ctrl+K to focus search"));
    fieldLayout->addWidget(searchKeycap_, 0, Qt::AlignVCenter);

    headerLayout->addWidget(searchField_, 1, Qt::AlignVCenter);

    searchButton_ = new QPushButton(tr("Search"), header);
    searchButton_->setObjectName(QStringLiteral("searchActionButton"));
    theme::applyActionRole(searchButton_, theme::ActionRole::Primary);
    searchButton_->setFixedHeight(theme::kControlHeight);
    searchButton_->setIconSize(QSize(theme::kIconSize + theme::kIconGlyphPad, theme::kIconSize));
    suggestTimer_ = new QTimer(this);
    suggestTimer_->setSingleShot(true);
    suggestTimer_->setInterval(320);
    connect(suggestTimer_, &QTimer::timeout, this, [this] {
        if (query_ != nullptr && query_->hasFocus()) {
            showSearchSuggestions(query_->text().trimmed());
        }
    });

    suggestPopup_ = new QFrame(this, Qt::ToolTip | Qt::FramelessWindowHint);
    suggestPopup_->setObjectName(QStringLiteral("settingsCard"));
    auto* popLayout = new QVBoxLayout(suggestPopup_);
    popLayout->setContentsMargins(theme::kSpacingXs, theme::kSpacingXs,
                                  theme::kSpacingXs, theme::kSpacingXs);
    suggestList_ = new QListWidget(suggestPopup_);
    suggestList_->setObjectName(QStringLiteral("searchSuggestionsList"));
    suggestList_->setFrameShape(QFrame::NoFrame);
    suggestList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    suggestList_->setFocusPolicy(Qt::NoFocus);
    connect(suggestList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (item == nullptr) return;
        const QString kind = item->data(Qt::UserRole).toString();
        const QString val = item->data(Qt::UserRole + 1).toString();
        hideSearchSuggestions();
        if (kind == QLatin1String("clear")) {
            QSettings s;
            s.remove(QStringLiteral("search/recentQueries"));
            return;
        }
        if (kind == QLatin1String("anime") && !val.isEmpty()) {
            Q_EMIT openRequested(val, item->data(Qt::UserRole + 2).toString());
            return;
        }
        if (!val.isEmpty() && query_ != nullptr) {
            query_->setText(val);
            search();
        }
    });
    popLayout->addWidget(suggestList_);
    suggestPopup_->hide();

    clearResetTimer_ = new QTimer(this);
    clearResetTimer_->setSingleShot(true);
    clearResetTimer_->setInterval(350);
    connect(clearResetTimer_, &QTimer::timeout, this, [this] {
        if (query_ == nullptr || !query_->text().trimmed().isEmpty() || browseMode_) {
            return;
        }
        if (hasExplicitCatalogueCriteria(catalogueQuery_)) {
            applyCatalogueFilters();
        } else {
            loadOngoing();
        }
    });

    connect(query_, &QLineEdit::returnPressed, this, [this] {
        if (clearResetTimer_ != nullptr) clearResetTimer_->stop();
        hideSearchSuggestions();
        search();
    });
    connect(searchButton_, &QPushButton::clicked, this, [this] {
        if (searchInFlight_) {
            ++requestSeq_;
            setSearchInFlight(false);
            statusLine_ = Status::BrowseHint;
            renderStatus();
            return;
        }
        if (clearResetTimer_ != nullptr) clearResetTimer_->stop();
        search();
    });
    connect(query_, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (searchKeycap_ != nullptr && searchField_ != nullptr) {
            searchKeycap_->setVisible(!query_->hasFocus() && text.isEmpty() &&
                                      searchField_->width() >= theme::kSearchKeycapMinFieldWidth);
        }
        if (!text.trimmed().isEmpty()) {
            if (clearResetTimer_ != nullptr) clearResetTimer_->stop();
            if (query_->hasFocus() && suggestTimer_ != nullptr) {
                if (text.trimmed().size() >= 2) {
                    suggestTimer_->start();
                } else {
                    hideSearchSuggestions();
                }
            }
            return;
        }
        if (query_->hasFocus()) {
            showSearchSuggestions(QString());
        }
        if (browseMode_) {
            return;
        }
        // Debounce empty-text reset so selecting all and typing a new query
        // does not fire a stray reload on the transient empty frame.
        if (clearResetTimer_ != nullptr) clearResetTimer_->start();
    });
    connect(rateLabel_, &RateLimitLabel::textChanged, this, &SearchPage::refreshHeaderActionWidths);
    headerLayout->addWidget(searchButton_, 0, Qt::AlignVCenter);
    // The row's own order: the search takes the free width, its action follows
    // it, and the passive quota pill closes the row.
    headerLayout->addWidget(rateLabel_, 0, Qt::AlignVCenter);

    rootLayout_->addWidget(header);

    connect(filtersButton_, &QToolButton::clicked, this, [this] {
        showCatalogueFilters();
        updateHomeIndicator();
    });
    connect(teamsButton_, &QToolButton::clicked, this, [this] {
        openTeamPicker();
        updateHomeIndicator();
    });
    refreshHeaderIcons();
    loadDictionaries();

    pageIntro_ = new QWidget(this);
    pageIntro_->setObjectName(QStringLiteral("pageIntro"));
    pageIntro_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto* introLayout = new QVBoxLayout(pageIntro_);
    introLayout->setContentsMargins(0, 0, 0, 0);
    introLayout->setSpacing(theme::kSpacingXs);

    resultsHeading_ = new QLabel(tr("Currently airing · sorted by newest episode"), pageIntro_);
    // The page's own heading, addressed by its own name: the discovery blocks
    // below carry `sectionHeading` too, and two captions under one name make a
    // selector that cannot tell the page's line from a carousel's.
    resultsHeading_->setObjectName(QStringLiteral("pageHeading"));
    resultsHeading_->setContentsMargins(0, 0, 0, 0);
    introLayout->addWidget(resultsHeading_);

    status_ = new QLabel(pageIntro_);
    status_->setObjectName(QStringLiteral("status"));
    status_->setContentsMargins(theme::kSpacingSmall, 0, 0, 0);
    introLayout->addWidget(status_);
    rootLayout_->addWidget(pageIntro_);

    auto* discovery = new QWidget(this);
    auto* discoveryLayout = new QVBoxLayout(discovery);
    discoveryLayout_ = discoveryLayout;
    // The right gap keeps the content clear of the dedicated scrollbar track.
    discoveryLayout->setContentsMargins(0, 0, theme::kHomeScrollContentGap, 0);
    discoveryLayout->setSpacing(theme::kSpacingMedium);
    quickCategoriesBar_ = new QWidget(discovery);
    quickCategoriesBar_->setObjectName(QStringLiteral("quickCategoriesBar"));
    {
        QSizePolicy qp(QSizePolicy::Expanding, QSizePolicy::Minimum);
        qp.setHeightForWidth(true);
        quickCategoriesBar_->setSizePolicy(qp);
    }
    auto* qcFlow = new FlowLayout(theme::kSpacingXs, theme::kSpacingXs);
    qcFlow->setContentsMargins(0, 0, 0, theme::kSpacingXs);
    quickCategoriesBar_->setLayout(qcFlow);
    auto addQuickCat = [this, qcFlow](const QString& label, std::function<void(backend::AnimeQuery&)> configure) {
        auto* btn = new QPushButton(label, quickCategoriesBar_);
        btn->setObjectName(QStringLiteral("categoryChip"));
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QPushButton::clicked, this, [this, fn = std::move(configure)] {
            catalogueQuery_ = backend::AnimeQuery{};
            activeFormat_.clear();
            if (query_ != nullptr) query_->clear();
            currentQuery_.clear();
            fn(catalogueQuery_);
            applyCatalogueFilters();
        });
        qcFlow->addWidget(btn);
    };
    addQuickCat(tr("Top Rated"), [](backend::AnimeQuery& q) {
        q.sort = QStringLiteral("score");
        q.order = QStringLiteral("desc");
    });
    addQuickCat(tr("This Season"), [](backend::AnimeQuery& q) {
        q.statuses = {QStringLiteral("ongoing")};
        q.sort = QStringLiteral("views");
        q.order = QStringLiteral("desc");
    });
    addQuickCat(tr("Movies"), [](backend::AnimeQuery& q) {
        q.formats = {QStringLiteral("movie")};
        q.sort = QStringLiteral("score");
        q.order = QStringLiteral("desc");
    });
    addQuickCat(tr("Completed"), [](backend::AnimeQuery& q) {
        q.statuses = {QStringLiteral("completed")};
        q.sort = QStringLiteral("score");
        q.order = QStringLiteral("desc");
    });
    for (const QString& genre : {QStringLiteral("Action"), QStringLiteral("Fantasy"),
                                 QStringLiteral("Romance"), QStringLiteral("Comedy"),
                                 QStringLiteral("Sci-Fi")}) {
        addQuickCat(genre, [genre](backend::AnimeQuery& q) {
            q.genres = {genre};
            q.sort = QStringLiteral("score");
            q.order = QStringLiteral("desc");
        });
    }
    auto* randomBtn = new QPushButton(tr("✦ Surprise me"), quickCategoriesBar_);
    randomBtn->setObjectName(QStringLiteral("filterChipInclude"));
    randomBtn->setCursor(Qt::PointingHandCursor);
    randomBtn->setToolTip(tr("Open a random anime matching active filters"));
    connect(randomBtn, &QPushButton::clicked, this, [this] {
        openRandomAnime(catalogueQuery_.genres, catalogueQuery_.formats);
    });
    qcFlow->addWidget(randomBtn);

    auto* moreFiltersBtn = new QPushButton(tr("More filters…"), quickCategoriesBar_);
    moreFiltersBtn->setObjectName(QStringLiteral("categoryChip"));
    moreFiltersBtn->setCursor(Qt::PointingHandCursor);
    connect(moreFiltersBtn, &QPushButton::clicked, this, &SearchPage::showCatalogueFilters);
    qcFlow->addWidget(moreFiltersBtn);
    discoveryLayout->addWidget(quickCategoriesBar_);

    CarouselSection* recent = buildCarousel(kRecentKey);
    CarouselSection* ongoing = buildCarousel(QStringLiteral("ongoing"));
    CarouselSection* popular = buildCarousel(QStringLiteral("popular"));
    FeedSection* updates = buildFeedSection(QStringLiteral("updates"));
    FeedSection* schedule = buildFeedSection(QStringLiteral("schedule"));
    discoveryLayout->addWidget(recent->section);
    discoveryLayout->addWidget(ongoing->section);
    discoveryLayout->addWidget(popular->section);
    discoveryLayout->addWidget(updates->section);
    discoveryLayout->addWidget(schedule->section);
    // Carousels and list feeds share one key space but not one widget set, so
    // each block is shown only if it is the kind of block this key names.
    for (const QString& key : feeds::all()) {
        const bool on = enabledFeeds_.contains(key);
        if (CarouselSection* carousel = carouselSection(key)) {
            carousel->section->setVisible(on);
        }
        if (FeedSection* feed = feedSection(key)) {
            feed->section->setVisible(on);
        }
    }
    discoveryLayout->addStretch();

    discoveryScroll_ = new QScrollArea(this);
    discoveryScroll_->setObjectName(QStringLiteral("discoveryScroll"));
    discoveryScroll_->setWidgetResizable(true);
    discoveryScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Show the slim themed vertical scrollbar whenever discovery overflows so
    // users have clear scroll-position awareness and drag-scrolling support.
    discoveryScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    if (QScrollBar* vbar = discoveryScroll_->verticalScrollBar()) {
        vbar->setSingleStep(48);
    }
    discoveryScroll_->viewport()->installEventFilter(this);
    discoveryScroll_->setFrameShape(QFrame::NoFrame);
    discoveryScroll_->setWidget(discovery);
    pageGlow_ = new PageGlow(this);
    rootLayout_->addWidget(discoveryScroll_, 1);
    placeIntro(true);

    activeFiltersBar_ = new QWidget(this);
    activeFiltersBar_->setObjectName(QStringLiteral("activeFiltersBar"));
    {
        QSizePolicy afp(QSizePolicy::Expanding, QSizePolicy::Minimum);
        afp.setHeightForWidth(true);
        activeFiltersBar_->setSizePolicy(afp);
    }
    activeFiltersLayout_ = new FlowLayout(theme::kSpacingXs, theme::kSpacingXs);
    activeFiltersLayout_->setContentsMargins(0, 0, 0, 0);
    activeFiltersBar_->setLayout(activeFiltersLayout_);
    activeFiltersBar_->hide();
    rootLayout_->addWidget(activeFiltersBar_, 0);

    resultsToolbar_ = new QWidget(this);
    resultsToolbar_->setObjectName(QStringLiteral("resultsToolbar"));
    resultsToolbar_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    resultsToolbar_->setFixedHeight(theme::kControlHeight + 4);
    auto* toolbarLayout = new QHBoxLayout(resultsToolbar_);
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(theme::kSpacingSmall);

    toolbarLayout->addStretch(1);

    resultsSortBox_ = new QComboBox(resultsToolbar_);
    resultsSortBox_->setObjectName(QStringLiteral("resultsSortBox"));
    resultsSortBox_->setMinimumWidth(148);
    resultsSortBox_->setMaximumWidth(196);
    resultsSortBox_->setFixedHeight(theme::kControlHeight);
    resultsSortBox_->setToolTip(tr("Sort results"));
    resultsSortBox_->setAccessibleName(tr("Sort results"));
    resultsSortBox_->addItem(tr("Name"), QStringLiteral("name"));
    resultsSortBox_->addItem(tr("Rating"), QStringLiteral("score"));
    resultsSortBox_->addItem(tr("Release year"), QStringLiteral("year"));
    resultsSortBox_->addItem(tr("Views"), QStringLiteral("views"));
    resultsSortBox_->addItem(tr("Recently updated"), QStringLiteral("updated"));
    connect(resultsSortBox_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (syncingSortToolbar_ || index < 0) return;
        const QString sortKey = resultsSortBox_->itemData(index).toString();
        if (catalogueQuery_.sort != sortKey) {
            catalogueQuery_.sort = sortKey;
            if (catalogueQuery_.order.isEmpty() && sortKey != QLatin1String("name")) {
                catalogueQuery_.order = QStringLiteral("desc");
            }
            syncResultsSortToolbar();
            applyCatalogueFilters();
        }
    });
    toolbarLayout->addWidget(resultsSortBox_, 0, Qt::AlignVCenter);

    resultsOrderButton_ = new QToolButton(resultsToolbar_);
    resultsOrderButton_->setObjectName(QStringLiteral("resultsOrderButton"));
    resultsOrderButton_->setMinimumWidth(78);
    resultsOrderButton_->setFixedHeight(theme::kControlHeight);
    theme::applyActionRole(resultsOrderButton_, theme::ActionRole::Secondary);
    connect(resultsOrderButton_, &QToolButton::clicked, this, [this] {
        catalogueQuery_.order =
            catalogueQuery_.order == QLatin1String("asc") ? QStringLiteral("desc")
                                                          : QStringLiteral("asc");
        syncResultsSortToolbar();
        applyCatalogueFilters();
    });
    toolbarLayout->addWidget(resultsOrderButton_, 0, Qt::AlignVCenter);

    {
        QSettings settings;
        gridViewMode_ = settings.value(QString::fromUtf8(kSearchViewMode), QStringLiteral("grid"))
                            .toString() != QLatin1String("list");
    }

    gridViewButton_ = new QToolButton(resultsToolbar_);
    gridViewButton_->setObjectName(QStringLiteral("resultsGridButton"));
    gridViewButton_->setText(tr("Grid"));
    gridViewButton_->setToolTip(tr("Card grid view"));
    gridViewButton_->setAccessibleName(tr("Card grid view"));
    gridViewButton_->setCheckable(true);
    gridViewButton_->setMinimumWidth(62);
    gridViewButton_->setFixedHeight(theme::kControlHeight);
    theme::applyActionRole(gridViewButton_, theme::ActionRole::Secondary);
    connect(gridViewButton_, &QToolButton::clicked, this, [this] { setResultsGridMode(true); });
    toolbarLayout->addWidget(gridViewButton_, 0, Qt::AlignVCenter);

    listViewButton_ = new QToolButton(resultsToolbar_);
    listViewButton_->setObjectName(QStringLiteral("resultsListButton"));
    listViewButton_->setText(tr("List"));
    listViewButton_->setToolTip(tr("Compact list view"));
    listViewButton_->setAccessibleName(tr("Compact list view"));
    listViewButton_->setCheckable(true);
    listViewButton_->setMinimumWidth(62);
    listViewButton_->setFixedHeight(theme::kControlHeight);
    theme::applyActionRole(listViewButton_, theme::ActionRole::Secondary);
    connect(listViewButton_, &QToolButton::clicked, this, [this] { setResultsGridMode(false); });
    toolbarLayout->addWidget(listViewButton_, 0, Qt::AlignVCenter);

    gridViewButton_->setChecked(gridViewMode_);
    listViewButton_->setChecked(!gridViewMode_);
    resultsToolbar_->hide();
    rootLayout_->addWidget(resultsToolbar_, 0);
    syncResultsSortToolbar();

    // Dedicated full-width format pill row so format tabs never clip or collide
    // with the sort and view-mode controls.
    formatBar_ = new QTabBar(this);
    formatBar_->setObjectName(QStringLiteral("formatPillBar"));
    formatBar_->setExpanding(false);
    formatBar_->setDrawBase(false);
    formatBar_->hide();
    connect(formatBar_, &QTabBar::currentChanged, this, [this](int) { applyFormatFilter(); });
    rootLayout_->addWidget(formatBar_, 0);

    // Single stacked container for Grid, List, Skeleton, and Empty/Error states
    // so the main content area always owns vertical stretch=1 and headers never expand.
    resultsViewStack_ = new QStackedWidget(this);
    resultsViewStack_->setObjectName(QStringLiteral("resultsViewStack"));

    resultsGridHost_ = new FlowScrollHost(resultsViewStack_);
    resultsGridHost_->setObjectName(QStringLiteral("searchResultsGrid"));
    resultsGridLayout_ = new FlowLayout(theme::kSpacingMedium, theme::kSpacingMedium);
    resultsGridLayout_->setContentsMargins(0, theme::kSpacingXs, theme::kSpacingSmall,
                                           theme::kSpacingSmall);
    resultsGridHost_->setLayout(resultsGridLayout_);

    resultsGridScroll_ = new QScrollArea(resultsViewStack_);
    resultsGridScroll_->setObjectName(QStringLiteral("searchResultsGridScroll"));
    resultsGridScroll_->setWidgetResizable(true);
    resultsGridScroll_->setFrameShape(QFrame::NoFrame);
    resultsGridScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    resultsGridScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    if (QScrollBar* vbar = resultsGridScroll_->verticalScrollBar()) {
        vbar->setSingleStep(48);
    }
    resultsGridScroll_->viewport()->installEventFilter(this);
    resultsGridScroll_->setWidget(resultsGridHost_);
    resultsViewStack_->addWidget(resultsGridScroll_);

    results_ = new QListWidget(resultsViewStack_);
    results_->setObjectName(QStringLiteral("searchResults"));
    results_->setIconSize(QSize(theme::kSearchPosterWidth, theme::kSearchPosterHeight));
    results_->setTextElideMode(Qt::ElideRight);
    results_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    results_->setContextMenuPolicy(Qt::CustomContextMenu);
    results_->installEventFilter(this);
    connect(results_, &QListWidget::itemClicked, this, &SearchPage::openItem);
    connect(results_, &QListWidget::itemActivated, this, &SearchPage::openItem);
    connect(results_, &QListWidget::customContextMenuRequested, this,
            &SearchPage::resultsContextMenu);
    resultsViewStack_->addWidget(results_);

    resultsSkeleton_ = new QWidget(resultsViewStack_);
    resultsSkeleton_->setObjectName(QStringLiteral("searchResultsSkeleton"));
    auto* skeletonFlow = new FlowLayout(theme::kSpacingMedium, theme::kSpacingMedium);
    skeletonFlow->setContentsMargins(0, theme::kSpacingXs, theme::kSpacingSmall,
                                     theme::kSpacingSmall);
    resultsSkeleton_->setLayout(skeletonFlow);
    for (int i = 0; i < 8; ++i) {
        auto* skel = new QFrame(resultsSkeleton_);
        skel->setObjectName(QStringLiteral("settingsCard"));
        skel->setFixedSize(theme::kHomeCardWidth, theme::kHomeCardHeight);
        auto* sl = new QHBoxLayout(skel);
        sl->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium,
                               theme::kSpacingLarge, theme::kSpacingMedium);
        sl->setSpacing(theme::kSpacingMedium);
        auto* posterBox = new QFrame(skel);
        posterBox->setObjectName(QStringLiteral("carouselPoster"));
        posterBox->setFixedSize(theme::kHomePosterWidth, theme::kHomePosterHeight);
        sl->addWidget(posterBox, 0, Qt::AlignTop | Qt::AlignLeft);
        auto* textCol = new QVBoxLayout();
        textCol->setSpacing(theme::kSpacingSmall);
        auto* line1 = new QLabel(tr("Loading…"), skel);
        line1->setObjectName(QStringLiteral("muted"));
        textCol->addWidget(line1);
        textCol->addStretch(1);
        sl->addLayout(textCol, 1);
        skeletonFlow->addWidget(skel);
    }
    resultsViewStack_->addWidget(resultsSkeleton_);

    resultsStateHost_ = new QWidget(resultsViewStack_);
    auto* stateHostLayout = new QVBoxLayout(resultsStateHost_);
    stateHostLayout->setContentsMargins(0, theme::kSpacingMedium, 0, 0);
    resultsStateCard_ = new QFrame(resultsStateHost_);
    resultsStateCard_->setObjectName(QStringLiteral("settingsCard"));
    auto* stateLayout = new QVBoxLayout(resultsStateCard_);
    stateLayout->setContentsMargins(theme::kSpacingLarge, theme::kSpacingLarge,
                                    theme::kSpacingLarge, theme::kSpacingLarge);
    stateLayout->setSpacing(theme::kSpacingSmall);
    resultsStateTitle_ = new QLabel(resultsStateCard_);
    resultsStateTitle_->setObjectName(QStringLiteral("sectionHeading"));
    resultsStateTitle_->setWordWrap(true);
    resultsStateDetail_ = new QLabel(resultsStateCard_);
    resultsStateDetail_->setObjectName(QStringLiteral("muted"));
    resultsStateDetail_->setWordWrap(true);
    auto* stateActions = new QHBoxLayout();
    stateActions->setSpacing(theme::kSpacingSmall);
    resultsStateRetry_ = new QPushButton(tr("Retry"), resultsStateCard_);
    theme::applyActionRole(resultsStateRetry_, theme::ActionRole::Primary);
    connect(resultsStateRetry_, &QPushButton::clicked, this, [this] { fetchPage(page_); });
    resultsStateClearFilters_ = new QPushButton(tr("Clear filters"), resultsStateCard_);
    theme::applyActionRole(resultsStateClearFilters_, theme::ActionRole::Secondary);
    connect(resultsStateClearFilters_, &QPushButton::clicked, this,
            &SearchPage::resetCatalogueFilters);
    resultsStateResetSearch_ = new QPushButton(tr("Back to Home"), resultsStateCard_);
    theme::applyActionRole(resultsStateResetSearch_, theme::ActionRole::Ghost);
    connect(resultsStateResetSearch_, &QPushButton::clicked, this, &SearchPage::returnHome);
    stateActions->addWidget(resultsStateRetry_);
    stateActions->addWidget(resultsStateClearFilters_);
    stateActions->addWidget(resultsStateResetSearch_);
    stateActions->addStretch(1);
    stateLayout->addWidget(resultsStateTitle_);
    stateLayout->addWidget(resultsStateDetail_);
    stateLayout->addLayout(stateActions);
    stateHostLayout->addWidget(resultsStateCard_, 0, Qt::AlignTop);
    stateHostLayout->addStretch(1);
    resultsViewStack_->addWidget(resultsStateHost_);

    resultsViewStack_->hide();
    rootLayout_->addWidget(resultsViewStack_, 1);

    {
        QSettings settings;
        const int savedSize = settings.value(QString::fromUtf8(kSearchPageSize),
                                             theme::kSearchResultsPerPage).toInt();
        if (savedSize == 20 || savedSize == 40 || savedSize == 60) {
            pageSize_ = savedSize;
        }
    }
    pager_ = new RangePager(pageSize_, this);
    pager_->setCompactPageNumbers(true);
    pager_->setPageSizeOptions({20, 40, 60}, pageSize_);
    connect(pager_, &RangePager::pageRequested, this, &SearchPage::goToPage);
    connect(pager_, &RangePager::pageSizeChanged, this, [this](int newSize) {
        pageSize_ = newSize;
        QSettings settings;
        settings.setValue(QString::fromUtf8(kSearchPageSize), newSize);
        if (!homeCurrent_) {
            page_ = 1;
            fetchPage(1);
        }
    });
    pager_->hide();
    rootLayout_->addWidget(pager_);

    healTimer_ = new QTimer(this);
    healTimer_->setSingleShot(true);
    healTimer_->setInterval(200);
    connect(healTimer_, &QTimer::timeout, this, &SearchPage::flushRecentPosters);

    // The same coalescing for thumbs, on a zero interval: a feed's posters land
    // in one burst, and one pass over the whole batch beats one pass per image.
    thumbTimer_ = new QTimer(this);
    thumbTimer_->setSingleShot(true);
    thumbTimer_->setInterval(0);
    connect(thumbTimer_, &QTimer::timeout, this, &SearchPage::flushThumbs);

    connect(&appearanceNotifier(), &AppearanceNotifier::changed, this,
            [this](const theme::Appearance& previous) {
                const auto& appearance = currentAppearance();
                if (previous.radiusStep != appearance.radiusStep ||
                    previous.coverGradient != appearance.coverGradient) {
                    refreshCardFrames();
                }
                refreshHeaderLogo();
                refreshHeaderIcons();
                if (teamBrowseMode_) renderTeamProfile();
            });

    // The page may be built into a stack at its default size, so the rail's
    // presentation is settled here and then kept current by resizeEvent.
    updateSidebarPresentation(true);
    updateHomeIndicator();

    QTimer::singleShot(0, this, &SearchPage::loadOngoing);
    if (!recentItems_.isEmpty()) {
        showCarousel(kRecentKey, recentItems_);
    }
}

SearchPage::~SearchPage() {
    qDeleteAll(carousels_);
    qDeleteAll(feeds_);
}

QString SearchPage::carouselHeading(const QString& key) const {
    if (key == kRecentKey) return tr("Recently opened");
    if (key == QLatin1String("ongoing")) return tr("Currently airing");
    if (key == QLatin1String("popular")) return tr("Popular on Mikai");
    return {};
}

QString SearchPage::feedHeading(const QString& key) const {
    if (key == QLatin1String("updates")) return tr("Latest updates");
    if (key == QLatin1String("schedule")) return tr("Release schedule");
    return {};
}

QString SearchPage::feedEmptyLine(const QString& key) const {
    return key == QLatin1String("updates") ? tr("No updates yet.")
                                           : tr("No airings in the schedule yet.");
}

QString SearchPage::feedCountLine(const QString& key, int count) const {
    if (key == QLatin1String("updates")) {
        return count == 1 ? tr("1 recent update") : tr("%1 recent updates").arg(count);
    }
    return count == 1 ? tr("1 upcoming airing") : tr("%1 upcoming airings").arg(count);
}

QString SearchPage::blockFailureContext(const QString& key) const {
    if (key == QLatin1String("updates")) return tr("Failed to load updates");
    if (key == QLatin1String("schedule")) return tr("Failed to load schedule");
    return tr("Failed to load");
}

QString SearchPage::feedDayLabel(const QString& dayKey) const {
    if (dayKey == QLatin1String("monday")) return tr("Mon");
    if (dayKey == QLatin1String("tuesday")) return tr("Tue");
    if (dayKey == QLatin1String("wednesday")) return tr("Wed");
    if (dayKey == QLatin1String("thursday")) return tr("Thu");
    if (dayKey == QLatin1String("friday")) return tr("Fri");
    if (dayKey == QLatin1String("saturday")) return tr("Sat");
    if (dayKey == QLatin1String("sunday")) return tr("Sun");
    return {};
}

void SearchPage::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
        event->type() == QEvent::StyleChange) {
        // A new font changes what the rail's captions measure at, so the mode
        // is re-derived and re-applied.
        updateSidebarPresentation(true);
    }
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    } else if (event->type() == QEvent::PaletteChange ||
               event->type() == QEvent::ApplicationPaletteChange) {
        refreshHeaderLogo();
        refreshHeaderIcons();
        refreshArtwork();
    }
}

bool SearchPage::event(QEvent* event) {
    // Qt consumes both of these in QWidget::event() and never forwards them to
    // changeEvent(), so this is the only place a monitor move is observable.
    const bool ratioChanged = event->type() == QEvent::DevicePixelRatioChange ||
                              event->type() == QEvent::ScreenChangeInternal;
    const bool result = QWidget::event(event);
    if (ratioChanged) {
        // The captions the rail measures at now resolve differently, and every
        // raster on the page was built for the previous ratio. The brand logo
        // refreshes from its own event filter.
        updateSidebarPresentation(true);
        refreshHeaderIcons();
        refreshArtwork();
    }
    return result;
}

void SearchPage::refreshHeaderLogo() {
    if (headerLogo_ == nullptr || headerLogoSource_.isNull()) return;
    const QColor tint = headerLogo_->palette().color(QPalette::WindowText);
    const qreal dpr = headerLogo_->devicePixelRatioF();
    if (!headerLogo_->pixmap().isNull() && tint == headerLogoTint_ &&
        qFuzzyCompare(dpr, headerLogoDpr_)) {
        return;
    }
    const QSize pixelSize = headerLogo_->size() * dpr;
    QImage tinted = headerLogoSource_.scaled(pixelSize, Qt::KeepAspectRatio,
                                              Qt::SmoothTransformation)
                        .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&tinted);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(tinted.rect(), tint);
    painter.end();
    QPixmap pixmap = QPixmap::fromImage(tinted);
    pixmap.setDevicePixelRatio(dpr);
    headerLogo_->setPixmap(pixmap);
    headerLogoTint_ = tint;
    headerLogoDpr_ = dpr;
}

void SearchPage::refreshHeaderActionWidths() {
    if (searchButton_ == nullptr || rateLabel_ == nullptr || rateLabel_->layout() == nullptr) return;
    searchButton_->ensurePolished();
    rateLabel_->ensurePolished();
    // Keep searchButton_ and rateLabel_ independently sized so rate-limit text
    // changes or translations never distort the primary Search CTA button.
    searchButton_->setMinimumWidth(searchButton_->sizeHint().width());
    searchButton_->setMaximumWidth(QWIDGETSIZE_MAX);
    rateLabel_->setMinimumWidth(rateLabel_->layout()->sizeHint().width());
    rateLabel_->setMaximumWidth(QWIDGETSIZE_MAX);
}

void SearchPage::refreshHeaderIcons() {
    // The glyphs resolve their ink when they are built, so every path that
    // re-tints the brand logo also rebuilds them: a theme switch cannot leave a
    // white play glyph on a pale sidebar.
    const QPalette pagePalette = palette();
    const QColor ink = pagePalette.color(QPalette::ButtonText);
    const QColor muted = pagePalette.color(QPalette::PlaceholderText);
    const QColor onPrimary(theme::searchHeaderPalette(currentAppearance()).onPrimary);
    const int trailing = theme::kIconGlyphPad;

    // The rail's entries draw at the sidebar's own glyph box; the trailing pad
    // is the gap between the glyph and the caption the expanded form shows.
    const int sidebarGlyph = theme::kSearchSidebarActionIconSize;
    if (mediaButton_ != nullptr) {
        mediaButton_->setIcon(theme::playIcon(sidebarGlyph, ink.name(), trailing));
    }
    if (homeButton_ != nullptr) {
        homeButton_->setIcon(theme::homeIcon(sidebarGlyph, ink.name(), trailing));
    }
    if (filtersButton_ != nullptr) {
        filtersButton_->setIcon(theme::filterIcon(sidebarGlyph, ink.name(), trailing));
    }
    if (teamsButton_ != nullptr) {
        teamsButton_->setIcon(theme::teamsIcon(sidebarGlyph, ink.name(), trailing));
    }
    if (searchButton_ != nullptr) {
        searchButton_->setIcon(theme::searchIcon(theme::kIconSize, onPrimary.name(), trailing));
        refreshHeaderActionWidths();
    }
    if (keysButton_ != nullptr) {
        keysButton_->setIcon(theme::keyboardIcon(sidebarGlyph, ink.name(), trailing));
    }
    if (settingsButton_ != nullptr) {
        settingsButton_->setIcon(theme::outlineGearIcon(sidebarGlyph, ink.name(), trailing));
    }
    if (searchGlyph_ != nullptr) {
        searchGlyph_->setPixmap(
            theme::searchIcon(theme::kIconSize, muted.name())
                .pixmap(QSize(theme::kIconSize, theme::kIconSize),
                        searchGlyph_->devicePixelRatioF()));
    }
}

void SearchPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateSidebarPresentation();
}

void SearchPage::updateSidebarPresentation(bool force) {
    if (sidebar_ == nullptr || headerLogo_ == nullptr) return;
    const bool expanded = width() >= theme::kSearchSidebarExpandedMinWidth;
    if (!force && expanded == sidebarExpanded_) return;
    sidebarExpanded_ = expanded;

    sidebar_->setFixedWidth(expanded ? theme::kSearchSidebarWidth
                                     : theme::kSearchSidebarRailWidth);
    const int inset = expanded ? theme::kSearchSidebarInset : theme::kSearchSidebarRailInset;
    const Qt::Alignment actionAlignment = expanded ? Qt::Alignment() : Qt::AlignHCenter;
    if (auto* rail = qobject_cast<QVBoxLayout*>(sidebar_->layout())) {
        rail->setContentsMargins(inset, inset, inset, inset);
        for (QToolButton* action : {mediaButton_, keysButton_, settingsButton_}) {
            if (action == nullptr) continue;
            rail->setAlignment(action, actionAlignment);
        }
    }
    if (sidebarBrandName_ != nullptr) {
        sidebarBrandName_->setVisible(expanded);
    }
    if (sidebarBrandLayout_ != nullptr) {
        sidebarBrandLayout_->setAlignment(headerLogo_, expanded
                                                         ? (Qt::AlignLeft | Qt::AlignVCenter)
                                                         : Qt::AlignHCenter);
    }
    for (QToolButton* button : {homeButton_, filtersButton_, teamsButton_, mediaButton_,
                                keysButton_, settingsButton_, updatesButton_}) {
        if (button == nullptr) continue;
        button->setToolButtonStyle(expanded ? Qt::ToolButtonTextBesideIcon
                                            : Qt::ToolButtonIconOnly);
    }
    for (QToolButton* action : {mediaButton_, keysButton_, settingsButton_}) {
        if (action == nullptr) continue;
        action->setProperty("compact", !expanded);
        if (expanded) {
            action->setMinimumWidth(0);
            action->setMaximumWidth(QWIDGETSIZE_MAX);
            action->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        } else {
            action->setFixedWidth(theme::kControlHeight);
            action->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        }
        action->style()->unpolish(action);
        action->style()->polish(action);
    }
}

void SearchPage::updateResultsHeading() {
    if (resultsHeading_ == nullptr) return;
    if (homeCurrent_) {
        resultsHeading_->setText(tr("Currently airing · sorted by newest episode"));
        return;
    }
    const QString countSuffix =
        total_ > 0
            ? QStringLiteral(" · ") +
                  (total_ == 1 ? tr("1 title") : tr("%1 titles").arg(QLocale().toString(total_)))
            : QString();
    if (teamBrowseMode_) {
        resultsHeading_->setText(tr("Anime from %1").arg(activeTeam_.name) + countSuffix);
    } else if (!currentQuery_.isEmpty()) {
        const QString elided =
            currentQuery_.size() > 40 ? currentQuery_.left(39) + QStringLiteral("…") : currentQuery_;
        resultsHeading_->setText(tr("Results for \"%1\"").arg(elided) + countSuffix);
    } else if (hasExplicitCatalogueCriteria(catalogueQuery_)) {
        resultsHeading_->setText(tr("Filtered catalogue") + countSuffix);
    } else {
        resultsHeading_->setText(tr("Recommendations") + countSuffix);
    }
}

void SearchPage::setResultsGridMode(bool grid) {
    gridViewMode_ = grid;
    QSettings settings;
    settings.setValue(QString::fromUtf8(kSearchViewMode),
                      grid ? QStringLiteral("grid") : QStringLiteral("list"));
    if (gridViewButton_ != nullptr) gridViewButton_->setChecked(grid);
    if (listViewButton_ != nullptr) listViewButton_->setChecked(!grid);
    if (!homeCurrent_ && resultsViewStack_ != nullptr &&
        resultsViewStack_->currentWidget() != resultsSkeleton_ &&
        resultsViewStack_->currentWidget() != resultsStateHost_) {
        resultsViewStack_->setCurrentWidget(grid ? static_cast<QWidget*>(resultsGridScroll_)
                                                 : static_cast<QWidget*>(results_));
    }
}
void SearchPage::openRandomAnime(const QStringList& genres, const QStringList& formats) {
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run(
        [client, genres, formats] {
            return QVariant::fromValue(client->getRandomAnime(genres, formats));
        },
        [self](const QVariant& result) {
            if (self.isNull()) return;
            const auto detail = result.value<backend::AnimeDetail>();
            if (!detail.ref.isEmpty()) {
                const QString poster = !detail.poster.big.webp.isEmpty()
                                           ? detail.poster.big.webp
                                           : detail.poster.big.jpg;
                Q_EMIT self->openRequested(detail.ref, poster);
            }
        },
        [self](const backend::Failure& failure) {
            if (self.isNull()) return;
            self->reportFailure(failure);
        });
}
void SearchPage::rebuildActiveFilterChips() {
    if (activeFiltersBar_ == nullptr || activeFiltersLayout_ == nullptr) return;
    while (QLayoutItem* item = activeFiltersLayout_->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    auto addChip = [this](const QString& text, const QString& tooltip, std::function<void()> onRemove) {
        auto* btn = new QPushButton(text + QStringLiteral("  ×"), activeFiltersBar_);
        btn->setObjectName(text.startsWith(QLatin1String("- "))
                               ? QStringLiteral("filterChipExclude")
                               : QStringLiteral("activeFilterChip"));
        btn->setToolTip(tooltip);
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QPushButton::clicked, this, [this, fn = std::move(onRemove)] {
            fn();
            applyCatalogueFilters();
        });
        activeFiltersLayout_->addWidget(btn);
    };

    for (const QString& g : catalogueQuery_.genres) {
        addChip(QStringLiteral("+ %1").arg(g), tr("Remove genre filter: %1").arg(g),
                [this, g] { catalogueQuery_.genres.removeAll(g); });
    }
    for (const QString& g : catalogueQuery_.genres_exclude) {
        addChip(QStringLiteral("- %1").arg(g), tr("Remove excluded genre: %1").arg(g),
                [this, g] { catalogueQuery_.genres_exclude.removeAll(g); });
    }
    for (const QString& t : catalogueQuery_.tags) {
        addChip(QStringLiteral("+ #%1").arg(t), tr("Remove tag filter: %1").arg(t),
                [this, t] { catalogueQuery_.tags.removeAll(t); });
    }
    for (const QString& t : catalogueQuery_.tags_exclude) {
        addChip(QStringLiteral("- #%1").arg(t), tr("Remove excluded tag: %1").arg(t),
                [this, t] { catalogueQuery_.tags_exclude.removeAll(t); });
    }
    if (catalogueQuery_.year_from || catalogueQuery_.year_to) {
        const QString range = QStringLiteral("%1–%2")
                                  .arg(catalogueQuery_.year_from ? QString::number(*catalogueQuery_.year_from) : QStringLiteral("…"))
                                  .arg(catalogueQuery_.year_to ? QString::number(*catalogueQuery_.year_to) : QStringLiteral("…"));
        addChip(tr("Year: %1").arg(range), tr("Remove year range filter"), [this] {
            catalogueQuery_.year_from.reset();
            catalogueQuery_.year_to.reset();
        });
    }
    for (int y : catalogueQuery_.years) {
        addChip(QString::number(y), tr("Remove year filter: %1").arg(y),
                [this, y] { catalogueQuery_.years.removeAll(y); });
    }
    for (const QString& f : catalogueQuery_.formats) {
        addChip(f.toUpper(), tr("Remove format filter: %1").arg(f),
                [this, f] { catalogueQuery_.formats.removeAll(f); });
    }
    for (const QString& s : catalogueQuery_.statuses) {
        addChip(labels::status(s).isEmpty() ? s : labels::status(s),
                tr("Remove status filter"),
                [this, s] { catalogueQuery_.statuses.removeAll(s); });
    }
    for (const QString& sn : catalogueQuery_.seasons) {
        addChip(sn, tr("Remove season filter: %1").arg(sn),
                [this, sn] { catalogueQuery_.seasons.removeAll(sn); });
    }
    for (const QString& st : catalogueQuery_.studios) {
        addChip(tr("Studio: %1").arg(st), tr("Remove studio filter: %1").arg(st),
                [this, st] { catalogueQuery_.studios.removeAll(st); });
    }
    for (qint64 tm : catalogueQuery_.teams) {
        addChip(tr("Team #%1").arg(tm), tr("Remove team filter #%1").arg(tm),
                [this, tm] { catalogueQuery_.teams.removeAll(tm); });
    }

    const bool hasChips = activeFiltersLayout_->count() > 0;
    if (hasChips) {
        auto* clearAll = new QPushButton(tr("Clear all"), activeFiltersBar_);
        clearAll->setObjectName(QStringLiteral("clearFiltersChip"));
        clearAll->setCursor(Qt::PointingHandCursor);
        connect(clearAll, &QPushButton::clicked, this, &SearchPage::resetCatalogueFilters);
        activeFiltersLayout_->addWidget(clearAll);
    }
    activeFiltersBar_->setVisible(hasChips && !homeCurrent_ && !teamBrowseMode_);
}

void SearchPage::syncResultsSortToolbar() {
    if (resultsSortBox_ == nullptr || resultsOrderButton_ == nullptr) return;
    syncingSortToolbar_ = true;
    int idx = resultsSortBox_->findData(catalogueQuery_.sort);
    if (idx < 0) idx = 0;
    resultsSortBox_->setCurrentIndex(idx);
    const bool asc = catalogueQuery_.order == QLatin1String("asc");
    resultsOrderButton_->setText(asc ? tr("↑ Asc") : tr("↓ Desc"));
    resultsOrderButton_->setToolTip(asc ? tr("Ascending order — click for descending")
                                        : tr("Descending order — click for ascending"));
    resultsOrderButton_->setAccessibleName(asc ? tr("Sort ascending") : tr("Sort descending"));
    syncingSortToolbar_ = false;
}

void SearchPage::updateHomeIndicator() {
    // Synchronize primary sidebar navigation states across Home, Filtered
    // Catalogue, Search Results, and Teams browse modes.
    if (homeButton_ != nullptr) homeButton_->setChecked(homeCurrent_);
    if (teamsButton_ != nullptr) teamsButton_->setChecked(!homeCurrent_ && teamBrowseMode_);
    if (filtersButton_ != nullptr) {
        const int activeCount = activeCatalogueFilterCount(catalogueQuery_);
        const bool active = !homeCurrent_ && !teamBrowseMode_ &&
                            hasExplicitCatalogueCriteria(catalogueQuery_);
        filtersButton_->setChecked(active);
        const QString label = activeCount > 0 ? tr("Filters (%1)").arg(activeCount) : tr("Filters");
        filtersButton_->setText(label);
        filtersButton_->setToolTip(
            activeCount > 0
                ? (activeCount == 1 ? tr("Filters — 1 active filter")
                                    : tr("Filters — %1 active filters").arg(activeCount))
                : tr("Filters"));
        filtersButton_->setAccessibleName(label);
    }
}

void SearchPage::retranslateUi() {
    // Only words move. The query keeps its text and its focus, the result page
    // its rows, the format bar its filter, the carousels their cards and the
    // feeds the rows they already hold - and nothing below reaches the network.
    if (query_ != nullptr) {
        query_->setPlaceholderText(tr("Search by title…"));
    }
    if (searchKeycap_ != nullptr) {
        searchKeycap_->setText(QStringLiteral("Ctrl+K"));
        searchKeycap_->setToolTip(tr("Press Ctrl+K or / to focus search"));
        searchKeycap_->setAccessibleName(tr("Press Ctrl+K to focus search"));
    }
    if (sidebarBrandName_ != nullptr) sidebarBrandName_->setText(tr("Mikai Ja Nai"));
    if (homeButton_ != nullptr) {
        // The caption, the hover tooltip and the accessible name are the same
        // word in every language; the rail's icon-only form shows the latter
        // two, the expanded form all three.
        homeButton_->setText(tr("Home"));
        homeButton_->setToolTip(tr("Home"));
        homeButton_->setAccessibleName(tr("Home"));
    }
    if (mediaButton_ != nullptr) {
        mediaButton_->setText(tr("Open video"));
        mediaButton_->setToolTip(tr("Open video"));
        mediaButton_->setAccessibleName(tr("Open video"));
    }
    if (openFileAction_ != nullptr) openFileAction_->setText(tr("Video file…"));
    if (openUrlAction_ != nullptr) openUrlAction_->setText(tr("Video URL…"));
    if (keysButton_ != nullptr) {
        keysButton_->setText(tr("Shortcuts"));
        keysButton_->setToolTip(tr("Keyboard shortcuts (?)"));
        keysButton_->setAccessibleName(tr("Keyboard shortcuts"));
    }
    if (settingsButton_ != nullptr) {
        settingsButton_->setText(tr("Settings"));
        settingsButton_->setToolTip(tr("Settings"));
        settingsButton_->setAccessibleName(tr("Settings"));
    }
    if (updatesButton_ != nullptr) {
        updatesButton_->setText(tr("Check for updates"));
        updatesButton_->setToolTip(tr("Check for updates"));
        updatesButton_->setAccessibleName(tr("Check for updates"));
    }
    if (searchButton_ != nullptr) {
        searchButton_->setText(tr("Search"));
        refreshHeaderActionWidths();
    }
    if (filtersButton_ != nullptr) {
        // The tooltip belongs to the dictionary load, which words its own
        // status; only the caption and the accessible name are this pass's.
        filtersButton_->setText(tr("Filters"));
        filtersButton_->setAccessibleName(tr("Filters"));
    }
    if (teamsButton_ != nullptr) {
        teamsButton_->setText(tr("Teams…"));
        teamsButton_->setToolTip(tr("Teams…"));
        teamsButton_->setAccessibleName(tr("Teams…"));
    }
    if (resultsHeading_ != nullptr) {
        // The heading names the mode the page is in, never the request that is
        // running: browse shows a recommendation set, everything else a search.
        updateResultsHeading();
    }
    if (teamDirectoryButton_ != nullptr) teamDirectoryButton_->setText(tr("← Teams"));
    if (teamBackButton_ != nullptr) teamBackButton_->setText(tr("All catalogue"));
    if (teamBrowseMode_) renderTeamProfile();
    // The bar's own "All (n)" caption is app wording; its counts, its tabs and
    // the active filter are read back from the state they already hold.
    if (formatBar_ != nullptr) {
        rebuildFormatBar();
        if (teamBrowseMode_) formatBar_->hide();
    }
    renderStatus();
    if (results_ != nullptr && results_->count() == static_cast<int>(entries_.size())) {
        // Re-worded where they stand. Rebuilding the list would drop the
        // selection and the scroll position the viewer is reading, and would
        // also re-ask for any thumb whose fetch had failed.
        for (int row = 0; row < results_->count(); ++row) {
            QListWidgetItem* item = results_->item(row);
            const QString text = resultRowText(entries_.at(row));
            item->setText(text);
            item->setToolTip(text);
        }
    }
    QStringList sectionKeys{kRecentKey};
    sectionKeys += feeds::all();
    for (const QString& key : sectionKeys) {
        if (CarouselSection* section = carouselSection(key)) {
            if (section->heading != nullptr) section->heading->setText(carouselHeading(key));
            section->previous->setToolTip(tr("Previous page (or Alt + mouse wheel)"));
            section->next->setToolTip(tr("Next page (or Alt + mouse wheel)"));
            section->previous->setAccessibleName(section->previous->toolTip());
            section->next->setAccessibleName(section->next->toolTip());
            renderCarouselLine(*section);
            if (section->hasFailure) {
                // The failure itself belongs to the surface, which re-localizes
                // what the app words and keeps what the backend produced. Only
                // the clause this page wrote is re-sent.
                section->failure->setContext(blockFailureContext(key));
            }
        }
        if (FeedSection* section = feedSection(key)) {
            if (section->heading != nullptr) section->heading->setText(feedHeading(key));
            section->retry->setText(tr("Refresh"));
            renderFeedLine(key, *section);
            renderFeedRows(*section);
            if (section->hasFailure) {
                section->failure->setContext(blockFailureContext(key));
            }
        }
    }
    // A wider translation is a different rail: the caption that has to fit is
    // measured again, and the mode is re-applied only if the page now crosses
    // its threshold.
    updateSidebarPresentation(true);
}

void SearchPage::renderCarouselLine(CarouselSection& section) {
    if (section.state == nullptr) return;
    switch (section.line) {
        case CarouselSection::Line::Unloaded:
            // The recently-opened block has no request behind it: it says so
            // until a title is recorded.
            section.state->setText(tr("Collection not loaded yet"));
            break;
        case CarouselSection::Line::Loading:
            section.state->setText(tr("Loading collection…"));
            break;
        case CarouselSection::Line::Empty:
            section.state->setText(tr("No titles in this collection yet."));
            break;
        case CarouselSection::Line::Count:
            section.state->setText(section.lineCount == 1
                                       ? tr("1 title")
                                       : tr("%1 titles").arg(section.lineCount));
            break;
    }
}

void SearchPage::renderFeedLine(const QString& key, FeedSection& section) {
    if (section.state == nullptr) return;
    switch (section.line) {
        case FeedSection::Line::Loading:
            // Each feed words its own progress; a load has already installed
            // the state this restates.
            section.state->setText(key == QLatin1String("schedule") ? tr("Loading schedule…")
                                                                    : tr("Loading new episodes…"));
            break;
        case FeedSection::Line::Empty:
            section.state->setText(feedEmptyLine(key));
            break;
        case FeedSection::Line::Count:
            section.state->setText(feedCountLine(key, section.lineCount));
            break;
    }
}

void SearchPage::renderFeedRows(FeedSection& section) {
    // One pass over the labels the rows already hold: the row widgets, their
    // order and the host's scroll stay exactly where they are.
    const qsizetype count = qMin(section.rowMeta.size(), section.rowMetaLabels.size());
    for (qsizetype index = 0; index < count; ++index) {
        QLabel* label = section.rowMetaLabels.at(index).data();
        if (label == nullptr) continue;
        label->setText(composeFeedMeta(section.rowMeta.at(index)));
    }
}

void SearchPage::renderStatus() {
    if (status_ == nullptr) return;
    switch (statusLine_) {
        case Status::BrowseHint:
            status_->setText(
                tr("Click a card to open a title · search by title is available above"));
            break;
        case Status::LoadingBrowse:
            status_->setText(tr("Loading collection…"));
            break;
        case Status::LoadingSearch:
            status_->setText(tr("Searching…"));
            break;
        case Status::Results:
            status_->setText(entries_.isEmpty()
                                 ? tr("Nothing found")
                                 : (resultsHeading_ != nullptr && !resultsHeading_->isHidden()
                                        ? QString()
                                        : tr("Found: %1").arg(total_)));
            break;
        case Status::Failure: {
            // The same rule the failure surface uses: only a code the app words
            // itself is re-localized, a line the backend produced stays as it
            // arrived.
            backend::Failure failure = statusFailure_;
            const QString localized = localizedApiError(failure.code);
            if (!localized.isEmpty()) failure.message = localized;
            status_->setText(backend::failureLine(failure));
            break;
        }
        case Status::Notice:
            status_->setText(noticeText(statusNotice_));
            break;
    }
}

QString SearchPage::noticeText(Notice notice) const {
    switch (notice) {
        case Notice::CopyLink:
            return tr("Failed to copy link");
        case Notice::CopyId:
            return tr("Failed to copy ID");
        case Notice::Browser:
            return tr("Failed to open in browser");
    }
    return {};
}

QString SearchPage::composeFeedMeta(const FeedRowMeta& meta) const {
    QStringList parts;
    // A schedule row is the one that names its weekday first, and that is also
    // what decides where the clock text goes: after the weekday there, last for
    // an updates row.
    const bool scheduleRow = !meta.dayKey.isEmpty();
    const QString episode =
        meta.episode.isEmpty() ? (meta.newEpisode ? tr("New episode") : QString())
                               : tr("Episode %1").arg(meta.episode);
    if (scheduleRow) {
        parts << feedDayLabel(meta.dayKey);
        if (!meta.when.isEmpty()) parts << meta.when;
        if (!episode.isEmpty()) parts << episode;
    } else if (!episode.isEmpty()) {
        parts << episode;
    }
    if (!meta.team.isEmpty()) parts << meta.team;
    if (!meta.kind.isEmpty()) {
        parts << (meta.kind == QLatin1String("voice")
                      ? tr("dub")
                      : (meta.kind == QLatin1String("sub") ? tr("subs") : meta.kind));
    }
    if (!meta.provider.isEmpty()) {
        QString capitalized = meta.provider;
        capitalized[0] = capitalized.at(0).toUpper();
        parts << capitalized;
    }
    if (!scheduleRow && !meta.when.isEmpty()) parts << meta.when;
    // A weekday or episode label that did not resolve leaves an empty part
    // rather than a stray separator.
    parts.removeAll(QString());
    return parts.join(QStringLiteral(" · "));
}

QString SearchPage::resultRowText(const backend::AnimeSummary& entry) const {
    // List responses already carry the airing status and the planned episode
    // count, so a search row reads like a home card without a detail call.
    QStringList parts;
    parts << labels::titleName(entry.titles);
    if (entry.year.has_value()) {
        parts << QString::number(*entry.year);
    }
    if (!entry.format.isEmpty()) {
        parts << entry.format.toUpper();
    }
    const QString statusText = labels::status(entry.status);
    if (!statusText.isEmpty()) {
        parts << statusText;
    }
    if (entry.episodes.has_value() && *entry.episodes > 0) {
        parts << (*entry.episodes == 1 ? tr("1 episode")
                                       : tr("%1 episodes").arg(*entry.episodes));
    }
    return parts.join(QStringLiteral(" · "));
}

void SearchPage::closeTransientOverlays() {
    if (keyHints_ != nullptr) keyHints_->close();
}

void SearchPage::returnHome() {
    closeTransientOverlays();
    if (browseMode_ && !teamBrowseMode_ && !discoveryScroll_->isHidden() &&
        query_->text().isEmpty()) {
        ++requestSeq_;
        ++catalogueRequestSeq_;
        ++teamRequestSeq_;
        ++formatTotalsSeq_;
    } else {
        loadOngoing();
    }
    query_->clearFocus();
}

bool SearchPage::escapeSearch() {
    if (keyHints_ != nullptr && keyHints_->isVisible()) {
        keyHints_->close();
        return true;
    }
    returnHome();
    return true;
}

SearchPage::CarouselSection* SearchPage::buildCarousel(const QString& key) {
    auto* section = new CarouselSection();
    section->section = new QWidget(this);
    auto* layout = new QVBoxLayout(section->section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::kSpacingSmall - theme::kSpacingXs);

    auto* header = new QHBoxLayout();
    header->setSpacing(theme::kSpacingSmall);
    // The caption belongs to the heading, not to the row of controls: the pager
    // is 40 px tall, and a caption placed under that row floated a pager's height
    // away from the title it describes. Title and caption share a column; the
    // controls align to the title's top edge.
    auto* titleColumn = new QVBoxLayout();
    titleColumn->setContentsMargins(0, 0, 0, 0);
    titleColumn->setSpacing(theme::kSpacingXs / 2);
    section->heading = new QLabel(carouselHeading(key), section->section);
    section->heading->setObjectName(QStringLiteral("sectionHeading"));
    section->heading->setContentsMargins(0, 0, 0, 0);
    titleColumn->addWidget(section->heading);
    section->state = new QLabel(tr("Collection not loaded yet"), section->section);
    section->state->setObjectName(QStringLiteral("muted"));
    section->state->setContentsMargins(theme::kSpacingSmall, 0, 0, 0);
    section->state->setWordWrap(true);
    titleColumn->addWidget(section->state);
    header->addLayout(titleColumn, 1);
    section->previous = new CarouselButton(CarouselButton::Direction::Left, section->section);
    section->next = new CarouselButton(CarouselButton::Direction::Right, section->section);
    for (QPushButton* button : {section->previous, section->next}) {
        header->addWidget(button, 0, Qt::AlignTop);
    }
    layout->addLayout(header);

    // A failure is a whole-row state, so it keeps its own row under the title
    // block instead of squeezing into the caption's column.
    auto* failureRow = new QHBoxLayout();
    section->failure = new FailureView(section->section);
    connect(section->failure, &FailureView::retryRequested, this,
            [this, key] { requestRecommendation(key); });
    failureRow->addWidget(section->failure, 1);
    layout->addLayout(failureRow);

    section->scroll = new QScrollArea(section->section);
    section->scroll->setObjectName(QStringLiteral("animeCarousel"));
    section->scroll->setWidgetResizable(false);
    section->scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    section->scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    section->scroll->setFixedHeight(theme::kHomeCardHeight + theme::kSpacingMedium);
    auto* track = new QWidget(section->scroll);
    track->setObjectName(QStringLiteral("animeCarouselTrack"));
    section->cards = new QHBoxLayout(track);
    section->cards->setContentsMargins(theme::kCarouselTrackInset, theme::kCarouselTrackInset,
                                       theme::kCarouselTrackInset, theme::kCarouselTrackInset);
    section->cards->setSpacing(theme::kCarouselCardSpacing);
    section->scroll->setWidget(track);
    layout->addWidget(section->scroll);

    QScrollBar* bar = section->scroll->horizontalScrollBar();
    QPushButton* previous = section->previous;
    QPushButton* next = section->next;
    previous->setToolTip(tr("Previous page (or Alt + mouse wheel)"));
    next->setToolTip(tr("Next page (or Alt + mouse wheel)"));
    previous->setAccessibleName(previous->toolTip());
    next->setAccessibleName(next->toolTip());
    connect(previous, &QPushButton::clicked, this,
            [this, section] { smoothScrollCarousel(*section, -1); });
    connect(next, &QPushButton::clicked, this,
            [this, section] { smoothScrollCarousel(*section, 1); });
    // Both edges of the track disable the arrow that would go nowhere.
    connect(bar, &QScrollBar::rangeChanged, this,
            [bar, previous, next](int, int) { syncArrows(bar, previous, next); });
    connect(bar, &QScrollBar::valueChanged, this,
            [bar, previous, next](int) { syncArrows(bar, previous, next); });
    syncArrows(bar, previous, next);

    // A wheel gesture scrolls the carousel horizontally only when it is
    // horizontal itself or Alt is held; ordinary vertical scrolling passes
    // through to the discovery feed behind it.
    section->scroll->viewport()->installEventFilter(this);
    section->scroll->hide();

    carousels_.insert(key, section);
    return section;
}

bool SearchPage::handleFeedRowWheel(QScrollArea* scroll, QWheelEvent* event) {
    if (scroll == nullptr || event == nullptr) return false;
    QScrollBar* bar = scroll->horizontalScrollBar();
    if (bar == nullptr || bar->maximum() <= bar->minimum()) return false;

    const QPoint angle = event->angleDelta();
    const QPoint pixel = event->pixelDelta();
    const bool shiftHeld = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool altHeld = event->modifiers().testFlag(Qt::AltModifier);
    // A plain vertical wheel belongs to the page; only a horizontal gesture, Alt
    // (smooth) or Shift (strong) is addressed to the row.
    const bool horizontalGesture = !pixel.isNull() ? pixel.x() != 0 : angle.x() != 0;
    if (!shiftHeld && !altHeld && !horizontalGesture) return false;

    const int wheelSteps = !angle.isNull() ? angle.y() != 0 ? angle.y() : angle.x()
                                           : pixel.y() != 0 ? pixel.y() : pixel.x();
    const int direction = wheelSteps < 0 ? 1 : -1;  // a downward wheel moves right
    if (shiftHeld) {
        // Strong: half a viewport per notch, so long titles are reachable in a
        // couple of gestures without a drag.
        const int step = qMax(scroll->viewport()->width() / 2, theme::kSpacingLarge * 4);
        const int before = bar->value();
        bar->setValue(qBound(bar->minimum(), before + direction * step, bar->maximum()));
        if (bar->value() != before) {
            event->accept();
            return true;
        }
        return false;
    }

    // Alt: the same smooth fractional scrolling the native bar performs for the
    // carousels, forwarded so the animation curve stays identical.
    const QPointF local = bar->mapFromGlobal(event->globalPosition());
    const QPoint forwardedDelta = !angle.isNull() ? QPoint(0, angle.y() != 0 ? angle.y() : angle.x())
                                                  : QPoint(0, pixel.y() != 0 ? pixel.y() : pixel.x());
    QWheelEvent forwarded(local, event->globalPosition(), {}, forwardedDelta, event->buttons(),
                          event->modifiers(), event->phase(), event->inverted(),
                          event->source(), event->pointingDevice());
    forwarded.ignore();
    QCoreApplication::sendEvent(bar, &forwarded);
    event->setAccepted(forwarded.isAccepted());
    return event->isAccepted();
}

void SearchPage::smoothScrollCarousel(CarouselSection& section, int direction) {
    if (section.scroll == nullptr || direction == 0) {
        return;
    }
    QScrollBar* bar = section.scroll->horizontalScrollBar();
    if (bar == nullptr) {
        return;
    }
    const int spacing = section.cards != nullptr ? section.cards->spacing()
                                                 : theme::kCarouselCardSpacing;
    const int pitch = theme::kHomeCardWidth + spacing;
    const QMargins margins = section.cards != nullptr ? section.cards->contentsMargins()
                                                      : QMargins{};
    const int usable = section.scroll->viewport()->width() - margins.left() - margins.right();
    const int perPage = qMax(1, (usable + spacing) / pitch);
    const int start = bar->value();
    const bool animating = section.animation != nullptr &&
                           section.animation->state() == QAbstractAnimation::Running;
    // Repeated clicks advance from the pending destination, not an intermediate frame.
    const int base = animating ? section.animation->endValue().toInt() : start;
    const int pageIndex = (base + pitch / 2) / pitch;
    const int target = qBound(bar->minimum(), (pageIndex + direction * perPage) * pitch,
                             bar->maximum());
    if (animating && base == target) {
        return;
    }
    if (section.animation != nullptr) {
        section.animation->stop();
    }
    if (start == target) {
        return;
    }
    const QStyleHints* hints = QGuiApplication::styleHints();
    const QAccessibilityHints* accessibility = hints != nullptr ? hints->accessibility() : nullptr;
    if (accessibility != nullptr &&
        accessibility->motionPreference() == Qt::MotionPreference::ReducedMotion) {
        bar->setValue(target);
        return;
    }
    if (section.animation == nullptr) {
        section.animation = new QPropertyAnimation(bar, "value", this);
        section.animation->setDuration(240);
        section.animation->setEasingCurve(QEasingCurve::OutCubic);
    }
    section.animation->setStartValue(start);
    section.animation->setEndValue(target);
    section.animation->start();
}

bool SearchPage::eventFilter(QObject* watched, QEvent* event) {
    const auto scrollVerticalCrisply = [](QScrollArea* area, QWheelEvent* wheel) -> bool {
        if (area == nullptr || wheel == nullptr) return false;
        QScrollBar* bar = area->verticalScrollBar();
        if (bar == nullptr || bar->maximum() <= bar->minimum()) return false;
        const QPoint pixel = wheel->pixelDelta();
        const QPoint angle = wheel->angleDelta();
        int delta = 0;
        if (pixel.y() != 0) {
            delta = qRound(-pixel.y() * 1.4);
        } else {
            const int steps = angle.y() != 0
                                  ? angle.y()
                                  : (wheel->modifiers().testFlag(Qt::AltModifier) ? angle.x() : 0);
            if (steps != 0) {
                delta = qRound(-steps / 120.0 * 120.0);
            }
        }
        if (delta == 0) return false;
        const int before = bar->value();
        bar->setValue(qBound(bar->minimum(), before + delta, bar->maximum()));
        wheel->accept();
        return true;
    };

    if (event->type() == QEvent::Wheel) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        if (discoveryScroll_ != nullptr && watched == discoveryScroll_->viewport()) {
            if (scrollVerticalCrisply(discoveryScroll_, wheel)) return true;
        }
        if (resultsGridScroll_ != nullptr && watched == resultsGridScroll_->viewport()) {
            if (scrollVerticalCrisply(resultsGridScroll_, wheel)) return true;
        }
    }
    if ((watched == headerLogo_ || watched == sidebarBrandName_) &&
        event->type() == QEvent::MouseButtonPress) {
        const auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() != Qt::LeftButton) return false;
        if (!QDesktopServices::openUrl(
                QUrl(QStringLiteral("https://github.com/smyhlin/Mikai-Ja-Nai")))) {
            reportFailure(Notice::Browser);
        }
        return true;
    }
    if (watched == headerLogo_ &&
        (event->type() == QEvent::PaletteChange ||
         event->type() == QEvent::ApplicationPaletteChange ||
         event->type() == QEvent::ScreenChangeInternal ||
         event->type() == QEvent::DevicePixelRatioChange)) {
        refreshHeaderLogo();
    }
    if (watched == query_ &&
        (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) &&
        searchField_ != nullptr) {
        // The frame paints the focus ring: the field is one control, and the
        // line edit inside it cannot draw the ring around the glyph and keycap.
        searchField_->setProperty("focused", event->type() == QEvent::FocusIn);
        searchField_->style()->unpolish(searchField_);
        searchField_->style()->polish(searchField_);
        searchField_->update();
        if (searchKeycap_ != nullptr) {
            const bool showHint = !query_->hasFocus() && query_->text().isEmpty() &&
                                  searchField_->width() >= theme::kSearchKeycapMinFieldWidth;
            searchKeycap_->setVisible(showHint);
        }
        if (event->type() == QEvent::FocusIn && query_->text().trimmed().isEmpty()) {
            showSearchSuggestions(QString());
        } else if (event->type() == QEvent::FocusOut) {
            QTimer::singleShot(140, this, [this] {
                if (query_ != nullptr && !query_->hasFocus()) {
                    hideSearchSuggestions();
                }
            });
        }
    }
    if (watched == searchField_) {
        if (event->type() == QEvent::Resize && searchKeycap_ != nullptr) {
            const bool showHint = (query_ == nullptr || (!query_->hasFocus() && query_->text().isEmpty())) &&
                                  static_cast<QResizeEvent*>(event)->size().width() >=
                                      theme::kSearchKeycapMinFieldWidth;
            searchKeycap_->setVisible(showHint);
        } else if (event->type() == QEvent::MouseButtonPress && query_ != nullptr) {
            // The glyph and the keycap are decoration: a click anywhere in the
            // frame belongs to the line edit.
            query_->setFocus(Qt::MouseFocusReason);
            return true;
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape && escapeSearch()) {
            ke->accept();
            return true;
        }
        if (watched == query_ && ke->key() == Qt::Key_Down && results_ != nullptr &&
            results_->isVisible() && results_->count() > 0) {
            results_->setFocus(Qt::OtherFocusReason);
            if (results_->currentRow() < 0) {
                results_->setCurrentRow(0);
            }
            ke->accept();
            return true;
        }
    }
    if (event->type() == QEvent::Wheel) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        for (auto* section : carousels_) {
            if (section != nullptr && section->scroll != nullptr &&
                watched == section->scroll->viewport()) {
                if (handleCarouselWheel(*section, wheel)) {
                    return true;
                }
                // Forward vertical wheel directly to the discovery page at full crisp speed
                // instead of letting the child horizontal QScrollArea stall or swallow it.
                if (scrollVerticalCrisply(discoveryScroll_, wheel)) {
                    return true;
                }
                event->ignore();
                return false;
            }
        }
        if (auto* rowScroll = qobject_cast<QScrollArea*>(watched->parent());
            rowScroll != nullptr &&
            rowScroll->objectName() == QLatin1String("homeFeedRowScroll")) {
            if (handleFeedRowWheel(rowScroll, wheel)) return true;
            if (scrollVerticalCrisply(discoveryScroll_, wheel)) return true;
            event->ignore();
            return false;
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool SearchPage::handleCarouselWheel(CarouselSection& section, QWheelEvent* event) {
    if (section.scroll == nullptr || event == nullptr) {
        return false;
    }
    QScrollBar* bar = section.scroll->horizontalScrollBar();
    if (bar == nullptr) {
        return false;
    }
    const QPoint pixel = event->pixelDelta();
    const bool altHeld = event->modifiers().testFlag(Qt::AltModifier);
    // Qt's scrollbar wheel handler reads angleDelta, not pixelDelta.
    if (!pixel.isNull()) {
        const bool altVertical = altHeld && pixel.y() != 0;
        if (altVertical || qAbs(pixel.x()) > qAbs(pixel.y())) {
            if (section.animation != nullptr) {
                section.animation->stop();
            }
            const int before = bar->value();
            const int delta = altVertical ? pixel.y() : pixel.x();
            bar->setValue(static_cast<int>(qBound(
                static_cast<qint64>(bar->minimum()), static_cast<qint64>(before) - delta,
                static_cast<qint64>(bar->maximum()))));
            if (bar->value() != before) {
                event->accept();
                return true;
            }
        }
        if (pixel.y() != 0 && discoveryScroll_ != nullptr) {
            QScrollBar* pageBar = discoveryScroll_->verticalScrollBar();
            const int before = pageBar->value();
            const int delta = qRound(-pixel.y() * 1.4);
            pageBar->setValue(static_cast<int>(qBound(
                static_cast<qint64>(pageBar->minimum()), static_cast<qint64>(before) + delta,
                static_cast<qint64>(pageBar->maximum()))));
            event->setAccepted(pageBar->value() != before);
        } else {
            event->ignore();
        }
        // Angle noise must not reinterpret the pixel gesture in the child area.
        return true;
    }

    const QPoint angle = event->angleDelta();
    const bool altVertical = altHeld && angle.y() != 0;
    if (!altVertical && qAbs(angle.x()) <= qAbs(angle.y())) {
        return false;
    }
    if (bar->maximum() <= bar->minimum()) {
        return false;
    }
    if (section.animation != nullptr) {
        section.animation->stop();
    }
    event->ignore();
    if (altVertical && angle.x() != 0) {
        const QPointF local = bar->mapFromGlobal(event->globalPosition());
        QWheelEvent forwarded(local, event->globalPosition(), {}, QPoint(0, angle.y()),
                              event->buttons(), event->modifiers(), event->phase(),
                              event->inverted(), event->source(), event->pointingDevice());
        forwarded.ignore();
        QCoreApplication::sendEvent(bar, &forwarded);
        event->setAccepted(forwarded.isAccepted());
    } else {
        QCoreApplication::sendEvent(bar, event);
    }
    // Native fractional deltas can be consumed before the integer value changes.
    return event->isAccepted();
}

void SearchPage::placeIntro(bool inDiscovery) {
    if (pageIntro_ == nullptr || rootLayout_ == nullptr || discoveryLayout_ == nullptr ||
        inDiscovery == introInDiscovery_) {
        return;
    }
    if (inDiscovery) {
        rootLayout_->removeWidget(pageIntro_);
        discoveryLayout_->insertWidget(0, pageIntro_);
    } else {
        discoveryLayout_->removeWidget(pageIntro_);
        int index = activeFiltersBar_ != nullptr ? rootLayout_->indexOf(activeFiltersBar_) : -1;
        if (index < 0 && resultsToolbar_ != nullptr) {
            index = rootLayout_->indexOf(resultsToolbar_);
        }
        if (index < 0 && resultsViewStack_ != nullptr) {
            index = rootLayout_->indexOf(resultsViewStack_);
        }
        rootLayout_->insertWidget(index < 0 ? 1 : index, pageIntro_);
    }
    introInDiscovery_ = inDiscovery;
}

void SearchPage::keyPressEvent(QKeyEvent* event) {
    const Qt::KeyboardModifiers mods = event->modifiers();
    if (event->key() == Qt::Key_Escape && escapeSearch()) {
        event->accept();
        return;
    }
    if (((mods == Qt::ControlModifier) &&
         (event->key() == Qt::Key_K || event->key() == Qt::Key_F)) ||
        (mods == Qt::NoModifier && event->key() == Qt::Key_Slash &&
         (query_ == nullptr || !query_->hasFocus()))) {
        if (query_ != nullptr) {
            query_->setFocus(Qt::ShortcutFocusReason);
            query_->selectAll();
            event->accept();
            return;
        }
    }
    if (mods == Qt::AltModifier && !homeCurrent_ && pages_ > 1) {
        if (event->key() == Qt::Key_Left && page_ > 1) {
            goToPage(page_ - 1);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Right && page_ < pages_) {
            goToPage(page_ + 1);
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

SearchPage::FeedSection* SearchPage::buildFeedSection(const QString& key) {
    auto* section = new FeedSection();
    section->section = new QWidget(this);
    auto* layout = new QVBoxLayout(section->section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::kSpacingSmall - theme::kSpacingXs);

    auto* header = new QHBoxLayout();
    header->setSpacing(theme::kSpacingSmall);
    // Same hierarchy as a carousel: heading and count share one column, and the
    // action sits beside the heading instead of pushing the count a button's
    // height away from it.
    auto* titleColumn = new QVBoxLayout();
    titleColumn->setContentsMargins(0, 0, 0, 0);
    titleColumn->setSpacing(theme::kSpacingXs / 2);
    section->heading = new QLabel(feedHeading(key), section->section);
    section->heading->setObjectName(QStringLiteral("sectionHeading"));
    section->heading->setContentsMargins(0, 0, 0, 0);
    titleColumn->addWidget(section->heading);
    section->state = new QLabel(tr("Loading…"), section->section);
    section->state->setObjectName(QStringLiteral("muted"));
    section->state->setContentsMargins(theme::kSpacingSmall, 0, 0, 0);
    titleColumn->addWidget(section->state);
    header->addLayout(titleColumn, 1);
    section->retry = new QPushButton(tr("Refresh"), section->section);
    section->retry->setObjectName(QStringLiteral("feedRetryAction"));
    theme::applyActionRole(section->retry, theme::ActionRole::Ghost);
    connect(section->retry, &QPushButton::clicked, this, [this, key] {
        if (key == QLatin1String("updates")) {
            loadUpdates();
        } else {
            loadSchedule();
        }
    });
    header->addWidget(section->retry, 0, Qt::AlignTop);
    layout->addLayout(header);

    // The row already carries its own always-visible Refresh, so this surface
    // names the cause without putting a second retry beside it.
    section->failure = new FailureView(section->section);
    section->failure->setRetryVisible(false);
    layout->addWidget(section->failure);

    section->rowsHost = new QWidget(section->section);
    section->rows = new QVBoxLayout(section->rowsHost);
    section->rows->setContentsMargins(0, 0, 0, 0);
    section->rows->setSpacing(theme::kSpacingXs);
    section->rowsHost->hide();
    layout->addWidget(section->rowsHost);

    feeds_.insert(key, section);
    return section;
}

SearchPage::CarouselSection* SearchPage::carouselSection(const QString& key) const {
    return carousels_.value(key, nullptr);
}

SearchPage::FeedSection* SearchPage::feedSection(const QString& key) const {
    return feeds_.value(key, nullptr);
}

QList<backend::AnimeSummary> SearchPage::readRecentTitles() {
    QSettings settings;
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(
        settings.value(QString::fromUtf8(kHomeRecentTitles)).toString().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        return {};
    }
    QList<backend::AnimeSummary> items;
    for (const QJsonValue& value : document.array()) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject record = value.toObject();
        const QString ref = record.value(QStringLiteral("ref")).toString();
        if (ref.isEmpty()) {
            continue;
        }
        backend::AnimeSummary summary;
        summary.ref = ref;
        summary.titles.ua = record.value(QStringLiteral("title")).toString();
        const QString poster = record.value(QStringLiteral("poster")).toString();
        if (isRemoteUrl(poster)) {
            summary.poster.big.webp = poster;
        }
        const QString thumb = record.value(QStringLiteral("thumb")).toString();
        if (isRemoteUrl(thumb)) {
            summary.poster.small.webp = thumb;
        }
        items.append(summary);
        if (items.size() >= theme::kHomeRecentMaxTitles) {
            break;
        }
    }
    return items;
}

void SearchPage::persistRecentTitles() {
    QJsonArray records;
    for (const backend::AnimeSummary& recent : recentItems_) {
        QJsonObject record;
        record.insert(QStringLiteral("ref"), recent.ref);
        record.insert(QStringLiteral("title"), labels::titleName(recent.titles));
        const QString poster = backend::posterUrl(recent);
        record.insert(QStringLiteral("poster"), isRemoteUrl(poster) ? poster : QString());
        const QString thumb = backend::thumbUrl(recent);
        record.insert(QStringLiteral("thumb"), isRemoteUrl(thumb) ? thumb : QString());
        records.append(record);
    }
    QSettings settings;
    settings.setValue(QString::fromUtf8(kHomeRecentTitles),
                      QString::fromUtf8(QJsonDocument(records).toJson(QJsonDocument::Compact)));
}

void SearchPage::recordRecentTitle(const QString& ref, const QString& title,
                                   const QString& poster) {
    if (ref.isEmpty()) {
        return;
    }
    for (int index = recentItems_.size() - 1; index >= 0; --index) {
        if (recentItems_.at(index).ref == ref) {
            recentItems_.removeAt(index);
        }
    }
    backend::AnimeSummary summary;
    summary.ref = ref;
    summary.titles.ua = title;
    if (isRemoteUrl(poster)) {
        summary.poster.big.webp = poster;
    }
    recentItems_.prepend(summary);
    while (recentItems_.size() > theme::kHomeRecentMaxTitles) {
        recentItems_.removeLast();
    }
    persistRecentTitles();
    carouselSection(kRecentKey)->section->show();
    showCarousel(kRecentKey, recentItems_);
}

void SearchPage::showCarousel(const QString& key, const QList<backend::AnimeSummary>& items) {
    CarouselSection* section = carouselSection(key);
    if (section == nullptr) {
        return;
    }
    section->section->show();
    clearBlockFailure(section->failure, section->state);
    section->hasFailure = false;
    QList<backend::AnimeSummary> shown = items;
    while (shown.size() > theme::kHomeCarouselMaxTitles) {
        shown.removeLast();
    }
    carouselEntries_.insert(key, shown);
    if (shown.isEmpty()) {
        section->line = CarouselSection::Line::Empty;
        section->lineCount = 0;
        renderCarouselLine(*section);
        section->scroll->hide();
        return;
    }
    section->line = CarouselSection::Line::Count;
    section->lineCount = static_cast<int>(shown.size());
    renderCarouselLine(*section);

    while (section->cards->count() > 0) {
        QLayoutItem* item = section->cards->takeAt(0);
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    QList<TitleCard*> cards;
    for (const backend::AnimeSummary& anime : shown) {
        auto* card = new TitleCard(anime, section->section);
        connect(card, &TitleCard::openRequested, this, &SearchPage::openRequested);
        connect(card, &TitleCard::menuRequested, this, &SearchPage::animeCardMenu);
        section->cards->addWidget(card);
        cards.append(card);
    }
    // Registered before the thumbs are asked for: a url already in the cache
    // paints synchronously, and it can only find these cards once they are
    // the ones carouselWidgets_ points at.
    // The cards this call is about to drop are on their way to deleteLater, so
    // they have to leave the image index with them; a thumb landing afterwards
    // must not reach a freed card.
    for (TitleCard* card : carouselWidgets_.value(key)) {
        const auto indexed = cardsByImageKey_.find(card->imageKey());
        if (indexed != cardsByImageKey_.end()) {
            indexed.value().removeAll(card);
            if (indexed.value().isEmpty()) {
                cardsByImageKey_.erase(indexed);
            }
        }
        const auto byRef = cardsByRef_.find(card->ref());
        if (byRef != cardsByRef_.end()) {
            byRef.value().removeAll(card);
            if (byRef.value().isEmpty()) {
                cardsByRef_.erase(byRef);
            }
        }
    }
    carouselWidgets_.insert(key, cards);
    for (TitleCard* card : cards) {
        cardsByImageKey_[card->imageKey()].append(card);
        cardsByRef_[card->ref()].append(card);
    }
    for (TitleCard* card : cards) {
        requestThumb(card->imageKey());
    }

    QWidget* track = section->scroll->widget();
    section->cards->activate();
    const QMargins trackMargins = section->cards->contentsMargins();
    const int width = trackMargins.left() + trackMargins.right() +
                      shown.size() * theme::kHomeCardWidth +
                      qMax(0, shown.size() - 1) * section->cards->spacing();
    track->setFixedSize(width, theme::kHomeCardHeight + theme::kSpacingXs);
    section->scroll->show();
    for (const backend::AnimeSummary& anime : shown) {
        requestCardDetail(anime.ref);
    }
}

void SearchPage::requestCardDetail(const QString& ref) {
    // A list response carries only the planned episode count, so every card
    // needs one detail request for its aired fraction. Each goes out as its own
    // task, so the pool works through a whole feed at the client's advertised
    // rate. A recent card persists a bare poster URL with no other artwork
    // source, so this same lookup is also what heals a rotted one.
    if (ref.isEmpty() || detailLookups_.contains(ref)) {
        const auto cached = cardDetails_.constFind(ref);
        if (ref.isEmpty()) {
            return;
        }
        if (cached != cardDetails_.constEnd()) {
            applyCardDetail(ref, cached.value());
        }
        return;
    }
    detailLookups_.insert(ref);
    // The client is taken on the GUI thread and carried by value into the
    // worker: a worker that reached through `this` would read a page that may
    // already be gone. The completion is guarded the same way, so a result
    // that lands after the page was closed is dropped instead of touching it.
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.runBulk(
        [client, ref] { return QVariant::fromValue(client->getAnime(ref)); },
        [self, ref](const QVariant& result) {
            if (self.isNull()) {
                return;
            }
            self->applyCardDetail(ref, result.value<backend::AnimeDetail>());
        },
        [](const backend::Failure&) {});
}

void SearchPage::applyCardDetail(const QString& ref, const backend::AnimeDetail& detail) {
    cardDetails_.insert(ref, detail);
    queueRecentPosterHeal(ref, detail);
    for (TitleCard* card : cardsByRef_.value(ref)) {
        card->applyDetail(detail);
    }
}

void SearchPage::queueRecentPosterHeal(const QString& ref,
                                       const backend::AnimeDetail& detail) {
    // A burst of detail results would otherwise re-render the recent feed once
    // per title; one timer collapses them into a single repaint.
    const QString cover = backend::coverUrl(detail);
    if (!isRemoteUrl(cover)) {
        return;
    }
    backend::PosterSet poster;
    poster.big.webp = cover;
    // A card keys on thumbUrl, i.e. poster.small. Left empty it falls back to
    // the big cover, so every healed recent card pulls full-size artwork.
    const QString small = detail.poster.small.webp.isEmpty() ? detail.poster.small.jpg
                                                             : detail.poster.small.webp;
    poster.small.webp = isRemoteUrl(small) ? small : cover;
    const QList<backend::AnimeSummary>& recent = carouselEntries_.value(kRecentKey);
    const bool isRecent = std::any_of(recent.cbegin(), recent.cend(),
                                      [&ref](const backend::AnimeSummary& item) {
                                          return item.ref == ref;
                                      });
    if (!isRecent) {
        return;
    }
    healedPosters_.insert(ref, poster);
    healTimer_->start();
}

void SearchPage::flushRecentPosters() {
    const QHash<QString, backend::PosterSet> healed = healedPosters_;
    healedPosters_.clear();
    if (healed.isEmpty()) {
        return;
    }
    QList<backend::AnimeSummary> refreshed = carouselEntries_.value(kRecentKey);
    // A heal that lands on the url already stored changes nothing, and a
    // rebuild for nothing is what blanks the carousel in the first place.
    bool changed = false;
    for (backend::AnimeSummary& item : refreshed) {
        const auto it = healed.constFind(item.ref);
        if (it == healed.constEnd() || item.poster.big.webp == it.value().big.webp) {
            continue;
        }
        item.poster = it.value();
        changed = true;
    }
    if (!changed) {
        return;
    }
    carouselEntries_.insert(kRecentKey, refreshed);
    recentItems_ = refreshed.mid(0, theme::kHomeRecentMaxTitles);
    persistRecentTitles();
    showCarousel(kRecentKey, recentItems_);
}

void SearchPage::refreshFeedSettings() {
    QSettings settings;
    const QSet<QString> previous = enabledFeeds_;
    enabledFeeds_ = feeds::enabled(settings.value(feeds::settingsKey()).toString());
    if (previous == enabledFeeds_) {
        return;
    }
    for (const QString& key : feeds::all()) {
        const bool on = enabledFeeds_.contains(key);
        // Bumping the sequence drops an in-flight result, so it belongs only
        // to a feed being switched off. Bumping a feed that stays on threw away
        // the answer to the request it was already waiting on and left the
        // block on its loading line with nothing left to finish it.
        if (previous.contains(key) && !on) {
            feedSeq_[key] += 1;
        }
        CarouselSection* carousel = carouselSection(key);
        FeedSection* feed = feedSection(key);
        if (carousel != nullptr) {
            carousel->section->setVisible(on);
        }
        if (feed != nullptr) {
            feed->section->setVisible(on);
        }
    }
    // A feed switched on has to load; one switched off is simply never asked.
    for (const QString& key : feeds::all()) {
        if (enabledFeeds_.contains(key) && !previous.contains(key)) {
            loadFeed(key);
        }
    }
}

void SearchPage::loadFeed(const QString& key) {
    if (key == QLatin1String("ongoing") || key == QLatin1String("popular")) {
        requestRecommendation(key);
    } else if (key == QLatin1String("updates")) {
        loadUpdates();
    } else if (key == QLatin1String("schedule")) {
        loadSchedule();
    }
}

void SearchPage::loadOngoing() {
    browseMode_ = true;
    teamBrowseMode_ = false;
    ++requestSeq_;
    ++catalogueRequestSeq_;
    ++teamRequestSeq_;
    ++formatTotalsSeq_;
    if (teamHeroCard_ != nullptr) teamHeroCard_->hide();
    if (teamBackButton_ != nullptr) teamBackButton_->hide();
    if (teamDetailLabel_ != nullptr) teamDetailLabel_->hide();
    query_->clear();
    showDiscovery();
    currentQuery_.clear();
    activeFormat_.clear();
    pageSize_ = theme::kSearchResultsPerPage;
    page_ = 1;
    pages_ = 1;
    total_ = 0;
    savedResultsScroll_ = 0;
    savedSelectedRef_.clear();
    entries_.clear();
    results_->clear();
    pager_->hide();
    resultsHeading_->setText(tr("Recommendations"));
    statusLine_ = Status::BrowseHint;
    renderStatus();
    for (const QString& key : feeds::all()) {
        loadFeed(key);
    }
}

void SearchPage::requestRecommendation(const QString& key) {
    CarouselSection* section = carouselSection(key);
    if (section == nullptr) {
        return;
    }
    feedSeq_[key] += 1;
    const int seq = feedSeq_[key];
    if (!enabledFeeds_.contains(key)) {
        return;
    }
    section->line = CarouselSection::Line::Loading;
    section->lineCount = 0;
    renderCarouselLine(*section);
    clearBlockFailure(section->failure, section->state);
    section->hasFailure = false;
    if (carouselEntries_.value(key).isEmpty()) {
        section->scroll->hide();
    }
    const bool popular = key == QLatin1String("popular");
    // The client travels by value into the worker and the page is reached only
    // through a guard, so neither outlives the page that asked for the work.
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run(
        [client, popular] {
            return QVariant::fromValue(popular ? client->browsePopular(1, theme::kHomeCarouselMaxTitles)
                                               : client->browseOngoing(1, theme::kHomeCarouselMaxTitles));
        },
        [self, key, seq](const QVariant& result) {
            if (self.isNull() || seq != self->feedSeq_[key]) {
                return;
            }
            self->rateLabel_->refresh();
            self->showCarousel(key, result.value<backend::SearchResult>().items);
        },
        [self, key, seq](const backend::Failure& failure) {
            if (self.isNull() || seq != self->feedSeq_[key]) {
                return;
            }
            self->rateLabel_->refresh();
            CarouselSection* current = self->carouselSection(key);
            current->hasFailure = true;
            self->showBlockFailure(current->failure, current->state, failure,
                                   self->blockFailureContext(key));
        });
}

void SearchPage::loadUpdates() {
    if (!enabledFeeds_.contains(QStringLiteral("updates"))) {
        return;
    }
    FeedSection* section = feedSection(QStringLiteral("updates"));
    feedSeq_[QStringLiteral("updates")] += 1;
    const int seq = feedSeq_[QStringLiteral("updates")];
    section->line = FeedSection::Line::Loading;
    section->lineCount = 0;
    renderFeedLine(QStringLiteral("updates"), *section);
    section->retry->setEnabled(false);
    const backend::UpdateFeedState previousState = updateFeedState_;
    // The incremental feed refresh runs against the client the page was given,
    // carried into the worker by value: the page itself is not read from the
    // worker thread, and the completion only touches the page through a guard.
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run(
        [client, previousState] {
            backend::UpdateFeedState updated = backend::refreshUpdateFeed(*client, previousState, 12);
            return QVariant::fromValue(updated);
        },
        [self, seq](const QVariant& result) {
            if (self.isNull() || seq != self->feedSeq_[QStringLiteral("updates")]) {
                return;
            }
            self->rateLabel_->refresh();
            FeedSection* current = self->feedSection(QStringLiteral("updates"));
            current->retry->setEnabled(true);
            // The row line is kept as the parts it was built from, so a
            // language switch re-words it without asking the backend again.
            QList<std::pair<backend::AnimeSummary, FeedRowMeta>> rows;
            self->updateFeedState_ = result.value<backend::UpdateFeedState>();
            for (int index = 0; index < self->updateFeedState_.items.size() &&
                                 index < theme::kHomeFeedMaxRows;
                 ++index) {
                const backend::AnimeUpdate& update = self->updateFeedState_.items.at(index);
                if (update.anime.ref.isEmpty()) {
                    continue;
                }
                FeedRowMeta meta;
                meta.episode = labels::variant(update.episode);
                meta.newEpisode = meta.episode.isEmpty();
                meta.team = update.team_name;
                if (meta.team.isEmpty()) {
                    QStringList names;
                    for (const backend::Team& entry : update.teams) {
                        if (!entry.name.isEmpty()) {
                            names << entry.name;
                        }
                    }
                    meta.team = names.join(QLatin1String(", "));
                }
                meta.kind = update.kind;
                meta.provider = update.provider;
                meta.when = formatFeedTime(update.published_at);
                rows.append({update.anime, meta});
            }
            self->showFeedRows(QStringLiteral("updates"), rows);
        },
        [self, seq](const backend::Failure& failure) {
            if (self.isNull() || seq != self->feedSeq_[QStringLiteral("updates")]) {
                return;
            }
            self->rateLabel_->refresh();
            FeedSection* current = self->feedSection(QStringLiteral("updates"));
            current->retry->setEnabled(true);
            current->rowsHost->setVisible(!current->rowMeta.isEmpty());
            current->hasFailure = true;
            self->showBlockFailure(current->failure, current->state, failure,
                                   self->blockFailureContext(QStringLiteral("updates")));
        });
}

void SearchPage::loadSchedule() {
    if (!enabledFeeds_.contains(QStringLiteral("schedule"))) {
        return;
    }
    FeedSection* section = feedSection(QStringLiteral("schedule"));
    feedSeq_[QStringLiteral("schedule")] += 1;
    const int seq = feedSeq_[QStringLiteral("schedule")];
    section->line = FeedSection::Line::Loading;
    section->lineCount = 0;
    renderFeedLine(QStringLiteral("schedule"), *section);
    section->retry->setEnabled(false);
    // The client is carried into the worker by value; the completion reaches
    // the page only through a guard.
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run(
        [client] { return QVariant::fromValue(client->getSchedule()); },
        [self, seq](const QVariant& result) {
            if (self.isNull() || seq != self->feedSeq_[QStringLiteral("schedule")]) {
                return;
            }
            self->rateLabel_->refresh();
            FeedSection* current = self->feedSection(QStringLiteral("schedule"));
            current->retry->setEnabled(true);
            // Today first, then the rest of the week in order: the top of a
            // schedule is only useful if it starts with what airs next.
            static const char* kDayKeys[] = {"monday", "tuesday", "wednesday", "thursday",
                                             "friday",  "saturday", "sunday"};
            const int today = QDate::currentDate().dayOfWeek() - 1;
            const backend::ScheduleResult schedule = result.value<backend::ScheduleResult>();
            // Every row keeps its weekday key rather than the label it was
            // painted with, so a language switch re-words the whole list.
            QList<std::pair<backend::AnimeSummary, FeedRowMeta>> rows;
            for (int offset = 0; offset < 7 && rows.size() < theme::kHomeFeedMaxRows; ++offset) {
                const int day = (today + offset) % 7;
                QVector<backend::AiringEntry> airing = schedule.days.value(QLatin1String(kDayKeys[day]));
                std::sort(airing.begin(), airing.end(),
                          [](const backend::AiringEntry& left, const backend::AiringEntry& right) {
                              return left.airing < right.airing;
                          });
                for (const backend::AiringEntry& entry : airing) {
                    if (entry.finished || entry.anime.ref.isEmpty()) {
                        continue;
                    }
                    FeedRowMeta meta;
                    meta.dayKey = QLatin1String(kDayKeys[day]);
                    meta.when = formatFeedTime(entry.airing, true);
                    meta.episode = labels::variant(entry.episode);
                    rows.append({entry.anime, meta});
                    if (rows.size() >= theme::kHomeFeedMaxRows) {
                        break;
                    }
                }
            }
            self->showFeedRows(QStringLiteral("schedule"), rows);
        },
        [self, seq](const backend::Failure& failure) {
            if (self.isNull() || seq != self->feedSeq_[QStringLiteral("schedule")]) {
                return;
            }
            self->rateLabel_->refresh();
            FeedSection* current = self->feedSection(QStringLiteral("schedule"));
            current->retry->setEnabled(true);
            current->rowsHost->hide();
            current->hasFailure = true;
            self->showBlockFailure(current->failure, current->state, failure,
                                   self->blockFailureContext(QStringLiteral("schedule")));
        });
}

void SearchPage::showFeedRows(
    const QString& key,
    const QList<std::pair<backend::AnimeSummary, FeedRowMeta>>& entries) {
    FeedSection* section = feedSection(key);
    while (section->rows->count() > 0) {
        QLayoutItem* item = section->rows->takeAt(0);
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    feedPosterLabels_.insert(key, {});
    section->rowMeta.clear();
    section->rowMetaLabels.clear();
    section->failure->clear();
    section->hasFailure = false;
    section->line = entries.isEmpty() ? FeedSection::Line::Empty : FeedSection::Line::Count;
    section->lineCount = static_cast<int>(entries.size());
    renderFeedLine(key, *section);
    if (entries.isEmpty()) {
        section->state->show();
        section->rowsHost->hide();
        return;
    }
    section->state->hide();

    QVector<FeedPosterSlot> posters;
    for (const auto& entry : entries) {
        const backend::AnimeSummary& anime = entry.first;
        auto* row = new QWidget(section->rowsHost);
        row->setObjectName(QStringLiteral("homeFeedRow"));
        row->setContextMenuPolicy(Qt::CustomContextMenu);
        // Every row is one height, so a feed scans as a list whatever its rows
        // carry.
        row->setMinimumHeight(theme::kRowHeight);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(theme::kSpacingSmall + 2, theme::kSpacingXs,
                                      theme::kSpacingSmall + 2, theme::kSpacingXs);
        rowLayout->setSpacing(theme::kSpacingMedium);
        const QString thumb = backend::thumbUrl(anime);
        if (!thumb.isEmpty()) {
            auto* poster = new QLabel(row);
            poster->setObjectName(QStringLiteral("homeFeedPoster"));
            poster->setFixedSize(theme::kHomeFeedPosterWidth, theme::kHomeFeedPosterHeight);
            poster->setAlignment(Qt::AlignCenter);
            poster->setAttribute(Qt::WA_TransparentForMouseEvents);
            posters.append({thumb, poster, {}});
            rowLayout->addWidget(poster, 0, Qt::AlignVCenter);
        } else {
            // The poster slot is reserved even when there is no artwork, or the
            // titled rows and the untitled ones would start at different x.
            auto* slot = new QWidget(row);
            slot->setObjectName(QStringLiteral("homeFeedPosterSlot"));
            slot->setFixedSize(theme::kHomeFeedPosterWidth, theme::kHomeFeedPosterHeight);
            slot->setAttribute(Qt::WA_TransparentForMouseEvents);
            rowLayout->addWidget(slot, 0, Qt::AlignVCenter);
        }
        const QString titleText = labels::titleName(anime.titles);
        // A row never renders as a bare metadata strip: a summary without a
        // title still names the title it belongs to.
        const QString name = titleText.isEmpty() ? tr("Untitled") : titleText;
        auto* title = new QPushButton(name, row);
        title->setObjectName(QStringLiteral("homeFeedTitle"));
        title->setToolTip(name);
        const QString ref = anime.ref;
        const QString posterUrl = backend::posterUrl(anime);
        connect(title, &QPushButton::clicked, this,
                [this, ref, posterUrl] { Q_EMIT openRequested(ref, posterUrl); });
        connect(row, &QWidget::customContextMenuRequested, this,
                [this, ref, row](const QPoint& position) {
                    animeCardMenu(ref, row->mapToGlobal(position));
                });
        auto* meta = new QLabel(composeFeedMeta(entry.second), row);
        meta->setObjectName(QStringLiteral("homeFeedMeta"));
        meta->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rowLayout->addWidget(title, 1);
        rowLayout->addWidget(meta);
        // A row keeps its full content width and scrolls sideways inside the
        // page column: the title is a real caption, and eliding it or hiding the
        // episode line behind the poster was the only alternative on a narrow
        // window. The minimum comes from the widgets' own hints.
        title->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        row->setMinimumWidth(rowLayout->contentsMargins().left() +
                             theme::kHomeFeedPosterWidth + rowLayout->spacing() +
                             title->sizeHint().width() + rowLayout->spacing() +
                             meta->sizeHint().width() + rowLayout->contentsMargins().right());
        auto* rowScroll = new QScrollArea(section->rowsHost);
        rowScroll->setObjectName(QStringLiteral("homeFeedRowScroll"));
        rowScroll->setFrameShape(QFrame::NoFrame);
        rowScroll->setWidgetResizable(true);
        rowScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        rowScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        // The horizontal bar's own height is reserved so a row cannot jump when
        // it appears; the row stretches into the viewport, which keeps every
        // card the same height whether or not its content overflows.
        rowScroll->ensurePolished();
        const int barHeight =
            qMax(rowScroll->horizontalScrollBar()->sizeHint().height(), theme::kSpacingSmall);
        rowScroll->setFixedHeight(theme::kRowHeight + barHeight);
        rowScroll->setWidget(row);
        rowScroll->viewport()->installEventFilter(this);
        section->rows->addWidget(rowScroll);
        // Kept with the row so the line can be re-worded in place: the row
        // widget itself, its poster and its order never change for a language.
        section->rowMeta.append(entry.second);
        section->rowMetaLabels.append(meta);
    }
    section->rowsHost->show();
    feedPosterLabels_.insert(key, posters);
    for (const FeedPosterSlot& poster : posters) {
        requestThumb(poster.url);
    }
}


}  // namespace mikai::app
