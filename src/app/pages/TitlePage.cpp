#include "pages/TitlePageInternal.hpp"
#include "support/PlaybackSelection.hpp"

namespace mikai::app {
using namespace title_page_detail;

void TitlePage::setPendingPlaybackLink(const PlaybackLink& link) {
    pendingPlaybackLink_ = link;
}

TitlePage::TitlePage(Runner& runner, backend::MikaiClient& client, QualityCache& qualityCache,
                     const QString& ref, const QString& posterHint,
                     const backend::AnimeDetail& detailHint, QWidget* parent,
                     std::optional<PlaybackLink> pendingLink)
    : QWidget(parent), runner_(runner), client_(client), qualityCache_(qualityCache),
      ref_(ref), posterHint_(posterHint), pendingPlaybackLink_(std::move(pendingLink)) {
    // Only a hint for this exact title is worth anything: a stale detail for a
    // different ref would paint the wrong header, and an empty one is no hint.
    if (!detailHint.ref.isEmpty() && detailHint.ref == ref) {
        detailHint_ = detailHint;
        hasDetailHint_ = true;
    }
    fitTimer_ = new QTimer(this);
    fitTimer_->setSingleShot(true);
    connect(fitTimer_, &QTimer::timeout, this, &TitlePage::fitEpisodeSection);
    buildUi();
    load();
}

TitlePage::~TitlePage() {
    // Destruction cannot be refused: the players are taken through their own
    // close and released anyway, with each forced stop named.
    shutdown(/*forced=*/true);
}

void TitlePage::buildUi() {
    setObjectName(QStringLiteral("titlePage"));
    // A plain QWidget paints its scoped QSS background only with this set.
    setAttribute(Qt::WA_StyledBackground, true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    buildHeader();
    root->addWidget(header_);

    // One vertical scroll for everything below the sticky header; the page
    // never nests a second scroller.
    overviewScroll_ = new QScrollArea(this);
    overviewScroll_->setObjectName(QStringLiteral("titleOverview"));
    if (QScrollBar* vbar = overviewScroll_->verticalScrollBar()) {
        vbar->setSingleStep(48);
    }
    // The widget is sized explicitly from the column's own height-for-width in
    // reflowOverviewHeight(): letting QScrollArea size it from the children's
    // minimum hints is what produced hundreds of pixels of empty scroll.
    overviewScroll_->setWidgetResizable(false);
    overviewScroll_->setFrameShape(QFrame::NoFrame);
    overviewScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    overviewScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // The scroll widget itself carries the content cap and the scroll area
    // centres it, so the column fills the viewport until kTitlePageMaxWidth
    // instead of taking a stretch share of it. Stretch factors cannot express
    // "grow, then stop": a stretch-capped column would keep just its share.
    overviewScroll_->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    body_ = new QWidget(overviewScroll_);
    body_->setObjectName(QStringLiteral("titleBody"));
    body_->setMaximumWidth(theme::kTitlePageMaxWidth);
    auto* bodyLayout = new QVBoxLayout(body_);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    column_ = new QWidget(body_);
    column_->setObjectName(QStringLiteral("titleColumn"));
    column_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    column_->installEventFilter(this);
    bodyLayout->addWidget(column_, 1);
    // A scrollbar appearing changes the viewport width, and with it every wrap
    // point this page measures against.
    overviewScroll_->viewport()->installEventFilter(this);

    columnLayout_ = new QVBoxLayout(column_);
    columnLayout_->setContentsMargins(theme::kTitlePageInset, theme::kTitlePageInset,
                                      theme::kTitlePageInset, theme::kTitlePageInset);
    columnLayout_->setSpacing(theme::kTitleSectionSpacing);

    buildBody();
    reflowOverviewHeight();

    connect(&appearanceNotifier(), &AppearanceNotifier::changed, this,
            [this](const theme::Appearance& previous) {
                const auto& appearance = currentAppearance();
                // The backdrop caches a composite, so it is rebuilt from the
                // resolved wash colour: switching the gradient source alone
                // changes what is painted without touching the stored custom
                // colour, and comparing the stored field would leave the cached
                // hero wash on the previous source.
                if (backdrop_ != nullptr &&
                    (previous.siteTheme != appearance.siteTheme ||
                     previous.accent != appearance.accent ||
                     theme::gradientColor(previous) != theme::gradientColor(appearance) ||
                     previous.backgroundGradient != appearance.backgroundGradient ||
                     previous.gradientIntensity != appearance.gradientIntensity ||
                     previous.gradientHeight != appearance.gradientHeight)) {
                    backdrop_->refreshPalette();
                }
                if ((previous.siteTheme != appearance.siteTheme ||
                     previous.accent != appearance.accent) && hasDetail_) {
                    refreshDetailTexts();
                }
                if (previous.radiusStep != appearance.radiusStep) {
                    refreshReleaseChipSizing();
                    scheduleFit();
                }
                if (previous.radiusStep != appearance.radiusStep ||
                    previous.coverGradient != appearance.coverGradient) {
                    // The radius and the shade are baked into the frame, so a
                    // change to either repaints it from the decoded pixels.
                    refreshRelatedPosters();
                    coverAppliedSize_ = {};
                    applyCoverPixmap();
                }
            });

    overviewScroll_->setWidget(body_);
    root->addWidget(overviewScroll_, 1);

    buildDetailsSheet();
    applyResponsiveLayout();
}

void TitlePage::buildHeader() {
    header_ = new QWidget(this);
    header_->setObjectName(QStringLiteral("titleHeader"));
    header_->setAttribute(Qt::WA_StyledBackground, true);
    header_->setFixedHeight(theme::kControlHeight + 2 * theme::kSpacingMedium);
    // The header spans the window so its surface can, while the row inside it
    // carries the same content cap as the body. Side margins are the centring
    // mechanism: a stretch pair would only ever give the row a third.
    headerOuterLayout_ = new QHBoxLayout(header_);
    headerOuterLayout_->setContentsMargins(0, 0, 0, 0);
    headerOuterLayout_->setSpacing(0);

    headerRow_ = new QWidget(header_);
    headerRow_->setObjectName(QStringLiteral("titleHeaderRow"));
    headerRow_->setMaximumWidth(theme::kTitlePageMaxWidth);
    headerRow_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    headerOuterLayout_->addWidget(headerRow_, 1);

    headerRowLayout_ = new QHBoxLayout(headerRow_);
    headerRowLayout_->setContentsMargins(theme::kTitlePageInset, theme::kSpacingMedium,
                                         theme::kTitlePageInset, theme::kSpacingMedium);
    headerRowLayout_->setSpacing(theme::kSpacingMedium);

    backButton_ = new QPushButton(tr("Back"), headerRow_);
    backButton_->setObjectName(QStringLiteral("titleBack"));
    theme::applyActionRole(backButton_, theme::ActionRole::Secondary);
    backButton_->setIcon(theme::chevronLeftIcon(theme::kIconSize));
    backButton_->setIconSize(QSize(theme::kIconSize, theme::kIconSize));
    // The glyph carries the direction; the caption is the accessible name and
    // the label, so the button reads the same at every DPI.
    backButton_->setAccessibleName(tr("Back"));
    backButton_->setCursor(Qt::PointingHandCursor);
    // Opening the page must not paint a focus ring on a control nobody focused:
    // Tab reaches it, a page load does not.
    backButton_->setFocusPolicy(Qt::TabFocus);
    connect(backButton_, &QPushButton::clicked, this, &TitlePage::backRequested);

    homeButton_ = new QPushButton(tr("Home"), headerRow_);
    homeButton_->setObjectName(QStringLiteral("titleHome"));
    homeButton_->setCursor(Qt::PointingHandCursor);
    homeButton_->setFocusPolicy(Qt::TabFocus);
    connect(homeButton_, &QPushButton::clicked, this, &TitlePage::homeRequested);

    status_ = new QLabel(headerRow_);
    status_->setObjectName(QStringLiteral("status"));
    // Right-aligned against the rate chip: the stretch absorbs text-length
    // changes on the left, so flashes never push the rate/settings controls.
    status_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // Ignored horizontally: a long failure line must not widen a page that has
    // no horizontal bar.
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    status_->setTextFormat(Qt::PlainText);
    status_->setMinimumWidth(0);
    status_->setWordWrap(false);

    settingsButton_ = new QPushButton(headerRow_);
    settingsButton_->setObjectName(QStringLiteral("settingsChip"));
    // The same outline gear the main page uses for the same action.
    settingsButton_->setIcon(theme::outlineGearIcon());
    settingsButton_->setIconSize(QSize(theme::kIconSize, theme::kIconSize));
    settingsButton_->setToolTip(tr("Settings"));
    settingsButton_->setAccessibleName(tr("Settings"));
    settingsButton_->setCursor(Qt::PointingHandCursor);
    connect(settingsButton_, &QPushButton::clicked, this, &TitlePage::settingsRequested);

    // One control height across the row, so the pills, the rate chip and the
    // gear read as a single bar whatever the font does to their text.
    for (QPushButton* control : {backButton_, homeButton_}) {
        control->setMinimumHeight(theme::kControlHeight);
    }
    // The gear is the one fixed square: an icon chip whose box no label or
    // style padding may widen.
    settingsButton_->setFixedSize(theme::kControlHeight, theme::kControlHeight);

    rateLabel_ = new RateLimitLabel(client_, headerRow_);
    connect(rateLabel_, &RateLimitLabel::clicked, this,
            &TitlePage::accessSettingsRequested);
    headerRowLayout_->addWidget(backButton_);
    headerRowLayout_->addWidget(homeButton_);
    headerRowLayout_->addStretch(1);
    headerRowLayout_->addWidget(status_);
    headerRowLayout_->addWidget(rateLabel_);
    headerRowLayout_->addWidget(settingsButton_);
}

void TitlePage::refreshActionWidths() {
    // Captions arrive with the detail, the history and the episode, so the
    // groups are measured again after each of those, not once at construction.
    theme::equalizeWidths({backButton_, homeButton_});
    theme::equalizeWidths({watchWatchedButton_, watchButton_});
}

void TitlePage::buildBody() {
    buildHero();
    buildEpisodeWorkspace();
    buildDiscovery();
    columnLayout_->addStretch(1);
}

void TitlePage::buildHero() {
    hero_ = new QWidget(column_);
    hero_->setObjectName(QStringLiteral("titleHero"));
    hero_->installEventFilter(this);
    backdrop_ = new PosterBackdrop(hero_);
    backdrop_->setObjectName(QStringLiteral("titleBackdrop"));
    backdrop_->show();
    backdrop_->lower();
    heroLayout_ = new QBoxLayout(QBoxLayout::LeftToRight, hero_);
    heroLayout_->setContentsMargins(0, 0, 0, 0);
    heroLayout_->setSpacing(theme::kTitleHeroSpacing);

    heroPosterHost_ = new QWidget(hero_);
    heroPosterHost_->setObjectName(QStringLiteral("titleHeroPoster"));
    heroPosterHost_->setFixedWidth(theme::kTitleHeroCoverWidth);
    auto* posterLayout = new QVBoxLayout(heroPosterHost_);
    posterLayout->setContentsMargins(0, 0, 0, 0);
    posterLayout->setSpacing(theme::kSpacingSmall);

    cover_ = new CoverLabel(tr("Loading…"), heroPosterHost_);
    coverState_ = CoverState::Loading;
    cover_->setObjectName(QStringLiteral("titleCover"));
    cover_->setFixedSize(theme::kTitleHeroCoverWidth, theme::kTitleHeroCoverHeight);
    connect(cover_, &CoverLabel::activated, this, &TitlePage::openCoverViewer);
    cover_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(cover_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& position) { coverContextMenu(cover_->mapToGlobal(position)); });
    posterLayout->addWidget(cover_, 0, Qt::AlignTop | Qt::AlignHCenter);

    studio_ = new QLabel(heroPosterHost_);
    studio_->setObjectName(QStringLiteral("titleStudio"));
    studio_->setTextFormat(Qt::PlainText);
    wrapLabel(studio_);
    studio_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    posterLayout->addWidget(studio_);
    posterLayout->addStretch(1);
    heroLayout_->addWidget(heroPosterHost_, 0, Qt::AlignLeft | Qt::AlignTop);

    auto* textColumn = new QVBoxLayout();
    // Facts, scores, description and disclosures stack at token gap; the
    // section gaps are explicit spacers below.
    textColumn->setSpacing(theme::kTitleStackSpacing);

    heading_ = new QLabel(hero_);
    heading_->setObjectName(QStringLiteral("titleHeading"));
    heading_->setTextFormat(Qt::PlainText);
    wrapLabel(heading_);
    heading_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    heading_->setFocusPolicy(Qt::StrongFocus);
    heading_->setCursor(Qt::PointingHandCursor);
    heading_->setToolTip(tr("Copy title"));
    heading_->setAccessibleDescription(tr("Copy title"));
    heading_->installEventFilter(this);
    textColumn->addWidget(heading_);

    subtitle_ = new QLabel(hero_);
    subtitle_->setObjectName(QStringLiteral("titleSubtitle"));
    subtitle_->setTextFormat(Qt::PlainText);
    wrapLabel(subtitle_);
    subtitle_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    subtitle_->setFocusPolicy(Qt::StrongFocus);
    subtitle_->setCursor(Qt::PointingHandCursor);
    subtitle_->setToolTip(tr("Copy title"));
    subtitle_->setAccessibleDescription(tr("Copy title"));
    subtitle_->installEventFilter(this);
    textColumn->addWidget(subtitle_);

    textColumn->addSpacing(theme::kSpacingMedium);

    failure_ = new FailureView(hero_);
    connect(failure_, &FailureView::retryRequested, this, &TitlePage::load);
    textColumn->addWidget(failure_);

    facts_ = new QLabel(hero_);
    facts_->setObjectName(QStringLiteral("titleFacts"));
    wrapLabel(facts_);
    textColumn->addWidget(facts_);

    scores_ = new QLabel(hero_);
    scores_->setObjectName(QStringLiteral("titleScores"));
    wrapLabel(scores_);
    scores_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    scores_->hide();
    // Added without an alignment flag: an aligned item keeps its single-line
    // size hint, which is what clipped a wrapped score line.
    textColumn->addWidget(scores_);

    // Genre and tag chips: wrapped like the disclosure actions, filled from the
    // retained detail, hidden until there is one.
    auto* chipsHost = new QWidget(hero_);
    chipsHost->setObjectName(QStringLiteral("titleGenres"));
    QSizePolicy chipsPolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    chipsPolicy.setHeightForWidth(true);
    chipsHost->setSizePolicy(chipsPolicy);
    chipsHost->setLayout(new FlowLayout(theme::kSpacingSmall, theme::kSpacingXs));
    chipsHost->hide();

    heroWatchButton_ = new QPushButton(tr("Watch"), hero_);
    heroWatchButton_->setObjectName(QStringLiteral("titleHeroWatchButton"));
    theme::applyActionRole(heroWatchButton_, theme::ActionRole::Primary);
    heroWatchButton_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    heroWatchButton_->setCursor(Qt::PointingHandCursor);
    heroWatchButton_->setEnabled(false);
    heroWatchButton_->setToolTip(tr("Select an available episode to watch"));
    connect(heroWatchButton_, &QPushButton::clicked, this, [this] {
        EpisodeEntry* target = watchEntry_;
        if (target == nullptr && tabs_ != nullptr) {
            target = preferredEntry(tabs_->currentIndex());
        }
        if (target != nullptr) {
            selectWatchEntry(target);
            playEpisode(target);
        }
    });
    auto* heroActionsHost = new QWidget(hero_);
    heroActionsHost->setObjectName(QStringLiteral("titleHeroActions"));
    QSizePolicy heroActionsPolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    heroActionsPolicy.setHeightForWidth(true);
    heroActionsHost->setSizePolicy(heroActionsPolicy);
    auto* heroActions = new FlowLayout(theme::kSpacingSmall, theme::kSpacingSmall);
    heroActionsHost->setLayout(heroActions);

    heroActions->addWidget(heroWatchButton_);
    heroActions->setAlignment(heroWatchButton_, Qt::AlignVCenter);
    metaButton_ = new QPushButton(tr("More details"), heroActionsHost);
    metaButton_->setObjectName(QStringLiteral("titleDetailsToggle"));
    theme::applyActionRole(metaButton_, theme::ActionRole::Ghost);
    metaButton_->hide();
    metaButton_->setCheckable(true);
    metaButton_->setToolTip(tr("Show title details"));
    metaButton_->setAccessibleName(tr("More details"));
    metaButton_->setCursor(Qt::PointingHandCursor);
    connect(metaButton_, &QPushButton::toggled, this, &TitlePage::toggleMetadata);
    heroActions->addWidget(metaButton_);
    heroActions->setAlignment(metaButton_, Qt::AlignVCenter);
    refreshActionWidths();
    QWidget::setTabOrder(heading_, subtitle_);
    QWidget::setTabOrder(subtitle_, heroWatchButton_);
    QWidget::setTabOrder(heroWatchButton_, metaButton_);
    textColumn->addWidget(heroActionsHost);

    heroActionContextHost_ = new QWidget(hero_);
    auto* actionContext = new QVBoxLayout(heroActionContextHost_);
    actionContext->setContentsMargins(0, theme::kSpacingSmall, 0, 0);
    actionContext->setSpacing(theme::kSpacingXs);
    actionContext->addWidget(chipsHost);
    heroActionContextHost_->hide();
    textColumn->addWidget(heroActionContextHost_);

    textColumn->addSpacing(theme::kSpacingSmall);
    description_ = new QLabel(hero_);
    description_->installEventFilter(this);
    description_->setObjectName(QStringLiteral("titleDescription"));
    description_->setTextFormat(Qt::PlainText);
    wrapLabel(description_);
    description_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    description_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    textColumn->addWidget(description_);

    disclosureRow_ = new QWidget(hero_);
    disclosureRow_->setObjectName(QStringLiteral("titleDisclosureActions"));
    // The actions wrap: their widest translation must not pan a compact column.
    QSizePolicy disclosurePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    disclosurePolicy.setHeightForWidth(true);
    disclosureRow_->setSizePolicy(disclosurePolicy);
    auto* disclosure = new FlowLayout(theme::kSpacingSmall, theme::kSpacingSmall);
    disclosureRow_->setLayout(disclosure);
    descriptionButton_ = new QPushButton(tr("Read more"), disclosureRow_);
    descriptionButton_->setObjectName(QStringLiteral("titleSynopsisToggle"));
    theme::applyActionRole(descriptionButton_, theme::ActionRole::Ghost);
    descriptionButton_->setCursor(Qt::PointingHandCursor);
    descriptionButton_->hide();
    connect(descriptionButton_, &QPushButton::clicked, this, &TitlePage::toggleDescription);
    QWidget::setTabOrder(metaButton_, descriptionButton_);
    disclosure->addWidget(descriptionButton_);
    // Synopsis is the only action directly below its preview.
    disclosureRow_->hide();
    textColumn->addWidget(disclosureRow_);

    textColumn->addSpacing(theme::kSpacingMedium);
    links_ = new QWidget(hero_);
    links_->setLayout(new FlowLayout(theme::kSpacingXs, theme::kSpacingXs));
    QSizePolicy linksPolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    linksPolicy.setHeightForWidth(true);
    links_->setSizePolicy(linksPolicy);
    textColumn->addWidget(links_);

    textColumn->addStretch(1);
    heroLayout_->addLayout(textColumn, 1);
    columnLayout_->addWidget(hero_);
}

void TitlePage::buildDiscovery() {
    // Discovery follows playback in the one overview scroll.
    discoveryHost_ = new QWidget(column_);
    discoveryHost_->setObjectName(QStringLiteral("titleDiscoverySection"));
    auto* discoveryLayout = new QVBoxLayout(discoveryHost_);
    discoveryLayout->setContentsMargins(0, 0, 0, 0);
    discoveryLayout->setSpacing(theme::kTitleStackSpacing);
    auto* discoveryHeader = new QHBoxLayout();
    discoveryHeading_ = new QLabel(tr("More to explore"), discoveryHost_);
    discoveryHeading_->setObjectName(QStringLiteral("titleSectionHeading"));
    discoveryHeader->addWidget(discoveryHeading_);
    discoveryHeader->addStretch(1);
    relatedButton_ = new QPushButton(tr("Hide"), discoveryHost_);
    relatedButton_->setObjectName(QStringLiteral("titleDiscoveryToggle"));
    theme::applyActionRole(relatedButton_, theme::ActionRole::Ghost);
    relatedButton_->setMinimumHeight(40);
    relatedButton_->setCheckable(true);
    relatedButton_->setCursor(Qt::PointingHandCursor);
    connect(relatedButton_, &QPushButton::toggled, this,
            [this](bool checked) { setRelatedExpanded(checked, true); });
    discoveryHeader->addWidget(relatedButton_);
    discoveryLayout->addLayout(discoveryHeader);
    relatedHost_ = new QWidget(discoveryHost_);
    relatedHost_->setObjectName(QStringLiteral("titleRelated"));
    relatedLayout_ = new QVBoxLayout(relatedHost_);
    relatedLayout_->setContentsMargins(0, 0, 0, 0);
    relatedLayout_->setSpacing(theme::kTitleSectionSpacing);
    relatedHost_->hide();
    discoveryLayout->addWidget(relatedHost_);
    discoveryHost_->hide();
    columnLayout_->addWidget(discoveryHost_);
}

void TitlePage::buildEpisodeWorkspace() {
    episodesHost_ = new QWidget(column_);
    episodesHost_->setObjectName(QStringLiteral("titleEpisodes"));
    auto* episodesLayout = new QVBoxLayout(episodesHost_);
    episodesLayout->setContentsMargins(0, 0, 0, 0);
    episodesLayout->setSpacing(theme::kTitleWorkspaceGroupGap);

    episodeHeaderHost_ = new QWidget(episodesHost_);
    episodeHeaderHost_->setObjectName(QStringLiteral("titleEpisodeHeader"));
    episodeHeaderHost_->setAttribute(Qt::WA_StyledBackground, true);
    auto* episodeHeader = new QHBoxLayout(episodeHeaderHost_);
    episodeHeader->setContentsMargins(0, 0, 0, theme::kSpacingSmall);
    episodeHeader->setSpacing(theme::kSpacingMedium);
    episodesHeading_ = new QLabel(tr("Episodes"), episodeHeaderHost_);
    episodesHeading_->setObjectName(QStringLiteral("episodeSectionHeading"));
    episodesHeading_->setFocusPolicy(Qt::StrongFocus);
    episodeCount_ = new QLabel(episodeHeaderHost_);
    episodeCount_->setObjectName(QStringLiteral("titleEpisodeCount"));
    episodeCount_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    episodeCount_->setWordWrap(false);
    episodeHeader->addWidget(episodesHeading_);
    episodeHeader->addStretch(1);
    episodeHeader->addWidget(episodeCount_);
    episodesLayout->addWidget(episodeHeaderHost_);

    episodeStatus_ = new QLabel(episodesHost_);
    episodeStatus_->setObjectName(QStringLiteral("titleEpisodeStatus"));
    episodeStatus_->setTextFormat(Qt::PlainText);
    episodeStatus_->setWordWrap(true);
    episodeStatus_->setFocusPolicy(Qt::StrongFocus);
    episodeStatus_->hide();
    episodesLayout->addWidget(episodeStatus_);

    episodeAvailability_ = new QLabel(episodesHost_);
    episodeAvailability_->setObjectName(QStringLiteral("episodeAvailabilityLabel"));
    episodeAvailability_->setTextFormat(Qt::PlainText);
    episodeAvailability_->setWordWrap(true);
    episodeAvailability_->setFocusPolicy(Qt::StrongFocus);
    episodeAvailability_->hide();
    episodesLayout->addWidget(episodeAvailability_);
    nextAiring_ = new QLabel(episodesHost_);
    nextAiring_->setObjectName(QStringLiteral("nextAiringLabel"));
    nextAiring_->setTextFormat(Qt::PlainText);
    nextAiring_->setWordWrap(true);
    nextAiring_->hide();
    episodesLayout->addWidget(nextAiring_);
    licenseInfo_ = new QLabel(episodesHost_);
    licenseInfo_->setObjectName(QStringLiteral("licenseInfoLabel"));
    licenseInfo_->setTextFormat(Qt::RichText);
    licenseInfo_->setOpenExternalLinks(false);
    licenseInfo_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    licenseInfo_->setWordWrap(true);
    licenseInfo_->hide();
    connect(licenseInfo_, &QLabel::linkActivated, this, [](const QString& target) {
        const QUrl url(target);
        if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) {
            QDesktopServices::openUrl(url);
        }
    });
    episodesLayout->addWidget(licenseInfo_);

    playbackControls_ = new QWidget(episodesHost_);
    playbackControls_->setObjectName(QStringLiteral("titlePlaybackControls"));
    playbackControls_->setAttribute(Qt::WA_StyledBackground, true);
    playbackControls_->hide();
    auto* playbackLayout = new QVBoxLayout(playbackControls_);
    playbackLayout->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium,
                                       theme::kSpacingMedium, theme::kSpacingMedium);
    playbackLayout->setSpacing(theme::kTitleWorkspaceGroupGap);
    episodesLayout->addWidget(playbackControls_);
    releaseSelectorHeading_ = new QLabel(tr("Release"), playbackControls_);
    releaseSelectorHeading_->setObjectName(QStringLiteral("releaseSelectorHeading"));
    releaseSelectorHeading_->hide();
    playbackLayout->addWidget(releaseSelectorHeading_);

    releaseChipsHost_ = new QWidget(playbackControls_);
    releaseChipsHost_->setObjectName(QStringLiteral("releaseChipsHost"));
    releaseRowsLayout_ = new QVBoxLayout(releaseChipsHost_);
    releaseRowsLayout_->setContentsMargins(theme::kSpacingSmall, theme::kSpacingSmall,
                                            theme::kSpacingSmall, theme::kSpacingSmall);
    releaseRowsLayout_->setSpacing(theme::kSpacingSmall);
    playbackLayout->addWidget(releaseChipsHost_);

    releaseGroup_ = new QButtonGroup(this);
    releaseGroup_->setExclusive(true);
    connect(releaseGroup_, &QButtonGroup::idClicked, this, [this](int index) {
        if (index != activeRelease_) {
            fillEpisodes(index);
        }
        probeReleaseQualities();
    });

    watchPanel_ = new QWidget(playbackControls_);
    watchPanel_->setObjectName(QStringLiteral("titleWatchPanel"));
    watchPanel_->setAttribute(Qt::WA_StyledBackground, true);
    watchLayout_ = new QBoxLayout(QBoxLayout::LeftToRight, watchPanel_);
    watchLayout_->setContentsMargins(theme::kTitleWatchPanelPadding,
                                     theme::kTitleWatchPanelPadding,
                                     theme::kTitleWatchPanelPadding,
                                     theme::kTitleWatchPanelPadding);
    watchLayout_->setSpacing(theme::kTitleWatchPanelGap);
    auto* watchText = new QVBoxLayout();
    watchText->setContentsMargins(0, 0, 0, 0);
    watchText->setSpacing(theme::kSpacingXs);
    watchEpisode_ = new QLabel(watchPanel_);
    watchEpisode_->setObjectName(QStringLiteral("titleWatchEpisode"));
    watchEpisode_->setTextFormat(Qt::PlainText);
    wrapLabel(watchEpisode_);
    watchMeta_ = new QLabel(watchPanel_);
    watchMeta_->setObjectName(QStringLiteral("titleWatchMeta"));
    wrapLabel(watchMeta_);
    watchText->addWidget(watchEpisode_);
    watchText->addWidget(watchMeta_);
    // A hairline of how far the half-watched episode got: it belongs with the
    // episode line, not in a separate badge, and it costs one widget.
    watchProgress_ = new QProgressBar(watchPanel_);
    watchProgress_->setObjectName(QStringLiteral("titleWatchProgress"));
    static_cast<QProgressBar*>(watchProgress_)->setRange(0, 1000);
    static_cast<QProgressBar*>(watchProgress_)->setTextVisible(false);
    watchProgress_->setFixedHeight(4);
    watchProgress_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    watchProgress_->hide();
    watchText->addWidget(watchProgress_);
    watchText->addStretch(1);
    watchLayout_->addLayout(watchText, 1);
    watchWatchedButton_ = new QPushButton(tr("Watched"), watchPanel_);
    watchWatchedButton_->setObjectName(QStringLiteral("titleWatchWatchedToggle"));
    theme::applyActionRole(watchWatchedButton_, theme::ActionRole::Secondary);
    watchWatchedButton_->setFixedHeight(theme::kControlHeight);
    watchWatchedButton_->setCheckable(true);
    watchWatchedButton_->setCursor(Qt::PointingHandCursor);
    connect(watchWatchedButton_, &QPushButton::toggled, this, [this](bool checked) {
        if (watchEntry_ == nullptr || !entryIsPlayable(watchEntry_)) {
            return;
        }
        setEpisodeWatched(watchEntry_->number, checked);
    });
    watchButton_ = new QPushButton(tr("Watch"), watchPanel_);
    watchButton_->setObjectName(QStringLiteral("titleWatchButton"));
    theme::applyActionRole(watchButton_, theme::ActionRole::Primary);
    watchButton_->setFixedHeight(theme::kWatchCtaHeight);
    watchButton_->setCursor(Qt::PointingHandCursor);
    connect(watchButton_, &QPushButton::clicked, this, [this] {
        EpisodeEntry* target = watchEntry_;
        if (target == nullptr && tabs_ != nullptr) {
            target = preferredEntry(tabs_->currentIndex());
        }
        if (target != nullptr) {
            selectWatchEntry(target);
            playEpisode(target);
        }
    });
    auto* watchActions = new QWidget(watchPanel_);
    watchActions->setObjectName(QStringLiteral("titleWatchActions"));
    auto* watchActionsLayout = new QVBoxLayout(watchActions);
    watchActionsLayout->setContentsMargins(0, 0, 0, 0);
    watchActionsLayout->setSpacing(theme::kTitleWatchPanelGap);
    watchActionsLayout->addWidget(watchWatchedButton_);
    watchActionsLayout->addWidget(watchButton_);
    refreshActionWidths();
    watchLayout_->addWidget(watchActions, 0, Qt::AlignVCenter);
    watchPanel_->hide();

    providerSelectorHeading_ = new QLabel(tr("Provider"), playbackControls_);
    providerSelectorHeading_->setObjectName(QStringLiteral("providerSelectorHeading"));
    providerSelectorHeading_->hide();
    playbackLayout->addWidget(providerSelectorHeading_);

    tabs_ = new TitlePageTabs(playbackControls_);
    tabs_->setObjectName(QStringLiteral("episodeTabs"));
    tabs_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    tabs_->setDocumentMode(true);
    tabs_->tabBar()->setExpanding(false);
    tabs_->tabBar()->setDrawBase(false);
    tabs_->tabBar()->setUsesScrollButtons(true);
    tabs_->tabBar()->hide();
    providerBar_ = new QTabBar(releaseChipsHost_);
    providerBar_->setObjectName(QStringLiteral("providerBar"));
    providerBar_->setExpanding(false);
    providerBar_->setDrawBase(false);
    providerBar_->setUsesScrollButtons(true);
    providerBar_->setCursor(Qt::PointingHandCursor);
    providerBar_->installEventFilter(this);
    connect(providerBar_, &QTabBar::currentChanged, tabs_, &QTabWidget::setCurrentIndex);
    playbackLayout->addWidget(tabs_);
    playbackLayout->addWidget(watchPanel_);

    episodeLegend_ = new QWidget(episodesHost_);
    episodeLegend_->setObjectName(QStringLiteral("titleEpisodeLegend"));
    auto* legendRow = new QHBoxLayout(episodeLegend_);
    legendRow->setContentsMargins(0, theme::kSpacingXs, 0, 0);
    legendRow->setSpacing(theme::kSpacingSmall);
    const auto addLegendItem = [this, legendRow](const QString& swatchName, const QSize& swatchSize,
                                                 QLabel** caption) {
        auto* swatch = new QWidget(episodeLegend_);
        swatch->setObjectName(swatchName);
        swatch->setAttribute(Qt::WA_StyledBackground, true);
        swatch->setFixedSize(swatchSize);
        *caption = new QLabel(episodeLegend_);
        (*caption)->setObjectName(QStringLiteral("episodeLegendLabel"));
        (*caption)->setTextFormat(Qt::PlainText);
        wrapLabel(*caption);
        legendRow->addWidget(swatch, 0, Qt::AlignVCenter);
        legendRow->addWidget(*caption, 0, Qt::AlignVCenter);
    };
    const QSize swatch(theme::kTitleLegendSwatchSize, theme::kTitleLegendSwatchSize);
    addLegendItem(QStringLiteral("episodeLegendCurrent"), swatch, &legendCurrent_);
    legendRow->addSpacing(theme::kSpacingMedium);
    addLegendItem(QStringLiteral("episodeLegendWatched"),
                  QSize(theme::kTitleWatchedMarkWidth, theme::kTitleWatchedMarkHeight),
                  &legendWatched_);
    legendRow->addSpacing(theme::kSpacingMedium);
    addLegendItem(QStringLiteral("episodeLegendUnavailable"), swatch, &legendUnavailable_);
    legendRow->addStretch(1);
    episodeLegend_->hide();
    episodesLayout->addWidget(episodeLegend_);
    // A tab that has never been shown has no pills in it; this is where they
    // get built, once, the first time the reader goes there.
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index < 0 || index >= tabStates_.size()) {
            return;
        }
        // A tab change only reaches here from the viewer: fillEpisodes blocks
        // the tabs while it applies the default, so the default never looks
        // like a choice, and this one outlives the release switch that rebuilds
        // the tabs.
        chosenProvider_ = tabStates_.at(index).provider;
        if (!tabStates_.at(index).rendered) {
            renderTabPage(index);
        }
        // Another provider's tab carries the same episode list, so the episode
        // already chosen - or the one the title's history says was last
        // playing - is kept instead of jumping back to the first pill.
        selectWatchEntry(preferredEntry(index));
        fitEpisodeSection();
    });

    columnLayout_->addWidget(episodesHost_);
}

void TitlePage::buildDetailsSheet() {
    // A fixed layer, not a dialog: opening it never rebuilds or refetches, and
    // Escape/scrim/close return to the same scroll position.
    detailsOverlay_ = new QWidget(this);
    detailsOverlay_->setObjectName(QStringLiteral("titleDetailsOverlay"));
    detailsOverlay_->setAttribute(Qt::WA_StyledBackground, true);
    detailsOverlay_->setAccessibleName(tr("Details"));
    detailsOverlay_->hide();

    detailsScrim_ = new QPushButton(detailsOverlay_);
    detailsScrim_->setObjectName(QStringLiteral("titleDetailsScrim"));
    detailsScrim_->setFlat(true);
    detailsScrim_->setCursor(Qt::PointingHandCursor);
    detailsScrim_->setAccessibleName(tr("Close details"));
    detailsScrim_->setFocusPolicy(Qt::NoFocus);
    connect(detailsScrim_, &QPushButton::clicked, this, [this] { setDetailsOpen(false); });

    detailsPanel_ = new QWidget(detailsOverlay_);
    detailsPanel_->setObjectName(QStringLiteral("titleDetailsPanel"));
    detailsPanel_->setAttribute(Qt::WA_StyledBackground, true);
    auto* panelLayout = new QVBoxLayout(detailsPanel_);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);

    detailsHeaderHost_ = new QWidget(detailsPanel_);
    detailsHeaderHost_->setObjectName(QStringLiteral("titleDetailsHeader"));
    detailsHeaderHost_->setAttribute(Qt::WA_StyledBackground, true);
    auto* detailsHeader = new QHBoxLayout(detailsHeaderHost_);
    detailsHeader->setContentsMargins(theme::kSpacingLarge, theme::kSpacingMedium,
                                      theme::kSpacingLarge, theme::kSpacingMedium);
    detailsHeader->setSpacing(theme::kSpacingMedium);
    detailsHeading_ = new QLabel(tr("Details"), detailsHeaderHost_);
    detailsHeading_->setObjectName(QStringLiteral("titleDetailsHeading"));
    detailsClose_ = new QPushButton(detailsHeaderHost_);
    detailsClose_->setObjectName(QStringLiteral("titleDetailsClose"));
    // The glyph is the same painted vector the chevrons use, so the close mark
    // carries the button colour at every DPI instead of a font's fallback.
    detailsClose_->setIcon(theme::closeIcon());
    detailsClose_->setIconSize(QSize(theme::kIconSize, theme::kIconSize));
    detailsClose_->setFixedSize(theme::kTitleDetailsCloseSize, theme::kTitleDetailsCloseSize);
    detailsClose_->setCursor(Qt::PointingHandCursor);
    detailsClose_->setAccessibleName(tr("Close details"));
    detailsClose_->setToolTip(tr("Close details"));
    connect(detailsClose_, &QPushButton::clicked, this, [this] { setDetailsOpen(false); });
    detailsHeader->addWidget(detailsHeading_);
    detailsHeader->addStretch(1);
    detailsHeader->addWidget(detailsClose_);
    panelLayout->addWidget(detailsHeaderHost_);

    detailsScroll_ = new QScrollArea(detailsPanel_);
    detailsScroll_->setObjectName(QStringLiteral("titleDetailsScroll"));
    if (QScrollBar* vbar = detailsScroll_->verticalScrollBar()) {
        vbar->setSingleStep(48);
    }
    detailsScroll_->setWidgetResizable(true);
    detailsScroll_->setFrameShape(QFrame::NoFrame);
    detailsScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    metaHost_ = new QWidget(detailsScroll_);
    metaHost_->setObjectName(QStringLiteral("titleDetails"));
    auto* detailsContent = new QVBoxLayout(metaHost_);
    detailsContent->setContentsMargins(theme::kSpacingLarge, theme::kSpacingLarge,
                                       theme::kSpacingLarge, theme::kSpacingLarge);
    detailsContent->setSpacing(theme::kSpacingLarge);

    auto* identity = new QHBoxLayout();
    identity->setContentsMargins(0, 0, 0, 0);
    identity->setSpacing(theme::kSpacingMedium);
    detailsThumb_ = new QLabel(metaHost_);
    detailsThumb_->setObjectName(QStringLiteral("titleDetailsThumb"));
    detailsThumb_->setFixedSize(theme::kTitleHeroCoverWidth / 4,
                                theme::kTitleHeroCoverHeight / 4);
    detailsThumb_->setAlignment(Qt::AlignCenter);
    detailsThumb_->setWordWrap(true);
    detailsThumb_->setText(tr("No cover"));
    detailsThumb_->setCursor(Qt::PointingHandCursor);
    detailsThumb_->setFocusPolicy(Qt::StrongFocus);
    detailsThumb_->setToolTip(QCoreApplication::translate("TitlePage", "Click to open the cover"));
    detailsThumb_->setAccessibleName(QCoreApplication::translate("TitlePage", "Click to open the cover"));
    detailsThumb_->installEventFilter(this);
    auto* identityText = new QVBoxLayout();
    identityText->setContentsMargins(0, 0, 0, 0);
    identityText->setSpacing(theme::kSpacingXs);
    detailsTitle_ = new QLabel(metaHost_);
    detailsTitle_->setObjectName(QStringLiteral("titleDetailsTitle"));
    detailsTitle_->setTextFormat(Qt::PlainText);
    detailsTitle_->setCursor(Qt::PointingHandCursor);
    detailsTitle_->setFocusPolicy(Qt::StrongFocus);
    detailsTitle_->setToolTip(tr("Copy title"));
    detailsTitle_->installEventFilter(this);
    wrapLabel(detailsTitle_);
    detailsOriginal_ = new QLabel(metaHost_);
    detailsOriginal_->setObjectName(QStringLiteral("muted"));
    detailsOriginal_->setTextFormat(Qt::PlainText);
    detailsOriginal_->setCursor(Qt::PointingHandCursor);
    detailsOriginal_->setFocusPolicy(Qt::StrongFocus);
    detailsOriginal_->setToolTip(tr("Copy title"));
    detailsOriginal_->installEventFilter(this);
    wrapLabel(detailsOriginal_);
    detailsScore_ = new QLabel(metaHost_);
    detailsScore_->setObjectName(QStringLiteral("titleScores"));
    wrapLabel(detailsScore_);
    detailsScore_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    identityText->addWidget(detailsTitle_);
    identityText->addWidget(detailsOriginal_);
    identityText->addWidget(detailsScore_);
    identityText->addStretch(1);
    identity->addWidget(detailsThumb_, 0, Qt::AlignTop);
    identity->addLayout(identityText, 1);
    detailsContent->addLayout(identity);

    detailsFactsHost_ = new QWidget(metaHost_);
    detailsFactsHost_->setObjectName(QStringLiteral("titleDetailsFacts"));
    detailsFactsLayout_ = new QVBoxLayout(detailsFactsHost_);
    detailsFactsLayout_->setContentsMargins(0, 0, 0, 0);
    detailsFactsLayout_->setSpacing(theme::kSpacingLarge);
    detailsContent->addWidget(detailsFactsHost_);
    detailsContent->addStretch(1);

    detailsScroll_->setWidget(metaHost_);
    panelLayout->addWidget(detailsScroll_, 1);
}

void TitlePage::applyResponsiveLayout() {
    if (applyingLayout_ || columnLayout_ == nullptr || heroLayout_ == nullptr ||
        detailsOverlay_ == nullptr) {
        return;
    }
    applyingLayout_ = true;
    const bool compact = width() > 0 && width() < theme::kTitleCompactBreakpoint;
    const bool narrow = width() > 0 && width() < theme::kTitleHeroStackBreakpoint;
    const bool stackedHero = narrow;
    heroLayout_->setDirection(stackedHero ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    heroLayout_->setSpacing(stackedHero ? theme::kSpacingMedium
                                        : (compact ? theme::kSpacingLarge
                                                   : theme::kTitleHeroSpacing));
    const Qt::Alignment posterAlignment =
        stackedHero ? Qt::Alignment(Qt::AlignHCenter)
                    : Qt::Alignment(Qt::AlignLeft | Qt::AlignTop);
    heroLayout_->setAlignment(heroPosterHost_, posterAlignment);
    studio_->setAlignment((stackedHero ? Qt::AlignHCenter : Qt::AlignLeft) | Qt::AlignTop);
    // Both properties are written before the one restyle they need: a crossing
    // of both thresholds is one repolish pass, not two.
    bool restyleHeading = false;
    if (compact != compact_) {
        compact_ = compact;
        // Theme-owned type sizes key off this property, so the switch has to
        // re-resolve the scoped sheet rather than set fonts here.
        setProperty("compact", compact);
        restyleHeading = true;
        heroLayout_->setContentsMargins(0, 0, 0, 0);
        const int coverWidth = narrow ? theme::kTitleNarrowCoverWidth
                                      : compact ? theme::kTitleCompactCoverWidth
                                                : theme::kTitleHeroCoverWidth;
        const int coverHeight = narrow ? theme::kTitleNarrowCoverHeight
                                       : compact ? theme::kTitleCompactCoverHeight
                                                 : theme::kTitleHeroCoverHeight;
        cover_->setFixedSize(coverWidth, coverHeight);
        heroPosterHost_->setFixedWidth(coverWidth);
        if (coverState_ == CoverState::Ready && !coverPixmap_.isNull()) {
            applyCoverPixmap();
        }
    }
    const int responsiveCoverWidth = narrow ? theme::kTitleNarrowCoverWidth
                                            : compact ? theme::kTitleCompactCoverWidth
                                                      : theme::kTitleHeroCoverWidth;
    const int responsiveCoverHeight = narrow ? theme::kTitleNarrowCoverHeight
                                             : compact ? theme::kTitleCompactCoverHeight
                                                       : theme::kTitleHeroCoverHeight;
    if (cover_ != nullptr &&
        cover_->size() != QSize(responsiveCoverWidth, responsiveCoverHeight)) {
        cover_->setFixedSize(responsiveCoverWidth, responsiveCoverHeight);
        heroPosterHost_->setFixedWidth(responsiveCoverWidth);
        if (coverState_ == CoverState::Ready && !coverPixmap_.isNull()) {
            applyCoverPixmap();
        }
    }
    if (narrow_ != narrow) {
        narrow_ = narrow;
        setProperty("narrow", narrow);
        restyleHeading = true;
    }
    if (restyleHeading) repolishTitleSubtree();
    const int inset = compact ? theme::kTitlePageCompactInset : theme::kTitlePageInset;
    columnLayout_->setContentsMargins(inset, theme::kTitlePageInset, inset,
                                      theme::kTitlePageInset);
    if (headerOuterLayout_ != nullptr) {
        // Whole pixels only: the header row is capped, so an odd surplus leaves
        // the extra pixel on the right rather than widening the row past the cap.
        const int side = qMax(0, (width() - theme::kTitlePageMaxWidth) / 2);
        headerOuterLayout_->setContentsMargins(side, 0, side, 0);
    }
    if (headerRowLayout_ != nullptr) {
        headerRowLayout_->setContentsMargins(inset, theme::kSpacingMedium, inset,
                                             theme::kSpacingMedium);
    }
    if (watchLayout_ != nullptr) {
        watchLayout_->setDirection(compact ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
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
    refreshDescriptionPreview();
    updateBackdrop();
    updateDetailsGeometry();
    applyingLayout_ = false;
    // A width change can change the pill count per row, and with it the height.
    scheduleFit();
}

void TitlePage::updateBackdrop() {
    if (backdrop_ == nullptr || column_ == nullptr) {
        return;
    }
    if (hero_ != nullptr) {
        backdrop_->setGeometry(hero_->rect());
        backdrop_->lower();
    }
}

void TitlePage::updateDetailsGeometry() {
    if (detailsOverlay_ == nullptr) {
        return;
    }
    // A resize or a reopen settles the panel where it belongs: a slide still
    // running toward an older geometry is stopped first.
    const auto slides = findChildren<QPropertyAnimation*>(QString(), Qt::FindDirectChildrenOnly);
    for (QPropertyAnimation* slide : slides) {
        if (slide->targetObject() == detailsPanel_) {
            slide->stop();
        }
    }
    detailsOverlay_->setGeometry(rect());
    if (detailsScrim_ != nullptr) {
        detailsScrim_->setGeometry(detailsOverlay_->rect());
    }
    if (detailsPanel_ == nullptr) {
        return;
    }
    const int available = detailsOverlay_->width();
    const int panelWidth = qMin(theme::kTitleDetailsWidth, available);
    detailsPanel_->setGeometry(available - panelWidth, 0, panelWidth,
                               detailsOverlay_->height());
    detailsPanel_->raise();
}

void TitlePage::repolishTitleSubtree() {
    QStyle* pageStyle = style();
    if (pageStyle == nullptr) {
        return;
    }
    // Despite the name kept from the first pass, the scoped sheet carries
    // exactly two rules keyed on `compact`/`narrow` and both restyle the
    // heading's font size: re-resolving every descendant re-polished the whole
    // release grid on each width crossing, and these two widgets are the entire
    // effect.
    pageStyle->unpolish(this);
    pageStyle->polish(this);
    int polished = 1;
    if (heading_ != nullptr) {
        pageStyle->unpolish(heading_);
        pageStyle->polish(heading_);
        heading_->updateGeometry();
        ++polished;
    }
    // Counted, not assumed: a page that somehow has no heading repolished one
    // widget, and this counter is the report's evidence for the per-crossing cost.
    mikai::gui::uiprofile::noteTitleRepolishWidgets(polished);
    update();
}

void TitlePage::load() {
    setStatusKind(BaseStatus::Loading);
    failure_->clear();
    hasFailure_ = false;
    // A task can outlive the page: the worker lambda runs on a pool thread, and
    // the delivery lambdas are queued to the GUI thread, so both may fire after
    // the widget is gone. The worker captures the client and ref by value (the
    // client outlives the page), and the delivery lambdas hold a QPointer that
    // goes null on destruction.
    backend::MikaiClient* client = &client_;
    const QString ref = ref_;
    const QPointer<TitlePage> self(this);
    if (hasDetailHint_) {
        const backend::AnimeDetail detail = detailHint_;
        runner_.run(
            [client, ref] { return QVariant::fromValue(client->getPlayer(ref)); },
            [self, detail](const QVariant& result) {
                if (self.isNull() || self->closed_) {
                    return;
                }
                self->onLoaded(detail, result.value<backend::PlayerResult>());
            },
            [self](const backend::Failure& failure) {
                if (!self.isNull() && !self->closed_) {
                    self->onError(failure);
                }
            });
        return;
    }
    const auto state = std::make_shared<LoadState>();
    runner_.run(
        [client, ref] { return QVariant::fromValue(client->getAnime(ref)); },
        [self, state](const QVariant& result) {
            if (self.isNull() || self->closed_) {
                return;
            }
            state->detail = result.value<backend::AnimeDetail>();
            state->hasDetail = true;
            self->joinLoad(state);
        },
        [self, state](const backend::Failure& failure) {
            if (self.isNull() || self->closed_ || !state->error.isEmpty()) {
                return;
            }
            state->error = backend::failureLine(failure);
            self->onError(failure);
        });
    runner_.run(
        [client, ref] { return QVariant::fromValue(client->getPlayer(ref)); },
        [self, state](const QVariant& result) {
            if (self.isNull() || self->closed_) {
                return;
            }
            state->player = result.value<backend::PlayerResult>();
            state->hasPlayer = true;
            self->joinLoad(state);
        },
        [self, state](const backend::Failure& failure) {
            if (self.isNull() || self->closed_ || !state->error.isEmpty()) {
                return;
            }
            state->error = backend::failureLine(failure);
            self->onError(failure);
        });
}

/// Hand the page its title once both halves of a two-task load have landed.
void TitlePage::joinLoad(const std::shared_ptr<LoadState>& state) {
    if (!state->hasDetail || !state->hasPlayer || closed_) {
        return;
    }
    const backend::AnimeDetail detail = state->detail;
    const backend::PlayerResult player = state->player;
    onLoaded(detail, player);
}

void TitlePage::onLoaded(const backend::AnimeDetail& detail, const backend::PlayerResult& player) {
    adultConfirmed_ = false;
    // A newly loaded title has no choice yet: the preferred source opens, and
    // only the viewer's own clicks override it from here.
    chosenProvider_.clear();
    rateLabel_->refresh();
    failure_->clear();
    hasFailure_ = false;
    const QString name = labels::titleName(detail.titles);
    heading_->setText(name == QLatin1String("?") ? ref_ : name);
    QString alternate = detail.titles.english;
    if (alternate.isEmpty() || alternate.compare(heading_->text(), Qt::CaseInsensitive) == 0) {
        alternate = detail.titles.original;
    }
    if (alternate.compare(heading_->text(), Qt::CaseInsensitive) == 0) {
        alternate.clear();
    }
    subtitle_->setText(alternate);
    subtitle_->setVisible(!alternate.isEmpty());
    scheduleOverviewReflow();
    // Retained before the sheet is restated: its Synopsis block reads this text.
    rawDescription_ = detail.description.trimmed();
    fullDescription_ = rawDescription_.isEmpty() ? tr("No description available.") : rawDescription_;
    populateDetail(detail);
    const QString cover = backend::coverUrl(detail);
    Q_EMIT recentTitleLoaded(detail.ref.isEmpty() ? ref_ : detail.ref, heading_->text(),
                             cover.isEmpty() ? posterHint_ : cover);
    loadRecommendations(detail);

    descriptionExpanded_ = false;
    description_->setText(descriptionPreview());
    descriptionButton_->setText(tr("Read more"));
    refreshDescriptionPreview();

    if (!cover.isEmpty()) {
        loadCover(cover, true);
    } else if (!posterHint_.isEmpty()) {
        loadCover(posterHint_, false);
    } else {
        coverState_ = CoverState::Missing;
        cover_->setText(tr("No cover"));
        detailsThumb_->setText(tr("No cover"));
    }

    const bool playbackRestricted = detail.licensed || player.licensed;
    releases_ = playbackRestricted ? QVector<backend::Release>()
                                   : backend::sortReleases(player.releases);
    releaseProbeSeq_ += 1;
    releaseQuality_.clear();
    releaseProbed_.clear();
    rebuildReleaseChips();

    watchEntry_ = nullptr;
    updateWatchPanel();
    player_ = player;
    updateLicenseInfo(player_);
    if (releases_.isEmpty()) {
        {
            const QSignalBlocker blocker(tabs_);
            while (tabs_->count() > 0) {
                QWidget* page = tabs_->widget(0);
                tabs_->removeTab(0);
                page->deleteLater();
            }
        }
        tabStates_.clear();
        activeRelease_ = -1;
        providerSelectorHeading_->hide();
        tabs_->hide();
        episodeLegend_->hide();
        updateEpisodeSummary();
        setStatusKind(playbackRestricted ? BaseStatus::Licensed : BaseStatus::NoReleases);
        return;
    }
    // Resolve opening release & provider deterministically:
    // shared deep link > per-title favourite team > preferred provider > first available.
    selection::SelectionInput selIn;
    selIn.releases = releases_;
    selIn.currentEpisode = watchHistory_.current(ref_);
    selIn.defaultProvider = labels::preferredProvider();
    if (const auto fav = favouriteTeams_.favourite(ref_)) {
        selIn.favouriteProvider = fav->provider;
        selIn.favouriteTeamSlug = fav->teamSlug;
    }
    if (pendingPlaybackLink_.has_value() && pendingPlaybackLink_->ref == ref_ &&
        pendingPlaybackLink_->hasPlaybackTarget()) {
        selIn.sharedProvider = pendingPlaybackLink_->provider;
        selIn.sharedTeamSlug = pendingPlaybackLink_->teamSlug;
        selIn.sharedEpisode = pendingPlaybackLink_->episode;
        selIn.sharedTimeSec = pendingPlaybackLink_->timeSec;
    }
    pendingPlaybackLink_.reset();

    const selection::SelectionResult resolved = selection::resolve(selIn);
    const int openingRelease =
        (resolved.valid && resolved.releaseIndex >= 0 && resolved.releaseIndex < releases_.size())
            ? resolved.releaseIndex
            : labels::preferredReleaseIndex(releases_, labels::preferredProvider());
    if (resolved.valid && !resolved.provider.isEmpty()) {
        chosenProvider_ = resolved.provider;
    }
    resolvedOpeningEpisode_ = resolved.valid ? resolved.episodeNumber : QString();
    resolvedOpeningTimeEpisode_ =
        (resolved.valid && resolved.startTimeSec.has_value()) ? resolved.episodeNumber : QString();
    resolvedOpeningTimeSec_ = resolved.valid ? resolved.startTimeSec : std::nullopt;
    releaseChips_.at(openingRelease)->setChecked(true);
    fillEpisodes(openingRelease);
}

void TitlePage::onError(const backend::Failure& failure) {
    {
        const QSignalBlocker blocker(tabs_);
        while (tabs_->count() > 0) {
            QWidget* page = tabs_->widget(0);
            tabs_->removeTab(0);
            page->deleteLater();
        }
    }
    tabStates_.clear();
    activeRelease_ = -1;
    chosenProvider_.clear();
    releases_.clear();
    rebuildReleaseChips();
    providerSelectorHeading_->hide();
    updateEpisodeSummary();
    tabs_->hide();
    episodeLegend_->hide();
    watchEntry_ = nullptr;
    updateWatchPanel();
    rateLabel_->refresh();
    setStatus(backend::failureLine(failure));
    hasFailure_ = true;
    failure_->showFailure(failure, tr("Could not open this title"));
    cover_->setZoomable(false);
    coverState_ = CoverState::Missing;
    cover_->setText(tr("No cover"));
    detailsThumb_->setText(tr("No cover"));
}

void TitlePage::populateDetail(const backend::AnimeDetail& detail) {
    // Retained for the language switches that follow: every caption the page
    // derives from the title is derived from this copy, never from a new call.
    detail_ = detail;
    hasDetail_ = true;
    populateRelated(detail);
    refreshDetailTexts();
}

/// Restates the facts, scores, details rows and links from the retained detail.
/// Run once when the detail lands and again after each language switch: the
/// rows are rebuilt from the model, and nothing outside the metadata grid and
/// the facts/score/link labels is touched.
void TitlePage::refreshDetailTexts() {
    const QString heroScores = scoresText(detail_);
    scores_->setText(heroScores);
    scores_->setVisible(!heroScores.isEmpty());
    disclosureRow_->setVisible(true);
    metaButton_->show();

    // The hero caption and the sheet read one model; both hide what it lacks.
    studio_->setText(detail_.studio);
    studio_->setVisible(!detail_.studio.isEmpty());
    const QString detailName = labels::titleName(detail_.titles);
    const QString visibleTitle = detailName == QLatin1String("?") ? ref_ : detailName;
    heading_->setText(visibleTitle);
    QString alternate = detail_.titles.english;
    if (alternate.isEmpty() || alternate.compare(visibleTitle, Qt::CaseInsensitive) == 0) {
        alternate = detail_.titles.original;
    }
    if (alternate.compare(visibleTitle, Qt::CaseInsensitive) == 0) {
        alternate.clear();
    }
    subtitle_->setText(alternate);
    subtitle_->setVisible(!alternate.isEmpty());
    detailsTitle_->setText(visibleTitle);
    const QString originalTitle =
        detail_.titles.original.isEmpty() ? detail_.titles.english : detail_.titles.original;
    detailsOriginal_->setText(originalTitle);
    detailsOriginal_->setVisible(!originalTitle.isEmpty());
    const QString detailScores = scoresText(detail_);
    detailsScore_->setText(detailScores);
    detailsScore_->setVisible(!detailScores.isEmpty());

    while (detailsFactsLayout_->count() > 0) {
        QLayoutItem* item = detailsFactsLayout_->takeAt(0);
        if (item->widget() != nullptr) {
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }

    const backend::EpisodeStats& episodes = detail_.episodes;
    const int total = episodes.total.value_or(0);
    const QString totalText = episodes.total.has_value() && total > 0
                                  ? QString::number(total)
                                  : tr("unknown");
    const QString episodesLabel =
        tr("Aired: %1 · Localized: %2 · Total: %3")
            .arg(episodes.aired)
            .arg(episodes.localized)
            .arg(totalText);
    episodeAvailability_->setText(episodesLabel);
    episodeAvailability_->setAccessibleName(tr("Episode availability"));
    episodeAvailability_->show();

    QString airingText;
    if (detail_.next_episode.has_value()) {
        const QDateTime airing = QDateTime::fromString(detail_.next_episode->airing, Qt::ISODate);
        const QString episode = detail_.next_episode->episode.toString();
        if (airing.isValid()) {
            const QString localTime = QLocale().toString(airing.toLocalTime(), QLocale::ShortFormat);
            airingText = episode.isEmpty()
                             ? tr("Next episode airing %1 (local time)").arg(localTime)
                             : tr("Episode %1 airs %2 (local time)").arg(episode, localTime);
        } else if (!detail_.next_episode->airing.isEmpty()) {
            airingText = episode.isEmpty()
                             ? tr("Next airing: %1").arg(detail_.next_episode->airing)
                             : tr("Episode %1 airing: %2")
                                   .arg(episode, detail_.next_episode->airing);
        }
    }
    nextAiring_->setText(airingText);
    nextAiring_->setVisible(!airingText.isEmpty());

    const QString statusText = labels::status(detail_.status);
    const QString formatText = labels::format(detail_.format);
    const QString seasonText = labels::season(detail_.start_date);
    QStringList facts;
    if (!statusText.isEmpty()) facts << statusText;
    if (!formatText.isEmpty()) facts << formatText;
    if (!seasonText.isEmpty()) facts << seasonText;
    facts_->setText(facts.join(QStringLiteral(" · ")));
    fillHeroChips(hero_->findChild<QWidget*>(QStringLiteral("titleGenres")), detail_, this);

    struct DetailRow {
        QString title;
        QString value;
        backend::AnimeQuery criteria;
        bool episodes = false;
    };
    QVector<DetailRow> rows;
    rows.append({tr("Episode availability"), episodesLabel, {}, true});
    if (!formatText.isEmpty()) {
        backend::AnimeQuery criteria;
        criteria.formats = {detail_.format};
        rows.append({tr("Type"), formatText, criteria});
    }
    if (episodes.duration_min.value_or(0) > 0)
        rows.append({tr("Duration"), tr("%1 min").arg(*episodes.duration_min), {}});
    if (!statusText.isEmpty()) {
        backend::AnimeQuery criteria;
        criteria.statuses = {detail_.status};
        rows.append({tr("Status"), statusText, criteria});
    }
    if (!seasonText.isEmpty()) rows.append({tr("Season"), seasonText, {}});
    if (!detail_.source.isEmpty())
        rows.append({tr("Original source"), labels::source(detail_.source), {}});
    if (!detail_.studio.isEmpty()) {
        backend::AnimeQuery criteria;
        criteria.studios = {detail_.studio};
        rows.append({tr("Studio"), detail_.studio, criteria});
    }
    if (!detail_.country.isEmpty())
        rows.append({tr("Country"), labels::country(detail_.country), {}});
    if (!detail_.age_rating.isEmpty())
        rows.append({tr("Age rating"), labels::ageRating(detail_.age_rating), {}});

    // Two facts per row, label above value, all of them built from the model.
    auto* gridHost = new QWidget(detailsFactsHost_);
    auto* grid = new QGridLayout(gridHost);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(theme::kSpacingMedium);
    grid->setVerticalSpacing(theme::kSpacingLarge);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    int index = 0;
    for (const auto& entry : rows) {
        auto* cell = new QWidget(gridHost);
        auto* cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(0, 0, 0, 0);
        cellLayout->setSpacing(theme::kSpacingXs);
        auto* key = new QLabel(entry.title.toUpper(), cell);
        key->setObjectName(QStringLiteral("muted"));
        key->setWordWrap(true);
        cellLayout->addWidget(key);
        const bool searchable = !entry.criteria.formats.isEmpty() ||
                                !entry.criteria.statuses.isEmpty() ||
                                !entry.criteria.studios.isEmpty();
        if (entry.episodes) {
            auto* value = new QLabel(entry.value, cell);
            value->setObjectName(QStringLiteral("titleDetailsValue"));
            wrapLabel(value);
            value->setTextFormat(Qt::PlainText);
            value->setTextInteractionFlags(Qt::TextSelectableByMouse);
            cellLayout->addWidget(value);
            auto* button = new QPushButton(tr("Episodes"), cell);
            button->setObjectName(QStringLiteral("titleDetailsAction"));
            button->setCursor(Qt::PointingHandCursor);
            button->setToolTip(tr("Jump to episodes"));
            connect(button, &QPushButton::clicked, this, [this] {
                setDetailsOpen(false);
                scrollToEpisodes();
            });
            cellLayout->addWidget(button, 0, Qt::AlignLeft);
        } else if (searchable) {
            auto* value = new QLabel(cell);
            value->setObjectName(QStringLiteral("titleDetailsValue"));
            value->setTextFormat(Qt::RichText);
            value->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
            value->setText(QStringLiteral("<a href=\"browse\" style=\"color: %1;\">%2</a>")
                               .arg(theme::paletteFor(currentAppearance()).accent,
                                    entry.value.toHtmlEscaped()));
            value->setWordWrap(true);
            value->setCursor(Qt::PointingHandCursor);
            value->setToolTip(tr("Show titles in %1").arg(entry.value));
            connect(value, &QLabel::linkActivated, this,
                    [this, criteria = entry.criteria](const QString&) {
                        emit catalogueRequested(criteria);
                    });
            cellLayout->addWidget(value);
        } else {
            auto* value = new QLabel(entry.value, cell);
            value->setObjectName(QStringLiteral("titleDetailsValue"));
            wrapLabel(value);
            value->setTextFormat(Qt::PlainText);
            value->setTextInteractionFlags(Qt::TextSelectableByMouse);
            cellLayout->addWidget(value);
        }
        grid->addWidget(cell, index / 2, index % 2, Qt::AlignTop);
        ++index;
    }
    detailsFactsLayout_->addWidget(gridHost);

    const auto addTopics = [this](const QString& caption,
                                  const QVector<backend::GenreTag>& topics, bool tags) {
        auto* section = new QWidget(detailsFactsHost_);
        auto* sectionLayout = new QVBoxLayout(section);
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->setSpacing(theme::kSpacingSmall);
        auto* heading = new QLabel(caption.toUpper(), section);
        heading->setObjectName(QStringLiteral("muted"));
        sectionLayout->addWidget(heading);
        auto* chips = new QWidget(section);
        chips->setLayout(new FlowLayout(theme::kSpacingSmall, theme::kSpacingXs));
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        policy.setHeightForWidth(true);
        chips->setSizePolicy(policy);
        int count = 0;
        for (const backend::GenreTag& topic : topics) {
            const QString text = topic.ua.isEmpty() ? topic.name : topic.ua;
            if (text.isEmpty()) continue;
            if (topic.name.isEmpty()) {
                auto* label = new QLabel(text, chips);
                label->setObjectName(QStringLiteral("titleDetailsValue"));
                chips->layout()->addWidget(label);
            } else {
                auto* button = new QPushButton(text, chips);
                button->setObjectName(QStringLiteral("titleDetailsChip"));
                button->setCursor(Qt::PointingHandCursor);
                button->setToolTip(tr("Show titles in %1").arg(text));
                backend::AnimeQuery criteria;
                if (tags) criteria.tags = {topic.name};
                else criteria.genres = {topic.name};
                connect(button, &QPushButton::clicked, this,
                        [this, criteria] { emit catalogueRequested(criteria); });
                chips->layout()->addWidget(button);
            }
            ++count;
        }
        if (count == 0) {
            delete section;
            return;
        }
        sectionLayout->addWidget(chips);
        detailsFactsLayout_->addWidget(section);
    };
    addTopics(tr("Genres"), detail_.genres, false);
    addTopics(tr("Tags"), detail_.tags, true);

    auto* synopsisHost = new QWidget(detailsFactsHost_);
    auto* synopsisLayout = new QVBoxLayout(synopsisHost);
    synopsisLayout->setContentsMargins(0, 0, 0, 0);
    synopsisLayout->setSpacing(theme::kSpacingXs);
    auto* synopsisKey = new QLabel(tr("Synopsis").toUpper(), synopsisHost);
    synopsisKey->setObjectName(QStringLiteral("muted"));
    auto* synopsis = new QLabel(fullDescription_.isEmpty() ? tr("No description available.")
                                                           : fullDescription_,
                               synopsisHost);
    synopsis->setObjectName(QStringLiteral("titleDetailsValue"));
    wrapLabel(synopsis);
    synopsis->setTextFormat(Qt::PlainText);
    synopsis->setTextInteractionFlags(Qt::TextSelectableByMouse);
    synopsisLayout->addWidget(synopsisKey);
    synopsisLayout->addWidget(synopsis);
    detailsFactsLayout_->addWidget(synopsisHost);

    if (coverPixmap_.isNull()) {
        detailsThumb_->setText(tr("No cover"));
    }

    refreshLinkTexts();
}

void TitlePage::refreshLinkTexts() {
    if (links_ == nullptr || !hasDetail_) {
        return;
    }
    QVector<std::pair<QString, QString>> external = backend::externalLinks(detail_);
    const QString page =
        backend::animePageUrl(detail_.ref, detail_.ids.value(QStringLiteral("slug")).toString());
    if (!detail_.ref.isEmpty()) {
        external.prepend({tr("Mikai"), page});
    }
    QLayout* layout = links_->layout();
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (item->widget() != nullptr) {
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }
    const theme::Palette palette = theme::paletteFor(currentAppearance());
    for (int index = 0; index < external.size(); ++index) {
        if (index > 0) {
            auto* separator = new QLabel(QStringLiteral("·"), links_);
            separator->setObjectName(QStringLiteral("titleLinks"));
            layout->addWidget(separator);
        }
        const auto& link = external.at(index);
        auto* label = new QLabel(links_);
        label->setObjectName(QStringLiteral("titleLinks"));
        label->setTextFormat(Qt::RichText);
        label->setOpenExternalLinks(true);
        label->setCursor(Qt::PointingHandCursor);
        auto* glow = new QGraphicsDropShadowEffect(label);
        glow->setBlurRadius(10);
        glow->setOffset(0);
        glow->setColor(QColor(palette.accent));
        glow->setEnabled(false);
        label->setGraphicsEffect(glow);
        const QString url = link.second.toHtmlEscaped();
        const QString text = link.first.toHtmlEscaped();
        const auto render = [label, glow, url, text, palette](bool hovered) {
            const QString& color = hovered ? palette.accent : palette.titleMuted;
            label->setText(QStringLiteral("<a style=\"color: %1; text-decoration: underline; font-weight: 400;\" href=\"%2\">%3</a>")
                               .arg(color, url, text));
            glow->setEnabled(hovered);
        };
        render(false);
        connect(label, &QLabel::linkHovered, label,
                [render](const QString& target) { render(!target.isEmpty()); });
        layout->addWidget(label);
    }
}

void TitlePage::loadCover(const QString& url, bool allowHint) {
    // A fetch is in flight, so a language switch landing before its callback
    // re-words the cover to its loading text rather than leaving stale wording.
    coverState_ = CoverState::Loading;
    // The client outlives the page, so the worker copies the address and the
    // callbacks hold a QPointer: an image landing after the page was disposed
    // is dropped instead of dereferencing freed widget state.
    backend::MikaiClient* client = &client_;
    const QPointer<TitlePage> self(this);
    runner_.runImage(
        [client, url] {
            return QVariant::fromValue(gui::decodeImage(client->fetchBytes(url)));
        },
        [self, url, allowHint](const QVariant& result) {
            if (self.isNull() || self->closed_) return;
            auto decoded = result.value<gui::DecodedImage>();
            if (decoded.image.isNull()) {
                self->onCoverFailed(url, allowHint, decoded.error);
                return;
            }
            self->coverState_ = CoverState::Ready;
            self->coverPixmap_ = QPixmap::fromImage(std::move(decoded.image));
            self->coverAppliedSize_ = {};
            self->coverUrl_ = url;
            self->cover_->setZoomable(true);
            self->cover_->setToolTip(
                QCoreApplication::translate("TitlePage", "Click to open the cover"));
            self->cover_->setText(QString());
            self->applyCoverPixmap();
        },
        [self, url, allowHint](const backend::Failure& failure) {
            if (!self.isNull() && !self->closed_) {
                self->onCoverFailed(url, allowHint, backend::failureLine(failure));
            }
        });
}

void TitlePage::onCoverFailed(const QString& url, bool allowHint, const QString& reason) {
    qWarning().noquote() << "TitlePage: cover failed:" << ref_ << reason;
    if (allowHint && !posterHint_.isEmpty() && posterHint_ != url) {
        loadCover(posterHint_, false);
        return;
    }
    coverState_ = CoverState::Missing;
    coverPixmap_ = {};
    coverAppliedSize_ = {};
    coverUrl_.clear();
    cover_->setZoomable(false);
    const QString missing = QCoreApplication::translate("TitlePage", "No cover");
    cover_->setText(missing);
    detailsThumb_->setText(missing);
    if (backdrop_ != nullptr) backdrop_->setPoster({});
}

void TitlePage::applyCoverPixmap() {
    if (coverPixmap_.isNull()) {
        return;
    }
    // Rounded copies are produced on a size or ratio change, never on a paint.
    // Each frame carries the radius its own QSS surface declares: the hero cover
    // at the poster radius, the details thumbnail at the thumbnail radius.
    const QSize posterSize = cover_->size();
    const qreal dpr = cover_->devicePixelRatioF();
    if (posterSize != coverAppliedSize_ || !qFuzzyCompare(dpr, coverAppliedDpr_)) {
        cover_->setPixmap(TitleCard::rounded(
            coverPixmap_, posterSize, dpr,
            theme::scaledRadius(theme::kTitlePosterRadius, currentAppearance()), true));
        coverAppliedSize_ = posterSize;
        coverAppliedDpr_ = dpr;
    }
    if (detailsThumb_ != nullptr) {
        detailsThumb_->setText(QString());
        detailsThumb_->setPixmap(TitleCard::rounded(
            coverPixmap_, detailsThumb_->size(), detailsThumb_->devicePixelRatioF(),
            theme::scaledRadius(theme::kTitleDetailsThumbRadius, currentAppearance()), true));
    }
    if (backdrop_ != nullptr) {
        backdrop_->setPoster(coverPixmap_);
    }
}

void TitlePage::coverContextMenu(const QPoint& globalPos) {
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("titleContextMenu"));
    QAction* copyUrl = menu.addAction(tr("Copy cover URL"));
    copyUrl->setToolTip(tr("Copy image address"));
    copyUrl->setEnabled(!coverUrl_.isEmpty());
    connect(copyUrl, &QAction::triggered, this, [this] { copyCoverUrl(); });
    QAction* saveAction = menu.addAction(tr("Save cover…"));
    saveAction->setToolTip(tr("Save image to file"));
    saveAction->setEnabled(!coverPixmap_.isNull());
    connect(saveAction, &QAction::triggered, this, [this] { saveCover(); });
    menu.exec(globalPos);
}

void TitlePage::copyCoverUrl() {
    if (coverUrl_.isEmpty()) {
        return;
    }
    if (!mikai::gui::copyText(coverUrl_)) {
        flashStatus(tr("Failed to copy cover URL"), true);
        return;
    }
    flashStatus(tr("Cover URL copied"));
}

void TitlePage::saveCover() {
    if (coverPixmap_.isNull()) {
        return;
    }
    const QString suffix =
        coverUrl_.endsWith(QStringLiteral(".webp"), Qt::CaseInsensitive) ? QStringLiteral("webp")
                                                                          : QStringLiteral("png");
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save cover"), tr("cover.%1").arg(suffix),
        tr("Images (*.png *.jpg *.webp)"));
    if (path.isEmpty()) {
        return;
    }
    if (coverPixmap_.save(path)) {
        flashStatus(tr("Cover saved: %1").arg(path));
    } else {
        flashStatus(tr("Failed to save cover"));
    }
}

void TitlePage::openCoverViewer() {
    if (coverPixmap_.isNull()) {
        return;
    }
    if (coverViewer_ != nullptr) {
        coverViewer_->close();
        coverViewer_ = nullptr;
    }
    coverViewer_ = new CoverViewer(coverPixmap_, heading_->text(), this);
    coverViewer_->setAttribute(Qt::WA_DeleteOnClose);
    // X-M4: WA_DeleteOnClose destruction is deferred (deleteLater), so the
    // old viewer's destroyed must not clear the pointer that now tracks a
    // new one — capture the viewer and compare the sender.
    connect(coverViewer_, &QObject::destroyed, this,
            [this, viewer = coverViewer_](QObject* gone) {
                if (gone == viewer) coverViewer_ = nullptr;
            });
    coverViewer_->show();
}

void TitlePage::setStatus(const QString& text) {
    // A line the app did not word itself (the backend's failure line): kept
    // verbatim, and restated verbatim on a language switch.
    statusKind_ = BaseStatus::Raw;
    baseStatus_ = text;
    status_->setText(text);
    setLabelState(status_, "error");
    if (episodeStatus_ != nullptr) {
        episodeStatus_->setText(text);
        episodeStatus_->setAccessibleName(tr("Episode status"));
        setLabelState(episodeStatus_, "error");
        episodeStatus_->show();
    }
}

void TitlePage::setStatusKind(BaseStatus kind) {
    statusKind_ = kind;
    baseStatus_ = baseStatusText(kind);
    status_->setText(baseStatus_);
    setLabelState(status_, "base");
    if (episodeStatus_ != nullptr) {
        const bool episodeProblem = kind == BaseStatus::Loading ||
                                    kind == BaseStatus::Licensed ||
                                    kind == BaseStatus::NoReleases ||
                                    kind == BaseStatus::NoSources;
        episodeStatus_->setText(baseStatus_);
        episodeStatus_->setAccessibleName(tr("Episode status"));
        setLabelState(episodeStatus_, "base");
        episodeStatus_->setVisible(episodeProblem);
    }
}

QString TitlePage::baseStatusText(BaseStatus kind) const {
    switch (kind) {
    case BaseStatus::Loading:
        return tr("Loading…");
    case BaseStatus::Licensed:
        return tr("Playback unavailable due to licensing");
    case BaseStatus::NoReleases:
        return tr("No releases available");
    case BaseStatus::NoSources:
        return tr("No sources in this release");
    case BaseStatus::Empty:
        return {};
    case BaseStatus::Raw:
        return baseStatus_;
    }
    return {};
}

QString TitlePage::relatedHeadingText(const QString& key) const {
    if (key == QLatin1String("related")) {
        return tr("Related titles");
    }
    if (key == QLatin1String("similar")) {
        return tr("Similar anime");
    }
    if (key == QLatin1String("recommendation")) {
        return tr("Recommended on Mikai");
    }
    return key;
}

void TitlePage::applyCoverFallback() {
    switch (coverState_) {
    case CoverState::Loading:
        cover_->setText(tr("Loading…"));
        break;
    case CoverState::Missing:
        cover_->setText(tr("No cover"));
        break;
    case CoverState::Ready:
        // The pixmap stays where it is; only the tooltip naming the click is
        // re-worded. The explicit context matches loadCover's lambda, where the
        // tooltip is first set, so both paths share one catalog entry.
        cover_->setToolTip(QCoreApplication::translate("TitlePage", "Click to open the cover"));
        break;
    }
    if (coverPixmap_.isNull()) {
        detailsThumb_->setText(tr("No cover"));
    }
}

void TitlePage::flashStatus(const QString& text, bool failure) {
    status_->setText(text);
    setLabelState(status_, failure ? "error" : "flash");
    if (failure) {
        QAccessibleEvent event(status_, QAccessible::Alert);
        QAccessible::updateAccessibility(&event);
    }
    QTimer::singleShot(kFlashMs, this, [this] {
        if (!closed_) {
            status_->setText(baseStatus_);
            setLabelState(status_, statusKind_ == BaseStatus::Raw ? "error" : "base");
        }
    });
}

void TitlePage::retranslateUi() {
    const int overviewScrollValue =
        overviewScroll_ != nullptr ? overviewScroll_->verticalScrollBar()->value() : 0;
    const int detailsScrollValue =
        detailsScroll_ != nullptr ? detailsScroll_->verticalScrollBar()->value() : 0;
    if (backButton_ != nullptr) {
        backButton_->setText(tr("Back"));
        backButton_->setAccessibleName(tr("Back"));
    }
    if (homeButton_ != nullptr) {
        homeButton_->setText(tr("Home"));
    }
    if (settingsButton_ != nullptr) {
        settingsButton_->setToolTip(tr("Settings"));
        settingsButton_->setAccessibleName(tr("Settings"));
    }
    if (episodesHeading_ != nullptr) {
        episodesHeading_->setText(tr("Episodes"));
    }
    if (releaseSelectorHeading_ != nullptr) {
        releaseSelectorHeading_->setText(tr("Release"));
    }
    if (providerSelectorHeading_ != nullptr) {
        providerSelectorHeading_->setText(tr("Provider"));
    }
    if (heading_ != nullptr) {
        heading_->setToolTip(tr("Copy title"));
        heading_->setAccessibleDescription(tr("Copy title"));
    }
    if (subtitle_ != nullptr) {
        subtitle_->setToolTip(tr("Copy title"));
        subtitle_->setAccessibleDescription(tr("Copy title"));
    }

    // A flash still on screen is left to run out: its own timer restores the
    // base line below, which is re-worded here.
    const bool showingBase = status_ != nullptr && status_->text() == baseStatus_;
    baseStatus_ = baseStatusText(statusKind_);
    if (showingBase) {
        status_->setText(baseStatus_);
    }
    if (episodeStatus_ != nullptr) {
        episodeStatus_->setAccessibleName(tr("Episode status"));
        if (statusKind_ != BaseStatus::Raw) {
            episodeStatus_->setText(baseStatus_);
            episodeStatus_->setVisible(statusKind_ == BaseStatus::Loading ||
                                       statusKind_ == BaseStatus::Licensed ||
                                       statusKind_ == BaseStatus::NoReleases ||
                                       statusKind_ == BaseStatus::NoSources);
        }
    }

    applyCoverFallback();

    if (hasDetail_) {
        // Re-worded before the sheet: its Synopsis block reads this retained text.
        fullDescription_ =
            rawDescription_.isEmpty() ? tr("No description available.") : rawDescription_;
        refreshDetailTexts();
        refreshDescriptionPreview();
    } else {
        refreshDescriptionPreview();
    }
    if (metaButton_ != nullptr) {
        metaButton_->setText(metaButton_->isChecked() ? tr("Hide details") : tr("More details"));
        metaButton_->setToolTip(metaButton_->isChecked() ? tr("Hide title details")
                                                         : tr("Show title details"));
        metaButton_->setAccessibleName(metaButton_->text());
        metaButton_->setVisible(hasDetail_);
    }
    if (relatedButton_ != nullptr) {
        relatedButton_->setMinimumHeight(40);
        relatedButton_->setText(relatedButton_->isChecked() ? tr("Hide") : tr("Show"));
    }
    if (discoveryHeading_ != nullptr) {
        discoveryHeading_->setText(tr("More to explore"));
    }
    reflowOverviewHeight();
    if (hasFailure_) {
        // The view re-words its own canonical line; only the context clause is
        // the page's. setContext restates just that: no re-show, no
        // re-classification and no second accessibility announcement.
        failure_->setContext(tr("Could not open this title"));
    }

    if (detailsHeading_ != nullptr) {
        detailsHeading_->setText(tr("Details"));
    }
    if (detailsClose_ != nullptr) {
        detailsClose_->setToolTip(tr("Close details"));
        detailsClose_->setAccessibleName(tr("Close details"));
    }
    if (detailsScrim_ != nullptr) {
        detailsScrim_->setAccessibleName(tr("Close details"));
    }
    if (watchButton_ != nullptr) {
        watchButton_->setText(tr("Watch"));
    }

    refreshRelatedTexts();
    refreshReleaseTexts();
    refreshEpisodeTexts();
    // The action captions changed, so every group is measured again from the
    // font it now renders in; a longer translation widens the group.
    refreshActionWidths();
    updateEpisodeSummary();
    if (hasDetail_) {
        updateLicenseInfo(player_);
    }
    updateWatchPanel();
    QTimer::singleShot(0, this, [this, overviewScrollValue, detailsScrollValue] {
        if (overviewScroll_ != nullptr) {
            QScrollBar* bar = overviewScroll_->verticalScrollBar();
            bar->setValue(qBound(bar->minimum(), overviewScrollValue, bar->maximum()));
        }
        if (detailsScroll_ != nullptr) {
            QScrollBar* bar = detailsScroll_->verticalScrollBar();
            bar->setValue(qBound(bar->minimum(), detailsScrollValue, bar->maximum()));
        }
    });
}

void TitlePage::refreshRelatedTexts() {
    for (const auto& heading : relatedHeadings_) {
        if (!heading.second.isNull()) {
            heading.second->setText(relatedHeadingText(heading.first));
        }
    }
    for (const auto& pager : relatedPagerButtons_) {
        if (!pager.first.isNull()) {
            pager.first->setToolTip(tr("Previous page"));
        }
        if (!pager.second.isNull()) {
            pager.second->setToolTip(tr("Next page"));
        }
    }
    for (const auto& kind : relatedKindLabels_) {
        if (!kind.second.isNull()) {
            kind.second->setText(labels::relation(kind.first));
        }
    }
    for (const QPointer<QLabel>& poster : relatedPosterLabels_) {
        if (poster.isNull()) {
            continue;
        }
        // A landed poster owns its label: setText would drop the pixmap it is
        // showing, so only a label still on its fallback is re-worded.
        if (poster->pixmap().isNull()) {
            poster->setText(tr("Poster unavailable"));
        }
    }
}

void TitlePage::refreshReleaseTexts() {
    // Releasing the fixed width first is what lets sizeHint() answer with the
    // width of the re-worded text instead of the width the previous language
    // clamped the label to.
    for (const auto& kind : releaseKindLabels_) {
        if (!kind.second.isNull()) {
            kind.second->setMinimumWidth(0);
            kind.second->setMaximumWidth(QWIDGETSIZE_MAX);
        }
    }
    int kindWidth = 0;
    for (const auto& kind : releaseKindLabels_) {
        if (kind.second.isNull()) {
            continue;
        }
        kind.second->setText(labels::kind(kind.first));
        kindWidth = qMax(kindWidth, kind.second->sizeHint().width());
    }
    for (const auto& kind : releaseKindLabels_) {
        if (!kind.second.isNull()) {
            kind.second->setFixedWidth(kindWidth);
        }
    }

    for (int index = 0; index < releaseChips_.size() && index < releases_.size(); ++index) {
        QPushButton* chip = releaseChips_.at(index);
        if (chip == nullptr) {
            continue;
        }
        QString label = releaseChipLabel(releases_.at(index));
        QString tooltip = releaseChipToolTip(releases_.at(index));
        // A probed quality is not a translation: it is appended again to the
        // re-worded label rather than dropped with the old text.
        const int quality = releaseQuality_.value(index, 0);
        if (quality > 0) {
            const QString suffix = QStringLiteral(" · %1p").arg(quality);
            label += suffix;
            tooltip += suffix;
        }
        setReleaseChipText(chip, label);
        chip->setToolTip(tooltip);
    }
    if (releaseChipsHost_ != nullptr) {
        if (auto* teamRow =
                releaseChipsHost_->findChild<QWidget*>(QStringLiteral("titleTeamsRow"))) {
            if (auto* teamLbl = teamRow->findChild<QLabel*>(QStringLiteral("muted"))) {
                teamLbl->setText(tr("Teams:"));
            }
            const auto teamButtons =
                teamRow->findChildren<QPushButton*>(QStringLiteral("categoryChip"));
            for (QPushButton* btn : teamButtons) {
                if (btn == nullptr) continue;
                const QString teamName = btn->text();
                btn->setToolTip(tr("Open %1 team profile and releases").arg(teamName));
                btn->setAccessibleName(tr("Browse team %1").arg(teamName));
            }
        }
    }
    refreshReleaseChipSizing();
}

void TitlePage::refreshEpisodeTexts() {
    if (tabs_ != nullptr) {
        for (QLineEdit* jump : tabs_->findChildren<QLineEdit*>(QStringLiteral("episodeJump"))) {
            jump->setPlaceholderText(tr("Episode number"));
            jump->setToolTip(tr("Jump to episode"));
        }
        for (QPushButton* filter : tabs_->findChildren<QPushButton*>(QStringLiteral("episodeTypeFilter"))) {
            const QString kind = filter->property("episodeType").toString();
            filter->setText(kind.isEmpty() ? tr("All") : episodeTypeCaption(kind));
        }
        for (QPushButton* all : tabs_->findChildren<QPushButton*>(QStringLiteral("episodeFullRange")))
            all->setText(tr("Show entire range"));
        for (int index = 0; index < tabStates_.size() && index < tabs_->count(); ++index) {
            const QString provider = tabStates_.at(index).provider;
            const QString known = labels::provider(provider);
            const QString name = known.isEmpty() ? capitalize(provider) : known;
            tabs_->setTabText(index, name);
            if (providerBar_ != nullptr && index < providerBar_->count())
                providerBar_->setTabText(index, name);
        }
    }
    // Labels are plain data: every entry carries one for the player's episode
    // list, whether or not its pill is built right now.
    for (EpisodeEntry* entry : ownedEntries_) {
        if (entry != nullptr) {
            entry->label = episodeCaption(entry->number);
            if (entry->typeBadge != nullptr) {
                const QString type = episodeType(entry);
                entry->typeBadge->setToolTip(episodeTypeCaption(type));
                entry->typeBadge->setAccessibleName(entry->typeBadge->toolTip());
            }
        }
    }
    // Tooltips only where a live pill exists. An entry whose page left its grid
    // still holds the pointer of a button already scheduled for deletion, so it
    // is skipped here and re-worded when that page is rendered again.
    for (const TabState& state : tabStates_) {
        if (!state.rendered) {
            continue;
        }
        for (EpisodeEntry* entry : state.displayed) {
            if (entry == nullptr || entry->button == nullptr) {
                continue;
            }
            // The tooltip and the accessible description both name the pill's
            // state in the new language; only the marks stay as they were.
            applyEpisodeState(entry);
        }
    }
    updateEpisodeLegend();
}


}  // namespace mikai::app
