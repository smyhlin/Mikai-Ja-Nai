#include "pages/TitlePage.hpp"

#include "pages/EpisodeGrid.hpp"

#include "Clipboard.hpp"
#include "Labels.hpp"
#include "dialogs/CoverViewer.hpp"
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

#include <QAbstractItemView>
#include <QComboBox>
#include <QDesktopServices>
#include <QEvent>
#include <QFileDialog>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace mikai::app {
namespace {

constexpr int kQualityRole = static_cast<int>(Qt::UserRole) + 1;
constexpr int kBadgeHeight = 20;
constexpr int kBadgePadding = 8;
constexpr int kFlashMs = 1200;

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

/// Paints the release row, then a quality chip on its right edge carrying the
/// best height that release actually serves.
class ReleaseQualityDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const QSize hint = QStyledItemDelegate::sizeHint(option, index);
        const int height = heightAt(index);
        if (height <= 0) {
            return hint;
        }
        // Reserve the chip's height as well as its width: sizing to the row
        // height alone lets a 20px badge overhang its row and collide with the
        // next one.
        return QSize(hint.width() + badgeWidth(option, index),
                     qMax(hint.height(), kBadgeHeight + 2 * theme::kSpacingXs));
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        const int height = heightAt(index);
        if (height <= 0) {
            return;
        }
        painter->save();
        const QString text = QStringLiteral("%1p").arg(height);
        const int width = painter->fontMetrics().horizontalAdvance(text) + kBadgePadding * 2;
        const int right = option.rect.right() - theme::kSpacingMedium / 2;
        const int top = option.rect.top() + (option.rect.height() - kBadgeHeight) / 2;
        const QRectF rect(right - width, top, width, kBadgeHeight);
        QPainterPath chip;
        chip.addRoundedRect(rect, theme::kRadiusSmall, theme::kRadiusSmall);
        painter->fillPath(chip, QColor(QLatin1String(theme::kSurfaceActive)));
        painter->setPen(QColor(QLatin1String(theme::kAccent)));
        painter->drawPath(chip);
        QFont font(painter->font());
        font.setPointSizeF(qMax(8.0, font.pointSizeF() - 1.0));
        painter->setFont(font);
        painter->setPen(QColor(QLatin1String(theme::kText)));
        const QRectF textRect = rect.adjusted(kBadgePadding, 0, -kBadgePadding, 0);
        painter->drawText(textRect, Qt::AlignCenter,
                          painter->fontMetrics().elidedText(text, Qt::ElideRight,
                                                           qRound(textRect.width())));
        painter->restore();
    }

private:
    static int heightAt(const QModelIndex& index) {
        bool ok = false;
        const int height = index.data(kQualityRole).toInt(&ok);
        return ok ? height : 0;
    }

    static int badgeWidth(const QStyleOptionViewItem& option, const QModelIndex& index) {
        const int height = heightAt(index);
        if (height <= 0) {
            return 0;
        }
        return option.fontMetrics.horizontalAdvance(QStringLiteral("%1p").arg(height)) +
               kBadgePadding * 2;
    }
};

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
        if (session_->entry != nullptr && !session_->entry->embedUrl.isEmpty()) {
            QDesktopServices::openUrl(QUrl(session_->entry->embedUrl));
        }
    }
    void requestCopyStreamUrl() override {
        // `copyUrl` is bare by design: the host handed the player a URL, so the
        // host is the one that knows what to copy.
        if (session_->entry != nullptr) {
            copyText(session_->entry->embedUrl);
        }
    }
    void playerClosed() override { page_->closeSession(session_); }
    void playbackFailed() override { page_->recoverStream(session_); }

private:
    TitlePage* page_;
    PlayerSession* session_;
};

TitlePage::TitlePage(Runner& runner, backend::MikaiClient& client, QualityCache& qualityCache,
                     const QString& ref, const QString& posterHint, QWidget* parent)
    : QWidget(parent), runner_(runner), client_(client), qualityCache_(qualityCache),
      ref_(ref), posterHint_(posterHint) {
    buildUi();
    load();
}

TitlePage::~TitlePage() { shutdown(); }

void TitlePage::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(theme::kSpacingLarge, theme::kSpacingLarge, theme::kSpacingLarge,
                             theme::kSpacingLarge);
    root->setSpacing(theme::kSpacingMedium);

    auto* top = new QHBoxLayout();
    auto* back = new QPushButton(tr("← Назад"), this);
    connect(back, &QPushButton::clicked, this, &TitlePage::backRequested);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("status"));
    auto* settingsButton = new QPushButton(this);
    settingsButton->setObjectName(QStringLiteral("settingsChip"));
    settingsButton->setIcon(theme::gearIcon());
    settingsButton->setIconSize(QSize(theme::kIconSize, theme::kIconSize));
    settingsButton->setToolTip(tr("Налаштування токена API"));
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
    pageLayout->setSpacing(theme::kSpacingLarge);

    cover_ = new CoverLabel(tr("Завантаження…"), page);
    cover_->setObjectName(QStringLiteral("titleCover"));
    cover_->setFixedSize(theme::kTitleCoverWidth, theme::kTitleCoverHeight);
    connect(cover_, &CoverLabel::activated, this, &TitlePage::openCoverViewer);
    cover_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(cover_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& position) { coverContextMenu(cover_->mapToGlobal(position)); });
    pageLayout->addWidget(cover_, 0, Qt::AlignTop);

    auto* textColumn = new QVBoxLayout();
    textColumn->setSpacing(theme::kSpacingSmall);
    heading_ = new QLabel(this);
    heading_->setObjectName(QStringLiteral("heading"));
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
    scores_->setStyleSheet(
        QStringLiteral("color: %1; font-weight: 600;").arg(QLatin1String(theme::kAccent)));
    textColumn->addWidget(scores_);

    description_ = new QLabel(this);
    description_->setObjectName(QStringLiteral("titleDescription"));
    description_->setTextFormat(Qt::PlainText);
    description_->setWordWrap(true);
    description_->setAlignment(Qt::AlignTop);
    description_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    textColumn->addWidget(description_);

    auto* disclosure = new QHBoxLayout();
    descriptionButton_ = new QPushButton(tr("Читати далі"), this);
    descriptionButton_->setObjectName(QStringLiteral("textAction"));
    descriptionButton_->hide();
    connect(descriptionButton_, &QPushButton::clicked, this, &TitlePage::toggleDescription);
    metaButton_ = new QPushButton(tr("Докладніше про тайтл"), this);
    metaButton_->setObjectName(QStringLiteral("textAction"));
    metaButton_->setCheckable(true);
    connect(metaButton_, &QPushButton::toggled, this, &TitlePage::toggleMetadata);
    relatedButton_ = new QPushButton(tr("Пов’язані та схожі"), this);
    relatedButton_->setObjectName(QStringLiteral("textAction"));
    relatedButton_->setCheckable(true);
    relatedButton_->hide();
    connect(relatedButton_, &QPushButton::toggled, this, &TitlePage::setRelatedExpanded);
    disclosure->addWidget(descriptionButton_);
    disclosure->addWidget(metaButton_);
    disclosure->addWidget(relatedButton_);
    disclosure->addStretch();
    textColumn->addLayout(disclosure);

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
    links_->setTextFormat(Qt::RichText);
    links_->setOpenExternalLinks(true);
    links_->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(theme::kAccent)));
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

    auto* selectorRow = new QHBoxLayout();
    selectorRow->setSpacing(theme::kSpacingSmall);
    auto* episodesHeading = new QLabel(tr("Серії"), this);
    episodesHeading->setObjectName(QStringLiteral("sectionHeading"));
    selectorRow->addWidget(episodesHeading);
    selector_ = new QComboBox(this);
    selector_->setEnabled(false);
    selector_->setItemDelegate(new ReleaseQualityDelegate(selector_));
    // A combo's popup is a private container, so `view()` exposes no
    // aboutToShow signal. Watching that container for Show is the one reliable
    // hook, and it fires on first open rather than on every repaint: the chips
    // are only visible in the popup, so a title nobody browses never pays for
    // a probe.
    if (QWidget* popup = selector_->view()->parentWidget()) {
        popup->installEventFilter(this);
    }
    connect(selector_, &QComboBox::currentIndexChanged, this, &TitlePage::fillEpisodes);
    selectorRow->addWidget(selector_, 1);
    copyAll_ = new QPushButton(tr("Копіювати всі"), this);
    copyAll_->setEnabled(false);
    connect(copyAll_, &QPushButton::clicked, this, &TitlePage::copyAllUrls);
    selectorRow->addWidget(copyAll_);
    root->addLayout(selectorRow);

    tabs_ = new QTabWidget(this);
    tabs_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    tabs_->setDocumentMode(true);
    root->addWidget(tabs_);
    fitEpisodeSection();
}

void TitlePage::load() {
    setStatus(tr("Завантаження…"));
    runner_.run(
        [this] {
            // Both calls share one task: the detail paints the header and the
            // player payload fills the tabs, and one task keeps them in step.
            return QVariant::fromValue(QPair<backend::AnimeDetail, backend::PlayerResult>(
                client_.getAnime(ref_), client_.getPlayer(ref_)));
        },
        [this](const QVariant& result) {
            if (closed_) {
                return;
            }
            const auto payload =
                result.value<QPair<backend::AnimeDetail, backend::PlayerResult>>();
            onLoaded(payload.first, payload.second);
        },
        [this](const QString& message) {
            if (!closed_) {
                onError(message);
            }
        });
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

    fullDescription_ =
        detail.description.trimmed().isEmpty() ? tr("Опис відсутній.") : detail.description.trimmed();
    descriptionExpanded_ = false;
    description_->setText(descriptionPreview());
    descriptionButton_->setText(tr("Читати далі"));
    descriptionButton_->setVisible(fullDescription_.size() > theme::kTitleDescriptionPreviewChars);

    if (!cover.isEmpty()) {
        loadCover(cover, true);
    } else if (!posterHint_.isEmpty()) {
        loadCover(posterHint_, false);
    } else {
        cover_->setText(tr("Без обкладинки"));
    }

    releases_ = backend::sortReleases(player.releases);
    releaseProbeSeq_ += 1;
    releaseQuality_.clear();
    releaseProbed_.clear();
    {
        const QSignalBlocker blocker(selector_);
        selector_->clear();
        for (int index = 0; index < releases_.size(); ++index) {
            selector_->addItem(releaseLabel(releases_.at(index)));
            selector_->setItemData(index, 0, kQualityRole);
        }
        if (!releases_.isEmpty()) {
            selector_->setCurrentIndex(0);
        }
    }

    if (releases_.isEmpty()) {
        setStatus(tr("Релізів немає"));
        return;
    }
    selector_->setEnabled(true);
    fillEpisodes(0);
}

void TitlePage::onError(const QString& message) {
    rateLabel_->refresh();
    setStatus(message);
    cover_->setZoomable(false);
    cover_->setText(tr("Без обкладинки"));
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
    if (!episodesLabel.isEmpty()) facts << tr("%1 серій").arg(episodesLabel);
    facts_->setText(facts.join(QStringLiteral("   ·   ")));

    QVector<QPair<QString, QString>> rows;
    if (!episodesLabel.isEmpty()) rows.append({tr("Серій"), episodesLabel});
    if (!formatText.isEmpty()) rows.append({tr("Тип"), formatText});
    if (episodes.duration_min.value_or(0) > 0) {
        rows.append({tr("Тривалість"), tr("%1 хв").arg(*episodes.duration_min)});
    }
    if (!statusText.isEmpty()) rows.append({tr("Статус"), statusText});
    if (!seasonText.isEmpty()) rows.append({tr("Сезон"), seasonText});
    if (!detail.source.isEmpty()) {
        rows.append({tr("Першоджерело"), labels::source(detail.source)});
    }
    if (!detail.studio.isEmpty()) rows.append({tr("Студія"), detail.studio});
    if (!detail.country.isEmpty()) rows.append({tr("Країна"), labels::country(detail.country)});
    if (!detail.age_rating.isEmpty()) {
        rows.append({tr("Вікові обмеження"), labels::ageRating(detail.age_rating)});
    }
    QStringList genres;
    for (const backend::GenreTag& genre : detail.genres) {
        const QString text = genre.ua.isEmpty() ? genre.name : genre.ua;
        if (!text.isEmpty()) {
            genres << text;
        }
    }
    if (!genres.isEmpty()) rows.append({tr("Жанри"), genres.join(QStringLiteral(", "))});
    QStringList tags;
    for (const backend::GenreTag& tag : detail.tags) {
        const QString text = tag.ua.isEmpty() ? tag.name : tag.ua;
        if (!text.isEmpty()) {
            tags << text;
        }
    }
    if (!tags.isEmpty()) rows.append({tr("Теги"), tags.join(QStringLiteral(", "))});

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
        relatedLayout_->addWidget(buildRelatedCarousel(tr("Пов’язані тайтли"), relations));
    }
    if (!similar.isEmpty()) {
        relatedLayout_->addWidget(buildRelatedCarousel(tr("Схожі аніме"), similar));
    }
    setRelatedExpanded(true);
}

void TitlePage::loadRecommendations(const backend::AnimeDetail& detail) {
    runner_.run(
        [this] {
            return QVariant::fromValue(
                client_.browsePopular(1, theme::kHomeCarouselMaxTitles));
        },
        [this, detail](const QVariant& result) {
            if (closed_) {
                return;
            }
            QSet<QString> excluded = {ref_, detail.ref};
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
            const bool expand = relatedHost_->isVisible() || !relatedButton_->isVisible();
            relatedLayout_->addWidget(
                buildRelatedCarousel(tr("Рекомендації на Mikai"), entries));
            setRelatedExpanded(expand);
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
    auto* title = new QLabel(heading, section);
    title->setObjectName(QStringLiteral("sectionHeading"));
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
        auto* posterLabel = new QLabel(tr("Постер недоступний"), card);
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
            runner_.runImage(
                [this, posterUrl] { return client_.fetchBytes(posterUrl); },
                [this, posterLabel](const QVariant& result) {
                    if (closed_ || posterLabel == nullptr) {
                        return;
                    }
                    QPixmap pixmap;
                    if (pixmap.loadFromData(result.toByteArray()) && !pixmap.isNull()) {
                        posterLabel->setText(QString());
                        posterLabel->setPixmap(pixmap.scaled(
                            posterLabel->size(), Qt::KeepAspectRatioByExpanding,
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

void TitlePage::setRelatedExpanded(bool expanded) {
    relatedHost_->setVisible(expanded);
    relatedButton_->setVisible(true);
    relatedButton_->setText(expanded ? tr("Сховати схожі") : tr("Пов’язані та схожі"));
    const QSignalBlocker blocker(relatedButton_);
    relatedButton_->setChecked(expanded);
    if (expanded) {
        QTimer::singleShot(0, this, [this] { overviewScroll_->ensureWidgetVisible(relatedHost_); });
    }
}

void TitlePage::toggleDescription() {
    descriptionExpanded_ = !descriptionExpanded_;
    description_->setText(descriptionExpanded_ ? fullDescription_ : descriptionPreview());
    descriptionButton_->setText(descriptionExpanded_ ? tr("Згорнути опис")
                                                      : tr("Читати далі"));
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
    metaButton_->setText(expanded ? tr("Сховати подробиці") : tr("Докладніше про тайтл"));
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

QString TitlePage::releaseLabel(const backend::Release& release, bool withCount) const {
    const QString kindText = labels::kind(release.kind);
    QStringList teams;
    for (const backend::Team& team : release.teams) {
        if (!team.name.isEmpty()) {
            teams << team.name;
        }
    }
    QString label =
        teams.isEmpty() ? kindText : kindText + QStringLiteral(" — ") + teams.join(QLatin1String(", "));
    if (release.is_collab) {
        label += tr(" (колаб)");
    }
    if (withCount) {
        const int count = backend::playableEpisodeCount(release);
        if (count > 0) {
            label += tr(" · %1 серій").arg(count);
        }
    }
    return label;
}

QString TitlePage::releaseTrackLabel(const backend::Release& release) const {
    // Team only, no kind and no episode count: the kind heads the section in the
    // player's track menu, and a track menu is scoped to the episode on screen,
    // so "12 серій" there would read as the length of what is about to play.
    QStringList teams;
    for (const backend::Team& team : release.teams) {
        if (!team.name.isEmpty()) {
            teams << team.name;
        }
    }
    if (!teams.isEmpty()) {
        return release.is_collab ? teams.join(QLatin1String(", ")) + tr(" (колаб)")
                                 : teams.join(QLatin1String(", "));
    }
    return labels::kind(release.kind);
}

void TitlePage::fillEpisodes(int index) {
    while (tabs_->count() > 0) {
        QWidget* page = tabs_->widget(0);
        tabs_->removeTab(0);
        page->deleteLater();
    }
    copyAll_->setEnabled(false);
    fetchWaiters_.clear();
    tabStates_.clear();
    tabEmbeds_.clear();
    if (index < 0 || index >= releases_.size()) {
        return;
    }

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

        QVector<QString> embeds;
        embeds.reserve(state.entries.size());
        for (const EpisodeEntry* entry : state.entries) {
            embeds << entry->embedUrl;
        }
        tabEmbeds_.append(embeds);

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
        auto* grid = new QWidget(scroll);
        grid->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        state.grid = grid;
        state.gridLayout = new QGridLayout(grid);
        state.gridLayout->setContentsMargins(0, theme::kSpacingSmall, 0, 0);
        state.gridLayout->setSpacing(theme::kSpacingSmall);
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
        renderTabPage(stateIndex);
    }

    copyAll_->setEnabled(tabs_->count() > 0);
    setStatus(tabs_->count() > 0 ? QString() : tr("Немає джерел у релізі"));
    fitEpisodeSection();
}

QPushButton* TitlePage::buildEpisodeButton(EpisodeEntry* entry, QWidget* grid) {
    // The grid comes from the tab being rendered, not from the entry: the entry
    // only ever needs its index for the player's playlist, and reading a vector
    // through an unvalidated index is how the pill grid ended up out of bounds.
    auto* button = new QPushButton(entry->number, grid);
    button->setObjectName(QStringLiteral("episodeButton"));
    button->setFixedSize(theme::kEpisodeButtonWidth, theme::kEpisodeButtonHeight);
    entry->label = tr("Серія %1").arg(entry->number);
    entry->button = button;
    button->setToolTip(tr("%1 — дивитися").arg(entry->label));
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
        1, state.grid->width() / (theme::kEpisodeButtonWidth + theme::kSpacingSmall));
    QVector<QWidget*> buttons;
    buttons.reserve(end - start);
    for (int index = start; index < end; ++index) {
        buttons.append(state.entries.at(index)->button);
    }
    layoutEpisodeGrid(state.gridLayout, buttons, columns);
    state.columns = columns;
    state.grid->updateGeometry();
    QTimer::singleShot(0, this, [this] { fitEpisodeSection(); });
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
    // A compact pill per episode: 72px plus the gutter fits a row of 14.
    const int columns = qMax(
        1, state.grid->width() / (theme::kEpisodeButtonWidth + theme::kSpacingSmall));
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
    QTimer::singleShot(0, this, [this] { fitEpisodeSection(); });
}

bool TitlePage::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Show && selector_ != nullptr &&
        watched == selector_->view()->parentWidget()) {
        // The release popup is up: this is the moment a quality probe is worth
        // spending, because that is the only time its chip is on screen.
        probeReleaseQualities();
        return QWidget::eventFilter(watched, event);
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

void TitlePage::fitEpisodeSection() {
    if (tabs_ == nullptr) {
        return;
    }
    const int frame = style()->pixelMetric(QStyle::PM_DefaultFrameWidth);
    const int available = qMax(1, tabs_->width() - 2 * frame);
    for (int index = 0; index < tabStates_.size(); ++index) {
        tabStates_[index].grid->resize(available, tabStates_[index].grid->height());
        reflowGrid(index);
    }
    int gridHeight = 0;
    int pagerHeight = 0;
    for (const TabState& state : tabStates_) {
        gridHeight = qMax(gridHeight, state.grid->sizeHint().height());
        if (state.pager != nullptr && !state.pager->isHidden()) {
            pagerHeight = qMax(pagerHeight, state.pager->sizeHint().height());
        }
    }
    const int tabBarHeight = tabs_->count() > 0 ? tabs_->tabBar()->sizeHint().height() : 0;
    const int gap = pagerHeight > 0 ? theme::kSpacingSmall - theme::kSpacingXs : 0;
    int desired = tabBarHeight + gridHeight + pagerHeight + gap + 2 * frame;
    // A 140-episode page would push the whole window; long lists scroll inside.
    desired = qBound(76, desired, theme::kEpisodeSectionMaxHeight);
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
        flashStatus(tr("У цього плеєра немає відео для цієї серії"));
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
        flashStatus(tr("Завантаження якостей…"));
        runner_.runBulk(
            [embed] { return QVariant::fromValue(backend::qualityStreams(embed)); },
            [this, embed](const QVariant& result) {
                finishFetch(embed, result.value<QMap<int, QString>>(), QString());
            },
            [this, embed](const QString& message) {
                finishFetch(embed, QMap<int, QString>(), message);
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
            flashStatus(tr("У цього плеєра немає відео для цієї серії"));
        } else {
            flashStatus(tr("Не вдалося завантажити якості серії"));
        }
        return;
    }
    if (qualities.isEmpty()) {
        flashStatus(tr("Не вдалося знайти потік для серії"));
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
    entry->button->setToolTip(tr("У цього плеєра немає відео для цієї серії"));
}

void TitlePage::episodeContextMenu(EpisodeEntry* entry, const QPoint& globalPos) {
    QMenu menu(this);
    // The embed page is one URL, but a viewer usually wants the stream. The
    // extraction fires here, on the press, and the rows fill in as it lands -
    // one flight per embed, the same one an episode open uses, so opening a
    // menu never costs a second extraction of a stream already resolved.
    QMenu* copyMenu = menu.addMenu(tr("Копіювати в буфер обміну"));
    QAction* copyUrl = copyMenu->addAction(tr("URL сторінки"));
    copyUrl->setToolTip(tr("Скопіювати посилання на сторінку серії"));
    connect(copyUrl, &QAction::triggered, this, [this, entry] { copyEpisode(entry); });
    copyMenu->addSeparator();
    // A provider with no video never calls back, so it gets a row that says so
    // instead of one that spins forever.
    const bool noSource = qualityCache_.isNoSource(entry->embedUrl);
    QAction* loading = copyMenu->addAction(
        noSource ? tr("Відео відсутнє") : tr("Завантаження якостей…"));
    loading->setEnabled(false);
    loading->setToolTip(noSource ? tr("У цього плеєра немає відео для цієї серії")
                                   : tr("Отримання посилань на відео"));

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

    QAction* external = menu.addAction(tr("Відкрити у зовнішньому плеєрі"));
    external->setToolTip(tr("Відкрити найкращу якість у mpv або VLC"));
    external->setEnabled(!qualityCache_.isNoSource(entry->embedUrl));
    connect(external, &QAction::triggered, this, [this, entry] { openExternalPlayer(entry); });
    QAction* browser = menu.addAction(tr("Відкрити в браузері"));
    browser->setToolTip(tr("Відкрити сторінку серії"));
    connect(browser, &QAction::triggered, this, [this, entry] { openEmbedInBrowser(entry); });
    menu.exec(globalPos);
}

void TitlePage::copyStreamUrl(const QString& url, int height) {
    if (!copyText(url)) {
        flashStatus(tr("Не вдалося скопіювати посилання"));
        return;
    }
    flashStatus(tr("Скопійовано пряме посилання (%1p)").arg(height));
}
void TitlePage::copyEpisode(EpisodeEntry* entry) {
    if (!copyText(entry->embedUrl)) {
        flashStatus(tr("Не вдалося скопіювати URL серії"));
        return;
    }
    flashStatus(qualityCache_.isNoSource(entry->embedUrl)
                    ? tr("URL скопійовано (відео відсутнє у цього плеєра)")
                    : tr("URL скопійовано"));
}

void TitlePage::openEmbedInBrowser(EpisodeEntry* entry) {
    QDesktopServices::openUrl(QUrl(entry->embedUrl));
    flashStatus(tr("Відкрито в браузері"));
}

void TitlePage::openExternalPlayer(EpisodeEntry* entry) {
    ensureQualities(entry, [this](EpisodeEntry* loaded, const QMap<int, QString>& qualities) {
        openExternalStream(loaded, qualities);
    });
}

void TitlePage::openExternalStream(EpisodeEntry* entry, const QMap<int, QString>& qualities) {
    const int height = maxQuality(qualities);
    if (height <= 0) {
        flashStatus(tr("Не знайдено доступної якості відео"));
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
        flashStatus(tr("Зовнішній плеєр не знайдено — встановіть mpv або VLC"));
        return;
    }
    const QString streamUrl = qualities.value(height);
    const QStringList arguments =
        name == QLatin1String("mpv")
            ? QStringList{streamUrl}
            : QStringList{QStringLiteral("--play-and-exit"), streamUrl};
    if (QProcess::startDetached(program, arguments)) {
        flashStatus(tr("Відкрито у %1 · %2p").arg(name).arg(height));
    } else {
        flashStatus(tr("Не вдалося запустити %1").arg(name));
    }
}

void TitlePage::copyAllUrls() {
    const int index = tabs_->currentIndex();
    if (index < 0 || index >= tabEmbeds_.size()) {
        return;
    }
    QStringList urls;
    for (const QString& url : tabEmbeds_.at(index)) {
        if (!url.isEmpty()) {
            urls << url;
        }
    }
    if (urls.isEmpty()) {
        return;
    }
    const QString original = copyAll_->text();
    copyAll_->setText(copyText(urls.join(QLatin1Char('\n')))
                          ? tr("Скопійовано (%1)").arg(urls.size())
                          : tr("Не вдалося скопіювати"));
    QTimer::singleShot(kFlashMs, this, [this, original] { copyAll_->setText(original); });
}

void TitlePage::probeReleaseQualities() {
    // One request per release, not per episode, and only when the list is
    // actually opened: the chip is invisible until the popup is up, so a title
    // whose releases are never browsed never pays for this.
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
        runner_.runBulk(
            [embed] { return QVariant::fromValue(backend::qualityStreams(embed)); },
            [this, index, seq](const QVariant& result) {
                applyReleaseQuality(index, result.value<QMap<int, QString>>(), seq);
            },
            [this, index, seq](const QString&) {
                applyReleaseQuality(index, QMap<int, QString>(), seq);
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
    // Record a probe result, ignoring one that landed after the releases changed.
    if (closed_ || seq != releaseProbeSeq_ || index < 0 || index >= selector_->count()) {
        return;
    }
    const int height = maxQuality(qualities);
    if (height <= 0) {
        return;
    }
    releaseQuality_.insert(index, height);
    selector_->setItemData(index, height, kQualityRole);
    selector_->view()->viewport()->update();
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
        flashStatus(tr("Не вдалося знайти потік для серії"));
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
    // The dialog is the only place an mpv failure surfaces, so show it either
    // way and let it report whatever it actually hits.
    connect(session->dialog, &mplayer::shell::PlayerDialog::runtimeRequested, this,
            [session] { session->dialog->show(); });
    session->dialog->setPlaylist(session->playlist, currentIndex);
    session->dialog->setVariants(episodeVariants(entry->number), entry->embedUrl);
    session->dialog->show();
    session->dialog->startPlayer();
}

void TitlePage::closeSession(PlayerSession* session) {
    if (session == nullptr) {
        return;
    }
    sessions_.removeAll(session);
    auto* dialog = session->dialog;
    delete session->host;
    session->host = nullptr;
    session->dialog = nullptr;
    delete session;
    if (dialog != nullptr) {
        dialog->deleteLater();
    }
}

void TitlePage::recoverStream(PlayerSession* session) {
    // Signed stream URLs expire, so re-extract once before reporting failure.
    // The re-extraction targets whatever track the dialog is on RIGHT NOW: a
    // dub/sub switch replaces the record, so recovery must not rewind the
    // episode to the variant it opened with.
    if (closed_ || session == nullptr || session->entry == nullptr ||
        session->dialog == nullptr) {
        return;
    }
    const QString embed = session->entry->embedUrl;
    const int height = session->dialog->currentHeight();
    // The cached URL is the one that just failed; keeping it would replay the
    // same dead link on the next click instead of extracting again.
    qualityCache_.forget(embed);
    flashStatus(tr("Потік застарів — оновлюю посилання…"));
    runner_.run(
        [embed] { return QVariant::fromValue(backend::qualityStreams(embed)); },
        [this, session, embed, height](const QVariant& result) {
            const QMap<int, QString> qualities = result.value<QMap<int, QString>>();
            if (closed_ || qualities.isEmpty() || session->dialog == nullptr) {
                return;
            }
            qualityCache_.remember(embed, qualities);
            session->dialog->applyRefreshedStreams(toQualities(qualities), height);
        },
        [](const QString&) {});
}

void TitlePage::switchPlayerVariant(PlayerSession* session, const QString& embed,
                                    const QString& label) {
    if (closed_ || session == nullptr) {
        return;
    }
    auto* entry = new EpisodeEntry;
    entry->number = label;
    entry->embedUrl = embed;
    entry->label = label;
    ownedEntries_.append(entry);
    ensureQualities(
        entry, [session](EpisodeEntry* loaded, const QMap<int, QString>& qualities) {
            if (session->dialog == nullptr) {
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
    if (closed_ || session == nullptr || session->dialog == nullptr) {
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
    runner_.runBulk(
        [embed] { return QVariant::fromValue(backend::qualityStreams(embed)); },
        [this, session, embed](const QVariant& result) {
            trackWaiters_[embed].removeAll(session);
            const QMap<int, QString> qualities = result.value<QMap<int, QString>>();
            if (closed_ || qualities.isEmpty() || session->dialog == nullptr) {
                return;
            }
            qualityCache_.remember(embed, qualities);
            session->dialog->setTrackQuality(embed, maxQuality(qualities));
        },
        [this, session, embed](const QString&) { trackWaiters_[embed].removeAll(session); });
}

void TitlePage::playEpisodeFrom(PlayerSession* session, const QVariantMap& item, int index) {
    // Step inside the open player: one fetch, then swap in place.
    if (closed_ || session == nullptr) {
        return;
    }
    const QString label = item.value(QStringLiteral("label")).toString();
    static const QString prefix = QStringLiteral("Серія ");
    const QString number = label.startsWith(prefix) ? label.mid(prefix.size()) : label;
    auto* entry = new EpisodeEntry;
    entry->number = number;
    entry->embedUrl = item.value(QStringLiteral("embed")).toString();
    entry->label = label;
    ownedEntries_.append(entry);
    const QVector<QVariantMap> variants = episodeVariants(number);
    ensureQualities(
        entry, [session, index, variants](EpisodeEntry* loaded, const QMap<int, QString>& qualities) {
            if (session->dialog == nullptr) {
                return;
            }
            session->entry = loaded;
            session->dialog->applyEpisode(loaded->label, loaded->embedUrl, index,
                                          toQualities(qualities), variants);
        });
}

void TitlePage::loadCover(const QString& url, bool allowHint) {
    runner_.runImage(
        [this, url] { return client_.fetchBytes(url); },
        [this, url](const QVariant& result) {
            if (closed_) {
                return;
            }
            QPixmap pixmap;
            if (pixmap.loadFromData(result.toByteArray()) && !pixmap.isNull()) {
                coverPixmap_ = pixmap;
                coverUrl_ = url;
                cover_->setZoomable(true);
                cover_->setToolTip(tr("Натисніть, щоб відкрити обкладинку"));
                cover_->setText(QString());
                cover_->setPixmap(pixmap.scaled(cover_->size(), Qt::KeepAspectRatio,
                                                Qt::SmoothTransformation));
            }
        },
        [this, url, allowHint](const QString&) {
            if (closed_) {
                return;
            }
            if (allowHint && !posterHint_.isEmpty() && posterHint_ != url) {
                loadCover(posterHint_, false);
            } else {
                cover_->setZoomable(false);
                cover_->setText(tr("Без обкладинки"));
            }
        });
}

void TitlePage::coverContextMenu(const QPoint& globalPos) {
    QMenu menu(this);
    QAction* copyUrl = menu.addAction(tr("Копіювати URL обкладинки"));
    copyUrl->setToolTip(tr("Скопіювати адресу зображення"));
    copyUrl->setEnabled(!coverUrl_.isEmpty());
    connect(copyUrl, &QAction::triggered, this, [this] { copyCoverUrl(); });
    QAction* saveAction = menu.addAction(tr("Зберегти обкладинку…"));
    saveAction->setToolTip(tr("Зберегти зображення у файл"));
    saveAction->setEnabled(!coverPixmap_.isNull());
    connect(saveAction, &QAction::triggered, this, [this] { saveCover(); });
    menu.exec(globalPos);
}

void TitlePage::copyCoverUrl() {
    if (coverUrl_.isEmpty()) {
        return;
    }
    if (!copyText(coverUrl_)) {
        flashStatus(tr("Не вдалося скопіювати URL обкладинки"));
        return;
    }
    flashStatus(tr("URL обкладинки скопійовано"));
}

void TitlePage::saveCover() {
    if (coverPixmap_.isNull()) {
        return;
    }
    const QString suffix =
        coverUrl_.endsWith(QStringLiteral(".webp"), Qt::CaseInsensitive) ? QStringLiteral("webp")
                                                                          : QStringLiteral("png");
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Зберегти обкладинку"), tr("cover.%1").arg(suffix),
        tr("Зображення (*.png *.jpg *.webp)"));
    if (path.isEmpty()) {
        return;
    }
    if (coverPixmap_.save(path)) {
        flashStatus(tr("Обкладинку збережено: %1").arg(path));
    } else {
        flashStatus(tr("Не вдалося зберегти обкладинку"));
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

void TitlePage::flashStatus(const QString& text) {
    status_->setText(text);
    QTimer::singleShot(kFlashMs, this, [this] {
        if (!closed_) {
            status_->setText(baseStatus_);
        }
    });
}

void TitlePage::shutdown() {
    if (closed_) {
        return;
    }
    closed_ = true;
    fetchWaiters_.clear();
    entryCallbacks_.clear();
    trackWaiters_.clear();
    // Bumping the sequence makes any release probe still in flight a no-op.
    releaseProbeSeq_ += 1;
    if (qualityCache_.size() > 0) {
        qualityCache_.flush();
    }
    const QVector<PlayerSession*> open = sessions_;
    for (PlayerSession* session : open) {
        mplayer::shell::PlayerDialog* dialog = session->dialog;
        delete session->host;
        session->host = nullptr;
        session->dialog = nullptr;
        sessions_.removeAll(session);
        delete session;
        if (dialog != nullptr) {
            dialog->close();
            dialog->deleteLater();
        }
    }
    if (coverViewer_ != nullptr) {
        // close() destroys it via WA_DeleteOnClose, which fires destroyed and
        // clears the pointer. A deleteLater() on top of that is a double free.
        CoverViewer* viewer = coverViewer_;
        coverViewer_ = nullptr;
        viewer->close();
    }
    qDeleteAll(ownedEntries_);
    ownedEntries_.clear();
}

}  // namespace mikai::app
