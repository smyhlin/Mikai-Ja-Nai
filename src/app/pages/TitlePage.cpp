#include "pages/TitlePage.hpp"

#include "pages/EpisodeGrid.hpp"

#include "gui/Clipboard.hpp"
#include "Labels.hpp"
#include "dialogs/CoverViewer.hpp"
#include "state/NativePlaybackAssembly.hpp"
#include "state/QualityCache.hpp"
#include "theme/Colors.hpp"
#include "theme/Icons.hpp"
#include "theme/Metrics.hpp"
#include "widgets/RangePager.hpp"
#include "widgets/RateLimitLabel.hpp"
#include "workers/Runner.hpp"

#include <PlayerDialog.hpp>
#include <backend/MikaiClient.hpp>
#include <backend/Releases.hpp>
#include <backend/Streams.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>

#include <QAbstractButton>
#include <QAccessible>
#include <QButtonGroup>
#include <QtCore/QDebug>
#include <QDesktopServices>
#include <QEvent>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStyle>
#include <QPointer>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace mikai::app {
namespace {

constexpr int kFlashMs = 1200;

class ReleaseFlowLayout final : public QLayout {
public:
    ReleaseFlowLayout(int horizontalSpacing, int verticalSpacing)
        : horizontalSpacing_(horizontalSpacing), verticalSpacing_(verticalSpacing) {
        setContentsMargins(0, 0, 0, 0);
    }

    ~ReleaseFlowLayout() override {
        while (QLayoutItem* item = takeAt(0)) {
            delete item;
        }
    }

    void addItem(QLayoutItem* item) override { items_.append(item); }

    int count() const override { return static_cast<int>(items_.size()); }

    QLayoutItem* itemAt(int index) const override { return items_.value(index); }

    QLayoutItem* takeAt(int index) override {
        if (index < 0 || index >= items_.size()) {
            return nullptr;
        }
        return items_.takeAt(index);
    }

    Qt::Orientations expandingDirections() const override { return {}; }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override {
        return doLayout(QRect(0, 0, width, 0), true);
    }

    QSize sizeHint() const override { return minimumSize(); }

    QSize minimumSize() const override {
        QSize size;
        for (const QLayoutItem* item : items_) {
            size = size.expandedTo(item->minimumSize());
        }
        const QMargins margins = contentsMargins();
        return size + QSize(margins.left() + margins.right(),
                            margins.top() + margins.bottom());
    }

    void setGeometry(const QRect& rect) override {
        QLayout::setGeometry(rect);
        doLayout(rect, false);
    }

private:
    int doLayout(const QRect& rect, bool testOnly) const {
        const QMargins margins = contentsMargins();
        const QRect content = rect.adjusted(margins.left(), margins.top(),
                                            -margins.right(), -margins.bottom());
        int x = content.x();
        int y = content.y();
        int lineHeight = 0;
        const int right = content.x() + content.width();

        for (QLayoutItem* item : items_) {
            const QSize size = item->sizeHint();
            if (x > content.x() && x + size.width() > right) {
                x = content.x();
                y += lineHeight + verticalSpacing_;
                lineHeight = 0;
            }
            if (!testOnly) {
                item->setGeometry(QRect(QPoint(x, y), size));
            }
            x += size.width() + horizontalSpacing_;
            lineHeight = qMax(lineHeight, size.height());
        }
        return y - rect.y() + lineHeight + margins.bottom();
    }

    QList<QLayoutItem*> items_;
    int horizontalSpacing_;
    int verticalSpacing_;
};

QList<mplayer::shell::QualityEntry> toQualities(const QMap<int, QString>& map) {
    QList<mplayer::shell::QualityEntry> list;
    list.reserve(map.size());
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (!it.value().trimmed().isEmpty()) {
            list.append({it.key(), it.value()});
        }
    }
    return list;
}

/// The highest usable height in a quality map, 0 when there is none.
int maxQuality(const QMap<int, QString>& map) {
    int best = 0;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (it.key() > 0 && !it.value().trimmed().isEmpty()) {
            best = qMax(best, it.key());
        }
    }
    return best;
}

QString capitalize(const QString& text) {
    if (text.isEmpty()) {
        return text;
    }
    QString result = text;
    result[0] = result.at(0).toUpper();
    return result;
}

/// The provider key for an embed URL, falling back to what the API said.
QString providerFor(const backend::Source& source) {
    QString provider;
    try {
        provider = backend::detectSite(source.embed_url);
    } catch (const std::exception&) {
        provider = source.provider.toLower().trimmed();
    }
    return provider.isEmpty() ? QStringLiteral("other") : provider;
}

/// The number an episode shows, from its label or its (possibly fractional)
/// number. Episodes number from anywhere, so this is never assumed to start at 1.
QString episodeNumber(const backend::Episode& episode) {
    if (!episode.label.isEmpty()) {
        return episode.label;
    }
    if (episode.number.isValid() && !episode.number.isNull()) {
        const QString rendered = labels::variant(episode.number);
        if (!rendered.isEmpty()) {
            return rendered;
        }
    }
    return QStringLiteral("?");
}

}  // namespace

struct TitlePage::PlayerSession {
    class Host* host = nullptr;
    mplayer::shell::PlayerDialog* dialog = nullptr;
    /// The episode the player is on right now, for stream recovery. A dub/sub
    /// switch replaces it, so recovery must not rewind to the opening episode.
    EpisodeEntry* entry = nullptr;
    /// True while this player's runtime request is in flight, so a second
    /// request from the same dialog does not start another one.
    bool runtimePending = false;
    int sourceSequence = 0;
    QVector<QVariantMap> playlist;
};

/// Everything the player cannot do for itself, answered by the page that opened
/// it. Non-owning: the session is freed when the dialog reports its close.
class TitlePage::Host : public mplayer::shell::PlayerHost {
public:
    Host(TitlePage* page, PlayerSession* session) : page_(page), session_(session) {}

    void requestVariant(const QString& embed, const QString& label) override {
        page_->switchPlayerVariant(session_, embed, label);
    }
    void requestEpisode(int index) override {
        if (index < 0 || index >= session_->playlist.size()) {
            return;
        }
        page_->playEpisodeFrom(session_, session_->playlist.at(index), index);
    }
    void requestTrackProbe(const QString& embed) override {
        page_->probeTrackQuality(session_, embed);
    }
    void requestOpenOriginal() override {
        if (session_->entry != nullptr && !session_->entry->embedUrl.isEmpty() &&
            !QDesktopServices::openUrl(QUrl(session_->entry->embedUrl))) {
            showPlayerWarning(QCoreApplication::translate("TitlePage", "Failed to open in browser"));
        }
    }
    void requestCopyStreamUrl() override {
        if (session_->entry != nullptr && !mikai::gui::copyText(session_->entry->embedUrl)) {
            showPlayerWarning(QCoreApplication::translate("TitlePage", "Failed to copy link"));
        }
    }
    void playerClosed() override { page_->closeSession(session_); }
    void playbackFailed() override { page_->recoverStream(session_); }

private:
    /// The named failure the dialog's clipboard or browser action could not
    /// complete. Parented to the player so the warning sits above the thing that
    /// raised it and is reachable by a screen reader, with the page's status line
    /// as the only fallback when there is no live dialog.
    ///
    /// The warning spins its own nested event loop, and everything that can
    /// delete this page or this player inside it - a window close, a navigation,
    /// the dialog's own close - has to be able to happen. Both pointers are
    /// therefore read once, weakly, before the loop, and nothing is dereferenced
    /// after it: the dialog's own close path is what disposes a session, and it
    /// does that while the session is still registered.
    void showPlayerWarning(const QString& message) {
        // Read weakly, once, before the nested loop: a dialog already on its way
        // to being deleted must not be handed a message box as its parent.
        const QPointer<mplayer::shell::PlayerDialog> dialog = session_->dialog;
        if (dialog.isNull()) {
            const QPointer<TitlePage> page = page_;
            if (!page.isNull()) {
                page->flashStatus(message, true);
            }
            return;
        }
        const QString title = dialog->windowTitle();
        QMessageBox::warning(dialog, title, message);
    }

    TitlePage* page_;
    PlayerSession* session_;
};

TitlePage::TitlePage(Runner& runner, backend::MikaiClient& client, QualityCache& qualityCache,
                     const QString& ref, const QString& posterHint,
                     const backend::AnimeDetail& detailHint, QWidget* parent)
    : QWidget(parent), runner_(runner), client_(client), qualityCache_(qualityCache),
      ref_(ref), posterHint_(posterHint) {
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
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(theme::kSpacingLarge, theme::kSpacingMedium, theme::kSpacingLarge,
                             theme::kSpacingMedium);
    root->setSpacing(theme::kSpacingSmall);

    auto* top = new QHBoxLayout();
    auto* back = new QPushButton(tr("← Back"), this);
    connect(back, &QPushButton::clicked, this, &TitlePage::backRequested);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("status"));
    // Right-aligned against the rate chip: the stretch absorbs text-length
    // changes on the left, so flashes never push the rate/settings controls.
    status_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    status_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* settingsButton = new QPushButton(this);
    settingsButton->setObjectName(QStringLiteral("settingsChip"));
    settingsButton->setIcon(theme::gearIcon());
    settingsButton->setIconSize(QSize(theme::kIconSize, theme::kIconSize));
    settingsButton->setToolTip(tr("API token settings"));
    settingsButton->setAccessibleName(tr("API token settings"));
    connect(settingsButton, &QPushButton::clicked, this, &TitlePage::settingsRequested);
    rateLabel_ = new RateLimitLabel(client_, this);
    top->addWidget(back);
    top->addStretch();
    top->addWidget(status_);
    top->addWidget(rateLabel_);
    top->addWidget(settingsButton);
    root->addLayout(top);

    overviewScroll_ = new QScrollArea(this);
    overviewScroll_->setObjectName(QStringLiteral("titleOverview"));
    overviewScroll_->setWidgetResizable(true);
    overviewScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* page = new QWidget(overviewScroll_);
    auto* pageLayout = new QHBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    // Cover-to-text gutter: one step down from the old full-large gap so the
    // facts block reads attached to its cover rather than floating.
    pageLayout->setSpacing(theme::kTitleColumnSpacing);

    cover_ = new CoverLabel(tr("Loading…"), page);
    cover_->setObjectName(QStringLiteral("titleCover"));
    cover_->setFixedSize(theme::kTitleCoverWidth, theme::kTitleCoverHeight);
    connect(cover_, &CoverLabel::activated, this, &TitlePage::openCoverViewer);
    cover_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(cover_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& position) { coverContextMenu(cover_->mapToGlobal(position)); });
    pageLayout->addWidget(cover_, 0, Qt::AlignTop);

    auto* textColumn = new QVBoxLayout();
    // Facts, scores, description and disclosures stack at token gap; the gaps
    // between *sections* come from the fixed spacers below instead.
    textColumn->setSpacing(theme::kTitleStackSpacing);
    heading_ = new QLabel(this);
    heading_->setObjectName(QStringLiteral("titleHeading"));
    heading_->setWordWrap(true);
    subtitle_ = new QLabel(this);
    subtitle_->setObjectName(QStringLiteral("muted"));
    subtitle_->setWordWrap(true);
    textColumn->addWidget(heading_);
    textColumn->addWidget(subtitle_);

    facts_ = new QLabel(this);
    facts_->setObjectName(QStringLiteral("titleFacts"));
    facts_->setWordWrap(true);
    textColumn->addWidget(facts_);

    scores_ = new QLabel(this);
    // The rule lives in the app stylesheet under this name. A per-widget
    // stylesheet makes Qt build a separate style cache for that subtree and
    // re-resolve the whole sheet against it.
    scores_->setObjectName(QStringLiteral("titleScores"));
    textColumn->addWidget(scores_);

    description_ = new QLabel(this);
    description_->setObjectName(QStringLiteral("titleDescription"));
    description_->setTextFormat(Qt::PlainText);
    description_->setWordWrap(true);
    description_->setAlignment(Qt::AlignTop);
    description_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    textColumn->addWidget(description_);

    auto* disclosure = new QHBoxLayout();
    disclosure->setSpacing(theme::kSpacingMedium);
    descriptionButton_ = new QPushButton(tr("Read more"), this);
    descriptionButton_->setObjectName(QStringLiteral("textAction"));
    descriptionButton_->hide();
    connect(descriptionButton_, &QPushButton::clicked, this, &TitlePage::toggleDescription);
    metaButton_ = new QPushButton(tr("More details"), this);
    metaButton_->setObjectName(QStringLiteral("textAction"));
    metaButton_->setCheckable(true);
    connect(metaButton_, &QPushButton::toggled, this, &TitlePage::toggleMetadata);
    relatedButton_ = new QPushButton(tr("Related and similar"), this);
    relatedButton_->setObjectName(QStringLiteral("textAction"));
    relatedButton_->setCheckable(true);
    relatedButton_->hide();
    // The button is the user pressing it, so this is the one expansion that
    // scrolls the overview.
    connect(relatedButton_, &QPushButton::toggled, this,
            [this](bool checked) { setRelatedExpanded(checked, true); });
    disclosure->addWidget(descriptionButton_);
    disclosure->addWidget(metaButton_);
    disclosure->addWidget(relatedButton_);
    disclosure->addStretch();
    // Disclosure row is the last of the description section: its gap sits above
    // the details/links/related sections that follow.
    textColumn->addLayout(disclosure);
    textColumn->addSpacing(theme::kSpacingSmall);

    metaHost_ = new QWidget(this);
    metaHost_->setObjectName(QStringLiteral("titleDetails"));
    metaGrid_ = new QGridLayout(metaHost_);
    metaGrid_->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium,
                                  theme::kSpacingMedium, theme::kSpacingMedium);
    metaGrid_->setHorizontalSpacing(theme::kSpacingMedium);
    metaGrid_->setVerticalSpacing(theme::kSpacingSmall - theme::kSpacingXs);
    metaGrid_->setColumnStretch(1, 1);
    metaHost_->hide();
    textColumn->addWidget(metaHost_);

    links_ = new QLabel(this);
    links_->setObjectName(QStringLiteral("titleLinks"));
    links_->setTextFormat(Qt::RichText);
    links_->setOpenExternalLinks(true);
    textColumn->addWidget(links_);

    relatedHost_ = new QWidget(this);
    relatedLayout_ = new QVBoxLayout(relatedHost_);
    relatedLayout_->setContentsMargins(0, 0, 0, 0);
    relatedLayout_->setSpacing(theme::kSpacingXs);
    relatedHost_->hide();
    textColumn->addWidget(relatedHost_);
    textColumn->addStretch();
    pageLayout->addLayout(textColumn, 1);

    overviewScroll_->setWidget(page);
    root->addWidget(overviewScroll_, 1);

    auto* releaseSection = new QVBoxLayout();
    releaseSection->setContentsMargins(0, theme::kSpacingSmall, 0, 0);
    releaseSection->setSpacing(theme::kSpacingXs);
    auto* episodesHeading = new QLabel(tr("Episodes"), this);
    episodesHeading->setObjectName(QStringLiteral("episodeSectionHeading"));
    releaseSection->addWidget(episodesHeading);

    releaseChipsHost_ = new QWidget(this);
    releaseChipsHost_->setObjectName(QStringLiteral("releaseChipsHost"));
    releaseRowsLayout_ = new QVBoxLayout(releaseChipsHost_);
    releaseRowsLayout_->setContentsMargins(0, 0, 0, 0);
    releaseRowsLayout_->setSpacing(theme::kSpacingXs);
    releaseSection->addWidget(releaseChipsHost_);
    root->addLayout(releaseSection);

    releaseGroup_ = new QButtonGroup(this);
    releaseGroup_->setExclusive(true);
    connect(releaseGroup_, &QButtonGroup::idClicked, this, [this](int index) {
        if (index != activeRelease_) {
            fillEpisodes(index);
        }
        probeReleaseQualities();
    });

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("episodeTabs"));
    tabs_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    tabs_->setDocumentMode(true);
    tabs_->tabBar()->setExpanding(false);
    tabs_->tabBar()->setDrawBase(false);
    root->addWidget(tabs_);
    // A tab that has never been shown has no pills in it; this is where they
    // get built, once, the first time the reader goes there.
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index < 0 || index >= tabStates_.size() || tabStates_.at(index).rendered) {
            return;
        }
        renderTabPage(index);
        fitEpisodeSection();
    });
    fitEpisodeSection();
}

void TitlePage::load() {
    setStatus(tr("Loading…"));
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
            [self](const QString& message) {
                if (!self.isNull() && !self->closed_) {
                    self->onError(message);
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
        [self, state](const QString& message) {
            if (self.isNull() || self->closed_ || !state->error.isEmpty()) {
                return;
            }
            state->error = message;
            self->onError(message);
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
        [self, state](const QString& message) {
            if (self.isNull() || self->closed_ || !state->error.isEmpty()) {
                return;
            }
            state->error = message;
            self->onError(message);
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
    rateLabel_->refresh();
    const QString name = backend::displayName(detail.titles);
    heading_->setText(name == QLatin1String("?") ? ref_ : name);
    subtitle_->setText(detail.titles.english.isEmpty() ? detail.titles.original
                                                       : detail.titles.english);
    populateDetail(detail);
    const QString cover = backend::coverUrl(detail);
    Q_EMIT recentTitleLoaded(detail.ref.isEmpty() ? ref_ : detail.ref, heading_->text(),
                             cover.isEmpty() ? posterHint_ : cover);
    loadRecommendations(detail);

    fullDescription_ = detail.description.trimmed().isEmpty()
                           ? tr("No description available.")
                           : detail.description.trimmed();
    descriptionExpanded_ = false;
    description_->setText(descriptionPreview());
    descriptionButton_->setText(tr("Read more"));
    descriptionButton_->setVisible(fullDescription_.size() > theme::kTitleDescriptionPreviewChars);

    if (!cover.isEmpty()) {
        loadCover(cover, true);
    } else if (!posterHint_.isEmpty()) {
        loadCover(posterHint_, false);
    } else {
        cover_->setText(tr("No cover"));
    }

    releases_ = backend::sortReleases(player.releases);
    releaseProbeSeq_ += 1;
    releaseQuality_.clear();
    releaseProbed_.clear();
    rebuildReleaseChips();

    if (releases_.isEmpty()) {
        setStatus(tr("No releases available"));
        return;
    }
    releaseChips_.first()->setChecked(true);
    fillEpisodes(0);
}

void TitlePage::onError(const QString& message) {
    rateLabel_->refresh();
    setStatus(message);
    cover_->setZoomable(false);
    cover_->setText(tr("No cover"));
}

void TitlePage::populateDetail(const backend::AnimeDetail& detail) {
    scores_->setText(scoresText(detail));
    populateRelated(detail);

    while (metaGrid_->count() > 0) {
        QLayoutItem* item = metaGrid_->takeAt(0);
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
    }

    const backend::EpisodeStats& episodes = detail.episodes;
    QString episodesLabel;
    if (episodes.aired > 0 || episodes.localized > 0) {
        const int total = episodes.total.value_or(0);
        const int localized = episodes.localized > 0 ? episodes.localized : episodes.aired;
        episodesLabel = (total > 0 && total != localized)
                            ? QStringLiteral("%1 / %2").arg(localized).arg(total)
                            : QString::number(localized);
    }

    const QString statusText = labels::status(detail.status);
    const QString formatText = labels::format(detail.format);
    const QString seasonText = labels::season(detail.start_date);
    QStringList facts;
    if (!statusText.isEmpty()) facts << statusText;
    if (!formatText.isEmpty()) facts << formatText;
    if (!seasonText.isEmpty()) facts << seasonText;
    if (!episodesLabel.isEmpty()) facts << tr("%1 episodes").arg(episodesLabel);
    facts_->setText(facts.join(QStringLiteral(" · ")));

    QVector<QPair<QString, QString>> rows;
    if (!episodesLabel.isEmpty()) rows.append({tr("Episodes"), episodesLabel});
    if (!formatText.isEmpty()) rows.append({tr("Type"), formatText});
    if (episodes.duration_min.value_or(0) > 0) {
        rows.append({tr("Duration"), tr("%1 min").arg(*episodes.duration_min)});
    }
    if (!statusText.isEmpty()) rows.append({tr("Status"), statusText});
    if (!seasonText.isEmpty()) rows.append({tr("Season"), seasonText});
    if (!detail.source.isEmpty()) {
        rows.append({tr("Original source"), labels::source(detail.source)});
    }
    if (!detail.studio.isEmpty()) rows.append({tr("Studio"), detail.studio});
    if (!detail.country.isEmpty()) rows.append({tr("Country"), labels::country(detail.country)});
    if (!detail.age_rating.isEmpty()) {
        rows.append({tr("Age rating"), labels::ageRating(detail.age_rating)});
    }
    QStringList genres;
    for (const backend::GenreTag& genre : detail.genres) {
        const QString text = genre.ua.isEmpty() ? genre.name : genre.ua;
        if (!text.isEmpty()) {
            genres << text;
        }
    }
    if (!genres.isEmpty()) rows.append({tr("Genres"), genres.join(QStringLiteral(", "))});
    QStringList tags;
    for (const backend::GenreTag& tag : detail.tags) {
        const QString text = tag.ua.isEmpty() ? tag.name : tag.ua;
        if (!text.isEmpty()) {
            tags << text;
        }
    }
    if (!tags.isEmpty()) rows.append({tr("Tags"), tags.join(QStringLiteral(", "))});

    int row = 0;
    for (const auto& entry : rows) {
        auto* key = new QLabel(entry.first, metaHost_);
        key->setObjectName(QStringLiteral("muted"));
        key->setAlignment(Qt::AlignRight | Qt::AlignTop);
        auto* value = new QLabel(entry.second, metaHost_);
        value->setWordWrap(true);
        metaGrid_->addWidget(key, row, 0);
        metaGrid_->addWidget(value, row, 1);
        ++row;
    }

    QVector<QPair<QString, QString>> links = backend::externalLinks(detail);
    // The site serves the title at /anime/<id>-<slug> and 404s on either half
    // alone, so the link is built from both rather than preferring the slug.
    const QString page =
        backend::animePageUrl(detail.ref, detail.ids.value(QStringLiteral("slug")).toString());
    if (!detail.ref.isEmpty()) {
        links.prepend({tr("Mikai"), page});
    }
    QStringList anchors;
    for (const auto& link : links) {
        anchors << QStringLiteral("<a style=\"text-decoration: none;\" href=\"%1\">%2</a>")
                       .arg(link.second.toHtmlEscaped(), link.first.toHtmlEscaped());
    }
    links_->setText(anchors.join(QStringLiteral(" · ")));
}

void TitlePage::populateRelated(const backend::AnimeDetail& detail) {
    while (relatedLayout_->count() > 0) {
        QLayoutItem* item = relatedLayout_->takeAt(0);
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
    }
    QVector<QPair<QString, backend::AnimeSummary>> relations;
    for (int index = 0;
         index < detail.relations.size() && index < theme::kRelatedCarouselMax; ++index) {
        if (!detail.relations.at(index).anime.ref.isEmpty()) {
            relations.append({detail.relations.at(index).kind, detail.relations.at(index).anime});
        }
    }
    QVector<QPair<QString, backend::AnimeSummary>> similar;
    for (int index = 0; index < detail.similar.size() && index < theme::kRelatedCarouselMax;
         ++index) {
        if (!detail.similar.at(index).ref.isEmpty()) {
            similar.append({QStringLiteral("similar"), detail.similar.at(index)});
        }
    }
    if (relations.isEmpty() && similar.isEmpty()) {
        relatedHost_->hide();
        relatedButton_->hide();
        return;
    }
    if (!relations.isEmpty()) {
        relatedLayout_->addWidget(buildRelatedCarousel(tr("Related titles"), relations));
    }
    if (!similar.isEmpty()) {
        relatedLayout_->addWidget(buildRelatedCarousel(tr("Similar anime"), similar));
    }
    setRelatedExpanded(true, false);
}

void TitlePage::loadRecommendations(const backend::AnimeDetail& detail) {
    backend::MikaiClient* client = &client_;
    const QPointer<TitlePage> self(this);
    runner_.run(
        [client] {
            return QVariant::fromValue(
                client->browsePopular(1, theme::kHomeCarouselMaxTitles));
        },
        [self, detail](const QVariant& result) {
            if (self.isNull() || self->closed_) {
                return;
            }
            QSet<QString> excluded = {self->ref_, detail.ref};
            for (const backend::AnimeRelation& relation : detail.relations) {
                excluded.insert(relation.anime.ref);
            }
            for (const backend::AnimeSummary& anime : detail.similar) {
                excluded.insert(anime.ref);
            }
            QVector<QPair<QString, backend::AnimeSummary>> entries;
            const backend::SearchResult popular = result.value<backend::SearchResult>();
            for (const backend::AnimeSummary& anime : popular.items) {
                if (anime.ref.isEmpty() || excluded.contains(anime.ref)) {
                    continue;
                }
                excluded.insert(anime.ref);
                entries.append({QStringLiteral("recommendation"), anime});
                if (entries.size() == theme::kRelatedCarouselMax) {
                    break;
                }
            }
            if (entries.isEmpty()) {
                return;
            }
            // A carousel arriving under a collapsed one must not expand it: the
            // reader already made that choice.
            const bool expand =
                self->relatedHost_->isVisible() || !self->relatedButton_->isVisible();
            self->relatedLayout_->addWidget(
                self->buildRelatedCarousel(tr("Recommended on Mikai"), entries));
            self->setRelatedExpanded(expand, false);
        },
        [](const QString&) {});
}

QWidget* TitlePage::buildRelatedCarousel(
    const QString& heading, const QVector<QPair<QString, backend::AnimeSummary>>& entries) {
    auto* section = new QWidget(relatedHost_);
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::kSpacingSmall - theme::kSpacingXs);

    auto* header = new QHBoxLayout();
    header->setSpacing(theme::kSpacingSmall);
    auto* title = new QLabel(heading, section);
    title->setObjectName(QStringLiteral("titleSectionHeading"));
    header->addWidget(title);
    header->addStretch();
    auto* previous = new QPushButton(QStringLiteral("←"), section);
    auto* next = new QPushButton(QStringLiteral("→"), section);
    previous->setObjectName(QStringLiteral("carouselControl"));
    next->setObjectName(QStringLiteral("carouselControl"));
    header->addWidget(previous);
    header->addWidget(next);
    layout->addLayout(header);

    auto* scroll = new QScrollArea(section);
    scroll->setObjectName(QStringLiteral("animeCarousel"));
    scroll->setWidgetResizable(false);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumHeight(theme::kCarouselCardHeight + theme::kSpacingMedium);
    auto* track = new QWidget(scroll);
    track->setObjectName(QStringLiteral("animeCarouselTrack"));
    auto* cards = new QHBoxLayout(track);
    cards->setContentsMargins(2, 2, 2, 2);
    cards->setSpacing(theme::kCarouselCardSpacing);

    for (const auto& entry : entries) {
        const backend::AnimeSummary& anime = entry.second;
        auto* card = new QPushButton(section);
        card->setObjectName(QStringLiteral("animeCard"));
        card->setFixedSize(theme::kCarouselCardWidth, theme::kCarouselCardHeight);
        const QString name = backend::displayName(anime.titles);
        card->setToolTip(name);
        const QString ref = anime.ref;
        const QString poster = backend::posterUrl(anime);
        connect(card, &QPushButton::clicked, this,
                [this, ref, poster] { Q_EMIT relatedOpenRequested(ref, poster); });

        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(theme::kSpacingSmall, theme::kSpacingSmall,
                                       theme::kSpacingSmall, theme::kSpacingSmall);
        cardLayout->setSpacing(theme::kSpacingSmall - theme::kSpacingXs);
        auto* posterLabel = new QLabel(tr("Poster unavailable"), card);
        posterLabel->setObjectName(QStringLiteral("carouselPoster"));
        posterLabel->setAlignment(Qt::AlignCenter);
        posterLabel->setFixedSize(theme::kCarouselPosterWidth, theme::kCarouselPosterHeight);
        posterLabel->setWordWrap(true);
        posterLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        cardLayout->addWidget(posterLabel, 0, Qt::AlignHCenter);

        auto* kind = new QLabel(labels::relation(entry.first), card);
        kind->setObjectName(QStringLiteral("carouselKind"));
        kind->setAlignment(Qt::AlignCenter);
        kind->setAttribute(Qt::WA_TransparentForMouseEvents);
        cardLayout->addWidget(kind);

        auto* titleLabel = new QLabel(name, card);
        titleLabel->setObjectName(QStringLiteral("carouselTitle"));
        titleLabel->setAlignment(Qt::AlignCenter);
        titleLabel->setWordWrap(true);
        titleLabel->setFixedHeight(theme::kCarouselTitleHeight);
        titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        cardLayout->addWidget(titleLabel);
        cards->addWidget(card);

        const QString posterUrl = backend::posterUrl(anime);
        if (!posterUrl.isEmpty()) {
            // One task per poster, on the image pool: a shared poster costs one
            // download, and a fan-out costs no API budget.
            backend::MikaiClient* client = &client_;
            const QPointer<TitlePage> self(this);
            const QPointer<QLabel> poster(posterLabel);
            runner_.runImage(
                [client, posterUrl] { return client->fetchBytes(posterUrl); },
                [self, poster](const QVariant& result) {
                    if (self.isNull() || poster.isNull()) {
                        return;
                    }
                    QPixmap pixmap;
                    if (pixmap.loadFromData(result.toByteArray()) && !pixmap.isNull()) {
                        poster->setText(QString());
                        poster->setPixmap(pixmap.scaled(
                            poster->size(), Qt::KeepAspectRatioByExpanding,
                            Qt::SmoothTransformation));
                    }
                },
                [](const QString&) {});
        }
    }
    track->adjustSize();
    scroll->setWidget(track);
    layout->addWidget(scroll);

    QScrollBar* bar = scroll->horizontalScrollBar();
    connect(previous, &QPushButton::clicked, this,
            [bar] { bar->setValue(bar->value() - theme::kCarouselScrollStep); });
    connect(next, &QPushButton::clicked, this,
            [bar] { bar->setValue(bar->value() + theme::kCarouselScrollStep); });
    previous->setEnabled(false);
    connect(bar, &QScrollBar::rangeChanged, this,
            [previous, next, bar](int minimum, int maximum) {
                previous->setEnabled(maximum > minimum && bar->value() > minimum);
                next->setEnabled(bar->value() < maximum);
            });
    connect(bar, &QScrollBar::valueChanged, this, [previous, next, bar](int value) {
        previous->setEnabled(value > bar->minimum());
        next->setEnabled(value < bar->maximum());
    });
    return section;
}

void TitlePage::setRelatedExpanded(bool expanded, bool fromUser) {
    relatedHost_->setVisible(expanded);
    relatedButton_->setVisible(true);
    relatedButton_->setText(expanded ? tr("Hide similar") : tr("Related and similar"));
    const QSignalBlocker blocker(relatedButton_);
    relatedButton_->setChecked(expanded);
    if (expanded && fromUser) {
        QTimer::singleShot(0, this, [this] { overviewScroll_->ensureWidgetVisible(relatedHost_); });
    }
}

void TitlePage::toggleDescription() {
    descriptionExpanded_ = !descriptionExpanded_;
    description_->setText(descriptionExpanded_ ? fullDescription_ : descriptionPreview());
    descriptionButton_->setText(descriptionExpanded_ ? tr("Collapse description")
                                                      : tr("Read more"));
}

QString TitlePage::descriptionPreview() const {
    if (fullDescription_.size() <= theme::kTitleDescriptionPreviewChars) {
        return fullDescription_;
    }
    QString excerpt = fullDescription_.left(theme::kTitleDescriptionPreviewChars);
    const int lastSpace = excerpt.lastIndexOf(QLatin1Char(' '));
    if (lastSpace > 0) {
        excerpt = excerpt.left(lastSpace);
    }
    while (!excerpt.isEmpty() && QStringLiteral(" ,.;:").contains(excerpt.back())) {
        excerpt.chop(1);
    }
    return excerpt + QStringLiteral("…");
}

void TitlePage::toggleMetadata(bool expanded) {
    metaHost_->setVisible(expanded);
    metaButton_->setText(expanded ? tr("Hide details") : tr("More details"));
    if (expanded) {
        QTimer::singleShot(0, this, [this] { overviewScroll_->ensureWidgetVisible(metaHost_); });
    }
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
        label += tr(" · %n episode(s)", nullptr, count);
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

void TitlePage::rebuildReleaseChips() {
    for (QAbstractButton* button : releaseGroup_->buttons()) {
        releaseGroup_->removeButton(button);
    }
    const auto oldRows = releaseChipsHost_->findChildren<QWidget*>(
        QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* row : oldRows) {
        releaseRowsLayout_->removeWidget(row);
        delete row;
    }
    releaseChips_.fill(nullptr, releases_.size());
    activeRelease_ = -1;

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
    for (const QString& kind : kindOrder) {
        auto* rowWidget = new QWidget(releaseChipsHost_);
        auto* row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(theme::kSpacingSmall);

        auto* kindLabel = new QLabel(labels::kind(kind), rowWidget);
        kindLabel->setObjectName(QStringLiteral("releaseKind"));
        kindLabels.append(kindLabel);
        row->addWidget(kindLabel, 0, Qt::AlignTop);
        auto* chips = new ReleaseFlowLayout(theme::kSpacingSmall, theme::kSpacingXs);
        row->addLayout(chips, 1);

        for (const int index : releasesByKind.value(kind)) {
            auto* chip = new QPushButton(releaseChipLabel(releases_.at(index)), rowWidget);
            chip->setToolTip(releaseChipToolTip(releases_.at(index)));
            chip->setObjectName(QStringLiteral("releaseChip"));
            chip->setCheckable(true);
            chip->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
            chip->setFixedHeight(theme::kTitleReleaseChipHeight);
            releaseGroup_->addButton(chip, index);
            releaseChips_[index] = chip;
            chips->addWidget(chip);
        }
        releaseRowsLayout_->addWidget(rowWidget);
    }

    int kindLabelWidth = 0;
    for (QLabel* label : kindLabels) {
        kindLabelWidth = qMax(kindLabelWidth, label->sizeHint().width());
    }
    for (QLabel* label : kindLabels) {
        label->setFixedWidth(kindLabelWidth);
    }
}

void TitlePage::fillEpisodes(int index) {
    if (index < 0 || index >= releases_.size() || index == activeRelease_) {
        return;
    }
    activeRelease_ = index;
    while (tabs_->count() > 0) {
        QWidget* page = tabs_->widget(0);
        tabs_->removeTab(0);
        page->deleteLater();
    }
    fetchWaiters_.clear();
    tabStates_.clear();
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
            entry->embedUrl = source.embed_url;
            ownedEntries_.append(entry);
            grouped[providerFor(source)].append(entry);
        }
    }

    for (auto it = grouped.constBegin(); it != grouped.constEnd(); ++it) {
        TabState state;
        state.provider = it.key();
        state.entries = it.value();
        state.pageSize = theme::kEpisodesPerPage;

        auto* tabPage = new QWidget(tabs_);
        auto* tabLayout = new QVBoxLayout(tabPage);
        tabLayout->setContentsMargins(0, 0, 0, 0);
        tabLayout->setSpacing(theme::kSpacingSmall - theme::kSpacingXs);

        // The pager leads the section: page navigation sits at hand and the grid
        // cannot push it around as it grows.
        auto* pager = new RangePager(theme::kEpisodesPerPage, tabPage);
        state.pager = pager;
        const int stateIndex = tabStates_.size();
        connect(pager, &RangePager::pageRequested, this,
                [this, stateIndex](int page) { goToEpisodePage(stateIndex, page); });
        tabLayout->addWidget(pager);

        auto* scroll = new QScrollArea(tabPage);
        scroll->setObjectName(QStringLiteral("episodeScroll"));
        scroll->setWidgetResizable(true);
        state.scroll = scroll;
        auto* grid = new QWidget(scroll);
        grid->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        state.grid = grid;
        state.gridLayout = new QGridLayout(grid);
        state.gridLayout->setContentsMargins(0, theme::kSpacingXs, 0, 0);
        state.gridLayout->setSpacing(theme::kSpacingXs);
        grid->installEventFilter(this);
        scroll->setWidget(grid);
        tabLayout->addWidget(scroll, 1);

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
        tabs_->addTab(tabPage, known.isEmpty() ? capitalize(provider) : known);
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

    setStatus(tabs_->count() > 0 ? QString() : tr("No sources in this release"));
    fitEpisodeSection();
}

QPushButton* TitlePage::buildEpisodeButton(EpisodeEntry* entry, QWidget* grid) {
    // The grid comes from the tab being rendered, not from the entry: the entry
    // only ever needs its index for the player's playlist, and reading a vector
    // through an unvalidated index is how the pill grid ended up out of bounds.
    auto* button = new QPushButton(entry->number, grid);
    button->setObjectName(QStringLiteral("episodeButton"));
    button->setFixedSize(theme::kEpisodeButtonWidth, theme::kEpisodeButtonHeight);
    entry->label = tr("Episode %1").arg(entry->number);
    entry->button = button;
    button->setToolTip(tr("%1 — watch").arg(entry->label));
    button->setContextMenuPolicy(Qt::CustomContextMenu);
    if (qualityCache_.isNoSource(entry->embedUrl)) {
        markNoSource(entry);
    }
    connect(button, &QPushButton::clicked, this, [this, entry] { playEpisode(entry); });
    connect(button, &QWidget::customContextMenuRequested, this,
            [this, entry, button](const QPoint& position) {
                episodeContextMenu(entry, button->mapToGlobal(position));
            });
    return button;
}

void TitlePage::renderTabPage(int stateIndex) {
    if (stateIndex < 0 || stateIndex >= tabStates_.size()) {
        return;
    }
    TabState& state = tabStates_[stateIndex];
    state.rendered = true;
    while (QLayoutItem* stale = state.gridLayout->takeAt(0)) {
        if (stale->widget() != nullptr) {
            stale->widget()->deleteLater();
        }
        delete stale;
    }

    // Every entry on this page gets a fresh button: a pill from the previous page
    // must never still be a click target after it leaves the grid.
    const int start = state.page * state.pageSize;
    const int end = qMin(start + state.pageSize, state.entries.size());
    for (int index = start; index < end; ++index) {
        buildEpisodeButton(state.entries.at(index), state.grid);
    }

    const int totalPages = qMax(1, (state.entries.size() + state.pageSize - 1) / state.pageSize);
    state.pager->setVisible(totalPages > 1);
    QStringList numbers;
    numbers.reserve(state.entries.size());
    for (const EpisodeEntry* entry : state.entries) {
        numbers << entry->number;
    }
    state.pager->setState(state.page + 1, totalPages, state.entries.size(), numbers);
    const int columns = qMax(
        1, state.grid->width() / (theme::kEpisodeButtonWidth + theme::kSpacingXs));
    QVector<QWidget*> buttons;
    buttons.reserve(end - start);
    for (int index = start; index < end; ++index) {
        buttons.append(state.entries.at(index)->button);
    }
    layoutEpisodeGrid(state.gridLayout, buttons, columns);
    state.columns = columns;
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
        renderTabPage(stateIndex);
    }
}

void TitlePage::reflowGrid(int stateIndex) {
    if (stateIndex < 0 || stateIndex >= tabStates_.size()) {
        return;
    }
    TabState& state = tabStates_[stateIndex];
    // A compact pill per episode: 64px plus the gutter fits a row of 16.
    const int columns = qMax(1, state.grid->width() /
                                    (theme::kEpisodeButtonWidth + theme::kSpacingXs));
    if (columns == state.columns) {
        return;
    }
    const int start = state.page * state.pageSize;
    const int end = qMin(start + state.pageSize, state.entries.size());
    QVector<QWidget*> buttons;
    buttons.reserve(end - start);
    for (int index = start; index < end; ++index) {
        buttons.append(state.entries.at(index)->button);
    }
    layoutEpisodeGrid(state.gridLayout, buttons, columns);
    state.columns = columns;
    state.grid->updateGeometry();
    scheduleFit();
}

bool TitlePage::eventFilter(QObject* watched, QEvent* event) {
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

void TitlePage::fitEpisodeSection() {
    if (tabs_ == nullptr) {
        return;
    }
    const int frame = style()->pixelMetric(QStyle::PM_DefaultFrameWidth);
    for (int index = 0; index < tabStates_.size(); ++index) {
        TabState& state = tabStates_[index];
        // The width that decides the column count is the scroll viewport's, not
        // the tab's: the section is clamped at kEpisodeSectionMaxHeight, so a
        // long list grows a vertical scrollbar that takes its width away. Sizing
        // off the tab made the grid a scrollbar's width too wide, which Qt then
        // answered with another Resize and another reflow.
        const int available = state.scroll != nullptr
                                  ? state.scroll->viewport()->width()
                                  : qMax(1, tabs_->width() - 2 * frame);
        if (state.grid->width() != available) {
            state.grid->resize(available, state.grid->height());
        }
        reflowGrid(index);
    }
    const int tabBarHeight = tabs_->count() > 0 ? tabs_->tabBar()->sizeHint().height() : 0;
    for (TabState& state : tabStates_) {
        if (state.scroll == nullptr) {
            continue;
        }
        const int pagerHeight = state.pager != nullptr && !state.pager->isHidden()
                                    ? state.pager->sizeHint().height()
                                    : 0;
        const int gap = pagerHeight > 0 ? theme::kTitleStackSpacing : 0;
        const int maxGridHeight =
            qMax(0, theme::kEpisodeSectionMaxHeight - tabBarHeight - pagerHeight - gap -
                         theme::kSpacingXs - 2 * frame);
        const int gridHeight = state.grid->sizeHint().height() + 2 * state.scroll->frameWidth();
        state.scroll->setFixedHeight(qMin(gridHeight, maxGridHeight));
    }
    // Size from the active page: reserving another release's taller grid leaves
    // a blank panel beneath shorter episode lists.
    const QWidget* currentPage = tabs_->currentWidget();
    const int desired = currentPage == nullptr
                            ? 0
                            : qMin(tabBarHeight + currentPage->sizeHint().height() +
                                       theme::kSpacingXs + 2 * frame,
                                   theme::kEpisodeSectionMaxHeight);
    if (fittedHeight_ == desired && tabs_->height() == desired) {
        return;
    }
    fittedHeight_ = desired;
    if (tabs_->height() != desired) {
        tabs_->setFixedHeight(desired);
    }
}

void TitlePage::playEpisode(EpisodeEntry* entry) {
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
    QVector<EpisodeEntry*>& waiters = fetchWaiters_[embed];
    // A double click asks for the same thing twice; one flight is enough.
    if (std::find(waiters.cbegin(), waiters.cend(), entry) != waiters.cend()) {
        return;
    }
    if (waiters.isEmpty()) {
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
            [self, embed](const QString& message) {
                if (!self.isNull()) {
                    self->finishFetch(embed, QMap<int, QString>(), message);
                }
            });
    }
    waiters.append(entry);
    entryCallbacks_.insert(entry, callback);
}

void TitlePage::finishFetch(const QString& embed, const QMap<int, QString>& qualities,
                            const QString& message) {
    const QVector<EpisodeEntry*> waiters = fetchWaiters_.take(embed);
    if (closed_ || waiters.isEmpty()) {
        return;
    }
    if (!message.isEmpty()) {
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
        flashStatus(tr("Could not find a stream for this episode"), true);
        return;
    }
    qualityCache_.remember(embed, qualities);
    for (EpisodeEntry* entry : waiters) {
        const auto callback = entryCallbacks_.take(entry);
        if (callback) {
            callback(entry, qualities);
        }
    }
}

void TitlePage::markNoSource(EpisodeEntry* entry) {
    if (entry == nullptr || entry->button == nullptr) {
        return;
    }
    entry->button->setEnabled(false);
    entry->button->setToolTip(tr("This player has no video for this episode"));
}

void TitlePage::episodeContextMenu(EpisodeEntry* entry, const QPoint& globalPos) {
    QMenu menu(this);
    // The embed page is one URL, but a viewer usually wants the stream. The
    // extraction fires here, on the press, and the rows fill in as it lands -
    // one flight per embed, the same one an episode open uses, so opening a
    // menu never costs a second extraction of a stream already resolved.
    QMenu* copyMenu = menu.addMenu(tr("Copy to clipboard"));
    QAction* copyUrl = copyMenu->addAction(tr("Page URL"));
    copyUrl->setToolTip(tr("Copy the episode page URL"));
    connect(copyUrl, &QAction::triggered, this, [this, entry] { copyEpisode(entry); });
    copyMenu->addSeparator();
    const bool noSource = qualityCache_.isNoSource(entry->embedUrl);
    QAction* loading = copyMenu->addAction(noSource ? tr("No video available")
                                                   : tr("Loading quality options…"));
    loading->setEnabled(false);
    loading->setToolTip(noSource ? tr("This player has no video for this episode")
                                 : tr("Fetching direct video links"));

    // Fill the submenu in place: an extraction started now usually lands
    // while the menu is still open, and a menu rebuilt under the pointer
    // would close it. Rows are appended to this one instead.
    const auto fillRows = [this, copyMenu, loading](const QMap<int, QString>& qualities) {
        if (qualities.isEmpty()) {
            return;
        }
        // QMap iterates ascending, so the rows are collected and reversed: the
        // best quality belongs at the top, where a viewer looks first.
        QList<int> heights = qualities.keys();
        std::sort(heights.begin(), heights.end(), std::greater<int>());
        for (int height : heights) {
            const QString url = qualities.value(height);
            QAction* row = copyMenu->addAction(tr("%1p").arg(height));
            row->setToolTip(url);
            connect(row, &QAction::triggered, this, [this, url, height] { copyStreamUrl(url, height); });
        }
        // Owned by the submenu, which dies with the page: detaching would leak
        // it, and deleteLater on an action whose parent is being destroyed in
        // the same turn is a double free waiting to happen.
        loading->setVisible(false);
    };

    ensureQualities(entry, [fillRows](EpisodeEntry*, const QMap<int, QString>& qualities) {
        fillRows(qualities);
    });

    QAction* external = menu.addAction(tr("Open in external player"));
    external->setToolTip(tr("Open the best quality in mpv or VLC"));
    external->setEnabled(!qualityCache_.isNoSource(entry->embedUrl));
    connect(external, &QAction::triggered, this, [this, entry] { openExternalPlayer(entry); });
    QAction* browser = menu.addAction(tr("Open in browser"));
    browser->setToolTip(tr("Open the episode page"));
    connect(browser, &QAction::triggered, this, [this, entry] { openEmbedInBrowser(entry); });
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
    const QString streamUrl = qualities.value(height);
    const QStringList arguments =
        name == QLatin1String("mpv")
            ? QStringList{streamUrl}
            : QStringList{QStringLiteral("--play-and-exit"), streamUrl};
    if (QProcess::startDetached(program, arguments)) {
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
            [self, index, seq](const QString&) {
                if (!self.isNull()) {
                    self->applyReleaseQuality(index, QMap<int, QString>(), seq);
                }
            });
    }
}

QString TitlePage::firstEmbed(const backend::Release& release) const {
    for (const backend::Episode& episode : release.episodes) {
        for (const backend::Source& source : episode.sources) {
            if (!source.embed_url.isEmpty()) {
                return source.embed_url;
            }
        }
    }
    return {};
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
        chip->setText(releaseChipLabel(releases_.at(index)) +
                      QStringLiteral(" · %1p").arg(height));
        chip->setToolTip(releaseChipToolTip(releases_.at(index)) +
                         QStringLiteral(" · %1p").arg(height));
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
                const QString provider = providerFor(source);
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
                variants.append(variant);
            }
        }
    }
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
    for (int position = 0; position < entries.size(); ++position) {
        QVariantMap item;
        item.insert(QStringLiteral("label"), entries.at(position)->label);
        item.insert(QStringLiteral("embed"), entries.at(position)->embedUrl);
        playlist.append(item);
        if (entries.at(position)->number == number) {
            index = position;
        }
    }
    if (currentIndex != nullptr) {
        *currentIndex = index;
    }
    return playlist;
}

void TitlePage::openPlayer(EpisodeEntry* entry, const QMap<int, QString>& qualities) {
    const int height = maxQuality(qualities);
    if (height <= 0) {
        flashStatus(tr("Could not find a stream for this episode"), true);
        return;
    }
    auto* session = new PlayerSession;
    session->host = new Host(this, session);
    session->entry = entry;
    int currentIndex = 0;
    session->playlist = tabPlaylist(entry->stateIndex, entry->number, &currentIndex);
    sessions_.append(session);

    session->dialog = new mplayer::shell::PlayerDialog(
        QStringLiteral("%1 · %2p").arg(entry->label).arg(height), qualities.value(height),
        toQualities(qualities), height, session->host, this);
    // Core creation can report failure synchronously from startPlayer().
    connect(session->dialog, &mplayer::shell::PlayerDialog::runtimeRequested, this,
            [this, session] { requestRuntime(session); });
    // A processing-failure remedy needs the application's Settings surface, which
    // only the window owns. The page re-emits it with the dialog that raised it and
    // nothing else: no URL work, no quality probe, no episode or hostless control
    // is added here. The session registration is checked on every use, so a
    // remedy from a player this page has already disposed is dropped instead of
    // being forwarded for a core that no longer exists.
    connect(session->dialog, &mplayer::shell::PlayerDialog::processingSettingsRequested, this,
            [this, session](mplayer::shell::ProcessingSettingsIntent intent) {
                if (!sessions_.contains(session) || session->dialog == nullptr) {
                    return;
                }
                emit playerProcessingSettingsRequested(session->dialog, intent);
            });
    connect(session->dialog, &mplayer::shell::PlayerDialog::componentSetupRequested,
            this, [this, session] {
                if (sessions_.contains(session) && session->dialog != nullptr) {
                    emit playerComponentSetupRequested();
                }
            });
    // The app-owned assembly answers this episode player's native requests: it
    // reads the owned model store, the requested recipe and the measured machine
    // identity off the GUI thread and submits the whole request back through the
    // dialog. Every episode player gets its own binding, exactly as the
    // standalone media player does.
    bindNativePlaybackAssembly(runner_, *session->dialog);
    session->dialog->setPlaylist(session->playlist, currentIndex);
    session->dialog->setVariants(episodeVariants(entry->number), entry->embedUrl);
    session->dialog->show();
    session->dialog->startPlayer();
}

bool TitlePage::closeSession(PlayerSession* session) {
    if (session == nullptr || !sessions_.contains(session)) {
        return true;
    }
    // The player has to acknowledge its own stop first: a refused close means
    // native work is still stopping, so the session stays exactly as it is and
    // the removal can be retried.
    if (session->dialog != nullptr && !session->dialog->close()) {
        qWarning().noquote() << "mikai: session removal refused (native stop unsettled)";
        return false;
    }
    disposeSession(session, false);
    return true;
}

void TitlePage::disposeSession(PlayerSession* session, bool forced) {
    // The player's own playerClosed() may already have disposed this session
    // while close() was running, so this is only ever a pointer comparison until
    // the session is known to still be registered.
    if (session == nullptr || !sessions_.contains(session)) {
        return;
    }
    auto* dialog = session->dialog;
    sessions_.removeAll(session);
    for (auto waiting = trackWaiters_.begin(); waiting != trackWaiters_.end();) {
        waiting->removeAll(session);
        if (waiting->isEmpty()) {
            waiting = trackWaiters_.erase(waiting);
        } else {
            ++waiting;
        }
    }
    session->dialog = nullptr;
    if (forced && dialog != nullptr) {
        // A refused close leaves the host bridge live until the core is destroyed.
        const QPointer<mplayer::shell::PlayerDialog> target(dialog);
        target->hide();
        delete target.data();
    }
    delete session->host;
    session->host = nullptr;
    delete session;
}

void TitlePage::requestRuntime(PlayerSession* session) {
    if (closed_ || session == nullptr || session->dialog == nullptr ||
        !sessions_.contains(session)) {
        return;
    }
    auto* dialog = session->dialog;
    dialog->show();
    if (mplayer::native::runtimeSelected()) {
        return;
    }
    if (session->runtimePending || !mplayer::native::beginRecovery()) {
        return;
    }
    session->runtimePending = true;
    const int seq = ++runtimeSeq_;
    const QPointer<TitlePage> self(this);
    const QPointer<mplayer::shell::PlayerDialog> target(dialog);
    // Validate the bundle off-thread; load the process-pinned module on the GUI thread.
    runner_.run(
        [] {
            return QVariant::fromValue(
                mplayer::native::resolveRuntime(mplayer::native::storedRuntimeChoice()));
        },
        [self, target, session, seq](const QVariant& result) {
            mplayer::native::endRecovery();
            if (self.isNull() || self->closed_ || seq != self->runtimeSeq_ ||
                target.isNull() || !self->sessions_.contains(session) ||
                session->dialog != target.data()) {
                return;
            }
            session->runtimePending = false;
            const mplayer::native::RuntimeSelection resolved =
                result.value<mplayer::native::RuntimeSelection>();
            const mplayer::native::RuntimeSelection selection =
                mplayer::native::commitRuntime(resolved);
            if (!selection.loaded) {
                session->dialog->showError(
                    mplayer::native::runtimeUnavailablePlaybackText(selection.reason));
                return;
            }
            session->dialog->startPlayer();
        },
        [self, target, session, seq](const QString& reason) {
            mplayer::native::endRecovery();
            if (self.isNull() || self->closed_ || seq != self->runtimeSeq_ ||
                target.isNull() || !self->sessions_.contains(session) ||
                session->dialog != target.data()) {
                return;
            }
            session->runtimePending = false;
            session->dialog->showError(mplayer::native::runtimeUnavailablePlaybackText(reason));
        });
}

void TitlePage::recoverStream(PlayerSession* session) {
    // Signed stream URLs expire, so re-extract once before reporting failure.
    // The re-extraction targets whatever track the dialog is on RIGHT NOW: a
    // dub/sub switch replaces the record, so recovery must not rewind the
    // episode to the variant it opened with.
    if (closed_ || session == nullptr || !sessions_.contains(session) ||
        session->entry == nullptr || session->dialog == nullptr) {
        return;
    }
    const EpisodeEntry* failedEntry = session->entry;
    const QString embed = failedEntry->embedUrl;
    // The cached URL is the one that just failed; keeping it would replay the
    // same dead link on the next click instead of extracting again.
    qualityCache_.forget(embed);
    flashStatus(tr("Stream expired — refreshing URL…"), true);
    const QPointer<TitlePage> self(this);
    const QPointer<mplayer::shell::PlayerDialog> target(session->dialog);
    const int seq = session->sourceSequence;
    runner_.runStream(
        [embed] { return QVariant::fromValue(backend::qualityStreams(embed)); },
        [self, target, session, failedEntry, embed, seq](const QVariant& result) {
            const QMap<int, QString> qualities = result.value<QMap<int, QString>>();
            if (self.isNull() || self->closed_ || qualities.isEmpty() ||
                target.isNull() || !self->sessions_.contains(session) ||
                session->dialog != target.data() || seq != session->sourceSequence ||
                session->entry != failedEntry) {
                return;
            }
            self->qualityCache_.remember(embed, qualities);
            target->applyRefreshedStreams(toQualities(qualities), target->currentHeight());
        },
        [self, target, session, failedEntry, seq](const QString& message) {
            if (self.isNull() || self->closed_ || target.isNull() ||
                !self->sessions_.contains(session) || session->dialog != target.data() ||
                seq != session->sourceSequence || session->entry != failedEntry) {
                return;
            }
            session->dialog->showError(
                message.contains(QStringLiteral("no_source"), Qt::CaseInsensitive)
                    ? QCoreApplication::translate("TitlePage",
                                                  "This player has no video any more.")
                    : QCoreApplication::translate("TitlePage",
                                                  "Could not refresh the stream link."));
        });
}

void TitlePage::switchPlayerVariant(PlayerSession* session, const QString& embed,
                                    const QString& label) {
    if (closed_ || session == nullptr || !sessions_.contains(session) ||
        session->dialog == nullptr) {
        return;
    }
    auto* entry = new EpisodeEntry;
    entry->number = label;
    entry->embedUrl = embed;
    entry->label = label;
    ownedEntries_.append(entry);
    const QPointer<TitlePage> self(this);
    const QPointer<mplayer::shell::PlayerDialog> target(session->dialog);
    const int seq = ++session->sourceSequence;
    ensureQualities(
        entry, [self, target, session, seq](EpisodeEntry* loaded,
                                           const QMap<int, QString>& qualities) {
            if (self.isNull() || self->closed_ || target.isNull() ||
                !self->sessions_.contains(session) || session->dialog != target.data() ||
                seq != session->sourceSequence) {
                return;
            }
            session->entry = loaded;
            session->dialog->applyVariant(toQualities(qualities), loaded->label,
                                          loaded->embedUrl);
        });
}

void TitlePage::probeTrackQuality(PlayerSession* session, const QString& embed) {
    // Answer the player's request for one track's best quality, sharing the
    // per-embed cache so a track already probed costs nothing.
    if (closed_ || session == nullptr || !sessions_.contains(session) ||
        session->dialog == nullptr) {
        return;
    }
    const QMap<int, QString> cached = qualityCache_.qualities(embed);
    if (!cached.isEmpty()) {
        session->dialog->setTrackQuality(embed, maxQuality(cached));
        return;
    }
    if (trackWaiters_[embed].contains(session)) {
        return;
    }
    trackWaiters_[embed].append(session);
    const QPointer<TitlePage> self(this);
    const QPointer<mplayer::shell::PlayerDialog> target(session->dialog);
    runner_.runStream(
        [embed] { return QVariant::fromValue(backend::qualityStreams(embed)); },
        [self, target, session, embed](const QVariant& result) {
            if (self.isNull() || target.isNull() || !self->sessions_.contains(session) ||
                session->dialog != target.data()) {
                return;
            }
            const auto waiting = self->trackWaiters_.find(embed);
            if (waiting != self->trackWaiters_.end()) {
                waiting->removeAll(session);
                if (waiting->isEmpty()) {
                    self->trackWaiters_.erase(waiting);
                }
            }
            const QMap<int, QString> qualities = result.value<QMap<int, QString>>();
            if (self->closed_ || qualities.isEmpty()) {
                return;
            }
            self->qualityCache_.remember(embed, qualities);
            session->dialog->setTrackQuality(embed, maxQuality(qualities));
        },
        [self, target, session, embed](const QString&) {
            if (self.isNull() || target.isNull() || !self->sessions_.contains(session) ||
                session->dialog != target.data()) {
                return;
            }
            const auto waiting = self->trackWaiters_.find(embed);
            if (waiting != self->trackWaiters_.end() && waiting->removeAll(session) > 0 &&
                waiting->isEmpty()) {
                self->trackWaiters_.erase(waiting);
            }
        });
}

void TitlePage::playEpisodeFrom(PlayerSession* session, const QVariantMap& item, int index) {
    if (closed_ || session == nullptr || !sessions_.contains(session) ||
        session->dialog == nullptr) {
        return;
    }
    const QString label = item.value(QStringLiteral("label")).toString();
    static const QString prefix = QStringLiteral("Episode ");
    const QString number = label.startsWith(prefix) ? label.mid(prefix.size()) : label;
    auto* entry = new EpisodeEntry;
    entry->number = number;
    entry->embedUrl = item.value(QStringLiteral("embed")).toString();
    entry->label = label;
    ownedEntries_.append(entry);
    const QVector<QVariantMap> variants = episodeVariants(number);
    const QPointer<TitlePage> self(this);
    const QPointer<mplayer::shell::PlayerDialog> target(session->dialog);
    const int seq = ++session->sourceSequence;
    ensureQualities(
        entry, [self, target, session, index, variants, seq](EpisodeEntry* loaded,
                                                          const QMap<int, QString>& qualities) {
            if (self.isNull() || self->closed_ || target.isNull() ||
                !self->sessions_.contains(session) || session->dialog != target.data() ||
                seq != session->sourceSequence) {
                return;
            }
            session->entry = loaded;
            session->dialog->applyEpisode(loaded->label, loaded->embedUrl, index,
                                          toQualities(qualities), variants);
        });
}

void TitlePage::loadCover(const QString& url, bool allowHint) {
    // The client outlives the page, so the worker copies the address and the
    // callbacks hold a QPointer: an image landing after the page was disposed
    // is dropped instead of dereferencing freed widget state.
    backend::MikaiClient* client = &client_;
    const QPointer<TitlePage> self(this);
    runner_.runImage(
        [client, url] { return client->fetchBytes(url); },
        [self, url](const QVariant& result) {
            if (self.isNull() || self->closed_) {
                return;
            }
            QPixmap pixmap;
            if (pixmap.loadFromData(result.toByteArray()) && !pixmap.isNull()) {
                self->coverPixmap_ = pixmap;
                self->coverUrl_ = url;
                self->cover_->setZoomable(true);
                self->cover_->setToolTip(
                    QCoreApplication::translate("TitlePage", "Click to open the cover"));
                self->cover_->setText(QString());
                self->cover_->setPixmap(pixmap.scaled(self->cover_->size(), Qt::KeepAspectRatio,
                                                      Qt::SmoothTransformation));
            }
        },
        [self, url, allowHint](const QString&) {
            if (self.isNull() || self->closed_) {
                return;
            }
            if (allowHint && !self->posterHint_.isEmpty() && self->posterHint_ != url) {
                self->loadCover(self->posterHint_, false);
            } else {
                self->cover_->setZoomable(false);
                self->cover_->setText(QCoreApplication::translate("TitlePage", "No cover"));
            }
        });
}

void TitlePage::coverContextMenu(const QPoint& globalPos) {
    QMenu menu(this);
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
    // WA_DeleteOnClose destroys the viewer the moment it closes, so the page's
    // pointer is dangling from that instant on. Reopening without clearing it
    // called close() on freed memory, which is why open-close-open-close
    // crashed. Clearing on destroyed is the only thing that makes a second open
    // safe; deleteLater() on top of it would be a double free.
    connect(coverViewer_, &QObject::destroyed, this, [this] { coverViewer_ = nullptr; });
    coverViewer_->show();
}

void TitlePage::setStatus(const QString& text) {
    baseStatus_ = text;
    status_->setText(text);
}

void TitlePage::flashStatus(const QString& text, bool failure) {
    status_->setText(text);
    if (failure) {
        QAccessibleEvent event(status_, QAccessible::Alert);
        QAccessible::updateAccessibility(&event);
    }
    QTimer::singleShot(kFlashMs, this, [this] {
        if (!closed_) {
            status_->setText(baseStatus_);
        }
    });
}

bool TitlePage::shutdown(bool forced) {
    if (closed_) {
        return true;
    }
    // Every player acknowledges its own stop before anything is disposed. A
    // refusal keeps the page, its sessions and its dialogs exactly as they are,
    // so the shutdown can be retried; only the destructor, which cannot refuse
    // destruction, disposes a refused session anyway - and even then each
    // widget's destructor releases what it held only after the core has died.
    const QVector<PlayerSession*> open = sessions_;
    for (PlayerSession* session : open) {
        if (!sessions_.contains(session)) {
            // The player's own playerClosed() already disposed this one while an
            // earlier close was running; this is only a pointer comparison.
            continue;
        }
        if (closeSession(session)) {
            continue;
        }
        if (!forced) {
            qWarning().noquote() << "mikai: title shutdown refused (native stop unsettled)";
            return false;
        }
        // Destruction cannot be refused, so the forced stop is named rather than
        // probed; each dialog's destructor still releases what it held only after
        // its core has synchronously died.
        qWarning().noquote()
            << "mikai: forced title teardown (destruction cannot be refused; native claims are "
               "released only after the core is destroyed)";
        disposeSession(session, true);
    }
    closed_ = true;
    fetchWaiters_.clear();
    entryCallbacks_.clear();
    trackWaiters_.clear();
    // Bumping the sequence makes any release probe still in flight a no-op.
    releaseProbeSeq_ += 1;
    qualityCache_.flushNow();
    if (coverViewer_ != nullptr) {
        // close() destroys it via WA_DeleteOnClose, which fires destroyed and
        // clears the pointer. A deleteLater() on top of that is a double free.
        CoverViewer* viewer = coverViewer_;
        coverViewer_ = nullptr;
        viewer->close();
    }
    qDeleteAll(ownedEntries_);
    ownedEntries_.clear();
    return true;
}

}  // namespace mikai::app
