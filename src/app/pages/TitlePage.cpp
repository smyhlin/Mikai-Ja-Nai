#include <utility>
#include "pages/TitlePage.hpp"

#include "pages/EpisodeGrid.hpp"

#include "gui/Clipboard.hpp"
#include "gui/ImageDecode.hpp"
#include "Labels.hpp"
#include "dialogs/CoverViewer.hpp"
#include "state/AppearanceStore.hpp"
#include "state/NativePlaybackAssembly.hpp"
#include "state/QualityCache.hpp"
#include "theme/Colors.hpp"
#include "theme/Icons.hpp"
#include "theme/Metrics.hpp"
#include "theme/Palette.hpp"
#include "widgets/FailureView.hpp"
#include "widgets/FlowLayout.hpp"
#include "widgets/PageGlow.hpp"
#include "widgets/RangePager.hpp"
#include "widgets/TitleCard.hpp"
#include "widgets/RateLimitLabel.hpp"
#include "workers/Runner.hpp"

#include <PlayerDialog.hpp>
#include <backend/MikaiClient.hpp>
#include <backend/Releases.hpp>
#include <backend/Streams.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>

#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QBoxLayout>
#include <QBrush>
#include <QButtonGroup>
#include <QColor>
#include <QtCore/QDebug>
#include <QDesktopServices>
#include <QDateTime>
#include <QEvent>
#include <QKeyEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGraphicsBlurEffect>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLocale>
#include <QLayout>
#include <QLinearGradient>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QResizeEvent>
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

/// The cover blurred once per cover, tails included, on a surface small enough
/// that the poster reads as its palette and not as a second face.
QImage blurredCover(const QImage& poster) {
    const QImage small = poster.scaled(theme::kTitleBackdropSourceWidth,
                                       theme::kTitleBackdropSourceHeight,
                                       Qt::KeepAspectRatioByExpanding,
                                       Qt::SmoothTransformation);
    QGraphicsScene scene;
    QGraphicsPixmapItem* item = scene.addPixmap(QPixmap::fromImage(small));
    auto* blur = new QGraphicsBlurEffect;
    blur->setBlurRadius(theme::kTitleBackdropBlurRadius);
    blur->setBlurHints(QGraphicsBlurEffect::QualityHint);
    item->setGraphicsEffect(blur);
    const QRectF bounds = blur->boundingRectFor(item->boundingRect());
    QImage out(bounds.size().toSize(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    scene.render(&painter, QRectF(out.rect()), bounds);
    return out;
}

/// The cover as atmosphere: blurred once per cover, then composited once per
/// size, so a repaint is one blit and the poster is never a second copy.
class PosterBackdrop : public QWidget {
public:
    explicit PosterBackdrop(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setFocusPolicy(Qt::NoFocus);
    }

    void setPoster(const QPixmap& poster) {
        if (poster.cacheKey() == posterKey_) {
            return;
        }
        posterKey_ = poster.cacheKey();
        image_ = poster.isNull() ? QImage() : blurredCover(poster.toImage());
        rebuild();
        update();
    }

    void refreshPalette() {
        rebuild();
        update();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        rebuild();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        if (art_.isNull()) {
            // Opaque paint is promised: an empty backdrop still covers its rect.
            painter.fillRect(rect(), theme::paletteFor(mikai::app::currentAppearance())
                                        .titleBackgroundValue);
            return;
        }
        painter.drawPixmap(0, 0, art_);
    }

private:
    void rebuild() {
        art_ = QPixmap();
        if (image_.isNull() || width() <= 0 || height() <= 0) {
            return;
        }
        const QColor background =
            theme::paletteFor(mikai::app::currentAppearance()).titleBackgroundValue;
        const QSize region(theme::kTitleBackdropWidth, theme::kTitleBackdropHeight);
        QPixmap art(size());
        art.fill(background);
        QPainter painter(&art);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setOpacity(theme::kTitleBackdropAlpha / 255.0);
        painter.drawImage(QRect(-theme::kSpacingLarge, 0, region.width(), region.height()),
                          image_.scaled(region, Qt::KeepAspectRatioByExpanding,
                                        Qt::SmoothTransformation));
        painter.setOpacity(1.0);
        // The bed takes over towards the facts, and the right edge dissolves, so
        // the atmosphere never ends in a line of its own.
        QColor clear = background;
        clear.setAlpha(0);
        QColor soft = background;
        soft.setAlpha(theme::kTitleBackdropFadeAlpha);
        QLinearGradient fade(0, 0, 0, height());
        fade.setColorAt(0.0, clear);
        fade.setColorAt(0.55, soft);
        fade.setColorAt(1.0, background);
        painter.fillRect(rect(), QBrush(fade));
        const int imageLeft = -theme::kSpacingLarge;
        const int imageRight = imageLeft + region.width();
        const int fadeHeight = qMin(height(), region.height());
        const int edgeFade = qMin(theme::kTitleBackdropEdgeFade, region.width() / 2);
        QLinearGradient right(imageRight - edgeFade, 0, imageRight, 0);
        right.setColorAt(0.0, clear);
        right.setColorAt(1.0, background);
        painter.fillRect(QRect(imageRight - edgeFade, 0, edgeFade, fadeHeight), QBrush(right));
        // The crop starts inside the page, so its left edge dissolves too.
        const int leftFade = qMin(edgeFade, theme::kSpacingLarge * 3);
        QLinearGradient left(imageLeft, 0, imageLeft + leftFade, 0);
        left.setColorAt(0.0, background);
        left.setColorAt(1.0, clear);
        painter.fillRect(QRect(imageLeft, 0, leftFade, fadeHeight), QBrush(left));
        art_ = art;
    }

    qint64 posterKey_ = 0;
    QImage image_;
    QPixmap art_;
};

namespace {

constexpr int kFlashMs = 1200;

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

/// A wrapping label inside a vertical layout has to declare height-for-width,
/// or the layout keeps the one-line height and clips a long title or synopsis
/// once the column narrows.
void wrapLabel(QLabel* label) {
    if (label == nullptr) {
        return;
    }
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    policy.setHeightForWidth(true);
    label->setSizePolicy(policy);
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

/// The identifier an episode shows on its pill, from its (possibly fractional)
/// number. Episodes number from anywhere, so this is never assumed to start at
/// 1. The API's label is only the identifier when there is no number: it is
/// usually the caption ("Episode 3"), which the pill cannot hold and the watch
/// panel would then word twice.
QString episodeNumber(const backend::Episode& episode) {
    if (episode.number.isValid() && !episode.number.isNull()) {
        const QString rendered = labels::variant(episode.number);
        if (!rendered.isEmpty()) {
            return rendered;
        }
    }
    if (!episode.label.isEmpty()) {
        return episode.label;
    }
    return QStringLiteral("?");
}

QString teamReference(const backend::Team& team) {
    if (!team.slug.isEmpty()) {
        return team.slug;
    }
    return team.id > 0 ? QString::number(team.id) : QString();
}

QString teamDisplayName(const backend::Team& team) {
    if (!team.name.isEmpty()) {
        return team.name;
    }
    return team.id > 0 ? QObject::tr("Team %1").arg(team.id) : team.slug;
}

QString licenseValueHtml(const QJsonValue& value) {
    if (value.isObject()) {
        QStringList rows;
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            const QString key = it.key().toHtmlEscaped();
            rows << QStringLiteral("<b>%1</b>: %2").arg(key, licenseValueHtml(it.value()));
        }
        return rows.join(QStringLiteral("<br>"));
    }
    if (value.isArray()) {
        QStringList items;
        for (const QJsonValue& item : value.toArray()) {
            const QString rendered = licenseValueHtml(item);
            if (!rendered.isEmpty()) {
                items << rendered;
            }
        }
        return items.join(QStringLiteral("<br>"));
    }
    if (value.isString()) {
        const QString text = value.toString();
        const QUrl url(text);
        if ((url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) &&
            !url.host().isEmpty()) {
            const QString escaped = text.toHtmlEscaped();
            return QStringLiteral("<a href=\"%1\">%1</a>").arg(escaped);
        }
        return text.toHtmlEscaped();
    }
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    return value.isNull() || value.isUndefined() ? QString() : value.toVariant().toString().toHtmlEscaped();
}

}  // namespace


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
    overviewScroll_->setWidgetResizable(true);
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
    bodyLayout->addWidget(column_, 1);

    columnLayout_ = new QVBoxLayout(column_);
    columnLayout_->setContentsMargins(theme::kTitlePageInset, theme::kTitlePageInset,
                                      theme::kTitlePageInset, theme::kTitlePageInset);
    columnLayout_->setSpacing(theme::kTitleSectionSpacing);

    backdrop_ = new PosterBackdrop(column_);
    backdrop_->setObjectName(QStringLiteral("titleBackdrop"));
    backdrop_->show();
    backdrop_->lower();

    buildBody();

    pageGlow_ = new PageGlow(this);
    connect(&appearanceNotifier(), &AppearanceNotifier::changed, this,
            [this](const theme::Appearance& previous) {
                const auto& appearance = currentAppearance();
                if (previous.siteTheme != appearance.siteTheme && backdrop_ != nullptr) {
                    backdrop_->refreshPalette();
                }
                if (previous.radiusStep != appearance.radiusStep ||
                    previous.coverGradient != appearance.coverGradient) {
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
    header_->setFixedHeight(theme::kTitleControlHeight + 2 * theme::kSpacingMedium);
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

    backButton_ = new QPushButton(tr("← Back"), headerRow_);
    backButton_->setObjectName(QStringLiteral("titleBack"));
    backButton_->setCursor(Qt::PointingHandCursor);
    connect(backButton_, &QPushButton::clicked, this, &TitlePage::backRequested);

    homeButton_ = new QPushButton(tr("Home"), headerRow_);
    homeButton_->setObjectName(QStringLiteral("titleHome"));
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
    settingsButton_->setIcon(theme::gearIcon());
    settingsButton_->setIconSize(QSize(theme::kIconSize, theme::kIconSize));
    settingsButton_->setToolTip(tr("API token settings"));
    settingsButton_->setAccessibleName(tr("API token settings"));
    settingsButton_->setCursor(Qt::PointingHandCursor);
    connect(settingsButton_, &QPushButton::clicked, this, &TitlePage::settingsRequested);

    rateLabel_ = new RateLimitLabel(client_, headerRow_);
    headerRowLayout_->addWidget(backButton_);
    headerRowLayout_->addWidget(homeButton_);
    headerRowLayout_->addStretch(1);
    headerRowLayout_->addWidget(status_);
    headerRowLayout_->addWidget(rateLabel_);
    headerRowLayout_->addWidget(settingsButton_);
}

void TitlePage::buildBody() {
    hero_ = new QWidget(column_);
    hero_->setObjectName(QStringLiteral("titleHero"));
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

    posterCredit_ = new QLabel(heroPosterHost_);
    posterCredit_->setObjectName(QStringLiteral("posterCreditLabel"));
    posterCredit_->setTextFormat(Qt::PlainText);
    posterCredit_->setWordWrap(true);
    posterCredit_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    posterLayout->addWidget(posterCredit_);
    posterTeamButton_ = new QPushButton(heroPosterHost_);
    posterTeamButton_->setObjectName(QStringLiteral("posterTeamButton"));
    posterTeamButton_->setFlat(true);
    posterTeamButton_->setCursor(Qt::PointingHandCursor);
    posterTeamButton_->hide();
    connect(posterTeamButton_, &QPushButton::clicked, this, [this] {
        const QString teamRef = posterTeamButton_->property("teamRef").toString();
        if (!teamRef.isEmpty()) {
            Q_EMIT teamRequested(teamRef);
        }
    });
    posterLayout->addWidget(posterTeamButton_);
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
    textColumn->addWidget(heading_);

    subtitle_ = new QLabel(hero_);
    subtitle_->setObjectName(QStringLiteral("titleSubtitle"));
    subtitle_->setTextFormat(Qt::PlainText);
    wrapLabel(subtitle_);
    subtitle_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    textColumn->addWidget(subtitle_);

    textColumn->addSpacing(theme::kSpacingSmall);
    accentRule_ = new QLabel(hero_);
    accentRule_->setObjectName(QStringLiteral("titleAccentRule"));
    accentRule_->setFixedSize(theme::kTitleAccentRuleWidth, theme::kTitleAccentRuleHeight);
    accentRule_->setAttribute(Qt::WA_StyledBackground, true);
    accentRule_->setAccessibleName(QString());
    textColumn->addWidget(accentRule_, 0, Qt::AlignLeft);
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
    textColumn->addWidget(scores_);

    textColumn->addSpacing(theme::kSpacingSmall);
    description_ = new QLabel(hero_);
    description_->setObjectName(QStringLiteral("titleDescription"));
    description_->setTextFormat(Qt::PlainText);
    wrapLabel(description_);
    description_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    description_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    textColumn->addWidget(description_);

    textColumn->addSpacing(theme::kSpacingMedium);
    disclosureRow_ = new QWidget(hero_);
    disclosureRow_->setObjectName(QStringLiteral("titleDisclosureActions"));
    // The actions wrap: their widest translation must not pan a compact column.
    QSizePolicy disclosurePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    disclosurePolicy.setHeightForWidth(true);
    disclosureRow_->setSizePolicy(disclosurePolicy);
    auto* disclosure = new FlowLayout(theme::kSpacingSmall, theme::kSpacingSmall);
    disclosureRow_->setLayout(disclosure);
    descriptionButton_ = new QPushButton(tr("Read more"), disclosureRow_);
    descriptionButton_->setObjectName(QStringLiteral("textAction"));
    descriptionButton_->setCursor(Qt::PointingHandCursor);
    descriptionButton_->hide();
    connect(descriptionButton_, &QPushButton::clicked, this, &TitlePage::toggleDescription);
    metaButton_ = new QPushButton(tr("More details"), disclosureRow_);
    metaButton_->setObjectName(QStringLiteral("textAction"));
    metaButton_->setCheckable(true);
    metaButton_->setCursor(Qt::PointingHandCursor);
    connect(metaButton_, &QPushButton::toggled, this, &TitlePage::toggleMetadata);
    relatedButton_ = new QPushButton(tr("Related and similar"), disclosureRow_);
    relatedButton_->setObjectName(QStringLiteral("textAction"));
    relatedButton_->setCheckable(true);
    relatedButton_->setCursor(Qt::PointingHandCursor);
    relatedButton_->hide();
    // A press of this button is the one expansion that scrolls the overview.
    connect(relatedButton_, &QPushButton::toggled, this,
            [this](bool checked) { setRelatedExpanded(checked, true); });
    disclosure->addWidget(descriptionButton_);
    disclosure->addWidget(metaButton_);
    disclosure->addWidget(relatedButton_);
    // The row appears with the first real detail: no action opens an empty sheet.
    disclosureRow_->hide();
    textColumn->addWidget(disclosureRow_);

    textColumn->addSpacing(theme::kSpacingMedium);
    links_ = new QLabel(hero_);
    links_->setObjectName(QStringLiteral("titleLinks"));
    links_->setTextFormat(Qt::RichText);
    links_->setOpenExternalLinks(true);
    wrapLabel(links_);
    textColumn->addWidget(links_);

    textColumn->addStretch(1);
    heroLayout_->addLayout(textColumn, 1);
    columnLayout_->addWidget(hero_);

    // Page sections, not hero text: full content width in the same scroll.
    relatedHost_ = new QWidget(column_);
    relatedHost_->setObjectName(QStringLiteral("titleRelated"));
    relatedLayout_ = new QVBoxLayout(relatedHost_);
    relatedLayout_->setContentsMargins(0, 0, 0, 0);
    relatedLayout_->setSpacing(theme::kTitleSectionSpacing);
    relatedHost_->hide();
    columnLayout_->addWidget(relatedHost_);

    episodesHost_ = new QWidget(column_);
    episodesHost_->setObjectName(QStringLiteral("titleEpisodes"));
    auto* episodesLayout = new QVBoxLayout(episodesHost_);
    episodesLayout->setContentsMargins(0, 0, 0, 0);
    episodesLayout->setSpacing(theme::kSpacingMedium);

    episodeHeaderHost_ = new QWidget(episodesHost_);
    episodeHeaderHost_->setObjectName(QStringLiteral("titleEpisodeHeader"));
    episodeHeaderHost_->setAttribute(Qt::WA_StyledBackground, true);
    auto* episodeHeader = new QHBoxLayout(episodeHeaderHost_);
    episodeHeader->setContentsMargins(0, 0, 0, theme::kSpacingSmall);
    episodeHeader->setSpacing(theme::kSpacingMedium);
    episodesHeading_ = new QLabel(tr("Episodes"), episodeHeaderHost_);
    episodesHeading_->setObjectName(QStringLiteral("episodeSectionHeading"));
    episodeCount_ = new QLabel(episodeHeaderHost_);
    episodeCount_->setObjectName(QStringLiteral("titleEpisodeCount"));
    episodeCount_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    episodeCount_->setWordWrap(false);
    episodeHeader->addWidget(episodesHeading_);
    episodeHeader->addStretch(1);
    episodeHeader->addWidget(episodeCount_);
    episodesLayout->addWidget(episodeHeaderHost_);

    episodeAvailability_ = new QLabel(episodesHost_);
    episodeAvailability_->setObjectName(QStringLiteral("episodeAvailabilityLabel"));
    episodeAvailability_->setTextFormat(Qt::PlainText);
    episodeAvailability_->setWordWrap(true);
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

    releaseChipsHost_ = new QWidget(episodesHost_);
    releaseChipsHost_->setObjectName(QStringLiteral("releaseChipsHost"));
    releaseRowsLayout_ = new QVBoxLayout(releaseChipsHost_);
    releaseRowsLayout_->setContentsMargins(0, 0, 0, 0);
    releaseRowsLayout_->setSpacing(theme::kSpacingXs);
    episodesLayout->addWidget(releaseChipsHost_);

    releaseGroup_ = new QButtonGroup(this);
    releaseGroup_->setExclusive(true);
    connect(releaseGroup_, &QButtonGroup::idClicked, this, [this](int index) {
        if (index != activeRelease_) {
            fillEpisodes(index);
        }
        probeReleaseQualities();
    });

    watchPanel_ = new QWidget(episodesHost_);
    watchPanel_->setObjectName(QStringLiteral("titleWatchPanel"));
    watchPanel_->setAttribute(Qt::WA_StyledBackground, true);
    watchPanel_->setMinimumHeight(theme::kTitleControlHeight * 2);
    auto* watchLayout = new QHBoxLayout(watchPanel_);
    watchLayout->setContentsMargins(theme::kTitlePageInset, theme::kSpacingMedium,
                                    theme::kTitlePageInset, theme::kSpacingMedium);
    watchLayout->setSpacing(theme::kSpacingMedium);
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
    watchText->addStretch(1);
    watchLayout->addLayout(watchText, 1);
    watchWatchedButton_ = new QPushButton(tr("Watched"), watchPanel_);
    watchWatchedButton_->setObjectName(QStringLiteral("titleWatchWatchedToggle"));
    watchWatchedButton_->setCheckable(true);
    watchWatchedButton_->setMinimumHeight(theme::kTitleControlHeight);
    watchWatchedButton_->setCursor(Qt::PointingHandCursor);
    connect(watchWatchedButton_, &QPushButton::toggled, this, [this](bool checked) {
        if (watchEntry_ == nullptr || !entryIsPlayable(watchEntry_)) {
            return;
        }
        setEpisodeWatched(watchEntry_->number, checked);
    });
    watchLayout->addWidget(watchWatchedButton_, 0, Qt::AlignVCenter);
    watchButton_ = new QPushButton(tr("Watch"), watchPanel_);
    watchButton_->setObjectName(QStringLiteral("titleWatchButton"));
    watchButton_->setMinimumHeight(theme::kTitleControlHeight);
    watchButton_->setCursor(Qt::PointingHandCursor);
    connect(watchButton_, &QPushButton::clicked, this, [this] {
        if (watchEntry_ != nullptr) {
            playEpisode(watchEntry_);
        }
    });
    watchLayout->addWidget(watchButton_, 0, Qt::AlignVCenter);
    watchPanel_->hide();
    episodesLayout->addWidget(watchPanel_);

    tabs_ = new QTabWidget(episodesHost_);
    tabs_->setObjectName(QStringLiteral("episodeTabs"));
    tabs_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    tabs_->setDocumentMode(true);
    tabs_->tabBar()->setExpanding(false);
    tabs_->tabBar()->setDrawBase(false);
    tabs_->tabBar()->setUsesScrollButtons(true);
    episodesLayout->addWidget(tabs_);

    episodeLegend_ = new QWidget(episodesHost_);
    episodeLegend_->setObjectName(QStringLiteral("titleEpisodeLegend"));
    auto* legendRow = new QHBoxLayout(episodeLegend_);
    legendRow->setContentsMargins(0, theme::kSpacingXs, 0, 0);
    legendRow->setSpacing(theme::kSpacingSmall);
    const auto addLegendItem = [this, legendRow](const QString& swatchName, const QSize& swatchSize,
                                                 QLabel** caption) {
        auto* swatch = new QWidget(episodeLegend_);
        swatch->setObjectName(swatchName);
        swatch->setFixedSize(swatchSize);
        *caption = new QLabel(episodeLegend_);
        (*caption)->setObjectName(QStringLiteral("episodeLegendLabel"));
        (*caption)->setTextFormat(Qt::PlainText);
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
    columnLayout_->addStretch(1);
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
    detailsClose_->setText(QStringLiteral("✕"));
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
    detailsThumb_->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* identityText = new QVBoxLayout();
    identityText->setContentsMargins(0, 0, 0, 0);
    identityText->setSpacing(theme::kSpacingXs);
    detailsTitle_ = new QLabel(metaHost_);
    detailsTitle_->setObjectName(QStringLiteral("titleDetailsTitle"));
    detailsTitle_->setTextFormat(Qt::PlainText);
    wrapLabel(detailsTitle_);
    detailsOriginal_ = new QLabel(metaHost_);
    detailsOriginal_->setObjectName(QStringLiteral("muted"));
    detailsOriginal_->setTextFormat(Qt::PlainText);
    wrapLabel(detailsOriginal_);
    detailsScore_ = new QLabel(metaHost_);
    detailsScore_->setObjectName(QStringLiteral("titleScores"));
    wrapLabel(detailsScore_);
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
    if (compact != compact_) {
        compact_ = compact;
        // Theme-owned type sizes key off this property, so the switch has to
        // re-resolve the scoped sheet once rather than set fonts here.
        setProperty("compact", compact);
        repolishTitleSubtree();
        heroLayout_->setDirection(compact ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
        heroLayout_->setSpacing(compact ? theme::kSpacingLarge : theme::kTitleHeroSpacing);
        const Qt::Alignment posterAlignment =
            compact ? Qt::Alignment(Qt::AlignHCenter)
                    : Qt::Alignment(Qt::AlignLeft | Qt::AlignTop);
        heroLayout_->setAlignment(heroPosterHost_, posterAlignment);
        const int coverWidth =
            compact ? theme::kTitleCompactCoverWidth : theme::kTitleHeroCoverWidth;
        const int coverHeight =
            compact ? theme::kTitleCompactCoverHeight : theme::kTitleHeroCoverHeight;
        cover_->setFixedSize(coverWidth, coverHeight);
        heroPosterHost_->setFixedWidth(coverWidth);
        studio_->setAlignment((compact ? Qt::AlignHCenter : Qt::AlignLeft) | Qt::AlignTop);
        if (coverState_ == CoverState::Ready && !coverPixmap_.isNull()) {
            applyCoverPixmap();
        }
    }
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
    if (watchPanel_ != nullptr && watchPanel_->layout() != nullptr) {
        watchPanel_->layout()->setContentsMargins(inset, theme::kSpacingMedium, inset,
                                                  theme::kSpacingMedium);
    }
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
    const int height = qMin(theme::kTitleBackdropHeight, qMax(0, column_->height()));
    backdrop_->setGeometry(0, 0, column_->width(), height);
}

void TitlePage::updateDetailsGeometry() {
    if (detailsOverlay_ == nullptr) {
        return;
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
    const QList<QWidget*> descendants = findChildren<QWidget*>();
    for (QWidget* widget : descendants) {
        pageStyle->unpolish(widget);
        pageStyle->polish(widget);
        widget->updateGeometry();
    }
    pageStyle->unpolish(this);
    pageStyle->polish(this);
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
    rateLabel_->refresh();
    failure_->clear();
    hasFailure_ = false;
    const QString name = labels::titleName(detail.titles);
    heading_->setText(name == QLatin1String("?") ? ref_ : name);
    subtitle_->setText(detail.titles.english.isEmpty() ? detail.titles.original
                                                       : detail.titles.english);
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
    descriptionButton_->setVisible(fullDescription_.size() > theme::kTitleDescriptionPreviewChars);

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

    player_ = player;
    updateLicenseInfo(player_);
    if (releases_.isEmpty()) {
        setStatusKind(playbackRestricted ? BaseStatus::Licensed : BaseStatus::NoReleases);
        return;
    }
    releaseChips_.first()->setChecked(true);
    fillEpisodes(0);
}

void TitlePage::onError(const backend::Failure& failure) {
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
    scores_->setText(scoresText(detail_));
    disclosureRow_->setVisible(true);

    // The hero caption and the sheet read one model; both hide what it lacks.
    studio_->setText(detail_.studio);
    studio_->setVisible(!detail_.studio.isEmpty());
    const QString detailName = labels::titleName(detail_.titles);
    detailsTitle_->setText(detailName == QLatin1String("?") ? ref_ : detailName);
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

    updatePosterCredit();

    const QString statusText = labels::status(detail_.status);
    const QString formatText = labels::format(detail_.format);
    const QString seasonText = labels::season(detail_.start_date);
    QStringList facts;
    if (!statusText.isEmpty()) facts << statusText;
    if (!formatText.isEmpty()) facts << formatText;
    if (!seasonText.isEmpty()) facts << seasonText;
    facts << episodesLabel;
    facts_->setText(facts.join(QStringLiteral(" · ")));

    QVector<std::pair<QString, QString>> rows;
    rows.append({tr("Episode availability"), episodesLabel});
    if (!formatText.isEmpty()) rows.append({tr("Type"), formatText});
    if (episodes.duration_min.value_or(0) > 0) {
        rows.append({tr("Duration"), tr("%1 min").arg(*episodes.duration_min)});
    }
    if (!statusText.isEmpty()) rows.append({tr("Status"), statusText});
    if (!seasonText.isEmpty()) rows.append({tr("Season"), seasonText});
    if (!detail_.source.isEmpty()) {
        rows.append({tr("Original source"), labels::source(detail_.source)});
    }
    if (!detail_.studio.isEmpty()) rows.append({tr("Studio"), detail_.studio});
    if (!detail_.country.isEmpty()) {
        rows.append({tr("Country"), labels::country(detail_.country)});
    }
    if (!detail_.age_rating.isEmpty()) {
        rows.append({tr("Age rating"), labels::ageRating(detail_.age_rating)});
    }
    QStringList genres;
    for (const backend::GenreTag& genre : detail_.genres) {
        const QString text = genre.ua.isEmpty() ? genre.name : genre.ua;
        if (!text.isEmpty()) {
            genres << text;
        }
    }
    if (!genres.isEmpty()) rows.append({tr("Genres"), genres.join(QStringLiteral(", "))});
    QStringList tags;
    for (const backend::GenreTag& tag : detail_.tags) {
        const QString text = tag.ua.isEmpty() ? tag.name : tag.ua;
        if (!text.isEmpty()) {
            tags << text;
        }
    }
    if (!tags.isEmpty()) rows.append({tr("Tags"), tags.join(QStringLiteral(", "))});

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
        auto* key = new QLabel(entry.first.toUpper(), cell);
        key->setObjectName(QStringLiteral("muted"));
        key->setWordWrap(true);
        auto* value = new QLabel(entry.second, cell);
        wrapLabel(value);
        value->setTextFormat(Qt::PlainText);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        cellLayout->addWidget(key);
        cellLayout->addWidget(value);
        grid->addWidget(cell, index / 2, index % 2, Qt::AlignTop);
        ++index;
    }
    detailsFactsLayout_->addWidget(gridHost);

    auto* synopsisHost = new QWidget(detailsFactsHost_);
    auto* synopsisLayout = new QVBoxLayout(synopsisHost);
    synopsisLayout->setContentsMargins(0, 0, 0, 0);
    synopsisLayout->setSpacing(theme::kSpacingXs);
    auto* synopsisKey = new QLabel(tr("Synopsis").toUpper(), synopsisHost);
    synopsisKey->setObjectName(QStringLiteral("muted"));
    auto* synopsis = new QLabel(fullDescription_.isEmpty() ? tr("No description available.")
                                                           : fullDescription_,
                               synopsisHost);
    wrapLabel(synopsis);
    synopsis->setTextFormat(Qt::PlainText);
    synopsis->setTextInteractionFlags(Qt::TextSelectableByMouse);
    synopsisLayout->addWidget(synopsisKey);
    synopsisLayout->addWidget(synopsis);
    detailsFactsLayout_->addWidget(synopsisHost);

    if (coverPixmap_.isNull()) {
        detailsThumb_->setText(tr("No cover"));
    }

    QVector<std::pair<QString, QString>> links = backend::externalLinks(detail_);
    // The site serves the title at /anime/<id>-<slug> and 404s on either half
    // alone, so the link is built from both rather than preferring the slug.
    const QString page =
        backend::animePageUrl(detail_.ref, detail_.ids.value(QStringLiteral("slug")).toString());
    if (!detail_.ref.isEmpty()) {
        links.prepend({tr("Mikai"), page});
    }
    const QString accent = QString::fromLatin1(theme::kTitleAccent);
    const QString muted = QString::fromLatin1(theme::kTitleMuted);
    QStringList anchors;
    for (const auto& link : links) {
        anchors << QStringLiteral("<a style=\"color: %1; text-decoration: none; font-weight: 600;\" href=\"%2\">%3</a>")
                       .arg(accent, link.second.toHtmlEscaped(), link.first.toHtmlEscaped());
    }
    links_->setText(anchors.join(QStringLiteral("<span style=\"color: %1;\"> &nbsp;·&nbsp; </span>")
                                     .arg(muted)));
}

void TitlePage::populateRelated(const backend::AnimeDetail& detail) {
    // The carousels are being rebuilt from a fresh detail, so the registrations
    // of the ones going away are dropped with them.
    relatedHeadings_.clear();
    relatedPagerButtons_.clear();
    relatedKindLabels_.clear();
    relatedPosterLabels_.clear();
    while (relatedLayout_->count() > 0) {
        QLayoutItem* item = relatedLayout_->takeAt(0);
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    QVector<std::pair<QString, backend::AnimeSummary>> relations;
    for (int index = 0;
         index < detail.relations.size() && index < theme::kRelatedCarouselMax; ++index) {
        if (!detail.relations.at(index).anime.ref.isEmpty()) {
            relations.append({detail.relations.at(index).kind, detail.relations.at(index).anime});
        }
    }
    QVector<std::pair<QString, backend::AnimeSummary>> similar;
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
        relatedLayout_->addWidget(buildRelatedCarousel(QStringLiteral("related"), relations));
    }
    if (!similar.isEmpty()) {
        relatedLayout_->addWidget(buildRelatedCarousel(QStringLiteral("similar"), similar));
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
            QVector<std::pair<QString, backend::AnimeSummary>> entries;
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
                self->buildRelatedCarousel(QStringLiteral("recommendation"), entries));
            self->setRelatedExpanded(expand, false);
        },
        [](const backend::Failure&) {});
}

QWidget* TitlePage::buildRelatedCarousel(
    const QString& headingKey,
    const QVector<std::pair<QString, backend::AnimeSummary>>& entries) {
    auto* section = new QWidget(relatedHost_);
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::kSpacingSmall - theme::kSpacingXs);

    // The host carries the section rule across the content column, not the label.
    auto* headerHost = new QWidget(section);
    headerHost->setObjectName(QStringLiteral("titleSectionHeader"));
    headerHost->setAttribute(Qt::WA_StyledBackground, true);
    auto* header = new QHBoxLayout(headerHost);
    header->setContentsMargins(0, 0, 0, theme::kSpacingSmall);
    header->setSpacing(theme::kSpacingSmall);
    auto* title = new QLabel(relatedHeadingText(headingKey), section);
    title->setObjectName(QStringLiteral("titleSectionHeading"));
    // Registered by its semantic key: a language switch re-words this label in
    // place instead of rebuilding the carousel and its posters.
    relatedHeadings_.append({headingKey, title});
    header->addWidget(title);
    header->addStretch();
    auto* previous = new QPushButton(section);
    previous->setIcon(theme::chevronLeftIcon(16));
    previous->setToolTip(tr("Previous page"));
    auto* next = new QPushButton(section);
    next->setIcon(theme::chevronRightIcon(16));
    next->setToolTip(tr("Next page"));
    relatedPagerButtons_.emplaceBack(previous, next);
    for (QPushButton* button : {previous, next}) {
        button->setObjectName(QStringLiteral("carouselControl"));
        button->setIconSize(QSize(16, 16));
        button->setCursor(Qt::PointingHandCursor);
        header->addWidget(button);
    }
    layout->addWidget(headerHost);

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
        const QString name = labels::titleName(anime.titles);
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
        // Registered only for its fallback: a switch re-words the label while it
        // is still empty, and leaves a landed poster alone.
        relatedPosterLabels_.append(posterLabel);
        cardLayout->addWidget(posterLabel, 0, Qt::AlignHCenter);

        auto* kind = new QLabel(labels::relation(entry.first), card);
        kind->setObjectName(QStringLiteral("carouselKind"));
        kind->setAlignment(Qt::AlignCenter);
        kind->setAttribute(Qt::WA_TransparentForMouseEvents);
        // Keyed by the raw relation, so the caption is re-derived on a switch.
        relatedKindLabels_.append({entry.first, kind});
        cardLayout->addWidget(kind);

        auto* titleLabel = new QLabel(card);
        titleLabel->setObjectName(QStringLiteral("carouselTitle"));
        titleLabel->setTextFormat(Qt::PlainText);
        titleLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        titleLabel->setWordWrap(false);
        titleLabel->setFixedHeight(theme::kCarouselTitleHeight);
        titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        // Match the carouselTitle rule so the elide is measured in its font.
        QFont titleFont = titleLabel->font();
        titleFont.setPixelSize(theme::kTitleMetaPixelSize);
        titleFont.setWeight(QFont::Medium);
        const int maxLineWidth = theme::kCarouselCardWidth - 2 * theme::kSpacingSmall - 6;
        titleLabel->setText(TitleCard::elideTwoLines(name, titleFont, maxLineWidth));
        cardLayout->addWidget(titleLabel);
        cards->addWidget(card);

        const QString posterUrl = backend::posterUrl(anime);
        if (!posterUrl.isEmpty()) {
            // One task per poster, on the image pool: a shared poster costs one
            // download, and a fan-out costs no API budget.
            backend::MikaiClient* client = &client_;
            const QPointer<TitlePage> self(this);
            const QPointer<QLabel> poster(posterLabel);
            const QSize target = posterLabel->size();
            runner_.runImage(
                [client, posterUrl, target] {
                    return QVariant::fromValue(gui::decodeImage(client->fetchBytes(posterUrl), target));
                },
                [self, poster](const QVariant& result) {
                    if (self.isNull() || poster.isNull()) {
                        return;
                    }
                    auto decoded = result.value<gui::DecodedImage>();
                    if (decoded.image.isNull()) {
                        qWarning().noquote() << "TitlePage: related artwork decode failed:"
                                             << self->ref_ << decoded.error;
                        return;
                    }
                    const QPixmap pixmap = QPixmap::fromImage(std::move(decoded.image));
                    poster->setText(QString());
                    poster->setPixmap(TitleCard::rounded(pixmap, poster->size()));
                },
                [](const backend::Failure& failure) {
                    qWarning().noquote() << "TitlePage: related artwork fetch failed:"
                                         << backend::failureLine(failure);
                });
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
    setDetailsOpen(expanded);
}

void TitlePage::setDetailsOpen(bool open) {
    if (detailsOverlay_ == nullptr || open == detailsOpen_) {
        return;
    }
    detailsOpen_ = open;
    detailsOverlay_->setVisible(open);
    if (open) {
        updateDetailsGeometry();
        detailsOverlay_->raise();
        if (detailsScroll_ != nullptr) {
            detailsScroll_->verticalScrollBar()->setValue(0);
        }
        if (detailsClose_ != nullptr) {
            detailsClose_->setFocus(Qt::OtherFocusReason);
        }
    } else {
        QWidget* focused = QApplication::focusWidget();
        if (focused != nullptr && detailsPanel_->isAncestorOf(focused)) {
            metaButton_->setFocus(Qt::OtherFocusReason);
        }
        // Scrim, close and Escape uncheck the disclosure without re-entering it.
        const QSignalBlocker blocker(metaButton_);
        metaButton_->setChecked(false);
    }
    metaButton_->setText(open ? tr("Hide details") : tr("More details"));
}

void TitlePage::selectWatchEntry(EpisodeEntry* entry) {
    watchEntry_ = entry;
    // Only the pill grid on screen holds buttons that are still live: a page
    // that left the grid keeps pointers to buttons already queued for deletion,
    // so those are re-worded when their page is built again, never touched here.
    const int stateIndex = tabs_ != nullptr ? tabs_->currentIndex() : -1;
    if (stateIndex >= 0 && stateIndex < tabStates_.size() && tabStates_.at(stateIndex).rendered) {
        const TabState& state = tabStates_.at(stateIndex);
        const int start = state.page * state.pageSize;
        const int end = qMin(start + state.pageSize, state.entries.size());
        for (int index = start; index < end; ++index) {
            EpisodeEntry* candidate = state.entries.at(index);
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
    if (entry->paintedCurrent != current) {
        entry->paintedCurrent = current;
        // Avoid repolishing a whole page of pills when only one mark changed.
        button->setProperty("current", current);
        button->style()->unpolish(button);
        button->style()->polish(button);
        button->update();
    }
    if (entry->paintedWatched != watched) {
        entry->paintedWatched = watched;
        if (entry->watchedMark != nullptr) {
            entry->watchedMark->setVisible(watched);
        }
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
    button->setToolTip(tr("%1 — watch").arg(entry->label));
    button->setAccessibleDescription(description);
}

void TitlePage::refreshEpisodeStates() {
    for (const TabState& state : tabStates_) {
        if (!state.rendered) {
            continue;
        }
        const int start = state.page * state.pageSize;
        const int end = qMin(start + state.pageSize, state.entries.size());
        for (int index = start; index < end; ++index) {
            applyEpisodeState(state.entries.at(index));
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
        watchButton_->setEnabled(false);
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
    watchMeta_->setText(meta.join(QStringLiteral(" · ")));
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
}

QString TitlePage::episodeType(const EpisodeEntry* entry) const {
    if (entry == nullptr) {
        return {};
    }
    backend::Episode episode;
    episode.number = entry->sourceNumber;
    episode.kind = entry->kind;
    return backend::episodeKindFor(detail_, episode);
}

void TitlePage::updatePosterCredit() {
    if (posterCredit_ == nullptr) {
        return;
    }
    const backend::UaPoster* creditPoster = backend::coverPoster(detail_);
    const bool showingDifferentCover =
        !coverUrl_.isEmpty() && coverUrl_ != backend::coverUrl(detail_);
    if (creditPoster == nullptr || showingDifferentCover || coverState_ == CoverState::Missing) {
        posterCredit_->clear();
        posterCredit_->hide();
        if (posterTeamButton_ != nullptr) posterTeamButton_->hide();
        return;
    }
    posterCredit_->show();
    const bool selected = creditPoster->is_selected;
    posterCredit_->setText(
        creditPoster->author.isEmpty()
            ? (selected ? tr("Selected poster · credit unavailable")
                        : tr("Poster credit unavailable"))
            : (selected ? tr("Selected poster by %1").arg(creditPoster->author)
                        : tr("Poster by %1").arg(creditPoster->author)));
    if (posterTeamButton_ == nullptr) {
        return;
    }
    if (creditPoster->team.has_value()) {
        const QString teamRef = teamReference(*creditPoster->team);
        posterTeamButton_->setText(teamDisplayName(*creditPoster->team));
        posterTeamButton_->setProperty("teamRef", teamRef);
        posterTeamButton_->setVisible(!teamRef.isEmpty());
    } else {
        posterTeamButton_->hide();
    }
}

QString TitlePage::episodeTypeCaption(const QString& type) const {
    if (type == QLatin1String("mangaCanon")) return tr("Manga canon");
    if (type == QLatin1String("animeCanon")) return tr("Anime canon");
    if (type == QLatin1String("filler")) return tr("Filler");
    if (type == QLatin1String("mixed")) return tr("Mixed");
    return type;
}

QString TitlePage::episodeTypeBadgeText(const QString& type) const {
    if (type == QLatin1String("mangaCanon")) return tr("M");
    if (type == QLatin1String("animeCanon")) return tr("A");
    if (type == QLatin1String("filler")) return tr("F");
    if (type == QLatin1String("mixed")) return tr("Mix");
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
    episodeCount_->setText(count > 0 ? tr("%n episode(s)", nullptr, count) : QString());
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
        label += tr(" · %n episode(s)", nullptr, count);
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

void TitlePage::rebuildReleaseChips() {
    releaseKindLabels_.clear();
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
            chip->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
            chip->setFixedHeight(theme::kTitleControlHeight);
            releaseGroup_->addButton(chip, index);
            releaseChips_[index] = chip;
            chips->addWidget(chip);
            chip->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(chip, &QWidget::customContextMenuRequested, this,
                    [this, chip](const QPoint& position) {
                        releaseContextMenu(chip, chip->mapToGlobal(position));
                    });
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

void TitlePage::releaseContextMenu(QPushButton* chip, const QPoint& globalPos) {
    const int index = chip == nullptr ? -1 : releaseGroup_->id(chip);
    if (index < 0 || index >= releases_.size()) {
        return;
    }
    QMenu menu(this);
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
    // The tab handler renders a page and re-selects an episode, and both would
    // index the vector being replaced mid-teardown.
    const QSignalBlocker tabsBlocker(tabs_);
    while (tabs_->count() > 0) {
        QWidget* page = tabs_->widget(0);
        tabs_->removeTab(0);
        page->deleteLater();
    }
    fetchWaiters_.clear();
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
            entry->sourceNumber = episode.number;
            entry->label = episodeCaption(number);
            entry->kind = episode.kind;
            entry->embedUrl = source.embed_url;
            ownedEntries_.append(entry);
            grouped[providerFor(source)].append(entry);
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

    for (auto it = grouped.constBegin(); it != grouped.constEnd(); ++it) {
        TabState state;
        state.provider = it.key();
        state.entries = it.value();
        state.pageSize = theme::kEpisodesPerPage;
        if (numbered) {
            QMap<int, EpisodeEntry*> byNumber;
            for (EpisodeEntry* entry : state.entries) {
                byNumber.insert(entry->number.toInt(), entry);
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
        state.pager = pager;
        const int stateIndex = tabStates_.size();
        connect(pager, &RangePager::pageRequested, this,
                [this, stateIndex](int page) { goToEpisodePage(stateIndex, page); });
        tabLayout->addWidget(pager);

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

    if (tabs_->count() > 0 && tabs_->currentIndex() < 0) {
        tabs_->setCurrentIndex(0);
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
    if (!type.isEmpty()) {
        auto* badge = new QLabel(episodeTypeBadgeText(type), button);
        badge->setObjectName(QStringLiteral("episodeTypeBadge"));
        badge->setToolTip(episodeTypeCaption(type));
        badge->setAccessibleName(badge->toolTip());
        badge->setAttribute(Qt::WA_TransparentForMouseEvents);
        badge->setAlignment(Qt::AlignCenter);
        badge->adjustSize();
        badge->move(button->width() - badge->sizeHint().width() - theme::kSpacingXs,
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
    const int spacing = theme::kSpacingSmall;
    const int available = state.grid->width() > 0 ? state.grid->width() : tabs_->width();
    const int columns = qMax(
        1, (available + spacing) / (theme::kTitleEpisodeButtonWidth + spacing));
    QVector<QWidget*> buttons;
    buttons.reserve(end - start);
    for (int index = start; index < end; ++index) {
        buttons.append(state.entries.at(index)->button);
    }
    layoutEpisodeGrid(state.gridLayout, buttons, columns);
    state.columns = columns;
    for (int index = start; index < end; ++index) {
        EpisodeEntry* entry = state.entries.at(index);
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
        renderTabPage(stateIndex);
    }
}

void TitlePage::reflowGrid(int stateIndex) {
    if (stateIndex < 0 || stateIndex >= tabStates_.size()) {
        return;
    }
    TabState& state = tabStates_[stateIndex];
    // A compact pill per episode: 64px plus the gutter fits a row of 16.
    const int spacing = theme::kSpacingSmall;
    const int available = state.grid->width() > 0 ? state.grid->width() : tabs_->width();
    const int columns = qMax(
        1, (available + spacing) / (theme::kTitleEpisodeButtonWidth + spacing));
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
    QWidget::keyPressEvent(event);
}

void TitlePage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    applyResponsiveLayout();
}

void TitlePage::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
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
    const int tabBarHeight = tabs_->count() > 0 ? tabs_->tabBar()->sizeHint().height() : 0;
    // Size from the active page only: a taller grid from another release would
    // leave a blank panel. The page scroll owns the overflow.
    const int desired = tabBarHeight + currentPage->sizeHint().height() + 2 * frame;
    if (fittedHeight_ == desired && tabs_->height() == desired) {
        return;
    }
    fittedHeight_ = desired;
    tabs_->setFixedHeight(desired);
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
    QVector<EpisodeEntry*>& waiters = fetchWaiters_[embed];
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
            [self, embed](const backend::Failure& failure) {
                if (!self.isNull()) {
                    self->finishFetch(embed, QMap<int, QString>(), backend::failureLine(failure));
                }
            });
    }
    if (std::find(waiters.cbegin(), waiters.cend(), entry) == waiters.cend()) {
        waiters.append(entry);
    }
    entryCallbacks_[entry].append(callback);
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
            [self, index, seq](const backend::Failure&) {
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
            self->updatePosterCredit();
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
    updatePosterCredit();
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
    // Rounded copies are produced on a size change, never on a paint.
    const QSize posterSize = cover_->size();
    if (posterSize != coverAppliedSize_) {
        cover_->setPixmap(TitleCard::rounded(coverPixmap_, posterSize));
        coverAppliedSize_ = posterSize;
    }
    if (detailsThumb_ != nullptr) {
        detailsThumb_->setText(QString());
        detailsThumb_->setPixmap(TitleCard::rounded(coverPixmap_, detailsThumb_->size()));
    }
    if (backdrop_ != nullptr) {
        backdrop_->setPoster(coverPixmap_);
    }
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
    // A line the app did not word itself (the backend's failure line): kept
    // verbatim, and restated verbatim on a language switch.
    statusKind_ = BaseStatus::Raw;
    baseStatus_ = text;
    status_->setText(text);
}

void TitlePage::setStatusKind(BaseStatus kind) {
    statusKind_ = kind;
    baseStatus_ = baseStatusText(kind);
    status_->setText(baseStatus_);
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

void TitlePage::retranslateUi() {
    if (backButton_ != nullptr) {
        backButton_->setText(tr("← Back"));
    }
    if (homeButton_ != nullptr) {
        homeButton_->setText(tr("Home"));
    }
    if (settingsButton_ != nullptr) {
        settingsButton_->setToolTip(tr("API token settings"));
        settingsButton_->setAccessibleName(tr("API token settings"));
    }
    if (episodesHeading_ != nullptr) {
        episodesHeading_->setText(tr("Episodes"));
    }

    // A flash still on screen is left to run out: its own timer restores the
    // base line below, which is re-worded here.
    const bool showingBase = status_ != nullptr && status_->text() == baseStatus_;
    baseStatus_ = baseStatusText(statusKind_);
    if (showingBase) {
        status_->setText(baseStatus_);
    }

    applyCoverFallback();

    if (hasDetail_) {
        // Re-worded before the sheet: its Synopsis block reads this retained text.
        fullDescription_ =
            rawDescription_.isEmpty() ? tr("No description available.") : rawDescription_;
        refreshDetailTexts();
        description_->setText(descriptionExpanded_ ? fullDescription_ : descriptionPreview());
        descriptionButton_->setText(descriptionExpanded_ ? tr("Collapse description")
                                                          : tr("Read more"));
    }
    if (metaButton_ != nullptr) {
        metaButton_->setText(metaButton_->isChecked() ? tr("Hide details") : tr("More details"));
    }
    if (relatedButton_ != nullptr) {
        relatedButton_->setText(relatedButton_->isChecked() ? tr("Hide similar")
                                                            : tr("Related and similar"));
    }
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
    updateEpisodeSummary();
    updatePosterCredit();
    if (hasDetail_) {
        updateLicenseInfo(player_);
    }
    updateWatchPanel();
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
        chip->setText(label);
        chip->setToolTip(tooltip);
    }
}

void TitlePage::refreshEpisodeTexts() {
    if (tabs_ != nullptr) {
        for (int index = 0; index < tabStates_.size() && index < tabs_->count(); ++index) {
            const QString provider = tabStates_.at(index).provider;
            const QString known = labels::provider(provider);
            tabs_->setTabText(index, known.isEmpty() ? capitalize(provider) : known);
        }
    }
    // Labels are plain data: every entry carries one for the player's episode
    // list, whether or not its pill is built right now.
    for (EpisodeEntry* entry : ownedEntries_) {
        if (entry != nullptr) {
            entry->label = episodeCaption(entry->number);
            if (entry->typeBadge != nullptr) {
                const QString type = episodeType(entry);
                entry->typeBadge->setText(episodeTypeBadgeText(type));
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
        const int start = state.page * state.pageSize;
        const int end = qMin(start + state.pageSize, state.entries.size());
        for (int index = start; index < end; ++index) {
            EpisodeEntry* entry = state.entries.at(index);
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
