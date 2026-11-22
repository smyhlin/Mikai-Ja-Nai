#include "pages/SearchPageInternal.hpp"

namespace mikai::app {
using namespace search_page_detail;

void SearchPage::loadDictionaries() {
    const int seq = ++dictionarySeq_;
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run(
        [client] {
            QVariantMap data;
            data.insert(QStringLiteral("meta"), QVariant::fromValue(client->getMeta()));
            data.insert(QStringLiteral("genres"), QVariant::fromValue(client->getGenres()));
            data.insert(QStringLiteral("tags"), QVariant::fromValue(client->getTags()));
            data.insert(QStringLiteral("studios"), QVariant::fromValue(client->getStudios()));
            return data;
        },
        [self, seq](const QVariant& value) {
            if (self.isNull() || seq != self->dictionarySeq_) return;
            const QVariantMap data = value.toMap();
            self->apiMeta_ = data.value(QStringLiteral("meta")).value<backend::ApiMeta>();
            self->dictionaryError_.clear();
            self->genres_ = data.value(QStringLiteral("genres")).value<QVector<backend::GenreTag>>();
            self->tags_ = data.value(QStringLiteral("tags")).value<QVector<backend::GenreTag>>();
            self->studios_ = data.value(QStringLiteral("studios")).toStringList();
            self->filtersButton_->setToolTip(tr("Catalogue dictionaries loaded"));
            self->filtersButton_->setEnabled(true);
        },
        [self, seq](const backend::Failure& failure) {
            if (self.isNull() || seq != self->dictionarySeq_) return;
            self->dictionaryError_ = failure.message;
            self->filtersButton_->setToolTip(failure.message);
            self->filtersButton_->setEnabled(true);
            self->statusLine_ = Status::Failure;
            self->statusFailure_ = failure;
            self->renderStatus();
        });
    filtersButton_->setEnabled(false);
}

void SearchPage::showCatalogueFilters() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Catalogue filters"));
    dialog.setObjectName(QStringLiteral("catalogueFiltersDialog"));
    dialog.setMinimumSize(theme::kSettingsDialogMinWidth, theme::kSettingsDialogMinHeight);
    dialog.resize(theme::kCatalogueFiltersWidth, theme::kCatalogueFiltersHeight);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(theme::kSpacingLarge, theme::kSpacingMedium,
                               theme::kSpacingLarge, theme::kSpacingMedium);
    layout->setSpacing(theme::kSpacingMedium);
    auto* filterScroll = new QScrollArea(&dialog);
    if (!dictionaryError_.isEmpty()) {
        auto* banner = new QFrame(&dialog);
        banner->setObjectName(QStringLiteral("settingsCard"));
        auto* bLayout = new QHBoxLayout(banner);
        bLayout->setContentsMargins(theme::kSpacingMedium, theme::kSpacingSmall,
                                    theme::kSpacingMedium, theme::kSpacingSmall);
        bLayout->setSpacing(theme::kSpacingSmall);
        auto* dictionaryFailure =
            new QLabel(tr("Catalogue dictionaries failed: %1").arg(dictionaryError_), banner);
        dictionaryFailure->setObjectName(QStringLiteral("catalogueDictionaryFailure"));
        dictionaryFailure->setWordWrap(true);
        auto* retry = new QPushButton(tr("Retry dictionaries"), banner);
        retry->setObjectName(QStringLiteral("catalogueDictionaryRetryButton"));
        theme::applyActionRole(retry, theme::ActionRole::Secondary);
        connect(retry, &QPushButton::clicked, &dialog, [this, retry, dictionaryFailure] {
            retry->setEnabled(false);
            retry->setText(tr("Retrying…"));
            dictionaryFailure->setText(tr("Reloading catalogue dictionaries in the background…"));
            loadDictionaries();
        });
        bLayout->addWidget(dictionaryFailure, 1);
        bLayout->addWidget(retry, 0, Qt::AlignVCenter);
        layout->addWidget(banner);
    }
    delete filterScroll;
    filterScroll = nullptr;

    // Two-pane sidebar-navigated card layout unified with SettingsDialog.
    auto* bodyRow = new QHBoxLayout();
    bodyRow->setContentsMargins(0, 0, 0, 0);
    bodyRow->setSpacing(theme::kSpacingMedium);

    auto* navRail = new QListWidget(&dialog);
    navRail->setObjectName(QStringLiteral("catalogueFilterNavRail"));
    navRail->setFixedWidth(168);
    navRail->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navRail->setFrameShape(QFrame::NoFrame);

    auto* sectionStack = new QStackedWidget(&dialog);
    sectionStack->setObjectName(QStringLiteral("catalogueFilterStack"));
    connect(navRail, &QListWidget::currentRowChanged, sectionStack, &QStackedWidget::setCurrentIndex);

    bodyRow->addWidget(navRail, 0);
    bodyRow->addWidget(sectionStack, 1);

    auto* filterContent = sectionStack;
    const auto addSection = [&](const QString& title) {
        auto* pageScroll = new QScrollArea(sectionStack);
        pageScroll->setWidgetResizable(true);
        pageScroll->setFrameShape(QFrame::NoFrame);
        pageScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        pageScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

        auto* pageHost = new QWidget(pageScroll);
        auto* pageLayout = new QVBoxLayout(pageHost);
        pageLayout->setContentsMargins(0, 0, theme::kSpacingSmall, 0);
        pageLayout->setSpacing(theme::kSpacingMedium);

        auto* card = new QFrame(pageHost);
        card->setObjectName(QStringLiteral("settingsCard"));
        auto* inner = new QVBoxLayout(card);
        inner->setContentsMargins(theme::kSpacingLarge, theme::kSpacingLarge,
                                  theme::kSpacingLarge, theme::kSpacingLarge);
        inner->setSpacing(theme::kSpacingSmall);

        auto* heading = new QLabel(title, card);
        heading->setObjectName(QStringLiteral("sectionHeading"));
        inner->addWidget(heading);

        pageLayout->addWidget(card);
        pageLayout->addStretch(1);
        pageScroll->setWidget(pageHost);

        sectionStack->addWidget(pageScroll);
        auto* navItem = new QListWidgetItem(title, navRail);
        navItem->setSizeHint(QSize(156, theme::kControlHeight));
        if (navRail->count() == 1) {
            navRail->setCurrentRow(0);
        }
        return inner;
    };
    const auto addField = [](QVBoxLayout* section, const QString& caption, QWidget* field) {
        QWidget* owner = section->parentWidget();
        auto* block = new QWidget(owner);
        auto* column = new QVBoxLayout(block);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(theme::kSpacingXs);
        auto* label = new QLabel(caption, block);
        label->setObjectName(QStringLiteral("muted"));
        column->addWidget(label);
        column->addWidget(field);
        section->addWidget(block);
    };
    // Expand choice lists to their visible row height with no inner vertical
    // scrollbar so the single outer section QScrollArea handles wheel scrolling
    // smoothly without nested scroll-wheel hijacking.
    const auto fitChoiceList = [](QListWidget* list) {
        list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        int shown = 0;
        for (int row = 0; row < list->count(); ++row) {
            if (!list->item(row)->isHidden()) ++shown;
        }
        list->setFixedHeight(shown > 0
                                 ? theme::kCatalogueChoiceRowHeight * shown + theme::kSpacingSmall
                                 : 0);
        list->setVisible(shown > 0);
    };
    const auto filterChoices = [fitChoiceList](QListWidget* list, const QString& term) {
        const QString needle = term.trimmed();
        for (int row = 0; row < list->count(); ++row) {
            QListWidgetItem* item = list->item(row);
            const bool matches =
                needle.isEmpty() ||
                item->text().contains(needle, Qt::CaseInsensitive) ||
                item->data(Qt::UserRole).toString().contains(needle, Qt::CaseInsensitive);
            item->setHidden(!matches);
        }
        fitChoiceList(list);
    };
    const auto addSectionSearch = [&](QVBoxLayout* section, const QString& objectName,
                                      const QString& placeholder,
                                      const QList<QListWidget*>& lists) {
        QWidget* owner = section->parentWidget();
        auto* search = new QLineEdit(owner);
        search->setObjectName(objectName);
        search->setPlaceholderText(placeholder);
        search->setClearButtonEnabled(true);
        section->addWidget(search);
        auto* emptyLabel = new QLabel(tr("No matching options found."), owner);
        emptyLabel->setObjectName(QStringLiteral("muted"));
        emptyLabel->hide();
        section->addWidget(emptyLabel);
        connect(search, &QLineEdit::textChanged, owner,
                [filterChoices, lists, emptyLabel](const QString& term) {
                    int totalShown = 0;
                    for (QListWidget* list : lists) {
                        filterChoices(list, term);
                        for (int r = 0; r < list->count(); ++r) {
                            if (!list->item(r)->isHidden()) ++totalShown;
                        }
                    }
                    emptyLabel->setVisible(!term.trimmed().isEmpty() && totalShown == 0);
                });
        return search;
    };
    layout->addLayout(bodyRow, 1);
    const auto optionLabel = [](const QString& value) {
        if (value == QLatin1String("all")) return tr("Match all");
        if (value == QLatin1String("any")) return tr("Match any");
        if (value == QLatin1String("name")) return tr("Name");
        if (value == QLatin1String("year")) return tr("Year");
        if (value == QLatin1String("updated")) return tr("Recently updated");
        if (value == QLatin1String("views")) return tr("Views");
        if (value == QLatin1String("mal_rating")) return tr("MAL rating");
        if (value == QLatin1String("asc")) return tr("Ascending");
        if (value == QLatin1String("desc")) return tr("Descending");
        if (value == QLatin1String("winter")) return QCoreApplication::translate("Labels", "Winter");
        if (value == QLatin1String("spring")) return QCoreApplication::translate("Labels", "Spring");
        if (value == QLatin1String("summer")) return QCoreApplication::translate("Labels", "Summer");
        if (value == QLatin1String("autumn")) return QCoreApplication::translate("Labels", "Autumn");
        if (value == QLatin1String("unknown")) return labels::status(value);
        return value;
    };
    auto selectionMode = [&dialog, &optionLabel](const QString& objectName, const QStringList& options,
                                                const QString& current) {
        auto* box = new QComboBox(&dialog);
        box->setObjectName(objectName);
        QStringList values = options;
        if (!current.isEmpty() && !values.contains(current)) values << current;
        for (const QString& option : values) box->addItem(optionLabel(option), option);
        const int index = box->findData(current);
        if (index >= 0) box->setCurrentIndex(index);
        return box;
    };
    auto checkedList = [&dialog, fitChoiceList](const QString& objectName,
                                        const QStringList& labels,
                                        const QStringList& canonical,
                                        const QStringList& selected) {
        auto* list = new QListWidget(&dialog);
        list->setObjectName(objectName);
        list->setUniformItemSizes(true);
        QStringList seen;
        for (int i = 0; i < canonical.size(); ++i) {
            const QString value = canonical.at(i);
            auto* item = new QListWidgetItem(labels.value(i, value), list);
            item->setData(Qt::UserRole, value);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(selected.contains(value) ? Qt::Checked : Qt::Unchecked);
            seen << value;
        }
        for (const QString& value : selected) {
            if (seen.contains(value)) continue;
            auto* item = new QListWidgetItem(value, list);
            item->setData(Qt::UserRole, value);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Checked);
        }
        fitChoiceList(list);
        return list;
    };
    const auto namesFor = [](const QVector<backend::GenreTag>& values) {
        QSettings s;
        const QString uiLang = s.value(QStringLiteral("ui/language"), QString()).toString().toLower();
        const bool preferEnglish = uiLang.startsWith(QLatin1String("en"));
        QStringList labels, canonical;
        for (const backend::GenreTag& value : values) {
            if (preferEnglish) {
                labels << (value.name.isEmpty() ? value.ua : value.name);
            } else {
                labels << (value.ua.isEmpty() ? value.name : value.ua);
            }
            canonical << value.name;
        }
        return qMakePair(labels, canonical);
    };
    const auto genreNames = namesFor(genres_);
    const auto tagNames = namesFor(tags_);
    auto* genreInclude = checkedList(QStringLiteral("genreIncludeList"), genreNames.first,
                                      genreNames.second, catalogueQuery_.genres);
    auto* genreExclude = checkedList(QStringLiteral("genreExcludeList"), genreNames.first,
                                      genreNames.second, catalogueQuery_.genres_exclude);
    auto* tagInclude = checkedList(QStringLiteral("tagIncludeList"), tagNames.first,
                                    tagNames.second, catalogueQuery_.tags);
    auto* tagExclude = checkedList(QStringLiteral("tagExcludeList"), tagNames.first,
                                    tagNames.second, catalogueQuery_.tags_exclude);
    // Enforce mutual exclusion: checking a genre/tag in Include clears it from
    // Exclude (and vice versa) so contradictory queries cannot be constructed.
    const auto linkMutualExclusion = [&dialog](QListWidget* primary, QListWidget* opposite) {
        connect(primary, &QListWidget::itemChanged, &dialog, [opposite](QListWidgetItem* changed) {
            if (changed == nullptr || changed->checkState() != Qt::Checked) return;
            const QString key = changed->data(Qt::UserRole).toString();
            for (int row = 0; row < opposite->count(); ++row) {
                QListWidgetItem* other = opposite->item(row);
                if (other != nullptr && other->data(Qt::UserRole).toString() == key &&
                    other->checkState() == Qt::Checked) {
                    other->setCheckState(Qt::Unchecked);
                }
            }
        });
    };
    linkMutualExclusion(genreInclude, genreExclude);
    linkMutualExclusion(genreExclude, genreInclude);
    linkMutualExclusion(tagInclude, tagExclude);
    linkMutualExclusion(tagExclude, tagInclude);
    QStringList matchModes = apiMeta_.match_modes;
    if (matchModes.isEmpty()) matchModes << QStringLiteral("all") << QStringLiteral("any");
    auto* genreMatch = selectionMode(QStringLiteral("genreMatchMode"), matchModes,
                                     catalogueQuery_.genres_match);
    auto* tagMatch = selectionMode(QStringLiteral("tagMatchMode"), matchModes,
                                   catalogueQuery_.tags_match);
    const auto attachSectionHeaderTools =
        [&dialog, navRail](int navIndex, const QString& baseTitle, QVBoxLayout* section,
                           const QList<QListWidget*>& lists) {
            QWidget* owner = section->parentWidget();
            auto* headerRow = new QHBoxLayout();
            headerRow->setContentsMargins(0, 0, 0, 0);
            auto* countBadge = new QLabel(owner);
            countBadge->setObjectName(QStringLiteral("muted"));
            auto* clearBtn = new QPushButton(tr("Clear"), owner);
            clearBtn->setObjectName(QStringLiteral("clearFiltersChip"));
            clearBtn->setCursor(Qt::PointingHandCursor);
            headerRow->addWidget(countBadge, 1);
            headerRow->addWidget(clearBtn, 0);
            section->insertLayout(1, headerRow);

            const auto refresh = [navRail, navIndex, baseTitle, countBadge, clearBtn, lists]() {
                int selectedCount = 0;
                for (QListWidget* list : lists) {
                    for (int r = 0; r < list->count(); ++r) {
                        if (list->item(r)->checkState() == Qt::Checked) ++selectedCount;
                    }
                }
                if (QListWidgetItem* navItem = navRail->item(navIndex)) {
                    navItem->setText(selectedCount > 0
                                         ? QStringLiteral("%1 (%2)").arg(baseTitle).arg(selectedCount)
                                         : baseTitle);
                }
                countBadge->setText(selectedCount > 0
                                        ? tr("%n selected", nullptr, selectedCount)
                                        : QString());
                clearBtn->setVisible(selectedCount > 0);
            };
            connect(clearBtn, &QPushButton::clicked, &dialog, [lists, refresh] {
                for (QListWidget* list : lists) {
                    for (int r = 0; r < list->count(); ++r) {
                        if (list->item(r)->checkState() == Qt::Checked) {
                            list->item(r)->setCheckState(Qt::Unchecked);
                        }
                    }
                }
                refresh();
            });
            for (QListWidget* list : lists) {
                connect(list, &QListWidget::itemChanged, &dialog,
                        [refresh](QListWidgetItem*) { refresh(); });
            }
            refresh();
        };

    const auto buildTriStateChipBoard =
        [&dialog](QVBoxLayout* section, QLineEdit* searchBox, QListWidget* incList,
                  QListWidget* excList) {
            QWidget* owner = section->parentWidget();
            auto* legend = new QLabel(tr("Click a chip to cycle: Off → + Include → − Exclude"), owner);
            legend->setObjectName(QStringLiteral("muted"));
            section->addWidget(legend);

            auto* board = new FlowScrollHost(owner);
            auto* flow = new FlowLayout(theme::kSpacingXs, theme::kSpacingXs);
            flow->setContentsMargins(0, 0, 0, 0);
            board->setLayout(flow);

            struct ChipBinding {
                QPushButton* btn = nullptr;
                QString label;
                QString canonical;
                int row = 0;
            };
            auto bindings = std::make_shared<QVector<ChipBinding>>();

            const auto applyChipVisual = [](QPushButton* btn, const QString& label, int state) {
                if (state == 1) {
                    btn->setText(QStringLiteral("+ ") + label);
                    btn->setObjectName(QStringLiteral("filterChipInclude"));
                } else if (state == 2) {
                    btn->setText(QStringLiteral("− ") + label);
                    btn->setObjectName(QStringLiteral("filterChipExclude"));
                } else {
                    btn->setText(label);
                    btn->setObjectName(QStringLiteral("filterChip"));
                }
                btn->style()->unpolish(btn);
                btn->style()->polish(btn);
            };

            const auto syncAllChips = [bindings, incList, excList, applyChipVisual]() {
                for (const ChipBinding& b : *bindings) {
                    const bool inc = b.row < incList->count() &&
                                     incList->item(b.row)->checkState() == Qt::Checked;
                    const bool exc = b.row < excList->count() &&
                                     excList->item(b.row)->checkState() == Qt::Checked;
                    applyChipVisual(b.btn, b.label, inc ? 1 : (exc ? 2 : 0));
                }
            };

            for (int r = 0; r < incList->count(); ++r) {
                const QString lbl = incList->item(r)->text();
                const QString key = incList->item(r)->data(Qt::UserRole).toString();
                auto* chip = new QPushButton(lbl, board);
                chip->setCursor(Qt::PointingHandCursor);
                chip->setToolTip(tr("%1 — click to include, click again to exclude").arg(lbl));
                bindings->append({chip, lbl, key, r});
                connect(chip, &QPushButton::clicked, &dialog, [r, incList, excList]() {
                    QListWidgetItem* incItem = incList->item(r);
                    QListWidgetItem* excItem = excList->item(r);
                    if (incItem == nullptr || excItem == nullptr) return;
                    if (incItem->checkState() != Qt::Checked && excItem->checkState() != Qt::Checked) {
                        incItem->setCheckState(Qt::Checked);
                    } else if (incItem->checkState() == Qt::Checked) {
                        incItem->setCheckState(Qt::Unchecked);
                        excItem->setCheckState(Qt::Checked);
                    } else {
                        excItem->setCheckState(Qt::Unchecked);
                    }
                });
                flow->addWidget(chip);
            }

            connect(incList, &QListWidget::itemChanged, &dialog,
                    [syncAllChips](QListWidgetItem*) { syncAllChips(); });
            connect(excList, &QListWidget::itemChanged, &dialog,
                    [syncAllChips](QListWidgetItem*) { syncAllChips(); });
            if (searchBox != nullptr) {
                connect(searchBox, &QLineEdit::textChanged, &dialog,
                        [bindings](const QString& term) {
                            const QString needle = term.trimmed();
                            for (const ChipBinding& b : *bindings) {
                                const bool match =
                                    needle.isEmpty() ||
                                    b.label.contains(needle, Qt::CaseInsensitive) ||
                                    b.canonical.contains(needle, Qt::CaseInsensitive);
                                b.btn->setVisible(match);
                            }
                        });
            }
            syncAllChips();
            section->addWidget(board);
            // Keep backing QListWidgets hidden (still owned by dialog for programmatic sync).
            incList->hide();
            excList->hide();
        };

    auto* genres = addSection(tr("Genres"));
    attachSectionHeaderTools(0, tr("Genres"), genres, {genreInclude, genreExclude});
    QLineEdit* genreSearch =
        addSectionSearch(genres, QStringLiteral("genreFilterSearch"), tr("Search genres…"),
                         {genreInclude, genreExclude});
    buildTriStateChipBoard(genres, genreSearch, genreInclude, genreExclude);
    addField(genres, tr("Genre matching"), genreMatch);

    auto* tagsSection = addSection(tr("Tags"));
    attachSectionHeaderTools(1, tr("Tags"), tagsSection, {tagInclude, tagExclude});
    QLineEdit* tagSearch =
        addSectionSearch(tagsSection, QStringLiteral("tagFilterSearch"), tr("Search tags…"),
                         {tagInclude, tagExclude});
    buildTriStateChipBoard(tagsSection, tagSearch, tagInclude, tagExclude);
    addField(tagsSection, tr("Tag matching"), tagMatch);

    const int currentYear = QDate::currentDate().year();
    const int maxYear = currentYear + 2;
    auto* yearFrom = new QSpinBox(&dialog);
    yearFrom->setObjectName(QStringLiteral("yearFromSpin"));
    yearFrom->setRange(0, maxYear);
    yearFrom->setSpecialValueText(tr("Any"));
    yearFrom->setValue(catalogueQuery_.year_from.value_or(0));
    auto* yearTo = new QSpinBox(&dialog);
    yearTo->setObjectName(QStringLiteral("yearToSpin"));
    yearTo->setRange(0, maxYear);
    yearTo->setSpecialValueText(tr("Any"));
    yearTo->setValue(catalogueQuery_.year_to.value_or(0));
    auto* years = new QLineEdit(&dialog);
    years->setObjectName(QStringLiteral("yearsEdit"));
    years->setPlaceholderText(QStringLiteral("2020, 2021"));
    QStringList yearStrings;
    for (int year : catalogueQuery_.years) yearStrings << QString::number(year);
    years->setText(yearStrings.join(QLatin1Char(',')));
    auto* release = addSection(tr("Release"));

    // Quick decade/year preset pills for fast range selection.
    auto* yearPresetsHost = new FlowScrollHost(filterContent);
    auto* yearPresetsFlow = new FlowLayout(theme::kSpacingXs, theme::kSpacingXs);
    yearPresetsFlow->setContentsMargins(0, 0, 0, 0);
    yearPresetsHost->setLayout(yearPresetsFlow);
    const auto addYearPreset = [&](const QString& label, int fromVal, int toVal) {
        auto* btn = new QPushButton(label, yearPresetsHost);
        btn->setObjectName(QStringLiteral("categoryChip"));
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QPushButton::clicked, &dialog, [yearFrom, yearTo, years, fromVal, toVal] {
            yearFrom->setValue(fromVal);
            yearTo->setValue(toVal);
            years->clear();
        });
        yearPresetsFlow->addWidget(btn);
    };
    addYearPreset(QString::number(currentYear), currentYear, currentYear);
    addYearPreset(QString::number(currentYear - 1), currentYear - 1, currentYear - 1);
    addYearPreset(QStringLiteral("2020s"), 2020, currentYear);
    addYearPreset(QStringLiteral("2010s"), 2010, 2019);
    addYearPreset(QStringLiteral("2000s"), 2000, 2009);
    addYearPreset(tr("Classic (<2000)"), 1950, 1999);
    addYearPreset(tr("Any"), 0, 0);
    release->addWidget(yearPresetsHost);

    auto* yearSpan = new QWidget(filterContent);
    auto* yearRow = new QHBoxLayout(yearSpan);
    yearRow->setContentsMargins(0, 0, 0, 0);
    yearRow->setSpacing(theme::kSpacingMedium);
    const auto addYear = [&](const QString& caption, QSpinBox* box) {
        box->setFixedWidth(theme::kCatalogueYearFieldWidth);
        auto* column = new QVBoxLayout();
        column->setSpacing(theme::kSpacingXs);
        auto* label = new QLabel(caption, yearSpan);
        label->setObjectName(QStringLiteral("muted"));
        column->addWidget(label);
        column->addWidget(box);
        yearRow->addLayout(column);
    };
    addYear(tr("Year from"), yearFrom);
    addYear(tr("Year to"), yearTo);
    yearRow->addStretch();
    release->addWidget(yearSpan);
    addField(release, tr("Years (comma separated)"), years);

    auto* yearValidation = new QLabel(filterContent);
    yearValidation->setObjectName(QStringLiteral("yearValidationLabel"));
    yearValidation->setStyleSheet(QStringLiteral("color: #f87171;"));
    yearValidation->setWordWrap(true);
    yearValidation->hide();
    release->addWidget(yearValidation);
    const auto validateYears = [yearFrom, yearTo, years, yearValidation, maxYear]() -> bool {
        QStringList errs;
        if (yearFrom->value() > 0 && yearTo->value() > 0 && yearFrom->value() > yearTo->value()) {
            errs << tr("\"Year from\" (%1) cannot exceed \"Year to\" (%2).")
                        .arg(yearFrom->value())
                        .arg(yearTo->value());
        }
        QStringList badTokens;
        for (const QString& part : years->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            bool ok = false;
            const int y = part.trimmed().toInt(&ok);
            if (!ok || y < 1950 || y > maxYear) {
                badTokens << part.trimmed();
            }
        }
        if (!badTokens.isEmpty()) {
            errs << tr("Invalid year(s): %1 (expected 1950–%2).")
                        .arg(badTokens.join(QStringLiteral(", ")))
                        .arg(maxYear);
        }
        if (errs.isEmpty()) {
            yearValidation->hide();
            return true;
        }
        yearValidation->setText(errs.join(QLatin1Char(' ')));
        yearValidation->show();
        return false;
    };
    connect(yearFrom, &QSpinBox::valueChanged, &dialog, [validateYears](int) { validateYears(); });
    connect(yearTo, &QSpinBox::valueChanged, &dialog, [validateYears](int) { validateYears(); });
    connect(years, &QLineEdit::textChanged, &dialog, [validateYears](const QString&) { validateYears(); });
    validateYears();

    const auto translatedValues = [](const QStringList& values, const auto& label) {
        QStringList names;
        names.reserve(values.size());
        for (const QString& value : values) names.append(label(value));
        return names;
    };
    auto* formatList = checkedList(QStringLiteral("formatFilterList"),
                                    translatedValues(apiMeta_.formats, labels::format),
                                    apiMeta_.formats, catalogueQuery_.formats);
    auto* statusList = checkedList(QStringLiteral("statusFilterList"),
                                    translatedValues(apiMeta_.statuses, labels::status),
                                    apiMeta_.statuses, catalogueQuery_.statuses);
    auto* seasonList = checkedList(QStringLiteral("seasonFilterList"),
                                    translatedValues(apiMeta_.seasons, optionLabel),
                                    apiMeta_.seasons, catalogueQuery_.seasons);
    auto* studioList = checkedList(QStringLiteral("studioFilterList"), studios_, studios_,
                                    catalogueQuery_.studios);
    addField(release, tr("Formats"), formatList);
    addField(release, tr("Statuses"), statusList);
    addField(release, tr("Seasons"), seasonList);
    addSectionSearch(release, QStringLiteral("studioFilterSearch"), tr("Search studios…"),
                     {studioList});
    addField(release, tr("Studios"), studioList);
    attachSectionHeaderTools(2, tr("Release"), release,
                             {formatList, statusList, seasonList, studioList});
    // Preserve selections when a newer API value is absent from the cached dictionary.
    QStringList sortFields = apiMeta_.anime_sort_fields;
    QStringList sortOrders = apiMeta_.sort_orders;
    if (sortFields.isEmpty()) sortFields << catalogueQuery_.sort;
    if (sortOrders.isEmpty()) sortOrders << QStringLiteral("asc") << QStringLiteral("desc");
    auto* sort = selectionMode(QStringLiteral("catalogueSortField"), sortFields,
                               catalogueQuery_.sort);
    auto* order = selectionMode(QStringLiteral("catalogueSortOrder"), sortOrders,
                                catalogueQuery_.order);
    auto* licensesSection = addSection(tr("Licenses"));
    auto* licensesStatus = new QLabel(tr("Loading official license holders…"), licensesSection->parentWidget());
    licensesStatus->setObjectName(QStringLiteral("muted"));
    licensesSection->addWidget(licensesStatus);
    auto* licensesBoard = new FlowScrollHost(licensesSection->parentWidget());
    auto* licensesFlow = new FlowLayout(theme::kSpacingXs, theme::kSpacingXs);
    licensesFlow->setContentsMargins(0, 0, 0, 0);
    licensesBoard->setLayout(licensesFlow);
    licensesSection->addWidget(licensesBoard);
    {
        backend::MikaiClient* client = &client_;
        QPointer<QLabel> statusGuard(licensesStatus);
        QPointer<QWidget> boardGuard(licensesBoard);
        QPointer<QDialog> dlgGuard(&dialog);
        const QPointer<SearchPage> self(this);
        runner_.run(
            [client] { return QVariant::fromValue(client->getLicenseHolders()); },
            [self, statusGuard, boardGuard, dlgGuard, licensesFlow](const QVariant& result) {
                if (self.isNull() || statusGuard.isNull() || boardGuard.isNull() || dlgGuard.isNull()) return;
                const QJsonArray holders = result.toJsonArray();
                if (holders.isEmpty()) {
                    statusGuard->setText(tr("No license holders reported by this server."));
                    return;
                }
                statusGuard->setText(tr("Select an official license holder to browse licensed releases:"));
                for (const QJsonValue& v : holders) {
                    QString holderName = v.isString() ? v.toString() : v.toObject().value(QStringLiteral("name")).toString();
                    if (holderName.trimmed().isEmpty()) continue;
                    auto* btn = new QPushButton(holderName, boardGuard.data());
                    btn->setObjectName(QStringLiteral("categoryChip"));
                    btn->setCursor(Qt::PointingHandCursor);
                    connect(btn, &QPushButton::clicked, dlgGuard.data(), [self, dlgGuard, holderName] {
                        if (self.isNull()) return;
                        if (!dlgGuard.isNull()) dlgGuard->reject();
                        if (self->query_ != nullptr) self->query_->setText(holderName);
                        self->search();
                    });
                    licensesFlow->addWidget(btn);
                }
                boardGuard->updateGeometry();
            },
            [statusGuard](const backend::Failure&) {
                if (!statusGuard.isNull()) {
                    statusGuard->setText(tr("License holders are unavailable on this server."));
                }
            });
    }
    auto* ordering = addSection(tr("Sorting"));
    addField(ordering, tr("Sort by"), sort);
    addField(ordering, tr("Order"), order);
    auto* relevanceBanner = new QFrame(ordering->parentWidget());
    relevanceBanner->setObjectName(QStringLiteral("settingsCard"));
    auto* relLayout = new QHBoxLayout(relevanceBanner);
    relLayout->setContentsMargins(theme::kSpacingMedium, theme::kSpacingSmall,
                                  theme::kSpacingMedium, theme::kSpacingSmall);
    auto* relevance = new QLabel(
        tr("Text search uses relevance ranking; sort selection is retained and applies once the search box is cleared."),
        relevanceBanner);
    relevance->setObjectName(QStringLiteral("catalogueRelevanceHint"));
    relevance->setWordWrap(true);
    relLayout->addWidget(relevance, 1);
    ordering->addWidget(relevanceBanner);
    const auto* currentSearch = query_;
    relevanceBanner->setVisible(!currentSearch->text().trimmed().isEmpty());

    auto* presetsSection = addSection(tr("Presets"));
    auto* saveRow = new QHBoxLayout();
    saveRow->setContentsMargins(0, 0, 0, 0);
    saveRow->setSpacing(theme::kSpacingSmall);
    auto* presetNameEdit = new QLineEdit(presetsSection->parentWidget());
    presetNameEdit->setPlaceholderText(tr("Preset name (e.g. Completed 2020+ TV)…"));
    auto* savePresetBtn = new QPushButton(tr("Save preset"), presetsSection->parentWidget());
    theme::applyActionRole(savePresetBtn, theme::ActionRole::Secondary);
    saveRow->addWidget(presetNameEdit, 1);
    saveRow->addWidget(savePresetBtn, 0);
    presetsSection->addLayout(saveRow);

    auto* presetsList = new QListWidget(presetsSection->parentWidget());
    presetsList->setObjectName(QStringLiteral("cataloguePresetsList"));
    presetsList->setMaximumHeight(180);
    presetsSection->addWidget(presetsList);

    auto* presetActions = new QHBoxLayout();
    presetActions->setContentsMargins(0, 0, 0, 0);
    presetActions->setSpacing(theme::kSpacingSmall);
    auto* loadPresetBtn = new QPushButton(tr("Load selected"), presetsSection->parentWidget());
    theme::applyActionRole(loadPresetBtn, theme::ActionRole::Primary);
    auto* deletePresetBtn = new QPushButton(tr("Delete"), presetsSection->parentWidget());
    theme::applyActionRole(deletePresetBtn, theme::ActionRole::Ghost);
    presetActions->addWidget(loadPresetBtn);
    presetActions->addWidget(deletePresetBtn);
    presetActions->addStretch(1);
    presetsSection->addLayout(presetActions);

    const auto reloadPresetsUi = [presetsList, loadPresetBtn, deletePresetBtn]() {
        presetsList->clear();
        QSettings s;
        const QByteArray raw = s.value(QStringLiteral("catalogue/presets")).toByteArray();
        const QJsonArray arr = QJsonDocument::fromJson(raw).array();
        for (const QJsonValue& v : arr) {
            const QJsonObject obj = v.toObject();
            const QString name = obj.value(QStringLiteral("name")).toString();
            if (name.isEmpty()) continue;
            auto* item = new QListWidgetItem(name, presetsList);
            item->setData(Qt::UserRole, obj);
        }
        const bool any = presetsList->count() > 0;
        loadPresetBtn->setEnabled(any);
        deletePresetBtn->setEnabled(any);
    };
    reloadPresetsUi();

    connect(savePresetBtn, &QPushButton::clicked, &dialog,
            [presetNameEdit, genreInclude, genreExclude, tagInclude, tagExclude, formatList,
             statusList, seasonList, studioList, yearFrom, yearTo, years, sort, order,
             reloadPresetsUi]() {
                const QString name = presetNameEdit->text().trimmed();
                if (name.isEmpty()) return;
                auto toJsonArr = [](QListWidget* list) {
                    QJsonArray a;
                    for (int r = 0; r < list->count(); ++r) {
                        if (list->item(r)->checkState() == Qt::Checked)
                            a.append(list->item(r)->data(Qt::UserRole).toString());
                    }
                    return a;
                };
                QJsonObject obj;
                obj.insert(QStringLiteral("name"), name);
                obj.insert(QStringLiteral("genres"), toJsonArr(genreInclude));
                obj.insert(QStringLiteral("genres_exclude"), toJsonArr(genreExclude));
                obj.insert(QStringLiteral("tags"), toJsonArr(tagInclude));
                obj.insert(QStringLiteral("tags_exclude"), toJsonArr(tagExclude));
                obj.insert(QStringLiteral("formats"), toJsonArr(formatList));
                obj.insert(QStringLiteral("statuses"), toJsonArr(statusList));
                obj.insert(QStringLiteral("seasons"), toJsonArr(seasonList));
                obj.insert(QStringLiteral("studios"), toJsonArr(studioList));
                obj.insert(QStringLiteral("year_from"), yearFrom->value());
                obj.insert(QStringLiteral("year_to"), yearTo->value());
                obj.insert(QStringLiteral("years"), years->text().trimmed());
                obj.insert(QStringLiteral("sort"), sort->currentData().toString());
                obj.insert(QStringLiteral("order"), order->currentData().toString());
                QSettings s;
                QJsonArray arr = QJsonDocument::fromJson(
                                     s.value(QStringLiteral("catalogue/presets")).toByteArray())
                                     .array();
                QJsonArray next;
                for (const QJsonValue& v : arr) {
                    if (v.toObject().value(QStringLiteral("name")).toString() != name)
                        next.append(v);
                }
                next.append(obj);
                s.setValue(QStringLiteral("catalogue/presets"),
                           QJsonDocument(next).toJson(QJsonDocument::Compact));
                presetNameEdit->clear();
                reloadPresetsUi();
            });

    connect(loadPresetBtn, &QPushButton::clicked, &dialog,
            [presetsList, genreInclude, genreExclude, tagInclude, tagExclude, formatList,
             statusList, seasonList, studioList, yearFrom, yearTo, years, sort, order]() {
                QListWidgetItem* item = presetsList->currentItem();
                if (item == nullptr) return;
                const QJsonObject obj = item->data(Qt::UserRole).toJsonObject();
                auto applyArr = [](QListWidget* list, const QJsonArray& arr) {
                    QSet<QString> vals;
                    for (const QJsonValue& v : arr) vals.insert(v.toString());
                    for (int r = 0; r < list->count(); ++r) {
                        const bool hit = vals.contains(list->item(r)->data(Qt::UserRole).toString());
                        list->item(r)->setCheckState(hit ? Qt::Checked : Qt::Unchecked);
                    }
                };
                applyArr(genreInclude, obj.value(QStringLiteral("genres")).toArray());
                applyArr(genreExclude, obj.value(QStringLiteral("genres_exclude")).toArray());
                applyArr(tagInclude, obj.value(QStringLiteral("tags")).toArray());
                applyArr(tagExclude, obj.value(QStringLiteral("tags_exclude")).toArray());
                applyArr(formatList, obj.value(QStringLiteral("formats")).toArray());
                applyArr(statusList, obj.value(QStringLiteral("statuses")).toArray());
                applyArr(seasonList, obj.value(QStringLiteral("seasons")).toArray());
                applyArr(studioList, obj.value(QStringLiteral("studios")).toArray());
                yearFrom->setValue(obj.value(QStringLiteral("year_from")).toInt());
                yearTo->setValue(obj.value(QStringLiteral("year_to")).toInt());
                years->setText(obj.value(QStringLiteral("years")).toString());
                const int sIdx = sort->findData(obj.value(QStringLiteral("sort")).toString());
                if (sIdx >= 0) sort->setCurrentIndex(sIdx);
                const int oIdx = order->findData(obj.value(QStringLiteral("order")).toString());
                if (oIdx >= 0) order->setCurrentIndex(oIdx);
            });

    connect(deletePresetBtn, &QPushButton::clicked, &dialog, [presetsList, reloadPresetsUi]() {
        QListWidgetItem* item = presetsList->currentItem();
        if (item == nullptr) return;
        const QString name = item->text();
        QSettings s;
        QJsonArray arr = QJsonDocument::fromJson(
                             s.value(QStringLiteral("catalogue/presets")).toByteArray())
                             .array();
        QJsonArray next;
        for (const QJsonValue& v : arr) {
            if (v.toObject().value(QStringLiteral("name")).toString() != name) next.append(v);
        }
        s.setValue(QStringLiteral("catalogue/presets"),
                   QJsonDocument(next).toJson(QJsonDocument::Compact));
        reloadPresetsUi();
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Reset |
                                          QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("catalogueFilterButtons"));
    QPushButton* applyButton = buttons->button(QDialogButtonBox::Apply);
    if (applyButton != nullptr) {
        applyButton->setObjectName(QStringLiteral("catalogueApplyButton"));
        // The shared role paints it: a private sheet here is what let this one
        // button drift from the accent while the rest of the dialog followed.
        theme::applyActionRole(applyButton, theme::ActionRole::Primary);
    }

    // Live debounced (400ms) result-count preview on the primary Apply button.
    auto previewSeq = std::make_shared<int>(0);
    auto* previewTimer = new QTimer(&dialog);
    previewTimer->setSingleShot(true);
    previewTimer->setInterval(400);
    const auto buildDraftQuery = [this, genreInclude, genreExclude, tagInclude, tagExclude,
                                  genreMatch, tagMatch, yearFrom, yearTo, years, formatList,
                                  statusList, seasonList, studioList, sort, order]() {
        auto collectChecked = [](QListWidget* list) {
            QStringList vals;
            for (int r = 0; r < list->count(); ++r) {
                if (list->item(r)->checkState() == Qt::Checked) {
                    vals << list->item(r)->data(Qt::UserRole).toString();
                }
            }
            return vals;
        };
        backend::AnimeQuery q = catalogueQuery_;
        q.search = query_ != nullptr ? query_->text().trimmed() : QString();
        q.genres = collectChecked(genreInclude);
        q.genres_exclude = collectChecked(genreExclude);
        q.genres_match = genreMatch->currentData().toString();
        q.tags = collectChecked(tagInclude);
        q.tags_exclude = collectChecked(tagExclude);
        q.tags_match = tagMatch->currentData().toString();
        q.year_from = yearFrom->value() ? std::optional<int>(yearFrom->value()) : std::nullopt;
        q.year_to = yearTo->value() ? std::optional<int>(yearTo->value()) : std::nullopt;
        q.years.clear();
        for (const QString& part : years->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            bool ok = false;
            const int y = part.trimmed().toInt(&ok);
            if (ok) q.years.append(y);
        }
        q.formats = collectChecked(formatList);
        q.statuses = collectChecked(statusList);
        q.seasons = collectChecked(seasonList);
        q.studios = collectChecked(studioList);
        q.sort = sort->currentData().toString();
        q.order = order->currentData().toString();
        q.page = 1;
        q.limit = 1;
        return q;
    };
    connect(previewTimer, &QTimer::timeout, &dialog,
            [this, previewSeq, buildDraftQuery, applyBtn = QPointer<QPushButton>(applyButton)] {
                if (applyBtn.isNull()) return;
                const int seq = ++(*previewSeq);
                const backend::AnimeQuery req = buildDraftQuery();
                backend::MikaiClient* client = &client_;
                runner_.run(
                    [client, req] { return QVariant::fromValue(client->search(req)); },
                    [previewSeq, seq, applyBtn](const QVariant& result) {
                        if (applyBtn.isNull() || seq != *previewSeq) return;
                        const auto res = result.value<backend::SearchResult>();
                        if (res.total.has_value()) {
                            const int n = *res.total;
                            applyBtn->setText(
                                n > 0
                                    ? (n == 1 ? tr("Apply · 1 title")
                                              : tr("Apply · %1 titles").arg(QLocale().toString(n)))
                                    : tr("Apply · No matches"));
                        }
                    },
                    [previewSeq, seq, applyBtn](const backend::Failure&) {
                        if (applyBtn.isNull() || seq != *previewSeq) return;
                        applyBtn->setText(tr("Apply"));
                    });
            });
    const auto schedulePreview = [previewTimer]() { previewTimer->start(); };
    for (QListWidget* list : {genreInclude, genreExclude, tagInclude, tagExclude, formatList,
                              statusList, seasonList, studioList}) {
        connect(list, &QListWidget::itemChanged, &dialog,
                [schedulePreview](QListWidgetItem*) { schedulePreview(); });
    }
    connect(yearFrom, &QSpinBox::valueChanged, &dialog, [schedulePreview](int) { schedulePreview(); });
    connect(yearTo, &QSpinBox::valueChanged, &dialog, [schedulePreview](int) { schedulePreview(); });
    connect(years, &QLineEdit::textChanged, &dialog,
            [schedulePreview](const QString&) { schedulePreview(); });
    connect(genreMatch, &QComboBox::currentIndexChanged, &dialog,
            [schedulePreview](int) { schedulePreview(); });
    connect(tagMatch, &QComboBox::currentIndexChanged, &dialog,
            [schedulePreview](int) { schedulePreview(); });
    auto* clearDraft = new QPushButton(tr("Clear draft"), &dialog);
    clearDraft->setObjectName(QStringLiteral("catalogueClearDraftButton"));
    clearDraft->setToolTip(tr("Reset all controls in this dialog without closing"));
    theme::applyActionRole(clearDraft, theme::ActionRole::Ghost);
    buttons->addButton(clearDraft, QDialogButtonBox::ActionRole);
    connect(clearDraft, &QPushButton::clicked, &dialog,
            [genreInclude, genreExclude, tagInclude, tagExclude, formatList, statusList, seasonList,
             studioList, yearFrom, yearTo, years, genreMatch, tagMatch, sort, order] {
                for (QListWidget* list : {genreInclude, genreExclude, tagInclude, tagExclude,
                                          formatList, statusList, seasonList, studioList}) {
                    for (int r = 0; r < list->count(); ++r) {
                        if (list->item(r)->checkState() == Qt::Checked) {
                            list->item(r)->setCheckState(Qt::Unchecked);
                        }
                    }
                }
                yearFrom->setValue(0);
                yearTo->setValue(0);
                years->clear();
                genreMatch->setCurrentIndex(0);
                tagMatch->setCurrentIndex(0);
                const int nameIdx = sort->findData(QStringLiteral("name"));
                if (nameIdx >= 0) sort->setCurrentIndex(nameIdx);
                order->setCurrentIndex(0);
            });

    if (auto* reset = buttons->button(QDialogButtonBox::Reset)) {
        reset->setObjectName(QStringLiteral("catalogueResetButton"));
        theme::applyActionRole(reset, theme::ActionRole::Secondary);
    }
    if (auto* cancel = buttons->button(QDialogButtonBox::Cancel))
        theme::applyActionRole(cancel, theme::ActionRole::Secondary);
    layout->addWidget(buttons);
    // Whole-row click toggling on visible checkable lists + Ctrl+Enter apply shortcut.
    for (QListWidget* list : {formatList, statusList, seasonList, studioList}) {
        connect(list, &QListWidget::itemClicked, &dialog, [](QListWidgetItem* item) {
            if (item == nullptr) return;
            item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
        });
    }
    auto* applyShortcut1 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), &dialog);
    auto* applyShortcut2 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter), &dialog);
    const auto triggerApply = [&dialog] {
        dialog.setProperty("applyRequested", true);
        dialog.accept();
    };
    connect(applyShortcut1, &QShortcut::activated, &dialog, triggerApply);
    connect(applyShortcut2, &QShortcut::activated, &dialog, triggerApply);

    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::clicked, &dialog, [&dialog, buttons](QAbstractButton* button) {
        if (buttons->standardButton(button) == QDialogButtonBox::Reset) {
            dialog.setProperty("resetRequested", true);
            dialog.accept();
        } else if (buttons->standardButton(button) == QDialogButtonBox::Apply) {
            dialog.setProperty("applyRequested", true);
            dialog.accept();
        }
    });
    if (dialog.exec() == QDialog::Accepted) {
        if (dialog.property("resetRequested").toBool()) resetCatalogueFilters();
        else if (dialog.property("applyRequested").toBool()) {
            auto selected = [](QListWidget* list) {
                QStringList values;
                for (int row = 0; row < list->count(); ++row) {
                    auto* item = list->item(row);
                    if (item->checkState() == Qt::Checked) values << item->data(Qt::UserRole).toString();
                }
                return values;
            };
            catalogueQuery_.genres = selected(genreInclude);
            catalogueQuery_.genres_exclude = selected(genreExclude);
            catalogueQuery_.genres_match = genreMatch->currentData().toString();
            catalogueQuery_.tags = selected(tagInclude);
            catalogueQuery_.tags_exclude = selected(tagExclude);
            catalogueQuery_.tags_match = tagMatch->currentData().toString();
            catalogueQuery_.year_from = yearFrom->value() ? std::optional<int>(yearFrom->value()) : std::nullopt;
            catalogueQuery_.year_to = yearTo->value() ? std::optional<int>(yearTo->value()) : std::nullopt;
            catalogueQuery_.years.clear();
            for (const QString& part : years->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                bool ok = false; const int year = part.trimmed().toInt(&ok);
                if (ok) catalogueQuery_.years.append(year);
            }
            catalogueQuery_.formats = selected(formatList);
            catalogueQuery_.statuses = selected(statusList);
            catalogueQuery_.seasons = selected(seasonList);
            catalogueQuery_.studios = selected(studioList);
            catalogueQuery_.sort = sort->currentData().toString();
            catalogueQuery_.order = order->currentData().toString();
            applyCatalogueFilters();
        }
    }
}

void SearchPage::resetCatalogueFilters() {
    catalogueQuery_ = backend::AnimeQuery{};
    activeFormat_.clear();
    applyCatalogueFilters();
}

void SearchPage::searchGenre(const QString& genre) {
    const QString name = genre.trimmed();
    if (name.isEmpty()) return;
    query_->clear();
    catalogueQuery_ = backend::AnimeQuery{};
    catalogueQuery_.genres = QStringList{name};
    applyCatalogueFilters();
}

void SearchPage::searchCatalogue(const backend::AnimeQuery& criteria) {
    query_->clear();
    catalogueQuery_ = criteria;
    activeFormat_.clear();
    applyCatalogueFilters();
}

void SearchPage::applyCatalogueFilters() {
    ++requestSeq_;
    ++catalogueRequestSeq_;
    ++teamRequestSeq_;
    for (const QString& g : catalogueQuery_.genres) {
        catalogueQuery_.genres_exclude.removeAll(g);
    }
    for (const QString& t : catalogueQuery_.tags) {
        catalogueQuery_.tags_exclude.removeAll(t);
    }
    catalogueQuery_.search = query_->text().trimmed();
    catalogueQuery_.page = 1;
    catalogueQuery_.limit = pageSize_;
    currentQuery_ = catalogueQuery_.search;
    teamBrowseMode_ = false;
    if (teamHeroCard_ != nullptr) teamHeroCard_->hide();
    if (teamBackButton_ != nullptr) teamBackButton_->hide();
    if (teamDetailLabel_ != nullptr) teamDetailLabel_->hide();
    browseMode_ = currentQuery_.isEmpty() && !hasExplicitCatalogueCriteria(catalogueQuery_);
    page_ = 1;
    fetchPage(1);
    showSearchResults();
}


}  // namespace mikai::app
