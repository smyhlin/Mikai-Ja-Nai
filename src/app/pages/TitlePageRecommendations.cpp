#include "pages/TitlePageInternal.hpp"

namespace mikai::app {
using namespace title_page_detail;

void TitlePage::populateRelated(const backend::AnimeDetail& detail) {
    // The carousels are being rebuilt from a fresh detail, so the registrations
    // of the ones going away are dropped with them.
    relatedHeadings_.clear();
    relatedPagerButtons_.clear();
    relatedKindLabels_.clear();
    relatedPosterLabels_.clear();
    // A landing decode finds its poster by slot index and proves the index with
    // the label it holds, so the slots of the carousels going away are dropped
    // before the new ones are built.
    relatedPosters_.clear();
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
        discoveryHost_->hide();
        relatedButton_->hide();
        scheduleOverviewReflow();
        return;
    }
    discoveryHost_->show();
    relatedButton_->show();
    if (!relations.isEmpty()) {
        relatedLayout_->addWidget(buildRelatedCarousel(QStringLiteral("related"), relations));
    }
    if (!similar.isEmpty()) {
        relatedLayout_->addWidget(buildRelatedCarousel(QStringLiteral("similar"), similar));
    }
    setRelatedExpanded(true, false);
    scheduleOverviewReflow();
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
            self->scheduleOverviewReflow();
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
    auto* previous = new CarouselButton(CarouselButton::Direction::Left, section);
    previous->setToolTip(tr("Previous page"));
    auto* next = new CarouselButton(CarouselButton::Direction::Right, section);
    next->setToolTip(tr("Next page"));
    relatedPagerButtons_.emplaceBack(previous, next);
    for (QPushButton* button : {previous, next}) {
        header->addWidget(button);
    }
    // The disclosure toggle above this pager takes the pager's own width — both
    // square controls plus the gap — instead of sizing to its caption, so the
    // three controls read as one column. The geometry comes from the buttons
    // themselves so a theme change carries over without a magic number here.
    if (relatedButton_ != nullptr) {
        previous->ensurePolished();
        next->ensurePolished();
        const int pairWidth = previous->sizeHint().width() + next->sizeHint().width() +
                              header->spacing();
        if (relatedButton_->minimumWidth() != pairWidth)
            relatedButton_->setFixedWidth(pairWidth);
    }
    layout->addWidget(headerHost);

    const int inset = theme::kCarouselTrackInset;
    auto* scroll = new QScrollArea(section);
    scroll->setObjectName(QStringLiteral("animeCarousel"));
    scroll->setWidgetResizable(false);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* track = new QWidget(scroll);
    track->setObjectName(QStringLiteral("animeCarouselTrack"));
    auto* cardsLayout = new QHBoxLayout(track);
    cardsLayout->setContentsMargins(inset, inset, inset, inset);
    cardsLayout->setSpacing(theme::kCarouselCardSpacing);

    QList<QPointer<QPushButton>> createdCards;
    for (const auto& entry : entries) {
        const backend::AnimeSummary& anime = entry.second;
        auto* card = new QPushButton(section);
        card->setObjectName(QStringLiteral("animeCard"));
        card->setFixedWidth(theme::kCarouselCardWidth);
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
        relatedPosterLabels_.append(posterLabel);
        cardLayout->addWidget(posterLabel, 0, Qt::AlignHCenter);

        if (headingKey == QLatin1String("related")) {
            auto* kind = new QLabel(labels::relation(entry.first), card);
            kind->setObjectName(QStringLiteral("carouselKind"));
            kind->setAlignment(Qt::AlignCenter);
            kind->setAttribute(Qt::WA_TransparentForMouseEvents);
            relatedKindLabels_.append({entry.first, kind});
            cardLayout->addWidget(kind);
        }

        auto* titleLabel = new ElidedTwoLineLabel(name, card);
        titleLabel->setObjectName(QStringLiteral("carouselTitle"));
        cardLayout->addWidget(titleLabel, 1);
        cardsLayout->addWidget(card);
        createdCards.append(card);

        const QString posterUrl = backend::posterUrl(anime);
        if (!posterUrl.isEmpty()) {
            relatedPosters_.append({posterLabel, posterUrl, {}, 0.0, false});
            requestRelatedPoster(relatedPosters_.size() - 1);
        }
    }

    const int cardCount = entries.size();
    const int trackWidth = 2 * inset +
                           cardCount * theme::kCarouselCardWidth +
                           qMax(0, cardCount - 1) * theme::kCarouselCardSpacing;
    track->setFixedWidth(trackWidth);
    scroll->setWidget(track);
    layout->addWidget(scroll);

    // Install dynamic refit filter (style, font, DPI, language, show)
    auto* fitFilter = new CarouselFitFilter(scroll, track, createdCards, inset);
    track->installEventFilter(fitFilter);

    // Defer actual height fit until after polish and style application.
    fitCarousel(scroll, track, createdCards, inset);

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

void TitlePage::requestRelatedPoster(int index) {
    if (index < 0 || index >= relatedPosters_.size()) {
        return;
    }
    RelatedPoster& slot = relatedPosters_[index];
    if (slot.label.isNull() || slot.url.isEmpty()) {
        return;
    }
    QLabel* label = slot.label.data();
    // The decode lands at the device size the label draws at on this screen, so
    // its pixels are the label's own rather than an upscaled 1x raster.
    const qreal requestedDpr = label->devicePixelRatioF();
    const QSize target = gui::deviceSizeFor(label->size(), requestedDpr);
    backend::MikaiClient* client = &client_;
    const QPointer<TitlePage> self(this);
    const QPointer<QLabel> poster(label);
    const QString url = slot.url;
    slot.pending = true;
    runner_.runImage(
        [client, url, target] {
            return QVariant::fromValue(gui::decodeImage(client->fetchBytes(url), target));
        },
        [self, url, requestedDpr, poster, index](const QVariant& result) {
            if (self.isNull() || poster.isNull() || index >= self->relatedPosters_.size()) {
                return;
            }
            // The strip is rebuilt as a whole, so the index is only meaningful
            // while it still names the label and the URL it was asked for.
            RelatedPoster& slot = self->relatedPosters_[index];
            if (slot.label.data() != poster.data() || slot.url != url) {
                return;
            }
            slot.pending = false;
            auto decoded = result.value<gui::DecodedImage>();
            if (decoded.image.isNull()) {
                qWarning().noquote() << "TitlePage: related artwork decode failed:"
                                     << self->ref_ << decoded.error;
                return;
            }
            slot.pixmap = QPixmap::fromImage(std::move(decoded.image));
            // Tagged with the ratio it was decoded at, never with the one the
            // label is on now: a 1x raster labelled as a HiDPI one is exactly
            // what left the strip soft after a screen move.
            slot.pixmap.setDevicePixelRatio(requestedDpr);
            slot.decodedDpr = requestedDpr;
            if (!qFuzzyCompare(requestedDpr, poster->devicePixelRatioF())) {
                // Answered for the screen this label has left: ask again rather
                // than stretch these pixels onto the new one.
                self->requestRelatedPoster(index);
                return;
            }
            self->paintRelatedPoster(index);
        },
        [self, poster, index](const backend::Failure& failure) {
            if (!self.isNull() && index < self->relatedPosters_.size() &&
                self->relatedPosters_[index].label.data() == poster.data()) {
                self->relatedPosters_[index].pending = false;
            }
            qWarning().noquote() << "TitlePage: related artwork fetch failed:"
                                 << backend::failureLine(failure);
        });
}

void TitlePage::paintRelatedPoster(int index) {
    if (index < 0 || index >= relatedPosters_.size()) {
        return;
    }
    const RelatedPoster& slot = relatedPosters_.at(index);
    if (slot.label.isNull() || slot.pixmap.isNull()) {
        return;
    }
    QLabel* label = slot.label.data();
    label->setText(QString());
    label->setPixmap(TitleCard::rounded(
        slot.pixmap, label->size(), label->devicePixelRatioF(),
        theme::scaledRadius(theme::kTitlePosterRadius, currentAppearance()), true));
}

void TitlePage::refreshRelatedPosters() {
    for (int index = 0; index < relatedPosters_.size(); ++index) {
        const RelatedPoster& slot = relatedPosters_.at(index);
        if (slot.label.isNull() || slot.pixmap.isNull()) {
            continue;
        }
        // The pixels on hand are painted at the ratio this screen draws on, so
        // the strip never falls back to QLabel's unfiltered rescale; only then
        // is the decode for that ratio worth asking for.
        paintRelatedPoster(index);
        if (!slot.pending && !qFuzzyCompare(slot.decodedDpr, slot.label->devicePixelRatioF())) {
            requestRelatedPoster(index);
        }
    }
}

void TitlePage::setRelatedExpanded(bool expanded, bool fromUser) {
    relatedHost_->setVisible(expanded);
    discoveryHost_->setVisible(relatedLayout_->count() > 0);
    reflowOverviewHeight();
    relatedButton_->setVisible(true);
    relatedButton_->setText(expanded ? tr("Hide") : tr("Show"));
    const QSignalBlocker blocker(relatedButton_);
    relatedButton_->setChecked(expanded);
    if (expanded && fromUser) {
        QTimer::singleShot(0, this, [this] {
            overviewScroll_->ensureWidgetVisible(discoveryHost_);
        });
    }
}

void TitlePage::toggleDescription() {
    descriptionExpanded_ = !descriptionExpanded_;
    refreshDescriptionPreview();
    scheduleOverviewReflow();
}

QString TitlePage::descriptionPreview() const {
    if (description_ == nullptr || fullDescription_.isEmpty()) {
        return fullDescription_;
    }
    const int availableWidth = qMax(1, description_->width());
    QTextLayout layout(fullDescription_, description_->font());
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    QVector<QTextLine> lines;
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) {
            break;
        }
        line.setLineWidth(availableWidth);
        lines.append(line);
    }
    layout.endLayout();
    if (lines.size() <= theme::kTitleDescriptionPreviewLines) {
        return fullDescription_;
    }
    const QTextLine& finalLine = lines.at(theme::kTitleDescriptionPreviewLines - 1);
    const QString finalText =
        fullDescription_.mid(finalLine.textStart(), finalLine.textLength());
    const QFontMetrics metrics(description_->font());
    QString elided = metrics.elidedText(finalText, Qt::ElideRight, availableWidth);
    if (!elided.endsWith(QChar(0x2026))) {
        elided = metrics.elidedText(finalText + QChar(0x2026), Qt::ElideRight,
                                    availableWidth);
    }
    QStringList previewLines;
    for (int index = 0; index < theme::kTitleDescriptionPreviewLines - 1; ++index) {
        const QTextLine& line = lines.at(index);
        previewLines.append(fullDescription_.mid(line.textStart(), line.textLength()).trimmed());
    }
    previewLines.append(elided);
    return previewLines.join(QLatin1Char('\n'));
}

void TitlePage::refreshDescriptionPreview() {
    if (description_ == nullptr || descriptionButton_ == nullptr) {
        return;
    }
    descriptionPreviewWidth_ = description_->width();
    const QString preview = descriptionPreview();
    description_->setText(descriptionExpanded_ ? fullDescription_ : preview);
    const bool hasMore = preview != fullDescription_;
    descriptionButton_->setVisible(hasMore);
    disclosureRow_->setVisible(hasDetail_ && hasMore);
    descriptionButton_->setText(descriptionExpanded_ ? tr("Show less") : tr("Read more"));
}

void TitlePage::scrollToEpisodes() {
    if (overviewScroll_ == nullptr || episodesHost_ == nullptr) {
        return;
    }
    QWidget* target = nullptr;
    const int stateIndex = tabs_ != nullptr ? tabs_->currentIndex() : -1;
    if (stateIndex >= 0 && stateIndex < tabStates_.size()) {
        TabState& state = tabStates_[stateIndex];
        if (!state.rendered) {
            renderTabPage(stateIndex);
        }
        for (EpisodeEntry* entry : state.entries) {
            if (entryIsPlayable(entry) && entry->button != nullptr &&
                entry->button->isEnabled()) {
                target = entry->button;
                break;
            }
        }
    }
    if (target == nullptr) {
        target = episodeStatus_ != nullptr && episodeStatus_->isVisible()
                     ? static_cast<QWidget*>(episodeStatus_)
                     : episodeAvailability_ != nullptr && episodeAvailability_->isVisible()
                           ? static_cast<QWidget*>(episodeAvailability_)
                           : static_cast<QWidget*>(episodesHeading_);
    }
    target->setFocus(Qt::OtherFocusReason);
    overviewScroll_->ensureWidgetVisible(target, 0, 0);
}

void TitlePage::toggleMetadata(bool expanded) {
    setDetailsOpen(expanded);
}

void TitlePage::setDetailsOpen(bool open) {
    QTimer::singleShot(0, this, &TitlePage::scheduleOverviewReflow);
    if (detailsOverlay_ == nullptr || open == detailsOpen_) {
        return;
    }
    detailsOpen_ = open;
    detailsOverlay_->setVisible(open);
    if (open) {
        updateDetailsGeometry();
        detailsOverlay_->raise();
        // The panel eases in from the right edge it lives on. Closing stays
        // immediate: Escape, scrim and the close button never wait on motion.
        auto* slide = new QPropertyAnimation(detailsPanel_, "pos", this);
        slide->setDuration(kSheetSlideMs);
        slide->setStartValue(detailsPanel_->pos() + QPoint(kSheetSlidePx, 0));
        slide->setEndValue(detailsPanel_->pos());
        slide->setEasingCurve(QEasingCurve::OutCubic);
        slide->start(QAbstractAnimation::DeleteWhenStopped);
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
    metaButton_->setToolTip(open ? tr("Hide title details") : tr("Show title details"));
    metaButton_->setAccessibleName(metaButton_->text());
}


}  // namespace mikai::app
